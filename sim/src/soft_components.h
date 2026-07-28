#pragma once

#include <bit>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <array>
#include <type_traits>
#include <optional>
#include <span>
#include <vector>
#include <cassert>
#include <stdexcept>
#include "system.h"
#include "util.h"

// Fixed-length 1-based vector.
//
// Backed by std::vector<T>. For types that are neither movable nor
// copyable (e.g. Verilator-generated soft_rtl), wrap in std::unique_ptr.
template<typename T>
class IndexVector {
  std::vector<T> inner;

public:
  IndexVector() = default;
  IndexVector(size_t size) : inner(size) {}

  template<typename F>
    requires std::is_invocable_r_v<T, F, size_t>
  IndexVector(size_t size, F factory) {
    inner.reserve(size);
    for (size_t i = 0; i < size; ++i) inner.emplace_back(factory(i + 1));
  }

  T& operator[](size_t idx) { return inner[idx - 1]; }
  const T& operator[](size_t idx) const { return inner[idx - 1]; }
  size_t maxIndex() const { return inner.size(); }

  // Borrowed contiguous span [start, end), 1-based indexing.
  std::span<T> slice(size_t start, size_t end) {
    return std::span<T>(inner.data() + start - 1, end - start);
  }

  auto begin() { return inner.begin(); }
  auto end() { return inner.end(); }
  auto cbegin() { return inner.cbegin(); }
  auto cend() { return inner.cend(); }

  void reserve(size_t n) { inner.reserve(n); }

  template<typename ...Args>
    requires std::constructible_from<T, Args...>
  T& emplace_back(Args&&... args) {
    return inner.emplace_back(std::forward<Args>(args)...);
  }
};

template<typename T>
struct FixedLenQueue {
  size_t head = 0, tail = 0;
  size_t capacity;
  std::vector<T> data;
  bool maybeFull = false;

  FixedLenQueue(size_t capacity) : capacity(capacity), data(capacity) {}

  // Returns a reference to the front element, or nullopt if empty.
  std::optional<std::reference_wrapper<const T>> front() const {
    if (head == tail && !maybeFull) return std::nullopt;
    return std::cref(data[head]);
  }
  std::optional<std::reference_wrapper<T>> front() {
    if (head == tail && !maybeFull) return std::nullopt;
    return std::ref(data[head]);
  }
  bool pop() {
    bool canPop = head != tail || maybeFull;
    if (canPop) head = head + 1 == capacity ? 0 : head + 1;
    maybeFull = false;
    return canPop;
  }

  bool full() const {
    return head == tail && maybeFull;
  }
  bool empty() const {
    return head == tail && !maybeFull;
  }
  bool push(const T &t) {
    if (full()) return false;
    data[tail] = t;
    tail = tail + 1 == capacity ? 0 : tail + 1;
    maybeFull = true;
    return true;
  }

  size_t size() const {
    if (head == tail) return maybeFull ? capacity : 0;
    return tail > head ? tail - head : capacity - head + tail;
  }
};

// TODO: buffer prio because we actually has the extra space
struct Flit {
  uint16_t src;
  uint16_t dst;
  uint16_t tag; // Tag is 12-bit
  uint32_t data[4];

  inline uint8_t prio() const {
    return tag >> 10; // Top two bits
  }
};

template <typename F>
concept FlitArbInputs = std::is_invocable_r_v<std::optional<uint8_t>, F, size_t>;

class FlitArb {
  size_t _num_inputs;
  uint8_t _next_grant;

  static inline bool prio_greater(std::optional<uint8_t> a, std::optional<uint8_t> b) {
    return a.has_value() && (!b.has_value() || a.value() < b.value());
  }

public:
  FlitArb(size_t num_inputs) : _num_inputs(num_inputs),
      _next_grant(static_cast<uint8_t>(num_inputs > 1 ? 1 : 0)) {
    assert(num_inputs >= 1 && num_inputs <= 256); // num_inputs == 256 = auto wrapped-around by uint8_t
  }

  // Returns index
  template <FlitArbInputs Inputs>
  [[gnu::always_inline]] inline std::optional<uint8_t> peek(Inputs inputs) const {
    // Look for first valid since nextGrant
    uint8_t i = _next_grant;
    std::optional<uint8_t> selected = std::nullopt;
    uint8_t idx;

    do {
      auto input = inputs(i);
      if (prio_greater(input, selected)) {
        selected = input;
        idx = i;
      }

      i += 1;
      if (i == _num_inputs) i = 0;
    } while (i != _next_grant);

    if (selected.has_value()) {
      return idx;
    } else {
      // No valid found
      return std::nullopt;
    }
  }

  inline void commit(uint8_t selected) {
    _next_grant = selected + 1;
    if (_next_grant == _num_inputs) _next_grant = 0;
  }
};

template<typename T>
concept WithPrio = requires(T t) {
  { t.prio() } -> std::convertible_to<uint8_t>;
};

template<WithPrio T, std::size_t N_DEPTH>
class alignas(DESTRUCTIVE_INTERFERENCE_SIZE) FlitQueue {
  // We use a uint64_t as occupancy bitmask — N_DEPTH must be < 64
  static_assert(N_DEPTH < 64, "N_DEPTH must be < 64 (uint64_t bitmask limit)");

  std::unique_ptr<T> buffer[N_DEPTH];
  uint64_t occupied = 0;

  // Every cycle, the invoker should first peek & ask if it can enqueue,
  // use those data, and then do the actual enqueue & commit at the same cycle
  //
  // Use peek() to decide whether to actually deq

public:
  std::optional<size_t> peekIdx() const {
    if (occupied == 0) return std::nullopt;

    // Smaller priority number is higher
    size_t returning = std::countr_zero(occupied);
    uint64_t remaining = occupied & (occupied - 1);
    while (remaining != 0) {
      size_t idx = std::countr_zero(remaining);
      if (buffer[idx]->prio() < buffer[returning]->prio()) {
        returning = idx;
      }
      remaining = remaining & (remaining - 1);
    }

    return { returning };
  }

  const std::unique_ptr<T>& operator[](size_t idx) const {
    return buffer[idx];
  }

  size_t size() const {
    return std::popcount(occupied);
  }

  bool occupiedAt(size_t idx) const {
    return (occupied & (1ull << idx)) != 0;
  }

  bool canEnq(uint8_t prio) const {
    size_t cnt = std::popcount(occupied);
    size_t space = N_DEPTH - cnt;

    return space > prio;
  }

  std::optional<size_t> firstFreeSlot() const {
    size_t idx = std::countr_one(occupied);
    if (idx == N_DEPTH) return std::nullopt;
    return idx;
  }

  std::unique_ptr<T> take(size_t idx) {
    if constexpr (ASSERTIONS_ENABLED) {
      if (!(occupied & (1ull << idx))) throw std::runtime_error("take slot is not occupied");
      if (!buffer[idx]) throw std::runtime_error("take slot was already moved");
    }
    return std::move(buffer[idx]);
  }

  void place(size_t idx, std::unique_ptr<T> value) {
    if constexpr (ASSERTIONS_ENABLED) {
      if (!value) throw std::runtime_error("placing a null value");
      if (occupied & (1ull << idx)) throw std::runtime_error("place slot is occupied");
      if (buffer[idx]) throw std::runtime_error("place slot already contains a value");
    }
    buffer[idx] = std::move(value);
  }

  void commit(std::optional<size_t> deq, std::optional<size_t> enq) {
    if (deq) occupied &= ~(1ull << *deq);
    if (enq) occupied |= 1ull << *enq;
  }
};

struct RouterOutputToken {
  uint8_t input;
  uint8_t slot;
  uint8_t output;
  uint8_t prio;
};

// A routing table is a function that spits out a
// destination index (either a local ejection port, or a forward egress port)
template <typename RT, std::size_t Q_DEPTH>
class Router : std::is_invocable_r<size_t, RT, size_t> {
  RT _tbl;
  size_t _num_inputs, _num_outputs;
  std::vector<FlitQueue<Flit, Q_DEPTH>> _input_queues;
  std::vector<FlitArb> _output_arbs;
  // Scratch for Router::step (pre-allocated to _num_inputs).
  std::vector<std::optional<size_t>> _input_deqs_scratch;
  std::vector<std::optional<size_t>> _input_enqs;

private:
  std::optional<std::pair<size_t, size_t>> peek_iq_slot(size_t o_port) const __attribute__((always_inline)) {
    const FlitArb &arb = _output_arbs[o_port];
    // Build the input lambda
    auto inputs = [this, o_port](size_t idx) __attribute__((always_inline)) -> std::optional<uint8_t> {
      const auto &q = _input_queues[idx];
      auto slot = q.peekIdx();
      if (!slot.has_value()) return std::nullopt;

      const Flit &flit = *q[slot.value()];
      // Query routing table
      size_t fwd_to = _tbl(flit.dst);
      if (fwd_to != o_port) return std::nullopt;

      return flit.prio();
    };

    auto idx = arb.peek(inputs);
    if (!idx.has_value()) return std::nullopt;
    const auto &selected_queue = _input_queues[idx.value()];
    const auto slot = selected_queue.peekIdx();
    assert(slot.has_value());
    return {{ idx.value(), slot.value() }};
  }

public:
  static constexpr size_t MAX_PORTS = 6;
  using OutputTokens = std::array<std::optional<RouterOutputToken>, MAX_PORTS>;

  Router() : _num_inputs(0), _num_outputs(0) {}

  Router(RT tbl, size_t num_inputs, size_t num_outputs) : _tbl(tbl), _num_inputs(num_inputs), _num_outputs(num_outputs) {
    _input_queues.resize(num_inputs);
    _input_deqs_scratch.assign(num_inputs, std::nullopt);
    _input_enqs.assign(num_inputs, std::nullopt);
    for (size_t i = 0; i < num_outputs; ++i)
      // There are num_inputs queues
      _output_arbs.emplace_back(num_inputs);
  }

  std::optional<RouterOutputToken> peekToken(size_t o_port) const {
    auto selected = peek_iq_slot(o_port);
    if (!selected.has_value()) return std::nullopt;
    auto [iq, slot] = *selected;
    return RouterOutputToken {
      .input = static_cast<uint8_t>(iq),
      .slot = static_cast<uint8_t>(slot),
      .output = static_cast<uint8_t>(o_port),
      .prio = _input_queues[iq][slot]->prio(),
    };
  }

  const Flit *peek(const RouterOutputToken &token) const {
    return _input_queues[token.input][token.slot].get();
  }

  bool prepareEnq(size_t i_port, std::optional<uint8_t> prio) {
    auto &reservation = _input_enqs[i_port];
    reservation = std::nullopt;
    if (!prio.has_value()) return false;

    const auto &q = _input_queues[i_port];
    if (!q.canEnq(*prio)) return false;
    reservation = q.firstFreeSlot();
    return true;
  }

  std::unique_ptr<Flit> take(const RouterOutputToken &token) {
    return _input_queues[token.input].take(token.slot);
  }

  void place(size_t i_port, std::unique_ptr<Flit> flit) {
    if constexpr (ASSERTIONS_ENABLED) {
      if (!_input_enqs[i_port]) throw std::runtime_error("place without a reserved input slot");
    }
    _input_queues[i_port].place(*_input_enqs[i_port], std::move(flit));
  }

  void step(const OutputTokens &accepted) {
    auto &input_deqs = _input_deqs_scratch;
    std::fill(input_deqs.begin(), input_deqs.end(), std::nullopt);
    for (size_t o = 0; o < _num_outputs; ++o) {
      if (!accepted[o]) continue;
      const auto &token = *accepted[o];
      if constexpr (ASSERTIONS_ENABLED) {
        if (token.output != o) throw std::runtime_error("accepted token has wrong output");
        if (input_deqs[token.input]) throw std::runtime_error("one flit routed to multiple output ports");
      }
      input_deqs[token.input] = token.slot;
      _output_arbs[o].commit(token.input);
    }
    for (size_t i = 0; i < _num_inputs; ++i) {
      _input_queues[i].commit(input_deqs[i], _input_enqs[i]);
      _input_enqs[i] = std::nullopt;
    }
  }

  size_t totalFlits() const {
    size_t n = 0;
    for (auto &q : _input_queues) n += q.size();
    return n;
  }

  size_t numInputs() const noexcept { return _num_inputs; }

  size_t inputQueueSize(size_t input) const {
    return _input_queues.at(input).size();
  }
};
