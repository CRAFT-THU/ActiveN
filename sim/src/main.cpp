/**
 * Unified simulation binary.
 *
 * Usage:
 *   sim_system --soft               Run the soft NoC backend only
 *   sim_system --hard               Run the hard RTL backend only
 *   sim_system --soft --hard        Cosimulation (both backends, lockstep)
 *
 * Environment variables:
 *   MEOW_TEXT        - Binary memory image (required)
 *   MEOW_DATA        - Optional data image loaded at MEOW_DATA_ADDR
 *   MEOW_DATA_ADDR   - Address for data image (default: 0x80100000)
 *   MEOW_MAX_CYCLES  - Max simulation cycles (default: 10000000)
 *   MEOW_MEM         - DRAMsim3 config file (optional)
 *   MEOW_MEM_LOG     - DRAMsim3 log directory (default: ".")
 *   MEOW_TRACE       - Enable FST tracing
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "system.h"
#include "sim_model_common.h"
#include "soft_system_model.h"
#include "hard_backend.h"

using namespace std;

int main(int argc, char **argv) {
  bool use_soft = false;
  bool use_hard = false;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--soft") == 0) use_soft = true;
    else if (strcmp(argv[i], "--hard") == 0) use_hard = true;
    else {
      cerr << "Unknown option: " << argv[i] << endl;
      cerr << "Usage: " << argv[0] << " [--soft] [--hard]" << endl;
      return 1;
    }
  }

  if (!use_soft && !use_hard) {
    cerr << "Error: specify at least one of --soft or --hard" << endl;
    return 1;
  }

  // Read environment
  auto text_path = getenv("MEOW_TEXT");
  if (!text_path || text_path[0] == '\0') {
    cerr << "Error: MEOW_TEXT not set" << endl;
    return 1;
  }

  uint64_t max_cycles = 10000000;
  auto max_cfg = getenv("MEOW_MAX_CYCLES");
  if (max_cfg && max_cfg[0] != '\0') max_cycles = strtoull(max_cfg, nullptr, 10);

  auto data_path = getenv("MEOW_DATA");
  uint32_t data_addr = 0x80100000;
  auto data_addr_cfg = getenv("MEOW_DATA_ADDR");
  if (data_addr_cfg && data_addr_cfg[0] != '\0') data_addr = strtoul(data_addr_cfg, nullptr, 0);

  // Build init files list: text_path [, data_path, data_addr]
  vector<string_view> dramInitFiles;
  dramInitFiles.push_back(text_path);
  string data_path_str;
  string data_addr_str;
  if (data_path && data_path[0] != '\0') {
    data_path_str = data_path;
    dramInitFiles.push_back(data_path_str);
    data_addr_str = to_string(data_addr);
    dramInitFiles.push_back(data_addr_str);
  }

  // Optional DRAMsim3
  optional<DRAMsim3Config> dram_cfg;
  string mem_cfg_str, mem_log_str;
  auto mem_cfg = getenv("MEOW_MEM");
  if (mem_cfg && mem_cfg[0] != '\0') {
    mem_cfg_str = mem_cfg;
    auto mem_log = getenv("MEOW_MEM_LOG");
    mem_log_str = (mem_log && mem_log[0] != '\0') ? mem_log : ".";
    dram_cfg = DRAMsim3Config{mem_cfg_str, mem_log_str};
  }

  System system(dramInitFiles, dram_cfg);

  if (use_hard) {
    auto hard = make_unique<HardSystemBackend>();
    // TODO: tracing support
    system.addBackend(move(hard));
    cerr << "[Main] Hard backend enabled" << endl;
  }

  if (use_soft) {
    // Load image for soft backend's scatter path (TODO: share with frontend)
    ModelImageConfig img_cfg{
      .text_path = text_path,
      .data_path = (data_path && data_path[0] != '\0') ? optional<string>(data_path) : nullopt,
      .data_addr = data_addr,
    };
    string img_err;
    if (!loadSoftSystemModelImage(img_cfg, &img_err)) {
      cerr << img_err << endl;
      return 1;
    }
    auto soft = make_unique<SoftSystemModel>(HARD_NUM_PU, HARD_NUM_MC);
    system.addBackend(move(soft));
    cerr << "[Main] Soft backend enabled" << endl;
  }

  system.run(max_cycles);

  return 0;
}
