#pragma once

#include <optional>
#include <cstdint>
#include <vector>
#include <string_view>
#include <memory>

// System-level simulators

const size_t MEM_BUS_WIDTH = 256;

struct GlobalMemReq {
  uint16_t id;

  // Size of the access, 2^size bytes
  uint8_t size;
  // MC-local address, aligned to 2^size
  uint32_t addr;

  // Small endian, lane-aligned (NOT ADDRESS-ALIGNED) data,
  // so wdata[0] always corresponds to the byte 0 at the 32-byte chunk
  // being written
  uint8_t wdata[MEM_BUS_WIDTH / 8];
  bool write;
};

struct GlobalMemResp {
  uint16_t id;
  // Small endian, lane-aligned response data
  uint8_t data[MEM_BUS_WIDTH / 8];
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

  // T can step the simulation by one cycle
  // Conceptually, this will trigger a FULL cycle, including a posedge and a negedge
  // It's implementation-defined whether the posedge or negedge happens first, but the backend should
  // guarantee that the dump is reasonable (see the explaination for mem)
  // The cycle number is provided by the frontend. Backends must not count cycles internally.
  virtual void step(uint64_t cycle) = 0;

  // The following functions are used by scheduling memory req / resp
  // They are guaranteed to be called exactly once per cycle, conceptually happens at the negedge
  // The ready-valid handshake protocol requires that "ready" never combinatorially
  //   depends on "valid" and the data, but the reverse is allowed.
  // So the frontend will never reject a request due to its content
  //   but the backend may block requests based on its content
  // Therefore, T should `eval` after all memory responses are shown to the RTL model.

  // mem: memory interaction. The frontend additionally
  // guarantees that it will not change the acceptance of a request based
  // on the request itself. So it will first pass in the request state
  // The length of the arrays is numMC + 1, ID = 0 being the peripheral
  virtual void mem(const MemBusIn *, MemBusOut *) = 0;

  // Print stat into stderr.
  // The backend should allocate a context object to store relevant information
  // needed for incremental stats (the last counter values, the last cycle, etc.)
  // If this backend does not support stats, returns false
  // Arguments:
  // - The current cycle count
  // - Whether this is the final print at the end
  virtual bool printStats(uint64_t cycles, bool final) = 0;
};

struct DRAMsim3Config {
  std::string_view configFile;
  std::string_view workDir;
};

class System {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  System(
    std::vector<std::string_view> dramInitFiles,
    std::optional<DRAMsim3Config> dramTimingModel
  );
  ~System();

  // Add a backend.
  // If multiple backends are added, they will form a cosimulation,
  // where any discrepancy in the observed behavior (e.g. memory requests) will cause a failure.
  void addBackend(std::unique_ptr<SystemBackend> backend);

  // Run the simulation until all backends have finished, or it's timed out, or a discrepancy is found.
  // Return true for sucessful finish, false for timeout or failure
  bool run(uint64_t maxCycles);
};