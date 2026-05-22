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
concept FlitArbInputs = std::is_invocable_r_v<std::optional<uint8_t>, F, size_t>;

class flit_arb {
  size_t _num_inputs;
  uint8_t _next_grant;

  static inline bool prio_greater(std::optional<uint8_t> a, std::optional<uint8_t> b) {
    return a.has_value() && (!b.has_value() || a.value() < b.value());
  }

public:
  flit_arb(size_t num_inputs) : _num_inputs(num_inputs),
      _next_grant(static_cast<uint8_t>(num_inputs > 1 ? 1 : 0)) {
    assert(num_inputs >= 1 && num_inputs <= 256); // num_inputs == 256 = auto wrapped-around by uint8_t
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
class flit_queue {
  // We use a uint64_t as occupancy bitmask — N_DEPTH must be < 64
  static_assert(N_DEPTH < 64, "N_DEPTH must be < 64 (uint64_t bitmask limit)");
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

  size_t size() const {
    return std::popcount(occupied);
  }

  bool occupied_at(size_t idx) const {
    return (occupied & (1ull << idx)) != 0;
  }
  uint64_t occupied_mask() const { return occupied; }

  bool can_enq(uint8_t prio) const {
    size_t cnt = std::popcount(occupied);
    size_t space = N_DEPTH - cnt;

    return space > prio;
  }

  // Atomically enqueue a item, and also dequeue the selected item if it's accepted
  void step(std::optional<T> enq, std::optional<size_t> deq) {
    if (enq.has_value() && !can_enq(enq->prio())) throw std::runtime_error("queue overflow");
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
class router : std::is_invocable_r<size_t, RT, size_t> {
  RT _tbl;
  size_t _num_inputs, _num_outputs;
  std::vector<flit_queue<flit_t, Q_DEPTH>> _input_queues;
  std::vector<flit_arb> _output_arbs;

private:
  std::optional<std::pair<size_t, size_t>> peek_iq_slot(size_t o_port) const {
    const flit_arb &arb = _output_arbs.at(o_port);
    // Build the input lambda
    auto inputs = [this, o_port](size_t idx) -> std::optional<uint8_t> {
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
  router() : _num_inputs(0), _num_outputs(0) {}

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

  size_t num_inputs_count() const { return _num_inputs; }
  size_t num_outputs_count() const { return _num_outputs; }
  size_t total_flits() const {
    size_t n = 0;
    for (auto &q : _input_queues) n += q.size();
    return n;
  }
  size_t queue_size(size_t i_port) const {
    return _input_queues.at(i_port).size();
  }
  // For debugging: iterate input queue contents.
  const flit_queue<flit_t, Q_DEPTH> &queue(size_t i_port) const {
    return _input_queues.at(i_port);
  }
  // Routing table lookup (output port for a given destination).
  size_t lookup(uint16_t dst) const { return _tbl(dst); }
  // Returns the winning input port index for the given output port (-1 if none)
  int winning_input_port(size_t o_port) const {
    auto sel = peek_iq_slot(o_port);
    if (!sel.has_value()) return -1;
    return (int)sel->first;
  }
  // Returns the arb's current next_grant for the given output port
  uint8_t arb_next_grant(size_t o_port) const {
    // flit_arb doesn't expose _next_grant directly; compute via a dummy peek
    // We need to expose it — for now just return 0 as placeholder.
    // (Will be added as a friend or accessor to flit_arb)
    return 0; // placeholder
  }
};

// ---------------------------------------------------------------------------
// RTL-aligned helpers (Phase A of the SOFT NoC refactor).
//
// Each soft module is a plain C++ class whose members are its state. Per cycle:
//   1. Driver calls module.peek_*() to read combinational outputs (computed
//      from current member values — Reg.Q equivalents).
//   2. Driver resolves ready/valid handshakes between modules using local
//      variables for each wire (bits, valid, ready triples are just three
//      locals; no wrapper needed).
//   3. Driver calls module.step(...) passing the resolved fires; step()
//      atomically updates the module's state members (D→Q at posedge).
//
// Modules that hold a register-equivalent compute `next_*` locals in the
// decide phase and apply them in step(). No reg<T> wrapper — a plain member
// variable does the job. No decoupled<T> wrapper — three locals or three
// function args do the job.
//
// Existing components (`flit_t`, `flit_arb`, `flit_queue`, `router`) above
// participate in the new model; the helpers below cover the remaining
// generic constructs from Chisel.

// A non-flow queue<T, N> matching Chisel `Queue(_, N)` (NOT pipe, NOT flow):
//   - enq.fire @ posedge K → entry visible to deq at state K (i.e., right
//     after posedge K). So minimum latency is 1 cycle.
//   - Combinational outputs (`enq_ready`, `deq_valid`, `deq_bits`, `count`)
//     reflect state at the start of the cycle (Reg.Q semantics).
//   - `step(enq_data, deq_fire)` commits both transitions atomically.
template <typename T, std::size_t N>
class queue {
  static_assert(N >= 1, "queue depth must be >= 1");
  std::array<T, N> buf_{};
  std::size_t head_ = 0;
  std::size_t count_ = 0;

 public:
  std::size_t count() const { return count_; }
  bool empty() const { return count_ == 0; }
  bool full() const { return count_ == N; }

  bool deq_valid() const { return count_ > 0; }
  const T &deq_bits() const {
    if (count_ == 0) throw std::runtime_error("queue::deq_bits empty");
    return buf_[head_];
  }
  bool enq_ready() const { return count_ < N; }

  void step(const std::optional<T> &enq_data, bool deq_fire) {
    if (deq_fire) {
      if (count_ == 0) throw std::runtime_error("queue::step deq with empty");
      head_ = (head_ + 1) % N;
      --count_;
    }
    if (enq_data) {
      if (count_ == N) throw std::runtime_error("queue::step enq with full");
      buf_[(head_ + count_) % N] = *enq_data;
      ++count_;
    }
  }
};

// A round-robin arbiter modeling Chisel `RRArbiter(T, N)`.
//   - peek(valid_mask): returns the chosen input index, starting from
//     next_grant_.
//   - commit(chosen): updates next_grant_ to (chosen + 1) % n_. Call only
//     when the chosen output fires.
class rr_arbiter {
  std::size_t n_;
  std::size_t next_grant_ = 0;

 public:
  explicit rr_arbiter(std::size_t n) : n_(n) {}

  template <std::size_t M>
  std::optional<std::size_t> peek(const std::array<bool, M> &valid_mask) const {
    if (n_ == 0) return std::nullopt;
    for (std::size_t off = 0; off < n_; ++off) {
      std::size_t idx = (next_grant_ + off) % n_;
      if (valid_mask[idx]) return idx;
    }
    return std::nullopt;
  }

  void commit(std::size_t chosen) { next_grant_ = (chosen + 1) % n_; }
  std::size_t next_grant() const { return next_grant_; }
};

// A fixed-priority arbiter modeling Chisel `Arbiter(T, N)` (in(0) is highest).
class priority_arbiter {
  std::size_t n_;

 public:
  explicit priority_arbiter(std::size_t n) : n_(n) {}

  template <std::size_t M>
  std::optional<std::size_t> peek(const std::array<bool, M> &valid_mask) const {
    for (std::size_t i = 0; i < n_; ++i)
      if (valid_mask[i]) return i;
    return std::nullopt;
  }

  std::optional<std::size_t> peek(const std::vector<bool> &valid_mask) const {
    for (std::size_t i = 0; i < n_ && i < valid_mask.size(); ++i)
      if (valid_mask[i]) return i;
    return std::nullopt;
  }
};

// ===========================================================================
// Phase B: MemIf hierarchy mirroring koneko.bus.MemIf{Base,DRAMIf,PeripheralIf}.
//
// Each class owns its Reg state as plain members. A single public `step()`
// per cycle takes a snapshot of all input bits/valids + any externally
// resolved fires (e.g. ringOut acceptance) and returns the cycle's output
// snapshot (used by the driver to resolve the next cycle's handshakes).
//
// Internal "wires" (scalarReq, scalarEject, ringSend, ...) are resolved
// inside step(); their fire signals are NOT exposed at the module boundary.
// Subclasses (`dram_if`, `mmio_if`) orchestrate their full per-cycle
// behavior, calling protected `apply_*` helpers on the base class to mutate
// shared state.
//
// `GlobalMemReq` / `GlobalMemResp` / `mem_line_t` come from system.h.
// ===========================================================================

#include "system.h"

// ---------------------------------------------------------------------------
// Bundle-equivalent data types.
// ---------------------------------------------------------------------------

enum class scalar_type_t : uint8_t { load = 0, store = 1 };

struct scalar_pending_t {
  scalar_type_t ty = scalar_type_t::load;
  uint16_t src = 0;
  uint16_t id = 0;
  uint32_t address = 0;
  uint8_t size = 0;
};

enum class bulk_type_t : uint8_t { scatter = 0, bulk_load = 1 };

struct bulk_pending_t {
  bulk_type_t ty = bulk_type_t::scatter;
  uint32_t base = 0;
  uint32_t end = 0;
  uint16_t src = 0;
  uint16_t tag = 0;
  uint32_t extras[2] = {0, 0};
  uint16_t issue_cnt = 0;
  uint16_t completed_cnt = 0;

  bool is_fully_issued() const {
    return static_cast<uint32_t>(issue_cnt) == ((end >> 5) - (base >> 5));
  }
  bool is_completed_next() const {
    return static_cast<uint32_t>(completed_cnt + 1) == ((end >> 5) - (base >> 5));
  }
  bool can_issue(uint32_t max_inflight) const {
    return static_cast<uint32_t>(issue_cnt) < static_cast<uint32_t>(completed_cnt) + max_inflight;
  }
  uint32_t issue_addr() const {
    return base + (static_cast<uint32_t>(issue_cnt) << 5);
  }
  bool is_broadcast() const { return ty == bulk_type_t::scatter; }
};

struct ring_resp_t {
  uint16_t dst = 0;
  uint16_t id = 0;
  mem_line_t data{};
};

struct bcast_beat_t {
  uint16_t pu = 0;
  uint16_t idx = 0;
  uint32_t data = 0;
};

struct bcast_line_t {
  uint16_t tag = 0;
  bcast_beat_t line[4] = {};
  uint32_t carried[2] = {0, 0};
};

// ---------------------------------------------------------------------------
// Flit parsers (mirror ScalarPending.fromFlit / BulkPending.initFromFlit).
// ---------------------------------------------------------------------------

struct scalar_parse_t {
  bool is_scalar = false;
  scalar_pending_t pending;
  uint32_t raw_data = 0;  // single 32-bit; HW replicates Fill(8, _) for wdata
};

inline scalar_parse_t parse_scalar_flit(const flit_t &f) {
  uint8_t tag_low = static_cast<uint8_t>(f.tag & 0xff);
  scalar_parse_t r;
  r.is_scalar = (tag_low == 0x00 || tag_low == 0x01);
  r.pending.ty = (tag_low == 0x00) ? scalar_type_t::load : scalar_type_t::store;
  r.pending.src = f.src;
  r.pending.address = f.data[0];
  r.pending.size = static_cast<uint8_t>((f.data[1] >> 16) & 0xf);
  r.pending.id = static_cast<uint16_t>(f.data[1] & 0xffff);
  r.raw_data = f.data[2];
  return r;
}

struct bulk_parse_t {
  bool is_bulk = false;
  bulk_pending_t pending;
};

inline bulk_parse_t parse_bulk_flit(const flit_t &f) {
  uint8_t tag_low = static_cast<uint8_t>(f.tag & 0xff);
  bulk_parse_t r;
  r.is_bulk = (tag_low == 0x10 || tag_low == 0x11);
  r.pending.ty = (tag_low == 0x10) ? bulk_type_t::scatter : bulk_type_t::bulk_load;
  r.pending.src = f.src;
  r.pending.base = f.data[0];
  r.pending.end = f.data[0] + (((f.data[1] >> 16) & 0xffff) << 5);
  r.pending.tag = static_cast<uint16_t>(f.data[1] & 0xffff);
  r.pending.extras[0] = f.data[2];
  r.pending.extras[1] = f.data[3];
  r.pending.issue_cnt = 0;
  r.pending.completed_cnt = 0;
  return r;
}

// Replicate a 32-bit word across an 8-lane 256-bit line (HW `Fill(8, _)`).
inline mem_line_t fill_line_from_word(uint32_t w) {
  mem_line_t out{};
  for (int lane = 0; lane < 8; ++lane) {
    out[lane * 4 + 0] = static_cast<uint8_t>((w >> 0) & 0xff);
    out[lane * 4 + 1] = static_cast<uint8_t>((w >> 8) & 0xff);
    out[lane * 4 + 2] = static_cast<uint8_t>((w >> 16) & 0xff);
    out[lane * 4 + 3] = static_cast<uint8_t>((w >> 24) & 0xff);
  }
  return out;
}

// ---------------------------------------------------------------------------
// I/O bundles passed across the module boundary each cycle.
// ---------------------------------------------------------------------------

struct mem_if_step_in {
  // ingress req ports (already arbitrated by FlitArb on the producer side?
  // No — flit_arb_ is inside mem_if_base, so per-port valid/bits are passed
  // verbatim).
  std::vector<std::optional<flit_t>> req;       // [num_req]
  // ringIn Decoupled input
  std::optional<ring_resp_t> ring_in;
  // mem.req Decoupled (external ready)
  bool mem_req_ready = false;
  // mem.resp Valid (external)
  std::optional<GlobalMemResp> mem_resp;
};

struct mem_if_step_out {
  // ingress req readies (one per port)
  std::vector<bool> req_ready;
  // ringIn ready
  bool ring_in_ready = false;
  // ringOut Decoupled output bits (valid iff has_value); external ready
  // determines whether step() should treat it as fired (caller passes
  // ring_out_ready in the fires struct).
  std::optional<ring_resp_t> ring_out;
  // mem.req Decoupled output bits (valid iff has_value)
  std::optional<GlobalMemReq> mem_req;
};

struct mem_if_step_fires {
  // Was ringOut accepted by downstream? (driver-resolved)
  bool ring_out_ready = false;
};

// dram_if has extra ports: unicast (Valid, always accepted) and broadcast
// (Decoupled, per-PU).
struct dram_if_step_out : mem_if_step_out {
  // per-cluster unicast Valid output (one entry per zone cluster)
  std::vector<std::optional<ring_resp_t>> unicast;
  // per-cluster broadcast Decoupled output bits
  std::vector<std::optional<bcast_line_t>> broadcast;
};

struct dram_if_step_fires : mem_if_step_fires {
  // per-cluster broadcast acceptance (driver-resolved)
  std::vector<bool> broadcast_ready;
};

// ---------------------------------------------------------------------------
// mem_if_base — shared scalar slot mgmt + ring forwarding + flit arbitration.
// ---------------------------------------------------------------------------

// Global cycle counter for debug instrumentation. Set by soft_backend driver
// before each per-cycle step. Used by SOFT_DBG_MEMIF logging.
extern uint64_t g_soft_dbg_cycle;

class mem_if_base {
 protected:
  std::size_t mc_idx_;
  std::size_t scalar_inflight_;
  std::size_t num_req_;
  bool silent_ = false;  // suppress debug logs (used by peek copies)

  // Regs
  std::vector<uint8_t> scalar_allocated_;
  std::vector<scalar_pending_t> scalar_pendings_;
  std::vector<uint8_t> scalar_issued_;
  std::vector<uint8_t> scalar_completed_;
  std::vector<mem_line_t> scalar_buffer_;
  queue<ring_resp_t, 2> ring_fwd_queue_;

  // Arbiters (have round-robin state)
  flit_arb flit_arb_;
  rr_arbiter scalar_req_arb_;

  // Subclass must define zone membership
  virtual bool is_local(uint16_t pu_id) const = 0;

  // -------- peek helpers (read Reg.Q only, no side effects) --------

  // Flit arbiter winner over per-port (valid+bits). Returns input index.
  std::optional<std::size_t> peek_flit_winner(
      const std::vector<std::optional<flit_t>> &reqs) const {
    return flit_arb_.peek([&](std::size_t i) -> std::optional<uint8_t> {
      if (i >= reqs.size() || !reqs[i]) return std::nullopt;
      return reqs[i]->prio();
    });
  }

  // scalarReq winner = scalar_req_arb_ over (allocated & !issued)
  std::optional<std::size_t> peek_scalar_req_slot() const {
    for (std::size_t off = 0; off < scalar_inflight_; ++off) {
      std::size_t s = (scalar_req_arb_.next_grant() + off) % scalar_inflight_;
      if (scalar_allocated_[s] && !scalar_issued_[s]) return s;
    }
    return std::nullopt;
  }

  // scalarReq.bits derived from slot's pending + buffer
  GlobalMemReq peek_scalar_req_bits(std::size_t slot) const {
    GlobalMemReq r{};
    const auto &p = scalar_pendings_[slot];
    r.id = static_cast<uint8_t>(slot);  // bit 7 = 0 → scalar
    r.size = p.size;
    r.addr = p.address;
    r.wdata = scalar_buffer_[slot];     // already Fill(8,_)-ed at parse time
    r.wbe = 0xFFFFFFFFu;                // HW: Fill(8, "b1111"_4) unconditionally
    r.write = (p.ty == scalar_type_t::store);
    return r;
  }

  // scalarEject slot = PriorityEncoderOH(allocated & completed)
  std::optional<std::size_t> peek_scalar_eject_slot() const {
    for (std::size_t s = 0; s < scalar_inflight_; ++s) {
      if (scalar_allocated_[s] && scalar_completed_[s]) return s;
    }
    return std::nullopt;
  }

  ring_resp_t peek_scalar_eject_bits(std::size_t slot) const {
    ring_resp_t r{};
    r.dst = scalar_pendings_[slot].src;
    r.id = scalar_pendings_[slot].id;
    r.data = scalar_buffer_[slot];
    return r;
  }

  // -------- apply helpers (mutate state — call inside subclass step) --------

  // Accept an ingress flit as a scalar request: alloc a free slot. Returns
  // the slot index allocated (caller must already have determined a slot is
  // available via PriorityEncoderOH on !scalar_allocated_).
  std::optional<std::size_t> peek_scalar_alloc_slot() const {
    for (std::size_t s = 0; s < scalar_inflight_; ++s) {
      if (!scalar_allocated_[s]) return s;
    }
    return std::nullopt;
  }

  void apply_scalar_alloc(std::size_t slot, const scalar_parse_t &parsed) {
    scalar_allocated_[slot] = 1;
    scalar_pendings_[slot] = parsed.pending;
    scalar_buffer_[slot] = fill_line_from_word(parsed.raw_data);
    scalar_issued_[slot] = 0;
    scalar_completed_[slot] = 0;
    if (!silent_ && getenv("SOFT_DBG_MEMIF")) {
      fprintf(stderr, "[memif%zu cy=%lu] ALLOC slot=%zu addr=0x%x src=%u id=%u\n",
              mc_idx_, g_soft_dbg_cycle, slot,
              parsed.pending.address, parsed.pending.src, parsed.pending.id);
    }
  }

  // scalarReq.fire: mark slot as issued, advance RR arb.
  void apply_scalar_req_fire(std::size_t slot) {
    scalar_issued_[slot] = 1;
    scalar_req_arb_.commit(slot);
    if (!silent_ && getenv("SOFT_DBG_MEMIF")) {
      fprintf(stderr, "[memif%zu cy=%lu] ISSUE slot=%zu addr=0x%x\n",
              mc_idx_, g_soft_dbg_cycle, slot, scalar_pendings_[slot].address);
    }
  }

  // scalarResp.valid: mark slot completed, capture rdata. (scalar resp id has
  // bit 7 == 0; caller must filter.)
  void apply_scalar_resp(uint8_t id, const mem_line_t &rdata) {
    if (id >= scalar_inflight_)
      throw std::runtime_error("mem_if_base: scalar resp id out of range");
    if (!scalar_allocated_[id])
      throw std::runtime_error("mem_if_base: scalar resp for non-allocated slot");
    if (scalar_completed_[id])
      throw std::runtime_error("mem_if_base: scalar resp for already-completed slot");
    scalar_completed_[id] = 1;
    scalar_buffer_[id] = rdata;
    if (!silent_ && getenv("SOFT_DBG_MEMIF")) {
      fprintf(stderr, "[memif%zu cy=%lu] RESP  slot=%u addr=0x%x\n",
              mc_idx_, g_soft_dbg_cycle, id, scalar_pendings_[id].address);
    }
  }

  // scalarEject.fire: free the slot.
  void apply_scalar_eject_fire(std::size_t slot) {
    if (!silent_ && getenv("SOFT_DBG_MEMIF")) {
      fprintf(stderr, "[memif%zu cy=%lu] EJECT slot=%zu addr=0x%x dst=%u\n",
              mc_idx_, g_soft_dbg_cycle, slot,
              scalar_pendings_[slot].address, scalar_pendings_[slot].src);
    }
    scalar_allocated_[slot] = 0;
    scalar_issued_[slot] = 0;
    scalar_completed_[slot] = 0;
  }

  void apply_flit_arb_commit(std::size_t input) {
    flit_arb_.commit(static_cast<uint8_t>(input));
  }

  // ringFwdQueue step: enq via ringPushArb winner (subclass-resolved), deq
  // via external ringOut acceptance.
  void apply_ring_fwd_queue_step(const std::optional<ring_resp_t> &enq,
                                 bool deq_fire) {
    ring_fwd_queue_.step(enq, deq_fire);
  }

 public:
  mem_if_base(std::size_t mc_idx, std::size_t scalar_inflight, std::size_t num_req)
      : mc_idx_(mc_idx),
        scalar_inflight_(scalar_inflight),
        num_req_(num_req),
        scalar_allocated_(scalar_inflight, 0),
        scalar_pendings_(scalar_inflight),
        scalar_issued_(scalar_inflight, 0),
        scalar_completed_(scalar_inflight, 0),
        scalar_buffer_(scalar_inflight),
        flit_arb_(num_req == 0 ? 1 : num_req),
        scalar_req_arb_(scalar_inflight) {
    if (scalar_inflight == 0)
      throw std::runtime_error("mem_if_base: scalar_inflight must be >= 1");
  }

  virtual ~mem_if_base() = default;

  std::size_t mc_idx() const { return mc_idx_; }
  std::size_t scalar_inflight() const { return scalar_inflight_; }
  std::size_t num_req() const { return num_req_; }

  // Debug accessors (state inspection only).
  uint64_t dbg_alloc_mask() const {
    uint64_t m = 0;
    for (std::size_t s = 0; s < scalar_inflight_; ++s) if (scalar_allocated_[s]) m |= (uint64_t)1 << s;
    return m;
  }
  uint64_t dbg_issued_mask() const {
    uint64_t m = 0;
    for (std::size_t s = 0; s < scalar_inflight_; ++s) if (scalar_issued_[s]) m |= (uint64_t)1 << s;
    return m;
  }
  uint64_t dbg_completed_mask() const {
    uint64_t m = 0;
    for (std::size_t s = 0; s < scalar_inflight_; ++s) if (scalar_completed_[s]) m |= (uint64_t)1 << s;
    return m;
  }
};

// ---------------------------------------------------------------------------
// dram_if — DRAMIf: adds bulk/scatter pipeline, broadcast distributor inputs,
// and per-cluster unicast distribution.
// ---------------------------------------------------------------------------

class dram_if : public mem_if_base {
  // Config
  int pu_start_;            // first PU ID in zone (1-based)
  int pu_end_;              // last PU ID in zone (1-based)
  std::size_t num_clusters_;
  std::size_t bulk_inflight_;
  std::size_t bulk_sub_id_width_;

  // Bulk regs
  uint8_t bulk_allocated_ = 0;
  bulk_pending_t bulk_pending_{};
  std::vector<mem_line_t> bulk_buffer_;       // [bulk_inflight]
  std::vector<uint8_t> bulk_completed_;       // [bulk_inflight]

  // Broadcast accepted mask: 1 bit per cluster, cleared on bcstStep (all set)
  std::vector<uint8_t> bcst_accepted_;

  static std::size_t ceil_log2(std::size_t n) {
    std::size_t r = 0;
    while ((std::size_t{1} << r) < n) ++r;
    return r;
  }

 public:
  dram_if(std::size_t mc_idx, int pu_start, int pu_end,
          std::size_t num_clusters, std::size_t scalar_inflight,
          std::size_t bulk_inflight)
      : mem_if_base(mc_idx, scalar_inflight, num_clusters),
        pu_start_(pu_start),
        pu_end_(pu_end),
        num_clusters_(num_clusters),
        bulk_inflight_(bulk_inflight),
        bulk_sub_id_width_(ceil_log2(bulk_inflight)),
        bulk_buffer_(bulk_inflight),
        bulk_completed_(bulk_inflight, 0),
        bcst_accepted_(num_clusters, 0) {
    if (num_clusters == 0)
      throw std::runtime_error("dram_if: num_clusters must be >= 1");
    if ((bulk_inflight & (bulk_inflight - 1)) != 0)
      throw std::runtime_error("dram_if: bulk_inflight must be a power of 2");
    if (bulk_sub_id_width_ + 1 > 8)
      throw std::runtime_error("dram_if: bulk id must fit in 8 bits");
    if (pu_end < pu_start)
      throw std::runtime_error("dram_if: pu_end < pu_start");
    if (static_cast<std::size_t>(pu_end - pu_start + 1) != num_clusters * 16)
      throw std::runtime_error("dram_if: PU range must equal num_clusters * 16");
  }

  bool is_local(uint16_t pu_id) const override {
    return static_cast<int>(pu_id) >= pu_start_ && static_cast<int>(pu_id) <= pu_end_;
  }

  std::size_t cluster_of(uint16_t pu_id) const {
    return static_cast<std::size_t>((static_cast<int>(pu_id) - pu_start_) >> 4);
  }

  // Bulk request id encoding: 1 ## 0^(7-w) ## beat[w-1:0]
  uint8_t bulk_id(uint16_t beat) const {
    return static_cast<uint8_t>(0x80u | (beat & ((1u << bulk_sub_id_width_) - 1u)));
  }

  // Computed bulkReq Decoupled output bits (valid iff returned has_value).
  std::optional<GlobalMemReq> peek_bulk_req() const {
    if (!bulk_allocated_) return std::nullopt;
    if (bulk_pending_.is_fully_issued()) return std::nullopt;
    if (!bulk_pending_.can_issue(static_cast<uint32_t>(bulk_inflight_))) return std::nullopt;
    GlobalMemReq r{};
    r.id = bulk_id(bulk_pending_.issue_cnt);
    r.size = 5;
    r.addr = bulk_pending_.issue_addr();
    r.wdata = {};
    r.wbe = 0;
    r.write = false;
    return r;
  }

  // bulkRespValid: a completed beat is waiting to broadcast/unicast.
  bool peek_bulk_resp_valid() const {
    return bulk_allocated_ &&
           bulk_completed_[bulk_pending_.completed_cnt] &&
           bulk_pending_.completed_cnt != bulk_pending_.issue_cnt;
  }

  // Decode 256-bit beat into 4 BcastBeat entries; field layout matches HARD
  // (verified in alignment.md FIX #5): word[0] = (idx<<16)|pu, word[1] = data.
  static void decode_bcast_line(const mem_line_t &data, bcast_beat_t out[4]) {
    for (int i = 0; i < 4; ++i) {
      uint32_t w0 = 0, w1 = 0;
      for (int b = 0; b < 4; ++b) {
        w0 |= static_cast<uint32_t>(data[i * 8 + b]) << (b * 8);
        w1 |= static_cast<uint32_t>(data[i * 8 + 4 + b]) << (b * 8);
      }
      out[i].pu = static_cast<uint16_t>(w0 & 0xffff);
      out[i].idx = static_cast<uint16_t>((w0 >> 16) & 0xffff);
      out[i].data = w1;
    }
  }

  // The full step: takes captured inputs + driver-resolved external fires,
  // returns this cycle's output snapshot, and atomically advances state.
  dram_if_step_out step(const mem_if_step_in &in, const dram_if_step_fires &fires) {
    dram_if_step_out out;
    out.req_ready.assign(num_req_, false);
    out.unicast.assign(num_clusters_, std::nullopt);
    out.broadcast.assign(num_clusters_, std::nullopt);
    if (fires.broadcast_ready.size() != num_clusters_)
      throw std::runtime_error("dram_if::step: broadcast_ready size mismatch");

    // ---- snapshot peeks (combinational on Reg.Q) ----
    auto fw = peek_flit_winner(in.req);
    std::optional<flit_t> flit_bits;
    if (fw) flit_bits = in.req[*fw];

    // Subclass acceptance: scalar (free slot) OR bulk (!bulk_allocated_)
    scalar_parse_t scalar_parsed{};
    bulk_parse_t bulk_parsed{};
    std::optional<std::size_t> scalar_alloc_slot;
    bool scalar_accept = false;
    bool bulk_accept = false;
    if (flit_bits) {
      scalar_parsed = parse_scalar_flit(*flit_bits);
      bulk_parsed = parse_bulk_flit(*flit_bits);
      if (scalar_parsed.is_scalar) {
        scalar_alloc_slot = peek_scalar_alloc_slot();
        scalar_accept = scalar_alloc_slot.has_value();
      }
      if (bulk_parsed.is_bulk) {
        bulk_accept = !bulk_allocated_;
      }
    }
    bool flit_ready = scalar_accept || bulk_accept;
    bool flit_fire = flit_bits.has_value() && flit_ready;
    if (fw) out.req_ready[*fw] = flit_ready;  // ready propagates to winner only

    // scalarReq peek (from RRArbiter winner)
    auto sreq_slot = peek_scalar_req_slot();
    std::optional<GlobalMemReq> sreq_bits;
    if (sreq_slot) sreq_bits = peek_scalar_req_bits(*sreq_slot);

    // bulkReq peek
    auto breq_bits = peek_bulk_req();

    // mem.req via reqArb (priority: scalar=0, bulk=1)
    bool sreq_chosen = false, breq_chosen = false;
    if (sreq_bits) {
      out.mem_req = sreq_bits;
      sreq_chosen = true;
    } else if (breq_bits) {
      out.mem_req = breq_bits;
      breq_chosen = true;
    }
    bool sreq_fire = sreq_chosen && in.mem_req_ready;
    bool breq_fire = breq_chosen && in.mem_req_ready;

    // scalarEject peek
    auto seject_slot = peek_scalar_eject_slot();
    std::optional<ring_resp_t> seject_bits;
    if (seject_slot) seject_bits = peek_scalar_eject_bits(*seject_slot);
    bool seject_dst_local = seject_bits && is_local(seject_bits->dst);

    // bulkRespValid → bulkUcst (only for non-broadcast) or broadcast
    bool bulk_resp_valid = peek_bulk_resp_valid();
    bool bulk_is_bcast = bulk_resp_valid && bulk_pending_.is_broadcast();
    bool bulk_is_ucst = bulk_resp_valid && !bulk_pending_.is_broadcast();

    // bulkUcstEject bits
    std::optional<ring_resp_t> bucst_bits;
    if (bulk_is_ucst) {
      ring_resp_t r{};
      r.dst = bulk_pending_.src;
      r.id = bulk_pending_.tag;
      r.data = bulk_buffer_[bulk_pending_.completed_cnt];
      bucst_bits = r;
    }
    bool bucst_dst_local = bucst_bits && is_local(bucst_bits->dst);

    // splitLocal: ringIn → ringRecv (local) + ringFwd (remote)
    std::optional<ring_resp_t> ring_recv_bits, ring_fwd_bits;
    if (in.ring_in) {
      if (is_local(in.ring_in->dst))
        ring_recv_bits = in.ring_in;
      else
        ring_fwd_bits = in.ring_in;
      if (!silent_ && getenv("SOFT_DBG_MEMIF")) {
        fprintf(stderr, "[memif%zu cy=%lu] RINGIN dst=%u id=0x%x %s\n",
                mc_idx_, g_soft_dbg_cycle, in.ring_in->dst, in.ring_in->id,
                is_local(in.ring_in->dst) ? "(local)" : "(fwd)");
      }
    }
    // splitLocal: scalarEject → scalarRecv (local) + scalarSend (remote)
    std::optional<ring_resp_t> scalar_recv_bits, scalar_send_bits;
    if (seject_bits) {
      if (seject_dst_local)
        scalar_recv_bits = seject_bits;
      else
        scalar_send_bits = seject_bits;
    }
    // splitLocal: bulkUcstEject → bulkUcstRecv + bulkUcstSend
    std::optional<ring_resp_t> bucst_recv_bits, bucst_send_bits;
    if (bucst_bits) {
      if (bucst_dst_local)
        bucst_recv_bits = bucst_bits;
      else
        bucst_send_bits = bucst_bits;
    }

    // ringSend = scalarSendArb(scalarSend=in0, bulkUcstSend=in1) — priority
    std::optional<ring_resp_t> ring_send_bits;
    bool ring_send_from_scalar = false, ring_send_from_bucst = false;
    if (scalar_send_bits) {
      ring_send_bits = scalar_send_bits;
      ring_send_from_scalar = true;
    } else if (bucst_send_bits) {
      ring_send_bits = bucst_send_bits;
      ring_send_from_bucst = true;
    }

    // ringSendGated: valid only when ringFwdQueue is empty
    bool ring_fwd_queue_empty = (ring_fwd_queue_.count() == 0);
    std::optional<ring_resp_t> ring_send_gated_bits;
    if (ring_send_bits && ring_fwd_queue_empty) ring_send_gated_bits = ring_send_bits;

    // ringPushArb (priority): in(0)=ringFwd, in(1)=ringSendGated
    std::optional<ring_resp_t> ring_push_bits;
    bool push_from_fwd = false, push_from_gated = false;
    if (ring_fwd_bits) {
      ring_push_bits = ring_fwd_bits;
      push_from_fwd = true;
    } else if (ring_send_gated_bits) {
      ring_push_bits = ring_send_gated_bits;
      push_from_gated = true;
    }
    bool ring_fwd_queue_can_enq = !ring_fwd_queue_.full();
    bool ring_push_fire = ring_push_bits && ring_fwd_queue_can_enq;
    bool ring_fwd_fire = push_from_fwd && ring_push_fire;
    bool ring_send_gated_fire = push_from_gated && ring_push_fire;
    bool ring_send_fire = ring_send_gated_fire;
    bool scalar_send_fire = ring_send_from_scalar && ring_send_fire;
    bool bucst_send_fire = ring_send_from_bucst && ring_send_fire;

    // ringOut = ringFwdQueue.deq
    if (ring_fwd_queue_.deq_valid()) out.ring_out = ring_fwd_queue_.deq_bits();
    bool ring_out_fire = out.ring_out && fires.ring_out_ready;

    // ringIn.ready = (local ? ringRecv.ready : ringFwd.ready)
    // ringFwd.ready = ring_fwd_queue_can_enq && in(0) chosen in arb (which is
    // always priority over gated). For ringRecv.ready, we need per-cluster
    // localDist arbiter resolution — see unicast section below.

    // ---- Per-cluster unicast distribution (localDist + arb) ----
    // For each cluster ci, arbiter has priority order: ringDist(ci)=in(0),
    // scalarDist(ci)=in(1), bulkUcstDist(ci)=in(2). Output is unicast(ci).
    // unicast is Valid (no backpressure: arb.io.out.ready := true.B in RTL).
    std::vector<uint8_t> ring_recv_to_cluster(num_clusters_, 0);
    std::vector<uint8_t> scalar_recv_to_cluster(num_clusters_, 0);
    std::vector<uint8_t> bucst_recv_to_cluster(num_clusters_, 0);
    if (ring_recv_bits) ring_recv_to_cluster[cluster_of(ring_recv_bits->dst)] = 1;
    if (scalar_recv_bits) scalar_recv_to_cluster[cluster_of(scalar_recv_bits->dst)] = 1;
    if (bucst_recv_bits) bucst_recv_to_cluster[cluster_of(bucst_recv_bits->dst)] = 1;

    bool ring_recv_fire = false, scalar_recv_fire = false, bucst_recv_fire = false;
    for (std::size_t ci = 0; ci < num_clusters_; ++ci) {
      if (ring_recv_to_cluster[ci]) {
        out.unicast[ci] = ring_recv_bits;
        ring_recv_fire = true;
      } else if (scalar_recv_to_cluster[ci]) {
        out.unicast[ci] = scalar_recv_bits;
        scalar_recv_fire = true;
      } else if (bucst_recv_to_cluster[ci]) {
        out.unicast[ci] = bucst_recv_bits;
        bucst_recv_fire = true;
      }
    }

    // ringIn.ready: derived from whether ringIn would be accepted at its
    // routed destination this cycle.
    if (in.ring_in) {
      if (is_local(in.ring_in->dst))
        out.ring_in_ready = ring_recv_fire;
      else
        out.ring_in_ready = ring_push_fire && push_from_fwd;
    }

    // scalarEject.fire combines: scalar_recv_fire || scalar_send_fire
    bool scalar_eject_fire = scalar_recv_fire || scalar_send_fire;
    // bulkUcstEject.fire combines: bucst_recv_fire || bucst_send_fire
    bool bulk_ucst_step = bucst_recv_fire || bucst_send_fire;

    // ---- Broadcast (per-cluster Decoupled bcstDists) ----
    bcast_line_t bcast_line{};
    if (bulk_is_bcast) {
      bcast_line.tag = bulk_pending_.tag;
      decode_bcast_line(bulk_buffer_[bulk_pending_.completed_cnt], bcast_line.line);
      bcast_line.carried[0] = bulk_pending_.extras[0];
      bcast_line.carried[1] = bulk_pending_.extras[1];
    }
    std::vector<uint8_t> bcst_accept_now(num_clusters_, 0);
    for (std::size_t ci = 0; ci < num_clusters_; ++ci) {
      if (bulk_is_bcast && !bcst_accepted_[ci]) {
        out.broadcast[ci] = bcast_line;
        if (fires.broadcast_ready[ci]) bcst_accept_now[ci] = 1;
      }
    }
    // bcstStep = (bcst_accepted | bcst_accept_now).andR
    bool bcst_step = bulk_is_bcast;
    if (bcst_step) {
      for (std::size_t ci = 0; ci < num_clusters_; ++ci) {
        if (!(bcst_accepted_[ci] || bcst_accept_now[ci])) {
          bcst_step = false;
          break;
        }
      }
    }
    bool bulk_step = bcst_step || bulk_ucst_step;

    // ---- mem.resp routing: scalar (id[7]==0) or bulk (id[7]==1) ----
    std::optional<std::pair<uint8_t, mem_line_t>> scalar_resp_apply;
    std::optional<std::pair<uint16_t, mem_line_t>> bulk_resp_apply;
    if (in.mem_resp) {
      uint8_t id = in.mem_resp->id;
      if ((id & 0x80) == 0) {
        scalar_resp_apply = std::make_pair(id, in.mem_resp->data);
      } else {
        uint16_t beat = id & ((1u << bulk_sub_id_width_) - 1u);
        bulk_resp_apply = std::make_pair(beat, in.mem_resp->data);
      }
    }

    // ---- COMMIT all state mutations atomically ----

    // Flit ingress: alloc / bulk register / arb commit
    if (flit_fire) {
      apply_flit_arb_commit(*fw);
      if (scalar_parsed.is_scalar && scalar_accept) {
        apply_scalar_alloc(*scalar_alloc_slot, scalar_parsed);
      }
      if (bulk_parsed.is_bulk && bulk_accept) {
        bulk_allocated_ = 1;
        bulk_pending_ = bulk_parsed.pending;
      }
    }
    if (!silent_ && getenv("SOFT_DBG_MEMIF") && flit_bits) {
      fprintf(stderr, "[memif%zu cy=%lu] FLIT_IN port=%zu src=%u dst=%u tag=0x%x fire=%d scalar=%d bulk=%d acc_s=%d acc_b=%d\n",
              mc_idx_, g_soft_dbg_cycle, fw ? *fw : 99u,
              flit_bits->src, flit_bits->dst, flit_bits->tag,
              flit_fire, scalar_parsed.is_scalar,
              bulk_parsed.is_bulk, scalar_accept, bulk_accept);
    }

    // scalarReq fire
    if (sreq_fire) apply_scalar_req_fire(*sreq_slot);

    // bulkReq fire: bump issueCnt, clear bulkCompleted slot for next beat
    if (breq_fire) {
      bulk_completed_[bulk_pending_.issue_cnt] = 0;
      bulk_pending_.issue_cnt = static_cast<uint16_t>(bulk_pending_.issue_cnt + 1);
    }

    // scalar / bulk resp
    if (scalar_resp_apply) {
      apply_scalar_resp(scalar_resp_apply->first, scalar_resp_apply->second);
    }
    if (bulk_resp_apply) {
      uint16_t beat = bulk_resp_apply->first;
      bulk_buffer_[beat] = bulk_resp_apply->second;
      bulk_completed_[beat] = 1;
    }

    // scalarEject fire → free slot
    if (scalar_eject_fire) apply_scalar_eject_fire(*seject_slot);

    // bulk step: advance completedCnt; if isCompletedNext, deallocate
    if (bulk_step) {
      bool finishing = bulk_pending_.is_completed_next();
      bulk_pending_.completed_cnt = static_cast<uint16_t>(bulk_pending_.completed_cnt + 1);
      if (finishing) {
        bulk_allocated_ = 0;
      }
    }

    // Broadcast accepted mask update
    if (bulk_is_bcast) {
      if (bcst_step) {
        std::fill(bcst_accepted_.begin(), bcst_accepted_.end(), 0);
      } else {
        for (std::size_t ci = 0; ci < num_clusters_; ++ci) {
          if (bcst_accept_now[ci]) bcst_accepted_[ci] = 1;
        }
      }
    }

    // ringFwdQueue: enq if ringPushArb fired, deq if ringOut fired
    std::optional<ring_resp_t> enq_data;
    if (ring_push_fire) enq_data = ring_push_bits;
    apply_ring_fwd_queue_step(enq_data, ring_out_fire);

    return out;
  }

  // Non-mutating peek: returns the same `out` snapshot that step() would
  // produce. Implemented by copying *this and calling step() on the copy.
  // Outputs do not depend on `fires` (verified — fires only affect commit),
  // so we pass dummy fires (no acceptance). The driver uses this to resolve
  // ring chain + broadcast back-pressure before calling step().
  dram_if_step_out peek(const mem_if_step_in &in) const {
    dram_if copy(*this);
    copy.silent_ = true;
    dram_if_step_fires dummy;
    dummy.broadcast_ready.assign(num_clusters_, false);
    dummy.ring_out_ready = false;
    return copy.step(in, dummy);
  }
};

// ---------------------------------------------------------------------------
// mmio_if — PeripheralIf: scalar slots + config ROM; no bulk; no local
// distributor (everything goes to the ring).
// ---------------------------------------------------------------------------

class mmio_if : public mem_if_base {
  // Config ROM: 32-byte-aligned beat addr → 256-bit data
  std::vector<std::pair<uint32_t, mem_line_t>> config_rom_;

 public:
  mmio_if(std::size_t scalar_inflight)
      : mem_if_base(99, scalar_inflight, 1) {}

  bool is_local(uint16_t /*pu_id*/) const override { return false; }

  void add_config_rom_entry(uint32_t aligned_addr, const mem_line_t &data) {
    if ((aligned_addr & 0x1f) != 0)
      throw std::runtime_error("mmio_if: config ROM entry must be 32B-aligned");
    config_rom_.emplace_back(aligned_addr, data);
  }

  std::optional<mem_line_t> config_lookup(uint32_t addr) const {
    uint32_t beat = addr & ~0x1fu;
    for (const auto &e : config_rom_) {
      if (e.first == beat) return e.second;
    }
    return std::nullopt;
  }

  mem_if_step_out step(const mem_if_step_in &in, const mem_if_step_fires &fires) {
    mem_if_step_out out;
    out.req_ready.assign(num_req_, false);

    // ---- snapshot peeks ----
    auto fw = peek_flit_winner(in.req);
    std::optional<flit_t> flit_bits;
    if (fw) flit_bits = in.req[*fw];

    scalar_parse_t scalar_parsed{};
    std::optional<std::size_t> scalar_alloc_slot;
    bool scalar_accept = false;
    if (flit_bits) {
      scalar_parsed = parse_scalar_flit(*flit_bits);
      if (scalar_parsed.is_scalar) {
        scalar_alloc_slot = peek_scalar_alloc_slot();
        scalar_accept = scalar_alloc_slot.has_value();
      }
    }
    bool flit_ready = scalar_accept;
    bool flit_fire = flit_bits.has_value() && flit_ready;
    if (fw) out.req_ready[*fw] = flit_ready;

    // scalarReq peek
    auto sreq_slot = peek_scalar_req_slot();
    std::optional<GlobalMemReq> sreq_bits;
    if (sreq_slot) sreq_bits = peek_scalar_req_bits(*sreq_slot);

    // Config ROM hit check on scalarReq.bits.addr (combinational)
    std::optional<mem_line_t> config_data;
    if (sreq_bits) config_data = config_lookup(sreq_bits->addr);
    bool config_hit = config_data.has_value();
    bool config_grant = !in.mem_resp.has_value();  // configGrant = !mem.resp.valid
    bool config_resp_valid = sreq_bits.has_value() && config_hit && config_grant;

    // mem.req = scalarReq if !configHit
    if (sreq_bits && !config_hit) out.mem_req = sreq_bits;
    bool mem_req_fire = out.mem_req.has_value() && in.mem_req_ready;
    bool scalar_req_grant = (config_hit ? config_grant : in.mem_req_ready);
    bool sreq_fire = sreq_bits.has_value() && scalar_req_grant;

    // scalarResp = mem.resp ? mem.resp : configResp (mem.resp takes priority)
    std::optional<std::pair<uint8_t, mem_line_t>> scalar_resp_apply;
    if (in.mem_resp) {
      // mem.resp always scalar in periph (no bulk)
      scalar_resp_apply = std::make_pair(in.mem_resp->id, in.mem_resp->data);
    } else if (config_resp_valid) {
      scalar_resp_apply = std::make_pair(sreq_bits->id, *config_data);
    }

    // scalarEject peek
    auto seject_slot = peek_scalar_eject_slot();
    std::optional<ring_resp_t> seject_bits;
    if (seject_slot) seject_bits = peek_scalar_eject_bits(*seject_slot);
    // In periph, is_local is always false → splitLocal sends all to scalarSend
    std::optional<ring_resp_t> scalar_send_bits = seject_bits;

    // ringSend = scalarSend (no bulkUcst in periph)
    std::optional<ring_resp_t> ring_send_bits = scalar_send_bits;
    bool ring_fwd_queue_empty = (ring_fwd_queue_.count() == 0);
    std::optional<ring_resp_t> ring_send_gated_bits;
    if (ring_send_bits && ring_fwd_queue_empty) ring_send_gated_bits = ring_send_bits;

    // splitLocal(ringIn): all !is_local → ringFwd
    std::optional<ring_resp_t> ring_fwd_bits = in.ring_in;
    // assert(!ringRecv.valid) — periph never receives local rings

    // ringPushArb priority: in(0)=ringFwd, in(1)=ringSendGated
    std::optional<ring_resp_t> ring_push_bits;
    bool push_from_fwd = false, push_from_gated = false;
    if (ring_fwd_bits) {
      ring_push_bits = ring_fwd_bits;
      push_from_fwd = true;
    } else if (ring_send_gated_bits) {
      ring_push_bits = ring_send_gated_bits;
      push_from_gated = true;
    }
    bool ring_fwd_queue_can_enq = !ring_fwd_queue_.full();
    bool ring_push_fire = ring_push_bits.has_value() && ring_fwd_queue_can_enq;
    bool ring_fwd_fire = push_from_fwd && ring_push_fire;
    bool ring_send_gated_fire = push_from_gated && ring_push_fire;
    bool scalar_send_fire = ring_send_gated_fire;

    out.ring_in_ready = in.ring_in.has_value() && ring_fwd_fire;

    if (ring_fwd_queue_.deq_valid()) out.ring_out = ring_fwd_queue_.deq_bits();
    bool ring_out_fire = out.ring_out.has_value() && fires.ring_out_ready;

    // ---- COMMIT ----

    if (flit_fire) {
      apply_flit_arb_commit(*fw);
      apply_scalar_alloc(*scalar_alloc_slot, scalar_parsed);
    }

    if (sreq_fire) apply_scalar_req_fire(*sreq_slot);

    if (scalar_resp_apply) {
      apply_scalar_resp(scalar_resp_apply->first, scalar_resp_apply->second);
    }

    if (scalar_send_fire) apply_scalar_eject_fire(*seject_slot);

    std::optional<ring_resp_t> enq_data;
    if (ring_push_fire) enq_data = ring_push_bits;
    apply_ring_fwd_queue_step(enq_data, ring_out_fire);

    if (!silent_ && getenv("SOFT_DBG_MMIO")) {
      size_t qc = ring_fwd_queue_.count();
      uint32_t completed_mask = 0;
      for (size_t s = 0; s < scalar_inflight_; ++s)
        if (scalar_allocated_[s] && scalar_completed_[s])
          completed_mask |= (1u << s);
      fprintf(stderr,
              "[periph cy=%lu] qcnt=%zu sej={%s slot=%d dst=%u} "
              "ringIn={%s dst=%u} push={%s from=%s} qdeq={%s dst=%u fire=%d} "
              "compMask=0x%x\n",
              g_soft_dbg_cycle, qc,
              seject_bits ? "v" : "_",
              seject_slot ? (int)*seject_slot : -1,
              seject_bits ? seject_bits->dst : 0,
              in.ring_in ? "v" : "_",
              in.ring_in ? in.ring_in->dst : 0,
              ring_push_fire ? "fire" : "_",
              push_from_fwd ? "fwd" : (push_from_gated ? "gated" : "none"),
              out.ring_out ? "v" : "_",
              out.ring_out ? out.ring_out->dst : 0,
              (int)ring_out_fire,
              completed_mask);
    }

    (void)mem_req_fire;  // currently unused; sreq_fire covers state changes
    return out;
  }

  // Non-mutating peek: same convention as dram_if::peek.
  mem_if_step_out peek(const mem_if_step_in &in) const {
    mmio_if copy(*this);
    copy.silent_ = true;
    mem_if_step_fires dummy;
    dummy.ring_out_ready = false;
    return copy.step(in, dummy);
  }
};

// ---------------------------------------------------------------------------
// Phase C: Router & Distributor.
//
// Router: the existing `router<RT, Q_DEPTH>` template (above) already mirrors
// the RTL `Router[D]` module — per-input FlitQueue (depth Q_DEPTH, priority
// admission via `space > prio`), per-output FlitArb, and a peek/step API:
//
//   can_enq(i_port, prio)  → ingress(i).ready
//   peek(o_port)           → egress(j).valid + bits
//   step(enqs, deq_accepts) atomically commits enq for each input port and
//                           dequeue for each output port whose downstream
//                           accepted (deq_accepts[j] = egress(j).fire).
//
// No new router class is needed. The driver-side `SoftRouter` wraps it with
// topology metadata (ingress/inject port ordering, dst→port table).
//
// Distributor (per cluster, 16 PUs): mirrors `Distributor.scala`. Holds a
// 1-deep pipe broadcast queue (Chisel `Queue(_, 1, pipe = true)`) and a
// 16-bit `bcstAccepted` register. Unicast is combinational fan-out to the
// addressed PU.
// ---------------------------------------------------------------------------

struct distributor_step_in {
  std::optional<ring_resp_t> unicast;       // Valid<RingResp>
  std::optional<bcast_line_t> broadcast;    // Decoupled<BcastLine>.bits (valid iff has_value)
};

struct distributor_pu_unicast_t {
  uint16_t id = 0;
  mem_line_t data{};
};

struct distributor_step_out {
  // Per-PU unicast: 16-bit valid mask, shared bits (RTL `out.unicast.resp`).
  uint16_t unicast_valids = 0;
  distributor_pu_unicast_t unicast_bits{};

  // Per-PU broadcast: 16-bit valid mask, shared bits.
  uint16_t broadcast_valids = 0;
  bcast_line_t broadcast_bits{};

  // Back-pressure to upstream `in.broadcast.ready` (Decoupled).
  bool broadcast_enq_ready = false;
};

struct distributor_step_fires {
  // Per-PU broadcast acceptances (RTL `out.broadcast.readies`).
  uint16_t broadcast_readies = 0;
};

class distributor {
  int pu_start_;

  // 1-deep pipe queue Reg. With pipe=true semantics:
  //   - Empty + enq → entry stored at posedge → visible to deq at state K
  //   - Full + deq.fire same cycle → enq.ready=1 (can accept new entry,
  //     replacing the dequeued one)
  std::optional<bcast_line_t> bcst_queue_;
  uint16_t bcst_accepted_ = 0;

 public:
  explicit distributor(int pu_start) : pu_start_(pu_start) {}

  int pu_start() const { return pu_start_; }

  distributor_step_out step(const distributor_step_in &in,
                            const distributor_step_fires &fires) {
    distributor_step_out out{};

    // ---- Unicast (combinational) ----
    if (in.unicast) {
      out.unicast_bits.id = in.unicast->id;
      out.unicast_bits.data = in.unicast->data;
      int rel = static_cast<int>(in.unicast->dst) - pu_start_;
      if (rel >= 0 && rel < 16)
        out.unicast_valids = static_cast<uint16_t>(1u << rel);
    }

    // ---- Broadcast pipe queue (read Reg.Q) ----
    bool deq_valid = bcst_queue_.has_value();
    bcast_line_t deq_bits = deq_valid ? *bcst_queue_ : bcast_line_t{};

    // bcstValids: OR over 4 line entries of (inRange ? UIntToOH(rel) : 0).
    // Matches RTL Distributor.bcstValids.
    uint16_t bcst_valids_mask = 0;
    if (deq_valid) {
      for (int i = 0; i < 4; ++i) {
        int rel = static_cast<int>(deq_bits.line[i].pu) - pu_start_;
        if (rel >= 0 && rel < 16)
          bcst_valids_mask |= static_cast<uint16_t>(1u << rel);
      }
    }

    // bcstWait: any PU that is valid AND not-already-accepted AND not-ready-now.
    uint16_t bcst_pending = static_cast<uint16_t>(
        ~(bcst_accepted_ | fires.broadcast_readies) & bcst_valids_mask);
    bool bcst_wait = (bcst_pending != 0);
    bool deq_fire = deq_valid && !bcst_wait;

    out.broadcast_valids = static_cast<uint16_t>(
        (deq_valid ? 0xFFFFu : 0u) & bcst_valids_mask & ~bcst_accepted_);
    out.broadcast_bits = deq_bits;

    // pipe=true semantics: enq_ready = !full || deq.fire
    out.broadcast_enq_ready = !bcst_queue_.has_value() || deq_fire;
    bool enq_fire = in.broadcast.has_value() && out.broadcast_enq_ready;

    // ---- COMMIT ----
    // bcstAccepted: cleared on deq.fire, else OR'd with newly fired bits.
    if (deq_fire) {
      bcst_accepted_ = 0;
    } else {
      bcst_accepted_ = static_cast<uint16_t>(
          bcst_accepted_ | (out.broadcast_valids & fires.broadcast_readies));
    }

    // bcst_queue_ Reg update — deq before enq so pipe-through works.
    if (deq_fire) bcst_queue_.reset();
    if (enq_fire) bcst_queue_ = in.broadcast;

    return out;
  }

  // Non-mutating peek. Unlike dram_if/mmio_if, distributor's
  // `broadcast_enq_ready` DOES depend on `fires.broadcast_readies` (because
  // `deq_fire` uses it), so peek takes the same fires struct as step.
  distributor_step_out peek(const distributor_step_in &in,
                            const distributor_step_fires &fires) const {
    distributor copy(*this);
    return copy.step(in, fires);
  }
};

