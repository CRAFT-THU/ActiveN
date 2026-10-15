#include <ramulator/base/config.h>
#include <ramulator/base/factory.h>
#include <ramulator/base/request.h>
#include <ramulator/dram/device.h>
#include <ramulator/frontend/i_frontend.h>
#include <ramulator/memory_system/i_memory_system.h>

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Ramulator;

static void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

struct Device {
  DRAMDevice dram;
  explicit Device(const ConfigNode& config) {
    dram.init(DRAMSpec::create("HBM2", config));
    dram.set_channel_id(0);
  }
  bool ready(const std::string& command, const AddrVec_t& address, Clk_t tick) {
    int id = dram.m_spec->get_command_id(command);
    return dram.get_preq_command(id, address, tick) == id &&
           dram.check_timing(id, address, tick);
  }
  void issue(const std::string& command, const AddrVec_t& address, Clk_t tick) {
    require(ready(command, address, tick), "illegal test issue " + command);
    dram.issue_command(dram.m_spec->get_command_id(command), address, tick);
  }
  void boundary(const std::string& name, const std::string& command,
                const AddrVec_t& address, Clk_t tick) {
    require(!ready(command, address, tick - 1), name + " accepted one tick early");
    require(ready(command, address, tick), name + " rejected at required boundary");
    std::cout << "boundary " << name << " rejected=" << tick - 1
              << " accepted=" << tick << " memory_ticks\n";
  }
};

int main(int argc, char** argv) try {
  require(argc == 2, "usage: hbm2e_timing exported.yaml");
  auto full = Config::parse_config_file(argv[1]);
  auto controllers = full["memory_system"]["controllers"];
  require(controllers.size() == 1, "one modeled channel per MC required");
  auto config = controllers.seq().front();
  Device geometry(config);
  auto& spec = *geometry.dram.m_spec;
  require(spec.get_timing_value("tCK_ps") == 625, "wrong memory clock");
  require(spec.get_timing_value("rate") == 3200, "wrong pin data rate");
  require(spec.get_tx_bytes() == 32, "wrong transaction bytes");
  require(spec.organization.level_sizes == std::vector<int>({1, 2, 4, 4, 32768, 128}),
          "organization changed");
  uint64_t bytes = spec.organization.dq / 8;
  for (int count : spec.organization.level_sizes) bytes *= count;
  require(bytes == (1ull << 30), "wrong capacity");
  const std::array<std::pair<const char*, int>, 15> minimum_ps = {{
    {"nCL", 14000}, {"nRCDRD", 14000}, {"nRCDWR", 12000},
    {"nRP", 14000}, {"nRAS", 34000}, {"nWR", 16000}, {"nRTPL", 5000},
    {"nCWL", 5000}, {"nWTRS", 6000}, {"nWTRL", 8000}, {"nRRDS", 4000},
    {"nRRDL", 4000}, {"nFAW", 15000}, {"nRFC", 260000}, {"nRFCpb", 160000}
  }};
  for (auto [name, ps] : minimum_ps) {
    int actual_ps = spec.get_timing_value(name) * 625;
    require(actual_ps >= ps && actual_ps < ps + 625, std::string(name) + " physical delay");
  }
  require(spec.get_timing_value("nRC") == spec.get_timing_value("nRAS") +
          spec.get_timing_value("nRP"), "rounded row-cycle relationship");
  require(spec.get_timing_value("nRC") * 625 >= 48000, "row cycle too short");
  require(spec.get_timing_value("nRREFD") == 13, "refresh-to-other-bank delay");
  require(spec.get_timing_value("nREFI") * 625 == 3900000, "refresh interval");
  require(spec.get_timing_value("nREFIpb") * 625 * 32 == 3900000, "per-bank refresh interval");
  require(spec.get_timing_value("nBL") == 2 && spec.get_timing_value("nCCDS") == 2 &&
          spec.get_timing_value("nCCDL") == 4, "digital burst/CAS counts changed");
  require(spec.read_latency == 25, "read callback latency");
  require(config["dram"]["timing_constraints"].size() == 37, "constraint export shape");
  std::cout << "geometry bytes_per_mc=" << bytes << " transaction_bytes=" << spec.get_tx_bytes()
            << " tCK_ps=625 read_latency_ticks=" << spec.read_latency << '\n';

  const AddrVec_t a{0, 0, 0, 0, 0, 0};
  const AddrVec_t same_group{0, 0, 0, 1, 0, 0};
  const AddrVec_t different_group{0, 0, 1, 0, 0, 0};
  const AddrVec_t other_pc{0, 1, 0, 0, 0, 0};
  {
    Device d(config);
    d.issue("ACT", a, 100);
    d.boundary("row-command-occupancy", "ACT", other_pc, 102);
    d.boundary("ACT-to-RD", "RD", a, 124);
    d.boundary("ACT-to-WR", "WR", a, 121);
    d.boundary("ACT-to-PRE", "PREpb", a, 156);
    d.issue("PREpb", a, 156);
    d.boundary("PRE-to-ACT-and-row-cycle", "ACT", a, 178);
  }
  for (auto [destination, spacing] : {std::pair{same_group, 4}, std::pair{different_group, 2}}) {
    Device d(config);
    d.issue("ACT", a, 100);
    d.boundary("activation-spacing", "ACT", destination, 107);
    d.issue("ACT", destination, 107);
    d.issue("RD", a, 200);
    d.boundary(spacing == 4 ? "same-group-CAS" : "different-group-CAS",
               "RD", destination, 200 + spacing);
    d.boundary("read-to-write-turnaround", "WR", a, 219);
  }
  for (auto [destination, spacing] : {std::pair{same_group, 23}, std::pair{different_group, 20}}) {
    Device d(config);
    d.issue("ACT", a, 100);
    d.issue("ACT", destination, 107);
    d.issue("WR", a, 200);
    d.boundary(spacing == 23 ? "same-group-write-to-read" : "different-group-write-to-read",
               "RD", destination, 200 + spacing);
  }
  {
    Device d(config);
    d.issue("REFab", a, 100);
    d.boundary("all-bank-refresh", "ACT", a, 515);
  }
  {
    Device d(config);
    d.issue("REFpb", a, 100);
    d.boundary("same-bank-refresh", "ACT", a, 355);
    d.boundary("other-bank-refresh", "ACT", same_group, 112);
  }

  std::unique_ptr<IFrontEnd> frontend(Factory::create_frontend(full));
  std::unique_ptr<IMemorySystem> memory(Factory::create_memory_system(full));
  frontend->connect_memory_system(memory.get());
  memory->connect_frontend(frontend.get());
  require(memory->get_tCK() == 0.625f && memory->get_tx_bytes() == 32,
          "controller geometry differs from device");
  constexpr unsigned count = 256;
  std::array<bool, count> accepted{}, completed{};
  unsigned issued = 0, done = 0, rejected = 0;
  uint64_t tick = 0;
  for (; tick < 200000 && done < count; ++tick) {
    memory->tick();
    if (issued == count) continue;
    const unsigned id = issued;
    bool ok = frontend->receive_external_requests(
        id % 2 ? Request::Type::Write : Request::Type::Read,
        uint64_t(id) * 65536, -1, id,
        [&, id](Request& request) {
          require(request.ingress_id == int(id), "request identity changed");
          require(accepted[id] && !completed[id], "callback for rejected or completed request");
          completed[id] = true;
          ++done;
          std::cout << "complete id=" << id << " tick=" << tick << '\n';
        }, 32);
    std::cout << (ok ? "accept" : "reject") << " id=" << id << " tick=" << tick << '\n';
    if (ok) { accepted[id] = true; ++issued; }
    else ++rejected;
  }
  require(issued == count && done == count, "requests failed to drain");
  require(rejected > 0, "test did not exercise backpressure");
  std::cout << "request_summary accepted=" << issued << " completed=" << done
            << " rejected_attempts=" << rejected << " memory_ticks=" << tick << '\n';
  std::cout << "HBM2-derived extension timing checks passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "HBM2-derived extension timing check failed: " << error.what() << '\n';
  return 1;
}
