/**
 * Software NoC system simulator.
 *
 * Method definitions and topology construction for SoftSystemBackend.
 * The class declaration lives in soft_backend.h.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "soft_backend.h"
#include "soft_components.h"
#include "soft_mem.h"

using namespace std;

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
    pu_memif(num_pu),
    pu_memif_inject(num_pu)
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
      if (port_idx == 0)
        pu_memif_inject[cpu] = static_cast<uint16_t>(memif_idx);
    }
  }

  // The peripheral memif (memif index 0) lives at PU 1. If PU 1 also
  // happens to be an MC connection PU for some MC, that conflict was
  // already caught above. Periph wins for PU 1 (overwrite is not
  // allowed by the throws above).
  if (pu_memif[1].has_value()) throw std::logic_error("Conflicting memif assignment for PU 1");
  pu_memif[1] = std::make_pair<uint16_t, uint8_t>(0, 0);
  pu_memif_inject[1] = 0;
}

// ===========================================================================
// SoftSystemBackend
// ===========================================================================

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
  links.memif_inject = topo.pu_memif_inject[pu];

  const size_t nf = my_dirs.size();
  size_t num_in  = nf + 1 + (links.memif_inject ? 1 : 0);
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
  return WrappedRouter(std::move(links), std::move(tbl), num_in, num_out,
                       soft_noc::spaceMultiplier());
}

soft_mem::MemIf &SoftSystemBackend::memifAt(size_t memifIdx) {
  return memifIdx == 0 ? static_cast<soft_mem::MemIf &>(periph)
                       : static_cast<soft_mem::MemIf &>(drams.at(memifIdx - 1));
}

bool SoftSystemBackend::memifNocAccepted(size_t memifIdx) const {
  const uint16_t pu = topo.memif_req_at.at(memifIdx).front();
  const auto &router = routers[pu];
  if constexpr (ASSERTIONS_ENABLED) {
    if (router.links.memif_inject != memifIdx)
      throw std::logic_error("MemIf injection router mapping mismatch");
  }
  return router.buf.memif_inject_accepting;
}

std::unique_ptr<Flit> SoftSystemBackend::takeMemifFlit(size_t memifIdx) {
  if (!memif_eject_accept[memifIdx]) return nullptr;
  const uint8_t port = *memif_eject_accept[memifIdx];
  const uint16_t pu = topo.memif_req_at[memifIdx][port];
  return routers[pu].rt.take(*memif_eject[memifIdx][port].token);
}

bool SoftSystemBackend::ringCanInject(size_t memifIdx) const {
  const size_t next = (memifIdx + 1) % (cfg.numMC + 1);
  return !ring.validAt(next) &&
         (!ring.validAt(memifIdx) || memif_ring_accept[memifIdx]);
}

bool SoftSystemBackend::ringOutputEmpty(size_t memifIdx) const {
  // Slots are indexed at the receiving MemIf. The next slot is this
  // MemIf's registered forwarding-queue output (System.scala ring wiring).
  return !ring.validAt((memifIdx + 1) % (cfg.numMC + 1));
}

SoftSystemBackend::SoftSystemBackend(SystemConfig cfg_in, size_t threads,
                                     soft_mem::ExperimentConfig experiment,
                                     size_t idleDelayCycles)
  : memoryExperiment(soft_mem::checkedExperimentConfig(experiment)),
    cfg(std::move(cfg_in)),
    idleDelay(idleDelayCycles),
    releaseResetAfter(10),
    topo(cfg.numPU, cfg.numMC),
    periph(0, 16, [&]() {
      // Build width-aligned config-ROM lines matching RTL System.scala.
      std::unordered_map<uint32_t, MemLine> rom;
      uint32_t npu = static_cast<uint32_t>(cfg.numPU);
      uint32_t nmc = static_cast<uint32_t>(cfg.numMC);
      uint32_t ppm = nmc ? npu / nmc : 0;
      uint32_t lineShift = MEM_BUS_WIDTH_SIZE;
      uint64_t mcSize = cfg.core.mcSizes.empty() ? 0 : cfg.core.mcSizes[0];
      auto storeField = [&rom](uint32_t offset, const auto &value) {
        const size_t lineOffset = offset & MEM_ADDR_OFFSET_MASK;
        if (lineOffset + sizeof(value) > MEM_BUS_WIDTH_B)
          throw std::logic_error("Configuration ROM field crosses a memory line");
        const uint32_t address = 0x28000000u + (offset & MEM_ADDR_ALIGN_MASK);
        auto it = rom.try_emplace(address, MemLine{}).first;
        std::memcpy(it->second.data() + lineOffset, &value, sizeof(value));
      };
      storeField(0, npu);
      storeField(8, nmc);
      storeField(16, ppm);
      storeField(24, lineShift);
      storeField(32, mcSize);
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
    memif_step_decisions(cfg.numMC + 1),
    memif_owner(cfg.numMC + 1),
    thread_memifs(threads),
    thread_bcst_accept(threads,
                       std::vector<uint16_t>(cfg.numPU /
                                             soft_mem::CLUSTER_SIZE)),
    dram_bcst_accept(
        cfg.numMC,
        std::vector<uint16_t>((cfg.numPU / cfg.numMC) /
                              soft_mem::CLUSTER_SIZE)),
    routers(cfg.numPU,
            [this](size_t i) { return buildRouter(static_cast<uint16_t>(i)); }),
    numThreads(threads),
    stageStart(numThreads),
    stageDone(numThreads),
    stepStart(numThreads),
    stepDone(numThreads)
{
  // Initialize per-MC DRAMIfs. PU range is contiguous in PU-id order.
  if (cfg.numPU % cfg.numMC != 0) throw std::invalid_argument("numPU must be divisible by numMC");
  if ((cfg.numPU / cfg.numMC) % soft_mem::CLUSTER_SIZE != 0) throw std::invalid_argument("PU per MC must be a multiple of cluster size");
  if (numThreads < 1 || numThreads > cfg.numPU) throw std::invalid_argument("numThreads must be between 1 and numPU");

  const size_t PU_PER_MC = cfg.numPU / cfg.numMC;
  const size_t CLUSTER_PER_MC = PU_PER_MC / soft_mem::CLUSTER_SIZE;
  std::cerr << "[Soft NoC] space_multiplier=" << soft_noc::spaceMultiplier()
            << " router_queue_depth=" << ROUTER_Q_DEPTH
            << " idle_delay=" << idleDelayCycles << '\n';
  std::cerr << "[Soft MemIf] slots=" << memoryExperiment.slots
            << " merge=" << (memoryExperiment.merge ? "on" : "off")
            << " allocation=" << soft_mem::allocationName(memoryExperiment.allocation)
            << " external_ids=" << memoryExperiment.slots
            << " id_mapping=direct unicast_reserve=" << memoryExperiment.unicastReserve
            << " bulk_classes=all bulk_reservation="
            << (memoryExperiment.allocation == soft_mem::AllocationMode::Sequential
                ? "consecutive-slots" : "free-slots") << '\n';
  drams.reserve(cfg.numMC);
  for (size_t m = 0; m < cfg.numMC; ++m) {
    drams.emplace_back(m + 1, CLUSTER_PER_MC, 2,
                       1 + m * PU_PER_MC,
                       1 + (m + 1) * PU_PER_MC, memoryExperiment);
  }

  memif_owner[0] = puOwner(1);
  thread_memifs[memif_owner[0]].push_back(0);
  for (size_t m = 0; m < cfg.numMC; ++m) {
    const size_t memifIdx = m + 1;
    const size_t domainFirstPU = 1 + m * PU_PER_MC;
    memif_owner[memifIdx] = puOwner(domainFirstPU);
    thread_memifs[memif_owner[memifIdx]].push_back(memifIdx);
  }
  for (size_t thread = 0; thread < numThreads; ++thread)
    prepareNextCoreMem(thread);

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

std::vector<SpmEndpoint> SoftSystemBackend::spms() {
  std::vector<SpmEndpoint> result;
  for (size_t pu = 1; pu <= cfg.numPU; ++pu)
    result.push_back(SpmEndpoint::create(*cores[pu], pu, "Core.exec.lsu.spm"));
  return result;
}

std::vector<ICacheEndpoint> SoftSystemBackend::icaches() {
  std::vector<ICacheEndpoint> result;
  for (size_t pu = 1; pu <= cfg.numPU; ++pu)
    result.push_back(ICacheEndpoint::create(*cores[pu], "Core.fetch.icache"));
  return result;
}

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
    core.mem_unicast_bits_id = resp.unicast->tag;
    core.mem_unicast_bits_ident = resp.unicast->ident;
    core.mem_unicast_bits_extra0 = resp.unicast->extra0;
    core.mem_unicast_bits_extra1 = resp.unicast->extra1;
    // The Verilated wide value and MemLine have the same byte layout.
    static_assert(sizeof(core.mem_unicast_bits_data) == sizeof(resp.unicast->data));
    std::memcpy(&core.mem_unicast_bits_data, resp.unicast->data.data(),
                sizeof(resp.unicast->data));
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
#define BCAST_SLOT(i)                                                        \
    core.mem_broadcast_bits_line_##i##_data = raw[2 * i + 1];                \
    core.mem_broadcast_bits_line_##i##_idx = (uint16_t)(raw[2 * i] >> 16);  \
    core.mem_broadcast_bits_line_##i##_pu = (uint16_t)(raw[2 * i] & 0xFFFF)
    SYSTEM_BCAST_SLOT_ENTRIES
#undef BCAST_SLOT
  }
}

void SoftSystemBackend::peek(uint64_t cycle, std::vector<MemBusOut *> out) {
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
  if constexpr (ASSERTIONS_ENABLED) {
    if (in.size() != cfg.numMC + 1)
      throw std::invalid_argument("stage: input size mismatch");
  }
  // Publish the frontend inputs together with completion of the preceding
  // cleanup before any owner thread starts its MemIf work.
  memif_ext = in;

  // Publish completion of the preceding cleanup before any thread reads a
  // neighboring router's committed queues.
  stageStart.arrive_and_wait();
  try {
    if constexpr (ASSERTIONS_ENABLED) {
      if (cycle <= releaseResetAfter) {
        for (const auto &i : in) {
          if (i.resp) throw std::runtime_error("Memory response presented during reset");
        }
      }
    }

    stageWorkResolve(0);
    stageMemifWork(0);
  } catch (const std::exception &e) {
    std::cerr << "Exception at thread " << 0 << ": " << e.what() << std::endl;
    std::exit(1);
  }

  stageDone.arrive_and_wait();

  // All cores are settled and every worker is waiting at stepStart. Observe
  // pre-edge state here, before any core, router, or MemIf can commit.
  rawSystemIdle = true;
  for (size_t pu = 1; pu <= cfg.numPU; ++pu)
    rawSystemIdle = rawSystemIdle && cores[pu]->idle &&
                    routers[pu].rt.totalFlits() == 0;
  for (size_t mc = 0; mc < cfg.numMC; ++mc)
    rawSystemIdle = rawSystemIdle && drams[mc].idle(ringOutputEmpty(mc + 1)) &&
                    drams[mc].distributorsIdle();
  delayedSystemIdle = idleDelay.output(rawSystemIdle);
}

void SoftSystemBackend::stageWorkResolve(size_t threadId) {
  auto [puStart, puEnd] = puWorkRange(threadId);

  // Resolve each incoming transfer from committed source-router state. Router
  // queues are read-only throughout stage and are not committed until step.
  for (uint16_t i = puStart; i < puEnd; ++i) {
    auto &local = routers[i];
    const size_t nf = local.links.num_forwards;
    local.buf.core_inject_presenting = peekCoreInjectPrio(*cores[i]);
    local.buf.core_inject_accepting =
        local.rt.prepareEnq(nf, local.buf.core_inject_presenting);
    if (local.links.memif_inject) {
      local.buf.memif_inject_presenting =
          memifAt(*local.links.memif_inject).peekNoc();
      local.buf.memif_inject_accepting = local.rt.prepareEnq(
          nf + 1,
          local.buf.memif_inject_presenting
              ? std::optional<uint8_t>(
                    local.buf.memif_inject_presenting->prio())
              : std::nullopt);
    } else {
      local.buf.memif_inject_presenting = std::nullopt;
      local.buf.memif_inject_accepting = false;
    }
    for (size_t o = 0; o < nf; ++o) {
      auto [from, port] = *local.links.forwards[o];
      auto &token = local.buf.forward_presenting[o];
      token = routers[from].rt.peekToken(port);
      local.buf.forward_accepting[o] = local.rt.prepareEnq(
          o, token ? std::optional<uint8_t>(token->prio) : std::nullopt);
    }
    local.buf.core_eject_presenting = local.rt.peekToken(nf);
  }

  // Drive PU inputs and settle on the negedge. Contract: accept signals
  // from the cores are read post-eval (after combinational settle).
  for (uint16_t i = puStart; i < puEnd; ++i) {
    auto &router = routers[i];
    const Flit *core_eject = router.buf.core_eject_presenting
        ? router.rt.peek(*router.buf.core_eject_presenting) : nullptr;
    presentCoreEject(*cores[i], i, core_eject);
    presentCoreMem(*cores[i], core_mem[i]);
    cores[i]->ext_out_ready = router.buf.core_inject_accepting;
    cores[i]->clock = false; // negedge
  }
  for (uint16_t i = puStart; i < puEnd; ++i) {
    cores[i]->eval();
  }
  for (uint16_t i = puStart; i < puEnd; ++i) {
    routers[i].buf.core_eject_accepting =
        routers[i].buf.core_eject_presenting.has_value() &&
        cores[i]->ext_in_ready;
    core_mem_accept[i].unicast = cores[i]->mem_unicast_valid;
    core_mem_accept[i].broadcast =
        cores[i]->mem_broadcast_ready && cores[i]->mem_broadcast_valid;
  }

  auto &threadMasks = thread_bcst_accept[threadId];
  std::fill(threadMasks.begin(), threadMasks.end(), 0);
  for (uint16_t i = puStart; i < puEnd; ++i) {
    if (!core_mem_accept[i].broadcast) continue;
    const size_t puZero = i - 1;
    threadMasks[puZero / soft_mem::CLUSTER_SIZE] |=
        static_cast<uint16_t>(1u << (puZero % soft_mem::CLUSTER_SIZE));
  }
}

void SoftSystemBackend::stageMemifWork(size_t threadId) {
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

  for (const size_t memifIdx : thread_memifs[threadId]) {
    resolveMemifTokens(memifIdx);
    auto *ringInput = ring.peekAt(memifIdx);
    auto &memif = memifAt(memifIdx);
    memif.prepareExternal(memif_ext[memifIdx].reqAccepting);
    memif_ring_accept[memifIdx] =
        ringInput && memif.ringCanAccept(*ringInput);
    memif_eject_accept[memifIdx] = memif.nocAcceptMultiple(
        [&peekMemifFlit, memifIdx](size_t port) -> const Flit * {
          return peekMemifFlit(memifIdx, port);
        });
  }
}

void SoftSystemBackend::prepareNextCoreMem(size_t threadId) {
  const auto [threadStart, threadEnd] = puWorkRange(threadId);
  const size_t PU_PER_MC = cfg.numPU / cfg.numMC;
  for (size_t m = 0; m < cfg.numMC; ++m) {
    const size_t domainStart = m * PU_PER_MC + 1;
    const size_t domainEnd = (m + 1) * PU_PER_MC + 1;
    const size_t rangeStart = std::max(threadStart, domainStart);
    const size_t rangeEnd = std::min(threadEnd, domainEnd);
    if (rangeStart >= rangeEnd) continue;
    auto bufferSpan = core_mem.slice(rangeStart, rangeEnd);
    // This snapshot is consumed by the cores during the next iteration's
    // stage, after the current DRAMIf and ring state has committed.
    drams[m].peekPUs(rangeStart, bufferSpan);
  }
}

void SoftSystemBackend::step(uint64_t cycle) {
  resetReleaseNow = cycle == releaseResetAfter;

  stepStart.arrive_and_wait();
  try {
    stepWork(0);
    stepPeriphWork(0);
    stepMemifWork(0);
  } catch (const std::exception &e) {
    std::cerr << "Exception at thread " << 0 << ": " << e.what() << std::endl;
    std::exit(1);
  }

  stepDone.arrive_and_wait();
  idleDelay.step(rawSystemIdle, cycle <= releaseResetAfter);

  // The RTL holds every MemIf idle-notification register in reset through
  // this edge. Other soft MemIf state cannot change while requests are gated.
  if (cycle <= releaseResetAfter) {
    for (size_t memifIdx = 0; memifIdx < cfg.numMC + 1; ++memifIdx)
      memifAt(memifIdx).resetIdle();
  }

  for (size_t memifIdx = 0; memifIdx < memif_step_decisions.size();
       ++memifIdx) {
    ring.updateValidAt(memifIdx,
                       memif_step_decisions[memifIdx].ring_injected,
                       memif_ring_accept[memifIdx]);
  }
  ring.progress();

  prepareNextCoreMem(0);
  stepCleanup(0);
}

void SoftSystemBackend::stepPeriphWork(size_t threadId) {
  if (memif_owner[0] != threadId) return;

  bool &injected = memif_step_decisions[0].ring_injected;
  injected = false;
  auto acceptedFlit = takeMemifFlit(0);
  std::optional<std::pair<std::reference_wrapper<const Flit>, uint8_t>>
      flitView;
  if (acceptedFlit) flitView = {{*acceptedFlit, *memif_eject_accept[0]}};

  periph.step(
      soft_mem::RingIntf {
        .buffer = &ring[0],
        .ingress = ring.validAt(0),
        .eject = static_cast<bool>(memif_ring_accept[0]),
        .canInject = ringCanInject(0),
        .injected = &injected,
        .outputEmpty = ringOutputEmpty(0),
      },
      memif_ext[0], flitView, memifNocAccepted(0), delayedSystemIdle);
}

void SoftSystemBackend::stepMemifWork(size_t threadId) {
  const size_t clustersPerMC =
      (cfg.numPU / cfg.numMC) / soft_mem::CLUSTER_SIZE;

  for (const size_t memifIdx : thread_memifs[threadId]) {
    if (memifIdx == 0) continue;
    bool &injected = memif_step_decisions[memifIdx].ring_injected;
    injected = false;
    auto acceptedFlit = takeMemifFlit(memifIdx);
    std::optional<std::pair<std::reference_wrapper<const Flit>, uint8_t>>
        flitView;
    if (acceptedFlit)
      flitView = {{*acceptedFlit, *memif_eject_accept[memifIdx]}};

    soft_mem::RingIntf ringIntf {
      .buffer = &ring[memifIdx],
      .ingress = ring.validAt(memifIdx),
      .eject = static_cast<bool>(memif_ring_accept[memifIdx]),
      .canInject = ringCanInject(memifIdx),
      .injected = &injected,
      .outputEmpty = ringOutputEmpty(memifIdx),
    };

    const size_t m = memifIdx - 1;
    auto &acceptedMasks = dram_bcst_accept[m];
    std::fill(acceptedMasks.begin(), acceptedMasks.end(), 0);
    const size_t firstCluster = m * clustersPerMC;
    for (size_t thread = 0; thread < numThreads; ++thread) {
      for (size_t dist = 0; dist < clustersPerMC; ++dist) {
        acceptedMasks[dist] |=
            thread_bcst_accept[thread][firstCluster + dist];
      }
    }
    drams[m].step(ringIntf, memif_ext[memifIdx], flitView,
                  std::span<const uint16_t>(acceptedMasks),
                  memifNocAccepted(memifIdx));
  }
}

void SoftSystemBackend::stepWork(size_t threadId) {
  auto [puStart, puEnd] = puWorkRange(threadId);

  // Materialize accepted core-inject flits from RTL BEFORE the posedge (the
  // posedge consumes ext_out), then posedge-eval every core.
  for (size_t i = puStart; i < puEnd; ++i) {
    auto &router = routers[i];
    if (router.buf.core_inject_accepting)
      router.rt.place(router.links.num_forwards,
          std::make_unique<Flit>(peekCoreInjectFlit(*cores[i], static_cast<uint16_t>(i))));
    if (router.buf.memif_inject_accepting)
      router.rt.place(router.links.num_forwards + 1,
          std::make_unique<Flit>(*router.buf.memif_inject_presenting));

    cores[i]->clock = true;
    cores[i]->eval();

    if (resetReleaseNow) {
      cores[i]->reset = false;
      cores[i]->eval();
    }
  }

  // Each destination router moves accepted flits directly from source slots.
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
}

void SoftSystemBackend::stepCleanup(size_t threadId) {
  auto [puStart, puEnd] = puWorkRange(threadId);
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
}

void SoftSystemBackend::worker(size_t threadId) {
  while (true) {
    if (halted) return;
    stageStart.arrive_and_wait();
    try {
      stageWorkResolve(threadId);
      stageMemifWork(threadId);
    } catch (const std::exception &e) {
      std::cerr << "Exception at thread " << threadId << ": " << e.what() << std::endl;
      std::exit(1);
    }
    stageDone.arrive_and_wait();
    stepStart.arrive_and_wait();
    try {
      stepWork(threadId);
      stepPeriphWork(threadId);
      stepMemifWork(threadId);
    } catch (const std::exception &e) {
      std::cerr << "Exception at thread " << threadId << ": " << e.what() << std::endl;
      std::exit(1);
    }
    stepDone.arrive_and_wait();
    try {
      prepareNextCoreMem(threadId);
      stepCleanup(threadId);
    } catch (const std::exception &e) {
      std::cerr << "Exception at thread " << threadId << ": " << e.what() << std::endl;
      std::exit(1);
    }
  }
}
