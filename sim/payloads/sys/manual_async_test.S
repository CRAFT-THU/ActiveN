  # Manual scalar line-load regression: 64 PUs, 2 MCs, 64-byte lines.
  # Build this source twice, with MC_INDEX=0 and MC_INDEX=1, and pass the
  # resulting manual_async_test.bin / manual_async_dram1.bin as MC0 / MC1.
  # Each PU sends four tag-0x400 / ID-0x8002 requests to the opposite MC.
  # Sources differ by MC, PU, line, and word; destinations are nonadjacent
  # SPM line indices relative to a nondefault AC SPM base.
  #
  # Thread 0 initializes SPM and issues all four requests without waiting.
  # Thread 1 handles only completions: its first work is to read and sum the
  # completed line, before inspecting metadata or waiting for other loads.
  # Its four U32 sums stay in s2-s5 until all four completions are seen.
  # Only then are they compared with independently calculated expectations.
  # PU1's thread 0 collects exactly one verified report per PU using a bitmap.
  # No polling, retries, or rereading completed lines can hide an early event.
  #
  # From the repository root, inside the Nix development shell:
  #   make -C sim/payloads sys/manual_async_test.bin sys/manual_async_dram1.bin
  #   sim/build/sim_system --hard --soft --pu 64 --mc 2 \
  #     --mc-size 0x100000000 --max-cycles 100000 \
  #     sim/payloads/sys/manual_async_test.bin sim/payloads/sys/manual_async_dram1.bin
  # Expected: auxiliary result 64 followed by exit code 0.

#ifndef MC_INDEX
#define MC_INDEX 0
#endif

#define AC_SPM_BASE    0x20000400
#define POISON         0xdeadbeef
#define DATA_SEED      0x9e3779b9
#define MC_DELTA       0x01020304
#define PU_DELTA       0x00010001
#define SLOT_DELTA     0x01010101
#define WORD_DELTA     0x00010203
#define COOKIE        0xcafe0000
#define REPORT_COOKIE 0x600d0004
#define STOP_ADDR     0x40000000
#define RESULT_ADDR   0x4000000c

  .macro SEND dst, tag
  .insn r 0x0b, 0x4, 0, x0, \dst, \tag
  .endm

  .macro LOAD_LINE slot, line
  addi  a0, s4, (\slot * 64)  # MC-local source, not CPU global address
  li    a1, (6 << 16) | 0x8002
  li    a2, (1 << 16) | \line # handler 1 and AC-relative SPM line index
  ori   a3, s5, \slot         # full 32-bit completion cookie
  SEND  s6, s7
  .endm

  .macro CHECK_GUARDS line
  lw    t0, ((\line * 64) - 4)(s10)
  bne   t0, t1, bad_guard
  lw    t0, ((\line + 1) * 64)(s10)
  bne   t0, t1, bad_guard
  .endm

  .section .text
  .globl _start
_start:
  bnez  a0, completion_init
  csrr  s0, 0xf14

  # Reject an incompatible elaboration before issuing manual accesses.
  li    t0, 0x68000000
  lw    t1, 0(t0)
  li    t2, 64
  bne   t1, t2, bad_config
  lw    t1, 8(t0)
  li    t2, 2
  bne   t1, t2, bad_config
  lw    t1, 24(t0)
  li    t2, 6
  bne   t1, t2, bad_config

  # Poison destination lines and their neighbors before any request is sent.
  li    t0, AC_SPM_BASE
  csrw  0x732, t0
  li    t1, AC_SPM_BASE + 28 * 64
  li    t2, POISON
poison_loop:
  sw    t2, 0(t0)
  addi  t0, t0, 4
  bltu  t0, t1, poison_loop

  # Only thread 0 consumes reports, so its register bitmap needs no atomics.
  li    s1, 0                 # reports from PUs 1..32
  li    s2, 0                 # reports from PUs 33..64
  li    s3, 0                 # report count
  la    t0, report_handler
  csrw  0x700, t0
  csrw  0x720, zero
  li    t0, (1 << 3) | 2
  csrw  0x710, t0

  # PU1..32 -> MC1 (0x8002); PU33..64 -> MC0 (0x8001).
  addi  t0, s0, -1
  srli  t1, t0, 5
  li    s6, 0x8002
  sub   s6, s6, t1
  slli  t0, t0, 8             # four lines per PU
  la    s4, source_lines
  li    t1, 0x80000000
  sub   s4, s4, t1
  add   s4, s4, t0
  slli  s5, s0, 8
  li    t0, COOKIE
  or    s5, s5, t0
  li    s7, 0x400             # manual request, lower priority than fetch
  LOAD_LINE 0, 3
  LOAD_LINE 1, 9
  LOAD_LINE 2, 17
  LOAD_LINE 3, 26
idle:
  wfi

completion_init:
  csrr  s0, 0xf14
  li    s1, 0                 # completion bitmap
  li    s2, 0                 # result for slot 0
  li    s3, 0                 # result for slot 1
  li    s4, 0                 # result for slot 2
  li    s5, 0                 # result for slot 3
  li    s10, AC_SPM_BASE

  # Expected sum = 16*(seed + opposite_MC*MC_DELTA + PU*PU_DELTA
  #                    + slot*SLOT_DELTA) + (0+...+15)*WORD_DELTA mod 2^32.
  # This uses arithmetic, independently of the DRAM contents and cookie.
  slli  t0, s0, 16
  add   t0, t0, s0            # PU * 0x10001
  li    s6, DATA_SEED
  add   s6, s6, t0
  li    t0, 32
  bltu  t0, s0, expected_mc0
  li    t0, MC_DELTA
  add   s6, s6, t0
expected_mc0:
  slli  s6, s6, 4
  li    t0, 120 * WORD_DELTA
  add   s6, s6, t0
  li    t0, 16 * SLOT_DELTA
  add   s7, s6, t0
  add   s8, s7, t0
  add   s9, s8, t0

  # This thread owns initialization of its handler: it cannot be dispatched
  # until all persistent registers and the handler address are ready.
  la    t0, completion_handler
  csrw  0x701, t0
  li    t0, (1 << 16) | 1     # at most one report per handler invocation
  csrw  0x721, t0
  li    t0, (2 << 3) | 2      # thread 1 only, a0=ident, a1=carried[1]
  csrw  0x711, t0
  j     idle

completion_handler:
  # Read immediately on dispatch; never wait for the other three loads.
  slli  t0, a0, 6
  add   t0, t0, s10
  lw    t2, 0(t0)
  .set word_offset, 4
  .rept 15
  lw    t1, word_offset(t0)
  add   t2, t2, t1
  .set word_offset, word_offset + 4
  .endr

  # Validate the full carried word and a one-to-one slot/destination mapping.
  andi  t3, a1, 3
  slli  t0, s0, 8
  li    t1, COOKIE
  or    t0, t0, t1
  or    t0, t0, t3
  bne   a1, t0, bad_metadata
  li    t4, 1
  sll   t4, t4, t3
  and   t0, s1, t4
  bnez  t0, bad_duplicate
  beqz  t3, completed_0
  li    t0, 1
  beq   t3, t0, completed_1
  li    t0, 2
  beq   t3, t0, completed_2
  li    t0, 26
  bne   a0, t0, bad_metadata
  mv    s5, t2
  j     record_completion
completed_0:
  li    t0, 3
  bne   a0, t0, bad_metadata
  mv    s2, t2
  j     record_completion
completed_1:
  li    t0, 9
  bne   a0, t0, bad_metadata
  mv    s3, t2
  j     record_completion
completed_2:
  li    t0, 17
  bne   a0, t0, bad_metadata
  mv    s4, t2
record_completion:
  or    s1, s1, t4
  li    t0, 15
  bne   s1, t0, idle

  bne   s2, s6, bad_sum
  bne   s3, s7, bad_sum
  bne   s4, s8, bad_sum
  bne   s5, s9, bad_sum
  li    t1, POISON
  CHECK_GUARDS 3
  CHECK_GUARDS 9
  CHECK_GUARDS 17
  CHECK_GUARDS 26

  mv    a0, s0
  li    a1, REPORT_COOKIE
  li    t0, 1
  li    t1, 0                  # PU1 report, handler 0, highest priority
  SEND  t0, t1
  j     idle

report_handler:
  li    t0, 1
  bne   s0, t0, bad_report
  li    t0, REPORT_COOKIE
  bne   a1, t0, bad_report
  addi  t0, a0, -1
  li    t1, 64
  bgeu  t0, t1, bad_report
  li    t1, 1
  sll   t1, t1, t0             # RV32 shift takes the low five bits
  li    t2, 32
  bgeu  t0, t2, report_high
  and   t2, s1, t1
  bnez  t2, bad_report
  or    s1, s1, t1
  j     report_count
report_high:
  and   t2, s2, t1
  bnez  t2, bad_report
  or    s2, s2, t1
report_count:
  addi  s3, s3, 1
  li    t0, 64
  bne   s3, t0, idle
  li    t0, -1
  bne   s1, t0, bad_report
  bne   s2, t0, bad_report
  li    t0, RESULT_ADDR
  sw    s3, 0(t0)              # auxiliary result must be 64
  li    t0, STOP_ADDR
  sw    zero, 0(t0)
  j     idle

bad_config:
  li    t6, 1
  j     fail
bad_metadata:
  li    t6, 2
  j     fail
bad_duplicate:
  li    t6, 3
  j     fail
bad_sum:
  li    t6, 4
  j     fail
bad_guard:
  li    t6, 5
  j     fail
bad_report:
  li    t6, 6
fail:
  li    t0, RESULT_ADDR
  sw    s0, 0(t0)              # failing PU
  sw    a0, 0(t0)              # completion ident / report PU
  sw    a1, 0(t0)              # carried cookie
  sw    t2, 0(t0)              # most recent sum
  li    t0, STOP_ADDR
  sw    t6, 0(t0)
  j     idle

  .section .data
  .balign 64
source_lines:
  .set source_pu, 1
  .rept 64
    .set source_slot, 0
    .rept 4
      .set source_word, 0
      .rept 16
        .word (DATA_SEED + MC_INDEX * MC_DELTA + source_pu * PU_DELTA + source_slot * SLOT_DELTA + source_word * WORD_DELTA) & 0xffffffff
        .set source_word, source_word + 1
      .endr
      .set source_slot, source_slot + 1
    .endr
    .set source_pu, source_pu + 1
  .endr
