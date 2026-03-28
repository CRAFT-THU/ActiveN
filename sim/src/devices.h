#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>

// Peripheral device: handles MMIO requests in the 0x40000000 region.
// Write to address 0x0 (= 0x40000000 physical) signals end-of-simulation.
struct PeripheralDevice {
  bool finished = false;
  uint32_t result = 0;

  // Handle a store to a peripheral address (addr relative to 0x40000000).
  void write(uint32_t addr, uint32_t data) {
    if (addr == 0) {
      finished = true;
      result = data;
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
