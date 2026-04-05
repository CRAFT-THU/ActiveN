#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <verilated_fst_c.h>

#include "soft_system_model.h"
#include "system_config.h"
#include "system_model.h"

using namespace std;

static bool exiting = false;

static void sighandler(int) { exiting = true; }

template <typename Event>
static string describeEvents(const vector<Event> &events) {
  if (events.empty()) return "[]";
  ostringstream out;
  out << '[';
  for (size_t i = 0; i < events.size(); ++i) {
    if (i != 0) out << ", ";
    out << events[i].describe();
  }
  out << ']';
  return out.str();
}

template <typename Event>
static bool compareEvents(uint64_t cycle, const char *label,
                          const vector<Event> &system_events,
                          const vector<Event> &soft_events) {
  if (system_events == soft_events) return true;

  cerr << "[CoSim] " << label << " mismatch at cycle " << cycle << '\n';
  cerr << "  system: " << describeEvents(system_events) << '\n';
  cerr << "  soft:   " << describeEvents(soft_events) << '\n';
  return false;
}

static bool compareState(const SystemModel &system, const SoftSystemModel &soft) {
  if (system.finished() != soft.finished()) {
    cerr << "[CoSim] finished mismatch at cycle " << system.cycle()
         << ": system=" << system.finished() << " soft=" << soft.finished() << '\n';
    return false;
  }

  if (system.timerCount() != soft.timerCount()) {
    cerr << "[CoSim] timer mismatch at cycle " << system.cycle()
         << ": system=" << system.timerCount() << " soft=" << soft.timerCount() << '\n';
    return false;
  }

  if (system.finished() && system.result() != soft.result()) {
    cerr << "[CoSim] result mismatch at cycle " << system.cycle()
         << ": system=0x" << hex << system.result()
         << " soft=0x" << soft.result() << dec << '\n';
    return false;
  }

  return true;
}

int main() {
  struct sigaction sig;
  sig.sa_handler = sighandler;
  sigemptyset(&sig.sa_mask);
  sig.sa_flags = 0;
  sigaction(SIGINT, &sig, nullptr);

  auto text_path = getenv("MEOW_TEXT");
  if (!text_path || text_path[0] == '\0') {
    cerr << "Error: MEOW_TEXT not set" << endl;
    return 1;
  }

  if (getenv("MEOW_MEM")) {
    cerr << "Error: sim_cosim currently supports only the flat-memory mode used by sim_soft" << endl;
    return 1;
  }

  uint64_t max_cycles = 10000000;
  auto max_cfg = getenv("MEOW_MAX_CYCLES");
  if (max_cfg && max_cfg[0] != '\0') max_cycles = strtoull(max_cfg, nullptr, 10);

  uint32_t data_addr = 0x80100000u;
  auto data_path = getenv("MEOW_DATA");
  auto data_addr_cfg = getenv("MEOW_DATA_ADDR");
  if (data_addr_cfg && data_addr_cfg[0] != '\0') data_addr = strtoul(data_addr_cfg, nullptr, 0);

  ModelImageConfig image_cfg{
    .text_path = text_path,
    .data_path = (data_path && data_path[0] != '\0') ? optional<string>(data_path) : nullopt,
    .data_addr = data_addr,
  };

  string error;
  if (!loadSystemModelImage(image_cfg, &error)) {
    cerr << error << endl;
    return 1;
  }
  if (!loadSoftSystemModelImage(image_cfg, &error)) {
    cerr << error << endl;
    unloadSystemModelImage();
    return 1;
  }

  auto log_cfg = getenv("MEOW_LOG");
  bool log_enabled = log_cfg && log_cfg[0] != '\0';
  setSystemModelLogging(log_enabled);
  setSoftSystemModelLogging(log_enabled);

  openSystemModelMemTrace(nullopt);
  openSoftSystemModelMemTrace(nullopt);

  unique_ptr<VerilatedFstC> tracer;
  auto trace_cfg = getenv("MEOW_TRACE");
  bool trace_enabled = trace_cfg && trace_cfg[0] != '\0';
  if (trace_enabled) {
    Verilated::traceEverOn(true);
    tracer.reset(new VerilatedFstC);
  }

  int num_pu = SYSTEM_CONFIG.numPU;
  int num_mc = SYSTEM_CONFIG.numMC;
  cout << "[CoSim] PUs=" << num_pu << " MCs=" << num_mc << endl;

  SystemModel system(num_pu, num_mc);
  SoftSystemModel soft(num_pu, num_mc);

  if (trace_enabled) {
    system.attachTrace(tracer.get(), 128);
    soft.attachTrace(tracer.get(), 64);
    tracer->open("./cosim_trace.fst");
  }

  cout << "[CoSim] Running lockstep (max " << max_cycles << " cycles)..." << endl;
  auto wall_start = chrono::steady_clock::now();

  bool matched = true;
  while (!exiting && system.cycle() < max_cycles) {
    system.stepPosedge();
    soft.stepPosedge();
    if (trace_enabled) tracer->dump(system.cycle() * 2);

    system.stepNegedge();
    soft.stepNegedge();
    if (trace_enabled) tracer->dump(system.cycle() * 2 + 1);

    if (system.cycle() != soft.cycle()) {
      cerr << "[CoSim] cycle mismatch: system=" << system.cycle() << " soft=" << soft.cycle() << '\n';
      matched = false;
      break;
    }

    if (!compareEvents(system.cycle(), "memory", system.lastMemTrace(), soft.lastMemTrace()) ||
        !compareEvents(system.cycle(), "peripheral", system.lastPeriphTrace(), soft.lastPeriphTrace()) ||
        !compareState(system, soft)) {
      matched = false;
      break;
    }

    if (system.finished()) break;
  }

  auto wall_end = chrono::steady_clock::now();
  double wall_secs = chrono::duration<double>(wall_end - wall_start).count();

  if (trace_enabled) tracer->close();
  unloadSystemModelImage();
  unloadSoftSystemModelImage();

  if (!matched) {
    if (trace_enabled) cerr << "[CoSim] Trace written to ./cosim_trace.fst" << endl;
    cerr << "[CoSim] Aborted at cycle " << system.cycle() << endl;
    return 1;
  }

  if (exiting) {
    cout << "[CoSim] Interrupted at cycle " << system.cycle() << endl;
    return 1;
  }

  if (system.finished()) {
    cout << "[CoSim] Matched through completion: result 0x" << hex << system.result() << dec
         << " in " << system.cycle() << " cycles" << endl;
  } else {
    cout << "[CoSim] Matched through cycle limit " << system.cycle() << endl;
  }
  cout << "[CoSim] Runtime: " << fixed << setprecision(3) << wall_secs << "s" << endl;
  return 0;
}