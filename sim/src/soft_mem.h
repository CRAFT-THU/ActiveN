#pragma once

#include "system.h"
#include "soft_components.h"
#include "morrison_phase_probe.h"
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
inline constexpr size_t DRAM_MSHR_COUNT = 256;
using MshrSlot = uint16_t;
struct ExperimentConfig {
  size_t slots = DRAM_MSHR_COUNT;
  bool merge = false;
};
inline ExperimentConfig experimentConfig() {
  ExperimentConfig c;
  if (const char *v = std::getenv("SOFT_MEMIF_MSHRS")) {
    const std::string text(v);
    if (text != "256" && text != "384" && text != "512")
      throw std::invalid_argument("SOFT_MEMIF_MSHRS must be 256, 384, or 512");
    c.slots = std::stoul(text);
  }
  if (const char *v = std::getenv("SOFT_MEMIF_WRITE_MERGE")) {
    const std::string text(v);
    if (text != "0" && text != "1") throw std::invalid_argument("SOFT_MEMIF_WRITE_MERGE must be 0 or 1");
    c.merge = text == "1";
  }
  return c;
}
inline constexpr size_t DRAM_UNICAST_RESERVE = 32;
inline constexpr size_t DRAM_BROADCAST_BOUND =
    DRAM_MSHR_COUNT - DRAM_UNICAST_RESERVE;
inline constexpr size_t DRAM_BULK_QUEUE_DEPTH = 32;

class DynamicBitMask {
  size_t bitCount;
  std::vector<uint64_t> words;

 public:
  explicit DynamicBitMask(size_t bits)
      : bitCount(bits), words((bits + 63) / 64, 0) {}

  bool test(size_t bit) const noexcept {
    return (words[bit / 64] & (uint64_t{1} << (bit % 64))) != 0;
  }

  void set(size_t bit) noexcept {
    words[bit / 64] |= uint64_t{1} << (bit % 64);
  }

  void reset(size_t bit) noexcept {
    words[bit / 64] &= ~(uint64_t{1} << (bit % 64));
  }

  bool none() const noexcept {
    for (uint64_t word : words)
      if (word != 0) return false;
    return true;
  }

  size_t count() const noexcept {
    size_t result = 0;
    for (uint64_t word : words) result += std::popcount(word);
    return result;
  }

  size_t countAnd(const DynamicBitMask &other) const noexcept {
    size_t result = 0;
    for (size_t i = 0; i < words.size(); ++i)
      result += std::popcount(words[i] & other.words[i]);
    return result;
  }

  size_t countAndNot(const DynamicBitMask &other) const noexcept {
    size_t result = 0;
    for (size_t i = 0; i < words.size(); ++i)
      result += std::popcount(words[i] & ~other.words[i]);
    return result;
  }

  size_t countAndAndNot(const DynamicBitMask &included,
                        const DynamicBitMask &excluded) const noexcept {
    size_t result = 0;
    for (size_t i = 0; i < words.size(); ++i)
      result += std::popcount(words[i] & included.words[i] &
                              ~excluded.words[i]);
    return result;
  }

  std::optional<MshrSlot> firstZero() const noexcept {
    for (size_t i = 0; i < words.size(); ++i) {
      uint64_t candidates = ~words[i];
      if (i + 1 == words.size() && bitCount % 64 != 0)
        candidates &= (uint64_t{1} << (bitCount % 64)) - 1;
      if (candidates != 0)
        return static_cast<MshrSlot>(i * 64 + std::countr_zero(candidates));
    }
    return std::nullopt;
  }
};

struct MSHR {
  uint32_t addr;
  uint16_t src;
  uint16_t id;
  MemLine rwdata;
  std::array<uint32_t, 2> carried;
  uint8_t size;
  bool write;
  mem_mask_t wbe = std::numeric_limits<mem_mask_t>::max();
  uint64_t flowToken = 0; // host-only observation, never part of a response payload

  bool isBroadcast() const noexcept { return src == 0; }
  bool isNoReceipt() const noexcept { return src == 0xFFFF; }

  static MSHR fromScalarFlit(const Flit &f) {
    const uint8_t tag = static_cast<uint8_t>(f.tag);
    if constexpr (ASSERTIONS_ENABLED) {
      if (tag != 0x00 && tag != 0x01 && tag != 0x02)
        throw std::invalid_argument("Unknown scalar request type");
    }
    bool isWrite = tag == 0x01 || tag == 0x02;

    std::array<uint32_t, MEM_BUS_WIDTH_B / 4> tmp;
    tmp.fill(f.data[3]);

    return MSHR {
      .addr = f.data[0],
      .src = tag == 0x02 ? static_cast<uint16_t>(0xFFFF) : f.src,
      .id = (uint16_t) (f.data[1] & 0xFFFF),
      .rwdata = std::bit_cast<MemLine>(tmp),
      .carried = {f.data[2], f.data[3]},
      .size = (uint8_t) (f.data[1] >> 16),
      .write = isWrite,
    };
  }
};

struct BcastLine {
  std::array<uint32_t, MEM_BUS_WIDTH_B / 4> line;
  uint16_t tag;
  std::array<uint32_t, 2> carried;
  uint64_t flowToken = 0;
};

struct BulkDispatcher {
  uint32_t base;
  uint16_t count;
  uint16_t src;
  uint16_t returnTag;
  std::array<uint32_t, 2> returnCarried;
  bool broadcast;
  uint64_t flowToken = 0;

  static BulkDispatcher fromFlit(const Flit &f) {
    if constexpr (ASSERTIONS_ENABLED) {
      const uint8_t tag = static_cast<uint8_t>(f.tag);
      if (tag != 0x10 && tag != 0x11)
        throw std::invalid_argument("Unknown bulk request type");
    }
    return {
      .base = f.data[0],
      .count = static_cast<uint16_t>(f.data[1] >> 16),
      .src = f.src,
      .returnTag = (uint16_t) (f.data[1] & 0xFFFF),
      .returnCarried = {f.data[2], f.data[3]},
      .broadcast = static_cast<uint8_t>(f.tag) == 0x10,
    };
  }

  MSHR dispatch(uint32_t offset) const noexcept {
    auto carried = returnCarried;
    if (!broadcast) carried[0] = offset;
    return MSHR {
      .addr = static_cast<uint32_t>(base + offset * MEM_BUS_WIDTH_B),
      .src = static_cast<uint16_t>(broadcast ? 0 : src),
      .id = returnTag,
      .rwdata = {},
      .carried = carried,
      .size = MEM_BUS_WIDTH_SIZE,
      .write = false,
      .flowToken = flowToken,
    };
  }
};

struct RingResp {
  uint16_t tag;
  uint16_t dst; // PU, because ring bus will only carry unicast responses for now
  uint16_t ident;
  MemLine data;
  uint16_t extra0; // High half of carried[0]: manual completion handler.
  uint32_t extra1; // carried[1]: manual completion argument.
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

  size_t inflight;
  std::vector<MSHR> mshrs;
  DynamicBitMask allocated;
  DynamicBitMask fulfilled;
  FlitArb reqArb;
  FixedLenQueue<MshrSlot> issueQueue;
  bool mergeWrites = false;
  std::unordered_map<uint32_t, MshrSlot> lastUnissued;
  DynamicBitMask externalIds{256};
  std::array<std::optional<MshrSlot>, 256> externalSlots{};
  bool externalAccepting = false;
  size_t peakExternalIds = 0;
  static uint32_t lineOf(uint32_t addr) { return addr & MEM_ADDR_ALIGN_MASK; }
  mem_mask_t effectiveMask(const MSHR &r) const {
    const size_t bytes = size_t{1} << r.size;
    if (bytes > MEM_BUS_WIDTH_B || r.addr % bytes)
      throw std::logic_error("invalid merge store size/alignment");
    const auto mask = bytes == MEM_BUS_WIDTH_B ? ~mem_mask_t{0} : (mem_mask_t{1} << bytes) - 1;
    return (mask << (r.addr & MEM_ADDR_OFFSET_MASK)) & r.wbe;
  }
  std::optional<uint8_t> externalTag(MshrSlot slot) const {
    if (inflight <= 256) return static_cast<uint8_t>(slot);
    const auto tag = externalIds.firstZero();
    return tag ? std::optional<uint8_t>(static_cast<uint8_t>(*tag)) : std::nullopt;
  }
  std::optional<MshrSlot> presentedSlot() const {
    const auto slot = mshrReqSlot();
    return slot && externalTag(*slot) ? slot : std::nullopt;
  }
  std::optional<MshrSlot> mergeCandidate(const MSHR &r) const {
    if (!mergeWrites || !r.write || !r.isNoReceipt()) return std::nullopt;
    const auto it = lastUnissued.find(lineOf(r.addr));
    if (it == lastUnissued.end()) return std::nullopt;
    const auto slot = it->second;
    // The frontend has already observed the old head. Never change a request
    // which will be accepted on this edge; no request is presented if IDs ran out.
    if (externalAccepting && presentedSlot() == slot) return std::nullopt;
    const auto &old = mshrs[slot];
    if (!allocated.test(slot) || !old.write || !old.isNoReceipt()) return std::nullopt;
    return slot;
  }
  void mergeCommit(MshrSlot slot, const MSHR &r, bool full) {
    const auto it = lastUnissued.find(lineOf(r.addr));
    if (it == lastUnissued.end() || it->second != slot || !allocated.test(slot) ||
        !mshrs[slot].write || !mshrs[slot].isNoReceipt())
      throw std::logic_error("merge target changed during commit");
    auto &old = mshrs[slot];
    const auto incoming = effectiveMask(r);
    const auto combined = effectiveMask(old) | incoming;
    for (size_t byte=0;byte<MEM_BUS_WIDTH_B;++byte)
      if ((incoming >> byte) & 1) old.rwdata[byte] = r.rwdata[byte];
    old.addr = lineOf(old.addr); old.size = MEM_BUS_WIDTH_SIZE; old.wbe = combined;
    recordWriteProbe(2, slot, r, &old, full ? 1u : 0u);
  }
  // Passive, lossless request log. Only this MemIf's owner thread appends;
  // the main thread publishes it after workers are parked at shutdown.
  static constexpr size_t writeProbeWords = 16 + MEM_BUS_WIDTH_B / 4;
  std::vector<std::array<uint32_t, writeProbeWords>> writeProbeEvents;
  std::vector<std::array<uint32_t,16>> flowMemEvents;
  uint64_t flowSerial=0;
  static bool writeProbeEnabled() {
    static const bool enabled = std::getenv("MORRISON_WRITE_PROBE") != nullptr;
    return enabled;
  }
  void recordWriteProbe(uint32_t kind, MshrSlot slot, const MSHR &r,
                        const MSHR *combined=nullptr, uint32_t aux=0,
                        uint32_t external=UINT32_MAX) {
    if (!writeProbeEnabled() || idx == 0) return;
    if (!morrison_probe::enabled()) throw std::runtime_error("write probe requires phase probe");
    const auto &data = combined ? *combined : r;
    std::array<uint32_t, writeProbeWords> event{};
    event[0]=morrison_probe::cycle; event[1]=morrison_probe::cycle>>32;
    event[2]=morrison_probe::step; event[3]=morrison_probe::phase;
    event[4]=idx-1; event[5]=kind; event[6]=slot; event[7]=r.addr; event[8]=r.size;
    event[9]=uint32_t(r.write)|(uint32_t(r.isNoReceipt())<<1)|(uint32_t(r.isBroadcast())<<2);
    event[10]=r.carried[1];event[11]=uint32_t(r.src)|(uint32_t(r.id)<<16);
    event[12]=uint32_t(data.wbe); event[13]=uint32_t(uint64_t(data.wbe)>>32);
    event[14]=external;event[15]=aux;
    const auto words=std::bit_cast<std::array<uint32_t,MEM_BUS_WIDTH_B/4>>(data.rwdata);
    std::copy(words.begin(),words.end(),event.begin()+16);
    writeProbeEvents.push_back(event);
  }
  IndexedFlitArb<MshrSlot> unicastArb;
  IndexedFlitArb<MshrSlot> broadcastArb;
  std::optional<IdleNotifier> idleNotifier;
  std::optional<IdleReturn> idleReturn;
  uint16_t idleCycles = 0;

  static bool isScalarRequest(const Flit &f) noexcept {
    const uint8_t tag = static_cast<uint8_t>(f.tag);
    return tag == 0x00 || tag == 0x01 || tag == 0x02;
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
    size_t inflight,
    size_t numReq,
    bool merge = false
  ) : idx(idx), inflight(inflight), mshrs(inflight), allocated(inflight),
      fulfilled(inflight), reqArb(numReq), issueQueue(inflight),
      unicastArb(inflight), broadcastArb(inflight) {
    mergeWrites = merge;
    if (inflight < 2 || inflight > 512)
      throw std::invalid_argument("inflight must be between 2 and 512");
  }

  void resetIdle() noexcept {
    idleNotifier = std::nullopt;
    idleReturn = std::nullopt;
    idleCycles = 0;
  }

  /*
    * Shared MSHR state machine and generator
    */

  size_t mshrCapacity() const { return inflight; }
  size_t externalIdCount() const { return inflight > 256 ? externalIds.count() : mshrIssuedUnfulfilledCount(); }
  size_t externalIdPeak() const { return peakExternalIds; }
  bool externalTagBlocked() const { return mshrReqSlot().has_value() && !presentedSlot(); }
  void prepareExternal(bool accepting) { externalAccepting = accepting; }

  size_t mshrInflightCount() const noexcept __attribute__((always_inline)) {
    return allocated.count();
  }

  // Passive timestep-boundary validation; includes unissued posted stores.
  size_t postedStoreCount() const noexcept {
    size_t count = 0;
    for (size_t slot = 0; slot < inflight; ++slot)
      count += allocated.test(slot) && mshrs[slot].write && mshrs[slot].isNoReceipt();
    return count;
  }

  size_t mshrUnissuedCount() const noexcept {
    return issueQueue.size();
  }

  void dumpFlowProbe(const char *directory) const {
    if (idx==0) return;
    std::ofstream out(std::filesystem::path(directory)/("flow-mem-mc"+std::to_string(idx-1)+".bin"),std::ios::binary);
    if (!flowMemEvents.empty()) out.write(reinterpret_cast<const char*>(flowMemEvents.data()),flowMemEvents.size()*sizeof(flowMemEvents[0]));
    if (!out) throw std::runtime_error("Cannot publish scatter-flow probe");
  }
  void dumpWriteProbe(const char *directory) const {
    if (!writeProbeEnabled() || idx == 0) return;
    static_assert(std::endian::native == std::endian::little);
    std::ofstream out(std::filesystem::path(directory) /
                      ("write-probe-mc" + std::to_string(idx - 1) + ".bin"),
                      std::ios::binary);
    const std::array<uint32_t, 8> header{
        0x57524831, 2, uint32_t(writeProbeWords), uint32_t(MEM_BUS_WIDTH_B), uint32_t(idx - 1),
        uint32_t(inflight), 0, 0};
    out.write(reinterpret_cast<const char *>(header.data()), sizeof(header));
    out.write(reinterpret_cast<const char *>(writeProbeEvents.data()),
              writeProbeEvents.size() * sizeof(writeProbeEvents[0]));
    if (!out) throw std::runtime_error("Cannot publish MemIf write probe");
  }

  size_t mshrIssuedUnfulfilledCount() const noexcept {
    return mshrInflightCount() - mshrUnissuedCount() - mshrFulfilledCount();
  }

  size_t mshrFulfilledCount() const noexcept {
    return allocated.countAnd(fulfilled);
  }

  // Passive sampled occupancy: ordinary/bulk unicast versus scatter returns.
  std::array<size_t, 2> mshrFulfilledByClass() const noexcept {
    std::array<size_t, 2> counts{};
    for (size_t slot = 0; slot < inflight; ++slot)
      if (allocated.test(slot) && fulfilled.test(slot))
        ++counts[mshrs[slot].isBroadcast() ? 1 : 0];
    return counts;
  }

  std::optional<MshrSlot> mshrAllocSlot() const noexcept __attribute__((always_inline)) {
    return allocated.firstZero();
  }

  void mshrAllocCommit(MshrSlot slot, MSHR req) noexcept __attribute__((always_inline)) {
    if constexpr (ASSERTIONS_ENABLED) {
      if (allocated.test(slot))
        throw std::logic_error("mshrAllocCommit: slot already allocated");
    }
    // Each queued ID owns an allocated MSHR, so a free MSHR guarantees
    // issue-queue space. Callers capture the old head before committing
    // allocations to preserve the RTL Queue's non-flowing behavior.
    const bool pushed = issueQueue.push(slot);
    if constexpr (ASSERTIONS_ENABLED) {
      if (!pushed)
        throw std::logic_error("mshrAllocCommit: issue queue unexpectedly full");
    }
    allocated.set(slot);
    fulfilled.reset(slot);
    mshrs[slot] = req;
    if (mergeWrites) lastUnissued[lineOf(req.addr)] = slot;
    recordWriteProbe(0, slot, req);
  }

  void mshrDeallocCommit(MshrSlot slot) noexcept __attribute__((always_inline)) {
    allocated.reset(slot);
    fulfilled.reset(slot);
  }

  std::optional<MshrSlot> mshrReqSlot() const noexcept __attribute__((always_inline)) {
    if (auto head = issueQueue.front()) return head->get();
    return std::nullopt;
  }

  GlobalMemReq mshrReq(MshrSlot slot) const noexcept __attribute__((always_inline)) {
    return {
      .id = externalTag(slot).value(),
      .size = mshrs[slot].size,
      .addr = mshrs[slot].addr,
      .wdata = mshrs[slot].rwdata,
      .wbe = mshrs[slot].wbe,
      .write = mshrs[slot].write,
    };
  }

  void mshrReqCommit(MshrSlot slot) __attribute__((always_inline)) {
    if constexpr (ASSERTIONS_ENABLED) {
      if (!allocated.test(slot))
        throw std::logic_error("mshrReqCommit: slot not allocated");
      if (mshrReqSlot() != slot)
        throw std::logic_error("mshrReqCommit: slot is not the issue queue head");
    }
    const auto tag = externalTag(slot);
    if (!tag) throw std::logic_error("issuing without an external request ID");
    if (inflight > 256) {
      if (externalSlots[*tag]) throw std::logic_error("external ID reused before response");
      externalSlots[*tag]=slot; externalIds.set(*tag);
      peakExternalIds=std::max(peakExternalIds,externalIds.count());
    }
    recordWriteProbe(1, slot, mshrs[slot], nullptr, 0, *tag);
    if (mergeWrites) {
      auto it=lastUnissued.find(lineOf(mshrs[slot].addr));
      if (it!=lastUnissued.end() && it->second==slot) lastUnissued.erase(it);
    }
    issueQueue.pop();
  }

  void mshrRespAccept(const GlobalMemResp &resp) {
    MshrSlot slot=resp.id;
    if (inflight > 256) {
      if (!externalSlots[resp.id]) throw std::logic_error("response has no external ID mapping");
      slot=*externalSlots[resp.id];externalSlots[resp.id].reset();externalIds.reset(resp.id);
    }
    if (slot>=inflight || !allocated.test(slot) || fulfilled.test(slot))
      throw std::logic_error("invalid or duplicate MSHR response");
    const bool noReceipt=mshrs[slot].isNoReceipt();
    mshrs[slot].rwdata=resp.data;fulfilled.set(slot);
    recordWriteProbe(3,slot,mshrs[slot],nullptr,0,resp.id);
    if (noReceipt) mshrDeallocCommit(slot);
  }

  std::optional<MshrSlot> mshrReturnSlot(bool broadcast) const noexcept
      __attribute__((always_inline)) {
    const auto &arb = broadcast ? broadcastArb : unicastArb;
    return arb.peek([&](size_t i) -> std::optional<uint8_t> {
      return allocated.test(i) && fulfilled.test(i) &&
                     mshrs[i].isBroadcast() == broadcast
          ? std::optional<uint8_t>(0) : std::nullopt;
    });
  }

  RingResp mshrUnicast(MshrSlot slot) const __attribute__((always_inline)) {
    return RingResp {
      .tag = mshrs[slot].id,
      .dst = mshrs[slot].src,
      .ident = static_cast<uint16_t>(mshrs[slot].carried[0]),
      .data = mshrs[slot].rwdata,
      .extra0 = static_cast<uint16_t>(mshrs[slot].carried[0] >> 16),
      .extra1 = mshrs[slot].carried[1],
    };
  }

  BcastLine mshrBroadcast(MshrSlot slot) const __attribute__((always_inline)) {
    return BcastLine {
      .line = std::bit_cast<std::array<uint32_t, MEM_BUS_WIDTH_B / 4>>(mshrs[slot].rwdata),
      .tag = mshrs[slot].id,
      .carried = mshrs[slot].carried,
      .flowToken = mshrs[slot].flowToken,
    };
  }

  void mshrReturnCommit(MshrSlot slot, bool broadcast) __attribute__((always_inline)) {
    (broadcast ? broadcastArb : unicastArb).commit(slot);
    mshrDeallocCommit(slot);
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
  // Observation only: the request arbiter's choice before capacity gating.
  // Keep admission and arbitration state changes in their existing paths.
  template<typename FI>
  std::optional<uint8_t> nocSelectedPort(FI flits) const
  requires(std::is_invocable_r_v<const Flit*, FI, size_t>) {
    return reqArb.peek([&flits](size_t port) -> std::optional<uint8_t> {
      const Flit *flit = flits(port);
      return flit ? std::optional<uint8_t>(flit->prio()) : std::nullopt;
    });
  }

  template<typename FI>
  [[gnu::always_inline]] std::optional<uint8_t> nocAcceptMultiple(FI flits) const
  requires(std::is_invocable_r_v<const Flit*, FI, size_t>) {
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

  FixedLenQueue<BulkDispatcher> bulkQueue{DRAM_BULK_QUEUE_DEPTH};
  uint32_t bulkCount = 0;
  size_t bcstInflight = 0;
  FixedLenQueue<BcastLine> bcstEgressQueue{2};
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

 public:
  struct CycleEvents {
    bool bulkHeadPresent = false;
    bool allocationFull = false;
    bool bulkScalarPriority = false;
    bool bulkBroadcastLimit = false;
    bool bulkAllocated = false;
    bool incomingRead = false, incomingWrite = false;
    bool mergedWrite = false, mergedWhileFull = false;
    bool idBlocked = false;
    bool externalRequestPresented = false;
    bool externalRequestAccepted = false;
    bool externalRequestWrite = false;
    bool externalResponseAccepted = false;
    bool mshrUnicastLocalRetired = false;
    bool mshrUnicastRemoteInjected = false;
    bool ringUnicastLocalDelivered = false;
    bool broadcastRetired = false;
  };

 private:
  CycleEvents lastCycleEvents;

  struct UnicastRespPeek {
    // The local responses, if exists. Index is pu index - puStart
    std::optional<size_t> ringLocal = std::nullopt;
    std::optional<size_t> mshrLocal = std::nullopt;
    bool mshrRemote = false;
  };

  UnicastRespPeek unicastRespPeek(const RingResp *ingress) const __attribute__((always_inline)) {
    UnicastRespPeek peek;
    if (ingress) if (ingress->dst >= puStart && ingress->dst < puEnd) peek.ringLocal = ingress->dst - puStart;
    if (auto slot = mshrReturnSlot(false)) {
      auto resp = mshrUnicast(*slot);
      if (resp.dst >= puStart && resp.dst < puEnd) peek.mshrLocal = resp.dst - puStart;
      else peek.mshrRemote = true;
    }

    // Arbiter cascade
    if (peek.ringLocal && peek.mshrLocal) {
      size_t ringDist = *peek.ringLocal / CLUSTER_SIZE;
      size_t mshrDist = *peek.mshrLocal / CLUSTER_SIZE;
      if (ringDist == mshrDist) peek.mshrLocal = std::nullopt;
    }

    return peek;
  }

public:
  struct PUResp {
    std::optional<RingResp> unicast;
    std::optional<BcastLine> bcast;
  };

  // Const snapshot of the same fulfilled-response selection used by step().
  bool selectedRemoteUnicast() const { return unicastRespPeek(nullptr).mshrRemote; }

  struct PUAccept {
    bool unicast;
    bool broadcast;
  };

  size_t numClusters() const {
    return (puEnd - puStart) / CLUSTER_SIZE;
  }

  size_t bulkQueueOccupancy() const noexcept { return bulkQueue.size(); }
  size_t broadcastInflightCount() const noexcept { return bcstInflight; }
  const CycleEvents &cycleEvents() const noexcept { return lastCycleEvents; }
  size_t bcstQueueOccupancy() const noexcept {
    size_t n = bcstEgressQueue.size();
    for (const auto &q : bcstQueues) n += q.size();
    return n;
  }

  bool broadcastAllocationBlocked() const noexcept {
    auto head = bulkQueue.front();
    return head && head->get().broadcast &&
           bcstInflight == inflight - DRAM_UNICAST_RESERVE;
  }

protected:
  virtual bool nocCanAccept(const Flit &f) const override __attribute__((always_inline)) {
    if (isIdleRequest(f)) return idleCanAccept(f);
    if (isBulkRequest(f))
      return static_cast<uint16_t>(f.data[1] >> 16) == 0 || !bulkQueue.full();
    if (isScalarRequest(f)) return mergeCandidate(MSHR::fromScalarFlit(f)).has_value() || mshrAllocSlot().has_value();
    return false;
  }

public:
  DRAMIf(size_t memif_idx, size_t numReq, size_t bcstQueueCap,
         size_t puStart, size_t puEnd, ExperimentConfig experiment = {})
    : MemIf(memif_idx, experiment.slots, numReq, experiment.merge),
      puStart(puStart),
      puEnd(puEnd),
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
        pus[rangeDelta].unicast = *unicast;
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
    if (auto slot = presentedSlot()) return MemIf::mshrReq(*slot);
    return std::nullopt;
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
    lastCycleEvents = {};
    *ring.injected = false;

    if constexpr (ASSERTIONS_ENABLED) {
      if (bcstAcceptMasks.size() != numClusters())
        throw std::logic_error("Incorrect broadcast acceptance mask count");
    }

    const Flit *acceptedFlit = flit ? &flit->first.get() : nullptr;
    idleStep(allocated.none() && bulkQueue.empty() && bcstEgressQueue.empty(),
             acceptedFlit, nocOutAccepted);

    const auto allocSlot = mshrAllocSlot();
    const auto issueSlot = presentedSlot();
    const auto unicastSlot = mshrReturnSlot(false);
    const auto broadcastSlot = mshrReturnSlot(true);

    const bool acceptedScalar = acceptedFlit && isScalarRequest(*acceptedFlit);
    const auto incoming = acceptedScalar ? std::optional<MSHR>(MSHR::fromScalarFlit(*acceptedFlit)) : std::nullopt;
    const auto mergeSlot = incoming ? mergeCandidate(*incoming) : std::nullopt;
    lastCycleEvents.incomingRead = incoming && !incoming->write;
    lastCycleEvents.incomingWrite = incoming && incoming->write;
    lastCycleEvents.mergedWrite = mergeSlot.has_value();
    lastCycleEvents.mergedWhileFull = mergeSlot && !allocSlot;
    lastCycleEvents.idBlocked = externalTagBlocked();
    const bool acceptedBulk = acceptedFlit && isBulkRequest(*acceptedFlit) &&
      static_cast<uint16_t>(acceptedFlit->data[1] >> 16) != 0;

    std::optional<BulkDispatcher> bulkHead;
    if (auto head = bulkQueue.front()) bulkHead = head->get();
    const bool bcstBlocked = bulkHead && bulkHead->broadcast &&
      bcstInflight == inflight - DRAM_UNICAST_RESERVE;
    const bool bulkAlloc = allocSlot && bulkHead && !acceptedScalar && !bcstBlocked;
    const bool bulkDone = bulkAlloc && bulkCount + 1 == bulkHead->count;
    lastCycleEvents.bulkHeadPresent = bool(bulkHead);
    lastCycleEvents.allocationFull = !allocSlot;
    lastCycleEvents.bulkScalarPriority = bulkHead && acceptedScalar;
    lastCycleEvents.bulkBroadcastLimit = bool(bulkHead) && bcstBlocked;
    lastCycleEvents.bulkAllocated = bulkAlloc;

    std::optional<BcastLine> centralLine;
    if (auto head = bcstEgressQueue.front()) centralLine = head->get();
    uint64_t centralPushMask = 0;
    if (centralLine) {
      for (size_t dist = 0; dist < numClusters(); ++dist) {
        if ((bcstDistAccepted & (1ULL << dist)) == 0 && !bcstQueues[dist].full())
          centralPushMask |= 1ULL << dist;
      }
    }
    const uint64_t allDists = numClusters() == 64
      ? UINT64_MAX : ((1ULL << numClusters()) - 1);
    const bool centralPop = centralLine &&
      ((bcstDistAccepted | centralPushMask) == allDists);

    const bool broadcastEject = broadcastSlot && !bcstEgressQueue.full();
    std::optional<BcastLine> broadcastLine;
    if (broadcastEject) broadcastLine = mshrBroadcast(*broadcastSlot);

    // Unicast completion & ringbus generation
    auto unicast = unicastRespPeek(ring.ingress ? ring.buffer : nullptr);
    bool unicastDealloc = false;
    for (auto &pipe : unicastPipes) pipe = std::nullopt;

    if (unicast.ringLocal) {
      if constexpr (ASSERTIONS_ENABLED) {
        if (!ring.eject) throw std::logic_error("local ring response was not ejected");
      }
      size_t dist = *unicast.ringLocal / CLUSTER_SIZE;
      unicastPipes[dist] = *ring.buffer;
      lastCycleEvents.ringUnicastLocalDelivered = true;
    }

    if (unicast.mshrLocal) {
      auto resp = MemIf::mshrUnicast(*unicastSlot);
      size_t dist = *unicast.mshrLocal / CLUSTER_SIZE;
      unicastPipes[dist] = resp;
      unicastDealloc = true;
      lastCycleEvents.mshrUnicastLocalRetired = true;
    }

    // Now that we're done with incoming ring ejection, we can overwrite the buffer.
    // For ringbus generation, we check if we're allowed a injection
    if (ring.canInject && unicast.mshrRemote) {
      *ring.injected = true;
      *ring.buffer = MemIf::mshrUnicast(*unicastSlot);
      unicastDealloc = true;
      lastCycleEvents.mshrUnicastRemoteInjected = true;
    }

    if (unicastDealloc) MemIf::mshrReturnCommit(*unicastSlot, false);

    // Distributor bcst acceptance
    for (size_t dist = 0; dist < numClusters(); ++dist) {
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

    // The central MemIf broadcast queue feeds each distributor independently.
    // Its ready signals observe the distributor queues before this edge.
    if (centralLine) {
      for (size_t dist = 0; dist < numClusters(); ++dist) {
        if (centralPushMask & (1ULL << dist)) {
          const bool pushed = bcstQueues[dist].push(*centralLine);
          if constexpr (ASSERTIONS_ENABLED) {
            if (!pushed) throw std::logic_error("Distributor broadcast queue unexpectedly full");
          }
        }
      }
    }
    if (centralPop) {
      const bool popped = bcstEgressQueue.pop();
      if constexpr (ASSERTIONS_ENABLED) {
        if (!popped) throw std::logic_error("Central broadcast queue unexpectedly empty");
      }
      bcstDistAccepted = 0;
    } else {
      bcstDistAccepted |= centralPushMask;
    }

    // External memory handling
    const bool issueFire = mem.reqAccepting && issueSlot.has_value();
    lastCycleEvents.externalRequestPresented = issueSlot.has_value();
    lastCycleEvents.externalRequestAccepted = issueFire;
    // Capture the same head as the handshake, before commits can reuse its slot.
    lastCycleEvents.externalRequestWrite = issueSlot && mshrs[*issueSlot].write;
    lastCycleEvents.externalResponseAccepted = mem.resp.has_value();
    if (issueFire) MemIf::mshrReqCommit(*issueSlot);
    if (mem.resp) MemIf::mshrRespAccept(*mem.resp);

    if (broadcastEject) {
      lastCycleEvents.broadcastRetired = true;
      MemIf::mshrReturnCommit(*broadcastSlot, true);
    }

    if (acceptedScalar) {
      if constexpr (ASSERTIONS_ENABLED) {
        if (!allocSlot && !mergeSlot) throw std::logic_error("Received scalar request without a slot or merge");
      }
      if (mergeSlot) mergeCommit(*mergeSlot, *incoming, !allocSlot);
      else MemIf::mshrAllocCommit(*allocSlot, *incoming);
    } else if (bulkAlloc) {
      MemIf::mshrAllocCommit(*allocSlot, bulkHead->dispatch(bulkCount));
    }

    if (bulkAlloc) {
      if (bulkDone) {
        const bool popped = bulkQueue.pop();
        if constexpr (ASSERTIONS_ENABLED) {
          if (!popped) throw std::logic_error("Bulk queue unexpectedly empty");
        }
        bulkCount = 0;
      } else {
        ++bulkCount;
      }
    }

    if (acceptedBulk) {
      auto dispatcher=BulkDispatcher::fromFlit(*acceptedFlit);
      if (std::getenv("MORRISON_FLOW_PROFILE") && dispatcher.broadcast && (dispatcher.returnTag&15)==5) {
        dispatcher.flowToken=(uint64_t(idx)<<56)|++flowSerial;
        const auto &f=*acceptedFlit;
        flowMemEvents.push_back({uint32_t(morrison_probe::cycle),uint32_t(morrison_probe::cycle>>32),
          morrison_probe::step,morrison_probe::phase,uint32_t(idx-1),0,f.src,f.dst,f.tag,
          f.data[0],f.data[1],f.data[2],f.data[3],uint32_t(dispatcher.flowToken),uint32_t(dispatcher.flowToken>>32),0});
      }
      const bool pushed = bulkQueue.push(dispatcher);
      if constexpr (ASSERTIONS_ENABLED) {
        if (!pushed) throw std::logic_error("Bulk queue unexpectedly full");
      }
    }

    const bool broadcastAlloc = bulkAlloc && bulkHead->broadcast;
    if (broadcastAlloc && !broadcastEject) {
      if constexpr (ASSERTIONS_ENABLED) {
        if (bcstInflight >= inflight - DRAM_UNICAST_RESERVE)
          throw std::logic_error("Broadcast MSHR counter overflow");
      }
      ++bcstInflight;
    } else if (!broadcastAlloc && broadcastEject) {
      if constexpr (ASSERTIONS_ENABLED) {
        if (bcstInflight == 0)
          throw std::logic_error("Broadcast MSHR counter underflow");
      }
      --bcstInflight;
    }

    if (broadcastLine) {
      const bool pushed = bcstEgressQueue.push(*broadcastLine);
      if constexpr (ASSERTIONS_ENABLED) {
        if (!pushed) throw std::logic_error("Central broadcast queue unexpectedly full");
      }
    }

    if (flit) {
      if constexpr (ASSERTIONS_ENABLED) {
        if (!isScalarRequest(*acceptedFlit) && !isBulkRequest(*acceptedFlit) &&
            !isIdleRequest(*acceptedFlit))
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
    if (isScalarRequest(f)) return mshrAllocSlot().has_value();
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
    auto slot = MemIf::mshrReqSlot();
    if (!slot) return std::nullopt;
    auto req = MemIf::mshrReq(*slot);
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
    idleStep(allocated.none(), acceptedFlit, nocOutAccepted);

    // Remember alloc slots
    auto allocSlot = mshrAllocSlot();
    auto issueSlot = mshrReqSlot();

    // Ring state machine
    // We have no local ejection, so always feed into the ring
    if constexpr (ASSERTIONS_ENABLED) {
      if (ring.eject) throw std::logic_error("ring ejection at PeriphIf should never happen");
    }
    if (ring.canInject) {
      auto slot = MemIf::mshrReturnSlot(false);
      if (slot) {
        *ring.buffer = MemIf::mshrUnicast(*slot);
        *ring.injected = true;
        MemIf::mshrReturnCommit(*slot, false);
      }
    }

    // External memory response
    // This is handled before req, because for peripheral, we need to look at the pending request
    // Grant is given to potentially config ROM is external mem does not give a response
    bool romServed = false;
    if (mem.resp) // Contains resp
      mshrRespAccept(*mem.resp);
    else if (issueSlot) {
      auto req = MemIf::mshrReq(*issueSlot);
      auto alignedAddr = req.addr & MEM_ADDR_ALIGN_MASK;
      if (configROM.contains(alignedAddr)) {
        romServed = true;
        mshrRespAccept({
          .id = req.id,
          .data = configROM[alignedAddr]
        });
      }
    }

    // External memory request
    if (romServed || (mem.reqAccepting && peekMem().has_value())) {
      if (issueSlot)
        mshrReqCommit(*issueSlot);
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
          if (!allocSlot) throw std::logic_error("Received a scalar request while no slot is available");
        }
        MemIf::mshrAllocCommit(*allocSlot, MSHR::fromScalarFlit(accepted));
      } else if constexpr (ASSERTIONS_ENABLED) {
        if (!isIdleRequest(accepted))
          throw std::logic_error("Received unknown PeripheralIf request tag");
      }
      nocAcceptCommit(flit->second);
    }
  }
};

}
