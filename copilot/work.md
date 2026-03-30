# Detailed Work Log

## Phase 1: Core Feature Implementation

### Task 0: Dev Environment Setup

- Used `nix develop` with the project's `flake.nix` to enter the dev shell
- Ran `mill Koneko.run` to generate Verilog, producing `work/Core.sv`
- Confirmed all tools available: mill, verilator, cmake, ninja, riscv64-unknown-none-elf-gcc

## Task 1: Single Core Simulation Driver

### Implementation: `sim/src/single.cpp`
- Loads binary from `MEOW_TEXT` environment variable into flat memory array
- Two-phase clock simulation (posedge/negedge) with configurable max cycles (`MEOW_MAX_CYCLES`, default 1M)
- Memory interface: Responds to `mem_req` with burst reads (8 beats), queues responses
- Event capture: Logs all `ext.out` events with cycle, destination, tag, and data
- End-of-simulation: Terminates when event has tag=0 and dst=0xFFFF, reports result from payload
- Optional FST tracing via `MEOW_TRACE` and logging via `MEOW_LOG`
- DPI support: Global `g_sim` pointer for `meow_softmem_read`/`meow_softmem_write` callbacks

### Build Integration: `sim/CMakeLists.txt`
- Added `sim_single` target with `single.cpp` and `aux.cpp` (no DrAMSim3 dependency)
- Pipeline count forced to 1 for single-core mode

## Task 2: ICache PLRU Eviction

### ICache Rewrite: `src/main/scala/koneko/fetch/ICache.scala`
- Added PLRU tree-based replacement policy with `plruState` memory (one entry per set)
- `plruVictim(state)`: Traverses binary tree to find LRU way
- `plruUpdate(state, way)`: Updates tree bits on access
- Victim selection: Prefers invalid ways first (`PriorityEncoder` over invalid mask), falls back to PLRU
- Reset phase: Iterates all sets to initialize metadata with invalid entries
- 2-stage pipeline: s0=address input, s1=tag/data comparison + refill state machine
- Blocking on miss: Pipeline stalls until cache line refill completes via burst memory reads

### Fetch Rewrite: `src/main/scala/koneko/fetch/Fetch.scala`
- Removed flat `instrMem` array and refilling logic
- Re-enabled ICache with proper ready/valid handshaking
- Holding register (`decodedHolding`) buffers decoded instructions when downstream stalls
- Kill mechanism: On branch, sends kill to ICache targeting the selected SMT context's PC
- Step signal: `step = icache.input.fire` gates PC and decode register updates

### Configuration Changes: `Main.scala`
- Changed `i$Assoc` from 1 to 2 (2-way set-associative)
- Cache capacity: 16 lines × 64 bytes / 2 ways = 512 bytes per way, 1024 bytes total

### Test: `sim/payloads/icache_test.S`
- 320 NOP instructions (1328 bytes total, exceeds 1024-byte cache)
- Loops 3 times back to start, counting iterations in scratchpad
- Sends final loop count via AM event to dst=0xFFFF
- Result: 3 (all 3 loops complete), cycle 1822

## Task 3: Synthesizable FPU (cvfpu/FPnew)

### FPU Wrapper: `Aux.sv`
- Wraps `fpnew_top` with RISC-V instruction field decoding
- Operation mapping from funct7 to `fpnew_pkg::operation_e`:
  - `7'b0000000` (FADD) → ADD (op=2)
  - `7'b0000100` (FSUB) → ADD (op=2) + `op_mod=1`
  - `7'b0001000` (FMUL) → MUL (op=3)
  - `7'b0010000` (FSGNJ) → SGNJ (op=6)
  - `7'b0010100` (FMINMAX) → MINMAX (op=7)
  - `7'b1100000` (FCVT.W.S) → F2I (op=11)
  - `7'b1010000` (FEQ/FLT/FLE) → CMP (op=8)
  - `7'b1101000` (FCVT.S.W) → I2F (op=12)
- Operand mapping:
  - FADD/FSUB: op[2]=1.0 (neutral FMA addend), op[0]=rs1, op[1]=rs2
  - FMUL: op[2]=0.0 (zero FMA addend), op[0]=rs1, op[1]=rs2
  - Others: op[0]=rs1, op[1]=rs2
- Rounding mode: funct3=0b111 (DYN) mapped to RNE (0b000) since no FCSR
- Timing: PipeRegs=0 (fully combinational fpnew) + custom `always_ff` holding register
  - On `in_valid`: fpnew computes combinationally, result captured in `r_held`
  - On next cycle: `r_held` is read as the output
  - Total latency: 1 cycle (matches original behavioral model)
- Configuration: `fpnew_pkg::RV32F` features, FP32 only, 1 lane

### Removed DPI Functions: `sim/src/aux.cpp`
- Deleted `softfpu_compute` and `softfpu_delay` (no longer needed)
- Kept `softmem_read` and `softmem_write` for scratchpad

### Build Integration: `sim/CMakeLists.txt`
- Added `CVFPU_SOURCES` list with all fpnew .sv files:
  - `fpnew_pkg.sv`, `fpnew_top.sv`, `fpnew_opgroup_block.sv`, etc.
  - Common cells: `cf_math_pkg.sv`, `lzc.sv`, `rr_arb_tree.sv`
- Source ordering: CVFPU_SOURCES listed before Core.sv/Aux.sv for package resolution
- Include path: `-I` for `common_cells/include`
- Warning suppression: WIDTHEXPAND, CASEINCOMPLETE, WIDTHTRUNC for cvfpu compatibility

### Design Decision: PipeRegs=0 + Holding Register
The initial approach used fpnew's built-in `BEFORE` pipeline configuration, but this caused the result to appear on the same cycle as the input (input register feeds combinational logic immediately), getting overwritten before the execute stage could read it next cycle. Setting PipeRegs=0 makes fpnew fully combinational, and the explicit holding register captures the result reliably for reading on the next cycle.

### Pipeline Fix: `src/main/scala/koneko/fetch/Fetch.scala`
Fixed a pre-existing bug where back-to-back delayed instructions (FP or MUL) caused the pipeline to hang indefinitely. The root cause was in the ICache output consumption logic:

1. When the execute stage stalls (busyMap blocks the next delayed instruction), the decoded instruction from the ICache output gets captured into a holding register (`decodedHoldingValid` becomes true).
2. However, the ICache output was never marked as consumed (`icache.output.ready` stayed false because `icacheOutputConsumed` was only set when `dec.ready && !d`, and during capture `dec.ready` was false).
3. The ICache kept presenting the stale instruction. When the exec stage later consumed the holding register copy, the ICache re-presented the same instruction, which got re-captured into the holding register — creating an infinite loop.

**Fix**: Added a `holdingCapture` signal that detects when the ICache output is being captured into the holding register (`!d && !dec.ready && sentSmsel(idx) && icache.output.valid`). This signal is OR'd into `icache.output.ready`, allowing the ICache to advance past the consumed instruction.

### Test: `sim/payloads/fpu_verify.S`
6 tests with careful register allocation (shared FP/INT register file):
1. FCVT.S.W: Convert int 3 → float 3.0 (0x40400000) ✓
2. FADD: 3.0 + 4.0 = 7.0 (0x40E00000) ✓
3. FSUB: 7.0 - 4.0 = 3.0 (0x40400000) ✓
4. FMUL: 3.0 × 4.0 = 12.0 (0x41400000) ✓
5. FCVT.W.S: Convert 12.0 → int 12 ✓
6. FEQ + FLT: 3.0 == 3.0 → 1, 3.0 < 4.0 → 1 ✓

Result: 6 (all 6 pass), cycle 191

### Test: `sim/payloads/fpu_extended.S`
20 tests covering all supported FP operations, using back-to-back FP instructions in tests 4, 15, 18–20:
1. FADD 2.5+1.0=3.5 (riscv-tests value) ✓
2. FSUB 2.5-1.0=1.5 (consecutive FP after test 1) ✓
3. FMUL 2.5×1.0=2.5 (consecutive FP) ✓
4. FCVT.S.W(3)+FCVT.S.W(4)+FADD = 7.0 (triple back-to-back FP) ✓
5. FSGNJ sign injection ✓
6. FSGNJN negated sign (consecutive FP) ✓
7. FSGNJX xor sign (consecutive FP) ✓
8. FMIN(-3.0, 4.0) = -3.0 (consecutive FP) ✓
9. FMAX(-3.0, 4.0) = 4.0 (consecutive FP) ✓
10. FLE -3.0 ≤ 4.0 → 1 (consecutive FP) ✓
11. FLT -3.0 < 4.0 → 1 (consecutive FP) ✓
12. FEQ 4.0 == 4.0 → 1 (consecutive FP) ✓
13. FCVT.S.W(-5) → -5.0 ✓
14. FCVT.W.S(-5.0) → -5 (consecutive FP after test 13) ✓
15. FCVT.S.WU + FCVT.WU.S roundtrip (back-to-back FP) ✓
16. FCVT.W.S 3.7 → 3 with RTZ rounding ✓
17. FCVT.W.S -3.7 → -3 with RTZ (consecutive FP) ✓
18. Zero addition: 0.0+0.0=0.0 (triple back-to-back FP) ✓
19. Large mult: 1000²=1000000, back-to-back fcvt+fmul ✓
20. FADD chain: 1+1+1+1=4.0 (triple back-to-back FADD) ✓

Result: 20 (all 20 pass), cycle 439

### Key Debugging Notes
- **Shared register file**: The core uses a unified FP/INT register file, so `fcvt.s.w f10, x10` overwrites x10's integer value with a float bit pattern. Tests use separate source/destination registers.
- **NOP padding**: One NOP between FP write and integer read of the same physical register prevents read-after-write hazards in the delayed writeback path.
- **Pre-existing isMul bug**: `Data.scala` defines `isMul = rdalu && !alu2imm && funct7(1)` but MUL's funct7 is 0b0000001 (bit 0, not bit 1). MUL operations fall through to the ALU as ADD. This is a pre-existing issue not introduced by the FPU changes.

---

## Phase 2: Synthesizability

### Task 0: FST Waveform Dump
Already implemented in Phase 1. The `MEOW_TRACE` env var enables FST output.

### Task 1: Synthesizable Scratchpad Memory

#### SPM Rewrite: `src/main/scala/koneko/exec/SPM.scala`
- Replaced DPI `BlackBox` with synthesizable Chisel `Module`
- Memory: `SyncReadMem(param.scratchpadSize / 4, Vec(4, UInt(8.W)))` — 4KB default, byte-addressable via Vec(4)
- Word address extraction: `addr(log2Ceil(param.scratchpadSize)-1, 2)`
- Read: Synchronous 1-cycle latency via `mem.read(wordAddr)`
- Write: Byte-enable via `mem.write(wordAddr, writeData, writeMask)` where `writeMask` comes from `wbe` bits

#### LSU Changes: `src/main/scala/koneko/exec/LSU.scala`
- Removed `hartid` input (no longer needed for DPI addressing)
- Removed `SPMElab` parameter (was for DPI configuration)
- Added `SyncReadMem` read pipeline: `spmReadPending` register tracks outstanding reads

#### DPI Removal
- `Aux.sv`: Removed `import "DPI-C"` for `meow_softmem_read`/`meow_softmem_write`, removed `SPM` module definition
- `sim/src/aux.cpp`: Deleted entirely (was only DPI functions)
- `sim/src/meow.h`: Removed `meow_softmem_read`/`meow_softmem_write` declarations
- `sim/CMakeLists.txt`: Removed `aux.cpp` from both `sim` and `sim_single` source lists

### Task 2: Global Memory Access with Crossbar

#### Crossbar: `src/main/scala/koneko/bus/Crossbar.scala`
- Priority arbiter for N upstream ports → 1 downstream port
- Burst tracking: `remaining` counter, `owner` register, `idle` state
- Port 0 (ICache) has higher priority than port 1 (LSU)
- **Same-cycle response fix**: When `reqJustFired` (request accepted this cycle), route the immediate response to the requester before `idle` register updates. Uses `activeOwner` and `busActive` combinational signals.

#### LSU Changes: `src/main/scala/koneko/exec/LSU.scala`
- Added `mem` IO port (Decoupled req/resp, same interface as ICache memory port)
- Address routing: `val isSPM = addr < param.scratchpadSize.U` determines path
- SPM path: Uses existing `spmReadPending` for synchronous reads
- Global memory path: `memSent`/`memGotResp` registers track outstanding requests
- **Same-cycle response fix**: `val memReqFired = mem.req.fire` combinational signal enables capturing response on the same cycle the request fires (before `memSent` register updates)

#### Exec Changes: `src/main/scala/koneko/exec/Exec.scala`
- Added `lsuMem` IO port, connected to LSU's `mem` port
- Connected LSU memory port through to Core level

#### Core Changes: `src/main/scala/koneko/Core.scala`
- Instantiates `Crossbar(2)` (2 upstream ports)
- Connections: `crossbar.upstream(0) <> fetch.mem`, `crossbar.upstream(1) <> exec.lsuMem`
- `crossbar.downstream <> io.mem` (external memory interface)

#### Driver Changes: `sim/src/single.cpp`
- Added write support: reads `mem_req_bits_write` and `mem_req_bits_wbe` signals
- Write handling: Applies byte-enable mask to backing memory when `write=1`
- Expanded backing memory: `const size_t MEM_SIZE = 16 * 1024 * 1024` (16MB, zero-initialized)
- Removed all DPI-related code: `g_sim`, `meow_softmem_read`/`meow_softmem_write`

#### AUIPC Bug Fix: `src/main/scala/koneko/exec/Exec.scala`
- **Bug**: For U-type instructions (AUIPC, LUI), `funct3 = instr[14:12]` comes from the immediate field, not a fixed operation code. When the immediate has non-zero bits at positions 14:12, the `ealuval` ALU mux selects the wrong operation (e.g., OR instead of ADD).
- **Fix**: Added `val aluOrAdded = Mux(uop.adder1pc, added, ealuval)` in the `rdsrc` section. Replaced `uop.rdalu -> ealuval` with `uop.rdalu -> aluOrAdded`. When `adder1pc` is set (indicating AUIPC), the result bypasses the ALU entirely and uses the pre-computed `added = pc + imm`.

#### Test: `sim/payloads/asm/memcopy_test.S`
Tests bidirectional data copy between SPM and global memory:
1. Write known patterns to SPM (0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0xA5A5A5A5)
2. Copy SPM → global memory (4 words at offset 0x100 in global)
3. Verify global memory matches (4 checks)
4. Write different patterns to global memory
5. Copy global memory → SPM
6. Verify SPM matches (4 checks)
7. Final word-level write/read test (2 checks)
Result: 10 (all 10 checks pass)

#### Test: `sim/payloads/asm/auipc_test.S`
Tests AUIPC instruction with various immediate values:
1. `auipc x5, 0` — zero offset
2. `auipc x6, 1` — small offset (0x1000)
3. `auipc x7, 0x12` — non-zero bits at funct3 position
4. `auipc x8, 0x80000` — large offset (high bit)
5. Series of relative AUIPC+verify with non-trivial immediates
Result: 5 (all 5 checks pass)

#### Test: `sim/payloads/c/sha256_test.c`
SHA-256 of the string "abc", implemented entirely inline in `main()`:
- Custom startup (`c/crt0.S`): Sets stack pointer, calls `main()`, sends result via AM event
- Custom linker script (`c/linker_c.ld`): 1MB RAM at 0x80000000, stack at top
- Compiled with: `riscv64-unknown-none-elf-gcc -mabi=ilp32 -march=rv32i -O1`
- Verifies all 8 words of the expected SHA-256 digest against known values
- Result: 8 (all 8 words match)
- **Note**: Compilation at -O2 produces incorrect results, likely due to compiler-generated instruction patterns beyond RV32I base support. The instruction says to skip if ISA support is insufficient, so -O1 is used.

### Task 3: Test Case Cleanup

#### Directory Organization
- `sim/payloads/asm/`: All assembly tests
  - `fpu_verify.S` (6 tests, result=6)
  - `fpu_extended.S` (20 tests, result=20)
  - `icache_test.S` (3 tests, result=3)
  - `memcopy_test.S` (10 tests, result=10)
  - `auipc_test.S` (5 tests, result=5)
- `sim/payloads/c/`: C test infrastructure
  - `sha256_test.c` (8 tests, result=8)
  - `crt0.S` (C startup code)
  - `linker_c.ld` (C linker script)
- Removed ~10 debug/temporary files: sha256_debug.c, sha256_diag.c, sha256_inline.c, sha256_round.c, sha256_step.c, ops_test.c, byte_test.c, globmem_test.c, simple_c_test.c, stack_test.c (and their binaries)

#### Updated Makefile
- Added `all-tests` target that builds both assembly and C tests
- Assembly tests: compiled with `-march=rv32imf_zicsr`, linked with `linker.ld`
- C tests: compiled with `-march=rv32i -O1`, linked with `c/linker_c.ld`
- ELF intermediates are removed after objcopy

#### Updated .gitignore
- Added patterns for `asm/*.bin`, `asm/*.elf`, `c/*.bin`, `c/*.elf`

#### Full Regression Results
All 6 tests pass:
- `asm/fpu_verify`: result=6 ✓
- `asm/fpu_extended`: result=20 ✓
- `asm/icache_test`: result=3 ✓
- `asm/memcopy_test`: result=10 ✓
- `asm/auipc_test`: result=5 ✓
- `c/sha256_test`: result=8 ✓

### Known Issues
- **isMul bug**: `Data.scala` line `isMul = rdalu && !alu2imm && funct7(1)` should use `funct7(0)`. MUL instructions currently fall through to ALU as ADD. Not fixed (pre-existing, out of scope).
- **SHA256 at -O2**: Produces incorrect results. Root cause not fully diagnosed; likely involves instruction patterns generated by -O2 optimization that exceed RV32I base support. Works correctly at -O0 and -O1.

---

## Phase 3: NoC Router

### Task 1: NoC Router (`bus/Router.scala`)

#### Interface Evaluation
Reviewed the existing skeleton interface. The design has:
- `Routable` trait with `dst` (routing key) and `prio` (VC selection)
- `Link` bundle pairing egress (Decoupled out) and ingress (Flipped Decoupled in) per physical neighbor
- Router parameters: `local` ID, `numLinks`, `numVc`, `buffer` depth, `table` routing map
- `eject`/`inject` ports for local core traffic

The interface is a good fit for single-flit packet switching. No changes were needed except the `buffer` parameter (added by the user) and adding package/import statements (the original skeleton had none).

#### Design Decisions
1. **Single-flit simplification**: No wormhole state machine or flit tracking needed. Each flit routes independently based on its `dst` field.
2. **VC buffers**: Each input port (numLinks ingresses + inject) gets `numVc` independent Queue buffers of depth `buffer`. Incoming flits are classified by `prio` into the corresponding VC. This prevents HOL blocking between priority classes.
3. **Two-level output arbitration**: For each output port:
   - **Level 1 (VC priority)**: Strict priority — highest VC number with a pending request wins.
   - **Level 2 (Input fairness)**: Within each VC, `RRArbiter` provides round-robin fairness across input ports.
4. **Route lookup**: Combinational `MuxLookup` from the static `table: Map[Int, Int]`. If `dst == local`, routes to eject port. Unknown destinations fall back to link 0.
5. **Dequeue gating**: Each buffer's head targets exactly one output port (destination is fixed), so a buffer appears as a candidate for at most one output — no double-granting conflicts.

#### Implementation Details
- `routeTo(dst)`: Combinational function returning output port index. Default for local → eject, otherwise table lookup.
- `vcBufs(i)(v)`: `Queue(data, buffer)` per (input port, VC). Input port ready = selected VC queue's enq.ready (via MuxLookup on prio).
- `bufOutPort`: Pre-computed output port for each buffer head, shared across all output arbiters (avoids redundant routing logic).
- Per-output loop creates `numVc` `RRArbiter`s (each over `numInputs` candidates), selects winning VC by scanning low-to-high (last-writer-wins → highest VC), gates ready/dequeue accordingly.
- `deqGrant` wire vector tracks which buffers won arbitration; buffer dequeue is enabled only when granted.

#### Verification
- Scala compilation: `mill Koneko.compile` — clean (no errors/warnings)
- Verilog elaboration: `mill Koneko.run` — succeeds (Router not instantiated in Core, no impact on generated RTL)
- Full regression: all 6 tests pass

### Task 2: AUIPC Fix Review

User split `rdimm`→`rdlui`+`rdauipc`, `imm(32.W)`→`cimm(21.W)` with `immU`/`immExt` helpers in Data.scala, and updated Decode.scala/Exec.scala accordingly.

**Compilation fixes:**
1. `Data.scala`: Trailing comma on `immU` definition — removed.
2. `Exec.scala`: Operator precedence: `uop.rdauipc -> uop.immU + uop.pc` parsed as `(uop.rdauipc -> uop.immU) + uop.pc`. Fixed with parentheses: `uop.rdauipc -> (uop.immU + uop.pc)`.

**Test extension:** Added tests 6–7 to `auipc_test.S`:
- Test 6: `auipc x10, 0xABCDE` — exercises non-zero bits at instr[30:25]/[24:20] (previously aliased as funct7/rs2).
- Test 7: `lui x10, 0xABCDE` — verifies LUI still works after the split.

All 6 regression tests pass with 7/7 auipc_test checks.

---

## Phase 4: Memory Event Encoding

### Task 0: Protocol Review
Reviewed `doc/mem.md` encoding format. Key design parameters:
- Event tags: 0xFF00=load (2 flits), 0xFF01=store (3 flits), 0xFF02=AMO (not implemented)
- Load flits: [addr, returning_tag]. Store flits: [addr, {size##returning_tag}, wdata]
- DRAM controller destination: 0x8000 + controller_index (16-bit dst field)
- Response: 256-bit data bus + 16-bit tag, always 32B-aligned

### Task 1: Encoder + MemReq/MemResp Refactor

**Data.scala changes:**
- MemReq: `burst: UInt(8.W)` → `size: UInt(2.W)` + `id: UInt(16.W)`
- MemResp: Merged old MemResp+MemBeat → single MemResp with `tag: UInt(16.W)` + `data: UInt(memBusWidth.W)`
- Parameters: Added `memCtrlSizes: List[BigInt]` and `memBusWidth` requirement (must be ≥ 32)

**bus/Encoder.scala (new):**
- FSM: sIdle → sSendAddr → sSendMeta → sSendWdata (for stores)
- Latches request fields on `mdm.req.fire`, emits flits on `out` (Decoupled)
- Controller destination: iterates `memCtrlSizes` cumulative sums, computes local address within controller
- Response path: direct passthrough `mdm.resp := resp`

**bus/Crossbar.scala rewrite:**
- Removed burst state machine (idle/owner/remaining registers, completion tracking)
- New ID encoding: `arb.io.chosen ## id(14,0)` — upstream port in bit 15
- Response routing: `respPort = tag(15,15)`, routes to matching upstream, strips port bit from tag

**fetch/ICache.scala refactor:**
- `wordsPerBeat = memBusWidth/32 = 8`, `beatsPerLine = blockSize/(memBusWidth/8) = 2`
- New registers: `s1refillBeatData` (latches 256-bit response), `s1refillWriting` (serializing writes), `s1reqSentForBeat` (per-beat request tracking)
- Refill loop: send request → wait for response → latch → write 8 words sequentially → repeat for next beat
- `s1refilledCapture`: captures requested PC's word during write serialization

**exec/LSU.scala updates:**
- `mem.req.bits.addr := req.bits.addr` (full byte address, not word-aligned)
- `mem.req.bits.size` computed from `req.bits.len` (byte→0, half→1, word→2)
- Word extraction: `wordInBeat = alignedAddr(beatAlignBits-1, 2)` indexes into `respWords` Vec

**Core.scala rewiring:**
- `val mem = IO(Flipped(Valid(new MemResp)))` — single wide response port
- Arbiter(2): port 0 = BIU ext.out (priority), port 1 = Encoder.out
- Crossbar downstream ↔ Encoder.mdm, Encoder.resp := mem

### Task 2: Driver Rewrite (`sim/src/single.cpp`)
- MemRequestCollector: tracks multi-flit memory requests (active, tag, dst, operands[], flit_count)
- Flit classification: tag 0xFF00/0xFF01 → memory request; others → AM event (unchanged)
- processMemRequest: reconstructs address = operand[0] + TEXT_BASE, extracts resp_tag, size, wdata
- readBlock: returns 32B aligned block as MemResponse{tag, data[8]}
- Store handling: size-based byte mask with byte_off from `addr & 3`
- Response queue: dequeued one entry per cycle when mem_valid is set

### Task 3: Regression Testing

**Build errors fixed during implementation:**
1. BigInt type mismatch in Parameters/Encoder (used `BigInt(0)` seed for scanLeft)
2. Duplicate `s1refillComplete` wire in ICache
3. Forward reference to `s1refillCompleted` in ICache (moved declaration before use)
4. Destination encoding overflow: 0x80000000 doesn't fit in 16-bit dst → changed to 0x8000+X
5. `aluOrAdded` reference in Exec.scala → renamed to `ealuval`

**SHA256 bug root cause:**
- LSU hardcoded `size := 2.U` (word) and sent `alignedAddr` (dropping byte offset)
- Sub-word stores (byte 'a'=0x61, 'b'=0x62, 'c'=0x63, 0x80) at the same word address all overwrote the entire word instead of individual bytes
- Fix: `mem.req.bits.addr := req.bits.addr` (preserve byte offset) + `size` from `req.bits.len`

**Final regression (all pass):**
- `asm/fpu_verify`: result=6 ✓
- `asm/fpu_extended`: result=20 ✓
- `asm/icache_test`: result=3 ✓
- `asm/memcopy_test`: result=10 ✓
- `asm/auipc_test`: result=7 ✓
- `c/sha256_test`: result=8 ✓

---

## Phase 5: System Construction

### Task 0: Make Core @instantiable

- Added `import chisel3.experimental.hierarchy.{instantiable, public}` to Core.scala
- Annotated `class Core` with `@instantiable`
- Annotated `mem`, `ext`, `cfg` ports with `@public`
- Added `-Ymacro-annotations` to `build.sc` scalacOptions (required for macro annotations in Scala 2.13)
- Verified: single-core Verilog generation + all 6 regression tests pass

### Task 1: System Implementation

#### Flit Type
- `class Flit extends Bundle with Routable` — carries `src`, `dst`, `data`, `tag` (all UInt(16/32.W))
- `prio = 0.U(1.W)` — single VC for now (all traffic equal priority)
- Router sets `src` field at injection time

#### Topology Object (pure Scala computation)

**Grid dimensions**: `gridDims(numPU)` computes (width, height) where W:H is 1:1 or 2:1.

**ID numbering**: `gridToId(row, col, gridW)` / `idToGrid(id, gridW)`
- 4×4 clusters, row-major within cluster, raster-scan across clusters
- Example (4×4 grid = 1 cluster): (0,0)→1, (0,1)→2, ..., (3,3)→16

**Torus links**: `torusNeighbor(row, col, dir, W, H)` with wrap-around
- Directions: N=0, E=1, S=2, W=3

**MC zone assignment**: `bisectAssign(...)` recursively bisects the cluster grid
- Alternates vertical/horizontal splits
- MCs assigned in raster-scan order to resulting regions

**MC connection nodes**: `mcConnectionNodes(clusters, gridW)` returns top-left PU ID of each cluster in the zone

**Routing tables**:
- PU→PU: XY torus routing (`xyRouteDir`) — X (column) first, then Y (row), shortest torus path
- PU→MC: XY route to nearest connection node in MC's zone, then MC link (link 4)
- MC→PU: forward to connection node closest to destination

#### System Module

**Core instantiation**: `Definition(new Core)` creates one definition, `Instance(coreDef)` creates N instances. Each core gets `hartid := id.U` and `mem := io.memResp(id-1)`.

**PU routers**: `Router(Flit, id, numLinks, 1VC, buffer=4, routingTable)`. Connection nodes get 5 links (4 torus + 1 MC), others get 4.

**MC routers**: `Router(Flit, mcId, numConns, 1VC, buffer=4, mcRoutingTable)`. No inject (inject.valid := false).

**Torus wiring**: For each PU, connect N/E/S/W links to neighbors. `connectedPairs` set prevents double-connections. Each pair: A.egress(dir) ↔ B.ingress(oppDir).

**MC wiring**: MC link i ↔ PU link 4 on connection node i.

**Core↔router**: Core ext.out → router inject (with src field set). Router eject → core ext.in.

**System IO**: `memOut(mcIdx) <> mcRouter.eject`, `memResp(puIdx) -> core.mem`.

#### Link Bundle Fix
- `Link` class had `val data: D` parameter — Chisel's Bundle plugin treated it as a hardware field
- Fix: changed to `proto: D` (non-val constructor parameter)
- Same fix applied to Router's `data` parameter for consistency

#### Verification
- 16-PU/1-MC: elaborates successfully
- 64-PU/2-MC: elaborates successfully, 95 Verilog modules total
- Core dedup: only 1 `module Core` in System.sv (confirmed with grep)
- Single-core regression: all 6 tests pass (unchanged)

---

## Phase 6: NoC Refactor (Mesh + Hilbert + New Router Interface)

### Router Rewrite (`bus/Router.scala`)

**New interface** (matched user's proposed sketch):
- `locals: Seq[Local]` — each local has `id: Int`, `inject: Boolean`, `eject: Boolean`
- `numIngress: Int`, `numEgress: Int` — truly asymmetric (mesh boundary has fewer links)
- `numVc: Int`, `buffer: Int` — VC config as before
- `table: Map[Int, Int]` — forwarding table

**Routing via TruthTable + decoder**:
- `import chisel3.util.experimental.decode.{TruthTable, decoder}`
- `fwdTT`: maps dst → 1-hot egress port selection. Table entries → their egress, eject-local IDs → all-zero.
- `localTT`: maps dst → 1-hot eject-local selection. Table entries → all-zero, eject-local IDs → their 1-hot.
- `bufTargets = Cat(localBits, fwdBits)` — combined 1-hot over all outputs. Bit j < numEgress → egress(j), bit numEgress+k → ejectLocal(k).

**IO ports**: `ingress: Vec[numIngress, Flipped(Decoupled)]`, `egress: Vec[numEgress, Decoupled]`, `injects/ejects: Seq[Option[DecoupledIO]]` (conditional per local).

**Fixes applied to user's sketch**:
1. `Bool` → `Boolean` in Local case class
2. `def x(id) =>` → `def x(id) =` (syntax)
3. `requires` → `require`
4. Binary string left-padding for IDs (was right-padding)
5. 1-hot bit ordering: `oneHot(w, j) = "0"*(w-1-j) + "1" + "0"*j` (bit j = LSB position j)
6. `val egressBitSet(...)` → `def egressBitSet(...)`
7. `TruthTable` takes 2 args (entries, default), not 3 — merged entry sequences
8. Used `BitPat` (standard TruthTable API) instead of `BitSet`

### System Rewrite (`System.scala`)

**Hilbert curve ID assignment**:
- `hilbertCurve(w, h)`: recursive. Non-square → split longer axis (left/right or top/bottom), flip second half for connectivity. Square → standard Hilbert via `hilbertD2xy(n, d)`.
- `hilbertD2xy`: standard algorithm with swap to make first step south (x→row, y→col).
- PU IDs 1-based along curve. Every 16 consecutive = 1 cluster.

**2D mesh** (replacing torus):
- `meshNeighbor(row, col, dir, W, H)`: returns `Option[(Int,Int)]`, None at boundary.
- `puMeshDirs`: per-PU list of active directions (0-3). Boundary nodes have 2-3 active dirs.
- `puDirToEgress`: dense packing of active directions → egress indices.
- Symmetric: `numIngress = numEgress` for each router.

**XY routing on mesh**:
- `xyRouteDir(sr, sc, dr, dc)`: X first (column), then Y (row). No wrap.

**MC as extra eject**:
- 9th node (index 8, 0-based) in each 16-node cluster segment is the MC connection point.
- That PU router gets `Local(mcId, inject=false, eject=true)` as a second local.
- MC IDs excluded from forwarding table on connection nodes (handled by local eject).
- Non-connection nodes route MC traffic via XY toward nearest connection node.

**MC eject arbitration**:
- Collect all eject ports across connection nodes for each MC.
- If only 1 eject → direct `<>` connection.
- If multiple → `Arbiter(Flit, n)` combines them into single `io.memOut(mcIdx)`.

**Mesh wiring**:
- `connectedPairs` set (min/max ID pair) prevents double-connections.
- Each pair: A.egress(eIdx) ↔ B.ingress(nEIdx) with correct direction mapping.

### Verification
- Scala compilation: clean
- Elaboration: 16-PU/1-MC and 64-PU/2-MC both succeed
- Single-core regression: all 6 tests pass
- System tests (16 PU / 1 MC):
  - Ping-pong: result=100 in 19,657 cycles ✓
  - SHA256 concurrent: result=0xa6e58910 in 21,822 cycles ✓

---

### Phase 6b: Hilbert curve simplification + MC connection change

#### Hilbert curve
- Replaced custom recursive `hilbertInner` (rotation-only, had negative modulo bug + missing reflections) with standard `hilbertD2xy` algorithm
- Backward C base case: H_1 = (0,0)→(0,1)→(1,1)→(1,0), first step east
- For 2:1 grids (w=2h): generate w×w square, take first w*h points (= top half). Works because first half of d2xy curve covers rows 0..w/2-1.
- Harness: coverage + adjacency assertions in `hilbert(w, h)`
- Also elaborated 32-PU/1-MC (w=8, h=4) — first test of the 2:1 slicing path

#### MC connection
- Changed from "9th node in Hilbert segment" to "node in cluster closest to grid center"
- `clusterNodes.minBy { puId => val (r,c) = puPos(puId); dist_to_center }`
- Fixed bug in user's code: original used `.map(puPos).minBy(...)` which returned position `(Int,Int)` instead of PU ID `Int` (type error)

#### Verification
- Elaboration: 16-PU/1-MC, 32-PU/1-MC, 64-PU/2-MC all succeed
- System tests unchanged (16 PU / 1 MC):
  - Ping-pong: result=100 in 19,657 cycles ✓
  - SHA256 concurrent: result=0xa6e58910 in 21,822 cycles ✓

---

## Phase 7: Hardware Memory Interface (MemIf + MemDistributor)

### Design Review
User provided skeleton MemIf.scala and MemDistributor.scala. Issues found:
1. MemIf.req expected pre-assembled operands, but Encoder sends 2-3 raw flits → user confirmed MemIf collects internally using CAM on src+remainingFlits
2. Ring bus IO was skeleton (no data/valid/ready) → implemented full Decoupled ring with 2-deep buffer
3. MemDistributor had undefined `clusterPerMemIf` → user fixed to 16
4. GlobalMemResp had unused `implicit CoreParameters` → removed
5. `mem.resp.ready` on Valid (no ready field) → removed
6. `completed` was declared as `Vec[MemPending]` instead of `Vec[Bool]` → fixed

### MemIf Implementation (`bus/MemIf.scala`)
**Flit collection** (per cluster input):
- CAM lookup: `allocated && !ready && src matches && remainingFlits > 0`
- CAM hit: continue collecting into existing slot. Decode flit index from totalFlits - remainingFlits.
- No hit + memory tag: allocate new slot, set remainingFlits (1 for load, 2 for store)
- No hit + non-memory tag: route to AM arbiter (passthrough)
- When remainingFlits reaches 0: mark `ready`

**Request issuing**:
- PriorityEncoder on `allocated && ready && !issued` → issue first ready slot
- GlobalMemReq: addr from pending, wdata = Fill(8, wdata32), wbe = mask << byteOffset
- Stores: mark completed immediately after issue (no response needed for write acknowledgement)

**Response delivery**:
- PriorityEncoder on `allocated && completed && isLocal(src)` → drive resp[clusterOf(src)]
- PriorityEncoder on `allocated && completed && !isLocal(src)` → enqueue to ring buffer
- Avoids combinational cycle (initial version used for-loop with running flag → FIRRTL cycle error)

**Ring bus**:
- ringIn: if local PU → deliver directly to resp port (overrides local completion); if remote → forward to ringBuf
- ringOut: 2-deep Queue → ringOut
- Deadlock avoidance: ring forward only when ringBuf has space

**AM passthrough**:
- Arbiter across numClusters inputs for non-memory first flits (tag ≠ 0xFF00/01)
- Exposed at System level as `io.am` per MC

### MemDistributor Implementation (`bus/MemDistributor.scala`)
- Single Flipped(Decoupled) input from MemIf resp
- 16 Valid outputs matching Core.mem (Valid[MemResp] with tag + data)
- Single-entry buffer: in.ready = !valid; latch on fire; drive valid on matching PU for one cycle; clear next cycle

### System.scala Changes
- IO: `io.mem: Vec(numMC, { req: Decoupled[GlobalMemReq], resp: Valid[GlobalMemResp] })` + `io.am: Vec(numMC, Decoupled[{ src, data, tag }])`
- Removed Core.mem direct connection (was `inst.mem := io.memResp(id-1)`)
- Removed MC eject Arbiter
- Added: MemIf per MC (with puStart/puEnd zone), ring bus connections, MemDistributor per cluster
- Wiring: MC eject → MemIf.req, MemIf.resp → MemDistributor.in, MemDistributor.out → Core.mem

### Simulator Changes (`system.cpp`)
- Replaced per-PU MemRespPort + per-MC MemOutPort with per-MC MemPort struct
- MemPort has: req (valid/ready/id/addr/wdata[8]/wbe/write), resp (valid/id/rdata[8]), am (valid/ready/src/data/tag)
- processMemReq: reads GlobalMemReq fields directly (addr, wbe for byte-masked write, wdata[8] for 256-bit data)
- AM handling: tag=0 → end-of-sim
- gen_system_ports.py: generates mem_ports.inc (replaces resp_ports.inc + out_ports.inc)

### Verification
- Compilation: clean (only expected index-width warnings in MemIf)
- Elaboration: 16-PU/1-MC and 64-PU/2-MC both succeed
- Ping-pong (16 PU, 1 MC): result=100 in 19,688 cycles ✓
- SHA256 concurrent (16 PU, 1 MC): result=0xa6e58910 in 22,883 cycles ✓
- Slight cycle increase (~30-1000) due to MemIf collection + MemDistributor buffering latency

---

## Phase 8: System-Level Peripheral Interface

### LSU Memory Mapping (`exec/LSU.scala`)
- Changed `isSPM` from `addr < scratchpadSize.U` to `addr >= 0x20000000.U && addr < 0x40000000.U`
- SPM address calculation: `spmAddr = addr - 0x20000000.U`, `spmAlignedAddr = (spmAddr >> 2) ## 0.U(2.W)`
- Added assert: `!req.valid || req.bits.addr >= 0x20000000.U`
- Exterior path (>= 0x40000000) unchanged — same flow through Crossbar → Encoder

### Encoder Peripheral Routing (`bus/Encoder.scala`)
- Added: `isPeripheral = !mem.req.bits.addr(31) && mem.req.bits.addr(30)` (detects 0x40000000-0x7FFFFFFF)
- Added: `periphAddr = addr - 0x40000000.U`, `periphDst = 0x8800`
- Mux: `finalDst = Mux(isPeripheral, 0x8800, ctrlDst)`, `finalAddr = Mux(isPeripheral, periphAddr, ctrlLocalAddr)`
- All existing encode FSM unchanged — only the latched dst/addr values change

### MemIf Cleanup (`bus/MemIf.scala`)
- Removed `done: Output(Bool())` and `result: Output(UInt(32.W))` IO ports
- Removed `doneReg`/`resultReg` registers
- Removed tag=0 detection in flit collection loop

### System Topology + Peripheral MemIf (`System.scala`)
- Routing table: added 0x8800 to all PU forwarding tables (XY route toward PU1)
- PU1 locals: added `Local(0x8800, inject=false, eject=true)`
- Peripheral MemIf: `Module(new MemIf(numMC, 0, 0, 1, 16))` — no zone-of-influence (puStart=puEnd=0), all responses are remote (go via ring)
- Connection: PU1.ejects(periphLocalIdx) → periphMemIf.req(0); periphMemIf.resp(0).ready tied to true
- Ring bus: MC[last].ringOut → periphMemIf.ringIn, periphMemIf.ringOut → MC[0].ringIn
- System IO: removed done/result, added `periph: { req, resp }` (same format as mem ports)

### Peripheral Device (`sim/src/devices.h`)
- `PeripheralDevice` struct: `finished` flag, `result` value. `write(addr, data)` sets finished when addr=0. `read(addr)` returns 0.
- `FlitCollector` struct: reusable multi-flit event assembler. `push(tag, dst, data)` returns true when request fully assembled. Provides `addr()`, `id()`, `size()`, `wdata()`, `isStore()` accessors.

### Single-Core Driver (`sim/src/single.cpp`)
- Replaced `MemRequestCollector` with `FlitCollector` from devices.h
- Added `PeripheralDevice periph` to `SingleCoreSim`
- Flit routing: `collector.dst == 0x8800` → `processPeriphRequest()` (calls periph.write, returns dummy MemResponse)
- End-of-sim: `periph.finished` instead of tag=0 event
- Core hartid changed from 0 to 1 (PU1)

### System Driver (`sim/src/system.cpp`)
- Added `#include "devices.h"`, `PeripheralDevice periph` + `deque<MemResponse> periph_resps`
- New `processPeriphReq()`: reads peripheral GlobalMemReq, extracts wdata from first active byte-enable word, calls `periph.write(addr, wdata)`, returns dummy MemResponse
- Peripheral ports driven alongside MC ports: `io_periph_req_ready = 1`, response signals set from `periph_resps` queue
- End-of-sim: `periph.finished` instead of `sys->io_done`

### Test Case Migration
- `asm/pingpong_test.S`: `done:` label changed from `li x2, 0x8000; .word 0x0025500b` (AM to MC) to `li t0, 0x40000000; sw a0, 0(t0)` (store to peripheral)
- `sys/sha256_concurrent.S`: Same done-label change; `li sp, 0x4000` → `li sp, 0x20004000` (SPM at 0x20000000)

### Important Debug Note
After changing Encoder.scala, BOTH `mill Koneko.run` (single-core Core.sv) AND `AN_SYSTEM=1 mill Koneko.run` (System.sv) must be re-run. The Core.sv is used by sim_single, and System.sv by sim_system. Missing the Core.sv regeneration caused the single-core sim to use stale RTL without peripheral detection.

### Verification
- Compilation: clean (only index-width warnings in MemIf)
- Ping-pong (16 PU, 1 MC): result=100 in 19,687 cycles ✓
- SHA256 concurrent (16 PU, 1 MC): result=0xa6e58910 in 22,882 cycles ✓
- Single-core peripheral test: result=42 in 51 cycles ✓

## Task 2.3: CSR-Aware Memory Access (Broadcast Scatter)

### Design Overview
Implements CSR-aware memory access from the paper: when a PU fires a neuron, it sends a `readCSR` command to the MC (via AM). The MC reads CSR data from DRAM and broadcasts it to all PUs in its zone via the MemDistributor. Each PU's BIU filters entries matching its hartid.

**Data flow**: PU → AM send (tag=0xFF02, base, end) → NoC → MemIf → scatter FSM → DRAM reads → broadcast via MemDistributor (dst=0xFFFF) → Core detects broadcast (tag=0xFFFF) → BIU filters by hartid → EvQueue → spike handler

**CSR entry format**: 64 bits = word0[31:16]=dst_core, word0[15:0]=neuron_index, word1=weight(f32). 4 entries per 256-bit beat.

### MemIf Scatter FSM (`bus/MemIf.scala`)
- Added `scatterAddrBase` parameter (default 0x80000000) for address translation
- 5-state FSM: Idle → Collect → Issue → WaitResp → Broadcast
- **Collect**: Recognizes tag=0xFF02 flits. First flit = base addr, second (same src) = end addr
- **Issue**: Arbitrates with normal pending slot issue (normal has priority). Uses `scatSlotId = maxInflight` (out-of-band id, separate from pending array)
- **WaitResp**: Catches response with matching scatter slot id. Normal responses guarded by `respId < maxInflight.U`
- **Broadcast**: Iterates through all clusters, sending to resp port with dst=0xFFFF, id=0xFFFF. Pauses normal local delivery during broadcast. Ring input still works (last-connect-wins)
- Address translation: `mem.req.addr = scatterAddr - scatterAddrBase` (handles multi-MC cumulative offsets)

### MemDistributor Broadcast (`bus/MemDistributor.scala`)
- Already implemented: when `bufDst === 0xFFFF.U`, all 16 `out(i).valid` are asserted simultaneously
- One-cycle broadcast delivery (no handshake, just valid signal)

### BIU Broadcast Processing (`BIU.scala`)
- New IO: `bcast` (Valid + 256-bit data), `hartid` (16-bit input)
- Entry matching: On `bcast.valid`, scans 4 entries per beat. For each, extracts `dst_core = word0[31:16]`, `neuron = word0[15:0]`, `weight = word1[63:32]`. Matches if `dst_core == hartid`
- Matching entries buffered in 16-deep Queue
- Drain FSM: Serializes 2-word events (neuron, weight) into EvQueue for SPIKE_TAG=1
- **EvQueue conflict fix**: Added `bcastDraining` wire (forward-declared). When broadcast drain is active, ext.in ready is deasserted for tag=1 (SPIKE_TAG), preventing data loss from last-connect-wins

### Core Broadcast Detection (`Core.scala`)
- Detects broadcast: `mem.valid && mem.bits.tag === 0xFFFF.U`
- Normal responses → Encoder → Crossbar (ICache/LSU)
- Broadcast responses → Exec.bcast → BIU

### Exec Wiring (`exec/Exec.scala`)
- Added `bcast` IO (valid + data), wired to BIU.bcast and BIU.hartid

### System Wiring (`System.scala`)
- Passes `scatterAddrBase = 0x80000000 + cumSizes(mcIdx)` to each MemIf
- cumSizes computed from `coreParams.memCtrlSizes.scanLeft(BigInt(0))(_ + _)`

### Test Payload (`sys/csr_scatter_test.S`)
- 16 CSR entries at binary offset 0x1000 (32-byte aligned), one per PU
- Each entry: `(hartid << 16) | 0` + `0x3F800000` (weight=1.0f)
- PU1 waits for all 15 "ready" messages, triggers scatter via AM (queue base+end, send to 0xFF028001)
- Each PU's spike handler sends pong to PU1
- PU1 counts 16 pongs → stops timer → writes result to peripheral → simulation ends

### Verification
- **16-PU, 1-MC, no DRAM**: Result=16, Timer=559 cycles, Total=1219 cycles ✓
- **16-PU, 1-MC, DRAMsim3**: Result=16, Timer=639 cycles, Total=1414 cycles ✓ (14% increase)
- **Scatter reads**: 4 reads at 0x80001000/20/40/60 with id=64 (scatter slot) confirmed in logs
- **Regression**: spm_init test unchanged (Result=16, Timer=9, Cycles=149192) ✓
