#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "system.h"

struct MemTraceEvent {
  uint64_t cycle = 0;
  int mc = 0;
  bool is_write = false;
  uint32_t local_addr = 0;
  uint16_t id = 0;

  bool operator==(const MemTraceEvent &other) const = default;

  std::string describe() const {
    std::ostringstream out;
    out << "mc=" << mc << ' ' << (is_write ? 'W' : 'R') << " addr=0x";
    out << std::hex << local_addr;
    out << std::dec << " id=" << id;
    return out.str();
  }
};

struct PeriphTraceEvent {
  uint64_t cycle = 0;
  uint16_t id = 0;
  uint32_t addr = 0;
  bool is_write = false;
  uint32_t wdata = 0;

  bool operator==(const PeriphTraceEvent &other) const = default;

  std::string describe() const {
    std::ostringstream out;
    out << (is_write ? "W" : "R") << " addr=0x" << std::hex << addr;
    if (is_write) out << " data=0x" << wdata;
    out << std::dec << " id=" << id;
    return out.str();
  }
};

class VerilatedFstC;

bool openSoftSystemModelMemTrace(const std::optional<std::string> &path, std::string *error = nullptr);
void setSoftSystemModelLogging(bool enabled);

class SoftSystemModel : public SystemBackend {
 public:
  SoftSystemModel(int pu, int mc);
  ~SoftSystemModel() override;
  SoftSystemModel(SoftSystemModel &&) noexcept;
  SoftSystemModel &operator=(SoftSystemModel &&) noexcept;
  SoftSystemModel(const SoftSystemModel &) = delete;
  SoftSystemModel &operator=(const SoftSystemModel &) = delete;

  void attachTrace(VerilatedFstC *tracer, int depth);

  // SystemBackend interface
  SystemConfig config() const override;
  void step(uint64_t cycle) override;
  void mem(const MemBusIn *, MemBusOut *) override;
  bool printStats(uint64_t cycles, bool final) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};