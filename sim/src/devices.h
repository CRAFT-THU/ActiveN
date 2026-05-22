#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <random>

// Peripheral device: handles MMIO requests in the 0x40000000 region.
// Address map (relative to 0x40000000):
//   0x0: Write → end-of-simulation. Zero = success, any non-zero = failure code
//   0x4: Write 1 → start timer, Write 0 → stop timer
//        Timer increments every cycle while enabled.
//   0x8: Write → ASCII output
//   0xC: Write -> arbitrary 32-bit output, e.g. additional info / failure output
//   0x10: Read → next 32-bit RNG value, Write → reseed RNG
// Config ROM (relative to 0x40000000):
//   0x28000000: numPU    (word 0 of beat 0)
//   0x28000008: numMC    (word 2 of beat 0)
//   0x28000010: pusPerMC (word 4 of beat 0)
//   0x28000020: mcSize low 32 bits  (word 0 of beat 1)
//   0x28000024: mcSize high 32 bits (word 1 of beat 1)
struct PeripheralDevice {
  static constexpr uint32_t kDefaultSeed = 0x19260817u;

  std::optional<uint32_t> result = {};

  bool timer_enabled = false;
  uint64_t timer_count = 0;

  uint32_t rng_seed = kDefaultSeed;
  std::mt19937 rng;

  // Config ROM values (set by setConfig before simulation starts)
  uint32_t num_pu = 0;
  uint32_t num_mc = 0;
  uint32_t pus_per_mc = 0;
  uint64_t mc_size = 0;

  // Global override for RNG seed (set by main before any PeripheralDevice is used).
  static inline std::optional<uint32_t> global_seed_override;

  PeripheralDevice() : rng(effectiveSeed()) {
    rng_seed = effectiveSeed();
  }

  void setConfig(uint32_t npu, uint32_t nmc, uint64_t mcsz) {
    num_pu    = npu;
    num_mc    = nmc;
    pus_per_mc = npu / (nmc ? nmc : 1);
    mc_size   = mcsz;
  }

  static uint32_t effectiveSeed() {
    return global_seed_override.value_or(kDefaultSeed);
  }

  void reseed(uint32_t seed) {
    rng_seed = seed;
    rng.seed(seed);
  }

  // Call every cycle (before checking result).
  void tick() {
    if (timer_enabled) timer_count++;
  }

  // Handle a store to a peripheral address (addr relative to 0x40000000).
  void write(uint32_t addr, uint32_t data) {
    if (addr == 0) {
      result = {data};
    } else if (addr == 0x4) {
      timer_enabled = (data != 0);
    } else if (addr == 0x8) {
      // ASCII output
      std::cout << static_cast<char>(data & 0xFF);
      std::cout.flush();
    } else if (addr == 0xC) {
      std::cerr<<"[Periph] Output: "<<std::hex<<data<<std::dec<<std::endl;
    } else if (addr == 0x10) {
      reseed(data);
    }
  }

  // Handle a load from a peripheral address (addr relative to 0x40000000).
  uint32_t read(uint32_t addr) {
    if (addr == 0x10) return rng();
    // Config ROM at 0x28000000-0x28001000
    if (addr >= 0x28000000u && addr < 0x28001000u) {
      uint32_t off = addr - 0x28000000u;
      if (off == 0x00) return num_pu;
      if (off == 0x08) return num_mc;
      if (off == 0x10) return pus_per_mc;
      if (off == 0x20) return static_cast<uint32_t>(mc_size & 0xFFFFFFFFu);
      if (off == 0x24) return static_cast<uint32_t>(mc_size >> 32);
      return 0;
    }
    return 0;
  }
};

// Decodes a single-flit memory/peripheral request from ext_out.
// New encoding (single flit, 4 words):
//   tag 0x000 (load) / 0x001 (store)
//   data[0] = address (controller-local)
//   data[1] = 0(14) ## size(2) ## id(16)
//   data[2] = wdata (stores only)
//   data[3] = reserved
struct MemFlitDecoder {
  static constexpr uint16_t TAG_LOAD  = 0x00;
  static constexpr uint16_t TAG_STORE = 0x01;

  uint16_t tag = 0;
  uint16_t dst = 0;
  uint32_t data[4] = {};

  void set(uint16_t t, uint16_t d, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    tag = t;
    dst = d;
    data[0] = d0;
    data[1] = d1;
    data[2] = d2;
    data[3] = d3;
  }

  uint32_t addr()    const { return data[0]; }
  uint16_t id()      const { return data[1] & 0xFFFF; }
  uint16_t size()    const { return (data[1] >> 16) & 0x3; }
  uint32_t wdata()   const { return data[2]; }
  bool     isStore() const { return tag == TAG_STORE; }

  static bool isMemTag(uint16_t t) { return (t & 0xFF) == TAG_LOAD || (t & 0xFF) == TAG_STORE; }
};
