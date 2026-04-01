# Session 11 Report: MUL Decode Bug & Multi-MC Scatter Fix

## Problem

After implementing the multi-MC scatter payload (Task 2), the new SNN workload deadlocked in simulation, even with 0-connectivity data that generates no scatter traffic. The old payload (hardcoded single-MC) worked correctly under identical conditions, completing in ~3349 cycles with Result=64.

## Debugging Process

### Symptom Investigation

1. Added per-MC request counting and scatter detection to `system.cpp` — confirmed MC1 was receiving scatter requests (54 scatter reads before deadlock), proving the multi-MC plumbing worked.

2. Added PU PC dumping on stall detection — discovered PUs were cycling between two addresses in the `updateOne` handler body (0x80000108 and 0x80000118), not stuck at a single instruction.

3. Investigated the `yield.1` instruction mechanism in `BIU.scala` and `Exec.scala` — yield.1 waits for `yieldAcked = biu.br.valid` (any event in any EvQueue) before firing. Initially suspected a circular dependency, but the old payload uses the same yield mechanism and works.

### Root Cause Discovery

4. Added **register dumps** (a0, s1, s6, s7) to the periodic PC trace in system.cpp:
   ```
   cycle=5000 PU1 pc=0x80000118 a0=0x20000b70 s1=0x20000012 s6=0x10 s7=0x2
   ```

   - `s1 = 0x20000012` is wrong — should be `0x20000020` (SPM_BASE + nn_count × stride = 0x20000000 + 2 × 16)
   - `a0 = 0x20000b70` has grown far past the expected range (only 2 neurons)

5. `s1` is computed as `SPM_BASE + nn_count * stride` using a `mul` instruction (the old payload used `slli` for fixed stride=16). The incorrect `s1 = 0x20000012` means `mul` returned 18 (0x12) instead of 32 (0x20).

### The Bug

In `Exec.scala` line 100:
```scala
val isMul = !uop.alu2imm && uop.funct7(1)
```

The RISC-V M extension uses `funct7 = 0b0000001` (= 1). `funct7(1)` checks **bit 1**, which is 0. The correct check is `funct7(0)` (bit 0).

This means `isMul` was always `false` — every `mul` instruction was misrouted to the normal ALU path, producing garbage results. The old payload never used `mul` (it used `slli` for fixed stride), so this bug was latent.

The same bug existed in `Data.scala` line 55:
```scala
def isMul = rdalu && !alu2imm && funct7(1)
```

## Fix

Changed `funct7(1)` to `funct7(0)` in both `Data.scala` and `Exec.scala`.

There is no conflict with other funct7 uses:
- Normal R-type ALU: funct7 = 0x00 (bits all zero)
- SUB/SRA: funct7 = 0x20 (bit 5 set)
- M extension: funct7 = 0x01 (bit 0 set)

## Additional Changes

- Renamed `MemResp.tag` to `MemResp.id` for consistency with `MemReq.id` and `GlobalMemResp.id`.
- Updated all references in `Data.scala`, `Crossbar.scala`, `MemDistributor.scala`, `Core.scala`, and `single.cpp`.

## Verification

After the fix, all tests pass:
- 0-connectivity 2MC: Result=64, Cycles=2822
- Connectivity (c=0.005) 2MC: Result=64, Cycles=2822
- High connectivity (c=0.08) with firing neurons, 2MC: Result=64, Cycles=2834
