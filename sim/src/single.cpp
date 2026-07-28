/**
 * Single core simulation driver.
 *
 * Simulates a single core with a flat memory image.
 * - Loads a binary file as the initial memory image.
 * - Memory requests arrive as single flits on ext.out (tag 0xF00 = load, 0xF01 = store).
 *   Flit layout: data[0]=addr, data[1]=size|id, data[2]=wdata, data[3]=reserved
 * - Memory responses are sent on the mem_unicast bus (id + one memory line).
 * - AM events (tag != 0xF00/0xF01) are captured and logged.
 * - End-of-simulation: store to peripheral address 0x40000000 (dst 0x8000).
 *
 * Usage:
 *   sim_single <text> [--trace] [--log] [--max-cycles N] [--rng-seed N]
 */

#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <vector>
#include <deque>
#include <signal.h>

#include <chrono>
#include <iomanip>
#include <verilated_fst_c.h>
#include "verilated/rtl.h"
#include "verilated/rtl___024root.h"

#include "include/argparse.h"
#include "devices.h"

using namespace std;

static bool TRACE = false;
static bool LOG = false;
static bool exiting = false;

static const uint64_t RESET_LENGTH = 10;
static const uint32_t TEXT_BASE = 0x80000000ul;
static_assert(CORE_MEM_BUS_WIDTH >= 64 && (CORE_MEM_BUS_WIDTH & (CORE_MEM_BUS_WIDTH - 1)) == 0);
static const int MEM_BUS_WORDS = CORE_MEM_BUS_WIDTH / 32;

static uint32_t *text_aligned = nullptr;
static size_t text_size = 0;

static std::unique_ptr<VerilatedFstC> tracer;

static void sighandler(int) {
  exiting = true;
}

// Memory response: id + one memory line
struct MemResponse {
  uint16_t id;
  uint32_t data[MEM_BUS_WORDS]; // little-endian
};

// State for collecting multi-flit memory requests
struct SingleCoreSim {
  std::unique_ptr<rtl> core;
  uint64_t cycle = 0;
  std::deque<MemResponse> mem_resps;
  MemFlitDecoder decoder;
  PeripheralDevice periph;

  // Captured events
  struct Event {
    uint64_t cycle;
    uint16_t dst;
    uint32_t data[4];
    uint16_t tag;
  };
  std::vector<Event> events;

  SingleCoreSim() {
    core.reset(new rtl("core"));
    core->cfg_hartid = 1;  // PU1
  }

  ~SingleCoreSim() {
    core->final();
  }

  // Read one aligned memory line from backing memory at the given byte address
  MemResponse readBlock(uint32_t addr, uint16_t resp_tag) {
    MemResponse resp;
    resp.id = resp_tag;
    // Align to a memory-line boundary
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
    uint32_t addr = decoder.addr() + TEXT_BASE; // local addr -> global addr
    uint16_t resp_tag = decoder.id();

    if (!decoder.isStore()) {
      // Load
      if (LOG) cerr << "[Single] Mem load: addr=0x" << hex << addr
                    << " resp_tag=0x" << resp_tag << dec << endl;
      mem_resps.push_back(readBlock(addr, resp_tag));
    } else {
      // Store
      uint16_t size = decoder.size();
      uint32_t wdata = decoder.wdata();
      if (LOG) cerr << "[Single] Mem store: addr=0x" << hex << addr
                    << " size=" << dec << size
                    << " data=0x" << hex << wdata
                    << " resp_tag=0x" << resp_tag << dec << endl;
      if (addr >= TEXT_BASE && (addr - TEXT_BASE) < text_size) {
        uint32_t offset = (addr - TEXT_BASE) / 4;
        uint32_t old_val = text_aligned[offset];
        uint32_t byte_off = addr & 3;
        uint32_t mask = 0;
        switch (size) {
          case 0: mask = 0xFFu << (byte_off * 8); break;
          case 1: mask = 0xFFFFu << (byte_off * 8); break;
          case 2: mask = 0xFFFFFFFFu; break;
          default: mask = 0xFFFFFFFFu; break;
        }
        text_aligned[offset] = (old_val & ~mask) | (wdata & mask);
      }
      mem_resps.push_back(readBlock(addr, resp_tag));
    }
  }

  void processPeriphRequest() {
    uint32_t addr = decoder.addr();
    uint16_t resp_tag = decoder.id();
    uint32_t rdata = 0;
    size_t word_idx = (addr & ((MEM_BUS_WORDS * sizeof(uint32_t)) - 1)) /
                      sizeof(uint32_t);

    bool bootrom = !decoder.isStore() && PeripheralDevice::isBootrom(addr);

    if (decoder.isStore()) {
      periph.write(addr, decoder.wdata());
      if (LOG) cerr << "[Single] Periph write: addr=0x" << hex << addr
                    << " data=0x" << decoder.wdata() << dec << endl;
    } else if (!bootrom) {
      rdata = periph.read(addr);
      if (LOG) cerr << "[Single] Periph read: addr=0x" << hex << addr
                    << " data=0x" << rdata << dec << endl;
    }

    MemResponse resp;
    resp.id = resp_tag;
    memset(resp.data, 0, sizeof(resp.data));
    if (bootrom) {
      // Boot ROM fetch/load: fill the whole beat (I-cache refills full lines).
      uint32_t beat_base =
          addr & ~((uint32_t)(MEM_BUS_WORDS * sizeof(uint32_t) - 1));
      periph.readBootrom(beat_base, reinterpret_cast<uint8_t *>(resp.data),
                         sizeof(resp.data));
    } else if (!decoder.isStore()) {
      resp.data[word_idx] = rdata;
    }
    mem_resps.push_back(resp);
  }

  void step() {
    ++cycle;

    if(cycle <= RESET_LENGTH) {
      core->reset = true;
      core->mem_unicast_valid = false;
      core->mem_broadcast_valid = false;
      core->ext_out_ready = false;
      core->ext_in_valid = false;
      core->clock = true;
      core->eval();
      if(TRACE) tracer->dump(cycle * 2);
      core->clock = false;
      core->eval();
      if(TRACE) tracer->dump(cycle * 2 + 1);
      return;
    }

    core->reset = false;

    // Posedge, update, dump
    core->clock = true;
    core->eval();
    if(TRACE) tracer->dump(cycle * 2);

    // Negedge, first, present memory response
    // Since we're presenting the memory responses first,
    // there is not single-cycle request-response loopback
    core->mem_broadcast_valid = false;
    if(mem_resps.empty()) {
      core->mem_unicast_valid = false;
      core->mem_unicast_bits_id = 0;
      core->mem_unicast_bits_ident = 0;
      for (int i = 0; i < MEM_BUS_WORDS; i++) {
        core->mem_unicast_bits_data[i] = 0;
      }
    } else {
      core->mem_unicast_valid = true;
      core->mem_unicast_bits_id = mem_resps.front().id;
      core->mem_unicast_bits_ident = 0;
      for (int i = 0; i < MEM_BUS_WORDS; i++) {
        core->mem_unicast_bits_data[i] = mem_resps.front().data[i];
      }
      mem_resps.pop_front();
    }

    // Second, accept outgoing events. They will be formally accepted
    // on the next posedge
    core->ext_in_valid = false;
    core->ext_out_ready = true;
    if(core->ext_out_valid) {
      uint16_t dst = (uint16_t)core->ext_out_bits_dst;
      uint32_t d0 = core->ext_out_bits_data_0;
      uint32_t d1 = core->ext_out_bits_data_1;
      uint32_t d2 = core->ext_out_bits_data_2;
      uint32_t d3 = core->ext_out_bits_data_3;
      uint16_t tag = (uint16_t)core->ext_out_bits_tag;

      if (MemFlitDecoder::isMemTag(tag)) {
        decoder.set(tag, dst, d0, d1, d2, d3);
        if (decoder.dst == 0x8000) {
          processPeriphRequest();
        } else {
          processMemRequest();
        }
      } else {
        // Regular AM event
        Event ev = { cycle, dst, {d0, d1, d2, d3}, tag };
        events.push_back(ev);

        if(LOG) cerr << "[Single] Event @" << dec << cycle
                     << ": dst=" << dst << " data={0x" << hex << d0
                     << ",0x" << d1 << ",0x" << d2 << ",0x" << d3
                     << "} tag=0x" << tag << dec << endl;
      }
    }

    core->clock = false;
    core->eval();
    if(TRACE) tracer->dump(cycle * 2 + 1);
  }
};

int main(int argc, char **argv) {
  argparse::ArgumentParser program("sim_single");

  program.add_argument("text")
    .help("Binary memory image");

  program.add_argument("--trace")
    .help("Enable FST tracing")
    .default_value(false)
    .implicit_value(true);

  program.add_argument("--log")
    .help("Enable verbose logging")
    .default_value(false)
    .implicit_value(true);

  program.add_argument("--max-cycles")
    .help("Max simulation cycles")
    .default_value(uint64_t(1000000))
    .scan<'u', uint64_t>();

  program.add_argument("--rng-seed")
    .help("RNG seed for peripheral device")
    .scan<'u', uint32_t>();

  program.add_argument("--bootrom")
    .help("Boot ROM image served at 0x60000000 (default: built-in)");

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &err) {
    cerr << err.what() << endl;
    cerr << program;
    return 1;
  }

  TRACE = program.get<bool>("--trace");
  LOG = program.get<bool>("--log");
  auto text_path = program.get<string>("text");
  uint64_t max_cycles = program.get<uint64_t>("--max-cycles");

  if (auto seed = program.present<uint32_t>("--rng-seed")) {
    PeripheralDevice::global_seed_override = *seed;
  }

  // Optional boot ROM override, loaded before the peripheral is constructed.
  if (auto bpath = program.present<string>("--bootrom")) {
    ifstream f(*bpath, ios::binary | ios::ate);
    if (!f) { cerr << "Error: cannot open bootrom image " << *bpath << endl; return 1; }
    streamsize sz = f.tellg();
    f.seekg(0);
    vector<uint8_t> buf(sz > 0 ? static_cast<size_t>(sz) : 0);
    if (sz > 0 && !f.read(reinterpret_cast<char *>(buf.data()), sz)) {
      cerr << "Error: cannot read bootrom image " << *bpath << endl; return 1;
    }
    PeripheralDevice::global_bootrom_override = std::move(buf);
  }

  if (TRACE) cerr << "[Single] Tracing enabled" << endl;
  if (LOG) cerr << "[Single] Logging enabled" << endl;

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
  cerr << "[Single] Loaded " << file_size << " bytes from " << text_path
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
  cerr << "[Single] Running simulation (max " << max_cycles << " cycles)..." << endl;
  auto wall_start = chrono::steady_clock::now();
  while(!sim.periph.result && sim.cycle < max_cycles && !exiting) {
    sim.step();
  }
  auto wall_end = chrono::steady_clock::now();
  double wall_secs = chrono::duration<double>(wall_end - wall_start).count();

  if(sim.periph.result) {
    cerr << "[Single] Result: " << dec << *sim.periph.result
         << " (0x" << hex << *sim.periph.result << ")" << dec << endl;
    cerr << "[Single] Cycles: " << dec << sim.cycle << endl;
  } else {
    cerr << "[Single] " << (exiting ? "Interrupted" : "Timed out") << " at cycle " << sim.cycle << endl;
  }
  cerr << "[Single] Speed: " << dec << (uint64_t)(sim.cycle / wall_secs) << " cycles/s" << endl;
  cerr << "[Single] Runtime: " << fixed << setprecision(3) << wall_secs << "s" << endl;

  cerr << "[Single] Total events captured: " << sim.events.size() << endl;
  for(auto &ev : sim.events) {
    cerr << "[Single]   @" << dec << ev.cycle
         << " dst=" << ev.dst << " tag=0x" << hex << ev.tag
         << " data={0x" << ev.data[0] << ",0x" << ev.data[1]
         << ",0x" << ev.data[2] << ",0x" << ev.data[3] << "}" << dec << endl;
  }

  if(TRACE) {
    tracer->close();
    tracer.reset();
  }

  ::operator delete[](text_mem, align_val_t(4));
  return sim.periph.result && *sim.periph.result == 0 ? 0 : 1;
}
