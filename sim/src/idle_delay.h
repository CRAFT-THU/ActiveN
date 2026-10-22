#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>

// Resettable RegNext chain. Read output before committing the current edge.
class IdleDelay {
  const size_t delay;
  uint64_t history = 0;

 public:
  explicit IdleDelay(size_t cycles = 4) : delay(cycles) {
    if (cycles > 63) throw std::invalid_argument("idle delay must be in 0..63");
  }

  bool output(bool currentIdle) const noexcept {
    return delay == 0 ? currentIdle : ((history >> (delay - 1)) & 1u) != 0;
  }

  void step(bool currentIdle, bool reset) noexcept {
    history = reset ? 0 : (history << 1) | uint64_t(currentIdle);
  }
};
