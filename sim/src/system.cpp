/**
 * System-level simulation driver.
 *
 * Drives the System module (multicore + NoC + MemIf) with flat backing memory.
 * Memory requests arrive as GlobalMemReq per MC (id, addr, wdata[256], wbe[32], write).
 * Responses sent as GlobalMemResp per MC (id, rdata[256]).
 * Peripheral requests arrive via a separate peripheral MemIf (same format).
 * End-of-simulation: peripheral write to address 0x0 (= physical 0x40000000).
 *
 * DRAMsim3 integration: when MEOW_MEM is set, memory responses are delayed
 * until DRAMsim3 completes the transaction (accurate timing simulation).
 *
 * Environment:
 *   MEOW_TEXT       - Binary memory image
 *   MEOW_MEM        - DRAMsim3 config file (optional; enables DRAM latency sim)
 *   MEOW_MEM_LOG    - DRAMsim3 log directory (default: ".")
 *   MEOW_TRACE      - Enable FST tracing
 *   MEOW_LOG        - Enable logging
 *   MEOW_MAX_CYCLES - Max cycles (default: 10000000)
 */

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <deque>
#include <map>
#include <signal.h>
#include <filesystem>

#include <chrono>
#include <iomanip>
#include <verilated_fst_c.h>
#include "sys_verilated/sys_rtl.h"
#include "sys_verilated/sys_rtl___024root.h"
#include "sys_verilated/sys_rtl_Core.h"

#include "devices.h"
#include "system_model.h"
#include "system_config.h"
#include "dramsim3/dramsim3.h"

using namespace std;

static bool TRACE = false;
static bool LOG = false;
static bool exiting = false;

static const uint64_t RESET_LENGTH = 10;
static const uint32_t TEXT_BASE = 0x80000000ul;
static const int MEM_BUS_WORDS = 256 / 32; // 8

static uint32_t *text_aligned = nullptr;
static size_t text_size = 0;
static VerilatedFstC *tracer = nullptr;
static std::unique_ptr<std::ofstream> mem_trace;
static std::vector<MemTraceEvent> *current_mem_trace = nullptr;
static std::vector<PeriphTraceEvent> *current_periph_trace = nullptr;

static void sighandler(int) { exiting = true; }

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

struct MemResponse {
  uint16_t id;
  uint32_t data[MEM_BUS_WORDS];
};

// Per-MC memory port accessors
struct MemPort {
  // Request (output from system)
  uint8_t *req_valid;
  uint8_t *req_ready;
  uint16_t *req_id;
  uint32_t *req_addr;
  uint32_t *req_wdata; // WData[8]
  uint32_t *req_wbe;
  uint8_t *req_write;
  // Response (input to system)
  uint8_t *resp_valid;
  uint16_t *resp_id;
  uint32_t *resp_rdata; // WData[8]
};

struct SystemModel::Impl {
  std::unique_ptr<sys_rtl> sys;
  uint64_t cycle = 0;
  int num_pu, num_mc;
  bool reset_cycle = false;

  std::vector<std::deque<MemResponse>> mem_resps;
  std::vector<MemPort> mem_ports;
  std::vector<uint64_t> mc_base; // cumulative base address per MC

  sys_rtl_Core* getPuCore(int id) {
    auto r = sys->rootp;
    switch(id) {
#define PU_CASE(N) case N: return r->__PVT__System__DOT__pu_##N
      SYSTEM_PU_CASE_ENTRIES
#undef PU_CASE
      default: return nullptr;
    }
  }

  // Peripheral
  PeripheralDevice periph;
  std::deque<MemResponse> periph_resps;
  std::vector<MemTraceEvent> last_mem_trace;
  std::vector<PeriphTraceEvent> last_periph_trace;

  // DRAMsim3 state (per MC)
  bool use_dram = false;
  struct DramMC {
    dramsim3::MemorySystem *dram = nullptr;
    struct PendingReq {
      uint64_t addr;
      bool is_write;
      MemResponse resp;
    };
    std::deque<PendingReq> submit_queue;
    std::multimap<uint64_t, MemResponse> inflight;
  };
  std::vector<DramMC> dram_mcs;

  Impl(int pu, int mc) : num_pu(pu), num_mc(mc) {
    sys.reset(new sys_rtl("system"));
    mem_resps.resize(num_mc);
    mem_ports.resize(num_mc);
    for (int i = 0; i < num_mc; i++) initMemPort(i);
    // Compute per-MC cumulative base addresses
    mc_base.resize(num_mc);
    uint64_t cumul = 0;
    for (int i = 0; i < num_mc; i++) {
      mc_base[i] = cumul;
      if (i < SYSTEM_CONFIG.core.numMemCtrl)
        cumul += SYSTEM_CONFIG.core.memCtrlSizes[i];
    }
  }

  void initDram(const char *cfg, const char *log_dir) {
    use_dram = true;
    dram_mcs.resize(num_mc);
    for (int i = 0; i < num_mc; i++) {
      auto log = std::string(log_dir) + "/" + std::to_string(i);
      std::filesystem::create_directories(log);
      dram_mcs[i].dram = new dramsim3::MemorySystem(
        cfg, log.c_str(),
        [this, i](uint64_t addr) { dramReadCb(i, addr); },
        [this, i](uint64_t addr) { dramWriteCb(i, addr); }
      );
    }
  }

  ~Impl() {
    sys->final();
    for (auto &d : dram_mcs) delete d.dram;
  }

  void initMemPort(int i) {
    auto &p = mem_ports[i];
    switch(i) {
      SYSTEM_MEM_PORT_CASES
      default: break;
    }
  }

  void dramReadCb(int mc, uint64_t addr) {
    auto &d = dram_mcs[mc];
    auto it = d.inflight.find(addr);
    if (it != d.inflight.end()) {
      mem_resps[mc].push_back(it->second);
      d.inflight.erase(it);
    } else {
      cerr << "[DRAM] WARNING: read callback for unknown addr 0x" << hex << addr
           << " on MC" << dec << mc << " cycle=" << cycle << endl;
    }
  }

  void dramWriteCb(int mc, uint64_t addr) {
    auto &d = dram_mcs[mc];
    auto it = d.inflight.find(addr);
    if (it != d.inflight.end()) {
      // Don't deliver write responses: MemIf marks writes completed on issue.
      // Delivering a stale write response can corrupt a reallocated slot
      // (MemIf section 3 last-connect overrides section 1's completed := false).
      d.inflight.erase(it);
    } else {
      cerr << "[DRAM] WARNING: write callback for unknown addr 0x" << hex << addr
           << " on MC" << dec << mc << " cycle=" << cycle << endl;
    }
  }

  MemResponse buildMemResp(int mc, uint16_t id, uint32_t addr) {
    MemResponse resp;
    resp.id = id;
    uint32_t aligned = addr & ~(uint32_t)(MEM_BUS_WORDS * 4 - 1);
    for (int i = 0; i < MEM_BUS_WORDS; i++) {
      uint32_t ba = aligned + i * 4;
      if (ba >= TEXT_BASE && (ba - TEXT_BASE + 3) < text_size)
        resp.data[i] = text_aligned[(ba - TEXT_BASE) / 4];
      else
        resp.data[i] = 0;
    }
    return resp;
  }

  void processMemReq(int mc) {
    auto &p = mem_ports[mc];
    uint16_t id = *p.req_id;
    uint32_t local_addr = *p.req_addr;
    // Convert MC-local address to global: add MC's base offset in global space
    uint32_t addr = local_addr + TEXT_BASE + (uint32_t)mc_base[mc];
    bool is_write = *p.req_write;

    if (is_write) {
      // Apply byte-masked write to backing memory immediately
      uint32_t wbe = *p.req_wbe;
      if (addr >= TEXT_BASE && (addr - TEXT_BASE) < text_size) {
        uint32_t block_base = addr & ~(uint32_t)(MEM_BUS_WORDS * 4 - 1);
        for (int w = 0; w < MEM_BUS_WORDS; w++) {
          uint32_t ba = block_base + w * 4;
          uint32_t word_be = (wbe >> (w * 4)) & 0xF;
          if (word_be && ba >= TEXT_BASE && (ba - TEXT_BASE + 3) < text_size) {
            uint32_t offset = (ba - TEXT_BASE) / 4;
            uint32_t old_val = text_aligned[offset];
            uint32_t new_val = p.req_wdata[w];
            uint32_t mask = 0;
            for (int b = 0; b < 4; b++) {
              if (word_be & (1 << b)) mask |= 0xFFu << (b * 8);
            }
            text_aligned[offset] = (old_val & ~mask) | (new_val & mask);
          }
        }
      }
      if (LOG) cout << "[System] Store MC" << mc << " addr=0x" << hex << addr
                    << " id=" << dec << id << endl;
    } else {
      if (LOG) cout << "[System] Load MC" << mc << " addr=0x" << hex << addr
                    << " id=" << dec << id << endl;
    }

    traceMemReq(cycle, mc, is_write, local_addr, id);

    // Build the response data from backing memory
    MemResponse resp = buildMemResp(mc, id, addr);

    if (use_dram) {
      // Queue for DRAMsim3 submission; response delayed until callback
      dram_mcs[mc].submit_queue.push_back({(uint64_t)local_addr, is_write, resp});
    } else {
      // Instant response
      mem_resps[mc].push_back(resp);
    }
  }

  void processPeriphReq() {
    uint16_t id = sys->io_periph_req_bits_id;
    uint32_t addr = sys->io_periph_req_bits_addr;
    bool is_write = sys->io_periph_req_bits_write;
    uint32_t wdata = 0;

    if (is_write) {
      // Decode wdata and byte enables to extract the written value
      uint32_t wbe = sys->io_periph_req_bits_wbe;
      // Find the first active word from the 256-bit write data
      for (int w = 0; w < MEM_BUS_WORDS; w++) {
        uint32_t word_be = (wbe >> (w * 4)) & 0xF;
        if (word_be) {
          wdata = sys->io_periph_req_bits_wdata[w];
          break;
        }
      }
      periph.write(addr, wdata);
      if (LOG) cout << "[System] Periph write: addr=0x" << hex << addr
                    << " data=0x" << wdata << dec << endl;
    } else {
      if (LOG) cout << "[System] Periph read: addr=0x" << hex << addr << dec << endl;
    }

    if (current_periph_trace) {
      current_periph_trace->push_back(PeriphTraceEvent{
        .cycle = cycle,
        .id = id,
        .addr = addr,
        .is_write = is_write,
        .wdata = is_write ? wdata : 0,
      });
    }

    // Return dummy response
    MemResponse resp;
    resp.id = id;
    memset(resp.data, 0, sizeof(resp.data));
    periph_resps.push_back(resp);
  }

  void stepPosedge() {
    ++cycle;
    last_mem_trace.clear();
    last_periph_trace.clear();
    current_mem_trace = &last_mem_trace;
    current_periph_trace = &last_periph_trace;
    reset_cycle = cycle <= RESET_LENGTH;

    if (reset_cycle) {
      sys->reset = true;
      for (int mc = 0; mc < num_mc; mc++) {
        *mem_ports[mc].req_ready = 0;
        *mem_ports[mc].resp_valid = 0;
      }
      sys->io_periph_req_ready = 0;
      sys->io_periph_resp_valid = 0;
      sys->clock = true;
      Verilated::timeInc(1);
      sys->eval();
      return;
    }

    sys->reset = false;

    // Process memory requests from previous cycle
    for (int mc = 0; mc < num_mc; mc++) {
      auto &p = mem_ports[mc];
      if (*p.req_ready && *p.req_valid) {
        processMemReq(mc);
      }
    }

    // Process peripheral request from previous cycle
    if (sys->io_periph_req_ready && sys->io_periph_req_valid) {
      processPeriphReq();
    }

    // Dequeue responses that were consumed last cycle
    for (int mc = 0; mc < num_mc; mc++) {
      if (*mem_ports[mc].resp_valid && !mem_resps[mc].empty()) {
        mem_resps[mc].pop_front();
      }
    }
    if (sys->io_periph_resp_valid && !periph_resps.empty()) {
      periph_resps.pop_front();
    }

    // Posedge
    sys->clock = true;
    Verilated::timeInc(1);
    sys->eval();

    // Tick peripheral timer
    periph.tick();

    // Tick DRAMsim3 and submit queued transactions
    if (use_dram) {
      for (int mc = 0; mc < num_mc; mc++) {
        auto &d = dram_mcs[mc];
        d.dram->ClockTick();
        while (!d.submit_queue.empty()) {
          auto &req = d.submit_queue.front();
          if (d.dram->WillAcceptTransaction(req.addr, req.is_write)) {
            d.dram->AddTransaction(req.addr, req.is_write);
            d.inflight.emplace(req.addr, req.resp);
            d.submit_queue.pop_front();
          } else {
            break;
          }
        }
      }
    }

    // Set memory request ready (backpressure if DRAMsim3 queue full)
    for (int mc = 0; mc < num_mc; mc++) {
      if (use_dram) {
        *mem_ports[mc].req_ready = dram_mcs[mc].submit_queue.size() < 32 ? 1 : 0;
      } else {
        *mem_ports[mc].req_ready = 1;
      }
    }
    sys->io_periph_req_ready = 1;

    // Set memory response signals
    for (int mc = 0; mc < num_mc; mc++) {
      auto &p = mem_ports[mc];
      if (mem_resps[mc].empty()) {
        *p.resp_valid = 0;
        *p.resp_id = 0;
        for (int w = 0; w < MEM_BUS_WORDS; w++) p.resp_rdata[w] = 0;
      } else {
        auto &r = mem_resps[mc].front();
        *p.resp_valid = 1;
        *p.resp_id = r.id;
        for (int w = 0; w < MEM_BUS_WORDS; w++) p.resp_rdata[w] = r.data[w];
      }
    }

    // Set peripheral response signals
    if (periph_resps.empty()) {
      sys->io_periph_resp_valid = 0;
      sys->io_periph_resp_bits_id = 0;
      for (int w = 0; w < MEM_BUS_WORDS; w++)
        sys->io_periph_resp_bits_rdata[w] = 0;
    } else {
      auto &r = periph_resps.front();
      sys->io_periph_resp_valid = 1;
      sys->io_periph_resp_bits_id = r.id;
      for (int w = 0; w < MEM_BUS_WORDS; w++)
        sys->io_periph_resp_bits_rdata[w] = r.data[w];
    }

  }

  void stepNegedge() {
    sys->clock = false;
    Verilated::timeInc(1);
    sys->eval();
    current_mem_trace = nullptr;
    current_periph_trace = nullptr;
  }
};

bool loadSystemModelImage(const ModelImageConfig &config, std::string *error) {
  unloadSystemModelImage();

  ifstream text_input(config.text_path, ios::binary);
  if (!text_input) {
    if (error) *error = "Error: cannot open " + config.text_path;
    return false;
  }

  text_input.seekg(0, ios::end);
  size_t file_size = text_input.tellg();
  if (file_size % 4 != 0) {
    if (error) *error = "Error: not 4-byte aligned";
    return false;
  }

  text_size = config.mem_size;
  char *text_mem = new (align_val_t(4)) char[text_size]();
  text_input.seekg(0);
  text_input.read(text_mem, file_size);
  text_aligned = reinterpret_cast<uint32_t *>(text_mem);

  if (config.data_path) {
    ifstream data_input(*config.data_path, ios::binary);
    if (!data_input) {
      if (error) *error = "Error: cannot open " + *config.data_path;
      unloadSystemModelImage();
      return false;
    }
    data_input.seekg(0, ios::end);
    size_t data_size = data_input.tellg();
    size_t data_off = config.data_addr - TEXT_BASE;
    if (data_off + data_size > text_size) {
      if (error) *error = "Error: data file too large";
      unloadSystemModelImage();
      return false;
    }
    data_input.seekg(0);
    data_input.read(reinterpret_cast<char *>(text_aligned) + data_off, data_size);
  }

  return true;
}

void unloadSystemModelImage() {
  if (!text_aligned) return;
  delete[] reinterpret_cast<char *>(text_aligned);
  text_aligned = nullptr;
  text_size = 0;
}

bool openSystemModelMemTrace(const std::optional<std::string> &path, std::string *error) {
  mem_trace.reset();
  if (!path) return true;

  mem_trace.reset(new ofstream(*path));
  if (*mem_trace) return true;

  if (error) *error = "Error: cannot open memory trace file " + *path;
  mem_trace.reset();
  return false;
}

void setSystemModelLogging(bool enabled) {
  LOG = enabled;
}

SystemModel::SystemModel(int pu, int mc) : impl_(std::make_unique<Impl>(pu, mc)) {}
SystemModel::~SystemModel() = default;
SystemModel::SystemModel(SystemModel &&) noexcept = default;
SystemModel &SystemModel::operator=(SystemModel &&) noexcept = default;

void SystemModel::attachTrace(VerilatedFstC *trace_file, int depth) {
  TRACE = trace_file != nullptr;
  tracer = trace_file;
  if (trace_file) impl_->sys->trace(trace_file, depth);
}

void SystemModel::initDram(const char *cfg, const char *log_dir) {
  impl_->initDram(cfg, log_dir);
}

void SystemModel::stepPosedge() { impl_->stepPosedge(); }
void SystemModel::stepNegedge() { impl_->stepNegedge(); }

void SystemModel::step() {
  impl_->stepPosedge();
  if (TRACE && tracer) tracer->dump(impl_->cycle * 2);
  impl_->stepNegedge();
  if (TRACE && tracer) tracer->dump(impl_->cycle * 2 + 1);
}

void SystemModel::printDramStats() const {
  if (!impl_->use_dram) return;
  for (int i = 0; i < impl_->num_mc; ++i) impl_->dram_mcs[i].dram->PrintStats();
}

uint64_t SystemModel::cycle() const { return impl_->cycle; }
bool SystemModel::finished() const { return impl_->periph.finished; }
uint32_t SystemModel::result() const { return impl_->periph.result; }
uint64_t SystemModel::timerCount() const { return impl_->periph.timer_count; }
bool SystemModel::useDram() const { return impl_->use_dram; }

const std::vector<MemTraceEvent> &SystemModel::lastMemTrace() const { return impl_->last_mem_trace; }
const std::vector<PeriphTraceEvent> &SystemModel::lastPeriphTrace() const {
  return impl_->last_periph_trace;
}

int runSystemModelFromEnv() {
  std::unique_ptr<VerilatedFstC> owned_tracer;

  auto trace_cfg = getenv("MEOW_TRACE");
  if (trace_cfg && trace_cfg[0] != '\0') TRACE = true;
  auto log_cfg = getenv("MEOW_LOG");
  if (log_cfg && log_cfg[0] != '\0') LOG = true;

  auto mem_trace_path = getenv("MEOW_MEM_TRACE");
  if (mem_trace_path && mem_trace_path[0] != '\0' &&
      !openSystemModelMemTrace(std::string(mem_trace_path))) {
    cerr << "Error: cannot open memory trace file " << mem_trace_path << endl;
    return 1;
  }

  int num_pu = SYSTEM_CONFIG.numPU;
  int num_mc = SYSTEM_CONFIG.numMC;
  cout << "[System] PUs=" << num_pu << " MCs=" << num_mc << endl;

  auto text_path = getenv("MEOW_TEXT");
  if (!text_path || text_path[0] == '\0') {
    cerr << "Error: MEOW_TEXT not set" << endl;
    return 1;
  }

  uint64_t max_cycles = 10000000;
  auto max_cfg = getenv("MEOW_MAX_CYCLES");
  if (max_cfg && max_cfg[0] != '\0') max_cycles = strtoull(max_cfg, nullptr, 10);

  // Optional: load additional data file at a configurable offset
  // MEOW_DATA      = path to data file (e.g. datagen output)
  // MEOW_DATA_ADDR = global address to load at (default: 0x80100000)
  uint32_t data_addr = 0x80100000;
  auto data_path = getenv("MEOW_DATA");
  auto data_addr_cfg = getenv("MEOW_DATA_ADDR");
  if (data_addr_cfg && data_addr_cfg[0] != '\0') data_addr = strtoul(data_addr_cfg, nullptr, 0);

  ModelImageConfig image_cfg{
    .text_path = text_path,
    .data_path = (data_path && data_path[0] != '\0') ? std::optional<std::string>(data_path) : std::nullopt,
    .data_addr = data_addr,
  };
  std::string image_error;
  if (!loadSystemModelImage(image_cfg, &image_error)) {
    cerr << image_error << endl;
    return 1;
  }

  cout << "[System] Loaded program image" << endl;
  if (image_cfg.data_path) {
    cout << "[System] Data loaded at 0x" << hex << image_cfg.data_addr << dec << endl;
  }

  struct sigaction sig;
  sig.sa_handler = sighandler;
  sigemptyset(&sig.sa_mask);
  sig.sa_flags = 0;
  sigaction(SIGINT, &sig, NULL);

  if (TRACE) {
    Verilated::traceEverOn(true);
    owned_tracer.reset(new VerilatedFstC);
  }

  SystemModel sim(num_pu, num_mc);

  if (TRACE) {
    sim.attachTrace(owned_tracer.get(), 128);
    owned_tracer->open("./trace.fst");
  }

  // Run reset phase first (suppresses init assertions)
  while (sim.cycle() < RESET_LENGTH) sim.step();

  // Optional DRAMsim3 integration (init after reset to avoid init assertion)
  auto mem_cfg = getenv("MEOW_MEM");
  if (mem_cfg && mem_cfg[0] != '\0') {
    auto mem_log = getenv("MEOW_MEM_LOG");
    const char *log_dir = (mem_log && mem_log[0] != '\0') ? mem_log : ".";
    sim.initDram(mem_cfg, log_dir);
    cout << "[System] DRAMsim3 enabled: " << mem_cfg << endl;
  }

  cout << "[System] Running (max " << max_cycles << " cycles)..." << endl;
  auto wall_start = chrono::steady_clock::now();
  while (!sim.finished() && sim.cycle() < max_cycles && !exiting) {
    sim.step();
  }

  auto wall_end = chrono::steady_clock::now();
  double wall_secs = chrono::duration<double>(wall_end - wall_start).count();

  if (sim.finished()) {
    cout << "[System] Result: " << dec << sim.result() << " (0x" << hex << sim.result() << ")" << endl;
    cout << "[System] Cycles: " << dec << sim.cycle() << endl;
  } else {
    cout << "[System] " << (exiting ? "Interrupted" : "Timed out") << " at cycle " << sim.cycle() << endl;
  }
  if (sim.timerCount() > 0) {
    cout << "[System] Timer: " << dec << sim.timerCount() << " cycles" << endl;
  }
  cout << "[System] Speed: " << dec << (uint64_t)(sim.cycle() / wall_secs) << " cycles/s" << endl;
  cout << "[System] Runtime: " << fixed << setprecision(3) << wall_secs << "s" << endl;

  sim.printDramStats();

  if (TRACE) owned_tracer->close();
  if (mem_trace) mem_trace->flush();
  unloadSystemModelImage();
  return sim.finished() ? 0 : 1;
}
