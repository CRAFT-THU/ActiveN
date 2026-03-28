/**
 * System-level simulation driver.
 *
 * Drives the System module (multicore + NoC + MemIf) with flat backing memory.
 * Memory requests arrive as GlobalMemReq per MC (id, addr, wdata[256], wbe[32], write).
 * Responses sent as GlobalMemResp per MC (id, rdata[256]).
 * Peripheral requests arrive via a separate peripheral MemIf (same format).
 * End-of-simulation: peripheral write to address 0x0 (= physical 0x40000000).
 *
 * Environment:
 *   MEOW_TEXT       - Binary memory image
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
#include <signal.h>

#include <chrono>
#include <iomanip>
#include <verilated_fst_c.h>
#include "sys_verilated/sys_rtl.h"

#include "devices.h"

using namespace std;

static bool TRACE = false;
static bool LOG = false;
static bool exiting = false;

static const uint64_t RESET_LENGTH = 10;
static const uint32_t TEXT_BASE = 0x80000000ul;
static const int MEM_BUS_WORDS = 256 / 32; // 8

static uint32_t *text_aligned = nullptr;
static size_t text_size = 0;
static std::unique_ptr<VerilatedFstC> tracer;

static void sighandler(int) { exiting = true; }

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

struct SystemSim {
  std::unique_ptr<sys_rtl> sys;
  uint64_t cycle = 0;
  int num_pu, num_mc;

  std::vector<std::deque<MemResponse>> mem_resps;
  std::vector<MemPort> mem_ports;

  // Peripheral
  PeripheralDevice periph;
  std::deque<MemResponse> periph_resps;

  SystemSim(int pu, int mc) : num_pu(pu), num_mc(mc) {
    sys.reset(new sys_rtl("system"));
    mem_resps.resize(num_mc);
    mem_ports.resize(num_mc);
    for (int i = 0; i < num_mc; i++) initMemPort(i);
  }

  ~SystemSim() { sys->final(); }

  void initMemPort(int i) {
    auto &p = mem_ports[i];
    switch(i) {
      #include "mem_ports.inc"
      default: break;
    }
  }

  void processMemReq(int mc) {
    auto &p = mem_ports[mc];
    uint16_t id = *p.req_id;
    uint32_t addr = *p.req_addr + TEXT_BASE;
    bool is_write = *p.req_write;

    if (is_write) {
      // Apply byte-masked write
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

    // Return 256-bit aligned read (also for stores, as read-after-write)
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
    mem_resps[mc].push_back(resp);
  }

  void processPeriphReq() {
    uint16_t id = sys->io_periph_req_bits_id;
    uint32_t addr = sys->io_periph_req_bits_addr;
    bool is_write = sys->io_periph_req_bits_write;

    if (is_write) {
      // Decode wdata and byte enables to extract the written value
      uint32_t wbe = sys->io_periph_req_bits_wbe;
      uint32_t wdata = 0;
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

    // Return dummy response
    MemResponse resp;
    resp.id = id;
    memset(resp.data, 0, sizeof(resp.data));
    periph_resps.push_back(resp);
  }

  void step() {
    ++cycle;

    if (cycle <= RESET_LENGTH) {
      sys->reset = true;
      for (int mc = 0; mc < num_mc; mc++) {
        *mem_ports[mc].req_ready = 0;
        *mem_ports[mc].resp_valid = 0;
      }
      sys->io_periph_req_ready = 0;
      sys->io_periph_resp_valid = 0;
      sys->clock = true;
      sys->eval();
      sys->clock = false;
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
    sys->eval();

    if (TRACE) tracer->dump(cycle * 2);

    // Set memory request ready (always accept)
    for (int mc = 0; mc < num_mc; mc++) {
      *mem_ports[mc].req_ready = 1;
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

    // Negedge
    sys->clock = false;
    sys->eval();
    if (TRACE) tracer->dump(cycle * 2 + 1);
  }
};

int main(int argc, char **argv) {
  auto trace_cfg = getenv("MEOW_TRACE");
  if (trace_cfg && trace_cfg[0] != '\0') TRACE = true;
  auto log_cfg = getenv("MEOW_LOG");
  if (log_cfg && log_cfg[0] != '\0') LOG = true;

  int num_pu = 16, num_mc = 1;
  auto pu_cfg = getenv("AN_NUM_PU");
  auto mc_cfg = getenv("AN_NUM_MC");
  if (pu_cfg && pu_cfg[0] != '\0') num_pu = atoi(pu_cfg);
  if (mc_cfg && mc_cfg[0] != '\0') num_mc = atoi(mc_cfg);
  cout << "[System] PUs=" << num_pu << " MCs=" << num_mc << endl;

  auto text_path = getenv("MEOW_TEXT");
  if (!text_path || text_path[0] == '\0') {
    cerr << "Error: MEOW_TEXT not set" << endl;
    return 1;
  }

  uint64_t max_cycles = 10000000;
  auto max_cfg = getenv("MEOW_MAX_CYCLES");
  if (max_cfg && max_cfg[0] != '\0') max_cycles = strtoull(max_cfg, nullptr, 10);

  ifstream text_input(text_path, ios::binary);
  if (!text_input) { cerr << "Error: cannot open " << text_path << endl; return 1; }
  text_input.seekg(0, ios::end);
  size_t file_size = text_input.tellg();
  if (file_size % 4 != 0) { cerr << "Error: not 4-byte aligned" << endl; return 1; }
  const size_t MEM_SIZE = 16 * 1024 * 1024;
  text_size = MEM_SIZE;
  char *text_mem = new (align_val_t(4)) char[MEM_SIZE]();
  text_input.seekg(0);
  text_input.read(text_mem, file_size);
  text_aligned = (uint32_t *)text_mem;
  cout << "[System] Loaded " << file_size << " bytes" << endl;

  struct sigaction sig;
  sig.sa_handler = sighandler;
  sigemptyset(&sig.sa_mask);
  sig.sa_flags = 0;
  sigaction(SIGINT, &sig, NULL);

  if (TRACE) {
    Verilated::traceEverOn(true);
    tracer.reset(new VerilatedFstC);
  }

  SystemSim sim(num_pu, num_mc);

  if (TRACE) {
    sim.sys->trace(tracer.get(), 128);
    tracer->open("./trace.fst");
  }

  cout << "[System] Running (max " << max_cycles << " cycles)..." << endl;
  auto wall_start = chrono::steady_clock::now();
  while (!sim.periph.finished && sim.cycle < max_cycles && !exiting) {
    sim.step();
  }
  auto wall_end = chrono::steady_clock::now();
  double wall_secs = chrono::duration<double>(wall_end - wall_start).count();

  if (sim.periph.finished) {
    cout << "[System] Result: " << dec << sim.periph.result << " (0x" << hex << sim.periph.result << ")" << endl;
    cout << "[System] Cycles: " << dec << sim.cycle << endl;
  } else {
    cout << "[System] " << (exiting ? "Interrupted" : "Timed out") << " at cycle " << sim.cycle << endl;
  }
  cout << "[System] Speed: " << dec << (uint64_t)(sim.cycle / wall_secs) << " cycles/s" << endl;
  cout << "[System] Runtime: " << fixed << setprecision(3) << wall_secs << "s" << endl;

  if (TRACE) tracer->close();
  delete[] text_aligned;
  return sim.periph.finished ? 0 : 1;
}
