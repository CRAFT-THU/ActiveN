#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <verilated_fst_c.h>

#include "soft_verilated/soft_rtl.h"
#include "soft_components.h"
#include "soft_mem.h"
#include "system.h"

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
// Note: RTL has one MC connection PER cluster (multiple connections per
// MC zone). Soft simplifies to a single connection per MC for now (the
// cluster connection closest to the grid center across the zone). This
// is a deviation from RTL routing that should be revisited if alignment
// requires per-cluster MC distribution.
struct Topology {
  static constexpr int CLUSTER_SIZE = 16;

  // 1-based; value = (col, row).
  IndexVector<std::pair<uint16_t, uint16_t>> pu_to_coord;
  // coord_to_pu[col * h + row] = list of PU ids at that cell (always 0 or
  // 1 entry in a 2-D mesh, but kept as vector to mirror multi-occupancy
  // designs).
  std::vector<std::vector<uint16_t>> coord_to_pu;
  // [0] = peripheral connection PU (1-based);
  // [1..numMC] = MC connection PUs (1-based).
  std::vector<uint16_t> memif_to_pu;

  // For each PU, the ordered list of active mesh directions. The order
  // determines the router-port mapping (port i+1 for inputs/outputs
  // corresponds to direction pu_mesh_dirs[pu][i]).
  IndexVector<std::vector<uint8_t>> pu_mesh_dirs;
  // For each PU, the memif index hosted here, or nullopt. Used by the
  // router to know which output port (and the routing table) talks to a
  // local memif.
  IndexVector<std::optional<uint16_t>> pu_memif;

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
                       std::optional<size_t> memifIdx) const {
    auto selfCoord = pu_to_coord[idx];
    return [idx, this, selfCoord, linkIdx, memifIdx](uint16_t dst) -> size_t {
      bool isMem = (dst & 0x8000) != 0;
      size_t tgt = 0;
      if (isMem) {
        uint16_t memIdx = dst & 0x7FFF;
        if (memIdx >= memif_to_pu.size())
          throw std::runtime_error("Memory index too large in flit");
        tgt = memif_to_pu[memIdx];
      } else {
        tgt = dst;
      }

      if (tgt == 0 || tgt == idx) {
        // Local delivery.
        if (isMem) {
          if (!memifIdx)
            throw std::runtime_error("Memory request sent to local but no memifIdx is set");
          return *memifIdx;
        }
        return 0; // core_eject port
      }

      // Else, XY routing: column (first) first, then row (second).
      if (tgt > pu_to_coord.maxIndex())
        throw std::runtime_error("Target index too large in flit");
      auto tgtCoord = pu_to_coord[tgt];
      size_t dir;
      if (tgtCoord.first != selfCoord.first)
        dir = tgtCoord.first < selfCoord.first ? 3 /* WEST */ : 1 /* EAST */;
      else
        dir = tgtCoord.second < selfCoord.second ? 0 /* NORTH */ : 2 /* SOUTH */;
      auto link = linkIdx[dir];
      if (!link) throw std::logic_error("Routed to a non-existing link");
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
  template<typename T>
  struct ReadyValidPair {
    std::optional<T> presenting;
    bool accepting;
  };

  // Per-router buffered ready-valid state, populated during stage().
  struct LinkBuffer {
    ReadyValidPair<Flit> core_inject;
    ReadyValidPair<Flit> core_eject;
    // forwards[o] is *this router's egress* on link slot o (output
    // port o+1). To read the ingress data, the router reads its peer's
    // forward buffer for the corresponding slot.
    std::array<ReadyValidPair<Flit>, 4> forwards;
  };

  // Per-router static topology: for each forward link slot, the remote
  // PU id and the remote port index (0-based, indexes the remote's
  // forward buffer). memif holds the memif index if this PU hosts one.
  struct LinkStatus {
    std::array<std::optional<std::pair<uint16_t, uint8_t>>, 4> forwards;
    std::optional<uint16_t> memif;
  };

  using RouteFn = decltype(std::declval<Topology>().routingTableFor(
      0, std::declval<const Topology::DirectionToLinkIdx &>(), std::nullopt));

  struct WrappedRouter {
    LinkStatus links;
    LinkBuffer buf;
    Router<RouteFn, ROUTER_Q_DEPTH> rt;

    WrappedRouter(LinkStatus l, RouteFn tbl, size_t num_in, size_t num_out)
      : links(std::move(l)), buf{}, rt(std::move(tbl), num_in, num_out) {}
  };

  SystemConfig cfg;

  // The cycle EDGE after which reset is released. For K cycles of reset
  // set this to K - 1 (0-based; cycle increments after step).
  size_t releaseResetAfter;

  Topology topo;
  soft_mem::PeriphIf periph;
  std::vector<soft_mem::DRAMIf> drams;
  IndexVector<std::unique_ptr<soft_rtl>> cores;
  soft_mem::Ringbus ring;

  std::vector<ReadyValidPair<Flit>> memif_eject;
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

 public:
  explicit SoftSystemBackend(SystemConfig cfg);
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
