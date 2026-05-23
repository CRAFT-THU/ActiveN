/**
 * Software NoC system simulator.
 *
 * Instantiates many Core RTLs and models the NoC and memory system in C++.
 * The NoC uses the same Hilbert/mesh/MC-connection topology as System.scala,
 * and routes flits through a software model of the mesh routers.
 *
 * Supported features:
 *   - core-to-core AM messages via ext.out/ext.in
 *   - memory/peripheral requests encoded as 0xF00/0xF01 flits (12-bit tag)
 *   - CSR scatter commands encoded as 0xF10 flits
 *   - wide memory responses on core.mem_unicast, broadcast on core.mem_broadcast
 *   - optional per-cycle memory request trace
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <signal.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <verilated_fst_c.h>

#include "soft_verilated/soft_rtl.h"
#include "soft_components.h"
#include "soft_backend.h"
#include "system.h"

using namespace std;

static bool TRACE = false;
static bool LOG = false;
static bool exiting = false;

// Wall-clock accumulator (ns) for time spent inside soft_rtl Verilator eval()
// calls, summed across all PUs. Thread-local because Verilated contexts are
// per-thread; we use this to attribute soft-model runtime between "verilator"
// (CPU spent inside soft_rtl::eval) and "uncore" (everything else inside
// peek/stage/step). Incremented at batch granularity by the two per-cycle
// eval loops in stage()/step(); read/snapshotted by SoftSystemSim in
// peek/stage/step via PhaseTimer.
static thread_local uint64_t g_soft_verilator_ns = 0;

static const uint64_t RESET_LENGTH = 10;
static const uint32_t TEXT_BASE = 0x80000000u;
static const int MEM_BUS_WORDS = 256 / 32;
static const uint16_t PERIPH_DST = 0x8000;
static const uint16_t FIRST_MC_DST = 0x8001;

static VerilatedFstC *tracer = nullptr;
static std::unique_ptr<std::ofstream> mem_trace;
static std::vector<MemTraceEvent> *current_mem_trace = nullptr;
static std::vector<PeriphTraceEvent> *current_periph_trace = nullptr;

static void sighandler(int) { exiting = true; }

// Defined as extern in soft_components.h.
uint64_t g_soft_dbg_cycle = 0;


static void traceMemReq(uint64_t cycle, int mc, bool is_write, uint32_t local_addr, uint16_t id) {
  if (current_mem_trace) {
    current_mem_trace->push_back(MemTraceEvent{
      .cycle = cycle,
      .mc = mc,
      .is_write = is_write,
      .local_addr = local_addr,
      .id = id,
    });
  }
  if (!mem_trace) return;
  (*mem_trace) << cycle << ' ' << mc << ' ' << (is_write ? 'W' : 'R') << ' '
               << std::hex << std::setw(8) << std::setfill('0') << local_addr
               << std::dec << ' ' << id << '\n';
}

static bool isPow2(int n) {
  return n > 0 && (n & (n - 1)) == 0;
}

struct TopologyInfo {
  int grid_w = 0;
  int grid_h = 0;
  std::vector<std::pair<int, int>> pu_pos;  // 1-based; index 0 unused
  std::map<std::pair<int, int>, int> pos_to_pu;
  std::vector<std::vector<int>> mc_zones;   // mc -> cluster indices
  std::vector<std::vector<int>> mc_conns;   // mc -> connection PU IDs
  std::vector<std::vector<int>> pu_mesh_dirs;
  std::vector<std::map<int, int>> pu_dir_to_egress;
  std::vector<std::unordered_map<uint16_t, int>> pu_tables;
  std::vector<std::vector<int>> pu_mc_ejects;
};

static pair<int, int> gridDims(int num_pu) {
  int s = static_cast<int>(std::sqrt(num_pu));
  if (s * s == num_pu) return {s, s};
  int h = static_cast<int>(std::sqrt(num_pu / 2));
  if (2 * h * h != num_pu) {
    throw runtime_error("Cannot form 1:1 or 2:1 grid for numPU=" + to_string(num_pu));
  }
  return {2 * h, h};
}

static pair<int, int> hilbertD2xy(int n, int d) {
  int rx = 0;
  int ry = 0;
  int s = 1;
  int t = d;
  int x = 0;
  int y = 0;
  while (s < n) {
    rx = ((t & 2) != 0) ? 1 : 0;
    ry = (((t ^ rx) & 1) != 0) ? 1 : 0;
    if (ry == 0) {
      if (rx == 1) {
        x = s - 1 - x;
        y = s - 1 - y;
      }
      std::swap(x, y);
    }
    x += s * rx;
    y += s * ry;
    t >>= 2;
    s <<= 1;
  }
  return {x, y};
}

static vector<pair<int, int>> hilbert(int w, int h) {
  if (!isPow2(w) || w < 2 || !(w == h || w == 2 * h)) {
    throw runtime_error("Invalid Hilbert dimensions");
  }
  vector<pair<int, int>> square;
  square.reserve(w * w);
  for (int d = 0; d < w * w; ++d) square.push_back(hilbertD2xy(w, d));
  square.resize(w * h);
  return square;
}

static int meshDist(pair<int, int> a, pair<int, int> b) {
  return std::abs(a.first - b.first) + std::abs(a.second - b.second);
}

static int meshDist(int r1, int c1, int r2, int c2) {
  return std::abs(r2 - r1) + std::abs(c2 - c1);
}

static int xyRouteDir(int sr, int sc, int dr, int dc) {
  if (sc != dc) return (dc > sc) ? 1 : 3;
  return (dr > sr) ? 2 : 0;
}

static TopologyInfo buildTopology(int num_pu, int num_mc) {
  auto [grid_w, grid_h] = gridDims(num_pu);
  auto curve = hilbert(grid_w, grid_h);
  if (static_cast<int>(curve.size()) != num_pu) {
    throw runtime_error("Hilbert curve length mismatch");
  }

  TopologyInfo topo;
  topo.grid_w = grid_w;
  topo.grid_h = grid_h;
  topo.pu_pos.resize(num_pu + 1);
  for (int id = 1; id <= num_pu; ++id) {
    topo.pu_pos[id] = curve[id - 1];
    topo.pos_to_pu[topo.pu_pos[id]] = id;
  }

  int num_clusters = num_pu / 16;
  int clusters_per_mc = num_clusters / num_mc;
  topo.mc_zones.resize(num_mc);
  topo.mc_conns.resize(num_mc);
  topo.pu_mesh_dirs.resize(num_pu + 1);
  topo.pu_dir_to_egress.resize(num_pu + 1);
  topo.pu_tables.resize(num_pu + 1);
  topo.pu_mc_ejects.resize(num_pu + 1);

  for (int mc = 0; mc < num_mc; ++mc) {
    for (int ci = mc * clusters_per_mc; ci < (mc + 1) * clusters_per_mc; ++ci) {
      topo.mc_zones[mc].push_back(ci);
      int best_pu = ci * 16 + 1;
      double best_dist = 1e30;
      for (int pu = ci * 16 + 1; pu <= ci * 16 + 16; ++pu) {
        auto [r, c] = topo.pu_pos[pu];
        double dist = std::abs(r - grid_h / 2.0) + std::abs(c - grid_w / 2.0);
        if (dist < best_dist) {
          best_dist = dist;
          best_pu = pu;
        }
      }
      topo.mc_conns[mc].push_back(best_pu);
    }
  }

  for (int id = 1; id <= num_pu; ++id) {
    auto [row, col] = topo.pu_pos[id];
    auto &dirs = topo.pu_mesh_dirs[id];
    if (row > 0) dirs.push_back(0);
    if (col + 1 < grid_w) dirs.push_back(1);
    if (row + 1 < grid_h) dirs.push_back(2);
    if (col > 0) dirs.push_back(3);
    for (size_t i = 0; i < dirs.size(); ++i) {
      topo.pu_dir_to_egress[id][dirs[i]] = static_cast<int>(i);
    }
  }

  for (int mc = 0; mc < num_mc; ++mc) {
    for (int pu : topo.mc_conns[mc]) topo.pu_mc_ejects[pu].push_back(mc);
  }

  for (int src = 1; src <= num_pu; ++src) {
    auto [sr, sc] = topo.pu_pos[src];
    auto &table = topo.pu_tables[src];

    for (int dst = 1; dst <= num_pu; ++dst) {
      if (dst == src) continue;
      auto [dr, dc] = topo.pu_pos[dst];
      int dir = xyRouteDir(sr, sc, dr, dc);
      table[static_cast<uint16_t>(dst)] = topo.pu_dir_to_egress[src].at(dir);
    }

    for (int mc = 0; mc < num_mc; ++mc) {
      bool local_eject = std::find(topo.pu_mc_ejects[src].begin(), topo.pu_mc_ejects[src].end(), mc) !=
                         topo.pu_mc_ejects[src].end();
      if (local_eject) continue;

      int nearest = topo.mc_conns[mc].front();
      int best_dist = INT32_MAX;
      for (int conn : topo.mc_conns[mc]) {
        int dist = meshDist(sr, sc, topo.pu_pos[conn].first, topo.pu_pos[conn].second);
        if (dist < best_dist) {
          best_dist = dist;
          nearest = conn;
        }
      }
      auto [cr, cc] = topo.pu_pos[nearest];
      int dir = xyRouteDir(sr, sc, cr, cc);
      table[static_cast<uint16_t>(FIRST_MC_DST + mc)] = topo.pu_dir_to_egress[src].at(dir);
    }

    if (src != 1) {
      auto [pr, pc] = topo.pu_pos[1];
      int dir = xyRouteDir(sr, sc, pr, pc);
      table[PERIPH_DST] = topo.pu_dir_to_egress[src].at(dir);
    }
  }

  return topo;
}

struct CoreState {
  std::unique_ptr<soft_rtl> core;
  std::optional<Flit> ext_input;

  explicit CoreState(int hartid) {
    std::string name = "soft_pu_" + std::to_string(hartid);
    core.reset(new soft_rtl(name.c_str()));
    core->cfg_hartid = hartid;
    core->ext_out_ready = 0;
  }

  ~CoreState() {
    core->final();
  }

  void clearExtInput() {
    ext_input.reset();
  }

  void driveExtInput(const Flit &flit) {
    ext_input = flit;
  }

  optional<Flit> pendingOut() const {
    if (!core->ext_out_valid) return {};
    return Flit{
      .src = static_cast<uint16_t>(core->cfg_hartid),
      .dst = static_cast<uint16_t>(core->ext_out_bits_dst),
      .tag = static_cast<uint16_t>(core->ext_out_bits_tag),
      .data = {core->ext_out_bits_data_0, core->ext_out_bits_data_1, core->ext_out_bits_data_2, core->ext_out_bits_data_3},
    };
  }

  optional<Flit> currentOut() const {
    if (!core->ext_out_ready) return {};
    return pendingOut();
  }
};




struct RouterOutput {
  enum Kind { Mesh, Core, MemIf, Periph } kind = Mesh;
  int dir = -1;
  int mc_idx = -1;
  int mc_input = -1;
};

struct SoftRouter {
  int pu_id = 0;
  int inject_port = 0;
  vector<int> mesh_dirs;
  map<int, int> dir_to_port;   // direction → port index (same for ingress & egress)
  vector<RouterOutput> outputs;
  vector<int> local_mc_output;
  int core_output = -1;
  int periph_output = -1;

  // Routing functor: minimal per-router data; computes XY direction inline
  // for PU dsts and reads a tiny precomputed table for memif dsts.
  // (Periph and MC dsts never collide: PERIPH_DST=0x8000, FIRST_MC_DST+mc=0x8001+,
  // so a single memif_port[] array indexed by (dst - 0x8000) handles both.)
  static constexpr int MAX_MEMIF = 1 + 8;  // 1 periph + up to 8 MCs
  struct LookupFn {
    uint16_t src_pu = 0;
    uint8_t core_port = 0;
    std::pair<int8_t, int8_t> local_coord{0, 0};
    std::array<uint8_t, 4> dir_to_port{0, 0, 0, 0};
    std::array<uint8_t, MAX_MEMIF> memif_port{};
    const std::pair<int, int> *pu_pos = nullptr;

    size_t operator()(size_t dst) const {
      uint16_t d = static_cast<uint16_t>(dst);
      if (d == 0 || d == src_pu) return core_port;
      if (d & 0x8000) return memif_port[d - 0x8000];
      const auto &pos = pu_pos[d];
      int dir = (local_coord.second != pos.second)
                  ? ((pos.second > local_coord.second) ? 1 : 3)
                  : ((pos.first > local_coord.first) ? 2 : 0);
      return dir_to_port[dir];
    }
  };

  // Factory matching the user-suggested API: `decltype(rtFor(nullptr, 0))`
  // names the functor type for use as Router's RT template parameter.
  static LookupFn rtFor(const TopologyInfo *topo, int pu) {
    LookupFn fn;
    if (!topo) return fn;
    auto [sr, sc] = topo->pu_pos[pu];
    fn.src_pu = static_cast<uint16_t>(pu);
    fn.local_coord = {static_cast<int8_t>(sr), static_cast<int8_t>(sc)};
    fn.pu_pos = topo->pu_pos.data();
    return fn;
  }

  // Hardware-aligned router: depth-8 priority queues, single VC, FlitArb round-robin.
  Router<decltype(rtFor(nullptr, 0)), 8> rt;

  SoftRouter() = default;

  SoftRouter(int pu, const TopologyInfo &topo, int num_mc)
      : pu_id(pu), local_mc_output(num_mc, -1) {
    mesh_dirs = topo.pu_mesh_dirs[pu];
    dir_to_port = topo.pu_dir_to_egress[pu];
    inject_port = static_cast<int>(mesh_dirs.size());

    for (int dir : mesh_dirs) {
      outputs.push_back(
          RouterOutput{.kind = RouterOutput::Mesh, .dir = dir, .mc_idx = -1, .mc_input = -1});
    }

    core_output = static_cast<int>(outputs.size());
    outputs.push_back(RouterOutput{.kind = RouterOutput::Core, .dir = -1, .mc_idx = -1, .mc_input = -1});

    for (int mc_idx : topo.pu_mc_ejects[pu]) {
      int mc_input = -1;
      for (size_t ci = 0; ci < topo.mc_conns[mc_idx].size(); ++ci) {
        if (topo.mc_conns[mc_idx][ci] == pu) { mc_input = static_cast<int>(ci); break; }
      }
      local_mc_output[mc_idx] = static_cast<int>(outputs.size());
      outputs.push_back(RouterOutput{.kind = RouterOutput::MemIf, .dir = -1, .mc_idx = mc_idx, .mc_input = mc_input});
    }

    if (pu == 1) {
      periph_output = static_cast<int>(outputs.size());
      outputs.push_back(RouterOutput{.kind = RouterOutput::Periph, .dir = -1, .mc_idx = -1, .mc_input = -1});
    }

    if (1 + num_mc > MAX_MEMIF) throw std::runtime_error("SoftRouter: too many memifs");

    // Build LookupFn: precompute memif_port[] (local or routed-via-egress);
    // dir_to_port[] for PU dsts via XY-routing.
    LookupFn fn = rtFor(&topo, pu);
    fn.core_port = static_cast<uint8_t>(core_output);
    for (auto &[dir, port] : topo.pu_dir_to_egress[pu])
      fn.dir_to_port[dir] = static_cast<uint8_t>(port);

    // Memif lookups: pu_tables[pu] already has direction-resolved egress ports
    // for non-local memifs; local memifs override with their direct output port.
    auto it = topo.pu_tables[pu].find(PERIPH_DST);
    if (it != topo.pu_tables[pu].end())
      fn.memif_port[PERIPH_DST - 0x8000] = static_cast<uint8_t>(it->second);
    if (periph_output >= 0)
      fn.memif_port[PERIPH_DST - 0x8000] = static_cast<uint8_t>(periph_output);
    for (int mc = 0; mc < num_mc; ++mc) {
      uint16_t key = static_cast<uint16_t>(FIRST_MC_DST + mc) - 0x8000;
      auto mit = topo.pu_tables[pu].find(static_cast<uint16_t>(FIRST_MC_DST + mc));
      if (mit != topo.pu_tables[pu].end())
        fn.memif_port[key] = static_cast<uint8_t>(mit->second);
      if (local_mc_output[mc] >= 0)
        fn.memif_port[key] = static_cast<uint8_t>(local_mc_output[mc]);
    }

    rt = Router<decltype(rtFor(nullptr, 0)), 8>(fn, inject_port + 1, outputs.size());
  }

  bool canInject(const Flit &flit) const {
    return rt.canEnq(inject_port, flit.prio());
  }
};

struct PeriodicStat {
  uint64_t total_hops = 0;
  uint64_t total_injections = 0;
  uint64_t total_mem_requests = 0;
  uint64_t idle_lane_cycles = 0;
  uint64_t idle_core_cycles = 0;
  uint64_t blocked_vc_depth = 0;
  uint64_t blocked_structural = 0;
  uint64_t inflight_messages_sum = 0;
  uint64_t inflight_dram_sum = 0;
  uint64_t inflight_resp_sum = 0;

  // Current inflight snapshots (not subtracted in operator-)
  uint64_t cur_inflight_messages = 0;
  uint64_t cur_inflight_dram = 0;
  uint64_t cur_inflight_resp = 0;

  PeriodicStat operator-(const PeriodicStat &o) const {
    PeriodicStat r;
    r.total_hops = total_hops - o.total_hops;
    r.total_injections = total_injections - o.total_injections;
    r.total_mem_requests = total_mem_requests - o.total_mem_requests;
    r.idle_lane_cycles = idle_lane_cycles - o.idle_lane_cycles;
    r.idle_core_cycles = idle_core_cycles - o.idle_core_cycles;
    r.blocked_vc_depth = blocked_vc_depth - o.blocked_vc_depth;
    r.blocked_structural = blocked_structural - o.blocked_structural;
    r.inflight_messages_sum = inflight_messages_sum - o.inflight_messages_sum;
    r.inflight_dram_sum = inflight_dram_sum - o.inflight_dram_sum;
    r.inflight_resp_sum = inflight_resp_sum - o.inflight_resp_sum;
    r.cur_inflight_messages = cur_inflight_messages;
    r.cur_inflight_dram = cur_inflight_dram;
    r.cur_inflight_resp = cur_inflight_resp;
    return r;
  }

  void print(size_t cycles) const {
    auto avg = [cycles](uint64_t val) -> double {
      return cycles > 0 ? static_cast<double>(val) / cycles : 0;
    };
    cerr << "  hops:           " << total_hops
         << " (avg " << fixed << setprecision(2) << avg(total_hops) << "/cyc)\n";
    cerr << "  injections:     " << total_injections
         << " (avg " << avg(total_injections) << "/cyc)\n";
    cerr << "  mem_requests:   " << total_mem_requests
         << " (avg " << avg(total_mem_requests) << "/cyc)\n";
    cerr << "  idle_lanes:     " << idle_lane_cycles
         << " (avg " << avg(idle_lane_cycles) << "/cyc)\n";
    cerr << "  idle_cores:     " << idle_core_cycles
         << " (avg " << avg(idle_core_cycles) << "/cyc)\n";
    cerr << "  blocked_vc:     " << blocked_vc_depth
         << " (avg " << avg(blocked_vc_depth) << "/cyc)\n";
    cerr << "  blocked_hazard: " << blocked_structural
         << " (avg " << avg(blocked_structural) << "/cyc)\n";
    cerr << "  inflight_msg:   " << inflight_messages_sum
         << " (avg " << avg(inflight_messages_sum) << "/cyc, cur " << cur_inflight_messages << ")\n";
    cerr << "  inflight_dram:  " << inflight_dram_sum
         << " (avg " << avg(inflight_dram_sum) << "/cyc, cur " << cur_inflight_dram << ")\n";
    cerr << "  inflight_resp:  " << inflight_resp_sum
         << " (avg " << avg(inflight_resp_sum) << "/cyc, cur " << cur_inflight_resp << ")\n";
  }
};

static constexpr uint64_t STAT_PERIOD = 1000;


// ===========================================================================
// SoftSystemSim — software model of the system uncore (NoC, memif, distrib).
// Aligned cycle-accurately with the HARD RTL. Driven via peek/stage/step.
// ===========================================================================

struct SoftSystemSim {
  uint64_t cycle = 0;
  SystemConfig cfg;
  int num_pu;
  int num_mc;
  int num_clusters;
  int clusters_per_mc;
  TopologyInfo topo;
  bool reset_cycle = false;
  bool reset_just_exited = false;

  vector<unique_ptr<CoreState>> cores;
  vector<SoftRouter> routers;

  // Refactored components.
  vector<unique_ptr<DramIf>> drams;        // [num_mc]
  unique_ptr<MmioIf> periph;
  vector<Distributor> dists;                // [num_clusters]

  // Per-router staged state for two-phase router step (filled in routeRouters,
  // applied after core posedge in applyRouterSteps).
  struct PendingRouterStep {
    std::vector<std::optional<Flit>> enqs;  // one per input port
    std::vector<bool> deq_accepts;          // one per output port
  };
  vector<PendingRouterStep> pending_router_steps;
  vector<pair<int, Flit>> pending_injected;

  // Saved bus state across cycles.
  // last_resp[i] is the resp received in mem(K-1); applied as in.mem_resp in step(K).
  // last_req_accepting is NOT saved across cycles — we use the live mem(K) bus_in.reqAccepting
  // by deferring the DramIf/MmioIf commit until mem(K).
  vector<optional<GlobalMemResp>> last_resp;        // [num_mc+1]  idx0=periph

  // State saved between stepPosedge(K) and mem(K): inputs to drams/periph for K,
  // and the fires resolved during stepPosedge (broadcast_ready / ring_out_ready).
  // mem_req_ready is patched in during mem() before step() is called on the module.
  vector<MemIfStepIn> dram_in_K;          // [num_mc]
  vector<DramIfStepFires> dram_fires_K;   // [num_mc] (mem_req_ready not yet captured here — implicit via "step in mem()")
  MemIfStepIn periph_in_K;
  MemIfStepFires periph_fires_K;
  // The peeked outputs from stepPosedge (we already used them for distribution &
  // back-pressure resolution); mem() re-runs step() on the modules with full fires.
  vector<DramIfStepOut> dram_peek_K;      // [num_mc]
  MemIfStepOut periph_peek_K;

  // State threaded between peek(K) → stage(K) → step(K).
  vector<DistributorStepOut> dist_out_pre_K;  // [num_clusters], populated in stage(K), consumed by step(K)
  vector<DistributorStepIn> ci_io_in_K;       // [num_clusters], populated in stage(K), consumed in step(K) by dists.step
  vector<DistributorStepFires> dist_fires_K;  // [num_clusters], populated in stage(K), consumed in step(K) by dists.step

  PeriodicStat stats;
  PeriodicStat last_periodic;
  int total_mesh_outputs = 0;

  // Runtime breakdown (ns), summed across all calls to peek/stage/step.
  // verilator_ns: time inside soft_rtl::eval() (the two per-cycle batch
  //               eval loops in stage()/step(), plus one-time reset-exit).
  // uncore_ns:    remaining time inside peek/stage/step (soft NoC model logic).
  uint64_t verilator_ns = 0;
  uint64_t uncore_ns = 0;

  // Timed batch eval of all cores. Caller must set core->clock before
  // calling. Adds elapsed wall time to g_soft_verilator_ns so the outer
  // PhaseTimer correctly attributes it to "verilator" not "uncore".
  void evalAllCoresTimed() {
    auto t0 = std::chrono::high_resolution_clock::now();
    for (auto &c : cores) c->core->eval();
    auto t1 = std::chrono::high_resolution_clock::now();
    g_soft_verilator_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  }

  static MemLine makeConfigBeat0(uint32_t num_pu, uint32_t num_mc, uint32_t pus_per_mc) {
    MemLine out{};
    auto put32 = [&](int off, uint32_t v) {
      out[off+0] = v & 0xff;
      out[off+1] = (v >> 8) & 0xff;
      out[off+2] = (v >> 16) & 0xff;
      out[off+3] = (v >> 24) & 0xff;
    };
    put32(0, num_pu);
    put32(8, num_mc);
    put32(16, pus_per_mc);
    return out;
  }

  static MemLine makeConfigBeat1(uint64_t mc_size) {
    MemLine out{};
    for (int b = 0; b < 8; ++b) out[b] = (mc_size >> (8*b)) & 0xff;
    return out;
  }

  SoftSystemSim(const SystemConfig &cfg_)
      : cfg(cfg_),
        num_pu((int)cfg_.numPU), num_mc((int)cfg_.numMC),
        num_clusters((int)cfg_.numPU / 16),
        clusters_per_mc(((int)cfg_.numPU / 16) / (int)cfg_.numMC),
        topo(buildTopology((int)cfg_.numPU, (int)cfg_.numMC)),
        last_resp((int)cfg_.numMC + 1) {
    routers.resize(num_pu + 1);
    for (int id = 1; id <= num_pu; ++id) {
      cores.push_back(std::make_unique<CoreState>(id));
      routers[id] = SoftRouter(id, topo, num_mc);
    }

    for (int m = 0; m < num_mc; ++m) {
      int pu_start = m * clusters_per_mc * 16 + 1;
      int pu_end = (m + 1) * clusters_per_mc * 16;
      drams.push_back(std::make_unique<DramIf>(m, pu_start, pu_end,
                                                 clusters_per_mc, 64, 16));
    }
    periph = std::make_unique<MmioIf>(16);
    // Config ROM entries (mirror SoftPeriphIf::isConfigRom + fillConfigRomBeat).
    uint64_t mc_size = cfg.core.mcSizes.empty() ? 0 : cfg.core.mcSizes[0];
    periph->addConfigRomEntry(0x28000000u,
        makeConfigBeat0((uint32_t)num_pu, (uint32_t)num_mc, (uint32_t)(num_pu/num_mc)));
    periph->addConfigRomEntry(0x28000020u,
        makeConfigBeat1(mc_size));

    for (int ci = 0; ci < num_clusters; ++ci)
      dists.emplace_back(ci * 16 + 1);

    pending_router_steps.resize(num_pu + 1);
    dram_in_K.resize(num_mc);
    dram_fires_K.resize(num_mc);
    dram_peek_K.resize(num_mc);
    for (int m = 0; m < num_mc; ++m) {
      dram_in_K[m].req.assign(clusters_per_mc, std::nullopt);
      dram_fires_K[m].broadcast_ready.assign(clusters_per_mc, false);
    }
    periph_in_K.req.assign(1, std::nullopt);

    for (int id = 1; id <= num_pu; ++id)
      total_mesh_outputs += static_cast<int>(routers[id].mesh_dirs.size());

    // Pre-size pending router-step buffers to fixed widths per router so per-cycle
    // routeRouters() only resets values via std::fill (no realloc).
    for (int id = 1; id <= num_pu; ++id) {
      auto &r = routers[id];
      pending_router_steps[id].enqs.assign(r.rt.numInputs(), std::nullopt);
      pending_router_steps[id].deq_accepts.assign(r.outputs.size(), false);
    }

    // Pre-size per-cycle scratch buffers (member-allocated, reused each cycle).
    memif_props_buf_.resize(num_mc);
    for (auto &v : memif_props_buf_) v.reserve(clusters_per_mc * 16);
    ring_outs_buf_.assign(ringNodeCount(), std::nullopt);
    ring_in_per_node_buf_.assign(ringNodeCount(), std::nullopt);
    dist_out_K_buf_.assign(num_clusters, DistributorStepOut{});
    core_props_K.reserve(num_pu * 4);

    fprintf(stderr, "[Soft] SoftSystemSim init: numPU=%d numMC=%d numClusters=%d cpm=%d\n",
            num_pu, num_mc, num_clusters, clusters_per_mc);
  }

  CoreState &core(int pu_id) { return *cores.at(pu_id - 1); }
  SoftRouter &routerAt(int pu_id) { return routers.at(pu_id); }
  const SoftRouter &routerAt(int pu_id) const { return routers.at(pu_id); }

  std::pair<int, int> meshNeighbor(int row, int col, int dir) const {
    switch (dir) {
      case 0: return {row - 1, col};
      case 1: return {row, col + 1};
      case 2: return {row + 1, col};
      default: return {row, col - 1};
    }
  }

  // --- Ring chain helpers. Order: [drams[0], drams[1], ..., drams[N-1], periph]
  // Ring out of node i flows to ring in of node (i+1) mod (N+1).
  int ringNodeCount() const { return num_mc + 1; }

  // Compute ring_out for each node (Reg.Q based; independent of `in`).
  // Uses DramIf/MmioIf peek with empty `in`.
  void snapshotRingOuts(vector<optional<RingResp>> &outs) {
    MemIfStepIn dummy_in;
    for (int m = 0; m < num_mc; ++m) {
      dummy_in.req.assign(clusters_per_mc, std::nullopt);
      dummy_in.ring_in = std::nullopt;
      dummy_in.mem_req_ready = false;
      dummy_in.mem_resp = std::nullopt;
      auto out = drams[m]->peek(dummy_in);
      outs[m] = out.ring_out;
    }
    dummy_in.req.assign(1, std::nullopt);
    dummy_in.ring_in = std::nullopt;
    dummy_in.mem_req_ready = false;
    dummy_in.mem_resp = std::nullopt;
    outs[num_mc] = periph->peek(dummy_in).ring_out;
  }

  // --- routeRouters: peek DramIf/MmioIf/periph for req_ready decisions,
  // set router deq_accepts. Core ext_in acceptance is deferred to stage()
  // where it can be checked after PU inputs are driven and the negedge
  // eval has propagated combinational signals.
  struct CoreProp { int pu; int output_idx; Flit flit; };
  struct MemIfProp { int pu; int output_idx; int cluster_input; Flit flit; };
  struct PeriphProp { int pu; int output_idx; Flit flit; };
  vector<CoreProp> core_props_K;
  // Per-MC scratch list of memif proposals, sized [num_mc] at construction;
  // inner vectors are clear()-ed each cycle (capacity retained).
  vector<vector<MemIfProp>> memif_props_buf_;
  // Scratch buffers for snapshotRingOuts() / peek() / stage().
  vector<optional<RingResp>> ring_outs_buf_;
  vector<optional<RingResp>> ring_in_per_node_buf_;
  vector<DistributorStepOut> dist_out_K_buf_;

  void routeRouters(const vector<optional<RingResp>> &ring_in_per_node) {
    auto &memif_props = memif_props_buf_;
    for (auto &v : memif_props) v.clear();
    optional<PeriphProp> periph_prop;
    core_props_K.clear();

    for (int pu = 1; pu <= num_pu; ++pu) core(pu).clearExtInput();

    for (int pu = 1; pu <= num_pu; ++pu) {
      auto &ps = pending_router_steps[pu];
      // enqs/deq_accepts are pre-sized in ctor; just reset values.
      std::fill(ps.enqs.begin(), ps.enqs.end(), std::nullopt);
      std::fill(ps.deq_accepts.begin(), ps.deq_accepts.end(), false);
    }

    size_t num_transfers = 0;
    for (int pu = 1; pu <= num_pu; ++pu) {
      auto &r = routerAt(pu);
      for (int oi = 0; oi < (int)r.outputs.size(); ++oi) {
        auto flit_opt = r.rt.peek(oi);
        if (!flit_opt) continue;
        const Flit &flit = *flit_opt;
        const RouterOutput &output = r.outputs[oi];
        switch (output.kind) {
          case RouterOutput::Mesh: {
            auto [row, col] = topo.pu_pos[pu];
            auto next_pos = meshNeighbor(row, col, output.dir);
            int next_pu = topo.pos_to_pu.at(next_pos);
            int opp_dir = (output.dir + 2) % 4;
            auto it = routerAt(next_pu).dir_to_port.find(opp_dir);
            if (it == routerAt(next_pu).dir_to_port.end()) break;
            int ingress_port = it->second;
            if (!routerAt(next_pu).rt.canEnq(ingress_port, flit.prio())) break;
            pending_router_steps[next_pu].enqs[ingress_port] = flit;
            pending_router_steps[pu].deq_accepts[oi] = true;
            ++num_transfers;
            break;
          }
          case RouterOutput::Core:
            // Defer: requires PU's mem_unicast/broadcast inputs of this cycle
            // to be driven first before ext_in_ready can be observed.
            core_props_K.push_back({pu, oi, flit});
            break;
          case RouterOutput::MemIf:
            memif_props[output.mc_idx].push_back({pu, oi, output.mc_input, flit});
            break;
          case RouterOutput::Periph:
            periph_prop = PeriphProp{pu, oi, flit};
            break;
        }
      }
    }

    // Build dram_in_K[].req from proposals (one slot per cluster_input).
    for (int m = 0; m < num_mc; ++m) {
      auto &in = dram_in_K[m];
      in.req.assign(clusters_per_mc, std::nullopt);
      in.ring_in = ring_in_per_node[m];
      in.mem_req_ready = false;
      in.mem_resp = std::nullopt;  // filled in mem() with bus_in[K].resp
      for (auto &p : memif_props[m]) {
        if ((int)in.req.size() <= p.cluster_input) continue;
        in.req[p.cluster_input] = p.flit;
      }
      // Peek DramIf to learn which port's req_ready is true.
      auto out = drams[m]->peek(in);
      dram_peek_K[m] = out;
      // For each proposal, mark router's deq_accepts if req_ready at its ci.
      for (auto &p : memif_props[m]) {
        if (p.cluster_input < (int)out.req_ready.size() && out.req_ready[p.cluster_input]) {
          pending_router_steps[p.pu].deq_accepts[p.output_idx] = true;
        }
      }
    }

    // Periph proposal.
    periph_in_K.req.assign(1, std::nullopt);
    periph_in_K.ring_in = ring_in_per_node[num_mc];
    periph_in_K.mem_req_ready = false;
    periph_in_K.mem_resp = std::nullopt;
    if (periph_prop) periph_in_K.req[0] = periph_prop->flit;
    auto p_out = periph->peek(periph_in_K);
    periph_peek_K = p_out;
    if (periph_prop && p_out.req_ready[0]) {
      pending_router_steps[periph_prop->pu].deq_accepts[periph_prop->output_idx] = true;
    }

    stats.total_hops += num_transfers;
    stats.idle_lane_cycles += total_mesh_outputs - (int)num_transfers;
  }

  void applyRouterSteps() {
    static const bool dbg = std::getenv("SOFT_DBG_ROUTER") != nullptr;
    for (const auto &[pu, flit] : pending_injected)
      pending_router_steps[pu].enqs[routerAt(pu).inject_port] = flit;
    if (dbg) {
      for (int pu = 1; pu <= num_pu; ++pu) {
        auto &r = routerAt(pu);
        auto &ps = pending_router_steps[pu];
        for (int ip = 0; ip < (int)ps.enqs.size(); ++ip) {
          if (ps.enqs[ip].has_value()) {
            const Flit &f = *ps.enqs[ip];
            fprintf(stderr, "[router pu=%d cy=%lu] ENQ in=%d (port=%s) dst=0x%x src=%d tag=0x%x\n",
                    pu, g_soft_dbg_cycle, ip,
                    ip == r.inject_port ? "inject" : "mesh",
                    f.dst, f.src, f.tag);
          }
        }
        for (int oi = 0; oi < (int)ps.deq_accepts.size(); ++oi) {
          if (ps.deq_accepts[oi]) {
            auto flit_opt = r.rt.peek(oi);
            if (flit_opt) {
              const Flit &f = *flit_opt;
              const char *kind = "?";
              switch (r.outputs[oi].kind) {
                case RouterOutput::Mesh:   kind = "MESH";   break;
                case RouterOutput::Core:   kind = "CORE";   break;
                case RouterOutput::MemIf:  kind = "MEMIF";  break;
                case RouterOutput::Periph: kind = "PERIPH"; break;
              }
              fprintf(stderr, "[router pu=%d cy=%lu] DEQ out=%d (%s) dst=0x%x src=%d tag=0x%x\n",
                      pu, g_soft_dbg_cycle, oi, kind, f.dst, f.src, f.tag);
            }
          }
        }
      }
    }
    for (int pu = 1; pu <= num_pu; ++pu)
      routerAt(pu).rt.step(pending_router_steps[pu].enqs, pending_router_steps[pu].deq_accepts);
  }

  // --- Per-cycle interface: phased peek / stage / step ---

  // peek(K): produce the memory requests presented by the backend at cycle K.
  // Reads only state from prior posedge (K-1). Does not consume bus_in.
  void peek(uint64_t cy, vector<MemBusOut *> out) {
    cycle = cy;
    g_soft_dbg_cycle = cy;
    bool was_reset = reset_cycle;
    reset_cycle = cycle <= RESET_LENGTH;
    if (reset_cycle) {
      for (auto &p : out) p->req = std::nullopt;
      return;
    }
    if (was_reset) reset_just_exited = true;

    // First non-reset cycle: deassert reset and re-evaluate once so that
    // combinational outputs (ext_in_ready, ring_out, etc.) reflect the
    // post-reset Reg.Q values during this peek. In HARD, synchronous reset
    // releases at posedge K=RESET_LENGTH+1 with reset already deasserted.
    if (reset_just_exited) {
      for (auto &c : cores) c->core->reset = 0;
      evalAllCoresTimed();
      reset_just_exited = false;
    }

    // Snapshot ring_out for each node (Reg.Q based).
    auto &ring_outs = ring_outs_buf_;
    auto &ring_in_per_node = ring_in_per_node_buf_;
    snapshotRingOuts(ring_outs);
    int N = ringNodeCount();
    for (int i = 0; i < N; ++i) {
      int prev = (i - 1 + N) % N;
      ring_in_per_node[i] = ring_outs[prev];
    }

    // Phase 1: routeRouters — fills dram_in_K, periph_in_K, peeks dram_peek_K, periph_peek_K, sets deq_accepts.
    routeRouters(ring_in_per_node);

    // Output presented requests to frontend. (These come from peeks of DramIf/MmioIf,
    // which are register-state-only and independent of bus_in.)
    if ((int)out.size() != num_mc + 1) {
      throw runtime_error("[Soft] peek: out vector size mismatch");
    }
    out[0]->req = periph_peek_K.mem_req;
    for (int m = 0; m < num_mc; ++m) {
      out[m + 1]->req = dram_peek_K[m].mem_req;
      if (out[m + 1]->req) stats.total_mem_requests++;
    }
  }

  // stage(K, in): consume bus_in (resp + reqAccepting), drive PU inputs,
  // run a single combinational negedge eval, compute fires.
  // Does NOT advance any Reg state.
  void stage(uint64_t cy, const vector<MemBusIn> &in) {
    if (reset_cycle) {
      // In reset: drive cores->reset=1 + zero inputs, but DON'T eval
      // (eval would happen in step's posedge). Actually we need state
      // ready for posedge in step(): set inputs here.
      for (auto &c : cores) {
        c->core->reset = 1;
        c->core->mem_unicast_valid = 0;
        c->core->mem_broadcast_valid = 0;
        c->core->ext_in_valid = 0;
        c->core->ext_out_ready = 0;
      }
      // SINGLE negedge eval under reset (matches non-reset path so each
      // cycle gets a clock 1→0→1 transition for the posedge in step()).
      for (auto &c : cores) c->core->clock = 0;
      evalAllCoresTimed();
      return;
    }

    // ---- Distributor + PU broadcast handshake ----
    if ((int)in.size() != num_mc + 1) {
      throw runtime_error("[Soft] stage: in vector size mismatch");
    }

    // Build per-cluster Distributor inputs from dram_peek_K (filled in peek()).
    ci_io_in_K.assign(num_clusters, DistributorStepIn{});
    for (int ci = 0; ci < num_clusters; ++ci) {
      int mc = ci / clusters_per_mc;
      int local_ci = ci % clusters_per_mc;
      auto &dout = dram_peek_K[mc];
      ci_io_in_K[ci].unicast = dout.unicast[local_ci];
      ci_io_in_K[ci].broadcast = dout.broadcast[local_ci];
    }

    // Compute Distributor outputs (peek with dummy fires=0 — unicast/broadcast
    // valids/bits don't depend on fires; only broadcast_enq_ready does).
    dist_out_pre_K.assign(num_clusters, DistributorStepOut{});
    for (int ci = 0; ci < num_clusters; ++ci) {
      DistributorStepFires zero{};
      dist_out_pre_K[ci] = dists[ci].peek(ci_io_in_K[ci], zero);
    }

    // ---- Drive ALL per-PU inputs (no eval yet). ----
    // mem_unicast / mem_broadcast come from dist_out_pre_K. ext_in defaults
    // to 0 and is overwritten below for proposals. ext_out_ready is computed
    // from pendingOut() reading post-step(K-1) Reg.Q values (ext_out_valid
    // is registered, so reading it before this cycle's eval is correct).
    for (int pu = 1; pu <= num_pu; ++pu) {
      auto &c = core(pu);
      c.core->ext_in_valid = 0;
      c.core->ext_in_bits_data_0 = 0;
      c.core->ext_in_bits_data_1 = 0;
      c.core->ext_in_bits_data_2 = 0;
      c.core->ext_in_bits_data_3 = 0;
      c.core->ext_in_bits_tag = 0;
      int ci = (pu - 1) >> 4;
      int rel = (pu - 1) & 15;
      auto &uout = dist_out_pre_K[ci];
      auto &bout = dist_out_pre_K[ci];
      if (uout.unicast_valids & (1u << rel)) {
        c.core->mem_unicast_valid = 1;
        c.core->mem_unicast_bits_id = uout.unicast_bits.id;
        for (int w = 0; w < MEM_BUS_WORDS; ++w) {
          uint32_t v = 0;
          for (int b = 0; b < 4; ++b) v |= (uint32_t)uout.unicast_bits.data[w*4+b] << (b*8);
          c.core->mem_unicast_bits_data[w] = v;
        }
      } else {
        c.core->mem_unicast_valid = 0;
        c.core->mem_unicast_bits_id = 0;
        for (int w = 0; w < MEM_BUS_WORDS; ++w) c.core->mem_unicast_bits_data[w] = 0;
      }
      if (bout.broadcast_valids & (1u << rel)) {
        c.core->mem_broadcast_valid = 1;
        const auto &bl = bout.broadcast_bits;
        c.core->mem_broadcast_bits_line_0_pu = bl.line[0].pu;
        c.core->mem_broadcast_bits_line_0_idx = bl.line[0].idx;
        c.core->mem_broadcast_bits_line_0_data = bl.line[0].data;
        c.core->mem_broadcast_bits_line_1_pu = bl.line[1].pu;
        c.core->mem_broadcast_bits_line_1_idx = bl.line[1].idx;
        c.core->mem_broadcast_bits_line_1_data = bl.line[1].data;
        c.core->mem_broadcast_bits_line_2_pu = bl.line[2].pu;
        c.core->mem_broadcast_bits_line_2_idx = bl.line[2].idx;
        c.core->mem_broadcast_bits_line_2_data = bl.line[2].data;
        c.core->mem_broadcast_bits_line_3_pu = bl.line[3].pu;
        c.core->mem_broadcast_bits_line_3_idx = bl.line[3].idx;
        c.core->mem_broadcast_bits_line_3_data = bl.line[3].data;
        c.core->mem_broadcast_bits_tag = bl.tag;
        c.core->mem_broadcast_bits_carried_0 = bl.carried[0];
        c.core->mem_broadcast_bits_carried_1 = bl.carried[1];
      } else {
        c.core->mem_broadcast_valid = 0;
      }
    }

    // Overwrite ext_in_* for any router CORE-output proposal.
    for (auto &cp : core_props_K) {
      auto &c = core(cp.pu);
      c.core->ext_in_valid = 1;
      c.core->ext_in_bits_data_0 = cp.flit.data[0];
      c.core->ext_in_bits_data_1 = cp.flit.data[1];
      c.core->ext_in_bits_data_2 = cp.flit.data[2];
      c.core->ext_in_bits_data_3 = cp.flit.data[3];
      c.core->ext_in_bits_tag = cp.flit.tag;
    }

    // Compute ext_out_ready per PU. pendingOut() reads ext_out_valid which
    // is registered (Reg.Q-based) and therefore valid from the post-K-1
    // posedge eval without re-eval this cycle.
    for (int pu = 1; pu <= num_pu; ++pu) {
      auto &c = core(pu);
      auto out = c.pendingOut();
      if (!out) c.core->ext_out_ready = 1;
      else c.core->ext_out_ready = routerAt(pu).canInject(*out) ? 1 : 0;
    }

    // ---- SINGLE negedge eval ----
    // Flip clock to 0 and eval once per core. Combinational outputs
    // (mem_broadcast_ready, ext_in_ready, ext_out_valid in response to
    // new ext_out_ready) are valid after this point.
    for (auto &c : cores) c->core->clock = 0;
    evalAllCoresTimed();

    // Resolve ext_in_ready for each proposal → decide acceptance.
    for (auto &cp : core_props_K) {
      auto &c = core(cp.pu);
      if (c.core->ext_in_ready) {
        c.driveExtInput(cp.flit);
        pending_router_steps[cp.pu].deq_accepts[cp.output_idx] = true;
      }
    }

    // Read per-PU mem_broadcast_ready → Distributor fires.
    dist_fires_K.assign(num_clusters, DistributorStepFires{});
    for (int ci = 0; ci < num_clusters; ++ci) {
      uint16_t mask = 0;
      for (int k = 0; k < 16; ++k) {
        int pu = ci * 16 + 1 + k;
        if (core(pu).core->mem_broadcast_ready) mask |= (uint16_t)(1u << k);
      }
      dist_fires_K[ci].broadcast_readies = mask;
    }

    // Recompute Distributor peek with real fires to get broadcast_enq_ready.
    auto &dist_out_K = dist_out_K_buf_;
    for (int ci = 0; ci < num_clusters; ++ci) {
      dist_out_K[ci] = dists[ci].peek(ci_io_in_K[ci], dist_fires_K[ci]);
    }

    // Build DramIf fires.broadcast_ready[ci] from Distributor enqReady.
    for (int m = 0; m < num_mc; ++m) {
      for (int local_ci = 0; local_ci < clusters_per_mc; ++local_ci) {
        int ci = m * clusters_per_mc + local_ci;
        dram_fires_K[m].broadcast_ready[local_ci] = dist_out_K[ci].broadcast_enq_ready;
      }
    }

    // Resolve ring chain ring_out_ready.
    int N = ringNodeCount();
    for (int i = 0; i < N; ++i) {
      int next = (i + 1) % N;
      bool next_ready;
      if (next < num_mc) next_ready = dram_peek_K[next].ring_in_ready;
      else next_ready = periph_peek_K.ring_in_ready;
      if (i < num_mc) dram_fires_K[i].ring_out_ready = next_ready;
      else periph_fires_K.ring_out_ready = next_ready;
    }

    // Patch in mem_req_ready / mem_resp from frontend bus_in.
    periph_in_K.mem_req_ready = in[0].reqAccepting;
    periph_in_K.mem_resp = in[0].resp;
    for (int m = 0; m < num_mc; ++m) {
      dram_in_K[m].mem_req_ready = in[m + 1].reqAccepting;
      dram_in_K[m].mem_resp = in[m + 1].resp;
    }

    // Note: the negedge eval already happened above in the SINGLE eval block.
  }

  // step(K): commit posedge — advance all Reg state to post-K.
  void step(uint64_t cy) {
    if (reset_cycle) {
      // SINGLE posedge eval under reset.
      for (auto &c : cores) c->core->clock = 1;
      evalAllCoresTimed();
      if (cy <= 3 && std::getenv("SOFT_DBG_RESET")) {
        fprintf(stderr, "[SOFT_RESET] cy=%lu cores[0]->reset=%u\n",
                (unsigned long)cy, (unsigned)cores[0]->core->reset);
      }
      return;
    }

    // Commit distributors.
    for (int ci = 0; ci < num_clusters; ++ci) {
      dists[ci].step(ci_io_in_K[ci], dist_fires_K[ci]);
    }

    // Collect pending_injected BEFORE any posedge K commits. This reads each
    // PU's ext_out at Reg.Q post-K-1 (with ext_out_ready set in stage(K)),
    // mirroring HARD's posedge K router-queue latch input.
    pending_injected.clear();
    for (int pu = 1; pu <= num_pu; ++pu) {
      auto out = core(pu).currentOut();
      if (out) pending_injected.emplace_back(pu, *out);
    }

    // Commit drams / periph (in old code these were in mem()).
    periph->step(periph_in_K, periph_fires_K);
    for (int m = 0; m < num_mc; ++m) {
      drams[m]->step(dram_in_K[m], dram_fires_K[m]);
    }

    // ---- SINGLE posedge eval ----
    // Flip clock to 1 and eval once per core. Registers latch their post-K
    // state; combinational outputs reflect new Reg.Q values.
    for (auto &c : cores) c->core->clock = 1;
    evalAllCoresTimed();

    applyRouterSteps();
    stats.total_injections += pending_injected.size();

    // Periodic stats
    if (cycle % STAT_PERIOD == 0) {
      PeriodicStat delta = stats - last_periodic;
      delta.print(STAT_PERIOD);
      last_periodic = stats;
    }
  }
};

struct SoftSystemModel::Impl {
  std::unique_ptr<SoftSystemSim> sim;
  std::vector<MemTraceEvent> last_mem_trace;
  std::vector<PeriphTraceEvent> last_periph_trace;

  Impl(const SystemConfig &cfg) {
    sim = std::make_unique<SoftSystemSim>(cfg);
  }

  std::vector<std::unique_ptr<CoreState>> &cores() { return sim->cores; }

  // RAII helper that attributes wall time between verilator eval (tracked
  // separately via g_soft_verilator_ns) and uncore logic.
  struct PhaseTimer {
    SoftSystemSim &sim;
    std::chrono::high_resolution_clock::time_point t0;
    uint64_t v_at_entry;
    PhaseTimer(SoftSystemSim &s)
      : sim(s),
        t0(std::chrono::high_resolution_clock::now()),
        v_at_entry(g_soft_verilator_ns) {}
    ~PhaseTimer() {
      auto t1 = std::chrono::high_resolution_clock::now();
      uint64_t total =
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      uint64_t v = g_soft_verilator_ns - v_at_entry;
      sim.verilator_ns += v;
      sim.uncore_ns += (total > v ? total - v : 0);
    }
  };

  void peek(uint64_t cycle, std::vector<MemBusOut *> out) {
    PhaseTimer _t(*sim);
    last_mem_trace.clear();
    last_periph_trace.clear();
    current_mem_trace = &last_mem_trace;
    current_periph_trace = &last_periph_trace;
    sim->peek(cycle, std::move(out));
  }

  void stage(uint64_t cycle, const std::vector<MemBusIn> &in) {
    PhaseTimer _t(*sim);
    sim->stage(cycle, in);
  }

  void step(uint64_t cycle) {
    PhaseTimer _t(*sim);
    sim->step(cycle);
    current_mem_trace = nullptr;
    current_periph_trace = nullptr;
  }

  SystemConfig config() const {
    return sim->cfg;
  }

  void printStats(uint64_t cycles) { sim->stats.print(cycles); }
};

bool openSoftSystemModelMemTrace(const std::optional<std::string> &path, std::string *error) {
  mem_trace.reset();
  if (!path) return true;

  mem_trace.reset(new ofstream(*path));
  if (*mem_trace) return true;

  if (error) *error = "Error: cannot open memory trace file " + *path;
  mem_trace.reset();
  return false;
}

void setSoftSystemModelLogging(bool enabled) {
  LOG = enabled;
}

SoftSystemModel::SoftSystemModel(SystemConfig cfg) : impl_(std::make_unique<Impl>(cfg)) {}
SoftSystemModel::~SoftSystemModel() = default;
SoftSystemModel::SoftSystemModel(SoftSystemModel &&) noexcept = default;
SoftSystemModel &SoftSystemModel::operator=(SoftSystemModel &&) noexcept = default;

void SoftSystemModel::attachTrace(VerilatedFstC *trace_file, int depth) {
  TRACE = trace_file != nullptr;
  tracer = trace_file;
  // NOTE: do NOT call core->trace() here. soft_rtl::trace() forwards to
  // contextp()->trace() which iterates *all* models in the shared default
  // VerilatedContext (including HARD's sys when cosim'ing). Doing it again
  // here causes duplicate registration of every model, which silently breaks
  // FST dumping (clock/reset signals stop transitioning in the trace).
  // The shared context is registered exactly once at top-level (system.cpp).
}

void SoftSystemModel::peek(uint64_t cycle, std::vector<MemBusOut *> out) {
  impl_->peek(cycle, std::move(out));
}

void SoftSystemModel::stage(uint64_t cycle, const std::vector<MemBusIn> &in) {
  impl_->stage(cycle, in);
}

void SoftSystemModel::step(uint64_t cycle) {
  impl_->step(cycle);
}

SystemConfig SoftSystemModel::config() const {
  return impl_->config();
}

bool SoftSystemModel::printStats(uint64_t cycles, bool final_print) {
  if (final_print) {
    cerr << "[Soft] Final stats (over " << cycles << " cycles):\n";
    impl_->printStats(cycles);
    double v_ms = impl_->sim->verilator_ns / 1e6;
    double u_ms = impl_->sim->uncore_ns / 1e6;
    double tot_ms = v_ms + u_ms;
    auto pct = [&](double x) { return tot_ms > 0 ? (100.0 * x / tot_ms) : 0.0; };
    fprintf(stderr,
            "[Soft] Time breakdown: verilator=%.3fms (%.1f%%) "
            "uncore=%.3fms (%.1f%%) total=%.3fms\n",
            v_ms, pct(v_ms), u_ms, pct(u_ms), tot_ms);
  }
  return true;
}