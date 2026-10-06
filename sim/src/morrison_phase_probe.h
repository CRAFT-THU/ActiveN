#pragma once

// Optional passive observations. No value here feeds simulated ready/valid,
// arbitration, memory timing, or workload state.
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>

namespace morrison_probe {
inline const char *directory() { return std::getenv("MORRISON_PHASE_PROBE"); }
inline bool enabled() { static const bool v = directory() != nullptr; return v; }
inline uint64_t cycle = 0;
inline uint32_t step = 0, phase = 0, mc = 0;
inline bool inPhase() { return step && (phase == 2 || phase == 3); }

// Long-form counters keep overlapping conditions explicit. All denominators
// are emitted alongside the numerators, with separate sampled/all-cycle keys.
struct Counters {
  using Key = std::tuple<uint32_t, uint32_t, uint32_t, std::string>;
  std::map<Key, uint64_t> values;
  std::string name;
  explicit Counters(std::string n) : name(std::move(n)) {}
  void add(uint32_t s, uint32_t p, uint32_t id, const std::string &k, uint64_t n=1) {
    if (n) values[{s,p,id,k}] += n;
  }
  void flush() const {
    if (!enabled()) return;
    std::ofstream f(std::filesystem::path(directory()) / name);
    f << "step,phase,id,key,count\n";
    for (const auto &[k,n] : values) {
      const auto &[s,p,id,label] = k;
      f << s << ',' << p << ',' << id << ',' << label << ',' << n << '\n';
    }
    if (!f) throw std::runtime_error("Cannot write Morrison phase counters");
  }
  ~Counters() { flush(); }
};
}
