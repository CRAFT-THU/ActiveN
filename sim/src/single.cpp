/**
 * Single core simulation driver.
 *
 * Simulates a single core with a flat memory image.
 * - Loads a binary file as the initial memory image (instruction + data).
 * - Responds to memory read requests from the core (instruction fetch).
 * - Captures and logs all outgoing events from the core (ext.out).
 * - Uses a special event (tag == 0, dst == 0xFFFF) to signal end of simulation,
 *   with the result carried in the event payload (ext.out.data).
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

static uint32_t *text_aligned = nullptr;
static size_t text_size = 0;

static std::unique_ptr<VerilatedFstC> tracer;

static void sighandler(int) {
  exiting = true;
}

struct SingleCoreSim {
  std::unique_ptr<rtl> core;
  uint64_t cycle = 0;
  std::deque<uint32_t> mem_resps;

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

  void step() {
    ++cycle;

    if(cycle <= RESET_LENGTH) {
      core->reset = true;
      core->mem_resp_valid = false;
      core->mem_req_ready = true;
      core->ext_out_ready = false;
      core->ext_in_valid = false;
      core->clock = true;
      core->eval();
      core->clock = false;
      core->eval();
      return;
    }

    core->reset = false;

    // Before posedge: advance queues based on handshake from previous cycle
    if(core->mem_resp_valid) mem_resps.pop_front();
    if(core->mem_req_valid && core->mem_req_ready) {
      uint32_t base_addr = core->mem_req_bits_addr;
      uint32_t burst_count = 1u << (uint32_t)core->mem_req_bits_burst;
      bool is_write = core->mem_req_bits_write;
      if(LOG) cout << "[Single] Mem " << (is_write ? "write" : "read")
                   << ": addr=0x" << hex << base_addr
                   << " burst=" << dec << burst_count << endl;
      for(uint32_t i = 0; i < burst_count; ++i) {
        uint32_t addr = base_addr + i * 4;
        if(is_write && addr >= TEXT_BASE && (addr - TEXT_BASE) < text_size) {
          uint32_t offset = (addr - TEXT_BASE) / 4;
          uint8_t wbe = core->mem_req_bits_wbe;
          uint32_t old_val = text_aligned[offset];
          uint32_t new_val = core->mem_req_bits_wdata;
          uint32_t result = old_val;
          for(int b = 0; b < 4; ++b) {
            if((wbe >> b) & 1)
              result = (result & ~(0xFFu << (b*8))) | (new_val & (0xFFu << (b*8)));
          }
          text_aligned[offset] = result;
        }
        // Always push a response (read data or write acknowledgment)
        if(addr >= TEXT_BASE && (addr - TEXT_BASE) < text_size) {
          uint32_t offset = (addr - TEXT_BASE) / 4;
          mem_resps.push_back(text_aligned[offset]);
        } else {
          mem_resps.push_back(0);
        }
      }
    }

    // Accept outgoing events from last cycle
    if(core->ext_out_ready && core->ext_out_valid) {
      uint16_t dst = (uint16_t)core->ext_out_bits_dst;
      uint32_t data = core->ext_out_bits_data;
      uint16_t tag = (uint16_t)core->ext_out_bits_tag;

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

    // Set input signals before posedge
    if(mem_resps.empty()) {
      core->mem_resp_valid = false;
      core->mem_resp_bits_data = 0xdeadbeef;
    } else {
      core->mem_resp_valid = true;
      core->mem_resp_bits_data = mem_resps.front();
    }
    core->mem_req_ready = true;
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
