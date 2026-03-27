/**
 * System-level simulation driver.
 *
 * Drives the System module (multicore + NoC) with flat backing memory.
 * Memory request events arrive on memOut ports (Flit: tag 0xFF00/0xFF01).
 * Responses sent on per-core memResp ports (256-bit data + tag).
 * End-of-simulation: flit with tag=0 arriving at any MC port, result in data.
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

#include <verilated_fst_c.h>
#include "sys_verilated/sys_rtl.h"

using namespace std;

static bool TRACE = false;
static bool LOG = false;
static bool exiting = false;

static const uint64_t RESET_LENGTH = 10;
static const uint32_t TEXT_BASE = 0x80000000ul;
static const int MEM_BUS_WORDS = 256 / 32;

static uint32_t *text_aligned = nullptr;
static size_t text_size = 0;
static std::unique_ptr<VerilatedFstC> tracer;

static void sighandler(int) { exiting = true; }

struct MemResponse {
  uint16_t tag;
  uint32_t data[MEM_BUS_WORDS];
};

struct MemRequestCollector {
  bool active = false;
  uint16_t tag = 0;
  uint16_t src = 0;
  int flit_count = 0;
  uint32_t operands[3];
  void reset() { active = false; flit_count = 0; }
};

static MemResponse readBlock(uint32_t addr, uint16_t resp_tag) {
  MemResponse resp;
  resp.tag = resp_tag;
  uint32_t aligned = addr & ~((MEM_BUS_WORDS * 4) - 1);
  for (int i = 0; i < MEM_BUS_WORDS; i++) {
    uint32_t ba = aligned + i * 4;
    if (ba >= TEXT_BASE && (ba - TEXT_BASE) < text_size)
      resp.data[i] = text_aligned[(ba - TEXT_BASE) / 4];
    else
      resp.data[i] = 0;
  }
  return resp;
}

// Port accessor structs
struct MemRespPort {
  uint8_t *valid;
  uint16_t *tag;
  uint32_t *data;
};

struct MemOutPort {
  uint8_t *valid;
  uint8_t *ready;
  uint16_t *src;
  uint16_t *dst;
  uint16_t *tag;
  uint32_t *data;
};

struct SystemSim {
  std::unique_ptr<sys_rtl> sys;
  uint64_t cycle = 0;
  int num_pu, num_mc;

  std::vector<std::deque<MemResponse>> mem_resps;
  std::vector<MemRequestCollector> src_collectors; // per-source (PU) collectors
  std::vector<MemRespPort> resp_ports;
  std::vector<MemOutPort> out_ports;

  bool finished = false;
  uint32_t result = 0;

  SystemSim(int pu, int mc) : num_pu(pu), num_mc(mc) {
    sys.reset(new sys_rtl("system"));
    mem_resps.resize(num_pu);
    src_collectors.resize(num_pu);
    resp_ports.resize(num_pu);
    out_ports.resize(num_mc);

    // Init port accessor tables
    for (int i = 0; i < num_pu; i++) initRespPort(i);
    for (int i = 0; i < num_mc; i++) initOutPort(i);
  }

  ~SystemSim() { sys->final(); }

  void initRespPort(int i) {
    auto &p = resp_ports[i];
    switch(i) {
      #include "resp_ports.inc"
      default: break;
    }
  }

  void initOutPort(int i) {
    auto &p = out_ports[i];
    switch(i) {
      #include "out_ports.inc"
      default: break;
    }
  }

  void processMemRequest(int src_idx) {
    auto &c = src_collectors[src_idx];
    uint32_t addr = c.operands[0] + TEXT_BASE;
    uint16_t resp_tag = c.operands[1] & 0xFFFF;

    if (c.tag == 0xFF00) {
      if (LOG) cout << "[System] Load PU" << c.src << " addr=0x" << hex << addr
                    << " rtag=0x" << resp_tag << dec << endl;
      mem_resps[src_idx].push_back(readBlock(addr, resp_tag));
    } else if (c.tag == 0xFF01) {
      uint16_t size = (c.operands[1] >> 16) & 0xFFFF;
      uint32_t wdata = c.operands[2];
      if (LOG) cout << "[System] Store PU" << c.src << " addr=0x" << hex << addr
                    << " sz=" << dec << size << " data=0x" << hex << wdata
                    << " rtag=0x" << resp_tag << dec << endl;
      if (addr >= TEXT_BASE && (addr - TEXT_BASE) < text_size) {
        uint32_t offset = (addr - TEXT_BASE) / 4;
        uint32_t old_val = text_aligned[offset];
        uint32_t byte_off = addr & 3;
        uint32_t mask = 0;
        switch (size) {
          case 0: mask = 0xFFu << (byte_off * 8); break;
          case 1: mask = 0xFFFFu << (byte_off * 8); break;
          default: mask = 0xFFFFFFFFu; break;
        }
        text_aligned[offset] = (old_val & ~mask) | (wdata & mask);
      }
      mem_resps[src_idx].push_back(readBlock(addr, resp_tag));
    }
    c.reset();
  }

  void step() {
    ++cycle;

    if (cycle <= RESET_LENGTH) {
      sys->reset = true;
      for (int mc = 0; mc < num_mc; mc++) *out_ports[mc].ready = 0;
      for (int i = 0; i < num_pu; i++) *resp_ports[i].valid = 0;
      sys->clock = true;
      sys->eval();
      sys->clock = false;
      sys->eval();
      return;
    }

    sys->reset = false;

    // Process memOut from previous cycle
    for (int mc = 0; mc < num_mc; mc++) {
      auto &op = out_ports[mc];
      if (*op.ready && *op.valid) {
        uint16_t src = *op.src;
        uint16_t dst = *op.dst;
        uint32_t data = *op.data;
        uint16_t tag = *op.tag;

        if (tag == 0xFF00 || tag == 0xFF01) {
          int src_idx = src - 1;
          if (src_idx < 0 || src_idx >= num_pu) {
            cerr << "[System] Invalid src PU " << src << endl;
            continue;
          }
          auto &col = src_collectors[src_idx];
          if (!col.active) {
            col.active = true;
            col.tag = tag;
            col.src = src;
            col.flit_count = 0;
          }
          col.operands[col.flit_count++] = data;
          int expected = (tag == 0xFF00) ? 2 : 3;
          if (col.flit_count >= expected) processMemRequest(src_idx);
        } else {
          if (LOG) cout << "[System] AM@MC" << mc << " src=" << src
                       << " dst=" << dst << " data=0x" << hex << data
                       << " tag=" << dec << tag << endl;
          if (tag == 0) {
            finished = true;
            result = data;
            cout << "[System] Finished: result=" << dec << data
                 << " (0x" << hex << data << ")" << dec << endl;
          }
        }
      }
    }

    // Dequeue responses that were consumed last cycle
    for (int i = 0; i < num_pu; i++) {
      if (*resp_ports[i].valid && !mem_resps[i].empty()) {
        mem_resps[i].pop_front();
      }
    }

    // Posedge
    sys->clock = true;
    sys->eval();

    if (TRACE) tracer->dump(cycle * 2);

    // Set memOut ready
    for (int mc = 0; mc < num_mc; mc++) *out_ports[mc].ready = 1;

    // Set memResp signals
    for (int i = 0; i < num_pu; i++) {
      auto &rp = resp_ports[i];
      if (mem_resps[i].empty()) {
        *rp.valid = 0;
        *rp.tag = 0;
        for (int w = 0; w < MEM_BUS_WORDS; w++) rp.data[w] = 0;
      } else {
        auto &r = mem_resps[i].front();
        *rp.valid = 1;
        *rp.tag = r.tag;
        for (int w = 0; w < MEM_BUS_WORDS; w++) rp.data[w] = r.data[w];
      }
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
  while (!sim.finished && sim.cycle < max_cycles && !exiting) {
    sim.step();
  }

  if (sim.finished) {
    cout << "[System] Result: " << dec << sim.result << " (0x" << hex << sim.result << ")" << endl;
    cout << "[System] Cycles: " << dec << sim.cycle << endl;
  } else {
    cout << "[System] " << (exiting ? "Interrupted" : "Timed out") << " at cycle " << sim.cycle << endl;
  }

  if (TRACE) { tracer->close(); tracer.reset(); }
  delete[] text_mem;
  return sim.finished ? 0 : 1;
}
