#include "soft_mem.h"
#include <cstddef>
#include <iostream>
#include <limits>
#include <type_traits>

using namespace soft_mem;

static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

template <class Exception, class F>
static void rejects(F operation, const char *message) {
  try { operation(); } catch (const Exception &) { return; }
  throw std::runtime_error(message);
}

// Exercise the shared MSHR machinery without a DRAM broadcast reservation.
struct TestMemIf : MemIf {
  explicit TestMemIf(size_t count, AllocationMode mode = AllocationMode::FirstFree)
      : MemIf(0, count, 1, false, mode) {}
  bool ringIsLocal(const RingResp &) const noexcept override { return false; }
  bool nocCanAccept(const Flit &) const override { return mshrAllocSlot().has_value(); }
  std::optional<GlobalMemReq> peekMem() const override {
    if (auto slot = mshrReqSlot()) return mshrReq(*slot);
    return std::nullopt;
  }
};

static MSHR request(size_t slot, bool posted) {
  MSHR result{};
  result.addr = slot * MEM_BUS_WIDTH_B;
  result.src = posted ? 0xffff : 1;
  result.id = 0x1234;
  result.size = MEM_BUS_WIDTH_SIZE;
  result.write = posted;
  return result;
}

int main() {
  static_assert(std::is_same_v<decltype(GlobalMemReq::id), uint16_t>);
  static_assert(std::is_same_v<decltype(GlobalMemResp::id), uint16_t>);
  static_assert(std::is_standard_layout_v<GlobalMemReq>);
  static_assert(std::is_standard_layout_v<GlobalMemResp>);
  static_assert(offsetof(GlobalMemReq, id) == 0);
  static_assert(offsetof(GlobalMemReq, size) == 2);
  static_assert(offsetof(GlobalMemReq, addr) == 4);
  static_assert(offsetof(GlobalMemReq, wdata) == 8);
  static_assert(offsetof(GlobalMemReq, wbe) == 8 + MEM_BUS_WIDTH_B);
  static_assert(alignof(GlobalMemReq) == alignof(mem_mask_t));
  static_assert(MEM_BUS_WIDTH_B == 32 || MEM_BUS_WIDTH_B == 64);
  static_assert(sizeof(GlobalMemReq) == (MEM_BUS_WIDTH_B == 32 ? 48 : 88));
  static_assert(offsetof(GlobalMemResp, data) == 2);
  static_assert(alignof(GlobalMemResp) == 2);
  static_assert(sizeof(GlobalMemResp) == MEM_BUS_WIDTH_B + 2);

  for (size_t count : {size_t{0}, size_t{65537}, std::numeric_limits<size_t>::max()})
    rejects<std::invalid_argument>([&] { TestMemIf invalid(count); }, "invalid capacity accepted");

  for (auto mode : {AllocationMode::FirstFree, AllocationMode::Sequential})
  for (size_t count : {1u, 257u, 513u, 65535u, 65536u}) {
    TestMemIf mem(count, mode);
    for (size_t i = 0; i < count; ++i) {
      auto slot = mem.mshrAllocSlot();
      require(slot && *slot == i, "first-free allocation truncated or skipped an index");
      mem.mshrAllocCommit(*slot, request(i, true));
    }
    require(!mem.mshrAllocSlot(), "full pool has an allocation candidate");
    for (size_t i = 0; i < count; ++i) {
      auto req = mem.peekMem();
      require(req && req->id == i && req->addr == i * MEM_BUS_WIDTH_B,
              "direct ID/address truncated or issue window exhausted");
      mem.mshrReqCommit(req->id);
    }
    require(!mem.peekMem() && mem.externalIdCount() == count,
            "could not issue the whole pool before responses");
    // Descending responses recycle posted writes without quadratic arbiter scans.
    for (size_t i = count; i != 0; --i)
      mem.mshrRespAccept({static_cast<uint16_t>(i - 1), {}});
    require(mem.mshrInflightCount() == 0 && mem.externalIdCount() == 0,
            "posted response leaked an MSHR");
    rejects<std::logic_error>([&] { mem.mshrRespAccept({0, {}}); }, "duplicate response accepted");

    // The issue queue wraps and slot zero becomes usable again.
    require(mem.mshrAllocSlot() == 0, "first slot not reusable after drain");
    mem.mshrAllocCommit(0, request(0, true));
    require(mem.peekMem() && mem.peekMem()->id == 0, "issue queue wrap failed");
    mem.mshrReqCommit(0); mem.mshrRespAccept({0, {}});
    std::cout << "PASS mode=" << allocationName(mode) << " capacity=" << count << " outstanding=" << count
              << " max_id=" << count - 1 << " reverse_responses/queue_wrap\n";
  }

  // Exercise a high-ID read response and the actual return arbiter's wrap.
  TestMemIf mem(65536);
  for (uint16_t slot : {uint16_t{65535}, uint16_t{0}}) {
    mem.mshrAllocCommit(slot, request(slot, false));
    auto req = mem.peekMem(); require(req && req->id == slot, "high read ID changed");
    mem.mshrReqCommit(slot);
    MemLine data{}; data.front() = 0x42; data.back() = 0x81;
    mem.mshrRespAccept({slot, data});
    require(mem.mshrReturnSlot(false) == slot, "return arbiter high-index/wrap failure");
    auto response = mem.mshrUnicast(slot);
    require(response.data == data && response.tag == 0x1234 && response.dst == 1,
            "high-ID read data or metadata changed");
    rejects<std::logic_error>([&] { mem.mshrRespAccept({slot, data}); }, "fulfilled duplicate accepted");
    mem.mshrReturnCommit(slot, false);
  }
  TestMemIf small(1);
  rejects<std::logic_error>([&] { small.mshrRespAccept({65535, {}}); }, "out-of-range response accepted");
  std::cout << "PASS line_bytes=" << MEM_BUS_WIDTH_B << " req_size=" << sizeof(GlobalMemReq)
            << " resp_size=" << sizeof(GlobalMemResp)
            << " layout/capacity_validation/high_read/arbiter_wrap/response_validation\n";
}
