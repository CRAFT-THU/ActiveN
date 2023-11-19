#include <iostream>
#include <fstream>
#include <verilated_fst_c.h>
#include "verilated/rtl.h"

#include <cstdio>
#include <cstdlib>
#include <signal.h>

#include <deque>

using namespace std;

bool exiting = false;

void sighandler(int s) {
  exiting = true;
}

const uint64_t RESET_LENGTH = 10;
const uint32_t MEM_BASE = 0x80000000ul;

int main(int argc, char **argv) {
  if(argc != 2) {
    cout<<"Usage: sim <mem_file>"<<endl;
    return 1;
  }

  std::ifstream input(argv[1]);
  input.seekg(0, std::ios::end);
  size_t input_size = input.tellg();
  if(input_size % 4 != 0) {
    cout<<"Input not a 4-byte aligned file"<<endl;
    return 1;
  }
  char *mem = new (std::align_val_t(4)) char[input_size];
  input.seekg(0);
  input.read(mem, input_size);
  auto mem_aligned = (uint32_t *) mem;

  struct sigaction sig;
  sig.sa_handler = sighandler;
  sigemptyset(&sig.sa_mask);
  sig.sa_flags = 0;
  sigaction(SIGINT, &sig, NULL);

  VerilatedFstC tracer;
  Verilated::traceEverOn(true);

  rtl core;
  core.trace(&tracer, 128);

  tracer.open("./trace.fst");
  
  // System reset
  uint64_t clk = 0;
  core.reset = true;
  for(; clk < RESET_LENGTH * 2; ++clk) {
    cout<<"Reset"<<endl;
    core.clock = clk % 2 == 0;
    core.reset = true;
    core.eval();
    tracer.dump(clk);
  }
  core.reset = false;
  // Always ready for memory request
  core.mem_resp_valid = false;
  core.mem_req_ready = true;

  std::deque<uint32_t> mem_resps;

  // Working
  for(;;++clk) {
    if(clk % 10000 == 0) cout<<"."<<flush;
    if(clk % 500000 == 0) cout<<endl;
    bool clk_level = clk % 2 == 0;
    if(!clk_level) { // About to go into posedge, update queues
      if(core.mem_resp_valid) mem_resps.pop_front();
      if(core.mem_req_valid && core.mem_req_ready) {
        for(uint32_t i = 0; i < 1ull << core.mem_req_bits_burst; ++i) {
          uint32_t addr = i + core.mem_req_bits_addr;
          if(addr - MEM_BASE > input_size) mem_resps.push_back(0);
          else {
            uint32_t offset = (addr - MEM_BASE) >> 4;
            mem_resps.push_back(mem_aligned[offset]);
          }
        }
      }
    }

    core.clock = clk_level;
    core.eval();

    if(clk_level) { // After posedge, present new data
      if(mem_resps.empty()) {
        core.mem_resp_valid = false;
        core.mem_resp_bits_data = 0xdeadbeef;
      } else {
        core.mem_resp_valid = true;
        core.mem_resp_bits_data = mem_resps.front();
      }
    }

    core.eval();
    tracer.dump(clk);
    if(exiting) {
      cout<<"Exiting..."<<endl;
      break;
    }
  }

  core.final();
  tracer.close();

  cout<<"Dump finished"<<endl;
  return 0;
}