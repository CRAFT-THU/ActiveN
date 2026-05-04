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
#include "soft_backend.h"
#include "system.h"
#include "system_config.h"

using namespace std;

static bool TRACE = false;
static bool LOG = false;
static bool exiting = false;

static const uint64_t RESET_LENGTH = 10;
static const uint32_t TEXT_BASE = 0x80000000u;
static const int MEM_BUS_WORDS = 256 / 32;
static const uint16_t PERIPH_DST = 0x8000;
static const uint16_t FIRST_MC_DST = 0x8001;
static const uint16_t SCATTER_RESP_ID = 64;

static VerilatedFstC *tracer = nullptr;
static std::unique_ptr<std::ofstream> mem_trace;
static std::vector<MemTraceEvent> *current_mem_trace = nullptr;
static std::vector<PeriphTraceEvent> *current_periph_trace = nullptr;

static void sighandler(int) { exiting = true; }

struct MemResponse {
  uint16_t id = 0;
  uint32_t data[MEM_BUS_WORDS] = {};
};

struct RingResponse {
  uint16_t dst = 0;
  uint16_t id = 0;
  uint32_t data[MEM_BUS_WORDS] = {};
};

struct ClusterRespBuffer {
  bool valid = false;
  bool draining = false;
  bool pending_valid = false;
  RingResponse resp;
  RingResponse pending_resp;
};

struct Flit {
  uint16_t src = 0;
  uint16_t dst = 0;
  uint16_t tag = 0;
  uint32_t data[4] = {};
};

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

struct BcastEntry {
  uint16_t line_pu[4];
  uint16_t line_idx[4];
  uint32_t line_data[4];
  uint16_t tag;
  uint32_t carried[2];
};

struct CoreState {
  std::unique_ptr<soft_rtl> core;
  std::optional<Flit> ext_input;
  std::deque<MemResponse> mem_inbox;
  std::deque<BcastEntry> bcast_inbox;

  explicit CoreState(int hartid) {
    std::string name = "soft_pu_" + std::to_string(hartid);
    core.reset(new soft_rtl(name.c_str()));
    core->cfg_hartid = hartid;
    core->ext_out_ready = 0;
  }

  ~CoreState() {
    core->final();
  }

  void retireInputs() {
    if (core->mem_unicast_valid && !mem_inbox.empty()) mem_inbox.pop_front();
    if (core->mem_broadcast_valid && core->mem_broadcast_ready && !bcast_inbox.empty()) bcast_inbox.pop_front();
  }

  void clearExtInput() {
    ext_input.reset();
  }

  bool canAcceptExtInput(const Flit &flit) {
    uint8_t saved_valid = core->ext_in_valid;
    uint32_t saved_data[4] = {core->ext_in_bits_data_0, core->ext_in_bits_data_1, core->ext_in_bits_data_2, core->ext_in_bits_data_3};
    uint16_t saved_tag = core->ext_in_bits_tag;

    core->ext_in_valid = 1;
    core->ext_in_bits_data_0 = flit.data[0];
    core->ext_in_bits_data_1 = flit.data[1];
    core->ext_in_bits_data_2 = flit.data[2];
    core->ext_in_bits_data_3 = flit.data[3];
    core->ext_in_bits_tag = flit.tag;
    core->eval();
    bool ready = core->ext_in_ready;

    core->ext_in_valid = saved_valid;
    core->ext_in_bits_data_0 = saved_data[0];
    core->ext_in_bits_data_1 = saved_data[1];
    core->ext_in_bits_data_2 = saved_data[2];
    core->ext_in_bits_data_3 = saved_data[3];
    core->ext_in_bits_tag = saved_tag;
    core->eval();
    return ready;
  }

  void driveExtInput(const Flit &flit) {
    ext_input = flit;
  }

  void driveInputs() {
    if (!ext_input) {
      core->ext_in_valid = 0;
      core->ext_in_bits_data_0 = 0;
      core->ext_in_bits_data_1 = 0;
      core->ext_in_bits_data_2 = 0;
      core->ext_in_bits_data_3 = 0;
      core->ext_in_bits_tag = 0;
    } else {
      const auto &f = *ext_input;
      core->ext_in_valid = 1;
      core->ext_in_bits_data_0 = f.data[0];
      core->ext_in_bits_data_1 = f.data[1];
      core->ext_in_bits_data_2 = f.data[2];
      core->ext_in_bits_data_3 = f.data[3];
      core->ext_in_bits_tag = f.tag;
    }

    if (mem_inbox.empty()) {
      core->mem_unicast_valid = 0;
      core->mem_unicast_bits_id = 0;
      for (int i = 0; i < MEM_BUS_WORDS; ++i) core->mem_unicast_bits_data[i] = 0;
    } else {
      const auto &r = mem_inbox.front();
      core->mem_unicast_valid = 1;
      core->mem_unicast_bits_id = r.id;
      for (int i = 0; i < MEM_BUS_WORDS; ++i) core->mem_unicast_bits_data[i] = r.data[i];
    }

    if (bcast_inbox.empty()) {
      core->mem_broadcast_valid = 0;
    } else {
      const auto &b = bcast_inbox.front();
      core->mem_broadcast_valid = 1;
      core->mem_broadcast_bits_line_0_pu = b.line_pu[0];
      core->mem_broadcast_bits_line_0_idx = b.line_idx[0];
      core->mem_broadcast_bits_line_0_data = b.line_data[0];
      core->mem_broadcast_bits_line_1_pu = b.line_pu[1];
      core->mem_broadcast_bits_line_1_idx = b.line_idx[1];
      core->mem_broadcast_bits_line_1_data = b.line_data[1];
      core->mem_broadcast_bits_line_2_pu = b.line_pu[2];
      core->mem_broadcast_bits_line_2_idx = b.line_idx[2];
      core->mem_broadcast_bits_line_2_data = b.line_data[2];
      core->mem_broadcast_bits_line_3_pu = b.line_pu[3];
      core->mem_broadcast_bits_line_3_idx = b.line_idx[3];
      core->mem_broadcast_bits_line_3_data = b.line_data[3];
      core->mem_broadcast_bits_tag = b.tag;
      core->mem_broadcast_bits_carried_0 = b.carried[0];
      core->mem_broadcast_bits_carried_1 = b.carried[1];
    }
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

  void posedge() {
    core->clock = 1;
    core->eval();
  }

  void negedge() {
    core->clock = 0;
    core->eval();
  }
};

struct MemInflightSlot {
  bool allocated = false;
  bool ready = false;
  bool issued = false;
  bool completed = false;
  uint16_t src = 0;
  uint16_t resp_id = 0;
  uint32_t addr = 0;
  uint16_t size = 0;
  uint32_t wdata = 0;
  bool is_write = false;
  uint8_t remaining_flits = 0;
  uint32_t data[MEM_BUS_WORDS] = {};
};

struct PeriphInflightSlot {
  bool allocated = false;
  bool ready = false;
  bool issued = false;
  bool completed = false;
  uint16_t src = 0;
  uint16_t resp_id = 0;
  uint32_t addr = 0;
  uint16_t size = 0;
  uint32_t wdata = 0;
  bool is_write = false;
  uint8_t remaining_flits = 0;
  uint32_t data[MEM_BUS_WORDS] = {};
};

struct SoftSystemSim;

struct SoftMemIf {
  static constexpr int kMaxInflight = 64;

  int mc_idx;
  int pu_start;
  int pu_end;
  vector<int> zone_clusters;
  int ingress_rr = 0;
  int issue_rr = 0;
  int local_resp_rr = 0;
  int remote_resp_rr = 0;
  bool ring_enq_prefer_incoming = true;
  SoftSystemSim *sim_ptr = nullptr;  // set during init

  array<MemInflightSlot, kMaxInflight> inflight = {};
  multimap<uint64_t, int> ready_events;
  multimap<uint64_t, int> free_events;
  multimap<uint64_t, std::pair<int, MemResponse>> completion_events;
  deque<RingResponse> ring_queue;

  // Scatter FSM (matches RTL MemIf scatter state machine)
  enum ScatState { ScatIdle, ScatIssue, ScatWait, ScatBcast };
  ScatState scat_state = ScatIdle;
  uint32_t scat_addr = 0;
  uint32_t scat_end = 0;
  uint32_t scat_data[MEM_BUS_WORDS] = {};
  uint16_t scat_resp_tag = 0;
  uint32_t scat_extras[2] = {};
  int scat_cluster = 0;

  SoftMemIf(int mc, int start, int end, vector<int> clusters)
      : mc_idx(mc), pu_start(start), pu_end(end), zone_clusters(std::move(clusters)) {
    ingress_rr = zone_clusters.size() > 1 ? 1 : 0;
  }

  int findCollectSlot(uint16_t src) const {
    for (int slot = 0; slot < kMaxInflight; ++slot) {
      const auto &entry = inflight[slot];
      if (entry.allocated && !entry.ready && entry.remaining_flits > 0 && entry.src == src) {
        return slot;
      }
    }
    return -1;
  }

  int findFreeSlot() const {
    for (int slot = 0; slot < kMaxInflight; ++slot) {
      if (!inflight[slot].allocated) return slot;
    }
    return -1;
  }

  int findIssueSlot() const {
    for (int offset = 0; offset < kMaxInflight; ++offset) {
      int slot = (issue_rr + offset) % kMaxInflight;
      const auto &entry = inflight[slot];
      if (entry.allocated && entry.ready && !entry.issued) return slot;
    }
    return -1;
  }

  void clearSlot(int slot) {
    inflight[slot] = MemInflightSlot{};
  }

  int localCluster(uint16_t pu_id) const {
    return static_cast<int>((pu_id - pu_start) >> 4);
  }

  int findCompletedLocalSlot() const {
    for (int offset = 0; offset < kMaxInflight; ++offset) {
      int slot = (local_resp_rr + offset) % kMaxInflight;
      const auto &entry = inflight[slot];
      if (entry.allocated && entry.completed && isLocal(entry.src)) return slot;
    }
    return -1;
  }

  int findCompletedRemoteSlot() const {
    for (int offset = 0; offset < kMaxInflight; ++offset) {
      int slot = (remote_resp_rr + offset) % kMaxInflight;
      const auto &entry = inflight[slot];
      if (entry.allocated && entry.completed && !isLocal(entry.src)) return slot;
    }
    return -1;
  }

  RingResponse makeRingResp(int slot) const {
    RingResponse resp;
    const auto &entry = inflight[slot];
    resp.dst = entry.src;
    resp.id = entry.resp_id;
    memcpy(resp.data, entry.data, sizeof(resp.data));
    return resp;
  }

  // Pending request for external servicing via mem()
  // -1 = none, 0..63 = normal slot, kMaxInflight = scatter read
  int pending_issue_slot = -1;

  // Deferred responses: stored in mem(), applied at start of next step()
  vector<MemResponse> deferred_responses;

  void accept(const Flit &flit, uint64_t cycle);
  void advanceSlots(uint64_t cycle);
  void advanceScatter(uint64_t cycle);
  void findPendingReq(uint64_t cycle, SoftSystemSim &sim);
  void acceptIssue(uint64_t cycle);
  void applyDeferredResponses();
  void deferResponse(const MemResponse &resp);
  int numIngress() const { return static_cast<int>(zone_clusters.size()); }

  bool isLocal(uint16_t pu_id) const {
    return pu_id >= pu_start && pu_id <= pu_end;
  }
};

struct SoftPeriphIf {
  static constexpr int kMaxInflight = 16;

  array<PeriphInflightSlot, kMaxInflight> inflight = {};
  multimap<uint64_t, int> ready_events;
  multimap<uint64_t, int> free_events;
  multimap<uint64_t, std::pair<int, MemResponse>> completion_events;
  deque<RingResponse> ring_queue;
  int issue_rr = 0;
  int completed_rr = 0;
  bool ring_enq_prefer_incoming = true;

  int findCollectSlot(uint16_t src) const {
    for (int slot = 0; slot < kMaxInflight; ++slot) {
      const auto &entry = inflight[slot];
      if (entry.allocated && !entry.ready && entry.remaining_flits > 0 && entry.src == src) {
        return slot;
      }
    }
    return -1;
  }

  int findFreeSlot() const {
    for (int slot = 0; slot < kMaxInflight; ++slot) {
      if (!inflight[slot].allocated) return slot;
    }
    return -1;
  }

  int findIssueSlot() const {
    for (int offset = 0; offset < kMaxInflight; ++offset) {
      int slot = (issue_rr + offset) % kMaxInflight;
      const auto &entry = inflight[slot];
      if (entry.allocated && entry.ready && !entry.issued) return slot;
    }
    return -1;
  }

  void clearSlot(int slot) {
    inflight[slot] = PeriphInflightSlot{};
  }

  int findCompletedSlot() const {
    for (int offset = 0; offset < kMaxInflight; ++offset) {
      int slot = (completed_rr + offset) % kMaxInflight;
      const auto &entry = inflight[slot];
      if (entry.allocated && entry.completed) return slot;
    }
    return -1;
  }

  RingResponse makeRingResp(int slot) const {
    RingResponse resp;
    const auto &entry = inflight[slot];
    resp.dst = entry.src;
    resp.id = entry.resp_id;
    memcpy(resp.data, entry.data, sizeof(resp.data));
    return resp;
  }

  // Pending request for external servicing via mem()
  int pending_issue_slot = -1;

  // Deferred responses: stored in mem(), applied at start of next step()
  vector<MemResponse> deferred_responses;

  void accept(const Flit &flit, uint64_t cycle);
  void advanceSlots(uint64_t cycle);
  void findPendingReq(uint64_t cycle, SoftSystemSim &sim);
  void acceptIssue(uint64_t cycle);
  void applyDeferredResponses(uint64_t cycle);
  void deferResponse(const MemResponse &resp);
};

static int flitVc(const Flit &flit) {
  return (flit.tag == 0xF00 || flit.tag == 0xF01) ? 0 : 1;
}

struct RouterOutput {
  enum Kind { Mesh, Core, MemIf, Periph } kind = Mesh;
  int dir = -1;
  int mc_idx = -1;
  int mc_input = -1;
};

struct SoftRouter {
  static constexpr int kNumVc = 2;
  static constexpr int kBufferDepth = 4;

  int pu_id = 0;
  int inject_port = 0;
  vector<int> mesh_dirs;
  map<int, int> dir_to_port;
  vector<RouterOutput> outputs;
  vector<array<deque<Flit>, kNumVc>> input_vcs;
  vector<array<int, kNumVc>> rr_next;
  vector<int> local_mc_output;
  int core_output = -1;
  int periph_output = -1;

  SoftRouter() = default;

  SoftRouter(int pu, const TopologyInfo &topo, int num_mc)
      : pu_id(pu), local_mc_output(num_mc, -1) {
    mesh_dirs = topo.pu_mesh_dirs[pu];
    dir_to_port = topo.pu_dir_to_egress[pu];
    inject_port = static_cast<int>(mesh_dirs.size());
    input_vcs.resize(inject_port + 1);

    for (int dir : mesh_dirs) {
      outputs.push_back(
          RouterOutput{.kind = RouterOutput::Mesh, .dir = dir, .mc_idx = -1, .mc_input = -1});
    }

    core_output = static_cast<int>(outputs.size());
    outputs.push_back(
        RouterOutput{.kind = RouterOutput::Core, .dir = -1, .mc_idx = -1, .mc_input = -1});

    for (int mc_idx : topo.pu_mc_ejects[pu]) {
      int mc_input = -1;
      for (size_t ci = 0; ci < topo.mc_conns[mc_idx].size(); ++ci) {
        if (topo.mc_conns[mc_idx][ci] == pu) {
          mc_input = static_cast<int>(ci);
          break;
        }
      }
      local_mc_output[mc_idx] = static_cast<int>(outputs.size());
      outputs.push_back(RouterOutput{
          .kind = RouterOutput::MemIf,
          .dir = -1,
          .mc_idx = mc_idx,
          .mc_input = mc_input,
      });
    }

    if (pu == 1) {
      periph_output = static_cast<int>(outputs.size());
      outputs.push_back(
          RouterOutput{.kind = RouterOutput::Periph, .dir = -1, .mc_idx = -1, .mc_input = -1});
    }

    rr_next.assign(outputs.size(), std::array<int, kNumVc>{});
    int initial_rr = input_vcs.size() > 1 ? 1 : 0;
    for (auto &rr : rr_next) rr.fill(initial_rr);
  }

  bool canAccept(int input_port, int vc) const {
    return static_cast<int>(input_vcs[input_port][vc].size()) < kBufferDepth;
  }

  bool canInject(const Flit &flit) const {
    return canAccept(inject_port, flitVc(flit));
  }

  void enqueue(int input_port, const Flit &flit) {
    input_vcs[input_port][flitVc(flit)].push_back(flit);
  }

  void enqueueInject(const Flit &flit) {
    enqueue(inject_port, flit);
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

struct SoftSystemSim {
  uint64_t cycle = 0;
  int num_pu;
  int num_mc;
  TopologyInfo topo;
  bool reset_cycle = false;

  vector<unique_ptr<CoreState>> cores;
  vector<SoftRouter> routers;
  vector<SoftMemIf> memifs;
  SoftPeriphIf periph_if;
  vector<ClusterRespBuffer> cluster_buffers;

  vector<uint64_t> mc_base;
  vector<multimap<uint64_t, MemResponse>> core_mem;
  vector<pair<int, Flit>> pending_injected;

  PeriodicStat stats;
  PeriodicStat last_periodic;
  int total_mesh_outputs = 0;

  SoftSystemSim(int pu, int mc)
      : num_pu(pu), num_mc(mc), topo(buildTopology(pu, mc)), core_mem(pu + 1) {
    routers.resize(num_pu + 1);
    for (int id = 1; id <= num_pu; ++id) {
      cores.push_back(std::make_unique<CoreState>(id));
      routers[id] = SoftRouter(id, topo, num_mc);
    }

    int num_clusters = num_pu / 16;
    int clusters_per_mc = num_clusters / num_mc;
    for (int mc_idx = 0; mc_idx < num_mc; ++mc_idx) {
      int pu_start = mc_idx * clusters_per_mc * 16 + 1;
      int pu_end = (mc_idx + 1) * clusters_per_mc * 16;
      memifs.emplace_back(mc_idx, pu_start, pu_end, topo.mc_zones[mc_idx]);
      memifs.back().sim_ptr = this;
    }

    cluster_buffers.resize(num_pu / 16);

    mc_base.resize(num_mc);
    uint64_t cumul = 0;
    for (int i = 0; i < num_mc; ++i) {
      mc_base[i] = cumul;
      if (i < SYSTEM_NUM_MEM_CTRL) cumul += SYSTEM_MC_SIZES[i];
    }

    for (int id = 1; id <= num_pu; ++id)
      total_mesh_outputs += static_cast<int>(routers[id].mesh_dirs.size());
  }

  CoreState &core(int pu_id) { return *cores.at(pu_id - 1); }

  SoftRouter &router(int pu_id) { return routers.at(pu_id); }
  const SoftRouter &router(int pu_id) const { return routers.at(pu_id); }

  std::pair<int, int> meshNeighbor(int row, int col, int dir) const {
    switch (dir) {
      case 0: return {row - 1, col};
      case 1: return {row, col + 1};
      case 2: return {row + 1, col};
      default: return {row, col - 1};
    }
  }

  optional<int> routeOutput(int pu_id, const Flit &flit) const {
    const auto &r = router(pu_id);
    if (flit.dst == 0 || flit.dst == pu_id) return r.core_output;

    if (flit.dst == PERIPH_DST && r.periph_output >= 0) return r.periph_output;

    if (flit.dst >= FIRST_MC_DST && flit.dst < FIRST_MC_DST + num_mc) {
      int mc_idx = flit.dst - FIRST_MC_DST;
      if (r.local_mc_output[mc_idx] >= 0) return r.local_mc_output[mc_idx];
    }

    auto it = topo.pu_tables[pu_id].find(flit.dst);
    if (it != topo.pu_tables[pu_id].end()) return it->second;
    return {};
  }

  void deliverMemResponses() {
    for (int pu = 1; pu <= num_pu; ++pu) {
      auto &mq = core_mem[pu];
      auto mems = mq.equal_range(cycle);
      for (auto it = mems.first; it != mems.second; ++it) core(pu).mem_inbox.push_back(it->second);
      mq.erase(mems.first, mems.second);
    }
  }

  int clusterIndex(uint16_t pu_id) const {
    return static_cast<int>((pu_id - 1) >> 4);
  }

  bool clusterCanAccept(int cluster_idx) const {
    return !cluster_buffers[cluster_idx].valid;
  }

  void loadClusterBuffer(int cluster_idx, const RingResponse &resp) {
    auto &buffer = cluster_buffers[cluster_idx];
    if (buffer.valid || buffer.pending_valid) {
      throw runtime_error("SoftSystemSim: cluster response buffer overflow");
    }
    buffer.pending_valid = true;
    buffer.pending_resp = resp;
  }

  void presentClusterResponses() {
    for (int ci = 0; ci < static_cast<int>(cluster_buffers.size()); ++ci) {
      auto &buffer = cluster_buffers[ci];
      if (!buffer.valid) continue;

      MemResponse resp;
      resp.id = buffer.resp.id;
      memcpy(resp.data, buffer.resp.data, sizeof(resp.data));
      if (buffer.resp.dst == 0xFFFFu) {
        int pu_start = ci * 16 + 1;
        int pu_end = pu_start + 16;
        for (int pu = pu_start; pu < pu_end; ++pu) core_mem[pu].emplace(cycle, resp);
      } else {
        core_mem[buffer.resp.dst].emplace(cycle, resp);
      }
      buffer.draining = true;
    }
  }

  void commitClusterBuffers() {
    for (auto &buffer : cluster_buffers) {
      if (buffer.draining) {
        buffer.valid = false;
        buffer.draining = false;
      }
      if (buffer.pending_valid) {
        buffer.resp = buffer.pending_resp;
        buffer.valid = true;
        buffer.pending_valid = false;
      }
    }
  }

  void processResponseNetwork() {
    vector<vector<bool>> resp_busy(num_mc);
    for (int mc = 0; mc < num_mc; ++mc) {
      resp_busy[mc].assign(memifs[mc].zone_clusters.size(), false);
      int slot = memifs[mc].findCompletedLocalSlot();
      if (slot < 0) continue;

      auto &entry = memifs[mc].inflight[slot];
      int local_ci = memifs[mc].localCluster(entry.src);
      int global_ci = clusterIndex(entry.src);
      if (!clusterCanAccept(global_ci)) continue;

      loadClusterBuffer(global_ci, memifs[mc].makeRingResp(slot));
      resp_busy[mc][local_ci] = true;
      memifs[mc].local_resp_rr = (slot + 1) % SoftMemIf::kMaxInflight;
      memifs[mc].free_events.emplace(cycle + 1, slot);
    }

    struct NodeRef {
      bool is_periph = false;
      int index = -1;
    };

    vector<NodeRef> ring_nodes;
    ring_nodes.reserve(num_mc + 1);
    for (int mc = 0; mc < num_mc; ++mc) ring_nodes.push_back(NodeRef{.is_periph = false, .index = mc});
    ring_nodes.push_back(NodeRef{.is_periph = true, .index = 0});

    auto nodeQueue = [&](const NodeRef &node) -> deque<RingResponse> & {
      return node.is_periph ? periph_if.ring_queue : memifs[node.index].ring_queue;
    };
    auto nodeQueueSize = [&](const NodeRef &node) {
      return static_cast<int>(node.is_periph ? periph_if.ring_queue.size() : memifs[node.index].ring_queue.size());
    };
    auto nodeLocal = [&](const NodeRef &node, uint16_t dst) {
      return !node.is_periph && memifs[node.index].isLocal(dst);
    };
    auto nodeLocalCluster = [&](const NodeRef &node, uint16_t dst) {
      return memifs[node.index].localCluster(dst);
    };
    auto nodeRingEnqPreferIncoming = [&](const NodeRef &node) -> bool & {
      return node.is_periph ? periph_if.ring_enq_prefer_incoming
                            : memifs[node.index].ring_enq_prefer_incoming;
    };

    vector<optional<RingResponse>> ring_front(ring_nodes.size());
    vector<bool> pop_front(ring_nodes.size(), false);
    vector<optional<RingResponse>> ring_enqueue(ring_nodes.size());
    vector<int> completed_slots(ring_nodes.size(), -1);

    for (size_t i = 0; i < ring_nodes.size(); ++i) {
      auto &queue = nodeQueue(ring_nodes[i]);
      if (!queue.empty()) ring_front[i] = queue.front();
    }

    for (size_t i = 0; i < ring_nodes.size(); ++i) {
      int prev = (static_cast<int>(i) - 1 + static_cast<int>(ring_nodes.size())) %
                 static_cast<int>(ring_nodes.size());
      if (!ring_front[prev]) continue;

      const RingResponse &incoming = *ring_front[prev];
      const auto &node = ring_nodes[i];
      if (nodeLocal(node, incoming.dst)) {
        int local_ci = nodeLocalCluster(node, incoming.dst);
        int global_ci = clusterIndex(incoming.dst);
        if (!resp_busy[node.index][local_ci] && clusterCanAccept(global_ci)) {
          loadClusterBuffer(global_ci, incoming);
          resp_busy[node.index][local_ci] = true;
          pop_front[prev] = true;
        }
        continue;
      }
    }

    for (size_t i = 0; i < ring_nodes.size(); ++i) {
      int prev = (static_cast<int>(i) - 1 + static_cast<int>(ring_nodes.size())) %
                 static_cast<int>(ring_nodes.size());
      const auto &node = ring_nodes[i];
      int queue_size = nodeQueueSize(node);
      if (queue_size > 2) {
        throw runtime_error("SoftSystemSim: ring response queue overflow");
      }

      RingResponse incoming;
      bool has_incoming = false;
      if (ring_front[prev] && !nodeLocal(node, ring_front[prev]->dst)) {
        incoming = *ring_front[prev];
        has_incoming = true;
      }

      RingResponse outgoing;
      bool has_completed = false;
      int completed_slot = -1;

      if (node.is_periph) {
        int slot = periph_if.findCompletedSlot();
        if (slot >= 0) {
          completed_slot = slot;
          outgoing = periph_if.makeRingResp(slot);
          has_completed = true;
        }
      } else {
        int slot = memifs[node.index].findCompletedRemoteSlot();
        if (slot >= 0) {
          completed_slot = slot;
          outgoing = memifs[node.index].makeRingResp(slot);
          has_completed = true;
        }
      }

      bool can_enqueue = queue_size < 2;
      if (!can_enqueue) continue;
      if (has_incoming && has_completed) {
        bool &prefer_incoming = nodeRingEnqPreferIncoming(node);
        if (prefer_incoming) {
          ring_enqueue[i] = incoming;
          pop_front[prev] = true;
        } else {
          ring_enqueue[i] = outgoing;
          completed_slots[i] = completed_slot;
        }
        prefer_incoming = !prefer_incoming;
        continue;
      }

      if (has_incoming) {
        ring_enqueue[i] = incoming;
        pop_front[prev] = true;
        continue;
      }

      if (has_completed) {
        ring_enqueue[i] = outgoing;
        completed_slots[i] = completed_slot;
      }
    }

    for (size_t i = 0; i < ring_nodes.size(); ++i) {
      if (!pop_front[i]) continue;
      auto &queue = nodeQueue(ring_nodes[i]);
      if (!queue.empty()) queue.pop_front();
    }

    for (size_t i = 0; i < ring_nodes.size(); ++i) {
      if (!ring_enqueue[i]) continue;
      auto &queue = nodeQueue(ring_nodes[i]);
      queue.push_back(*ring_enqueue[i]);

      int slot = completed_slots[i];
      if (slot < 0) continue;
      if (ring_nodes[i].is_periph) {
        periph_if.completed_rr = (slot + 1) % SoftPeriphIf::kMaxInflight;
        periph_if.free_events.emplace(cycle + 1, slot);
      } else {
        memifs[ring_nodes[i].index].remote_resp_rr = (slot + 1) % SoftMemIf::kMaxInflight;
        memifs[ring_nodes[i].index].free_events.emplace(cycle + 1, slot);
      }
    }
  }

  int pickRouterInput(int pu_id, int output_idx, int vc) const {
    const auto &r = router(pu_id);
    int num_inputs = static_cast<int>(r.input_vcs.size());
    int start = r.rr_next[output_idx][vc] % std::max(1, num_inputs);
    for (int offset = 0; offset < num_inputs; ++offset) {
      int input_idx = (start + offset) % num_inputs;
      const auto &q = r.input_vcs[input_idx][vc];
      if (q.empty()) continue;
      auto target = routeOutput(pu_id, q.front());
      if (target && *target == output_idx) return input_idx;
    }
    return -1;
  }

  void routeRouters() {
    struct MeshTransfer {
      int dst_pu = 0;
      int ingress_port = 0;
      Flit flit;
    };
    struct DeferredPop {
      int pu = 0;
      int output_idx = -1;
      int input_idx = -1;
      int vc = 0;
    };
    struct MemIfProposal {
      int pu = 0;
      int output_idx = 0;
      int input_idx = 0;
      int vc = 0;
      int cluster_input = 0;
      Flit flit;
    };

    vector<MeshTransfer> transfers;
    vector<DeferredPop> deferred_pops;
    vector<vector<MemIfProposal>> memif_props(num_mc);
    for (int pu = 1; pu <= num_pu; ++pu) core(pu).clearExtInput();

    // Compute demand per output port for blocking stats
    vector<vector<int>> demand(num_pu + 1);
    for (int pu = 1; pu <= num_pu; ++pu) {
      auto &r = router(pu);
      demand[pu].assign(r.outputs.size(), 0);
      for (int input_idx = 0; input_idx < static_cast<int>(r.input_vcs.size()); ++input_idx) {
        for (int vc = 0; vc < SoftRouter::kNumVc; ++vc) {
          auto &q = r.input_vcs[input_idx][vc];
          if (q.empty()) continue;
          auto target = routeOutput(pu, q.front());
          if (target) demand[pu][*target]++;
        }
      }
    }

    for (int pu = 1; pu <= num_pu; ++pu) {
      auto &r = router(pu);

      for (int output_idx = 0; output_idx < static_cast<int>(r.outputs.size()); ++output_idx) {
        int chosen_input = -1;
        int chosen_vc = -1;
        for (int vc = SoftRouter::kNumVc - 1; vc >= 0; --vc) {
          chosen_input = pickRouterInput(pu, output_idx, vc);
          if (chosen_input >= 0) {
            chosen_vc = vc;
            break;
          }
        }
        if (chosen_input < 0) continue;

        const Flit flit = r.input_vcs[chosen_input][chosen_vc].front();
        const auto &output = r.outputs[output_idx];
        bool fired = false;

        switch (output.kind) {
          case RouterOutput::Mesh: {
            auto [row, col] = topo.pu_pos[pu];
            auto next_pos = meshNeighbor(row, col, output.dir);
            int next_pu = topo.pos_to_pu.at(next_pos);
            int opp_dir = (output.dir + 2) % 4;
            auto &next_router = router(next_pu);
            auto ingress_it = next_router.dir_to_port.find(opp_dir);
            if (ingress_it != next_router.dir_to_port.end() &&
                next_router.canAccept(ingress_it->second, chosen_vc)) {
              transfers.push_back(MeshTransfer{
                .dst_pu = next_pu,
                .ingress_port = ingress_it->second,
                .flit = flit,
              });
              fired = true;
            }
            break;
          }
          case RouterOutput::Core:
              if (core(pu).canAcceptExtInput(flit)) {
              core(pu).driveExtInput(flit);
              fired = true;
            }
            break;
          case RouterOutput::MemIf:
            memif_props[output.mc_idx].push_back(MemIfProposal{
                .pu = pu,
                .output_idx = output_idx,
                .input_idx = chosen_input,
                .vc = chosen_vc,
                .cluster_input = output.mc_input,
                .flit = flit,
            });
            break;
          case RouterOutput::Periph:
            periph_if.accept(flit, cycle);
            fired = true;
            break;
        }

        if (!fired) continue;
        deferred_pops.push_back(DeferredPop{
          .pu = pu,
          .output_idx = output_idx,
          .input_idx = chosen_input,
          .vc = chosen_vc,
        });
      }
    }

    for (int mc = 0; mc < num_mc; ++mc) {
      auto &proposals = memif_props[mc];
      if (proposals.empty()) continue;

      auto &memif = memifs[mc];
      MemIfProposal *chosen = nullptr;
      for (int offset = 0; offset < memif.numIngress(); ++offset) {
        int cluster_input = (memif.ingress_rr + offset) % memif.numIngress();
        auto it = std::find_if(proposals.begin(), proposals.end(), [&](const MemIfProposal &proposal) {
          return proposal.cluster_input == cluster_input;
        });
        if (it == proposals.end()) continue;
        chosen = &*it;
        memif.ingress_rr = (cluster_input + 1) % memif.numIngress();
        break;
      }

      if (!chosen) continue;

      memif.accept(chosen->flit, cycle);
      deferred_pops.push_back(DeferredPop{
        .pu = chosen->pu,
        .output_idx = chosen->output_idx,
        .input_idx = chosen->input_idx,
        .vc = chosen->vc,
      });
    }

    for (const auto &pop : deferred_pops) {
      auto &r = router(pop.pu);
      r.input_vcs[pop.input_idx][pop.vc].pop_front();
      r.rr_next[pop.output_idx][pop.vc] =
          (pop.input_idx + 1) % static_cast<int>(r.input_vcs.size());
    }

    for (const auto &transfer : transfers) {
      router(transfer.dst_pu).enqueue(transfer.ingress_port, transfer.flit);
    }

    // Collect stats: hops, idle lanes, blocking
    stats.total_hops += transfers.size();
    stats.idle_lane_cycles += total_mesh_outputs - static_cast<int>(transfers.size());

    vector<vector<bool>> consumed(num_pu + 1);
    for (int pu = 1; pu <= num_pu; ++pu)
      consumed[pu].assign(router(pu).outputs.size(), false);
    for (const auto &pop : deferred_pops)
      consumed[pop.pu][pop.output_idx] = true;

    for (int pu = 1; pu <= num_pu; ++pu) {
      auto &r = router(pu);
      for (int oi = 0; oi < static_cast<int>(r.outputs.size()); ++oi) {
        int d = demand[pu][oi];
        int c = consumed[pu][oi] ? 1 : 0;
        if (d > 0 && c == 0 && r.outputs[oi].kind == RouterOutput::Mesh)
          stats.blocked_vc_depth++;
        if (d > c)
          stats.blocked_structural += d - c;
      }
    }
  }

  void updateExtOutReady() {
    for (int pu = 1; pu <= num_pu; ++pu) {
      auto &c = core(pu);
      auto out = c.pendingOut();
      if (!out) {
        c.core->ext_out_ready = 1;
        continue;
      }
      c.core->ext_out_ready = router(pu).canInject(*out) ? 1 : 0;
    }
  }

  void scheduleMcResp(uint16_t dst, int mc_idx, uint64_t at, const MemResponse &resp) {
    core_mem[dst].emplace(at, resp);
  }

  void stepPosedge(uint64_t cy) {
    cycle = cy;
    reset_cycle = cycle <= RESET_LENGTH;

    if (reset_cycle) {
      for (auto &c : cores) {
        c->core->reset = 1;
        c->core->mem_unicast_valid = 0;
        c->core->mem_broadcast_valid = 0;
        c->core->ext_in_valid = 0;
        c->core->ext_out_ready = 0;
        c->core->clock = 1;
        c->core->eval();
      }
      return;
    }

    for (auto &c : cores) c->retireInputs();

    // Apply deferred responses from previous cycle's mem() before advanceSlots/processResponseNetwork
    for (int mc = 0; mc < num_mc; ++mc) memifs[mc].applyDeferredResponses();
    periph_if.applyDeferredResponses(cycle);

    presentClusterResponses();
    deliverMemResponses();
    for (int mc = 0; mc < num_mc; ++mc) {
      memifs[mc].advanceSlots(cycle);
      memifs[mc].advanceScatter(cycle);
      memifs[mc].findPendingReq(cycle, *this);
    }
    periph_if.advanceSlots(cycle);
    periph_if.findPendingReq(cycle, *this);
    processResponseNetwork();

    routeRouters();

    for (auto &c : cores) {
      c->core->reset = 0;
      c->driveInputs();
      // Resolve the core's current ext.out payload before computing the ready path.
      c->core->ext_out_ready = 0;
      c->core->eval();
    }

    updateExtOutReady();

    for (auto &c : cores) {
      c->core->eval();
    }

    pending_injected.clear();
    for (int pu = 1; pu <= num_pu; ++pu) {
      auto out = core(pu).currentOut();
      if (out) {
        if (LOG) {
          cerr << "[Soft] cycle=" << cycle << " pu=" << pu
               << " out dst=0x" << hex << out->dst << " tag=0x" << out->tag
               << " data=0x" << out->data << dec << endl;
        }
        pending_injected.emplace_back(pu, *out);
      }
    }

    for (auto &c : cores) {
      c->posedge();
    }
    for (const auto &[pu, flit] : pending_injected) router(pu).enqueueInject(flit);
    commitClusterBuffers();

    // Stat: injections
    stats.total_injections += pending_injected.size();

    // Stat: idle cores (no inject and no ext_input delivered this cycle)
    {
      int active_cores = 0;
      for (int pu = 1; pu <= num_pu; ++pu) {
        bool injected = false;
        for (const auto &[ipu, _] : pending_injected) {
          if (ipu == pu) { injected = true; break; }
        }
        bool received = core(pu).ext_input.has_value();
        if (injected || received) active_cores++;
      }
      stats.idle_core_cycles += num_pu - active_cores;
    }

    // Stat: inflight messages (flits in router VC buffers)
    {
      uint64_t inflight_msg = 0;
      for (int pu = 1; pu <= num_pu; ++pu) {
        auto &r = router(pu);
        for (auto &vcs : r.input_vcs)
          for (auto &q : vcs) inflight_msg += q.size();
      }
      stats.inflight_messages_sum += inflight_msg;
      stats.cur_inflight_messages = inflight_msg;
    }

    // Stat: inflight DRAM requests (allocated memif slots)
    {
      uint64_t inflight_dram = 0;
      for (int mc = 0; mc < num_mc; ++mc)
        for (auto &s : memifs[mc].inflight)
          if (s.allocated) inflight_dram++;
      stats.inflight_dram_sum += inflight_dram;
      stats.cur_inflight_dram = inflight_dram;
    }

    // Stat: inflight memory responses (ring queues + completed slots + cluster buffers)
    {
      uint64_t inflight_resp = 0;
      for (int mc = 0; mc < num_mc; ++mc) {
        inflight_resp += memifs[mc].ring_queue.size();
        for (auto &s : memifs[mc].inflight)
          if (s.allocated && s.completed) inflight_resp++;
      }
      inflight_resp += periph_if.ring_queue.size();
      for (auto &s : periph_if.inflight)
        if (s.allocated && s.completed) inflight_resp++;
      for (auto &buf : cluster_buffers)
        if (buf.valid || buf.pending_valid) inflight_resp++;
      stats.inflight_resp_sum += inflight_resp;
      stats.cur_inflight_resp = inflight_resp;
    }

    // Periodic stats
    if (cycle % STAT_PERIOD == 0) {
      PeriodicStat delta = stats - last_periodic;
      cerr << "[Soft] Stats @ cycle " << cycle << ":\n";
      delta.print(STAT_PERIOD);
      last_periodic = stats;
    }
  }

  void stepNegedge() {
    for (auto &c : cores) c->negedge();
  }

  // mem() is called once per cycle by the frontend after step().
  // bus_in[0] / bus_out[0] = peripheral, bus_in[1..numMC] / bus_out[1..numMC] = MCs
  void mem(const MemBusIn *bus_in, MemBusOut *bus_out) {
    // --- Defer responses (applied at start of next step()) ---

    // Peripheral (idx 0)
    if (bus_in[0].resp) {
      MemResponse resp;
      resp.id = bus_in[0].resp->id;
      memcpy(resp.data, bus_in[0].resp->data, sizeof(resp.data));
      periph_if.deferResponse(resp);
    }

    // MCs (idx 1..numMC)
    for (int mc = 0; mc < num_mc; ++mc) {
      if (bus_in[mc + 1].resp) {
        MemResponse resp;
        resp.id = bus_in[mc + 1].resp->id;
        memcpy(resp.data, bus_in[mc + 1].resp->data, sizeof(resp.data));
        memifs[mc].deferResponse(resp);
      }
    }

    // --- Report pending requests ---

    // Peripheral (idx 0)
    if (periph_if.pending_issue_slot >= 0 && bus_in[0].reqAccepting) {
      auto &entry = periph_if.inflight[periph_if.pending_issue_slot];
      GlobalMemReq req{};
      req.id = static_cast<uint16_t>(periph_if.pending_issue_slot);
      req.addr = entry.addr;
      req.size = entry.size;
      req.write = entry.is_write;
      memset(req.wdata, 0, sizeof(req.wdata));
      if (entry.is_write) {
        uint32_t word_idx = (entry.addr & 31) >> 2;
        memcpy(req.wdata + word_idx * 4, &entry.wdata, 4);
      }
      bus_out[0].req = req;
      periph_if.acceptIssue(cycle);
    } else {
      bus_out[0].req = std::nullopt;
    }

    // MCs (idx 1..numMC)
    for (int mc = 0; mc < num_mc; ++mc) {
      int idx = mc + 1;
      if (memifs[mc].pending_issue_slot >= 0 && bus_in[idx].reqAccepting) {
        if (memifs[mc].pending_issue_slot == SoftMemIf::kMaxInflight) {
          // Scatter read: issue via the same bus as normal reads
          GlobalMemReq req{};
          req.id = static_cast<uint16_t>(SoftMemIf::kMaxInflight);
          req.addr = memifs[mc].scat_addr;
          req.size = 0;  // full 32-byte line
          req.write = false;
          memset(req.wdata, 0, sizeof(req.wdata));
          bus_out[idx].req = req;
        } else {
          auto &entry = memifs[mc].inflight[memifs[mc].pending_issue_slot];
          GlobalMemReq req{};
          req.id = static_cast<uint16_t>(memifs[mc].pending_issue_slot);
          req.addr = entry.addr;
          req.size = entry.size;
          req.write = entry.is_write;
          memset(req.wdata, 0, sizeof(req.wdata));
          if (entry.is_write) {
            uint32_t word_idx = (entry.addr & 31) >> 2;
            memcpy(req.wdata + word_idx * 4, &entry.wdata, 4);
          }
          bus_out[idx].req = req;
        }
        memifs[mc].acceptIssue(cycle);
        stats.total_mem_requests++;
      } else {
        bus_out[idx].req = std::nullopt;
      }
    }
  }
};

void SoftMemIf::accept(const Flit &flit, uint64_t cycle) {
  if (flit.tag == 0xF00 || flit.tag == 0xF01) {
    // Single-flit memory request:
    //   data[0] = address (controller-local)
    //   data[1] = 0(14) ## size(2) ## id(16)
    //   data[2] = wdata (stores only)
    int slot = findFreeSlot();
    if (slot < 0) throw runtime_error("SoftMemIf: no free inflight slot");

    auto &entry = inflight[slot];
    entry = MemInflightSlot{};
    entry.allocated = true;
    entry.src = flit.src;
    entry.addr = flit.data[0];
    entry.resp_id = static_cast<uint16_t>(flit.data[1] & 0xFFFFu);
    entry.size = static_cast<uint16_t>((flit.data[1] >> 16) & 0x3u);
    entry.is_write = flit.tag == 0xF01;
    entry.wdata = flit.data[2];
    entry.remaining_flits = 0;
    ready_events.emplace(cycle + 1, slot);
  } else if ((flit.tag & 0xFF) == 0x10) {
    // Single-flit scatter request (matches RTL BulkPending.initFromFlit):
    //   data[0] = base address (controller-local)
    //   data[1] = (length_in_beats << 16) | response_tag
    //   data[2], data[3] = carried data (passed through to handlers)
    scat_addr = flit.data[0];
    uint32_t length_in_beats = (flit.data[1] >> 16) & 0xFFFF;
    scat_end = scat_addr + (length_in_beats << 5);
    scat_resp_tag = static_cast<uint16_t>(flit.data[1] & 0xFFFF);
    scat_extras[0] = flit.data[2];
    scat_extras[1] = flit.data[3];
    scat_state = ScatIssue;
  }
}

void SoftMemIf::advanceSlots(uint64_t cycle) {
  auto free_range = free_events.equal_range(cycle);
  for (auto it = free_range.first; it != free_range.second; ++it) clearSlot(it->second);
  free_events.erase(free_range.first, free_range.second);

  auto ready_range = ready_events.equal_range(cycle);
  for (auto it = ready_range.first; it != ready_range.second; ++it) {
    if (inflight[it->second].allocated) inflight[it->second].ready = true;
  }
  ready_events.erase(ready_range.first, ready_range.second);

  auto complete_range = completion_events.equal_range(cycle);
  for (auto it = complete_range.first; it != complete_range.second; ++it) {
    int slot = it->second.first;
    if (!inflight[slot].allocated) continue;
    inflight[slot].completed = true;
    memcpy(inflight[slot].data, it->second.second.data, sizeof(inflight[slot].data));
  }
  completion_events.erase(complete_range.first, complete_range.second);
}

// Advance scatter broadcast FSM. Called each cycle from stepPosedge.
// In ScatBcast state: delivers data to one cluster per cycle, then advances
// to next 32-byte beat (ScatIssue) or finishes (ScatIdle).
void SoftMemIf::advanceScatter(uint64_t cycle) {
  if (scat_state != ScatBcast) return;

  int cluster_idx = zone_clusters[scat_cluster];
  // Build BcastEntry from raw DRAM data (4 entries x 8 bytes per 32-byte beat)
  BcastEntry entry;
  entry.tag = scat_resp_tag;
  entry.carried[0] = scat_extras[0];
  entry.carried[1] = scat_extras[1];
  for (int i = 0; i < 4; i++) {
    uint32_t word0 = scat_data[i * 2];      // (pu_id << 16) | sub_idx
    uint32_t word1 = scat_data[i * 2 + 1];  // data (e.g. weight)
    entry.line_pu[i] = static_cast<uint16_t>((word0 >> 16) & 0xFFFF);
    entry.line_idx[i] = static_cast<uint16_t>(word0 & 0xFFFF);
    entry.line_data[i] = word1;
  }
  // Deliver to all PUs in this cluster
  for (int pu = cluster_idx * 16 + 1; pu <= cluster_idx * 16 + 16; ++pu) {
    sim_ptr->core(pu).bcast_inbox.push_back(entry);
  }

  if (scat_cluster == static_cast<int>(zone_clusters.size()) - 1) {
    // All clusters done for this beat. Advance to next beat or finish.
    scat_cluster = 0;
    scat_addr += 32;
    if (scat_addr >= scat_end) {
      scat_state = ScatIdle;
    } else {
      scat_state = ScatIssue;
    }
  } else {
    scat_cluster++;
  }
}

void SoftMemIf::findPendingReq(uint64_t cycle, SoftSystemSim &sim) {
  pending_issue_slot = findIssueSlot();
  // RTL: normal has priority; scatter only issues when no normal slot ready
  if (pending_issue_slot < 0 && scat_state == ScatIssue) {
    pending_issue_slot = kMaxInflight;  // sentinel: scatter read
  }
  (void)cycle;
  (void)sim;
}

void SoftMemIf::acceptIssue(uint64_t cycle) {
  if (pending_issue_slot < 0) return;
  if (pending_issue_slot == kMaxInflight) {
    // Scatter read accepted by frontend → transition to ScatWait
    traceMemReq(cycle, mc_idx, false, scat_addr, SCATTER_RESP_ID);
    scat_state = ScatWait;
    pending_issue_slot = -1;
    return;
  }
  auto &entry = inflight[pending_issue_slot];
  traceMemReq(cycle, mc_idx, entry.is_write, entry.addr, static_cast<uint16_t>(pending_issue_slot));
  entry.issued = true;
  // Writes complete internally (1-cycle latency), matching RTL MemIf behavior.
  // The external write response from the frontend will be ignored (slot already completed).
  if (entry.is_write) {
    MemResponse resp;
    resp.id = entry.resp_id;
    memset(resp.data, 0, sizeof(resp.data));
    completion_events.emplace(cycle + 1, std::make_pair(pending_issue_slot, resp));
  }
  issue_rr = (pending_issue_slot + 1) % kMaxInflight;
  pending_issue_slot = -1;
}

void SoftMemIf::deferResponse(const MemResponse &resp) {
  deferred_responses.push_back(resp);
}

void SoftMemIf::applyDeferredResponses() {
  for (auto &resp : deferred_responses) {
    int id = static_cast<int>(resp.id);
    if (id == kMaxInflight) {
      // Scatter response: capture data and start broadcast
      if (scat_state == ScatWait) {
        memcpy(scat_data, resp.data, sizeof(scat_data));
        scat_cluster = 0;
        scat_state = ScatBcast;
      }
      continue;
    }
    // Normal response: resp.id is the slot index
    if (id < 0 || id >= kMaxInflight) continue;
    auto &entry = inflight[id];
    if (entry.allocated && entry.issued && !entry.completed) {
      entry.completed = true;
      memcpy(entry.data, resp.data, sizeof(entry.data));
    }
  }
  deferred_responses.clear();
}

void SoftPeriphIf::advanceSlots(uint64_t cycle) {
  auto free_range = free_events.equal_range(cycle);
  for (auto it = free_range.first; it != free_range.second; ++it) clearSlot(it->second);
  free_events.erase(free_range.first, free_range.second);

  auto ready_range = ready_events.equal_range(cycle);
  for (auto it = ready_range.first; it != ready_range.second; ++it) {
    if (inflight[it->second].allocated) inflight[it->second].ready = true;
  }
  ready_events.erase(ready_range.first, ready_range.second);

  auto complete_range = completion_events.equal_range(cycle);
  for (auto it = complete_range.first; it != complete_range.second; ++it) {
    int slot = it->second.first;
    if (!inflight[slot].allocated) continue;
    inflight[slot].completed = true;
    memcpy(inflight[slot].data, it->second.second.data, sizeof(inflight[slot].data));
  }
  completion_events.erase(complete_range.first, complete_range.second);
}

void SoftPeriphIf::findPendingReq(uint64_t cycle, SoftSystemSim &sim) {
  pending_issue_slot = findIssueSlot();
  (void)cycle;
  (void)sim;
}

void SoftPeriphIf::acceptIssue(uint64_t cycle) {
  if (pending_issue_slot < 0) return;
  auto &entry = inflight[pending_issue_slot];
  if (current_periph_trace) {
    current_periph_trace->push_back(PeriphTraceEvent{
      .cycle = cycle,
      .id = static_cast<uint16_t>(pending_issue_slot),
      .addr = entry.addr,
      .is_write = entry.is_write,
      .wdata = entry.is_write ? entry.wdata : 0,
    });
  }
  entry.issued = true;
  // Writes complete internally (1-cycle latency); only the side-effect matters.
  // The response data is all-zeros anyway. Frontend handles the actual write.
  if (entry.is_write) {
    MemResponse resp;
    resp.id = entry.resp_id;
    memset(resp.data, 0, sizeof(resp.data));
    completion_events.emplace(cycle + 1, std::make_pair(pending_issue_slot, resp));
  }
  issue_rr = (pending_issue_slot + 1) % kMaxInflight;
  pending_issue_slot = -1;
}

void SoftPeriphIf::deferResponse(const MemResponse &resp) {
  deferred_responses.push_back(resp);
}

void SoftPeriphIf::applyDeferredResponses(uint64_t cycle) {
  for (auto &resp : deferred_responses) {
    // Stage read responses through completion_events with +2 cycle delay,
    // adding the same 2-cycle shift as writes to match the hard backend's
    // frontend round-trip latency.
    int slot = static_cast<int>(resp.id);
    if (slot < 0 || slot >= kMaxInflight) continue;
    auto &entry = inflight[slot];
    if (entry.allocated && entry.issued && !entry.completed) {
      completion_events.emplace(cycle + 2, std::make_pair(slot, resp));
    }
  }
  deferred_responses.clear();
}

void SoftPeriphIf::accept(const Flit &flit, uint64_t cycle) {
  // Single-flit peripheral request:
  //   data[0] = address (peripheral-local, relative to 0x40000000)
  //   data[1] = 0(14) ## size(2) ## id(16)
  //   data[2] = wdata (stores only)
  int slot = findFreeSlot();
  if (slot < 0) throw runtime_error("SoftPeriphIf: no free inflight slot");

  auto &entry = inflight[slot];
  entry = PeriphInflightSlot{};
  entry.allocated = true;
  entry.src = flit.src;
  entry.addr = flit.data[0];
  entry.resp_id = static_cast<uint16_t>(flit.data[1] & 0xFFFFu);
  entry.size = static_cast<uint16_t>((flit.data[1] >> 16) & 0x3u);
  entry.is_write = flit.tag == 0xF01;
  entry.wdata = flit.data[2];
  entry.remaining_flits = 0;
  ready_events.emplace(cycle + 1, slot);
}

struct SoftSystemModel::Impl {
  SoftSystemSim sim;
  std::vector<MemTraceEvent> last_mem_trace;
  std::vector<PeriphTraceEvent> last_periph_trace;

  Impl(int pu, int mc) : sim(pu, mc) {}

  void stepPosedge(uint64_t cycle) {
    last_mem_trace.clear();
    last_periph_trace.clear();
    current_mem_trace = &last_mem_trace;
    current_periph_trace = &last_periph_trace;
    sim.stepPosedge(cycle);
  }

  void stepNegedge() {
    sim.stepNegedge();
    current_mem_trace = nullptr;
    current_periph_trace = nullptr;
  }
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

SoftSystemModel::SoftSystemModel(int pu, int mc) : impl_(std::make_unique<Impl>(pu, mc)) {}
SoftSystemModel::~SoftSystemModel() = default;
SoftSystemModel::SoftSystemModel(SoftSystemModel &&) noexcept = default;
SoftSystemModel &SoftSystemModel::operator=(SoftSystemModel &&) noexcept = default;

void SoftSystemModel::attachTrace(VerilatedFstC *trace_file, int depth) {
  TRACE = trace_file != nullptr;
  tracer = trace_file;
  if (!trace_file) return;
  for (auto &core : impl_->sim.cores) core->core->trace(trace_file, depth);
}

void SoftSystemModel::step(uint64_t cycle) {
  impl_->stepPosedge(cycle);
  if (TRACE && tracer) tracer->dump(cycle * 2);
  impl_->stepNegedge();
  if (TRACE && tracer) tracer->dump(cycle * 2 + 1);
}

SystemConfig SoftSystemModel::config() const {
  SystemConfig cfg;
  cfg.numPU = impl_->sim.num_pu;
  cfg.numMC = impl_->sim.num_mc;
  for (int i = 0; i < SYSTEM_NUM_MEM_CTRL; ++i)
    cfg.core.mcSizes.push_back(SYSTEM_MC_SIZES[i]);
  return cfg;
}

void SoftSystemModel::mem(const MemBusIn *bus_in, MemBusOut *bus_out) {
  impl_->sim.mem(bus_in, bus_out);
}

bool SoftSystemModel::printStats(uint64_t cycles, bool final_print) {
  if (final_print) {
    cerr << "[Soft] Final stats (over " << cycles << " cycles):\n";
    impl_->sim.stats.print(cycles);
  }
  return true;
}