#include "idle_delay.h"
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

int main() {
  for (size_t delay : {0u, 1u, 4u, 63u}) {
    IdleDelay actual(delay);
    std::deque<bool> expected(delay, false);
    size_t checks = 0;
    for (size_t cycle = 1; cycle <= 512; ++cycle) {
      // Initial reset, isolated idle/busy pulses, long idle, and reset while
      // idle samples are still in flight. Output is read BEFORE each edge.
      const bool reset = cycle <= 10 || cycle == 91 || cycle == 230;
      const bool idle = cycle == 11 || (cycle >= 31 && cycle <= 190 &&
                                       cycle != 76) || cycle >= 220;
      const bool reference = delay == 0 ? idle : expected.front();
      if (actual.output(idle) != reference)
        throw std::runtime_error("wrong delay " + std::to_string(delay) +
                                 " before edge " + std::to_string(cycle));
      ++checks;
      actual.step(idle, reset);
      if (delay != 0) {
        expected.pop_front();
        expected.push_back(idle);
        if (reset) expected.assign(delay, false);
      }
    }
    std::cout << "PASS delay=" << delay << " pre_edge_checks=" << checks << '\n';
  }
  for (size_t invalid : {size_t{64}, size_t{256},
                         std::numeric_limits<size_t>::max()}) {
    bool rejected = false;
    try { IdleDelay bad(invalid); }
    catch (const std::invalid_argument &) { rejected = true; }
    if (!rejected) throw std::runtime_error("out-of-range delay accepted");
  }
  std::cout << "PASS invalid_delays=3\n";
}
