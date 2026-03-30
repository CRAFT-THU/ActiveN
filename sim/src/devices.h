#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>

// Peripheral device: handles MMIO requests in the 0x40000000 region.
// Address map (relative to 0x40000000):
//   0x0: Write → end-of-simulation (data = result value)
//   0x4: Write 1 → start timer, Write 0 → stop timer
//        Timer increments every cycle while enabled.
struct PeripheralDevice {
  bool finished = false;
  uint32_t result = 0;

  bool timer_enabled = false;
  uint64_t timer_count = 0;

  // Call every cycle (before checking finished).
  void tick() {
    if (timer_enabled) timer_count++;
  }

  // Handle a store to a peripheral address (addr relative to 0x40000000).
  void write(uint32_t addr, uint32_t data) {
    if (addr == 0) {
      finished = true;
      result = data;
    } else if (addr == 4) {
      timer_enabled = (data != 0);
    }
  }

  // Handle a load from a peripheral address (addr relative to 0x40000000).
  uint32_t read(uint32_t addr) {
    return 0;
  }
};

// Collects multi-flit memory/peripheral requests from event flits.
// 0xFF00 (load): 2 flits — addr, meta(id)
// 0xFF01 (store): 3 flits — addr, meta(size|id), wdata
struct FlitCollector {
  bool active = false;
  uint16_t tag = 0;       // 0xFF00 or 0xFF01
  uint16_t dst = 0;
  int flit_count = 0;
  uint32_t operands[3];   // addr, meta, wdata

  void reset() {
    active = false;
    flit_count = 0;
  }

  // Push a flit. Returns true when a complete request has been assembled.
  bool push(uint16_t t, uint16_t d, uint32_t data) {
    if (!active || tag != t || dst != d) {
      active = true;
      tag = t;
      dst = d;
      flit_count = 0;
    }
    operands[flit_count++] = data;
    int expected = (tag == 0xFF00) ? 2 : 3;
    return flit_count >= expected;
  }

  // Decode collected request fields
  uint32_t addr()    const { return operands[0]; }
  uint16_t id()      const { return operands[1] & 0xFFFF; }
  uint16_t size()    const { return (operands[1] >> 16) & 0xFFFF; }
  uint32_t wdata()   const { return operands[2]; }
  bool     isStore() const { return tag == 0xFF01; }
};
