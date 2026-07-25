#pragma once

#include "system.h"
#include "soft_components.h"
#include <cstddef>
#include <limits>
#include <sys/cdefs.h>
#include <vector>
#include <cstdint>
#include <optional>
#include <bit>
#include <stdexcept>
#include <unordered_map>
#include <cassert>
#include <span>

namespace soft_mem {

// DON'T CHANGE THIS, we us uint16_t as masks
const size_t CLUSTER_SIZE = 16;

// States for a pending scalar request
struct ScalarRequest {
  uint32_t addr;
  uint16_t src;
  uint16_t id;
  // Wdata or rdata
  MemLine rwdata;
  uint8_t size;
  bool write;

  static ScalarRequest fromFlit(const Flit &f) {
    if constexpr (ASSERTIONS_ENABLED) {
      if ((f.tag & 0xFF) > 1) throw std::invalid_argument("Unknown scalar request type");
    }
    bool isWrite = (f.tag & 0xFF) != 0;

    std::array<uint32_t, MEM_BUS_WIDTH_B / 4> tmp;
    tmp.fill(f.data[2]);

    return ScalarRequest {
      .addr = f.data[0],
      .src = f.src,
      .id = (uint16_t) (f.data[1] & 0xFFFF),
      .rwdata = std::bit_cast<MemLine>(tmp),
      .size = (uint8_t) (f.data[1] >> 16),
      .write = isWrite,
    };
  }
};

struct BcastLine {
  std::array<uint32_t, MEM_BUS_WIDTH_B / 4> line;
  uint16_t tag;
  std::array<uint32_t, 2> carried;
};

struct BulkRequest {
  // TODO: supports bulk unicast, remember to update DRAMIf response pathway
  uint32_t base;
  size_t cnt;
  size_t issueCnt = 0;
  size_t retireCnt = 0;

  size_t maxInflight = 0;

  // Accessed as idx % maxInflight. Reset during allocation.
  uint64_t recentResp = 0;

  uint16_t returnTag;
  std::array<uint32_t, 2> returnCarried;

  static BulkRequest fromFlit(size_t maxInflight, const Flit &f) {
    if constexpr (ASSERTIONS_ENABLED) {
      if (maxInflight > 64) throw std::invalid_argument("maxInflight too large");
      if ((f.tag & 0xFF) != 0x10) throw std::invalid_argument("Unknown bulk request type");
    }
    return {
      .base = f.data[0],
      .cnt = (uint16_t) (f.data[1] >> 16),
      .maxInflight = maxInflight,
      .returnTag = (uint16_t) (f.data[1] & 0xFFFF),
      .returnCarried = {f.data[2], f.data[3]}
    };
  }

  std::optional<GlobalMemReq> req() const {
    if (issueCnt == cnt) return std::nullopt;
    if (issueCnt == maxInflight + retireCnt) return std::nullopt;
    uint8_t slot = issueCnt % maxInflight;
    return {{
      .id = (uint8_t) (0x80 | slot),
      .size = MEM_BUS_WIDTH_SIZE,
      .addr = (uint32_t) (base + issueCnt * MEM_BUS_WIDTH_B),
      .write = false,
    }};
  }

  std::optional<BcastLine> peekBcastLine(const std::vector<MemLine> &buffer) const {
    // Mirror RTL: bulkRespValid requires completedCnt != issueCnt
    if (retireCnt == issueCnt) return std::nullopt;
    size_t retireSlot = retireCnt % maxInflight;
    if (recentResp & (1ULL << retireSlot)) {
      return BcastLine {
        .line = std::bit_cast<std::array<uint32_t, MEM_BUS_WIDTH_B / 4>>(buffer[retireSlot]),
        .tag = returnTag,
        .carried = returnCarried
      };
    }
    return std::nullopt;
  }

  // TODO: broadcast -> distributor

  void step(bool issued, bool retire, const GlobalMemResp *resp, std::vector<MemLine> &buffer) {
    size_t oldIssueCnt = issueCnt;
    if (issued) {
      if constexpr (ASSERTIONS_ENABLED) {
        if (issueCnt >= maxInflight + retireCnt || issueCnt >= cnt)
          throw std::logic_error("issueCnt out of bounds");
      }

      size_t issueSlot = issueCnt % maxInflight;
      recentResp &= ~(1ULL << issueSlot);
      ++issueCnt;
    }

    if (retire) {
      if constexpr (ASSERTIONS_ENABLED) {
        if ((recentResp & (1ULL << (retireCnt % maxInflight))) == 0)
          throw std::logic_error("retired without response");
      }
      ++retireCnt;
      if constexpr (ASSERTIONS_ENABLED) {
        if (retireCnt > oldIssueCnt)
          throw std::logic_error("retireCnt out of bounds");
      }
    }

    if (resp != nullptr) {
      size_t respSlot = resp->id & 0x7F;
      if constexpr (ASSERTIONS_ENABLED) {
        if (respSlot >= maxInflight) throw std::logic_error("respSlot out of bounds");
      }
      buffer[respSlot] = resp->data;
      if constexpr (ASSERTIONS_ENABLED) {
        if (recentResp & (1ULL << respSlot)) throw std::logic_error("double response");
      }
      recentResp |= 1ULL << respSlot;
    }
  }
};

struct RingResp {
  uint16_t tag;
  uint16_t dst; // PU, because ring bus will only carry unicast responses for now
  MemLine data;
};

struct RingIntf {
  // Shared buffer for injection & ejection.
  RingResp *buffer;

  // Whether this buffer contains a transfer that forwarded into us at this cycle
  bool ingress;
  // Whether ejection is asserted this cycle. Implies that the traffic is destined to this MemIf
  bool eject;
  // Whether this cycle a injection is allowed
  bool canInject;
  // Whether a injection actually happened
  bool *injected;
};

// Base class for software memory interface
// contains the common components:
// - scalar states, arbitration, state machine transitions
// - ringbus with other memory interfaces
// - external memory bus
//
// Ring bus states are managed externally. Each MemIf along the ring bus
// can accept a transfer and tries to inject a transfer into the ring bus.
//
// In our current design, ring bus ejection (accept) will never block.
// THIS MIGHT CHANGE LATER. Rignt how, the acceptance is hardcoded as a true.
//
class MemIf {
protected:
  struct IdleNotifier {
    uint16_t tgt;
    uint16_t tag;
    uint16_t cycles;
  };

  struct IdleReturn {
    uint16_t tgt;
    uint16_t tag;
    uint32_t data;
  };

  size_t idx;

  size_t scalarInflight;
  std::vector<ScalarRequest> scalarPendings;
  uint64_t scalarAllocated = 0;
  uint64_t scalarSent = 0;
  uint64_t scalarCompleted = 0;
  FlitArb reqArb;
  FlitArb scalarIssueArb;
  std::optional<IdleNotifier> idleNotifier;
  std::optional<IdleReturn> idleReturn;
  uint16_t idleCycles = 0;

  static bool isScalarRequest(const Flit &f) noexcept {
    const uint8_t tag = static_cast<uint8_t>(f.tag);
    return tag == 0x00 || tag == 0x01;
  }

  static bool isBulkRequest(const Flit &f) noexcept {
    const uint8_t tag = static_cast<uint8_t>(f.tag);
    return tag == 0x10 || tag == 0x11;
  }

  static bool isIdleRequest(const Flit &f) noexcept {
    const uint8_t tag = static_cast<uint8_t>(f.tag);
    return tag == 0xF0 || tag == 0xF1;
  }

  bool idleCanAccept(const Flit &f) const noexcept {
    return isIdleRequest(f) && !idleReturn.has_value();
  }

  void idleStep(bool idleCurrently, const Flit *request,
                bool nocOutAccepted) __attribute__((always_inline)) {
    const bool notifierWasValid = idleNotifier.has_value();
    const bool requestIsIdle = request && isIdleRequest(*request);

    // These are registered RTL updates: all decisions below observe the
    // pre-edge notifier, return buffer, and accumulated idle count.
    if (notifierWasValid && !idleReturn &&
        idleCycles > idleNotifier->cycles && !requestIsIdle) {
      idleReturn = IdleReturn {
        .tgt = idleNotifier->tgt,
        .tag = idleNotifier->tag,
        .data = 0,
      };
      idleNotifier = std::nullopt;
    }

    if (requestIsIdle) {
      const uint8_t tag = static_cast<uint8_t>(request->tag);
      const uint16_t returnTag = static_cast<uint16_t>(request->data[0]);
      if (tag == 0xF0) {
        if (!notifierWasValid) {
          idleNotifier = IdleNotifier {
            .tgt = request->src,
            .tag = returnTag,
            .cycles = static_cast<uint16_t>(request->data[0] >> 16),
          };
        } else {
          idleReturn = IdleReturn {
            .tgt = request->src,
            .tag = returnTag,
            .data = UINT32_MAX,
          };
        }
      } else {
        idleNotifier = std::nullopt;
        idleReturn = IdleReturn {
          .tgt = request->src,
          .tag = returnTag,
          .data = notifierWasValid ? 0u : UINT32_MAX,
        };
      }
    }

    // nocOut is driven from the old return register, so a simultaneous
    // transfer clears it after all other next-state decisions.
    if (nocOutAccepted) idleReturn = std::nullopt;

    if (!idleCurrently) idleCycles = 0;
    else if (idleCycles != UINT16_MAX) ++idleCycles;
  }

public:
  MemIf(
    size_t idx,
    size_t scalarInflight,
    size_t numReq
  ) : idx(idx), scalarInflight(scalarInflight), scalarPendings(scalarInflight), reqArb(numReq), scalarIssueArb(scalarInflight) {
    if (scalarInflight > 64) throw std::invalid_argument("scalarInflight must be <= 64");
  }

  void resetIdle() noexcept {
    idleNotifier = std::nullopt;
    idleReturn = std::nullopt;
    idleCycles = 0;
  }

  /*
    * Scalar request state machine and generator
    */

  // Number of currently allocated scalar inflight slots.
  size_t scalarInflightCount() const noexcept __attribute__((always_inline)) {
    return std::popcount(scalarAllocated);
  }

  std::optional<uint8_t> scalarAllocSlot() const noexcept __attribute__((always_inline)) {
    // Returns the lowest 0 in scalarAllocated, bounded by the configured
    // inflight depth. Comparing against the fixed mask width (64) instead
    // would hand out slot == scalarInflight once every real slot is taken,
    // overflowing scalarPendings (e.g. the 16-slot peripheral at boot).
    int r_one = std::countr_one(scalarAllocated);
    if ((size_t) r_one >= scalarInflight) return std::nullopt;
    return r_one;
  }

  void scalarAllocCommit(uint8_t slot, ScalarRequest req) noexcept __attribute__((always_inline)) {
    scalarAllocated |= (1ULL << slot);
    scalarPendings[slot] = req;
  }

  void scalarDeallocCommit(uint8_t slot) noexcept __attribute__((always_inline)) {
    uint64_t mask = ~(1ULL << slot);
    scalarAllocated &= mask;
    scalarSent &= mask;
    scalarCompleted &= mask;
  }

  std::optional<uint8_t> scalarReqSlot() const noexcept __attribute__((always_inline)) {
    // RR over allocated-but-not-yet-sent slots (matches RTL FlitArb).
    uint64_t candidates = ~scalarSent & scalarAllocated;
    if (candidates == 0) return std::nullopt;
    return scalarIssueArb.peek([&](size_t i) -> std::optional<uint8_t> {
      return (candidates & (1ULL << i)) ? std::optional<uint8_t>(0) : std::nullopt;
    });
  }

  GlobalMemReq scalarReq(uint8_t slot) const noexcept __attribute__((always_inline)) {
    return {
      .id = slot,
      .size = scalarPendings[slot].size,
      .addr = scalarPendings[slot].addr,
      .wdata = scalarPendings[slot].rwdata,
      .wbe = std::numeric_limits<mem_mask_t>::max(), // Always full write
      .write = scalarPendings[slot].write,
    };
  }

  void scalarReqCommit(uint8_t slot) __attribute__((always_inline)) {
    if constexpr (ASSERTIONS_ENABLED) {
      if((scalarAllocated & (1ULL << slot)) == 0) throw std::logic_error("scalarReqCommit: slot not yet allocated");
      if((scalarSent & (1ULL << slot))) throw std::logic_error("scalarReqCommit: slot already committed");
    }
    scalarSent |= (1ULL << slot);
    scalarIssueArb.commit(slot);
  }

  void scalarRespAccept(const GlobalMemResp &resp) __attribute__((always_inline)) {
    if constexpr (ASSERTIONS_ENABLED) {
      if (resp.id >= scalarInflight) throw std::logic_error("scalarRespAccept: invalid id");
    }
    // TODO: implement AMO
    scalarPendings[resp.id].rwdata = resp.data;
    scalarCompleted |= (1ULL << resp.id);
  }

  std::optional<uint8_t> scalarReturnSlot() const noexcept __attribute__((always_inline)) {
    if (scalarCompleted == 0) return std::nullopt;
    // Select the lowest 1 bit in scalarCompleted
    return std::countr_zero(scalarCompleted);
  }

  RingResp scalarReturn(uint8_t slot) const __attribute__((always_inline)) {
    return RingResp {
      .tag = scalarPendings[slot].id,
      .dst = scalarPendings[slot].src,
      .data = scalarPendings[slot].rwdata,
    };
  }

  /*
   * Ring bus related functions
   */

  virtual bool ringIsLocal(const RingResp &resp) const noexcept = 0;
  virtual bool ringCanAccept(const RingResp &resp) const noexcept __attribute__((always_inline)) {
    return ringIsLocal(resp);
  }

  /*
   * External memory bus interfacing
   *
   * MemIf unconditionally accepts memory responses. So the peek function only peeks for requests
   */

  virtual std::optional<GlobalMemReq> peekMem() const = 0;

  std::optional<Flit> peekNoc() const noexcept __attribute__((always_inline)) {
    if (!idleReturn) return std::nullopt;
    return Flit {
      .src = static_cast<uint16_t>(0x8000u + idx),
      .dst = idleReturn->tgt,
      .tag = static_cast<uint16_t>(idleReturn->tag & 0x0FFFu),
      .data = {idleReturn->data, 0, 0, 0},
    };
  }

  /*
   * NoC ejection interface
   */
protected:
  virtual bool nocCanAccept(const Flit &f) const = 0;
public:
  template<typename FI>
  std::optional<uint8_t> nocAcceptMultiple(FI flits) const
  requires(std::is_invocable_r_v<const Flit*, FI, size_t>)
  __attribute__((always_inline)) {
    // Iterate through all request ports
    auto selected = reqArb.peek([&flits](size_t idx) __attribute__((always_inline)) -> std::optional<uint8_t> {
      const Flit *flit = flits(idx);
      if (!flit) return std::nullopt;
      return flit->prio();
    });
    if (selected && !nocCanAccept(*flits(*selected))) return std::nullopt;
    return selected;
  }
  void nocAcceptCommit(uint8_t accepted) {
    reqArb.commit(accepted);
  }
};

// Ring bus implementation
// The ring bus interacts with MemIf in the following way:
// Each MemIf has a buffer containing a memory response
// that's the injection / forward buffer at that MemIf.
// So the way we do this
class Ringbus {
  // Number of nodes in this ring bus
  size_t size;
  // Indexing offset
  size_t offset = 0;
  std::vector<RingResp> buffers;
  uint64_t validness = 0;

public:
  Ringbus(size_t size) : size(size), buffers(size) {
    if (size > 64) throw std::logic_error("Ringbus: size must be <= 64");
  }

  bool validAt(size_t index) const __attribute__((always_inline)) {
    index = (index + offset) % size;
    return (validness >> index) & 1;
  }
  void updateValidAt(size_t index, bool inject, bool eject) __attribute__((always_inline)) {
    // If nothing happens, do nothing
    if (!inject && !eject) return;

    size_t offsetted = (index + offset) % size;
    // If injected, always set one
    if (inject) validness |= 1ULL << offsetted;
    // If not injected, but ejected, clear one
    else if (eject) validness &= ~(1ULL << offsetted);
  }
  RingResp* peekAt(size_t index) __attribute__((always_inline)) {
    if (validAt(index)) return &operator[](index);
    else return nullptr;
  }

  RingResp& operator[](size_t index) __attribute__((always_inline)) {
    return buffers.at((index + offset) % size);
  }
  const RingResp& operator[](size_t index) const __attribute__((always_inline)) {
    return buffers.at((index + offset) % size);
  }

  void progress() __attribute__((always_inline)) {
    offset = (offset + size - 1) % size;
  }
};

/**
 * The DRAMIf. Including the distributors, so it directly interfaces with PUs
 */
class DRAMIf : public MemIf {
  // Influenced PU range: [puStart, puEnd).
  // puEnd - puStart has to be a multiple of 16
  size_t puStart;
  size_t puEnd;

  /*
   * Bulk state machine
   */
  std::optional<BulkRequest> bulk = {};
  std::vector<MemLine> bulkBuffer;
  size_t bulkMaxInflight;
  uint64_t bcstDistAccepted = 0;

  /*
   * Per-distributor states
   */
  // Bulk queues in distributors
  std::vector<FixedLenQueue<BcastLine>> bcstQueues;
  size_t bcstQueueCap;
  // Bulk acceptance states
  std::vector<uint16_t> bcstPUAccepted;
  // Registered unicast output of each distributor.
  std::vector<std::optional<RingResp>> unicastPipes;

  struct UnicastRespPeek {
    // The local responses, if exists. Index is pu index - puStart
    std::optional<size_t> ringLocal = std::nullopt;
    std::optional<size_t> scalarLocal = std::nullopt;
    bool scalarRemote = false;
  };

  UnicastRespPeek unicastRespPeek(const RingResp *ingress) const __attribute__((always_inline)) {
    UnicastRespPeek peek;
    if (ingress) if (ingress->dst >= puStart && ingress->dst < puEnd) peek.ringLocal = ingress->dst - puStart;
    if (auto slot = scalarReturnSlot()) {
      auto resp = scalarReturn(*slot);
      if (resp.dst >= puStart && resp.dst < puEnd) peek.scalarLocal = resp.dst - puStart;
      else peek.scalarRemote = true;
    }

    // Arbiter cascade
    if (peek.ringLocal && peek.scalarLocal) {
      size_t ringDist = *peek.ringLocal / CLUSTER_SIZE;
      size_t scalarDist = *peek.scalarLocal / CLUSTER_SIZE;
      if (ringDist == scalarDist) peek.scalarLocal = std::nullopt;
    }

    return peek;
  }

public:
  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) PUResp {
    std::optional<std::pair<uint16_t, MemLine>> unicast;
    std::optional<BcastLine> bcast;
  };

  struct alignas(DESTRUCTIVE_INTERFERENCE_SIZE) PUAccept {
    bool unicast;
    bool broadcast;
  };

  static_assert(alignof(PUResp) >= DESTRUCTIVE_INTERFERENCE_SIZE);
  static_assert(alignof(PUAccept) >= DESTRUCTIVE_INTERFERENCE_SIZE);

  size_t numClusters() const {
    return (puEnd - puStart) / CLUSTER_SIZE;
  }

  // True iff a bulk request is currently in-flight (occupying the
  // single bulk slot).
  bool bulkActive() const noexcept { return bulk.has_value(); }
  // Sum of pending broadcast lines across cluster distributor queues.
  size_t bcstQueueOccupancy() const noexcept {
    size_t n = 0;
    for (const auto &q : bcstQueues) n += q.size();
    return n;
  }

protected:
  virtual bool nocCanAccept(const Flit &f) const override __attribute__((always_inline)) {
    if (isIdleRequest(f)) return idleCanAccept(f);
    if (isBulkRequest(f)) return !bulk.has_value();
    if (isScalarRequest(f)) return scalarAllocSlot().has_value();
    return false;
  }

public:
  DRAMIf(size_t memif_idx, size_t numReq, size_t scalarInflight, size_t bulkInflight, size_t bcstQueueCap, size_t puStart, size_t puEnd)
    : MemIf(memif_idx, scalarInflight, numReq),
      puStart(puStart),
      puEnd(puEnd),
      bulkMaxInflight(bulkInflight),
      bulkBuffer(bulkInflight),
      bcstQueueCap(bcstQueueCap),
      bcstPUAccepted(numClusters()),
      unicastPipes(numClusters()) {
        if ((puEnd - puStart) % CLUSTER_SIZE) throw std::logic_error("PU range not aligned");
        for (size_t dist = 0; dist < numClusters(); ++dist) {
          bcstQueues.emplace_back(bcstQueueCap);
        }
      }

  /*
   * PU interfacing
   */
  // Compute the per-cluster validsMask for a broadcast line: bit set
  // if some beat of `bl` is addressed to a PU within
  // [clusterStart, clusterStart + CLUSTER_SIZE). Mirrors RTL Distributor's
  // `bcstValids` computed via `e.pu -% puStart < 16.U`.
  static uint16_t bcastValidsMask(const BcastLine &bl, uint16_t clusterStart) {
    uint16_t mask = 0;
    for (size_t i = 0; i < MEM_BUS_WIDTH_B / sizeof(uint64_t); ++i) {
      uint16_t pu = (uint16_t)(bl.line[2*i] & 0xFFFF);
      uint16_t delta = (uint16_t)(pu - clusterStart);
      if (delta < CLUSTER_SIZE) mask |= (uint16_t)(1u << delta);
    }
    return mask;
  }

  void peekPUs(size_t rangeStart, const std::span<PUResp> &pus) const
      __attribute__((always_inline)) {
    if constexpr (ASSERTIONS_ENABLED) {
      if (rangeStart < puStart || rangeStart + pus.size() > puEnd)
        throw std::logic_error("peekPUs range is outside the MemIf domain");
    }

    // Populate PU responses
    for (size_t rangeDelta = 0; rangeDelta < pus.size(); ++rangeDelta) {
      const size_t puDelta = rangeStart + rangeDelta - puStart;
      size_t dist = puDelta / CLUSTER_SIZE;
      uint8_t puSubidx = puDelta % CLUSTER_SIZE;
      const auto &unicast = unicastPipes[dist];
      if (unicast && unicast->dst == puStart + puDelta)
        pus[rangeDelta].unicast = {unicast->tag, unicast->data};
      else
        pus[rangeDelta].unicast = std::nullopt;

      auto presented = bcstQueues[dist].front();
      pus[rangeDelta].bcast = std::nullopt;
      if (presented && ((bcstPUAccepted[dist] >> puSubidx) & 1) == 0) {
        uint16_t clusterStart = (uint16_t)(puStart + dist * CLUSTER_SIZE);
        uint16_t valids = bcastValidsMask(presented.value().get(), clusterStart);
        if (valids & (1u << puSubidx))
          pus[rangeDelta].bcast = presented.value().get();
      }
    }
  }

  virtual bool ringIsLocal(const RingResp &resp) const noexcept override __attribute__((always_inline)) {
    return resp.dst >= puStart && resp.dst < puEnd;
  };

  virtual std::optional<GlobalMemReq> peekMem() const override __attribute__((always_inline)) {
    // Static priority is given to scalar requests
    if (auto slot = MemIf::scalarReqSlot()) return MemIf::scalarReq(*slot);
    else if (bulk) return bulk->req();
    else return std::nullopt;
  }

  /*
   * Step function
   */
  void step(
    RingIntf ring,
    MemBusIn &mem,
    std::optional<std::pair<std::reference_wrapper<const Flit>, uint8_t>> flit,
    const std::span<const uint16_t> &bcstAcceptMasks,
    bool nocOutAccepted
  ) __attribute__((always_inline)) {
    *ring.injected = false;

    if constexpr (ASSERTIONS_ENABLED) {
      if (bcstAcceptMasks.size() != numClusters())
        throw std::logic_error("Incorrect broadcast acceptance mask count");
    }

    const Flit *acceptedFlit = flit ? &flit->first.get() : nullptr;
    idleStep(scalarAllocated == 0 && !bulk.has_value(), acceptedFlit,
             nocOutAccepted);

    // Remember allocation slots
    auto scalarAlloc = scalarAllocSlot();

    // Bulk completion happens before any potential de-queue in distributor, so we handle that first
    bool bulkRetireOne = false;
    bool bulkAcceptOne = false;
    if (bulk) if (auto line = bulk->peekBcastLine(bulkBuffer)) {
      // Iterate through all distributors
      for (size_t dist = 0; dist < (puEnd - puStart) / CLUSTER_SIZE; ++dist)
        if (!(bcstDistAccepted & (1ULL << dist)) && bcstQueues[dist].size() < bcstQueueCap) {
          bcstQueues[dist].push(*line);
          bcstDistAccepted |= 1ULL << dist;
        }

      // FIXME: may overflow here! assert that dist count < 64
      if (bcstDistAccepted == ((1ULL << ((puEnd - puStart) / CLUSTER_SIZE)) - 1)) {
        // Step to next bulk line
        bcstDistAccepted = 0;
        bulkRetireOne = true;
      }
    }

    // Unicast completion & ringbus generation
    auto unicast = unicastRespPeek(ring.ingress ? ring.buffer : nullptr);
    auto scalarRet = MemIf::scalarReturnSlot();
    bool scalarDealloc = false;
    for (auto &pipe : unicastPipes) pipe = std::nullopt;

    if (unicast.ringLocal) {
      if constexpr (ASSERTIONS_ENABLED) {
        if (!ring.eject) throw std::logic_error("local ring response was not ejected");
      }
      size_t dist = *unicast.ringLocal / CLUSTER_SIZE;
      unicastPipes[dist] = *ring.buffer;
    }

    if (unicast.scalarLocal) {
      auto resp = MemIf::scalarReturn(*scalarRet);
      size_t dist = *unicast.scalarLocal / CLUSTER_SIZE;
      unicastPipes[dist] = resp;
      scalarDealloc = true;
    }

    // Now that we're donw with incoming ring ejection, we can overwrite the buffer
    // For ringbus generation, we check if we're allowed a injection
    if (ring.canInject && unicast.scalarRemote) {
      *ring.injected = true;
      *ring.buffer = MemIf::scalarReturn(*scalarRet);
      scalarDealloc = true;
    }

    if (scalarDealloc) {
      MemIf::scalarDeallocCommit(*scalarRet);
    }

    // Distributor bcst acceptance
    for (size_t dist = 0; dist < (puEnd - puStart) / CLUSTER_SIZE; ++dist) {
      // Per dist handling
      uint16_t &bcstPUMask = bcstPUAccepted[dist];
      const uint16_t accepted = bcstAcceptMasks[dist];
      if constexpr (ASSERTIONS_ENABLED) {
        if (bcstPUMask & accepted)
          throw std::logic_error("PU accepted a non-existing broadcast");
      }
      bcstPUMask |= accepted;

      auto &queue = bcstQueues[dist];
      auto head = queue.front();
      if (head) {
        uint16_t clusterStart = (uint16_t)(puStart + dist * CLUSTER_SIZE);
        uint16_t valids = bcastValidsMask(head.value().get(), clusterStart);
        // Pop when every addressed PU has accepted (matches RTL bcstWait==0)
        if ((bcstPUMask & valids) == valids) {
          bcstPUMask = 0;
          bool popped = queue.pop();
          if constexpr (ASSERTIONS_ENABLED) {
            if (!popped) throw std::logic_error("Broadcast queue is empty when dequeue");
          }
        }
      }
    }

    // External memory handling
    // Crucially, this is after handling unicast & broadcast response generation, so
    // that still used the state from the previous cycle
    //
    // If scalar has memory request, it's given priority
    if (mem.reqAccepting && peekMem().has_value()) {
      if (auto slot = MemIf::scalarReqSlot()) {
        scalarReqCommit(*slot);
      }
      else if (bulk)
        bulkAcceptOne = true;
      else {
        if constexpr (ASSERTIONS_ENABLED) {
          throw std::logic_error("External memory accepted a ghost request");
        }
      }
    }

    GlobalMemResp *bulkMemResp = nullptr;
    if (mem.resp) {
      if (mem.resp->id & 0x80) bulkMemResp = &*mem.resp;
      else {
        MemIf::scalarRespAccept(*mem.resp);
      }
    }

    // Finally, steps bulk with generated control signals
    if (bulk) {
      bulk->step(bulkAcceptOne, bulkRetireOne, bulkMemResp, bulkBuffer);
      if (bulk->retireCnt == bulk->cnt) bulk = std::nullopt;
    } else if (bulkAcceptOne || bulkRetireOne || bulkMemResp != nullptr)
      if constexpr (ASSERTIONS_ENABLED) {
        throw std::logic_error("Bulk state mismatch");
      }

    // Finally, allocations
    if (flit) {
      const Flit &accepted = flit->first.get();
      if (isBulkRequest(accepted)) {
        if constexpr (ASSERTIONS_ENABLED) {
          if (bulk.has_value()) throw std::logic_error("Received a bulk request while another bulk is in-flight");
        }
        bulk = BulkRequest::fromFlit(bulkMaxInflight, flit->first);
      } else if (isScalarRequest(accepted)) {
        if constexpr (ASSERTIONS_ENABLED) {
          if (!scalarAlloc) throw std::logic_error("Received a scalar request while no slot is available");
        }
        scalarAllocCommit(*scalarAlloc, ScalarRequest::fromFlit(flit->first));
      } else if constexpr (ASSERTIONS_ENABLED) {
        if (!isIdleRequest(accepted))
          throw std::logic_error("Received unknown DRAMIf request tag");
      }
      nocAcceptCommit(flit->second);
    }
  }
};

/*
 * Peripheral interface
 *
 * The only difference between it and generic MemIf is that it has a constant ROM bypass
 */
class PeriphIf : public MemIf {
  // TODO: check for addr alignment in ctor
  std::unordered_map<uint32_t, MemLine> configROM;

protected:
  virtual bool nocCanAccept(const Flit &f) const override __attribute__((always_inline)) {
    if (isIdleRequest(f)) return idleCanAccept(f);
    if (isScalarRequest(f)) return scalarAllocSlot().has_value();
    return false;
  }

public:
  PeriphIf(size_t memif_idx, size_t scalarInflight, std::unordered_map<uint32_t, MemLine> configROM) : MemIf(memif_idx, scalarInflight, 1), configROM(std::move(configROM)) {
    // Check configROM
    for (const auto &[addr, line] : configROM)
      if constexpr (ASSERTIONS_ENABLED) {
        if (addr & MEM_ADDR_OFFSET_MASK) throw std::logic_error("ConfigROM address not aligned");
      }
  }

  virtual bool ringIsLocal(const RingResp &resp) const noexcept override __attribute__((always_inline)) {
    return false;
  }

  virtual std::optional<GlobalMemReq> peekMem() const override __attribute__((always_inline)) {
    auto slot = MemIf::scalarReqSlot();
    if (!slot) return std::nullopt;
    auto req = MemIf::scalarReq(*slot);
    if (configROM.contains(req.addr & MEM_ADDR_ALIGN_MASK)) return std::nullopt;
    return req;
  };

  void step(
    RingIntf ring,
    MemBusIn &mem,
    std::optional<std::pair<std::reference_wrapper<const Flit>, uint8_t>> flit,
    bool nocOutAccepted
  ) __attribute__((always_inline)) {
    *ring.injected = false;

    const Flit *acceptedFlit = flit ? &flit->first.get() : nullptr;
    idleStep(scalarAllocated == 0, acceptedFlit, nocOutAccepted);

    // Remember alloc slots
    auto scalarAlloc = scalarAllocSlot();

    // Ring state machine
    // We have no local ejection, so always feed into the ring
    if constexpr (ASSERTIONS_ENABLED) {
      if (ring.eject) throw std::logic_error("ring ejection at PeriphIf should never happen");
    }
    if (ring.canInject) {
      auto slot = MemIf::scalarReturnSlot();
      if (slot) {
        *ring.buffer = MemIf::scalarReturn(*slot);
        *ring.injected = true;
        MemIf::scalarDeallocCommit(*slot);
      }
    }

    // External memory response
    // This is handled before req, because for peripheral, we need to look at the pending request
    // Grant is given to potentially config ROM is external mem does not give a response
    bool romServed = false;
    if (mem.resp) // Contains resp
      scalarRespAccept(*mem.resp);
    else if (auto slot = MemIf::scalarReqSlot()) {
      auto req = MemIf::scalarReq(*slot);
      auto alignedAddr = req.addr & MEM_ADDR_ALIGN_MASK;
      if (configROM.contains(alignedAddr)) {
        romServed = true;
        scalarRespAccept({
          .id = req.id,
          .data = configROM[alignedAddr]
        });
      }
    }

    // External memory request
    if (romServed || (mem.reqAccepting && peekMem().has_value())) {
      if (auto slot = MemIf::scalarReqSlot())
        scalarReqCommit(*slot);
      else {
        if constexpr (ASSERTIONS_ENABLED) {
          throw std::logic_error("External memory accepted a ghost request");
        }
      }
    }

    if (flit) {
      const Flit &accepted = flit->first.get();
      if (isScalarRequest(accepted)) {
        if constexpr (ASSERTIONS_ENABLED) {
          if (!scalarAlloc) throw std::logic_error("Received a scalar request while no slot is available");
        }
        MemIf::scalarAllocCommit(*scalarAlloc, ScalarRequest::fromFlit(flit->first));
      } else if constexpr (ASSERTIONS_ENABLED) {
        if (!isIdleRequest(accepted))
          throw std::logic_error("Received unknown PeripheralIf request tag");
      }
      nocAcceptCommit(flit->second);
    }
  }
};

}
