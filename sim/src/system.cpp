/**
 * System-level simulation: frontend + CLI entry point.
 *
 * Contains the System class (frontend) that drives one or more SystemBackend
 * instances (soft NoC or hard RTL), serving memory requests through the mem()
 * interface with flat or DRAMsim3 backing memory.  When multiple backends are
 * attached, they run in lockstep (cosimulation) with discrepancy detection.
 *
 * Also contains main() for the sim_system binary.
 *
 * Source layout:
 *   sim/src/system.h          SystemBackend interface, System class declaration
 *   sim/src/system.cpp         This file (frontend implementation + main)
 *   sim/src/soft_backend.h     Soft NoC backend (SoftSystemModel) declaration
 *   sim/src/soft_backend.cpp   Soft NoC backend implementation
 *   sim/src/single.cpp         Single-core simulation driver (sim_single)
 *   sim/src/devices.h          PeripheralDevice
 *   sim/gen_system_header.py   Generates hard_backend.h + system_config.h
 *
 * Usage:
 *   sim_system <image>... --soft|--hard [options]
 *
 * Positional:
 *   image...            One binary memory image per MC (count must match hardware)
 *
 * Options:
 *   --soft              Use soft NoC backend
 *   --hard              Use hard RTL backend
 *   --max-cycles N      Max simulation cycles (default: 10000000)
 *   --dram-config PATH  DRAMsim3 config file
 *   --dram-log DIR      DRAMsim3 log directory (default: ".")
 *   --trace             Enable FST tracing
 *   --log               Enable verbose logging
 *   --rng-seed N        RNG seed for peripheral device
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <signal.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/argparse.h"
#include "system.h"
#include "devices.h"
#include "soft_backend.h"
#include "hard_backend.h"
#include "dramsim3/dramsim3.h"

using namespace std;

static constexpr int MEM_BUS_WORDS = MEM_BUS_WIDTH / 32;
static constexpr int RESP_QUEUE_DEPTH = 64;

static bool exiting = false;
static void sighandler(int) { exiting = true; }

// ---- Per-MC memory images (owned by the frontend) ----
struct McImage {
  std::unordered_map<uint32_t, mem_line_t> data;
  std::size_t size; // This is the total size, according to hardware config, not the image size

  GlobalMemResp handle(const GlobalMemReq &req) {
    if ((1 << req.size) > MEM_BUS_WIDTH / 8) {
      throw runtime_error("Memory request size exceeds bus width");
    }

    uint32_t aligned_addr = req.addr & ~(MEM_BUS_WIDTH / 8 - 1);
    if (aligned_addr >= size) {
      throw runtime_error("Memory request address out of bounds");
    }

    if (aligned_addr % (1 << req.size) != 0) {
      throw runtime_error("Memory request address not aligned");
    }

    GlobalMemResp resp;
    // Readout
    resp.id = req.id;
    mem_line_t readout = data.contains(aligned_addr) ? data.at(aligned_addr) : mem_line_t {};
    resp.data = readout;

    if (req.write) {
      uint32_t subline_addr = req.addr - aligned_addr;
      mem_mask_t mask = ((((uint64_t) 1) << (1 << req.size)) - 1) << subline_addr; // Byte-enables
      mask &= req.wbe;

      for (size_t i = 0; i < MEM_BUS_WIDTH / 8; ++i) {
        if (mask & (1 << i)) {
          readout[i] = req.wdata[i];
        }
      }
      data[aligned_addr] = readout;
    }

    return resp;
  }
};

static vector<McImage> mc_images;

static void loadImages(const vector<string> &paths, const vector<uint64_t> &mc_sizes) {
  mc_images.resize(paths.size());
  for (size_t i = 0; i < paths.size(); ++i) {
    ifstream fin(paths[i], ios::binary);
    if (!fin) throw runtime_error("Cannot open " + paths[i]);
    fin.seekg(0, ios::end);
    size_t fsize = fin.tellg();
    fin.seekg(0);

    mc_images[i].size = (i < mc_sizes.size()) ? (size_t)mc_sizes[i] : fsize;

    // Read file in 32-byte chunks into the unordered_map
    constexpr size_t LINE_BYTES = MEM_BUS_WIDTH / 8;
    mem_line_t line{};
    for (size_t offset = 0; offset < fsize; offset += LINE_BYTES) {
      size_t to_read = min(LINE_BYTES, fsize - offset);
      line = {};
      fin.read(reinterpret_cast<char *>(line.data()), to_read);
      // Only store non-zero lines
      bool all_zero = true;
      for (size_t b = 0; b < LINE_BYTES; ++b) {
        if (line[b] != 0) { all_zero = false; break; }
      }
      if (!all_zero) {
        mc_images[i].data[(uint32_t)offset] = line;
      }
    }
  }
}

// ---- Per-MC state in the frontend ----
struct McState {
  uint64_t mc_base = 0;      // cumulative offset for this MC in global space
  deque<GlobalMemResp> resp_queue;
};

struct DramMC {
  dramsim3::MemorySystem *dram = nullptr;
  struct Pending { uint64_t addr; bool is_write; GlobalMemResp resp; };
  deque<Pending> submit_queue;
  multimap<uint64_t, GlobalMemResp> inflight;
};

// ---- System implementation ----
struct System::Impl {
  vector<unique_ptr<SystemBackend>> backends;
  PeripheralDevice periph;
  deque<GlobalMemResp> periph_resps;

  int num_mc = 0;
  vector<McState> mc_states;

  // DRAMsim3
  bool use_dram = false;
  vector<DramMC> dram_mcs;

  // Image paths (one per MC)
  vector<string> image_paths;

  // Bus arrays (numMC + 1 entries, index 0 = peripheral)
  vector<MemBusIn> bus_in;

  // Deferred DRAMsim3 config
  struct StoredDRAMConfig {
    string configFile;
    string workDir;
  };
  optional<StoredDRAMConfig> dram_cfg;

  void initMcBases(const SystemConfig &cfg) {
    num_mc = cfg.numMC;
    mc_states.resize(num_mc);
    uint64_t cumul = 0;
    for (int i = 0; i < num_mc && i < (int)cfg.core.mcSizes.size(); ++i) {
      mc_states[i].mc_base = cumul;
      cumul += cfg.core.mcSizes[i];
    }
    bus_in.resize(num_mc + 1);
  }

  void initDram(const StoredDRAMConfig &dcfg) {
    use_dram = true;
    dram_mcs.resize(num_mc);
    for (int i = 0; i < num_mc; ++i) {
      auto log = dcfg.workDir + "/" + to_string(i);
      filesystem::create_directories(log);
      dram_mcs[i].dram = new dramsim3::MemorySystem(
        dcfg.configFile.c_str(), log.c_str(),
        [this, i](uint64_t addr) { dramReadCb(i, addr); },
        [this, i](uint64_t addr) { dramWriteCb(i, addr); }
      );
    }
  }

  void dramReadCb(int mc, uint64_t addr) {
    auto &d = dram_mcs[mc];
    auto it = d.inflight.find(addr);
    if (it != d.inflight.end()) {
      mc_states[mc].resp_queue.push_back(it->second);
      d.inflight.erase(it);
    }
  }

  void dramWriteCb(int mc, uint64_t addr) {
    auto &d = dram_mcs[mc];
    auto it = d.inflight.find(addr);
    if (it != d.inflight.end()) {
      mc_states[mc].resp_queue.push_back(it->second);
      d.inflight.erase(it);
    }
  }

  // Serve a single request from a backend for MC mc
  void serveMemReq(int mc, const GlobalMemReq &req) {
    GlobalMemResp resp = mc_images[mc].handle(req);

    if (use_dram) {
      dram_mcs[mc].submit_queue.push_back({(uint64_t)req.addr, req.write, resp});
    } else {
      mc_states[mc].resp_queue.push_back(resp);
    }
  }

  void servePeriphReq(const GlobalMemReq &req) {
    GlobalMemResp resp{};
    resp.id = req.id;
    resp.data = {};

    size_t byte_offset = req.addr & ((MEM_BUS_WORDS * sizeof(uint32_t)) - 1);
    size_t word_idx = byte_offset / sizeof(uint32_t);

    if (req.write) {
      uint32_t wdata;
      memcpy(&wdata, req.wdata.data() + byte_offset, sizeof(uint32_t));
      periph.write(req.addr, wdata);
    } else {
      uint32_t rdata = periph.read(req.addr);
      memcpy(resp.data.data() + word_idx * sizeof(uint32_t), &rdata, sizeof(uint32_t));
    }
    periph_resps.push_back(resp);
  }

  // Pop consumed responses and tick DRAM
  void tickMemory() {
    periph.tick();

    if (use_dram) {
      for (int mc = 0; mc < num_mc; ++mc) {
        auto &d = dram_mcs[mc];
        d.dram->ClockTick();
        while (!d.submit_queue.empty()) {
          auto &req = d.submit_queue.front();
          if (d.dram->WillAcceptTransaction(req.addr, req.is_write)) {
            d.dram->AddTransaction(req.addr, req.is_write);
            d.inflight.emplace(req.addr, req.resp);
            d.submit_queue.pop_front();
          } else break;
        }
      }
    }
  }

  // Per-backend bus_out arrays for cosim comparison
  vector<vector<MemBusOut>> per_backend_out;

  // Run mem() on all backends with the same bus_in, verify requests match, serve once.
  void memInteractAll(uint64_t cycle) {
    int num_ports = num_mc + 1;
    size_t n = backends.size();

    // --- Build bus_in (shared across all backends) ---

    // Peripheral (idx 0)
    bus_in[0].reqAccepting = periph_resps.size() < RESP_QUEUE_DEPTH;
    if (!periph_resps.empty()) {
      bus_in[0].resp = periph_resps.front();
    } else {
      bus_in[0].resp = nullopt;
    }

    // MC ports (idx 1..numMC)
    for (int mc = 0; mc < num_mc; ++mc) {
      int idx = mc + 1;
      auto &ms = mc_states[mc];
      bus_in[idx].reqAccepting = ms.resp_queue.size() < RESP_QUEUE_DEPTH;
      if (!ms.resp_queue.empty()) {
        bus_in[idx].resp = ms.resp_queue.front();
      } else {
        bus_in[idx].resp = nullopt;
      }
    }

    // --- Call each backend's mem() with the same bus_in ---
    per_backend_out.resize(n);
    for (size_t bi = 0; bi < n; ++bi) {
      per_backend_out[bi].resize(num_ports);
      backends[bi]->mem(bus_in.data(), per_backend_out[bi].data());
    }

    // --- Cosim: verify all backends agree on requests ---
    if (n > 1) {
      for (int p = 0; p < num_ports; ++p) {
        bool first_has = per_backend_out[0][p].req.has_value();
        for (size_t bi = 1; bi < n; ++bi) {
          bool cur_has = per_backend_out[bi][p].req.has_value();
          if (first_has != cur_has) {
            cerr << "[System] COSIM MISMATCH @ cycle " << dec << cycle
                 << ": port " << p
                 << " backend 0 req=" << first_has
                 << " backend " << bi << " req=" << cur_has << endl;
            exiting = true;
            return;
          }
          if (first_has && cur_has) {
            auto &a = *per_backend_out[0][p].req;
            auto &b = *per_backend_out[bi][p].req;
            if (a.addr != b.addr || a.write != b.write) {
              cerr << "[System] COSIM MISMATCH @ cycle " << dec << cycle
                   << ": port " << p
                   << " addr 0x" << hex << a.addr << " vs 0x" << b.addr
                   << " write " << a.write << " vs " << b.write
                   << dec << endl;
              exiting = true;
              return;
            }
          }
        }
      }
    }

    // --- Pop consumed responses (once, not per-backend) ---
    if (bus_in[0].resp) periph_resps.pop_front();
    for (int mc = 0; mc < num_mc; ++mc) {
      if (bus_in[mc + 1].resp) mc_states[mc].resp_queue.pop_front();
    }

    // --- Serve requests (once, using backend 0's output) ---
    auto &out = per_backend_out[0];
    if (out[0].req && bus_in[0].reqAccepting) {
      servePeriphReq(*out[0].req);
    }
    for (int mc = 0; mc < num_mc; ++mc) {
      int idx = mc + 1;
      if (out[idx].req && bus_in[idx].reqAccepting) {
        serveMemReq(mc, *out[idx].req);
      }
    }
  }
};

System::System(
  vector<string_view> dramInitFiles,
  optional<DRAMsim3Config> dramTimingModel
) : impl_(make_unique<Impl>()) {
  for (auto &f : dramInitFiles) {
    impl_->image_paths.push_back(string(f));
  }
  if (dramTimingModel) {
    impl_->dram_cfg = Impl::StoredDRAMConfig{
      string(dramTimingModel->configFile),
      string(dramTimingModel->workDir)
    };
  }
}

System::~System() = default;

void System::addBackend(unique_ptr<SystemBackend> backend) {
  impl_->backends.push_back(move(backend));
}

bool System::run(uint64_t maxCycles) {
  if (impl_->backends.empty()) {
    cerr << "[System] Error: no backends added" << endl;
    return false;
  }

  // Initialize MC bases from first backend's config
  auto cfg = impl_->backends[0]->config();
  impl_->initMcBases(cfg);

  cerr << "[System] PUs=" << cfg.numPU << " MCs=" << cfg.numMC << endl;

  // Load per-MC images
  loadImages(impl_->image_paths, cfg.core.mcSizes);
  cerr << "[System] Loaded " << mc_images.size() << " memory image(s)" << endl;

  // DRAMsim3
  if (impl_->dram_cfg) {
    impl_->initDram(*impl_->dram_cfg);
    cerr << "[System] DRAMsim3 enabled" << endl;
  }

  // Signal handling
  struct sigaction sig;
  sig.sa_handler = sighandler;
  sigemptyset(&sig.sa_mask);
  sig.sa_flags = 0;
  sigaction(SIGINT, &sig, nullptr);

  cerr << "[System] Running (max " << maxCycles << " cycles)..." << endl;
  auto wall_start = chrono::steady_clock::now();

  uint64_t cycle = 0;
  uint64_t mc_req_count = 0, periph_req_count = 0;

  while (!impl_->periph.result && cycle < maxCycles && !exiting) {
    ++cycle;

    // Step all backends
    for (auto &b : impl_->backends) b->step(cycle);

    // Tick frontend memory
    impl_->tickMemory();

    // Call all backends' mem() with the same bus_in, verify requests match, serve once
    impl_->memInteractAll(cycle);

    // Count requests for heartbeat
    auto &out = impl_->per_backend_out[0];
    if (out[0].req) periph_req_count++;
    for (int mc = 0; mc < impl_->num_mc; ++mc)
      if (out[mc+1].req) mc_req_count++;

    if (cycle % 500000 == 0) {
      fprintf(stderr, "[HEARTBEAT %lu] mc_reqs=%lu periph_reqs=%lu periph_resps_pending=%zu mc_resps_pending=%zu\n",
              cycle, mc_req_count, periph_req_count,
              impl_->periph_resps.size(),
              impl_->mc_states.empty() ? 0 : impl_->mc_states[0].resp_queue.size());
    }

  }

  auto wall_end = chrono::steady_clock::now();
  double wall_secs = chrono::duration<double>(wall_end - wall_start).count();

  if (impl_->periph.result) {
    cerr << "[System] Result: " << dec << *impl_->periph.result
         << " (0x" << hex << *impl_->periph.result << ")" << endl;
    cerr << "[System] Cycles: " << dec << cycle << endl;
  } else {
    cerr << "[System] " << (exiting ? "Interrupted" : "Timed out")
         << " at cycle " << cycle << endl;
  }
  if (impl_->periph.timer_count > 0) {
    cerr << "[System] Timer: " << dec << impl_->periph.timer_count << " cycles" << endl;
  }
  cerr << "[System] Speed: " << dec << (uint64_t)(cycle / wall_secs)
       << " cycles/s" << endl;
  cerr << "[System] Runtime: " << fixed << setprecision(3) << wall_secs << "s" << endl;

  // Print backend stats
  for (auto &b : impl_->backends) {
    b->printStats(cycle, true);
  }

  // Print DRAM stats
  if (impl_->use_dram) {
    for (int i = 0; i < impl_->num_mc; ++i)
      impl_->dram_mcs[i].dram->PrintStats();
  }

  mc_images.clear();

  return impl_->periph.result && *impl_->periph.result == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  argparse::ArgumentParser program("sim_system");

  program.add_argument("images")
    .help("Binary memory images (one per MC)")
    .remaining();

  program.add_argument("--soft")
    .help("Use soft NoC backend")
    .default_value(false)
    .implicit_value(true);

  program.add_argument("--hard")
    .help("Use hard RTL backend")
    .default_value(false)
    .implicit_value(true);

  program.add_argument("--max-cycles")
    .help("Max simulation cycles")
    .default_value(uint64_t(10000000))
    .scan<'u', uint64_t>();

  program.add_argument("--dram-config")
    .help("DRAMsim3 config file");

  program.add_argument("--dram-log")
    .help("DRAMsim3 log directory")
    .default_value(string("."));

  program.add_argument("--trace")
    .help("Enable FST tracing")
    .default_value(false)
    .implicit_value(true);

  program.add_argument("--log")
    .help("Enable verbose logging")
    .default_value(false)
    .implicit_value(true);

  program.add_argument("--rng-seed")
    .help("RNG seed for peripheral device")
    .scan<'u', uint32_t>();

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &err) {
    cerr << err.what() << endl;
    cerr << program;
    return 1;
  }

  bool use_soft = program.get<bool>("--soft");
  bool use_hard = program.get<bool>("--hard");

  if (!use_soft && !use_hard) {
    cerr << "Error: specify at least one of --soft or --hard" << endl;
    cerr << program;
    return 1;
  }

  auto image_paths = program.get<vector<string>>("images");
  if (image_paths.empty()) {
    cerr << "Error: at least one memory image must be provided" << endl;
    cerr << program;
    return 1;
  }

  // Validate image count matches hardware MC count
  int hw_num_mc = HARD_NUM_MC;
  if ((int)image_paths.size() != hw_num_mc) {
    cerr << "Error: " << image_paths.size() << " image(s) provided but hardware has "
         << hw_num_mc << " memory controller(s)" << endl;
    return 1;
  }

  uint64_t max_cycles = program.get<uint64_t>("--max-cycles");

  // RNG seed
  if (auto seed = program.present<uint32_t>("--rng-seed")) {
    PeripheralDevice::global_seed_override = *seed;
  }

  // Logging
  if (program.get<bool>("--log")) {
    setSoftSystemModelLogging(true);
  }

  // Build init files list
  vector<string_view> dramInitFiles;
  for (auto &p : image_paths) dramInitFiles.push_back(p);

  // Optional DRAMsim3
  optional<DRAMsim3Config> dram_cfg;
  if (auto cfg = program.present<string>("--dram-config")) {
    auto dram_log = program.get<string>("--dram-log");
    dram_cfg = DRAMsim3Config{*cfg, dram_log};
  }

  System system(dramInitFiles, dram_cfg);

  bool enable_trace = program.get<bool>("--trace");
  std::unique_ptr<VerilatedFstC> fst_tracer;
  if (enable_trace) {
    Verilated::traceEverOn(true);
    fst_tracer = std::make_unique<VerilatedFstC>();
  }

  if (use_hard) {
    auto hard = make_unique<HardSystemBackend>();
    if (fst_tracer) hard->attachTrace(fst_tracer.get(), 99);
    system.addBackend(move(hard));
    cerr << "[Main] Hard backend enabled" << endl;
  }

  if (use_soft) {
    auto soft = make_unique<SoftSystemModel>(HARD_NUM_PU, HARD_NUM_MC);
    if (fst_tracer) soft->attachTrace(fst_tracer.get(), 99);
    system.addBackend(move(soft));
    cerr << "[Main] Soft backend enabled" << endl;
  }

  if (fst_tracer) {
    fst_tracer->open("trace.fst");
    cerr << "[Main] FST tracing enabled → trace.fst" << endl;
  }

  int rc = system.run(max_cycles);

  if (fst_tracer) {
    fst_tracer->flush();
    fst_tracer->close();
  }

  return rc;
}
