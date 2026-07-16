#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <stdexcept>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

inline constexpr std::size_t DESTRUCTIVE_INTERFERENCE_SIZE =
    std::hardware_destructive_interference_size;

inline void spin_loop_hint() noexcept {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  _mm_pause();

#elif defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64) || defined(_M_ARM)
  #if defined(_MSC_VER)
  __yield();
  #else
  __asm__ __volatile__("yield" ::: "memory");
  #endif

#elif defined(__riscv)
  // Encoding of the Zihintpause PAUSE hint. Older assemblers may not accept
  // the mnemonic even though implementations safely treat the hint as FENCE.
  __asm__ __volatile__(".4byte 0x0100000f" ::: "memory");

#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

class alignas(DESTRUCTIVE_INTERFERENCE_SIZE) SpinBarrier {
  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) ArrivalState {
    const std::size_t participants;
    std::atomic<std::size_t> remaining;

    explicit ArrivalState(std::size_t count)
        : participants(count), remaining(count) {}
  } arrival_;

  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) PhaseState {
    std::atomic<std::size_t> generation{0};
  } phase_;

  static_assert(alignof(ArrivalState) >= DESTRUCTIVE_INTERFERENCE_SIZE);
  static_assert(alignof(PhaseState) >= DESTRUCTIVE_INTERFERENCE_SIZE);

public:
  explicit SpinBarrier(std::size_t participants) : arrival_(participants) {
    if (participants == 0)
      throw std::invalid_argument("SpinBarrier requires at least one participant");
  }

  SpinBarrier(const SpinBarrier &) = delete;
  SpinBarrier &operator=(const SpinBarrier &) = delete;

  void arrive_and_wait() noexcept {
    const std::size_t generation =
        phase_.generation.load(std::memory_order_relaxed);

    if (arrival_.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      arrival_.remaining.store(arrival_.participants, std::memory_order_relaxed);
      phase_.generation.store(generation + 1, std::memory_order_release);
      return;
    }

    while (phase_.generation.load(std::memory_order_acquire) == generation)
      spin_loop_hint();
  }
};

static_assert(alignof(SpinBarrier) >= DESTRUCTIVE_INTERFERENCE_SIZE);
