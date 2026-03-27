/**
 * Single core simulation driver.
 *
 * Simulates a single core with a flat memory image.
 * - Loads a binary file as the initial memory image.
 * - Memory requests arrive as event flits on ext.out (tag 0xFF00 = load, 0xFF01 = store).
 * - Memory responses are sent on the mem bus (tag + 256-bit data).
 * - AM events (tag != 0xFF00/0xFF01) are captured and logged.
 * - End-of-simulation: tag == 0, dst == 0xFFFF, result in payload.
 *
 * Environment variables:
 *   MEOW_TEXT       - Path to the binary memory image
 *   MEOW_TRACE      - If non-empty, enable FST tracing
 *   MEOW_LOG        - If non-empty, enable logging
 *   MEOW_MAX_CYCLES - Maximum cycles before timeout (default: 1000000)
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
#include "verilated/rtl.h"

using namespace std;

static bool TRACE = false;
static bool LOG = false;
static bool exiting = false;

static const uint64_t RESET_LENGTH = 10;
static const uint32_t TEXT_BASE = 0x80000000ul;
static const int MEM_BUS_WORDS = 256 / 32; // 8 words per beat

static uint32_t *text_aligned = nullptr;
static size_t text_size = 0;

static std::unique_ptr<VerilatedFstC> tracer;

static void sighandler(int) {
  exiting = true;
}

// Memory response: tag + 256-bit data (8 words)
struct MemResponse {
  uint16_t tag;
  uint32_t data[MEM_BUS_WORDS]; // 8 words, little-endian
};

// State for collecting multi-flit memory requests
struct MemRequestCollector {
  bool active = false;
  uint16_t tag = 0;       // 0xFF00 = load, 0xFF01 = store
  uint16_t dst = 0;
  int flit_count = 0;
  uint32_t operands[3];   // addr, meta, wdata

  void reset() {
    active = false;
    flit_count = 0;
  }
};

struct SingleCoreSim {
  std::unique_ptr<rtl> core;
  uint64_t cycle = 0;
  std::deque<MemResponse> mem_resps;
  MemRequestCollector mem_collector;

  // Captured events
  struct Event {
    uint64_t cycle;
    uint16_t dst;
    uint32_t data;
    uint16_t tag;
  };
  std::vector<Event> events;

  bool finished = false;
  uint32_t result = 0;

  SingleCoreSim() {
    core.reset(new rtl("core"));
    core->cfg_hartid = 0;
  }

  ~SingleCoreSim() {
    core->final();
  }

  // Read 256-bit aligned block from backing memory at given byte address
  MemResponse readBlock(uint32_t addr, uint16_t resp_tag) {
    MemResponse resp;
    resp.tag = resp_tag;
    // Align to 32-byte boundary (256 bits)
    uint32_t aligned = addr & ~((MEM_BUS_WORDS * 4) - 1);
    for (int i = 0; i < MEM_BUS_WORDS; i++) {
      uint32_t byte_addr = aligned + i * 4;
      if (byte_addr >= TEXT_BASE && (byte_addr - TEXT_BASE) < text_size) {
        resp.data[i] = text_aligned[(byte_addr - TEXT_BASE) / 4];
      } else {
        resp.data[i] = 0;
      }
    }
    return resp;
  }

  void processMemRequest() {
    auto &c = mem_collector;
    uint32_t addr = c.operands[0] + TEXT_BASE; // local addr -> global addr
    uint16_t resp_tag = c.operands[1] & 0xFFFF;

    if (c.tag == 0xFF00) {
      // Load: operand[0] = addr, operand[1] = returning tag
      if (LOG) cout << "[Single] Mem load: addr=0x" << hex << addr
                    << " resp_tag=0x" << resp_tag << dec << endl;
      mem_resps.push_back(readBlock(addr, resp_tag));
    } else if (c.tag == 0xFF01) {
      // Store: operand[0] = addr, operand[1] = {size, returning_tag}, operand[2] = wdata
      uint16_t size = (c.operands[1] >> 16) & 0xFFFF;
      uint32_t wdata = c.operands[2];
      if (LOG) cout << "[Single] Mem store: addr=0x" << hex << addr
                    << " size=" << dec << size
                    << " data=0x" << hex << wdata
                    << " resp_tag=0x" << resp_tag << dec << endl;
      // Write to backing memory with byte-enable based on size
      if (addr >= TEXT_BASE && (addr - TEXT_BASE) < text_size) {
        uint32_t offset = (addr - TEXT_BASE) / 4;
        uint32_t old_val = text_aligned[offset];
        uint32_t byte_off = addr & 3;
        uint32_t mask = 0;
        switch (size) {
          case 0: mask = 0xFFu << (byte_off * 8); break;      // byte
          case 1: mask = 0xFFFFu << (byte_off * 8); break;    // half-word
          case 2: mask = 0xFFFFFFFFu; break;                   // word
          default: mask = 0xFFFFFFFFu; break;
        }
        text_aligned[offset] = (old_val & ~mask) | (wdata & mask);
      }
      // Store also gets a response (for completion)
      mem_resps.push_back(readBlock(addr, resp_tag));
    }
    c.reset();
  }

  void step() {
    ++cycle;

    if(cycle <= RESET_LENGTH) {
      core->reset = true;
      core->mem_valid = false;
      core->ext_out_ready = false;
      core->ext_in_valid = false;
      core->clock = true;
      core->eval();
      core->clock = false;
      core->eval();
      return;
    }

    core->reset = false;

    // Process ext_out from previous cycle
    if(core->ext_out_ready && core->ext_out_valid) {
      uint16_t dst = (uint16_t)core->ext_out_bits_dst;
      uint32_t data = core->ext_out_bits_data;
      uint16_t tag = (uint16_t)core->ext_out_bits_tag;

      // Check if this is a memory request flit
      if (tag == 0xFF00 || tag == 0xFF01) {
        auto &c = mem_collector;
        if (!c.active || c.tag != tag || c.dst != dst) {
          // Start new memory request
          c.active = true;
          c.tag = tag;
          c.dst = dst;
          c.flit_count = 0;
        }
        c.operands[c.flit_count++] = data;

        // Check if we have all flits
        int expected = (tag == 0xFF00) ? 2 : 3;
        if (c.flit_count >= expected) {
          processMemRequest();
        }
      } else {
        // Regular AM event
        Event ev = { cycle, dst, data, tag };
        events.push_back(ev);

        if(LOG) cout << "[Single] Event @" << dec << cycle
                     << ": dst=" << dst << " data=0x" << hex << data
                     << " tag=" << dec << tag << endl;

        if(tag == 0 && dst == 0xFFFF) {
          finished = true;
          result = data;
          if(LOG) cout << "[Single] Simulation finished with result: " << dec << data
                       << " (0x" << hex << data << ")" << endl;
        }
      }
    }

    // Advance mem response queue
    if(core->mem_valid) mem_resps.pop_front();

    // Set mem response signals
    if(mem_resps.empty()) {
      core->mem_valid = false;
      core->mem_bits_tag = 0;
      for (int i = 0; i < MEM_BUS_WORDS; i++) {
        core->mem_bits_data[i] = 0;
      }
    } else {
      core->mem_valid = true;
      core->mem_bits_tag = mem_resps.front().tag;
      // Pack 8 x 32-bit words into 256-bit data
      // Verilator represents wide signals as uint32_t arrays
      for (int i = 0; i < MEM_BUS_WORDS; i++) {
        core->mem_bits_data[i] = mem_resps.front().data[i];
      }
    }

    core->ext_in_valid = false;

    // Posedge
    core->clock = true;
    core->eval();

    if(TRACE) tracer->dump(cycle * 2);

    // Set ext_out_ready for next evaluation
    core->ext_out_ready = true;

    // Negedge
    core->clock = false;
    core->eval();
    if(TRACE) tracer->dump(cycle * 2 + 1);
  }
};

int main(int argc, char **argv) {
  auto trace_cfg = getenv("MEOW_TRACE");
  if(trace_cfg && trace_cfg[0] != '\0') {
    TRACE = true;
    cout << "[Single] Tracing enabled" << endl;
  }

  auto log_cfg = getenv("MEOW_LOG");
  if(log_cfg && log_cfg[0] != '\0') {
    LOG = true;
    cout << "[Single] Logging enabled" << endl;
  }

  auto text_path = getenv("MEOW_TEXT");
  if(!text_path || text_path[0] == '\0') {
    cerr << "Error: MEOW_TEXT environment variable not set" << endl;
    return 1;
  }

  uint64_t max_cycles = 1000000;
  auto max_cfg = getenv("MEOW_MAX_CYCLES");
  if(max_cfg && max_cfg[0] != '\0') max_cycles = strtoull(max_cfg, nullptr, 10);

  // Load binary into a larger backing memory
  ifstream text_input(text_path, ios::binary);
  if(!text_input) {
    cerr << "Error: cannot open " << text_path << endl;
    return 1;
  }
  text_input.seekg(0, ios::end);
  size_t file_size = text_input.tellg();
  if(file_size % 4 != 0) {
    cerr << "Error: input not 4-byte aligned" << endl;
    return 1;
  }
  // Allocate 16MB backing memory (zero-initialized)
  const size_t MEM_SIZE = 16 * 1024 * 1024;
  text_size = MEM_SIZE;
  char *text_mem = new (align_val_t(4)) char[MEM_SIZE]();
  text_input.seekg(0);
  text_input.read(text_mem, file_size);
  text_aligned = (uint32_t *)text_mem;
  cout << "[Single] Loaded " << file_size << " bytes from " << text_path
       << " (backing memory: " << MEM_SIZE / 1024 << " KB)" << endl;

  // Signal handler
  struct sigaction sig;
  sig.sa_handler = sighandler;
  sigemptyset(&sig.sa_mask);
  sig.sa_flags = 0;
  sigaction(SIGINT, &sig, NULL);

  // Setup tracing
  if(TRACE) {
    Verilated::traceEverOn(true);
    tracer.reset(new VerilatedFstC);
  }

  SingleCoreSim sim;

  if(TRACE) {
    sim.core->trace(tracer.get(), 128);
    tracer->open("./trace.fst");
  }

  // Run simulation
  cout << "[Single] Running simulation (max " << max_cycles << " cycles)..." << endl;
  while(!sim.finished && sim.cycle < max_cycles && !exiting) {
    sim.step();
  }

  if(sim.finished) {
    cout << "[Single] Simulation completed at cycle " << dec << sim.cycle << endl;
    cout << "[Single] Result: " << dec << sim.result
         << " (0x" << hex << sim.result << ")" << endl;
  } else if(exiting) {
    cout << "[Single] Simulation interrupted at cycle " << dec << sim.cycle << endl;
  } else {
    cout << "[Single] Simulation timed out at cycle " << dec << sim.cycle << endl;
  }

  cout << "[Single] Total events captured: " << sim.events.size() << endl;
  for(auto &ev : sim.events) {
    cout << "[Single]   @" << dec << ev.cycle
         << " dst=" << ev.dst << " tag=" << ev.tag
         << " data=0x" << hex << ev.data << dec << endl;
  }

  if(TRACE) {
    tracer->close();
    tracer.reset();
  }

  ::operator delete[](text_mem, align_val_t(4));
  return sim.finished ? 0 : 1;
}
