#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <thread>

#include <verilated_fst_c.h>

#include "soft_verilated/soft_rtl.h"
#include "soft_components.h"
#include "soft_mem.h"
#include "system.h"
#include "util.h"

// Toggle verbose soft-model logging. (Member function — see
// SoftSystemBackend::setLogging.)

// Topology mirrors koneko.Topology (see
// ActiveN/src/main/scala/koneko/System.scala).
//
//  - Grid: power-of-two PUs on a 1:1 or 2:1 grid. PU IDs (1-based) are
//    assigned via a Hilbert curve.
//  - Coordinates are (col, row): col+ = EAST, row+ = SOUTH; origin (0, 0)
//    is at the north-west corner.
//  - Direction encoding: 0 = NORTH, 1 = EAST, 2 = SOUTH, 3 = WEST.
//  - Routing is XY: column (X) first, then row (Y); matches RTL
//    `koneko.Topology.xyRouteDir`.
//  - memif_to_pu uses 1-based PU indexing. memif_to_pu[0] is the
//    peripheral connection PU; memif_to_pu[1..numMC] are MC connection
//    PUs. memif_to_pu entries must never be 0 (loopback marker).
//
//  - memif_req_at[i] = list of PU ids (1-based) hosting a request port
//    of memif i. memif_req_at[0] = {1} (peripheral on PU 1);
//    memif_req_at[1..numMC] lists one connection PU per cluster in the
//    MC's zone, in zone-cluster order (matches RTL `memIfs(mc).req(ci)`).
//  - pu_memif[pu] = (memif_idx, port_idx) when PU hosts a memif port;
//    the port_idx is the position of `pu` inside memif_req_at[memif_idx].
//  - Routing for memory destinations is anycast to the *nearest* request
//    port of the target memif (Manhattan distance; first-encountered on
//    tie, matching RTL Scala `minBy` semantics on the same iteration
//    order).
struct Topology {
  static constexpr int CLUSTER_SIZE = 16;

  // 1-based; value = (col, row).
  IndexVector<std::pair<uint16_t, uint16_t>> pu_to_coord;
  // coord_to_pu[col * h + row] = list of PU ids at that cell (always 0 or
  // 1 entry in a 2-D mesh, but kept as vector to mirror multi-occupancy
  // designs).
  std::vector<std::vector<uint16_t>> coord_to_pu;
  // Request port locations for each of the MC
  std::vector<std::vector<uint16_t>> memif_req_at;

  // For each PU, the ordered list of active mesh directions. The order
  // determines the router-port mapping (port i+1 for inputs/outputs
  // corresponds to direction pu_mesh_dirs[pu][i]).
  IndexVector<std::vector<uint8_t>> pu_mesh_dirs;
  // For each PU, the memif index hosted here, or nullopt. Used by the
  // router to know which output port (and the routing table) talks to a
  // local memif.
  IndexVector<std::optional<std::pair<uint16_t, uint8_t>>> pu_memif;

  uint16_t w = 0; // # columns
  uint16_t h = 0; // # rows

  Topology(size_t num_pu, size_t num_mc);

  // Directions to links. 0 = North, 1 = East, 2 = South, 3 = West.
  // Slot value = output port index (0-based, used to index forward
  // buffer slots in the router).
  typedef std::array<std::optional<size_t>, 4> DirectionToLinkIdx;

  // Returns a callable (dst flit-id) -> output-port-index for a router
  // at PU `idx`, given its direction-to-port map and (optional) local
  // memif index.
  auto routingTableFor(uint16_t idx,
                       const DirectionToLinkIdx &linkIdx,
                       size_t coreEjectPort,
                       std::optional<size_t> memifIdx) const {
    auto selfCoord = pu_to_coord[idx];
    // Precompute the routing destination for each memif
    std::vector<size_t> memif_to_pu;
    memif_to_pu.resize(memif_req_at.size());
    for (size_t i = 0; i < memif_req_at.size(); i++) {
      memif_to_pu[i] = *std::ranges::min_element(memif_req_at[i], std::less{}, [&selfCoord, this](auto pu) -> size_t {
        auto coord = this->pu_to_coord[pu];
        return std::abs(int(coord.first) - int(selfCoord.first)) + std::abs(int(coord.second) - int(selfCoord.second));
      });
    }

    // Capture memif_to_pu by value! It now lives inside the lambda
    return [idx, this, selfCoord, linkIdx, coreEjectPort, memifIdx, memif_to_pu = std::move(memif_to_pu)](uint16_t dst) -> size_t {
      bool isMem = (dst & 0x8000) != 0;
      size_t tgt = 0;
      if (isMem) {
        uint16_t memIdx = dst & 0x7FFF;
        if constexpr (ASSERTIONS_ENABLED) {
          if (memIdx >= memif_to_pu.size())
            throw std::runtime_error("Memory index too large in flit");
        }
        tgt = memif_to_pu[memIdx];
      } else {
        tgt = dst;
      }

      if (tgt == 0 || tgt == idx) {
        // Local delivery.
        if (isMem) {
          if constexpr (ASSERTIONS_ENABLED) {
            if (!memifIdx)
              throw std::runtime_error("Memory request sent to local but no memifIdx is set");
          }
          return *memifIdx;
        }
        return coreEjectPort;
      }

      // Else, XY routing: column (first) first, then row (second).
      if constexpr (ASSERTIONS_ENABLED) {
        if (tgt > pu_to_coord.maxIndex())
          throw std::runtime_error("Target index too large in flit");
      }
      auto tgtCoord = pu_to_coord[tgt];
      size_t dir;
      if (tgtCoord.first != selfCoord.first)
        dir = tgtCoord.first < selfCoord.first ? 3 /* WEST */ : 1 /* EAST */;
      else
        dir = tgtCoord.second < selfCoord.second ? 0 /* NORTH */ : 2 /* SOUTH */;
      auto link = linkIdx[dir];
      if constexpr (ASSERTIONS_ENABLED) {
        if (!link) throw std::logic_error("Routed to a non-existing link");
      }
      return *link;
    };
  }
};

class SoftSystemBackend : public SystemBackend {
  static constexpr size_t ROUTER_Q_DEPTH = 8;

 public:
  // Per-period stats. Counters are running totals; periodic prints emit
  // delta vs. the prior snapshot. cur_* fields are instantaneous and
  // are not subtracted.
  struct Stat {
    static constexpr uint64_t PERIOD = 1000;

    uint64_t total_hops = 0;
    uint64_t total_injections = 0;
    uint64_t total_mem_requests = 0;
    uint64_t idle_lane_cycles = 0;
    uint64_t idle_core_cycles = 0;
    uint64_t blocked_structural = 0;
    uint64_t inflight_messages_sum = 0;
    uint64_t inflight_dram_sum = 0;
    uint64_t inflight_resp_sum = 0;

    // Instantaneous snapshots, copied through subtraction.
    uint64_t cur_inflight_messages = 0;
    uint64_t cur_inflight_dram = 0;
    uint64_t cur_inflight_resp = 0;

    Stat operator-(const Stat &o) const;
    void print(uint64_t cycles) const;
  };

 private:
  // Eject to a memif (cross-thread: routers run on workers, memif on the
  // leader). The token identifies the source slot; presenting is its stage-only
  // borrow for nocCanAccept.
  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) MemifEject {
    std::optional<RouterOutputToken> token;
    const Flit *presenting = nullptr;
  };

  // Source-owned values presented by this router. Forward tokens identify the
  // source queue slots that destination routers move during commit.
  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) LinkPresentBuffer {
    std::optional<uint8_t> core_inject;
    std::optional<RouterOutputToken> core_eject;
    std::array<std::optional<RouterOutputToken>, 4> forwards;
  };

  // Destination-owned acceptance state. forwards[o] corresponds to this
  // router's forward input o, keeping every write in the destination's block.
  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) LinkAcceptBuffer {
    bool core_inject = false;
    bool core_eject = false;
    std::array<bool, 4> forwards{};
  };

  // Keep each router's link state isolated from adjacent routers. The two
  // sub-buffers are independently aligned because different phases transfer
  // ownership of their cache lines between different threads.
  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) LinkBuffer {
    LinkPresentBuffer presenting;
    LinkAcceptBuffer accepting;
  };

  static_assert(alignof(MemifEject) >= DESTRUCTIVE_INTERFERENCE_SIZE);
  static_assert(alignof(LinkPresentBuffer) >= DESTRUCTIVE_INTERFERENCE_SIZE);
  static_assert(alignof(LinkAcceptBuffer) >= DESTRUCTIVE_INTERFERENCE_SIZE);
  static_assert(alignof(LinkBuffer) >= DESTRUCTIVE_INTERFERENCE_SIZE);

  // Per-router static topology: for each forward link slot, the remote
  // PU id and the remote port index (0-based, indexes the remote's
  // forward buffer). memif holds the memif index if this PU hosts one.
  struct LinkStatus {
    std::array<std::optional<std::pair<uint16_t, uint8_t>>, 4> forwards;
    // Number of forward links (count of non-null entries in `forwards`),
    // which also equals the input/output port index of core_inject/core_eject.
    uint8_t num_forwards = 0;
    // Memif: which one, which req port
    std::optional<std::pair<uint16_t, uint8_t>> memif;
  };

  using RouteFn = decltype(std::declval<Topology>().routingTableFor(
      0, std::declval<const Topology::DirectionToLinkIdx &>(), 0, std::nullopt));

  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) WrappedRouter {
    LinkStatus links;
    LinkBuffer buf;
    Router<RouteFn, ROUTER_Q_DEPTH> rt;

    WrappedRouter(LinkStatus l, RouteFn tbl, size_t num_in, size_t num_out)
      : links(std::move(l)), buf{}, rt(std::move(tbl), num_in, num_out) {}
  };

  static_assert(alignof(WrappedRouter) >= DESTRUCTIVE_INTERFERENCE_SIZE);

  SystemConfig cfg;

  // The cycle EDGE after which reset is released. For K cycles of reset
  // set this to K - 1 (0-based; cycle increments after step).
  size_t releaseResetAfter;

  Topology topo;
  soft_mem::PeriphIf periph;
  std::vector<soft_mem::DRAMIf> drams;
  IndexVector<std::unique_ptr<soft_rtl>> cores;
  soft_mem::Ringbus ring;

  std::vector<std::vector<MemifEject>> memif_eject;
  std::vector<std::optional<uint8_t>> memif_eject_accept;
  std::vector<MemBusIn> memif_ext;
  std::vector<bool> memif_ring_accept;
  IndexVector<soft_mem::DRAMIf::PUResp> core_mem;
  IndexVector<soft_mem::DRAMIf::PUAccept> core_mem_accept;

  // Tightly-coupled router states. Each WrappedRouter owns its routing
  // table closure (which captures `this` of the Topology); Topology
  // must outlive routers, which is enforced by member-declaration order.
  IndexVector<WrappedRouter> routers;

  // Wall-clock accounting (snapshot by PhaseTimer).
  uint64_t verilator_ns_ = 0;
  uint64_t uncore_ns_ = 0;

  // Stat tracking and verbose logging.
  Stat stats_;
  Stat last_periodic_;
  uint64_t last_periodic_cycle_ = 0;
  bool log_ = false;

  // Parallelism
  size_t numThreads = 8; // Overridden by the ctor's threads parameter.
  std::atomic_bool halted = false;
  SpinBarrier stageStart, stagePresented, stageDone;
  SpinBarrier stepStart, stepDone;
  bool resetReleaseNow;
  std::vector<std::thread> workers; // Should be numThreads - 1

  // RAII helper that attributes wall time between verilator eval
  // (tracked separately via a thread-local accumulator) and uncore.
  struct PhaseTimer {
    SoftSystemBackend &owner;
    std::chrono::high_resolution_clock::time_point t0;
    uint64_t v_at_entry;
    PhaseTimer(SoftSystemBackend &o);
    ~PhaseTimer();
  };

  // Construct one router given its 1-based PU id. Used by the routers
  // factory.
  WrappedRouter buildRouter(uint16_t pu) const;

  // Per-cycle stat accumulation. Called at the end of stage()/step().
  void accumulateStats();

  void stageWorkPresent(size_t threadId);
  void stageWorkAccept(size_t threadId);
  void stepWork(size_t threadId);
  void stepCleanup(size_t threadId);
  void worker(size_t threadId);

  std::pair<size_t, size_t> puWorkRange(size_t threadId) const {
    size_t start = threadId * cfg.numPU / numThreads;
    size_t end = (threadId + 1) * cfg.numPU / numThreads;
    return {start + 1, end + 1}; // PUs are 1-based
  }

 public:
  explicit SoftSystemBackend(SystemConfig cfg, size_t threads = 8);
  ~SoftSystemBackend() override = default;
  SoftSystemBackend(const SoftSystemBackend &) = delete;
  SoftSystemBackend &operator=(const SoftSystemBackend &) = delete;

  void attachTrace(VerilatedFstC *tracer, int depth);
  void setLogging(bool enabled) { log_ = enabled; }

  // SystemBackend interface
  SystemConfig config() const override;
  void peek(uint64_t cycle, std::vector<MemBusOut *> out) override;
  void stage(uint64_t cycle, const std::vector<MemBusIn> &in) override;
  void step(uint64_t cycle) override;
  bool printStats(uint64_t cycles, bool final) override;
};
