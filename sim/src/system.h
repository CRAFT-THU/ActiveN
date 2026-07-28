#pragma once

#include "system_config.h"
#include "verilated_fst_c.h"
#include <bit>
#include <cwchar>
#include <optional>
#include <cstdint>
#include <string>
#include <vector>
#include <string_view>
#include <memory>
#include <array>
#include <type_traits>

static const bool ASSERTIONS_ENABLED = false;

// System-level simulators

const size_t MEM_BUS_WIDTH = SYSTEM_MEM_BUS_WIDTH;
const size_t MEM_BUS_WIDTH_B = MEM_BUS_WIDTH / 8;
const uint8_t MEM_BUS_WIDTH_SIZE = std::countr_zero(MEM_BUS_WIDTH_B);
static_assert(MEM_BUS_WIDTH >= 64 && std::has_single_bit(MEM_BUS_WIDTH));
static_assert(MEM_BUS_WIDTH_B <= 64, "memory byte-enable mask is at most 64 bits");
const uint32_t MEM_ADDR_OFFSET_MASK = ((((uint32_t) 1) << MEM_BUS_WIDTH_SIZE) - 1);
const uint32_t MEM_ADDR_ALIGN_MASK = ~MEM_ADDR_OFFSET_MASK;

typedef std::array<uint8_t, MEM_BUS_WIDTH / 8> MemLine;
struct alignas(MEM_BUS_WIDTH_B) AlignedMemLine {
  MemLine inner;
};
using mem_mask_t = std::conditional_t<(MEM_BUS_WIDTH_B <= 32), uint32_t, uint64_t>;

struct GlobalMemReq {
  uint8_t id;

  // Size of the access, 2^size bytes
  uint8_t size;
  // MC-local address, aligned to 2^size
  uint32_t addr;

  // Small endian, lane-aligned (NOT ADDRESS-ALIGNED) data,
  // so wdata[0] always corresponds to byte 0 of the memory line
  // being written
  MemLine wdata;

  // Write byte enable, also lane-aligned
  mem_mask_t wbe;

  bool write;

  bool operator==(const GlobalMemReq &o) const {
    if (id != o.id || size != o.size || addr != o.addr || write != o.write) return false;
    if (write) {
      if (wbe != o.wbe) return false;
      if (wdata != o.wdata) return false;
    }
    return true;
  }
  bool operator!=(const GlobalMemReq &o) const { return !(*this == o); }
};

struct GlobalMemResp {
  uint8_t id;
  // Small endian, lane-aligned response data
  MemLine data;
};

struct MemBusIn {
  // Whether the frontend is accepting a new request
  bool reqAccepting;
  // Response presented to the backend, which should be unconditionally accepted
  std::optional<GlobalMemResp> resp;
};

struct MemBusOut {
  // Req responded by the backend
  std::optional<GlobalMemReq> req;
};

// Shape of the generated CoreConfig, that's relevant to the model
struct CoreConfig {
  std::vector<uint64_t> mcSizes;
};

// Shape of the generated SystemConfig, that's relevant to the model
struct SystemConfig {
  size_t numPU;
  size_t numMC;
  CoreConfig core;
};

// System-level simulator backend
// Backends drives the RTL simulation, but does not manage the memory hierarchy outside
// of the RTL model.
class SystemBackend {
public:
  virtual ~SystemBackend() = default;

  virtual SystemConfig config() const = 0;

  /**
   * Peek the outgoing memory requests presented by the backend for the next cycle
   * In the hardware backend, this should only include a single potential de-assertion of the reset signal,
   * and if the reset is de-asserted, eval.
   *
   * Conceptually, this function can perform:
   * - All the logic after the posedge
   * - All the logic before the negedge
   * But without the negedge itself.
   * This should be sufficient for backend modules to generate the presented memory requests
   *
   * The contract for all ready-valid interfaces (including the reqeust interface) is that the presented data
   * and its validness should never depend on the readiness of the accepting side. So we query the presented
   * request first, and then return the readiness given by the frontend in the following stage function.
   *
   * Because the lack of RVO for virtual functions, we use a pointer to return the result.
   *
   * Since there might be multiple memory buses, the out vector should contain one MemBusOut pointer per bus.
   * Backend should assert that out.size() == expected number of memory interfaces
   */
  virtual void peek(uint64_t cycle, std::vector<MemBusOut *> out) = 0;

  /**
   * Setup the transcations, and all the relevant signal value for the next cycle transition edge
   * In the hardware model, this should include a series of signal assignments (including a clock negedge), and a eval
   *
   * This is always called after peek
   * Conceptually, this contains:
   * - The negedge itself
   * - Logic after the negedge (set the memory responses and request readiness here)
   * - The logic before the posedge
   * But without actually triggering the posedge. Semantically, this will stage all the transactions
   * that's to be immediately executed.
   */
  virtual void stage(uint64_t cycle, const std::vector<MemBusIn> &in) = 0;

  /**
   * Actually perform the cycle, and commit all the transactions.
   * In hardware backend, this should only include a flip of the clock + a eval.
   *
   * In each cycle, the frontend will do the following sequence:
   * - First, peek
   * - Then, stage
   * - If tracing is enabled, dump the signals here
   * - Finally, step
   */
  virtual void step(uint64_t cycle) = 0;

  // Workload-timer boundaries for optional backend-local profiling.
  virtual void timerStatsStart(uint64_t cycle) {}
  virtual void timerStatsStop(uint64_t cycle) {}

  // Print stat into stderr.
  // The backend should allocate a context object to store relevant information
  // needed for incremental stats (the last counter values, the last cycle, etc.)
  // If this backend does not support stats, returns false
  // Arguments:
  // - The current cycle count
  // - Whether this is the final print at the end
  virtual bool printStats(uint64_t cycles, bool final) = 0;
};

struct RamulatorConfig {
  std::string configFile;
  std::optional<std::string> statsDir;
  double coreFreqGHz;
  bool timerStatsOnly;
};

class VerilatedFstC;

class System {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  System(
    std::vector<std::string_view> dramInitFiles,
    std::optional<RamulatorConfig> ramulatorTimingModel
  );
  ~System();

  // Add a backend.
  // If multiple backends are added, they will form a cosimulation,
  // where any discrepancy in the observed behavior (e.g. memory requests) will cause a failure.
  void addBackend(std::unique_ptr<SystemBackend> backend);

  // Set the FST tracer used to dump signals once per cycle (between stage and step).
  // Must be called before run(). Pass nullptr to disable tracing.
  void setTracer(VerilatedFstC *tracer, uint64_t trace_start = 0);

  // Run the simulation until all backends have finished, or it's timed out, or a discrepancy is found.
  // Return true for sucessful finish, false for timeout or failure
  bool run(uint64_t maxCycles);
};
