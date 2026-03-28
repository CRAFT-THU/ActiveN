# DRAMsim3 Deadlock Bug Report

## Symptoms

When DRAMsim3 is enabled (delayed memory responses), the SHA-256 concurrent test
hangs after ~1800 memory requests. All responses have been delivered, but no new
requests are generated. The hardware appears idle. Without DRAMsim3 (instant
responses), the test completes correctly.

## Method of Identification

### Step 1: Narrow to pipeline corruption

Added debug printfs to the LSU and Exec stages. Without DRAMsim3: zero bad
addresses. With DRAMsim3: 1398 accesses to address `0x00000000` from
PC `0x80000084`, which disassembles to `addi gp, gp, 1` — an ALU instruction,
not a memory instruction. This indicated pipeline state corruption: the core was
decoding wrong instruction data.

### Step 2: Trace to ICache

Added debug printfs to the ICache for unexpected responses — cases where
`mem.resp.valid` is asserted while the ICache is not in a refill state.
Result: **4 unexpected ICache responses** with `tag=0x0000`:

- 3× `s1hit=1` (cache already has data, not expecting a refill)
- 1× `s1refillWriting=1` (writing a different beat, not expecting new data)

All had `tag=0x0000`, which is the ICache's beat-0 response tag after Crossbar
encoding. This confirmed that stale/misrouted memory responses were arriving at
the ICache when not expected.

### Step 3: Root cause analysis

Analyzed the MemIf response delivery path through the source code. The key
insight: **write requests are marked `completed` immediately on issue** (since
the MemIf doesn't need write data back), but DRAMsim3 still fires a write
callback later, which the driver delivers as a `mem.resp`.

The race condition:

1. **Cycle N**: Write slot `S` is issued → `completed(S) := true`. Delivered to
   PU via MemDistributor. Slot freed: `allocated(S) := false`.
2. **Cycle N+K**: A new read request allocates slot `S` (now free).
   Section 1 (flit collection): `allocated(S) := true`, `completed(S) := false`,
   `pending(S).id := 0` (default, flit 1 hasn't arrived yet).
3. **Same cycle N+K**: DRAMsim3's write callback fires → driver presents
   `resp_valid=1, resp_id=S`.
   Section 3 (receive response): `completed(S) := true`.
4. **Chisel last-connect semantics**: Section 3 appears after Section 1 in the
   source, so `completed(S) := true` **wins**.
5. Slot `S` is now allocated for the new request but **prematurely marked
   completed** with `pending(S).id = 0x0000` (default value, before the second
   flit updates it).
6. The MemIf delivers this bogus response with `id=0x0000` to the wrong PU.
7. The PU's ICache sees an unexpected response with `tag=0x0000`. If the ICache
   was in a hit state, the response is dropped. If it was mid-refill, the
   response corrupts the refill data.
8. The **actual request's response is lost** (its slot was freed in step 5
   without ever receiving real data) → the PU hangs forever waiting for a
   response that never comes.

With instant responses, the write callback fires immediately (before the slot can
be freed and reallocated), so the race never occurs.

## Fix

Two complementary fixes were applied:

### 1. Driver fix (sim/src/system.cpp)

The `dramWriteCb` callback no longer pushes to `mem_resps`. Since the MemIf
already marks writes as completed on issue, the DRAMsim3 callback is only needed
for timing simulation — the response should not be delivered to hardware.

```cpp
void dramWriteCb(int mc, uint64_t addr) {
    auto &d = dram_mcs[mc];
    auto it = d.inflight.find(addr);
    if (it != d.inflight.end()) {
      // Don't deliver: MemIf marks writes completed on issue.
      d.inflight.erase(it);
    }
}
```

### 2. Hardware guard (bus/MemIf.scala)

Added an `allocated` check before accepting a memory response, so stale
responses for freed slots are ignored:

```scala
when(mem.resp.valid && allocated(mem.resp.bits.id)) {
    data(mem.resp.bits.id)      := mem.resp.bits.rdata
    completed(mem.resp.bits.id) := true.B
}
```

This is defense-in-depth: if a stale response arrives for a freed (but not yet
reallocated) slot, the `allocated` check reads `false` and ignores it.

## Verification

| Mode | Result | Cycles |
|------|--------|--------|
| Without DRAMsim3 | 0xa6e58910 ✓ | 22,840 |
| With DRAMsim3 | 0xa6e58910 ✓ | 29,997 |

Both the SHA-256 concurrent test and the pingpong test pass with DRAMsim3
enabled. The 31% cycle increase reflects actual HBM latency modeling.
