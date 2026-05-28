#pragma once

#include <bit>
#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <optional>
#include <span>
#include <vector>
#include <cassert>
#include <stdexcept>

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
  inline std::optional<uint8_t> peek(Inputs inputs) const {
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
class FlitQueue {
  // We use a uint64_t as occupancy bitmask — N_DEPTH must be < 64
  static_assert(N_DEPTH < 64, "N_DEPTH must be < 64 (uint64_t bitmask limit)");
  T buffer[N_DEPTH];
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
      if (buffer[idx].prio() < buffer[returning].prio()) {
        returning = idx;
      }
      remaining = remaining & (remaining - 1);
    }

    return { returning };
  }

  const T& operator[](size_t idx) const {
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

  // Atomically enqueue a item, and also dequeue the selected item if it's accepted
  void step(std::optional<T> enq, std::optional<size_t> deq) {
    if (enq.has_value() && !canEnq(enq->prio())) throw std::runtime_error("queue overflow");
    if (deq.has_value() && !(occupied & (1ull << *deq))) throw std::runtime_error("deq slot is not occupied");

    // Select lowest zero, may be undefined if queue is full
    size_t enq_idx = std::countr_one(occupied);
    if (deq.has_value()) occupied &= ~(1ull << *deq);
    if (enq.has_value()) {
      buffer[enq_idx] = *enq;
      occupied |= (1ull << enq_idx);
    }
  }
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

private:
  std::optional<std::pair<size_t, size_t>> peek_iq_slot(size_t o_port) const {
    const FlitArb &arb = _output_arbs.at(o_port);
    // Build the input lambda
    auto inputs = [this, o_port](size_t idx) -> std::optional<uint8_t> {
      const auto &q = _input_queues.at(idx);
      auto slot = q.peekIdx();
      if (!slot.has_value()) return std::nullopt;

      const Flit &flit = q[slot.value()];
      // Query routing table
      size_t fwd_to = _tbl(flit.dst);
      if (fwd_to != o_port) return std::nullopt;

      return flit.prio();
    };

    auto idx = arb.peek(inputs);
    if (!idx.has_value()) return std::nullopt;
    const auto &selected_queue = _input_queues.at(idx.value());
    const auto slot = selected_queue.peekIdx();
    assert(slot.has_value());
    return {{ idx.value(), slot.value() }};
  }

public:
  Router() : _num_inputs(0), _num_outputs(0) {}

  Router(RT tbl, size_t num_inputs, size_t num_outputs) : _tbl(tbl), _num_inputs(num_inputs), _num_outputs(num_outputs) {
    _input_queues.resize(num_inputs);
    _input_deqs_scratch.assign(num_inputs, std::nullopt);
    for (size_t i = 0; i < num_outputs; ++i)
      // There are num_inputs queues
      _output_arbs.emplace_back(num_inputs);
  }

  // Peek ports
  std::optional<Flit> peek(size_t o_port) const {
    auto selected = peek_iq_slot(o_port);
    if (!selected.has_value()) return std::nullopt;
    auto [iq, slot] = *selected;
    return _input_queues.at(iq)[slot];
  }

  bool canEnq(size_t i_port, uint8_t prio) const {
    const auto &q = _input_queues.at(i_port);
    return q.canEnq(prio);
  }

  template<typename FE, typename FD>
  void step(FE enqs, FD deq_accepts)
    requires(std::is_invocable_r_v<std::optional<Flit>, FE, size_t> && std::is_invocable_r_v<bool, FD, size_t>)
    __attribute__((always_inline))
  {
    // Reverse-tracing the dequeue signal for each input queue.
    // Reuse pre-allocated scratch buffer; reset values only.
    auto &input_deqs = _input_deqs_scratch;
    std::fill(input_deqs.begin(), input_deqs.end(), std::nullopt);

    for (size_t o = 0; o < _num_outputs; ++o)
      if (deq_accepts(o)) {
        auto selected = peek_iq_slot(o);
        if (!selected.has_value()) throw std::runtime_error("deq_accepts is true but peek_iq_slot returns nullopt");
        auto [iq, slot] = *selected;
        if (input_deqs[iq].has_value()) throw std::runtime_error("one flit routed to multiple output ports");
        input_deqs[iq] = slot;

        // Update arbiter
        _output_arbs[o].commit(iq);
      }

    // Update input queues
    for (size_t i = 0; i < _num_inputs; ++i)
      _input_queues[i].step(enqs(i), input_deqs[i]);
  }

  size_t numInputs() const { return _num_inputs; }
  size_t numOutputs() const { return _num_outputs; }
  size_t totalFlits() const {
    size_t n = 0;
    for (auto &q : _input_queues) n += q.size();
    return n;
  }
  size_t queueSize(size_t i_port) const {
    return _input_queues.at(i_port).size();
  }
  // For debugging: iterate input queue contents.
  const FlitQueue<Flit, Q_DEPTH> &inputQueue(size_t i_port) const {
    return _input_queues.at(i_port);
  }
  // Routing table lookup (output port for a given destination).
  size_t lookup(uint16_t dst) const { return _tbl(dst); }
  // Returns the winning input port index for the given output port (-1 if none)
  int winningInputPort(size_t o_port) const {
    auto sel = peek_iq_slot(o_port);
    if (!sel.has_value()) return -1;
    return (int)sel->first;
  }
};
