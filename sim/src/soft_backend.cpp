/**
 * Software NoC system simulator.
 *
 * Method definitions and topology construction for SoftSystemBackend.
 * The class declaration lives in soft_backend.h.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "soft_backend.h"
#include "soft_components.h"

using namespace std;

// Wall-clock accumulator (ns) for time spent inside soft_rtl Verilator
// eval() calls, summed across all PUs. Thread-local because Verilated
// contexts are per-thread; we use this to attribute soft-model runtime
// between "verilator" (CPU spent inside soft_rtl::eval) and "uncore"
// (everything else inside peek/stage/step). Incremented at batch
// granularity by the per-cycle eval loops in stage()/step();
// snapshotted by PhaseTimer.
static thread_local uint64_t g_soft_verilator_ns = 0;

// ===========================================================================
// Topology
// ===========================================================================

static bool isPow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

static pair<int, int> gridDims(int num_pu) {
  int s = static_cast<int>(std::sqrt(num_pu));
  if (s * s == num_pu) return {s, s};
  int h = static_cast<int>(std::sqrt(num_pu / 2));
  if (2 * h * h != num_pu)
    throw runtime_error("Cannot form 1:1 or 2:1 grid for numPU=" + to_string(num_pu));
  return {2 * h, h};
}

// Hilbert curve d -> (x, y). Backward C base case (first step east).
// Mirrors koneko.Topology.hilbertD2xy. We label the returned axes
// (x → row, y → col) for parity with the RTL comment.
static pair<int, int> hilbertD2xy(int n, int d) {
  int rx = 0, ry = 0, s = 1, t = d, x = 0, y = 0;
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
  return {x, y}; // x → row, y → col (RTL convention)
}

static vector<pair<int, int>> hilbert(int w, int h) {
  if (!isPow2(w) || w < 2 || !(w == h || w == 2 * h))
    throw runtime_error("Invalid Hilbert dimensions");
  vector<pair<int, int>> square;
  square.reserve(w * w);
  for (int d = 0; d < w * w; ++d) square.push_back(hilbertD2xy(w, d));
  square.resize(w * h);
  return square;
}

// Manhattan distance between integer points.
static int meshDist(int r1, int c1, int r2, int c2) {
  return std::abs(r2 - r1) + std::abs(c2 - c1);
}

Topology::Topology(size_t num_pu, size_t num_mc)
  : pu_to_coord(num_pu),
    coord_to_pu(),
    memif_to_pu(1 + num_mc, 0),
    pu_mesh_dirs(num_pu),
    pu_memif(num_pu)
{
  auto [grid_w, grid_h] = gridDims(static_cast<int>(num_pu));
  w = static_cast<uint16_t>(grid_w);
  h = static_cast<uint16_t>(grid_h);
  coord_to_pu.assign(static_cast<size_t>(grid_w) * grid_h, {});

  // Generate Hilbert curve. Each curve entry is (x=row, y=col) per RTL
  // convention; store soft pairs as (col, row).
  auto curve = hilbert(grid_w, grid_h);
  if (static_cast<int>(curve.size()) != static_cast<int>(num_pu))
    throw runtime_error("Hilbert curve length mismatch");

  for (uint16_t id = 1; id <= num_pu; ++id) {
    auto [row, col] = curve[id - 1];
    pu_to_coord[id] = { static_cast<uint16_t>(col), static_cast<uint16_t>(row) };
    coord_to_pu[static_cast<size_t>(col) * grid_h + row].push_back(id);
  }

  // Per-PU active mesh directions: N if row > 0, E if col < w-1,
  // S if row < h-1, W if col > 0. Direction order matches RTL link
  // ordering used by the router-port-index mapping.
  for (uint16_t id = 1; id <= num_pu; ++id) {
    auto [col, row] = pu_to_coord[id];
    auto &dirs = pu_mesh_dirs[id];
    if (row > 0)            dirs.push_back(0); // NORTH
    if (col + 1 < grid_w)   dirs.push_back(1); // EAST
    if (row + 1 < grid_h)   dirs.push_back(2); // SOUTH
    if (col > 0)            dirs.push_back(3); // WEST
  }

  // MC zones: every 16 consecutive Hilbert IDs form a cluster; clusters
  // are split into numMC contiguous zones.
  if (num_pu % CLUSTER_SIZE != 0)
    throw runtime_error("num_pu must be a multiple of cluster size (16)");
  size_t num_clusters = num_pu / CLUSTER_SIZE;
  if (num_clusters % num_mc != 0)
    throw runtime_error("num_clusters must be divisible by num_mc");
  size_t clusters_per_mc = num_clusters / num_mc;

  // Connection PU per cluster: the one closest to the grid center.
  // For soft's single-PU-per-MC simplification we further pick, across
  // all clusters in the zone, the connection PU closest to the grid
  // center.
  auto best_conn_in_cluster = [&](size_t cluster_idx) -> uint16_t {
    uint16_t best_pu = static_cast<uint16_t>(cluster_idx * CLUSTER_SIZE + 1);
    double best_dist = 1e30;
    for (size_t pu = cluster_idx * CLUSTER_SIZE + 1;
         pu <= cluster_idx * CLUSTER_SIZE + CLUSTER_SIZE; ++pu) {
      auto [col, row] = pu_to_coord[static_cast<uint16_t>(pu)];
      double d = std::abs(row - grid_h / 2.0) + std::abs(col - grid_w / 2.0);
      if (d < best_dist) {
        best_dist = d;
        best_pu = static_cast<uint16_t>(pu);
      }
    }
    return best_pu;
  };

  // memif_to_pu[0] = peripheral connection = PU 1 (matches old impl).
  memif_to_pu[0] = 1;

  for (size_t mc = 0; mc < num_mc; ++mc) {
    uint16_t best_pu = 0;
    double best_dist = 1e30;
    for (size_t ci = mc * clusters_per_mc; ci < (mc + 1) * clusters_per_mc; ++ci) {
      uint16_t cpu = best_conn_in_cluster(ci);
      auto [col, row] = pu_to_coord[cpu];
      double d = std::abs(row - grid_h / 2.0) + std::abs(col - grid_w / 2.0);
      if (d < best_dist) { best_dist = d; best_pu = cpu; }
    }
    memif_to_pu[1 + mc] = best_pu;
    pu_memif[best_pu] = static_cast<uint16_t>(1 + mc);
  }

  // The peripheral memif (memif index 0) lives at memif_to_pu[0] = PU 1.
  // pu_memif[1] takes precedence for the peripheral so the same PU can
  // host both periph and an MC eject if it happens to coincide; the
  // current simple selection puts periph at PU 1 and MC conns near the
  // center, so coincidence is unlikely for non-trivial sizes.
  if (!pu_memif[1].has_value()) pu_memif[1] = 0;
}

// ===========================================================================
// SoftSystemBackend
// ===========================================================================

SoftSystemBackend::Stat SoftSystemBackend::Stat::operator-(const Stat &o) const {
  Stat r;
  r.total_hops          = total_hops          - o.total_hops;
  r.total_injections    = total_injections    - o.total_injections;
  r.total_mem_requests  = total_mem_requests  - o.total_mem_requests;
  r.idle_lane_cycles    = idle_lane_cycles    - o.idle_lane_cycles;
  r.idle_core_cycles    = idle_core_cycles    - o.idle_core_cycles;
  r.blocked_structural  = blocked_structural  - o.blocked_structural;
  r.inflight_messages_sum = inflight_messages_sum - o.inflight_messages_sum;
  r.inflight_dram_sum   = inflight_dram_sum   - o.inflight_dram_sum;
  r.inflight_resp_sum   = inflight_resp_sum   - o.inflight_resp_sum;
  r.cur_inflight_messages = cur_inflight_messages;
  r.cur_inflight_dram   = cur_inflight_dram;
  r.cur_inflight_resp   = cur_inflight_resp;
  return r;
}

void SoftSystemBackend::Stat::print(uint64_t cycles) const {
  auto avg = [cycles](uint64_t v) -> double {
    return cycles > 0 ? static_cast<double>(v) / cycles : 0.0;
  };
  std::cerr << std::fixed << std::setprecision(2);
  std::cerr << "  hops:           " << total_hops
            << " (avg " << avg(total_hops) << "/cyc)\n";
  std::cerr << "  injections:     " << total_injections
            << " (avg " << avg(total_injections) << "/cyc)\n";
  std::cerr << "  mem_requests:   " << total_mem_requests
            << " (avg " << avg(total_mem_requests) << "/cyc)\n";
  std::cerr << "  idle_lanes:     " << idle_lane_cycles
            << " (avg " << avg(idle_lane_cycles) << "/cyc)\n";
  std::cerr << "  idle_cores:     " << idle_core_cycles
            << " (avg " << avg(idle_core_cycles) << "/cyc)\n";
  std::cerr << "  blocked_hazard: " << blocked_structural
            << " (avg " << avg(blocked_structural) << "/cyc)\n";
  std::cerr << "  inflight_msg:   " << inflight_messages_sum
            << " (avg " << avg(inflight_messages_sum)
            << "/cyc, cur " << cur_inflight_messages << ")\n";
  std::cerr << "  inflight_dram:  " << inflight_dram_sum
            << " (avg " << avg(inflight_dram_sum)
            << "/cyc, cur " << cur_inflight_dram << ")\n";
  std::cerr << "  inflight_resp:  " << inflight_resp_sum
            << " (avg " << avg(inflight_resp_sum)
            << "/cyc, cur " << cur_inflight_resp << ")\n";
}

SoftSystemBackend::PhaseTimer::PhaseTimer(SoftSystemBackend &o)
  : owner(o),
    t0(std::chrono::high_resolution_clock::now()),
    v_at_entry(g_soft_verilator_ns) {}

SoftSystemBackend::PhaseTimer::~PhaseTimer() {
  auto t1 = std::chrono::high_resolution_clock::now();
  uint64_t total =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  uint64_t v = g_soft_verilator_ns - v_at_entry;
  owner.verilator_ns_ += v;
  owner.uncore_ns_ += (total > v ? total - v : 0);
}

SoftSystemBackend::WrappedRouter SoftSystemBackend::buildRouter(uint16_t pu) const {
  LinkStatus links;
  Topology::DirectionToLinkIdx dir_to_link{};

  const auto &my_dirs = topo.pu_mesh_dirs[pu];
  auto [col, row] = topo.pu_to_coord[pu];
  for (size_t slot = 0; slot < my_dirs.size(); ++slot) {
    uint8_t dir = my_dirs[slot];
    // Neighbor coord under direction `dir` (col, row).
    int ncol = col, nrow = row;
    switch (dir) {
      case 0: nrow -= 1; break; // NORTH
      case 1: ncol += 1; break; // EAST
      case 2: nrow += 1; break; // SOUTH
      case 3: ncol -= 1; break; // WEST
    }
    const auto &cell = topo.coord_to_pu[static_cast<size_t>(ncol) * topo.h + nrow];
    if (cell.empty())
      throw std::logic_error("buildRouter: neighbor cell is empty");
    uint16_t npu = cell.front();
    // Locate `opposite(dir)` in the neighbor's direction list to find the
    // remote forward-buffer slot index.
    uint8_t opp = static_cast<uint8_t>((dir + 2) % 4);
    const auto &n_dirs = topo.pu_mesh_dirs[npu];
    auto it = std::find(n_dirs.begin(), n_dirs.end(), opp);
    if (it == n_dirs.end())
      throw std::logic_error("buildRouter: neighbor lacks opposite link");
    uint8_t remote_port = static_cast<uint8_t>(std::distance(n_dirs.begin(), it));
    links.forwards[slot] = std::make_pair(npu, remote_port);
    dir_to_link[dir] = slot;
  }

  links.memif = topo.pu_memif[pu];

  size_t num_in  = 1 + my_dirs.size();                       // core_inject + fwds
  size_t num_out = 1 + my_dirs.size() + (links.memif ? 1 : 0); // core_eject + fwds + memif

  // memif output-port index (passed to routing table) = num_out - 1 when present.
  std::optional<size_t> memif_port =
      links.memif ? std::optional<size_t>(num_out - 1) : std::nullopt;
  auto tbl = topo.routingTableFor(pu, dir_to_link, memif_port);
  return WrappedRouter(std::move(links), std::move(tbl), num_in, num_out);
}

SoftSystemBackend::SoftSystemBackend(SystemConfig cfg_in)
  : cfg(std::move(cfg_in)),
    releaseResetAfter(0),
    topo(cfg.numPU, cfg.numMC),
    periph(0, 16, {}),
    drams(),
    cores(0),
    ring(1 + cfg.numMC),
    memif_eject(cfg.numMC + 1),
    memif_ext(cfg.numMC + 1),
    memif_ring_accept(cfg.numMC + 1, false),
    core_mem(cfg.numPU),
    core_mem_accept(cfg.numPU),
    routers(cfg.numPU,
            [this](size_t i) { return buildRouter(static_cast<uint16_t>(i)); })
{
  // Initialize per-MC DRAMIfs. PU range is contiguous in PU-id order.
  const size_t PU_PER_MC = cfg.numPU / cfg.numMC;
  drams.reserve(cfg.numMC);
  for (size_t m = 0; m < cfg.numMC; ++m) {
    drams.emplace_back(m + 1, 64, 16, 2,
                       1 + m * PU_PER_MC,
                       1 + (m + 1) * PU_PER_MC);
  }

  char rtl_name_buffer[64];
  cores.reserve(cfg.numPU);
  for (size_t i = 1; i <= cfg.numPU; ++i) {
    sprintf(rtl_name_buffer, "soft_pu_%lu", i);
    cores.emplace_back(std::make_unique<soft_rtl>(rtl_name_buffer));
    cores[i]->reset = true;
    cores[i]->eval();
  }
}

void SoftSystemBackend::attachTrace(VerilatedFstC *trace_file, int depth) {
  for (size_t i = 1; i <= cfg.numPU; ++i)
    cores[i]->trace(trace_file, depth);
}

SystemConfig SoftSystemBackend::config() const { return cfg; }

__attribute__((always_inline))
static std::optional<Flit> peekCoreInject(const soft_rtl &core, uint16_t puIdx) {
  if (!core.ext_out_valid) return std::nullopt;
  return Flit {
    .src = puIdx,
    .dst = core.ext_out_bits_dst,
    .tag = core.ext_out_bits_tag,
    .data = {
      core.ext_out_bits_data_0,
      core.ext_out_bits_data_1,
      core.ext_out_bits_data_2,
      core.ext_out_bits_data_3,
    }
  };
}

__attribute__((always_inline))
static void presentCoreEject(soft_rtl &core, uint16_t puIdx, const std::optional<Flit> &flit) {
  core.ext_in_valid = flit.has_value();
  if (flit) {
    if (flit->dst != puIdx && !(flit->src == puIdx && flit->dst == 0))
      throw std::invalid_argument("presentCoreEject: flit presented to wrong PU");
    core.ext_in_bits_tag = flit->tag;
    core.ext_in_bits_data_0 = flit->data[0];
    core.ext_in_bits_data_1 = flit->data[1];
    core.ext_in_bits_data_2 = flit->data[2];
    core.ext_in_bits_data_3 = flit->data[3];
  }
}

__attribute__((always_inline))
static void presentCoreMem(soft_rtl &core, const soft_mem::DRAMIf::PUResp &resp) {
  core.mem_unicast_valid = resp.unicast.has_value();
  if (resp.unicast.has_value()) {
    core.mem_unicast_bits_id = resp.unicast->first;
    // mem_unicast_bits_data is VlWide<8> (32 bytes); MemLine is
    // std::array<uint8_t,32>. Same size; raw copy preserves byte order.
    static_assert(sizeof(core.mem_unicast_bits_data) == sizeof(resp.unicast->second));
    std::memcpy(&core.mem_unicast_bits_data, resp.unicast->second.data(),
                sizeof(resp.unicast->second));
  }

  core.mem_broadcast_valid = resp.bcast.has_value();
  if (resp.bcast.has_value()) {
    core.mem_broadcast_bits_tag = resp.bcast->tag;
    core.mem_broadcast_bits_carried_0 = resp.bcast->carried[0];
    core.mem_broadcast_bits_carried_1 = resp.bcast->carried[1];

    // BcastLine.line is Vec(4, BcastBeat) where BcastBeat is
    // (data:32, idx:16, pu:16) — Chisel default Bundle places `data`
    // at the MSB. Vec(0) occupies the highest 64 bits of the 256-bit
    // line. The soft side stores the same 256 bits as uint32_t[8] in
    // little-endian word order, so:
    //   line(i).data       = soft.line[7 - 2*i]
    //   line(i).idx || pu  = soft.line[6 - 2*i]   (idx upper, pu lower)
    const auto &raw = resp.bcast->line;
    core.mem_broadcast_bits_line_0_data = raw[7];
    core.mem_broadcast_bits_line_0_idx  = (uint16_t)(raw[6] >> 16);
    core.mem_broadcast_bits_line_0_pu   = (uint16_t)(raw[6] & 0xFFFF);
    core.mem_broadcast_bits_line_1_data = raw[5];
    core.mem_broadcast_bits_line_1_idx  = (uint16_t)(raw[4] >> 16);
    core.mem_broadcast_bits_line_1_pu   = (uint16_t)(raw[4] & 0xFFFF);
    core.mem_broadcast_bits_line_2_data = raw[3];
    core.mem_broadcast_bits_line_2_idx  = (uint16_t)(raw[2] >> 16);
    core.mem_broadcast_bits_line_2_pu   = (uint16_t)(raw[2] & 0xFFFF);
    core.mem_broadcast_bits_line_3_data = raw[1];
    core.mem_broadcast_bits_line_3_idx  = (uint16_t)(raw[0] >> 16);
    core.mem_broadcast_bits_line_3_pu   = (uint16_t)(raw[0] & 0xFFFF);
  }
}

void SoftSystemBackend::peek(uint64_t cycle, std::vector<MemBusOut *> out) {
  PhaseTimer _t(*this);
  if (out.size() != cfg.numMC + 1)
    throw std::invalid_argument("peek: out size mismatch");
  if (cycle <= releaseResetAfter) {
    for (auto &o : out) o->req = std::nullopt;
    return;
  }

  out[0]->req = periph.peekMem();
  for (size_t i = 0; i < cfg.numMC; ++i)
    out[i + 1]->req = drams[i].peekMem();
}

void SoftSystemBackend::stage(uint64_t cycle, const std::vector<MemBusIn> &in) {
  PhaseTimer _t(*this);
  if (cycle <= releaseResetAfter) {
    for (const auto &i : in) {
      if (i.reqAccepting) throw std::runtime_error("Memory request accepted during reset");
      if (i.resp) throw std::runtime_error("Memory response presented during reset");
    }
  }

  memif_ext = in;

  // Populate presented data on all router-facing ready-valid interfaces.
  for (uint16_t i = 1; i <= cfg.numPU; ++i)
    routers[i].buf.core_inject.presenting = peekCoreInject(*cores[i], i);

  for (uint16_t i = 1; i <= cfg.numPU; ++i) {
    auto &router = routers[i];
    router.buf.core_eject.presenting = router.rt.peek(0);
    size_t o = 0;
    for (; o < 4 && router.links.forwards[o]; ++o)
      router.buf.forwards[o].presenting = router.rt.peek(o + 1);
    // o + 1 is now the memif port (if any).
    if (router.links.memif)
      memif_eject[*router.links.memif].presenting = router.rt.peek(o + 1);
  }

  const size_t PU_PER_MC = cfg.numPU / cfg.numMC;
  for (size_t m = 0; m < cfg.numMC; ++m) {
    auto buffer_span = core_mem.slice(m * PU_PER_MC + 1, (m + 1) * PU_PER_MC + 1);
    drams[m].peekPUs(buffer_span, ring.peekAt(m + 1)); // 0 is periph
  }

  // Generate accepting signals. Each router's egress is buffered on the
  // PEER's forward[port_idx]; we drive that buffer's `accepting` from
  // the local router's canEnq.
  for (uint16_t i = 1; i <= cfg.numPU; ++i) {
    auto &local = routers[i];
    local.buf.core_inject.accepting =
        local.buf.core_inject.presenting &&
        local.rt.canEnq(0, local.buf.core_inject.presenting->prio());
    size_t o = 0;
    for (; o < 4 && local.links.forwards[o]; ++o) {
      auto [from, port] = *local.links.forwards[o];
      auto &remote = routers[from];
      auto &remoteBuf = remote.buf.forwards[port];
      remoteBuf.accepting =
          remoteBuf.presenting &&
          local.rt.canEnq(o + 1, remoteBuf.presenting->prio());
    }
  }

  memif_ring_accept[0] =
      ring.peekAt(0) && periph.ringCanAccept(*ring.peekAt(0));
  memif_eject[0].accepting =
      memif_eject[0].presenting && periph.nocCanAccept(*memif_eject[0].presenting);
  for (size_t m = 0; m < cfg.numMC; ++m) {
    memif_ring_accept[m + 1] =
        ring.peekAt(m + 1) && drams[m].ringCanAccept(*ring.peekAt(m + 1));
    memif_eject[m + 1].accepting =
        memif_eject[m + 1].presenting &&
        drams[m].nocCanAccept(*memif_eject[m + 1].presenting);
  }

  // Drive PU inputs and settle on the negedge. Contract: accept signals
  // from the cores are read post-eval (after combinational settle).
  for (uint16_t i = 1; i <= cfg.numPU; ++i) {
    presentCoreEject(*cores[i], i, routers[i].buf.core_eject.presenting);
    presentCoreMem(*cores[i], core_mem[i]);
    cores[i]->ext_out_ready = routers[i].buf.core_inject.accepting;
    cores[i]->clock = false; // negedge
    auto v0 = std::chrono::high_resolution_clock::now();
    cores[i]->eval();
    auto v1 = std::chrono::high_resolution_clock::now();
    g_soft_verilator_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(v1 - v0).count();
    routers[i].buf.core_eject.accepting = cores[i]->ext_in_ready;
    core_mem_accept[i].unicast = cores[i]->mem_unicast_valid;
    core_mem_accept[i].broadcast =
        cores[i]->mem_broadcast_ready && cores[i]->mem_broadcast_valid;
  }
}

void SoftSystemBackend::step(uint64_t cycle) {
  PhaseTimer _t(*this);

  // Accumulate per-cycle stats BEFORE committing this cycle's
  // transfers. The accepting signals computed in stage() are still
  // valid; queue/inflight occupancies reflect the state entering
  // this cycle.
  accumulateStats();

  // Posedge eval for every core.
  for (size_t i = 1; i <= cfg.numPU; ++i) {
    cores[i]->clock = true;
    auto v0 = std::chrono::high_resolution_clock::now();
    cores[i]->eval();
    auto v1 = std::chrono::high_resolution_clock::now();
    g_soft_verilator_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(v1 - v0).count();

    if (cycle == releaseResetAfter) {
      cores[i]->reset = false;
      cores[i]->eval();
    }
  }

  // Step routers.
  for (auto &router : routers) {
    const auto &buffer = router.buf;
    const auto &links = router.links;
    const auto &all = this->routers;
    bool memif_accepting = false;
    if (links.memif.has_value())
      memif_accepting = memif_eject[*links.memif].accepting;
    router.rt.step(
      [&buffer, &links, &all](size_t i) -> std::optional<Flit> {
        if (i == 0) {
          if (buffer.core_inject.accepting) return buffer.core_inject.presenting;
        } else {
          assert(i < 5 && links.forwards[i - 1]);
          auto [from, port] = *links.forwards[i - 1];
          const auto &remote = all[from];
          const auto &remoteBuf = remote.buf.forwards[port];
          if (remoteBuf.accepting) return remoteBuf.presenting;
        }
        return std::nullopt;
      },
      [&buffer, &links, memif_accepting](size_t i) -> bool {
        if (i == 0) {
          return buffer.core_eject.accepting;
        } else if (i < 5 && links.forwards[i - 1]) {
          return buffer.forwards[i - 1].accepting;
        } else {
          assert(links.memif);
          return memif_accepting;
        }
      }
    );
  }

  // Step memifs.
  const size_t PU_PER_MC = cfg.numPU / cfg.numMC;
  bool injected;
  periph.step(
    soft_mem::RingIntf {
      .buffer = &ring[0],
      .ingress = ring.validAt(0),
      .eject = memif_ring_accept[0],
      // canInject: forward queue is empty AND no forward will push into
      // the outgoing queue this cycle.
      .canInject = !ring.validAt(1) &&
                   (!ring.validAt(0) || memif_eject[0].accepting),
      .injected = &injected
    },
    memif_ext[0],
    memif_eject[0].accepting ? &*memif_eject[0].presenting : nullptr
  );
  ring.updateValidAt(0, injected, memif_ring_accept[0]);

  for (size_t m = 0; m < cfg.numMC; ++m) {
    drams[m].step(
      soft_mem::RingIntf {
        .buffer = &ring[m + 1],
        .ingress = ring.validAt(m + 1),
        .eject = memif_ring_accept[m + 1],
        .canInject = !ring.validAt((m + 2) % (cfg.numMC + 1)) &&
                     (!ring.validAt(m + 1) || memif_eject[m + 1].accepting),
        .injected = &injected
      },
      memif_ext[m + 1],
      memif_eject[m + 1].accepting ? &*memif_eject[m + 1].presenting : nullptr,
      core_mem_accept.slice(1 + PU_PER_MC * m, 1 + PU_PER_MC * (m + 1))
    );
    ring.updateValidAt(m + 1, injected, memif_ring_accept[m + 1]);
  }
  ring.progress();

  // Periodic delta print.
  if (log_ && cycle > 0 && cycle % Stat::PERIOD == 0) {
    auto delta = stats_ - last_periodic_;
    uint64_t span = cycle - last_periodic_cycle_;
    std::cerr << "[Soft] Periodic stats (cycles " << last_periodic_cycle_
              << ".." << cycle << ", span=" << span << "):\n";
    delta.print(span);
    last_periodic_ = stats_;
    last_periodic_cycle_ = cycle;
  }
}

void SoftSystemBackend::accumulateStats() {
  uint64_t hops = 0;
  uint64_t idle_lanes = 0;
  uint64_t blocked = 0;
  uint64_t injections = 0;
  uint64_t mem_reqs = 0;
  uint64_t inflight_msgs = 0;
  uint64_t idle_cores = 0;

  for (uint16_t i = 1; i <= cfg.numPU; ++i) {
    const auto &r = routers[i];
    // Inflight router messages = total queued flits across all input
    // queues of every router.
    inflight_msgs += r.rt.totalFlits();
    // Core inject: an injection means a flit transferred from the PU
    // into the local router this cycle.
    if (r.buf.core_inject.presenting) {
      if (r.buf.core_inject.accepting) {
        ++hops;
        ++injections;
      } else {
        ++blocked;
      }
    }
    // Core eject: flit delivered to the local PU.
    if (r.buf.core_eject.presenting && r.buf.core_eject.accepting) ++hops;
    // Forward links: each is a possible egress lane this cycle.
    for (size_t o = 0; o < 4 && r.links.forwards[o]; ++o) {
      const auto &fwd = r.buf.forwards[o];
      if (fwd.presenting) {
        if (fwd.accepting) ++hops;
        else ++blocked;
      } else {
        ++idle_lanes;
      }
    }
    // Memif eject (from router into memif) counts as a memory request
    // arriving at the memif this cycle.
    if (r.links.memif.has_value()) {
      const auto &me = memif_eject[*r.links.memif];
      if (me.presenting && me.accepting) {
        ++hops;
        ++mem_reqs;
      }
    }
    // A core is "idle" this cycle if it has nothing to inject and
    // nothing being delivered to it.
    if (!r.buf.core_inject.presenting && !r.buf.core_eject.presenting)
      ++idle_cores;
  }

  // DRAM inflight = scalar slots in use + 1 if a bulk is active.
  uint64_t inflight_dram = 0;
  for (const auto &d : drams) {
    inflight_dram += d.scalarInflightCount();
    if (d.bulkActive()) ++inflight_dram;
    inflight_dram += d.bcstQueueOccupancy();
  }
  inflight_dram += periph.scalarInflightCount();

  // Response ring occupancy = number of valid slots on the ringbus.
  uint64_t inflight_resp = 0;
  for (size_t i = 0; i < cfg.numMC + 1; ++i)
    if (ring.validAt(i)) ++inflight_resp;

  stats_.total_hops          += hops;
  stats_.total_injections    += injections;
  stats_.total_mem_requests  += mem_reqs;
  stats_.idle_lane_cycles    += idle_lanes;
  stats_.idle_core_cycles    += idle_cores;
  stats_.blocked_structural  += blocked;
  stats_.inflight_messages_sum += inflight_msgs;
  stats_.inflight_dram_sum   += inflight_dram;
  stats_.inflight_resp_sum   += inflight_resp;
  stats_.cur_inflight_messages = inflight_msgs;
  stats_.cur_inflight_dram     = inflight_dram;
  stats_.cur_inflight_resp     = inflight_resp;
}

bool SoftSystemBackend::printStats(uint64_t cycles, bool final_print) {
  if (final_print) {
    double v_ms = verilator_ns_ / 1e6;
    double u_ms = uncore_ns_ / 1e6;
    double tot_ms = v_ms + u_ms;
    auto pct = [&](double x) { return tot_ms > 0 ? (100.0 * x / tot_ms) : 0.0; };
    fprintf(stderr,
            "[Soft] Final stats (over %lu cycles):\n"
            "[Soft] Time breakdown: verilator=%.3fms (%.1f%%) "
            "uncore=%.3fms (%.1f%%) total=%.3fms\n",
            (unsigned long)cycles,
            v_ms, pct(v_ms), u_ms, pct(u_ms), tot_ms);
    stats_.print(cycles);
  }
  return true;
}
