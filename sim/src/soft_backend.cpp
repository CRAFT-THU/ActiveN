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
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

#include "soft_backend.h"
#include "soft_components.h"
#include "soft_mem.h"

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
    memif_req_at(num_mc + 1),
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

  // Generate memif connection information
  memif_req_at[0].push_back(1); // Peripheral memif only connects to PU 1.

  // For each MC, every cluster in its zone contributes one connection
  // PU (the one in that cluster closest to the grid center). The order
  // of clusters in the zone determines the request-port index inside
  // the memif (matches RTL: System.scala `req(ci)` for ci in zone).
  for (size_t mc = 0; mc < num_mc; ++mc) {
    size_t memif_idx = 1 + mc;
    for (size_t ci = mc * clusters_per_mc; ci < (mc + 1) * clusters_per_mc; ++ci) {
      uint16_t cpu = best_conn_in_cluster(ci);
      uint8_t port_idx = static_cast<uint8_t>(memif_req_at[memif_idx].size());
      memif_req_at[memif_idx].push_back(cpu);
      if (pu_memif[cpu].has_value())
        throw std::logic_error("Conflicting memif assignment for PU");
      pu_memif[cpu] = std::make_pair(static_cast<uint16_t>(memif_idx), port_idx);
    }
  }

  // The peripheral memif (memif index 0) lives at PU 1. If PU 1 also
  // happens to be an MC connection PU for some MC, that conflict was
  // already caught above. Periph wins for PU 1 (overwrite is not
  // allowed by the throws above).
  if (pu_memif[1].has_value()) throw std::logic_error("Conflicting memif assignment for PU 1");
  pu_memif[1] = std::make_pair<uint16_t, uint8_t>(0, 0);
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

uint64_t SoftSystemBackend::workProfileNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool SoftSystemBackend::workProfileSampleCycle(uint64_t cycle) const {
  return workProfileEnabled && cycle > releaseResetAfter &&
         cycle % WORK_PROFILE_SAMPLE_PERIOD == 0;
}

void SoftSystemBackend::workProfileMark(size_t threadId,
                                        WorkProfilePhase phase,
                                        uint64_t localDone,
                                        uint64_t barrierArrival) {
  const size_t p = static_cast<size_t>(phase);
  workProfileTimes[threadId].local_done[p] = localDone;
  workProfileTimes[threadId].barrier_arrival[p] = barrierArrival;
}

void SoftSystemBackend::workProfileRecordPhase(uint64_t cycle,
                                               WorkProfilePhase phase) {
  const size_t p = static_cast<size_t>(phase);
  uint64_t localMin = workProfileTimes[0].local_done[p];
  uint64_t localMax = localMin;
  uint64_t arrivalMin = workProfileTimes[0].barrier_arrival[p];
  uint64_t arrivalMax = arrivalMin;
  uint64_t workerMin = numThreads > 1 ? workProfileTimes[1].local_done[p]
                                      : localMin;
  uint64_t workerMax = workerMin;

  for (size_t t = 1; t < numThreads; ++t) {
    localMin = std::min(localMin, workProfileTimes[t].local_done[p]);
    localMax = std::max(localMax, workProfileTimes[t].local_done[p]);
    arrivalMin = std::min(arrivalMin,
                          workProfileTimes[t].barrier_arrival[p]);
    arrivalMax = std::max(arrivalMax,
                          workProfileTimes[t].barrier_arrival[p]);
    workerMin = std::min(workerMin, workProfileTimes[t].local_done[p]);
    workerMax = std::max(workerMax, workProfileTimes[t].local_done[p]);
  }

  const uint64_t leaderLocal = workProfileTimes[0].local_done[p];
  const uint64_t leaderArrival = workProfileTimes[0].barrier_arrival[p];
  workProfilePhaseSamples.push_back({
      .cycle = cycle,
      .phase = phase,
      .local_spread_ns = localMax - localMin,
      .worker_spread_ns = workerMax - workerMin,
      .leader_extra_ns = leaderArrival - leaderLocal,
      .arrival_spread_ns = arrivalMax - arrivalMin,
      .critical_extension_ns = arrivalMax > localMax
                                   ? arrivalMax - localMax
                                   : 0,
  });
}

void SoftSystemBackend::workProfileRecordUnit(uint64_t cycle,
                                              WorkProfilePhase phase,
                                              WorkProfileUnit unit,
                                              size_t index,
                                              uint64_t duration) {
  workProfileUnitSamples.push_back({
      .cycle = cycle,
      .phase = phase,
      .unit = unit,
      .index = static_cast<uint16_t>(index),
      .duration_ns = duration,
  });
}

void SoftSystemBackend::workProfileRecordSubsteps(
    uint64_t cycle, WorkProfileSubstep first, WorkProfileSubstep last) {
  const size_t firstIndex = static_cast<size_t>(first);
  const size_t lastIndex = static_cast<size_t>(last);
  for (size_t s = firstIndex; s <= lastIndex; ++s) {
    for (size_t thread = 0; thread < numThreads; ++thread) {
      workProfileSubstepSamples.push_back({
          .cycle = cycle,
          .substep = static_cast<WorkProfileSubstep>(s),
          .thread = static_cast<uint16_t>(thread),
          .duration_ns = workProfileTimes[thread].substep_duration[s],
      });
    }
  }
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

  const size_t nf = my_dirs.size();
  size_t num_in  = nf + 1;                            // fwds + core_inject
  size_t num_out = nf + 1 + (links.memif ? 1 : 0);    // fwds + core_eject + memif

  // Output-port assignment (RTL convention: egress(forwards) first, then locals):
  //   0..nf-1 = forward egress (slot index = port index)
  //   nf      = core_eject
  //   nf+1    = memif (if present)
  const size_t core_eject_port = nf;
  std::optional<size_t> memif_port =
      links.memif ? std::optional<size_t>(nf + 1) : std::nullopt;
  links.num_forwards = static_cast<uint8_t>(nf);
  auto tbl = topo.routingTableFor(pu, dir_to_link, core_eject_port, memif_port);
  return WrappedRouter(std::move(links), std::move(tbl), num_in, num_out);
}

SoftSystemBackend::SoftSystemBackend(SystemConfig cfg_in, size_t threads)
  : cfg(std::move(cfg_in)),
    releaseResetAfter(10),
    topo(cfg.numPU, cfg.numMC),
    periph(0, 16, [&]() {
      // Build the single config-ROM line matching RTL System.scala.
      std::unordered_map<uint32_t, MemLine> rom;
      MemLine line{};
      uint32_t npu = static_cast<uint32_t>(cfg.numPU);
      uint32_t nmc = static_cast<uint32_t>(cfg.numMC);
      uint32_t ppm = nmc ? npu / nmc : 0;
      uint32_t lineShift = MEM_BUS_WIDTH_SIZE;
      uint64_t mcSize = cfg.core.mcSizes.empty() ? 0 : cfg.core.mcSizes[0];
      std::memcpy(&line[0],  &npu, 4);
      std::memcpy(&line[8],  &nmc, 4);
      std::memcpy(&line[16], &ppm, 4);
      std::memcpy(&line[24], &lineShift, 4);
      std::memcpy(&line[32], &mcSize, 8);
      rom[0x28000000u] = line;
      return rom;
    }()),
    drams(),
    cores(0),
    ring(1 + cfg.numMC),
    memif_eject(cfg.numMC + 1),
    memif_eject_accept(cfg.numMC + 1),
    memif_ext(cfg.numMC + 1),
    memif_ring_accept(cfg.numMC + 1, false),
    core_mem(cfg.numPU),
    core_mem_accept(cfg.numPU),
    routers(cfg.numPU,
            [this](size_t i) { return buildRouter(static_cast<uint16_t>(i)); }),
    numThreads(threads),
    workProfileEnabled([] {
      const char *value = std::getenv("SOFT_WORK_PROFILE");
      return value != nullptr && std::strcmp(value, "0") != 0;
    }()),
    workProfileTimes(numThreads),
    stageStart(numThreads),
    stageDone(numThreads),
    stepStart(numThreads),
    stepDone(numThreads)
{
  // Initialize per-MC DRAMIfs. PU range is contiguous in PU-id order.
  if (cfg.numPU % cfg.numMC != 0) throw std::invalid_argument("numPU must be divisible by numMC");
  if ((cfg.numPU / cfg.numMC) % soft_mem::CLUSTER_SIZE != 0) throw std::invalid_argument("PU per MC must be a multiple of cluster size");
  if (numThreads < 1 || numThreads > cfg.numPU) throw std::invalid_argument("numThreads must be between 1 and numPU");

  if (workProfileEnabled) {
    workProfilePhaseSamples.reserve(1 << 18);
    workProfileUnitSamples.reserve(1 << 20);
    workProfileSubstepSamples.reserve(1 << 20);
    std::cerr << "[Soft work profile] sampling every "
              << WORK_PROFILE_SAMPLE_PERIOD << " cycles\n";
  }

  const size_t PU_PER_MC = cfg.numPU / cfg.numMC;
  const size_t CLUSTER_PER_MC = PU_PER_MC / soft_mem::CLUSTER_SIZE;
  drams.reserve(cfg.numMC);
  for (size_t m = 0; m < cfg.numMC; ++m) {
    drams.emplace_back(m + 1, CLUSTER_PER_MC, 64, 16, 2,
                       1 + m * PU_PER_MC,
                       1 + (m + 1) * PU_PER_MC);
  }
  prepareNextCoreMem(0);

  // Size per-memif eject buffers: one slot per request port (matches
  // topo.memif_req_at[i].size()).
  for (size_t i = 0; i < topo.memif_req_at.size(); ++i)
    memif_eject[i].resize(topo.memif_req_at[i].size());

  char rtl_name_buffer[64];
  cores.reserve(cfg.numPU);
  for (size_t i = 1; i <= cfg.numPU; ++i) {
    sprintf(rtl_name_buffer, "soft_pu_%lu", i);
    cores.emplace_back(std::make_unique<soft_rtl>(rtl_name_buffer));
    cores[i]->reset = true;
    cores[i]->cfg_hartid = static_cast<uint32_t>(i);
    cores[i]->eval();
  }

  // Kickoff worker threads
  workers.reserve(numThreads - 1);
  for (size_t i = 1; i < numThreads; ++i)
    workers.emplace_back(&SoftSystemBackend::worker, this, i);
}

void SoftSystemBackend::attachTrace(VerilatedFstC *trace_file, int depth) {
  for (size_t i = 1; i <= cfg.numPU; ++i)
    cores[i]->trace(trace_file, depth);
}

SystemConfig SoftSystemBackend::config() const { return cfg; }

// Peek only the priority of the flit the core is presenting for injection
// (nullopt = nothing presented). Used for the stage accept decision, so no
// flit needs to be materialized before it is known to be accepted.
__attribute__((always_inline))
static std::optional<uint8_t> peekCoreInjectPrio(const soft_rtl &core) {
  if (!core.ext_out_valid) return std::nullopt;
  return static_cast<uint8_t>(core.ext_out_bits_tag >> 10); // matches Flit::prio()
}

// Materialize the full flit the core is presenting for injection. ext_out is
// stable between the stage negedge eval and the step posedge, so this reads
// the same flit whose prio was peeked at stage. The caller must invoke this
// before the step posedge and only when the injection was accepted.
__attribute__((always_inline))
static Flit peekCoreInjectFlit(const soft_rtl &core, uint16_t puIdx) {
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
static void presentCoreEject(soft_rtl &core, uint16_t puIdx, const Flit *flit) {
  core.ext_in_valid = flit != nullptr;
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
    // The Verilated wide value and MemLine have the same byte layout.
    static_assert(sizeof(core.mem_unicast_bits_data) == sizeof(resp.unicast->second));
    std::memcpy(&core.mem_unicast_bits_data, resp.unicast->second.data(),
                sizeof(resp.unicast->second));
  }

  core.mem_broadcast_valid = resp.bcast.has_value();
  if (resp.bcast.has_value()) {
    core.mem_broadcast_bits_tag = resp.bcast->tag;
    core.mem_broadcast_bits_carried_0 = resp.bcast->carried[0];
    core.mem_broadcast_bits_carried_1 = resp.bcast->carried[1];

    // BcastLine.line is a Vec of BcastBeat, where BcastBeat is
    // (data:32, idx:16, pu:16) — Chisel default Bundle places `data`
    // at the MSB. The soft side stores the same line as uint32_t words
    // in little-endian order. The RTL PU
    // expects line(i) at raw[2*i, 2*i+1] (i.e. Vec(0) at LOW bits in
    // this representation), so:
    //   line(i).data       = soft.line[2*i + 1]
    //   line(i).idx || pu  = soft.line[2*i]   (idx upper, pu lower)
    const auto &raw = resp.bcast->line;
    core.mem_broadcast_bits_line_0_data = raw[1];
    core.mem_broadcast_bits_line_0_idx  = (uint16_t)(raw[0] >> 16);
    core.mem_broadcast_bits_line_0_pu   = (uint16_t)(raw[0] & 0xFFFF);
    core.mem_broadcast_bits_line_1_data = raw[3];
    core.mem_broadcast_bits_line_1_idx  = (uint16_t)(raw[2] >> 16);
    core.mem_broadcast_bits_line_1_pu   = (uint16_t)(raw[2] & 0xFFFF);
    core.mem_broadcast_bits_line_2_data = raw[5];
    core.mem_broadcast_bits_line_2_idx  = (uint16_t)(raw[4] >> 16);
    core.mem_broadcast_bits_line_2_pu   = (uint16_t)(raw[4] & 0xFFFF);
    core.mem_broadcast_bits_line_3_data = raw[7];
    core.mem_broadcast_bits_line_3_idx  = (uint16_t)(raw[6] >> 16);
    core.mem_broadcast_bits_line_3_pu   = (uint16_t)(raw[6] & 0xFFFF);
    core.mem_broadcast_bits_line_4_data = raw[9];
    core.mem_broadcast_bits_line_4_idx  = (uint16_t)(raw[8] >> 16);
    core.mem_broadcast_bits_line_4_pu   = (uint16_t)(raw[8] & 0xFFFF);
    core.mem_broadcast_bits_line_5_data = raw[11];
    core.mem_broadcast_bits_line_5_idx  = (uint16_t)(raw[10] >> 16);
    core.mem_broadcast_bits_line_5_pu   = (uint16_t)(raw[10] & 0xFFFF);
    core.mem_broadcast_bits_line_6_data = raw[13];
    core.mem_broadcast_bits_line_6_idx  = (uint16_t)(raw[12] >> 16);
    core.mem_broadcast_bits_line_6_pu   = (uint16_t)(raw[12] & 0xFFFF);
    core.mem_broadcast_bits_line_7_data = raw[15];
    core.mem_broadcast_bits_line_7_idx  = (uint16_t)(raw[14] >> 16);
    core.mem_broadcast_bits_line_7_pu   = (uint16_t)(raw[14] & 0xFFFF);
  }
}

void SoftSystemBackend::peek(uint64_t cycle, std::vector<MemBusOut *> out) {
  PhaseTimer _t(*this);
  if constexpr (ASSERTIONS_ENABLED) {
    if (out.size() != cfg.numMC + 1)
      throw std::invalid_argument("peek: out size mismatch");
  }
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
  const bool profile = workProfileSampleCycle(cycle);
  // Publish completion of the preceding cleanup before any thread reads a
  // neighboring router's committed queues.
  if (profile) {
    const uint64_t now = workProfileNowNs();
    workProfileMark(0, WorkProfilePhase::StageStart, now, now);
  }
  stageStart.arrive_and_wait();
  if (cycle > 0 && workProfileSampleCycle(cycle - 1))
    workProfileRecordSubsteps(cycle - 1, WorkProfileSubstep::StepCleanup,
                              WorkProfileSubstep::StepCleanup);
  if (profile)
    workProfileRecordPhase(cycle, WorkProfilePhase::StageStart);
  try {
    if constexpr (ASSERTIONS_ENABLED) {
      if (cycle <= releaseResetAfter) {
        for (const auto &i : in) {
          if (i.resp) throw std::runtime_error("Memory response presented during reset");
        }
      }
    }

    memif_ext = in;
    stageWorkResolve(0, profile);
    if (profile)
      workProfileTimes[0].local_done[static_cast<size_t>(
          WorkProfilePhase::StageDone)] = workProfileNowNs();

    auto resolveMemifTokens = [this](size_t memifIdx) {
      for (size_t port = 0; port < memif_eject[memifIdx].size(); ++port) {
        const uint16_t pu = topo.memif_req_at[memifIdx][port];
        auto &router = routers[pu];
        if constexpr (ASSERTIONS_ENABLED) {
          if (!router.links.memif || router.links.memif->first != memifIdx ||
              router.links.memif->second != port)
            throw std::logic_error("MemIf source router mapping mismatch");
        }
        memif_eject[memifIdx][port].token =
            router.rt.peekToken(router.links.num_forwards + 1);
      }
    };
    auto peekMemifFlit = [this](size_t memifIdx, size_t port) -> const Flit * {
      const auto &token = memif_eject[memifIdx][port].token;
      if (!token) return nullptr;
      const uint16_t pu = topo.memif_req_at[memifIdx][port];
      return routers[pu].rt.peek(*token);
    };

    uint64_t unitStart = profile ? workProfileNowNs() : 0;
    resolveMemifTokens(0);
    memif_ring_accept[0] = ring.peekAt(0) && periph.ringCanAccept(*ring.peekAt(0));
    memif_eject_accept[0] = periph.nocAcceptMultiple(
      [&peekMemifFlit](size_t idx) -> const Flit * {
        return peekMemifFlit(0, idx);
      });
    if (profile)
      workProfileRecordUnit(cycle, WorkProfilePhase::StageDone,
                            WorkProfileUnit::AcceptPeriph, 0,
                            workProfileNowNs() - unitStart);
    for (size_t m = 0; m < cfg.numMC; ++m) {
      unitStart = profile ? workProfileNowNs() : 0;
      resolveMemifTokens(m + 1);
      memif_ring_accept[m + 1] =
          ring.peekAt(m + 1) && drams[m].ringCanAccept(*ring.peekAt(m + 1));
      memif_eject_accept[m + 1] = drams[m].nocAcceptMultiple(
        [&peekMemifFlit, m](size_t idx) -> const Flit * {
          return peekMemifFlit(m + 1, idx);
        });
      if (profile)
        workProfileRecordUnit(cycle, WorkProfilePhase::StageDone,
                              WorkProfileUnit::AcceptMC, m,
                              workProfileNowNs() - unitStart);
    }
  } catch (const std::exception &e) {
    std::cerr << "Exception at thread " << 0 << ": " << e.what() << std::endl;
    std::exit(1);
  }

  if (profile)
    workProfileTimes[0].barrier_arrival[static_cast<size_t>(
        WorkProfilePhase::StageDone)] = workProfileNowNs();
  stageDone.arrive_and_wait();
  if (profile) {
    workProfileRecordPhase(cycle, WorkProfilePhase::StageDone);
    workProfileRecordSubsteps(cycle,
                              WorkProfileSubstep::StageResolveLinks,
                              WorkProfileSubstep::StageCaptureAccepts);
  }
}

void SoftSystemBackend::stageWorkResolve(size_t threadId, bool profile) {
  auto [puStart, puEnd] = puWorkRange(threadId);
  auto &substeps = workProfileTimes[threadId].substep_duration;
  uint64_t substepStart = profile ? workProfileNowNs() : 0;

  // Resolve each incoming transfer from committed source-router state. Router
  // queues are read-only throughout stage and are not committed until step.
  for (uint16_t i = puStart; i < puEnd; ++i) {
    auto &local = routers[i];
    const size_t nf = local.links.num_forwards;
    local.buf.core_inject_presenting = peekCoreInjectPrio(*cores[i]);
    local.buf.core_inject_accepting =
        local.rt.prepareEnq(nf, local.buf.core_inject_presenting);
    for (size_t o = 0; o < nf; ++o) {
      auto [from, port] = *local.links.forwards[o];
      auto &token = local.buf.forward_presenting[o];
      token = routers[from].rt.peekToken(port);
      local.buf.forward_accepting[o] = local.rt.prepareEnq(
          o, token ? std::optional<uint8_t>(token->prio) : std::nullopt);
    }
    local.buf.core_eject_presenting = local.rt.peekToken(nf);
  }
  if (profile)
    substeps[static_cast<size_t>(WorkProfileSubstep::StageResolveLinks)] =
        workProfileNowNs() - substepStart;

  // Drive PU inputs and settle on the negedge. Contract: accept signals
  // from the cores are read post-eval (after combinational settle).
  if (profile) substepStart = workProfileNowNs();
  for (uint16_t i = puStart; i < puEnd; ++i) {
    auto &router = routers[i];
    const Flit *core_eject = router.buf.core_eject_presenting
        ? router.rt.peek(*router.buf.core_eject_presenting) : nullptr;
    presentCoreEject(*cores[i], i, core_eject);
    presentCoreMem(*cores[i], core_mem[i]);
    cores[i]->ext_out_ready = router.buf.core_inject_accepting;
    cores[i]->clock = false; // negedge
  }
  if (profile) {
    substeps[static_cast<size_t>(WorkProfileSubstep::StageDriveInputs)] =
        workProfileNowNs() - substepStart;
    substepStart = workProfileNowNs();
  }
  auto v0 = std::chrono::high_resolution_clock::now();
  for (uint16_t i = puStart; i < puEnd; ++i) {
    cores[i]->eval();
  }
  auto v1 = std::chrono::high_resolution_clock::now();
  // TODO: move into non-thread local atomic member variable
  g_soft_verilator_ns +=
      std::chrono::duration_cast<std::chrono::nanoseconds>(v1 - v0).count();
  if (profile) {
    substeps[static_cast<size_t>(WorkProfileSubstep::StageNegedgeEval)] =
        workProfileNowNs() - substepStart;
    substepStart = workProfileNowNs();
  }
  for (uint16_t i = puStart; i < puEnd; ++i) {
    routers[i].buf.core_eject_accepting =
        routers[i].buf.core_eject_presenting.has_value() &&
        cores[i]->ext_in_ready;
    core_mem_accept[i].unicast = cores[i]->mem_unicast_valid;
    core_mem_accept[i].broadcast =
        cores[i]->mem_broadcast_ready && cores[i]->mem_broadcast_valid;
  }
  if (profile)
    substeps[static_cast<size_t>(WorkProfileSubstep::StageCaptureAccepts)] =
        workProfileNowNs() - substepStart;
}

void SoftSystemBackend::prepareNextCoreMem(uint64_t cycle) {
  const bool profile = workProfileSampleCycle(cycle);
  const size_t PU_PER_MC = cfg.numPU / cfg.numMC;
  for (size_t m = 0; m < cfg.numMC; ++m) {
    const uint64_t unitStart = profile ? workProfileNowNs() : 0;
    auto bufferSpan = core_mem.slice(m * PU_PER_MC + 1,
                                     (m + 1) * PU_PER_MC + 1);
    // This snapshot is consumed by the cores during the next iteration's
    // stage, after the current DRAMIf and ring state has committed.
    drams[m].peekPUs(bufferSpan);
    if (profile)
      workProfileRecordUnit(cycle, WorkProfilePhase::StepDone,
                            WorkProfileUnit::PeekMC, m,
                            workProfileNowNs() - unitStart);
  }
}

void SoftSystemBackend::step(uint64_t cycle) {
  PhaseTimer _t(*this);
  const bool profile = workProfileSampleCycle(cycle);

  // Accumulate per-cycle stats BEFORE committing this cycle's
  // transfers. The accepting signals computed in stage() are still
  // valid; queue/inflight occupancies reflect the state entering
  // this cycle.
  if (profile)
    workProfileTimes[0].local_done[static_cast<size_t>(
        WorkProfilePhase::StepStart)] = workProfileNowNs();
  const uint64_t statsStart = profile ? workProfileNowNs() : 0;
  accumulateStats();
  if (profile)
    workProfileRecordUnit(cycle, WorkProfilePhase::StepStart,
                          WorkProfileUnit::Stats, 0,
                          workProfileNowNs() - statsStart);

  resetReleaseNow = cycle == releaseResetAfter;

  if (profile)
    workProfileTimes[0].barrier_arrival[static_cast<size_t>(
        WorkProfilePhase::StepStart)] = workProfileNowNs();
  stepStart.arrive_and_wait();
  if (profile)
    workProfileRecordPhase(cycle, WorkProfilePhase::StepStart);

  try {
    stepWork(0, profile);
    if (profile)
      workProfileTimes[0].local_done[static_cast<size_t>(
          WorkProfilePhase::StepDone)] = workProfileNowNs();

    const size_t PU_PER_MC = cfg.numPU / cfg.numMC;
    bool injected;
    auto takeMemif = [this](size_t memifIdx) {
      std::unique_ptr<Flit> flit;
      if (memif_eject_accept[memifIdx]) {
        uint8_t port = *memif_eject_accept[memifIdx];
        uint16_t pu = topo.memif_req_at[memifIdx][port];
        flit = routers[pu].rt.take(*memif_eject[memifIdx][port].token);
      }
      return flit;
    };
    uint64_t unitStart = profile ? workProfileNowNs() : 0;
    auto periphFlit = takeMemif(0);
    std::optional<std::pair<std::reference_wrapper<const Flit>, uint8_t>> periphView;
    if (periphFlit) periphView = {{*periphFlit, *memif_eject_accept[0]}};
    periph.step(
      soft_mem::RingIntf {
        .buffer = &ring[0],
        .ingress = ring.validAt(0),
        .eject = memif_ring_accept[0],
        // canInject: forward queue is empty AND no forward will push into
        // the outgoing queue this cycle.
        .canInject = !ring.validAt(1) &&
                     (!ring.validAt(0) || memif_ring_accept[0]),
        .injected = &injected
      },
      memif_ext[0],
      periphView
    );
    if (profile)
      workProfileRecordUnit(cycle, WorkProfilePhase::StepDone,
                            WorkProfileUnit::StepPeriph, 0,
                            workProfileNowNs() - unitStart);
    unitStart = profile ? workProfileNowNs() : 0;
    ring.updateValidAt(0, injected, memif_ring_accept[0]);
    if (profile)
      workProfileRecordUnit(cycle, WorkProfilePhase::StepDone,
                            WorkProfileUnit::RingUpdate, 0,
                            workProfileNowNs() - unitStart);

    for (size_t m = 0; m < cfg.numMC; ++m) {
      unitStart = profile ? workProfileNowNs() : 0;
      auto dramFlit = takeMemif(m + 1);
      std::optional<std::pair<std::reference_wrapper<const Flit>, uint8_t>> dramView;
      if (dramFlit) dramView = {{*dramFlit, *memif_eject_accept[m + 1]}};
      drams[m].step(
        soft_mem::RingIntf {
          .buffer = &ring[m + 1],
          .ingress = ring.validAt(m + 1),
          .eject = memif_ring_accept[m + 1],
          .canInject = !ring.validAt((m + 2) % (cfg.numMC + 1)) &&
                       (!ring.validAt(m + 1) || memif_ring_accept[m + 1]),
          .injected = &injected
        },
        memif_ext[m + 1],
        dramView,
        core_mem_accept.slice(1 + PU_PER_MC * m, 1 + PU_PER_MC * (m + 1))
      );
      if (profile)
        workProfileRecordUnit(cycle, WorkProfilePhase::StepDone,
                              WorkProfileUnit::StepMC, m,
                              workProfileNowNs() - unitStart);
      unitStart = profile ? workProfileNowNs() : 0;
      ring.updateValidAt(m + 1, injected, memif_ring_accept[m + 1]);
      if (profile)
        workProfileRecordUnit(cycle, WorkProfilePhase::StepDone,
                              WorkProfileUnit::RingUpdate, m + 1,
                              workProfileNowNs() - unitStart);
    }
    unitStart = profile ? workProfileNowNs() : 0;
    ring.progress();
    if (profile)
      workProfileRecordUnit(cycle, WorkProfilePhase::StepDone,
                            WorkProfileUnit::RingProgress, 0,
                            workProfileNowNs() - unitStart);
    prepareNextCoreMem(cycle);
  } catch (const std::exception &e) {
    std::cerr << "Exception at thread " << 0 << ": " << e.what() << std::endl;
    std::exit(1);
  }

  if (profile)
    workProfileTimes[0].barrier_arrival[static_cast<size_t>(
        WorkProfilePhase::StepDone)] = workProfileNowNs();
  stepDone.arrive_and_wait();
  if (profile) {
    workProfileRecordPhase(cycle, WorkProfilePhase::StepDone);
    workProfileRecordSubsteps(cycle, WorkProfileSubstep::StepPosedge,
                              WorkProfileSubstep::StepTransfer);
  }
  stepCleanup(0, profile);

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

void SoftSystemBackend::stepWork(size_t threadId, bool profile) {
  auto [puStart, puEnd] = puWorkRange(threadId);
  auto &substeps = workProfileTimes[threadId].substep_duration;
  const uint64_t posedgeStart = profile ? workProfileNowNs() : 0;

  // Materialize accepted core-inject flits from RTL BEFORE the posedge (the
  // posedge consumes ext_out), then posedge-eval every core.
  auto v0 = std::chrono::high_resolution_clock::now();
  for (size_t i = puStart; i < puEnd; ++i) {
    auto &router = routers[i];
    if (router.buf.core_inject_accepting)
      router.rt.place(router.links.num_forwards,
          std::make_unique<Flit>(peekCoreInjectFlit(*cores[i], static_cast<uint16_t>(i))));

    cores[i]->clock = true;
    cores[i]->eval();

    if (resetReleaseNow) {
      cores[i]->reset = false;
      cores[i]->eval();
    }
  }
  auto v1 = std::chrono::high_resolution_clock::now();
  g_soft_verilator_ns +=
      std::chrono::duration_cast<std::chrono::nanoseconds>(v1 - v0).count();
  if (profile)
    substeps[static_cast<size_t>(WorkProfileSubstep::StepPosedge)] =
        workProfileNowNs() - posedgeStart;

  // Each destination router moves accepted flits directly from source slots.
  const uint64_t transferStart = profile ? workProfileNowNs() : 0;
  for (size_t i = puStart; i < puEnd; ++i) {
    auto &router = routers[i];
    const auto &links = router.links;
    const size_t nf = links.num_forwards;
    for (size_t k = 0; k < nf; ++k) {
      const uint16_t from = links.forwards[k]->first;
      const auto &sourceToken = router.buf.forward_presenting[k];
      if (router.buf.forward_accepting[k])
        router.rt.place(k, routers[from].rt.take(*sourceToken));
    }
    if (router.buf.core_eject_accepting)
      (void)router.rt.take(*router.buf.core_eject_presenting);
  }
  if (profile)
    substeps[static_cast<size_t>(WorkProfileSubstep::StepTransfer)] =
        workProfileNowNs() - transferStart;
}

void SoftSystemBackend::stepCleanup(size_t threadId, bool profile) {
  auto [puStart, puEnd] = puWorkRange(threadId);
  const uint64_t cleanupStart = profile ? workProfileNowNs() : 0;
  for (size_t i = puStart; i < puEnd; ++i) {
    auto &router = routers[i];
    const size_t nf = router.links.num_forwards;
    Router<RouteFn, ROUTER_Q_DEPTH>::OutputTokens accepted{};
    for (size_t o = 0; o < nf; ++o) {
      auto [to, port] = *router.links.forwards[o];
      const auto &destination = routers[to].buf;
      if (destination.forward_accepting[port])
        accepted[o] = destination.forward_presenting[port];
    }
    if (router.buf.core_eject_accepting)
      accepted[nf] = router.buf.core_eject_presenting;
    if (router.links.memif &&
        std::make_optional(router.links.memif->second) == memif_eject_accept[router.links.memif->first])
      accepted[nf + 1] = memif_eject[router.links.memif->first][router.links.memif->second].token;
    router.rt.step(accepted);
  }
  if (profile)
    workProfileTimes[threadId]
        .substep_duration[static_cast<size_t>(
            WorkProfileSubstep::StepCleanup)] =
        workProfileNowNs() - cleanupStart;
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
    if (r.buf.core_inject_presenting) {
      if (r.buf.core_inject_accepting) {
        ++hops;
        ++injections;
      } else {
        ++blocked;
      }
    }
    // Core eject: flit delivered to the local PU.
    if (r.buf.core_eject_presenting && r.buf.core_eject_accepting) ++hops;
    // Each incoming forward link corresponds one-to-one with a mesh egress
    // lane, so destination-owned decisions can count the same global totals.
    for (size_t o = 0; o < r.links.num_forwards; ++o) {
      const auto &fwd = r.buf.forward_presenting[o];
      if (fwd) {
        if (r.buf.forward_accepting[o]) ++hops;
        else ++blocked;
      } else {
        ++idle_lanes;
      }
    }
    // A core is "idle" this cycle if it has nothing to inject and
    // nothing being delivered to it.
    if (!r.buf.core_inject_presenting && !r.buf.core_eject_presenting)
      ++idle_cores;
  }

  // Memif eject (from router into memif) counts as a memory request
  // arriving at the memif this cycle.
  for (auto &a : memif_eject_accept) {
    if (a) {
      ++hops;
      ++mem_reqs;
    }
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

void SoftSystemBackend::printWorkProfile() const {
  if (!workProfileEnabled || workProfilePhaseSamples.empty()) return;

  struct Distribution {
    double mean;
    uint64_t p50;
    uint64_t p90;
    uint64_t p99;
    uint64_t max;
  };
  auto distribution = [](std::vector<uint64_t> values) -> Distribution {
    std::sort(values.begin(), values.end());
    long double sum = 0;
    for (uint64_t value : values) sum += value;
    auto percentile = [&values](size_t numerator) {
      const size_t index =
          ((values.size() - 1) * numerator + 99) / 100;
      return values[index];
    };
    return {
      .mean = static_cast<double>(sum / values.size()),
      .p50 = percentile(50),
      .p90 = percentile(90),
      .p99 = percentile(99),
      .max = values.back(),
    };
  };
  auto phaseName = [](WorkProfilePhase phase) {
    switch (phase) {
      case WorkProfilePhase::StageStart: return "stageStart";
      case WorkProfilePhase::StageDone: return "stageDone";
      case WorkProfilePhase::StepStart: return "stepStart";
      case WorkProfilePhase::StepDone: return "stepDone";
      case WorkProfilePhase::Count: break;
    }
    return "unknown";
  };
  auto unitName = [](WorkProfileUnit unit) {
    switch (unit) {
      case WorkProfileUnit::Stats: return "stats";
      case WorkProfileUnit::PeekMC: return "peekMC";
      case WorkProfileUnit::AcceptPeriph: return "acceptPeriph";
      case WorkProfileUnit::AcceptMC: return "acceptMC";
      case WorkProfileUnit::StepPeriph: return "stepPeriph";
      case WorkProfileUnit::StepMC: return "stepMC";
      case WorkProfileUnit::RingUpdate: return "ringUpdate";
      case WorkProfileUnit::RingProgress: return "ringProgress";
    }
    return "unknown";
  };
  auto substepName = [](WorkProfileSubstep substep) {
    switch (substep) {
      case WorkProfileSubstep::StageResolveLinks: return "stageResolveLinks";
      case WorkProfileSubstep::StageDriveInputs: return "stageDriveInputs";
      case WorkProfileSubstep::StageNegedgeEval: return "stageNegedgeEval";
      case WorkProfileSubstep::StageCaptureAccepts:
        return "stageCaptureAccepts";
      case WorkProfileSubstep::StepPosedge: return "stepPosedge";
      case WorkProfileSubstep::StepTransfer: return "stepTransfer";
      case WorkProfileSubstep::StepCleanup: return "stepCleanup";
      case WorkProfileSubstep::Count: break;
    }
    return "unknown";
  };
  auto printDistribution = [](const Distribution &d) {
    std::cerr << "mean=" << std::fixed << std::setprecision(1) << d.mean
              << " p50=" << d.p50
              << " p90=" << d.p90
              << " p99=" << d.p99
              << " max=" << d.max;
  };
  auto printScaledDistribution = [](const Distribution &d, double scale,
                                    const char *suffix) {
    std::cerr << "mean=" << std::fixed << std::setprecision(2)
              << d.mean / scale
              << " p50=" << d.p50 / scale
              << " p90=" << d.p90 / scale
              << " p99=" << d.p99 / scale
              << " max=" << d.max / scale << suffix;
  };

  std::cerr << "[Soft work profile] times are nanoseconds; local-spread is "
               "the optimistic first-arriver task budget\n";
  for (size_t p = 0; p < WORK_PROFILE_PHASE_COUNT; ++p) {
    const auto phase = static_cast<WorkProfilePhase>(p);
    std::vector<uint64_t> localSpread;
    std::vector<uint64_t> workerSpread;
    std::vector<uint64_t> leaderExtra;
    std::vector<uint64_t> arrivalSpread;
    std::vector<uint64_t> criticalExtension;
    for (const auto &sample : workProfilePhaseSamples) {
      if (sample.phase != phase) continue;
      localSpread.push_back(sample.local_spread_ns);
      workerSpread.push_back(sample.worker_spread_ns);
      leaderExtra.push_back(sample.leader_extra_ns);
      arrivalSpread.push_back(sample.arrival_spread_ns);
      criticalExtension.push_back(sample.critical_extension_ns);
    }
    if (localSpread.empty()) continue;
    std::cerr << "[Soft work profile] phase " << phaseName(phase)
              << " samples=" << localSpread.size() << " local-spread ";
    printDistribution(distribution(std::move(localSpread)));
    std::cerr << "\n[Soft work profile] phase " << phaseName(phase)
              << " worker-spread ";
    printDistribution(distribution(std::move(workerSpread)));
    std::cerr << "\n[Soft work profile] phase " << phaseName(phase)
              << " leader-extra ";
    printDistribution(distribution(std::move(leaderExtra)));
    std::cerr << "\n[Soft work profile] phase " << phaseName(phase)
              << " arrival-spread ";
    printDistribution(distribution(std::move(arrivalSpread)));
    std::cerr << "\n[Soft work profile] phase " << phaseName(phase)
              << " critical-extension ";
    printDistribution(distribution(std::move(criticalExtension)));
    std::cerr << '\n';
  }

  auto phaseKey = [](uint64_t cycle, WorkProfilePhase phase) {
    return cycle * WORK_PROFILE_PHASE_COUNT + static_cast<size_t>(phase);
  };
  auto availableSlack = [&](const WorkProfileUnitSample &unit) {
    const uint64_t key = phaseKey(unit.cycle, unit.phase);
    auto it = std::lower_bound(
        workProfilePhaseSamples.begin(), workProfilePhaseSamples.end(), key,
        [&](const WorkProfilePhaseSample &sample, uint64_t wanted) {
          return phaseKey(sample.cycle, sample.phase) < wanted;
        });
    return it != workProfilePhaseSamples.end() &&
                   phaseKey(it->cycle, it->phase) == key
               ? it->local_spread_ns
               : uint64_t{0};
  };

  constexpr size_t UNIT_COUNT =
      static_cast<size_t>(WorkProfileUnit::RingProgress) + 1;
  for (size_t u = 0; u < UNIT_COUNT; ++u) {
    const auto unit = static_cast<WorkProfileUnit>(u);
    size_t maxIndex = 0;
    bool present = false;
    for (const auto &sample : workProfileUnitSamples) {
      if (sample.unit == unit) {
        present = true;
        maxIndex = std::max(maxIndex, static_cast<size_t>(sample.index));
      }
    }
    if (!present) continue;
    for (size_t index = 0; index <= maxIndex; ++index) {
      std::vector<uint64_t> durations;
      size_t hidden = 0;
      for (const auto &sample : workProfileUnitSamples) {
        if (sample.unit != unit || sample.index != index) continue;
        durations.push_back(sample.duration_ns);
        if (sample.duration_ns <= availableSlack(sample)) ++hidden;
      }
      if (durations.empty()) continue;
      std::cerr << "[Soft work profile] unit " << unitName(unit)
                << '[' << index << "] samples=" << durations.size()
                << " duration ";
      printDistribution(distribution(durations));
      std::cerr << " hidden-by-local-spread=" << std::fixed
                << std::setprecision(1)
                << (100.0 * hidden / durations.size()) << "%\n";
    }
  }

  for (size_t s = 0; s < WORK_PROFILE_SUBSTEP_COUNT; ++s) {
    const auto substep = static_cast<WorkProfileSubstep>(s);
    std::vector<const WorkProfileSubstepSample *> samples;
    std::vector<std::vector<uint64_t>> byThread(numThreads);
    for (const auto &sample : workProfileSubstepSamples) {
      if (sample.substep != substep) continue;
      samples.push_back(&sample);
      byThread[sample.thread].push_back(sample.duration_ns);
    }
    if (samples.empty()) continue;

    for (size_t thread = 0; thread < numThreads; ++thread) {
      if (byThread[thread].empty()) continue;
      const auto [puStart, puEnd] = puWorkRange(thread);
      std::cerr << "[Soft work profile] substep " << substepName(substep)
                << " thread=" << thread << " pu=" << puStart << '-'
                << (puEnd - 1) << " samples=" << byThread[thread].size()
                << " duration ";
      printDistribution(distribution(std::move(byThread[thread])));
      std::cerr << '\n';
    }

    std::vector<uint64_t> maxMinusMin;
    std::vector<uint64_t> maxMinusMean;
    std::vector<uint64_t> efficiencyBasisPoints;
    std::vector<uint64_t> idealSpeedupMilli;
    std::vector<size_t> slowestCount(numThreads, 0);
    uint64_t totalMax = 0;
    long double totalMean = 0;
    size_t completeCycles = 0;
    for (size_t offset = 0; offset + numThreads <= samples.size();
         offset += numThreads) {
      const uint64_t cycle = samples[offset]->cycle;
      bool complete = true;
      uint64_t sum = 0;
      uint64_t minimum = samples[offset]->duration_ns;
      uint64_t maximum = minimum;
      for (size_t thread = 0; thread < numThreads; ++thread) {
        const auto &sample = *samples[offset + thread];
        if (sample.cycle != cycle || sample.thread != thread) {
          complete = false;
          break;
        }
        sum += sample.duration_ns;
        minimum = std::min(minimum, sample.duration_ns);
        maximum = std::max(maximum, sample.duration_ns);
      }
      if (!complete) continue;

      const uint64_t mean = sum / numThreads;
      maxMinusMin.push_back(maximum - minimum);
      maxMinusMean.push_back(maximum - mean);
      efficiencyBasisPoints.push_back(
          maximum == 0 ? 10000 : (sum * 10000) / (numThreads * maximum));
      idealSpeedupMilli.push_back(
          sum == 0 ? 1000 : (maximum * numThreads * 1000) / sum);
      for (size_t thread = 0; thread < numThreads; ++thread) {
        if (samples[offset + thread]->duration_ns == maximum)
          ++slowestCount[thread];
      }
      totalMax += maximum;
      totalMean += static_cast<long double>(sum) / numThreads;
      ++completeCycles;
    }
    if (completeCycles == 0) continue;

    std::cerr << "[Soft work profile] substep " << substepName(substep)
              << " cycles=" << completeCycles << " max-minus-min ";
    printDistribution(distribution(std::move(maxMinusMin)));
    std::cerr << "\n[Soft work profile] substep " << substepName(substep)
              << " max-minus-mean ";
    printDistribution(distribution(std::move(maxMinusMean)));
    std::cerr << "\n[Soft work profile] substep " << substepName(substep)
              << " balance-efficiency ";
    printScaledDistribution(distribution(std::move(efficiencyBasisPoints)),
                            100.0, "%");
    std::cerr << "\n[Soft work profile] substep " << substepName(substep)
              << " ideal-balance-speedup ";
    printScaledDistribution(distribution(std::move(idealSpeedupMilli)),
                            1000.0, "x");
    std::cerr << " aggregate=" << std::fixed << std::setprecision(3)
              << (totalMean > 0 ? totalMax / totalMean : 1.0L) << "x";
    std::cerr << "\n[Soft work profile] substep " << substepName(substep)
              << " tied-for-slowest";
    for (size_t thread = 0; thread < numThreads; ++thread) {
      std::cerr << " t" << thread << '=' << std::fixed
                << std::setprecision(1)
                << (100.0 * slowestCount[thread] / completeCycles) << '%';
    }
    std::cerr << '\n';
  }
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
    printWorkProfile();
  }
  return true;
}

void SoftSystemBackend::worker(size_t threadId) {
  uint64_t cycle = 0;
  while (true) {
    if (halted) return;
    ++cycle;
    const bool profile = workProfileSampleCycle(cycle);
    if (profile) {
      const uint64_t now = workProfileNowNs();
      workProfileMark(threadId, WorkProfilePhase::StageStart, now, now);
    }
    stageStart.arrive_and_wait();
    try {
      stageWorkResolve(threadId, profile);
    } catch (const std::exception &e) {
      std::cerr << "Exception at thread " << threadId << ": " << e.what() << std::endl;
      std::exit(1);
    }
    if (profile) {
      const uint64_t now = workProfileNowNs();
      workProfileMark(threadId, WorkProfilePhase::StageDone, now, now);
    }
    stageDone.arrive_and_wait();
    if (profile) {
      const uint64_t now = workProfileNowNs();
      workProfileMark(threadId, WorkProfilePhase::StepStart, now, now);
    }
    stepStart.arrive_and_wait();
    try {
      stepWork(threadId, profile);
    } catch (const std::exception &e) {
      std::cerr << "Exception at thread " << threadId << ": " << e.what() << std::endl;
      std::exit(1);
    }
    if (profile) {
      const uint64_t now = workProfileNowNs();
      workProfileMark(threadId, WorkProfilePhase::StepDone, now, now);
    }
    stepDone.arrive_and_wait();
    try {
      stepCleanup(threadId, profile);
    } catch (const std::exception &e) {
      std::cerr << "Exception at thread " << threadId << ": " << e.what() << std::endl;
      std::exit(1);
    }
  }
}
