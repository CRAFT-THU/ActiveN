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
#include <deque>
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
    if ((f.tag & 0xFF) > 1) throw std::invalid_argument("Unknown scalar request type");
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
  std::vector<MemLine> buffer;

  uint16_t returnTag;
  std::array<uint32_t, 2> returnCarried;

  static BulkRequest fromFlit(size_t maxInflight, const Flit &f) {
    if (maxInflight > 64) throw std::invalid_argument("maxInflight too large");
    if ((f.tag & 0xFF) != 0x10) throw std::invalid_argument("Unknown bulk request type");
    return {
      .base = f.data[0],
      .cnt = (uint16_t) (f.data[1] >> 16),
      .maxInflight = maxInflight,
      .buffer = std::vector<MemLine>(maxInflight),
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

  std::optional<BcastLine> peekBcastLine() const {
    // First, check if resp is already here
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

  void step(bool issued, bool retire, const GlobalMemResp *resp) {
    size_t oldIssueCnt = issueCnt;
    if (issued) {
      if (issueCnt >= maxInflight + retireCnt || issueCnt >= cnt)
        throw std::logic_error("issueCnt out of bounds");

      size_t issueSlot = issueCnt % maxInflight;
      recentResp &= ~(1ULL << issueSlot);
      ++issueCnt;
    }

    if (retire) {
      if ((recentResp & (1ULL << (retireCnt % maxInflight))) == 0)
        throw std::logic_error("retired without response");
      ++retireCnt;
      if (retireCnt > oldIssueCnt)
        throw std::logic_error("retireCnt out of bounds");
    }

    if (resp != nullptr) {
      size_t respSlot = resp->id & 0x7F;
      if (respSlot >= maxInflight) throw std::logic_error("respSlot out of bounds");
      buffer[respSlot] = resp->data;
      if (recentResp & (1ULL << respSlot)) throw std::logic_error("double response");
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
  size_t idx;

  size_t scalarInflight;
  std::vector<ScalarRequest> scalarPendings;
  uint64_t scalarAllocated = 0;
  uint64_t scalarSent = 0;
  uint64_t scalarCompleted = 0;
public:
  MemIf(
    size_t idx,
    size_t scalarInflight
  ) : idx(idx), scalarInflight(scalarInflight), scalarPendings(scalarInflight) {
    if (scalarInflight > 64) throw std::invalid_argument("scalarInflight must be <= 64");
  }

  /*
    * Scalar request state machine and generator
    */

  std::optional<uint8_t> scalarAllocSlot() const noexcept __attribute__((always_inline)) {
    // Returns the lowest 0 in scalarAllocated
    int r_one = std::countr_one(scalarAllocated);
    if (r_one == 64) return std::nullopt;
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
    // Find lowest 1 in (~scalarSent & scalarAllocated)
    uint64_t candidates = ~scalarSent & scalarAllocated;
    if (candidates == 0) return std::nullopt;
    int r_one = std::countr_zero(candidates);
    return r_one;
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
    if((scalarAllocated & (1ULL << slot)) == 0) throw std::logic_error("scalarReqCommit: slot not yet allocated");
    if((scalarSent & (1ULL << slot))) throw std::logic_error("scalarReqCommit: slot already committed");
    scalarSent |= (1ULL << slot);
  }

  void scalarRespAccept(const GlobalMemResp &resp) __attribute__((always_inline)) {
    if (resp.id >= scalarInflight) throw std::logic_error("scalarRespAccept: invalid id");
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
    return true;
  }

  /*
   * External memory bus interfacing
   *
   * MemIf unconditionally accepts memory responses. So the peek function only peeks for requests
   */

  virtual std::optional<GlobalMemReq> peekMem() const = 0;

  /*
   * NoC ejection interface
   */
  virtual bool nocCanAccept(const Flit &f) const = 0;
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
  size_t offset;
  std::vector<RingResp> buffers;
  uint64_t validness;

public:
  Ringbus(size_t size, size_t offset) : size(size), offset(offset), buffers(size), validness(0) {
    if (size > 64) throw std::logic_error("Ringbus: size must be <= 64");
  }

  bool validAt(size_t index) const __attribute__((always_inline)) {
    index = (index + offset) % size;
    return (validness >> index) & 1;
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
    offset = (offset + 1) % size;
  }
};

/**
 * The DRAMIf. Including the distributors, so it directly interfaces with PUs
 */
class DRAMIf : public MemIf {
  /*
   * Bulk state machine
   */
  std::optional<BulkRequest> bulk = {};
  size_t bulkMaxInflight;
  uint64_t bcstDistAccepted = 0;

  /*
   * Per-distributor states
   */
  // Bulk queues in distributors
  std::vector<std::deque<BcastLine>> bcstQueues;
  size_t bcstQueueCap;
  // Bulk acceptance states
  std::vector<uint16_t> bcstPUAccepted;

  // Influenced PU range: [puStart, puEnd).
  // puEnd - puStart has to be a multiple of 16
  size_t puStart;
  size_t puEnd;

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
  struct PUResp {
    std::optional<std::pair<uint16_t, MemLine>> unicast;
    std::optional<BcastLine> bcast;
  };

  struct PUAccept {
    bool unicast; // Right now, PU unconditionally accepts unicast. The ringbus impl depends on this. TODO: add a assertion
    bool broadcast;
  };

  /*
   * PU interfacing
   */
  void peekPUs(std::span<PUResp> &pus, const RingResp *ingress) const __attribute__((always_inline)) {
    if (pus.size() != puEnd - puStart) throw std::logic_error("Incorrect peekPUs buffer length");

    // Generate the UNIQUE arbitration of unicast response
    auto unicast = unicastRespPeek(ingress);
    RingResp scalarResp;
    if (unicast.scalarLocal) scalarResp = scalarReturn(*scalarReturnSlot());

    // Populate PU responses
    for (size_t puDelta = 0; puDelta < puEnd - puStart; ++puDelta) {
      if (std::make_optional(puDelta) == unicast.ringLocal) pus[puDelta].unicast = { ingress->tag, ingress->data };
      else if (std::make_optional(puDelta) == unicast.scalarLocal) {
        pus[puDelta].unicast = { scalarResp.tag, scalarResp.data };
      } else pus[puDelta].unicast = std::nullopt;

      size_t dist = puDelta / CLUSTER_SIZE;
      uint8_t puSubidx = puDelta % CLUSTER_SIZE;
      if (bcstQueues[dist].size() > 0 && ((bcstPUAccepted[dist] >> puSubidx) & 1) == 0)
        pus[puDelta].bcast = bcstQueues[dist].front();
      else pus[puDelta].bcast = std::nullopt;
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

  virtual bool nocCanAccept(const Flit &f) const override __attribute__((always_inline)) {
    bool isBulk = (f.tag & 0x10) != 0;
    if (isBulk) return !bulk.has_value();
    else return scalarAllocSlot().has_value();
  }

  /*
   * Step function
   */
  void step(
    RingIntf ring,
    MemBusIn &mem,
    Flit *flit,
    const std::span<PUAccept> &puAccepts
  ) __attribute__((always_inline)) {
    // Remember allocation slots
    auto scalarAlloc = scalarAllocSlot();

    // Bulk completion happens before any potential de-queue in distributor, so we handle that first
    bool bulkRetireOne = false;
    bool bulkAcceptOne = false;
    if (bulk) if (auto line = bulk->peekBcastLine()) {
      // Iterate through all distributors
      for (size_t dist = 0; dist < (puEnd - puStart) / CLUSTER_SIZE; ++dist)
        if (!(bcstDistAccepted & (1ULL << dist)) && bcstQueues[dist].size() < bcstQueueCap) {
          bcstQueues[dist].push_back(*line);
          bcstDistAccepted |= 1ULL << dist;
        }

      // FIXME: may overflow here! assert that dist count < 64
      if (bcstDistAccepted == (1ULL << (puEnd - puStart) / CLUSTER_SIZE) - 1) {
        // Step to next bulk line
        bcstDistAccepted = 0;
        bulkRetireOne = true;
      }
    }

    // Unicast completion & ringbus generation
    auto unicast = unicastRespPeek(ring.ingress ? ring.buffer : nullptr);
    auto scalarRet = MemIf::scalarReturnSlot();
    bool scalarDealloc = false;
    // For unicast responses that's from ringbus forward, we don't need to handle it here.
    // it's automatically overwritten by the ringbus injection logic
    // Check if any PU accepted a unicast
    for (size_t puDelta = 0; puDelta < puEnd - puStart; ++ puDelta)
      if (puAccepts[puDelta].unicast) {
        if (
          std::make_optional(puDelta) != unicast.ringLocal
          && std::make_optional(puDelta) != unicast.scalarLocal
        ) throw std::logic_error("PU accepted a non-existing unicast");
      }

    if (unicast.ringLocal && puAccepts[*unicast.ringLocal].unicast) {
      if (!ring.eject) throw std::logic_error("PU accepted a ringbus forward, but ringbus does not eject");
    }

    if (unicast.scalarLocal && puAccepts[*unicast.scalarLocal].unicast) {
      auto resp = MemIf::scalarReturn(*scalarRet);
      if (*unicast.scalarLocal + puStart != resp.dst) throw std::logic_error("locally returning a unicast with wrong dest");
      scalarDealloc = true;
    }

    // Now that we're donw with incoming ring ejection, we can overwrite the buffer
    // For ringbus generation, we check if we're allowed a injection
    if (ring.canInject && unicast.scalarRemote) {
      *ring.injected = true;
      *ring.buffer = MemIf::scalarReturn(*scalarRet);
      scalarDealloc = true;
    }

    if (scalarDealloc) MemIf::scalarDeallocCommit(*scalarRet);

    // Distributor bcst acceptance
    for (size_t dist = 0; dist < (puEnd - puStart) / CLUSTER_SIZE; ++dist) {
      // Per dist handling
      uint16_t &bcstPUMask = bcstPUAccepted[dist];
      for (size_t puDelta = 0; puDelta < CLUSTER_SIZE; ++puDelta) {
        if (puAccepts[dist * CLUSTER_SIZE + puDelta].broadcast) {
          if (bcstPUMask & (1 << puDelta)) throw std::logic_error("PU accepted a non-existing broadcast");
          bcstPUMask |= 1 << puDelta;
        }
      }

      if (bcstPUMask == ((uint16_t) ~0ULL)) {
        // All accepted
        bcstPUMask = 0;
        auto &queue = bcstQueues[dist];
        if (queue.empty()) throw std::logic_error("Broadcast queue is empty when dequeue");
        queue.pop_front();
      }
    }

    // External memory handling
    // Crucially, this is after handling unicast & broadcast response generation, so
    // that still used the state from the previous cycle
    //
    // If scalar has memory request, it's given priority
    if (mem.reqAccepting) {
      if (auto slot = MemIf::scalarReqSlot())
        scalarReqCommit(*slot);
      else if (bulk)
        bulkAcceptOne = true;
      else
        throw std::logic_error("External memory accepted a ghost request");
    }

    GlobalMemResp *bulkMemResp = nullptr;
    if (mem.resp) {
      if (mem.resp->id & 0x80) bulkMemResp = &*mem.resp;
      else MemIf::scalarRespAccept(*mem.resp);
    }

    // Finally, steps bulk with generated control signals
    if (bulk) {
      bulk->step(bulkAcceptOne, bulkRetireOne, bulkMemResp);
      if (bulk->retireCnt == bulk->cnt) bulk = std::nullopt;
    } else if (bulkAcceptOne || bulkRetireOne || bulkMemResp != nullptr)
      throw std::logic_error("Bulk state mismatch");

    // Finally, allocations
    if (flit) {
      bool isBulk = (flit->tag & 0x10) != 0;
      if (isBulk) {
        if (bulk.has_value()) throw std::logic_error("Received a bulk request while another bulk is in-flight");
        bulk = BulkRequest::fromFlit(bulkMaxInflight, *flit);
      } else {
        if (!scalarAlloc) throw std::logic_error("Received a scalar request while no slot is available");
        scalarAllocCommit(*scalarAlloc, ScalarRequest::fromFlit(*flit));
      }
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

public:
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

  virtual bool nocCanAccept(const Flit &f) const override __attribute__((always_inline)) {
    return scalarAllocSlot().has_value();
  }

  void step(
    RingIntf ring,
    MemBusIn &mem,
    Flit *flit
  ) __attribute__((always_inline)) {
    // Remember alloc slots
    auto scalarAlloc = scalarAllocSlot();

    // Ring state machine
    // We have no local ejection, so always feed into the ring
    if (ring.eject) throw std::logic_error("ring ejection at PeriphIf should never happen");
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
    if (mem.reqAccepting || romServed) {
      if (auto slot = MemIf::scalarReqSlot())
        scalarReqCommit(*slot);
      else
        throw std::logic_error("External memory accepted a ghost request");
    }

    if (flit) {
      if (!scalarAlloc) throw std::logic_error("Received a scalar request while no slot is available");
      MemIf::scalarAllocCommit(*scalarAlloc, ScalarRequest::fromFlit(*flit));
    }
  }
};

}
