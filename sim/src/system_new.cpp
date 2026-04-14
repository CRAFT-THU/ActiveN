/**
 * Unified system-level simulation frontend.
 *
 * Drives one or more SystemBackend instances (soft NoC or hard RTL),
 * serving memory requests through the mem() interface with flat or DRAMsim3
 * backing memory.  When multiple backends are attached, they run in lockstep
 * (cosimulation) with discrepancy detection.
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
#include <signal.h>
#include <string>
#include <vector>

#include "system.h"
#include "devices.h"
#include "dramsim3/dramsim3.h"

using namespace std;

static constexpr uint32_t TEXT_BASE = 0x80000000u;
static constexpr int MEM_BUS_WORDS = MEM_BUS_WIDTH / 32;
static constexpr int RESP_QUEUE_DEPTH = 64;

static bool exiting = false;
static void sighandler(int) { exiting = true; }

// ---- Memory image (owned by the frontend, exported for hard backend) ----
uint32_t *text_aligned = nullptr;
size_t text_size = 0;

static void loadImage(const string &text_path,
                      const optional<string> &data_path,
                      uint32_t data_addr,
                      size_t mem_size) {
  ifstream fin(text_path, ios::binary);
  if (!fin) throw runtime_error("Cannot open " + text_path);
  fin.seekg(0, ios::end);
  size_t fsize = fin.tellg();
  if (fsize % 4 != 0) throw runtime_error("Image is not 4-byte aligned");
  text_size = mem_size;
  char *buf = new (align_val_t(4)) char[text_size]();
  fin.seekg(0);
  fin.read(buf, fsize);
  text_aligned = reinterpret_cast<uint32_t *>(buf);
  if (data_path) {
    ifstream din(*data_path, ios::binary);
    if (!din) throw runtime_error("Cannot open " + *data_path);
    din.seekg(0, ios::end);
    size_t dsz = din.tellg();
    size_t off = data_addr - TEXT_BASE;
    if (off + dsz > text_size) throw runtime_error("Data file too large");
    din.seekg(0);
    din.read(buf + off, dsz);
  }
}

static void unloadImage() {
  if (text_aligned) {
    delete[] reinterpret_cast<char *>(text_aligned);
    text_aligned = nullptr;
    text_size = 0;
  }
}

// Build a 32-byte-aligned read response from backing memory
void buildMemResp(uint32_t global_addr, uint8_t *data) {
  uint32_t aligned = global_addr & ~(uint32_t)(MEM_BUS_WORDS * 4 - 1);
  auto *out = reinterpret_cast<uint32_t *>(data);
  for (int i = 0; i < MEM_BUS_WORDS; ++i) {
    uint32_t ba = aligned + i * 4;
    if (ba >= TEXT_BASE && (ba - TEXT_BASE + 3) < text_size)
      out[i] = text_aligned[(ba - TEXT_BASE) / 4];
    else
      out[i] = 0;
  }
}

// Apply a write to backing memory using the lane-aligned wdata
void applyWrite(uint32_t global_addr, uint8_t size, const uint8_t *wdata) {
  // size: 2^size bytes.  wdata is lane-aligned.
  uint32_t nbytes = 1u << size;
  uint32_t block_base = global_addr & ~(uint32_t)(MEM_BUS_WORDS * 4 - 1);
  uint32_t byte_off = global_addr - block_base;
  if (block_base < TEXT_BASE) return;
  uint32_t mem_off = block_base - TEXT_BASE;
  auto *mem_bytes = reinterpret_cast<uint8_t *>(text_aligned);
  for (uint32_t b = 0; b < nbytes && b + byte_off < (uint32_t)(MEM_BUS_WORDS * 4); ++b) {
    uint32_t pos = mem_off + byte_off + b;
    if (pos < text_size) mem_bytes[pos] = wdata[byte_off + b];
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

  // Image
  string text_path;
  optional<string> data_path;
  uint32_t data_addr = 0x80100000u;
  size_t mem_size = 16 * 1024 * 1024;

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
    if (it != d.inflight.end()) d.inflight.erase(it);
  }

  // Serve a single request from a backend for MC mc
  void serveMemReq(int mc, const GlobalMemReq &req) {
    uint32_t global_addr = req.addr + TEXT_BASE + (uint32_t)mc_states[mc].mc_base;
    GlobalMemResp resp{};
    resp.id = req.id;

    if (req.write) {
      applyWrite(global_addr, req.size, req.wdata);
      memset(resp.data, 0, sizeof(resp.data));
    } else {
      buildMemResp(global_addr, resp.data);
    }

    if (use_dram) {
      dram_mcs[mc].submit_queue.push_back({(uint64_t)req.addr, req.write, resp});
    } else {
      mc_states[mc].resp_queue.push_back(resp);
    }
  }

  void servePeriphReq(const GlobalMemReq &req) {
    GlobalMemResp resp{};
    resp.id = req.id;
    memset(resp.data, 0, sizeof(resp.data));

    size_t byte_offset = req.addr & ((MEM_BUS_WORDS * sizeof(uint32_t)) - 1);
    size_t word_idx = byte_offset / sizeof(uint32_t);

    if (req.write) {
      uint32_t wdata;
      memcpy(&wdata, req.wdata + byte_offset, sizeof(uint32_t));
      periph.write(req.addr, wdata);
    } else {
      uint32_t rdata = periph.read(req.addr);
      memcpy(resp.data + word_idx * sizeof(uint32_t), &rdata, sizeof(uint32_t));
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
  void memInteractAll() {
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
            cerr << "[System] COSIM MISMATCH: port " << p
                 << " backend 0 req=" << first_has
                 << " backend " << bi << " req=" << cur_has << endl;
            exiting = true;
            return;
          }
          if (first_has && cur_has) {
            auto &a = *per_backend_out[0][p].req;
            auto &b = *per_backend_out[bi][p].req;
            if (a.addr != b.addr || a.write != b.write || a.id != b.id) {
              cerr << "[System] COSIM MISMATCH: port " << p
                   << " addr 0x" << hex << a.addr << " vs 0x" << b.addr
                   << " write " << a.write << " vs " << b.write
                   << " id " << dec << a.id << " vs " << b.id << endl;
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
  if (!dramInitFiles.empty()) {
    impl_->text_path = string(dramInitFiles[0]);
  }
  if (dramInitFiles.size() > 1) {
    impl_->data_path = string(dramInitFiles[1]);
  }
  if (dramInitFiles.size() > 2) {
    impl_->data_addr = strtoul(string(dramInitFiles[2]).c_str(), nullptr, 0);
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

void System::run(uint64_t maxCycles) {
  if (impl_->backends.empty()) {
    cerr << "[System] Error: no backends added" << endl;
    return;
  }

  // Initialize MC bases from first backend's config
  auto cfg = impl_->backends[0]->config();
  impl_->initMcBases(cfg);

  cerr << "[System] PUs=" << cfg.numPU << " MCs=" << cfg.numMC << endl;

  // Load image
  loadImage(impl_->text_path, impl_->data_path, impl_->data_addr, impl_->mem_size);
  cerr << "[System] Loaded program image" << endl;

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
  bool finished = false;

  while (!finished && cycle < maxCycles && !exiting) {
    ++cycle;

    // Step all backends
    for (auto &b : impl_->backends) b->step();

    // Tick frontend memory
    impl_->tickMemory();

    // Call all backends' mem() with the same bus_in, verify requests match, serve once
    impl_->memInteractAll();

    // Check if the frontend's peripheral indicates finished
    finished = impl_->periph.finished;
  }

  auto wall_end = chrono::steady_clock::now();
  double wall_secs = chrono::duration<double>(wall_end - wall_start).count();

  if (finished) {
    cerr << "[System] Result: " << dec << impl_->periph.result
         << " (0x" << hex << impl_->periph.result << ")" << endl;
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

  unloadImage();
}
