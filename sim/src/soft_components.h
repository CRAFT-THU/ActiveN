#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>
#include <optional>
#include <vector>
#include <cassert>
#include <stdexcept>

struct flit_t {
  uint16_t src;
  uint16_t dst;
  uint16_t tag; // Tag is 12-bit
  uint32_t data[4];

  inline uint8_t prio() const {
    return tag >> 10; // Top two bits
  }
};

template <typename F>
concept FlitArbInputs = std::is_invocable_r<std::optional<uint8_t>, F, size_t>;

class flit_arb {
  size_t _num_inputs;
  uint8_t _next_grant = 0; // FIXME: correctly initialize

  static inline bool prio_greater(std::optional<uint8_t> a, std::optional<uint8_t> b) {
    return a.has_value() && (!b.has_value() || a.value() < b.value());
  }

public:
  flit_arb(size_t num_inputs) : _num_inputs(num_inputs) {
    assert(num_inputs <= 256); // num_inputs == 256 = auto wrapped-around by uint8_t
  }

  // Returns index
  template <FlitArbInputs Inputs>
  inline std::optional<uint8_t> peek(Inputs inputs) const {
    // Look for first valid since next_grant
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
class flit_queue :
  // We want iterator in bitset, so let's use a uint64_t as bitmask
  std::enable_if_t<N_DEPTH < 64, void>
{
  T buffer[N_DEPTH];
  uint64_t occupied = 0;

  // Every cycle, the invoker should first peek & ask if it can enqueue,
  // use those data, and then do the actual enqueue & commit at the same cycle
  //
  // Use peek() to decide whether to actually deq

public:
  std::optional<size_t> peek_idx() const {
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

  bool can_enq(uint8_t prio) const {
    size_t cnt = std::popcount(occupied);
    size_t space = N_DEPTH - cnt;

    return space > prio;
  }

  // Atomically enqueue a item, and also dequeue the selected item if it's accepted
  void step(std::optional<T> enq, std::optional<size_t> deq) {
    if (enq.has_value() && !can_enq(enq->prio())) throw std::runtime_error("queue overflow");
    if (deq.has_value() && occupied & (1ull << *deq)) throw std::runtime_error("deq_accepts is true but deq slot is occupied");

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
class router : std::is_invocable_r<size_t, RT, size_t> {
  RT _tbl;
  size_t _num_inputs, _num_outputs;
  std::vector<flit_queue<flit_t, Q_DEPTH>> _input_queues;
  std::vector<flit_arb> _output_arbs;

private:
  std::optional<std::pair<size_t, size_t>> peek_iq_slot(size_t o_port) const {
    const flit_arb &arb = _output_arbs.at(o_port);
    // Build the input lambda
    auto inputs = [this, o_port](size_t idx) {
      const auto &queue = _input_queues.at(idx);
      auto slot = queue.peek_idx();
      if (!slot.has_value()) return std::nullopt;

      const flit_t &flit = queue[slot.value()];
      // Query routing table
      size_t fwd_to = _tbl(flit.dst);
      if (fwd_to != o_port) return std::nullopt;

      return flit.prio();
    };

    auto idx = arb.peek(inputs);
    if (!idx.has_value()) return std::nullopt;
    const auto &selected_queue = _input_queues.at(idx.value());
    const auto slot = selected_queue.peek_idx();
    assert(slot.has_value());
    return {{ idx.value(), slot.value() }};
  }

public:
  router(RT tbl, size_t num_inputs, size_t num_outputs) : _tbl(tbl), _num_inputs(num_inputs), _num_outputs(num_outputs) {
    _input_queues.resize(num_inputs);
    for (size_t i = 0; i < num_outputs; ++i)
      // There are num_inputs queues
      _output_arbs.emplace_back(num_inputs);
  }

  // Peek ports
  std::optional<flit_t> peek(size_t o_port) const {
    auto selected = peek_iq_slot(o_port);
    if (!selected.has_value()) return std::nullopt;
    auto [iq, slot] = *selected;
    return _input_queues.at(iq)[slot];
  }

  bool can_enq(size_t i_port, uint8_t prio) const {
    const auto &queue = _input_queues.at(i_port);
    return queue.can_enq(prio);
  }

  void step(std::vector<std::optional<flit_t>> enqs, std::vector<bool> deq_accepts) {
    if (enqs.size() != _num_inputs) throw std::runtime_error("wrong number of enqs");

    // Reverse-tracing the dequeue signal for each input queue
    std::vector<std::optional<size_t>> input_deqs;
    input_deqs.resize(_num_inputs);

    for (size_t o = 0; o < _num_outputs; ++o)
      if (deq_accepts.at(o)) {
        auto selected = peek_iq_slot(o);
        if (!selected.has_value()) throw std::runtime_error("deq_accepts is true but peek_iq_slot returns nullopt");
        auto [iq, slot] = *selected;
        if (input_deqs.at(iq).has_value()) throw std::runtime_error("one flit routed to multiple output ports");
        input_deqs.at(iq) = slot;

        // Update arbiter
        _output_arbs.at(o).commit(iq);
      }

    // Update input queues
    for (size_t i = 0; i < _num_inputs; ++i)
      _input_queues[i].step(enqs[i], input_deqs[i]);
  }
};
