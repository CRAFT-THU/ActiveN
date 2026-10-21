#pragma once

#include <algorithm>
#include <array>
#include <atomic>
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
  // MemIf NoC injection is present only at the first request connection in
  // each domain, matching the inject-enabled local in System.scala.
  IndexVector<std::optional<uint16_t>> pu_memif_inject;

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

  // Each MemIf owner resolves its ejections from the source routers. Retain
  // the token so the source-router owner can move the selected flit at commit.
  struct MemifEject {
    std::optional<RouterOutputToken> token;
  };

  // A router's owner resolves all of its incoming transfers directly from the
  // committed source queues. Tokens remain valid until the following step.
  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) LinkDecisionBuffer {
    std::optional<uint8_t> core_inject_presenting;
    bool core_inject_accepting = false;
    std::optional<RouterOutputToken> core_eject_presenting;
    bool core_eject_accepting = false;
    std::array<std::optional<RouterOutputToken>, 4> forward_presenting;
    std::array<bool, 4> forward_accepting{};
    std::optional<Flit> memif_inject_presenting;
    bool memif_inject_accepting = false;
  };

  static_assert(alignof(LinkDecisionBuffer) >=
                DESTRUCTIVE_INTERFERENCE_SIZE);
  static_assert(sizeof(LinkDecisionBuffer) %
                    DESTRUCTIVE_INTERFERENCE_SIZE == 0);

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
    // MemIf whose registered notification output injects at this router.
    std::optional<uint16_t> memif_inject;
  };

  using RouteFn = decltype(std::declval<Topology>().routingTableFor(
      0, std::declval<const Topology::DirectionToLinkIdx &>(), 0, std::nullopt));

  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) WrappedRouter {
    LinkStatus links;
    LinkDecisionBuffer buf;
    Router<RouteFn, ROUTER_Q_DEPTH> rt;

    WrappedRouter(LinkStatus l, RouteFn tbl, size_t num_in, size_t num_out,
                  unsigned spaceMultiplier)
      : links(std::move(l)), buf{}, rt(std::move(tbl), num_in, num_out, spaceMultiplier) {}
  };

  static_assert(alignof(WrappedRouter) >= DESTRUCTIVE_INTERFERENCE_SIZE);

  const soft_mem::ExperimentConfig memoryExperiment;
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
  std::vector<uint8_t> memif_ring_accept;
  IndexVector<soft_mem::DRAMIf::PUResp> core_mem;
  IndexVector<soft_mem::DRAMIf::PUAccept> core_mem_accept;

  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) MemifStepDecision {
    bool ring_injected = false;
  };

  std::vector<MemifStepDecision> memif_step_decisions;
  std::vector<size_t> memif_owner;
  std::vector<std::vector<size_t>> thread_memifs;
  // One cluster mask array per worker avoids concurrent read-modify-write
  // when a 16-PU distributor spans multiple host threads.
  std::vector<std::vector<uint16_t>> thread_bcst_accept;
  std::vector<std::vector<uint16_t>> dram_bcst_accept;

  // Tightly-coupled router states. Each WrappedRouter owns its routing
  // table closure (which captures `this` of the Topology); Topology
  // must outlive routers, which is enforced by member-declaration order.
  IndexVector<WrappedRouter> routers;

  // Parallelism
  size_t numThreads = 8; // Overridden by the ctor's threads parameter.

  std::atomic_bool halted = false;
  SpinBarrier stageStart, stageDone;
  SpinBarrier stepStart, stepDone;
  bool resetReleaseNow;
  std::vector<std::thread> workers; // Should be numThreads - 1

  // Construct one router given its 1-based PU id. Used by the routers
  // factory.
  WrappedRouter buildRouter(uint16_t pu) const;
  soft_mem::MemIf &memifAt(size_t memifIdx);
  bool memifNocAccepted(size_t memifIdx) const;
  std::unique_ptr<Flit> takeMemifFlit(size_t memifIdx);
  bool ringCanInject(size_t memifIdx) const;

  void stageWorkResolve(size_t threadId);
  void stageMemifWork(size_t threadId);
  void prepareNextCoreMem(size_t threadId);
  void stepWork(size_t threadId);
  void stepPeriphWork(size_t threadId);
  void stepMemifWork(size_t threadId);
  void stepCleanup(size_t threadId);
  void worker(size_t threadId);

  std::pair<size_t, size_t> puWorkRange(size_t threadId) const {
    size_t start = threadId * cfg.numPU / numThreads;
    size_t end = (threadId + 1) * cfg.numPU / numThreads;
    return {start + 1, end + 1}; // PUs are 1-based
  }

  size_t puOwner(size_t pu) const {
    return (pu * numThreads - 1) / cfg.numPU;
  }

 public:
  explicit SoftSystemBackend(SystemConfig cfg, size_t threads = 8,
                             soft_mem::ExperimentConfig experiment = {});
  ~SoftSystemBackend() override = default;
  SoftSystemBackend(const SoftSystemBackend &) = delete;
  SoftSystemBackend &operator=(const SoftSystemBackend &) = delete;

  void attachTrace(VerilatedFstC *tracer, int depth);
  std::string_view name() const override { return "soft"; }
  std::vector<SpmEndpoint> spms() override;

  // SystemBackend interface
  SystemConfig config() const override;
  std::vector<ICacheEndpoint> icaches() override;
  void peek(uint64_t cycle, std::vector<MemBusOut *> out) override;
  void stage(uint64_t cycle, const std::vector<MemBusIn> &in) override;
  void step(uint64_t cycle) override;
};
