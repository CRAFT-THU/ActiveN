#include "soft_mem.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <numeric>
#include <random>

using namespace soft_mem;

static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
template<class Exception, class F>
static void rejects(F operation, const char *message) {
  try { operation(); } catch (const Exception &) { return; }
  throw std::runtime_error(message);
}

struct TestDram : DRAMIf {
  explicit TestDram(size_t n, bool merge = false,
                    AllocationMode mode = AllocationMode::Sequential, size_t reserve = 0)
      : DRAMIf(1, 1, 2, 1, 17, {n, merge, mode, reserve}) {}
  size_t cursor() const { return allocationCursor; }
  std::optional<MshrSlot> oldest() const { return oldestAllocated; }
  void corruptIssue(MshrSlot slot) { issueQueue.pop(); issueQueue.push(slot); }
};

static Flit write(uint32_t addr, uint32_t value = 1) {
  return {1, 0x8001, 2, {addr, 2u << 16, 0, value}};
}
static Flit bulk(bool broadcast, uint16_t count) {
  return {1, 0x8001, static_cast<uint16_t>(broadcast ? 0x10 : 0x11),
          {0, uint32_t(count) << 16, 0, 0}};
}
static bool tick(TestDram &d, const Flit *flit = nullptr, bool accepting = false,
                 std::optional<GlobalMemResp> response = {}) {
  d.prepareExternal(accepting);
  auto port = d.nocAcceptMultiple([&](size_t) { return flit; });
  bool injected = false; RingResp ring{}; uint16_t mask = 0xffff;
  MemBusIn mem{accepting, response};
  std::optional<std::pair<std::reference_wrapper<const Flit>, uint8_t>> input;
  if (port) input = {{std::cref(*flit), *port}};
  d.step({&ring, false, false, true, &injected}, mem, input,
         std::span<const uint16_t>(&mask, 1), false);
  return bool(port);
}

static void configuration() {
  unsetenv("SOFT_MEMIF_MSHRS"); unsetenv("SOFT_MEMIF_WRITE_MERGE");
  require(experimentConfig().isBaseline(), "default configuration changed");
  setenv("SOFT_MEMIF_MSHRS", "513", 1);
  auto c = experimentConfig(); require(c.slots == 513 && !c.merge, "MSHR environment/default merge changed");
  setenv("SOFT_MEMIF_MSHRS", "invalid", 1);
  c = experimentConfig({"1", "sequential", "off", "0"});
  require(c.slots == 1 && c.allocation == AllocationMode::Sequential && !c.merge && !c.unicastReserve,
          "explicit options did not override bad environment");
  rejects<std::invalid_argument>([] { experimentConfig(); }, "bad environment accepted");
  unsetenv("SOFT_MEMIF_MSHRS"); unsetenv("SOFT_MEMIF_WRITE_MERGE");
  for (const char *s : {"0", "65537", "-1", "+1", "", "1x", " 1", "1 ", "18446744073709551616"})
    rejects<std::invalid_argument>([&] { experimentConfig({s, {}, {}, "0"}); }, "invalid MSHR CLI value accepted");
  for (const char *s : {"-1", "+1", "", "1x", "18446744073709551616", "65537"})
    rejects<std::invalid_argument>([&] { experimentConfig({"65536", {}, {}, s}); }, "invalid reservation accepted");
  require(experimentConfig({"65536", "first-free", "on", "65536"}).unicastReserve == 65536,
          "reservation equal to capacity rejected");
  rejects<std::invalid_argument>([] { experimentConfig({"1"}); }, "small pool accepted incompatible default reserve");
  rejects<std::invalid_argument>([] { experimentConfig({{}, "unknown"}); }, "bad allocation name accepted");
  for (const char *text : {"on", "true"})
    require(experimentConfig({{}, {}, text}).merge, "enabled merge spelling rejected");
  for (const char *text : {"off", "false"})
    require(!experimentConfig({{}, {}, text}).merge, "disabled merge spelling rejected");
  for (const char *text : {"0", "1", "2", "yes", ""})
    rejects<std::invalid_argument>([&] { experimentConfig({{}, {}, text}); }, "bad merge setting accepted");
  for (const char *text : {"1", "on", "invalid"}) {
    setenv("SOFT_MEMIF_WRITE_MERGE", text, 1);
    require(!experimentConfig().merge, "removed merge environment still affects default");
    require(experimentConfig({{}, {}, "true"}).merge, "removed environment affects explicit CLI");
  }
  unsetenv("SOFT_MEMIF_WRITE_MERGE");
  rejects<std::invalid_argument>([] { TestDram d(1, false, AllocationMode::Sequential, 2); }, "DRAM constructor accepted R>N");
  rejects<std::invalid_argument>([] { TestDram d(0); }, "DRAM constructor accepted zero capacity");
  rejects<std::invalid_argument>([] { TestDram d(65537); }, "DRAM constructor accepted excessive capacity");
  TestDram equal(1, false, AllocationMode::Sequential, 1);
  require(equal.mshrCapacity() == 1, "R=N constructor failed");
  std::cout << "PASS configuration defaults/env precedence/parse/ranges/reservation validation\n";
}

static void ageOracle() {
  std::mt19937 random(0x19260817);
  for (size_t n : {1u, 2u, 3u, 5u, 17u, 33u, 257u, 513u}) {
    TestDram d(n);
    std::vector<std::optional<uint64_t>> live(n);
    size_t cursor = 0; uint64_t serial = 0;
    for (size_t step = 0; step < 6000; ++step) {
      size_t freeWindow = 0;
      while (freeWindow < n && !live[(cursor + freeWindow) % n]) ++freeWindow;
      std::optional<size_t> oldest;
      size_t occupied = 0;
      for (size_t i = 0; i < n; ++i) if (live[i]) {
        ++occupied;
        if (!oldest || *live[i] < *live[*oldest]) oldest = i;
      }
      require(d.cursor() == cursor && d.oldest() == oldest, "oldest/cursor disagrees with allocation-age oracle");
      require(d.mshrAllocatableCount() == freeWindow, "reachable window disagrees with scanned oracle");
      require(d.mshrInflightCount() == occupied, "physical occupancy mismatch");
      require(d.mshrCursorBlocked() == (bool(live[cursor]) && occupied < n), "cursor/full distinction wrong");
      auto slot = d.mshrAllocSlot();
      require(bool(slot) == !live[cursor] && (!slot || *slot == cursor), "sequential allocation skipped cursor");
      if (slot && (!occupied || random() % 3 != 0)) {
        const auto f = write(static_cast<uint32_t>(cursor * MEM_BUS_WIDTH_B));
        d.mshrAllocCommit(*slot, MSHR::fromScalarFlit(f));
        auto req = d.peekMem(); require(req && req->id == cursor, "cyclic request ID mismatch");
        // Repeated presentation must not advance the expected accepted ID.
        require(d.peekMem()->id == req->id && d.peekMem()->id == req->id, "peek advanced request sequence");
        d.mshrReqCommit(req->id);
        live[cursor] = serial++; cursor = (cursor + 1) % n;
      } else if (occupied) {
        size_t victim = random() % n;
        while (!live[victim]) victim = (victim + 1) % n;
        d.mshrRespAccept({static_cast<uint16_t>(victim), {}}); live[victim].reset();
      }
    }
    std::cout << "PASS age-oracle slots=" << n << " transitions=6000\n";
  }
}

static void admission() {
  for (auto mode : {AllocationMode::FirstFree, AllocationMode::Sequential})
  for (bool merge : {false, true}) {
    TestDram d(4, merge, mode);
    std::array<Flit, 4> writes;
    for (size_t i = 0; i < 4; ++i) {
      writes[i] = write(i * MEM_BUS_WIDTH_B);
      require(tick(d, &writes[i]), "initial scalar allocation refused");
    }
    const auto match = write(2 * MEM_BUS_WIDTH_B + 4, 0x12345678);
    require(!tick(d, &match) && !d.cycleEvents().mergedWrite, "full pool admitted a merge");
    tick(d, nullptr, true); tick(d, nullptr, true); // Issue0 and1; hold both responses.
    tick(d, nullptr, false, GlobalMemResp{1, {}}); // Hole1, oldest0 remains live.
    const auto before = d.mshrUnissuedCount();
    if (mode == AllocationMode::Sequential) {
      require(!tick(d, &match) && d.cycleEvents().cursorBlocked && !d.cycleEvents().allocationFull,
              "busy cursor with holes admitted a scalar or was labelled full");
      require(d.mshrUnissuedCount() == before, "rejected merge changed queue");
      require(!tick(d, &match, false, GlobalMemResp{0, {}}), "same-edge release bypassed cursor admission");
    }
    const size_t cursor = d.cursor();
    require(tick(d, &match), "available allocation did not admit scalar");
    require(d.cycleEvents().mergedWrite == merge, "merge choice did not take effect");
    require(d.mshrUnissuedCount() == before + !merge, "merge allocated a slot or nonmerge lost one");
    if (mode == AllocationMode::Sequential)
      require(d.cursor() == (merge ? cursor : (cursor + 1) % 4), "merge advanced cursor");
  }
  TestDram d(3, true);
  auto a = write(0), b = write(4), c = write(8);
  require(tick(d, &a) && tick(d, &b), "merge setup refused");
  require(d.cursor() == 1 && d.peekMem()->id == 0, "merge created an ID gap");
  for (int i = 0; i < 3; ++i) { tick(d); require(d.peekMem()->id == 0, "backpressure advanced ID"); }
  require(tick(d, &c, true, GlobalMemResp{0, {}}), "same-edge scalar refused despite a free cursor");
  require(!d.cycleEvents().mergedWrite && d.peekMem()->id == 1, "merged into accepting head or skipped ID");
  tick(d, nullptr, true, GlobalMemResp{1, {}});
  require(d.mshrInflightCount() == 0 && d.cursor() == 2 && !d.oldest(), "nonzero empty cursor reset");
  require(tick(d, &a) && d.peekMem()->id == 2, "empty interval reset cyclic request IDs");

  TestDram corrupt(3); require(tick(corrupt, &a), "assertion setup"); corrupt.corruptIssue(1);
  rejects<std::logic_error>([&] { corrupt.peekMem(); }, "presentation ID assertion disabled");
  rejects<std::logic_error>([&] { corrupt.mshrReqCommit(1); }, "accepted ID assertion disabled");
  std::cout << "PASS admission modes/merge choices/full/holes/same-edge/backpressure/ID assertion\n";
}

static void histograms() {
  std::vector<uint64_t> histogram;
  std::map<size_t, uint64_t> oracle;
  const std::vector<size_t> observed{0, 513, 32, 65536, 513, 1, 32, 0, 65535, 65536};
  for (size_t n : observed) { recordOccupancy(histogram, n); ++oracle[n]; }
  require(std::accumulate(histogram.begin(), histogram.end(), uint64_t{0}) == observed.size(),
          "histogram sample count changed");
  for (size_t i = 0; i < histogram.size(); ++i)
    require(histogram[i] == oracle[i], "histogram clamped or mixed occupancy values");
  require(histogram[32] == 2 && histogram[513] == 2 && histogram[65536] == 2,
          "independent queue/high MSHR bins were lost");
  std::cout << "PASS exact histograms samples=10 max_value=65536 queue occupancy above small pools\n";
}

static void reservation() {
  for (auto mode : {AllocationMode::FirstFree, AllocationMode::Sequential})
  for (bool broadcast : {false, true}) {
    TestDram d(5, false, mode, 2);
    auto descriptor = bulk(broadcast, 6);
    require(tick(d, &descriptor) && d.mshrInflightCount() == 0, "descriptor bypassed non-flowing queue");
    for (size_t i = 0; i < 3; ++i) {
      tick(d);
      require(d.mshrInflightCount() == i + 1 && d.mshrAllocatableCount() == 4 - i,
              "bulk did not leave the required allocation reserve");
    }
    tick(d);
    require(d.mshrInflightCount() == 3 && d.bulkReservationBlocked() &&
            d.cycleEvents().bulkReservationLimit, "bulk exceeded reservation boundary");
    require(d.broadcastAllocationBlocked() == broadcast, "broadcast metric included unicast bulk");
    for (size_t i = 0; i < 2; ++i) {
      auto scalar = write(0x1000 + i * MEM_BUS_WIDTH_B);
      require(tick(d, &scalar), "reserved scalar allocation unavailable");
    }
    require(d.mshrInflightCount() == 5, "reserved allocations did not fill pool");
    for (size_t i = 0; i < 5; ++i) {
      auto req = d.peekMem(); require(req && req->id == i, "mixed scalar/bulk ID sequence changed");
      d.mshrReqCommit(req->id);
    }

    TestDram zero(3, false, mode, 0);
    auto four = bulk(broadcast, 4); require(tick(zero, &four), "R0 descriptor rejected");
    for (size_t i = 0; i < 3; ++i) tick(zero);
    require(zero.mshrInflightCount() == 3, "R0 did not permit entire pool");
    tick(zero); require(zero.mshrInflightCount() == 3, "R0 overallocated full pool");

    TestDram equal(3, false, mode, 3);
    auto one = bulk(broadcast, 1); require(tick(equal, &one), "R=N descriptor rejected at FIFO");
    tick(equal); require(equal.mshrInflightCount() == 0 && equal.bulkReservationBlocked(), "R=N allocated bulk");
    auto scalar = write(0x1000); require(tick(equal, &scalar), "R=N blocked scalar");

    TestDram holes(5, false, mode, 2);
    for (size_t i = 0; i < 3; ++i) {
      auto f = write(0x1000 + i * MEM_BUS_WIDTH_B); require(tick(holes, &f), "hole setup allocation");
    }
    for (size_t i = 0; i < 3; ++i) tick(holes, nullptr, true);
    require(tick(holes, &one), "hole setup descriptor");
    tick(holes, nullptr, false, GlobalMemResp{1, {}});
    require(!holes.cycleEvents().bulkAllocated && holes.mshrInflightCount() == 2,
            "same-edge freeing bypassed reservation snapshot");
    tick(holes);
    if (mode == AllocationMode::FirstFree) {
      require(holes.cycleEvents().bulkAllocated && holes.mshrInflightCount() == 3,
              "first-free did not use a free hole for bulk");
    } else {
      require(!holes.cycleEvents().bulkAllocated && holes.mshrAllocatableCount() == 2,
              "sequential counted an unreachable hole as reserved capacity");
      tick(holes, nullptr, false, GlobalMemResp{0, {}});
      require(!holes.cycleEvents().bulkAllocated && holes.mshrAllocatableCount() == 4,
              "oldest retirement failed to expose the free window");
      tick(holes);
      require(holes.cycleEvents().bulkAllocated && holes.mshrAllocatableCount() == 3,
              "bulk stayed blocked after oldest retired");
    }

    TestDram priority(5, false, mode, 2);
    require(tick(priority, &one), "scalar priority descriptor");
    require(tick(priority, &scalar) && !priority.cycleEvents().bulkAllocated,
            "bulk stole allocation from scalar");
    tick(priority); require(priority.cycleEvents().bulkAllocated, "bulk failed after scalar priority cycle");
    TestDram empty(1, false, mode, 1);
    auto noBeats = bulk(broadcast, 0);
    require(tick(empty, &noBeats) && empty.bulkQueueOccupancy() == 0 && empty.mshrInflightCount() == 0,
            "zero-length bulk consumed capacity");
    std::cout << "PASS reservation mode=" << allocationName(mode) << " broadcast=" << broadcast
              << " R0/R=N/bulk classes/scalar reserve/holes/pre-edge/priority/empty descriptor\n";
  }
}

static void simultaneousRetirement() {
  TestDram d(5);
  Flit read{1, 0x8001, 0, {0, uint32_t(MEM_BUS_WIDTH_SIZE) << 16, 0, 0}};
  auto posted = write(MEM_BUS_WIDTH_B), descriptor = bulk(true, 1);
  require(tick(d, &read) && tick(d, &posted) && tick(d, &descriptor), "multi-retirement setup");
  tick(d); require(d.broadcastInflightCount() == 1 && d.mshrInflightCount() == 3, "broadcast setup failed");
  for (size_t i = 0; i < 3; ++i) tick(d, nullptr, true);
  d.mshrRespAccept({0, {}}); d.mshrRespAccept({2, {}});
  auto next = write(3 * MEM_BUS_WIDTH_B);
  require(tick(d, &next, false, GlobalMemResp{1, {}}), "allocation during multiple retirements refused");
  require(d.cycleEvents().mshrUnicastLocalRetired && d.cycleEvents().broadcastRetired &&
          d.cycleEvents().externalResponseAccepted, "retirements were not simultaneous");
  require(d.mshrInflightCount() == 1 && d.oldest() == 3 && d.cursor() == 4 &&
          d.broadcastInflightCount() == 0 && d.peekMem()->id == 3,
          "oldest lost during empty-to-nonempty transition on one edge");
  std::cout << "PASS simultaneous unicast/posted/broadcast retirement and new allocation\n";
}

int main() {
  configuration(); ageOracle(); admission(); histograms(); reservation(); simultaneousRetirement();
}
