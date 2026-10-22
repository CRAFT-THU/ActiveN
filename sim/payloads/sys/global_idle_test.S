  # Peripheral global-idle regression: 64 PUs, 2 MCs, 64-byte lines.
  # Build with MC_INDEX=0 / 1 for global_idle_test.bin / global_idle_dram1.bin.
  # All PUs report readiness. PU1 arms the peripheral notifier while PU32 and
  # PU64 work, then checks their completion bitmap immediately on notification.
  # Epoch 2 rearms the notifier and loads a line from each worker's opposite MC.
  # Each completion handler verifies the returned line before doing more work.
  # Expected auxiliary outputs: 1, 2, 0x600d0002; simulation result: 0.
  # Result 3 specifically means a notification arrived before remote completion.

#ifndef MC_INDEX
#define MC_INDEX 0
#endif

#define RESULT_ADDR 0x4000000c
#define STOP_ADDR   0x40000000
#define IDLE_GATE   0x100
#define TAG_IDLE    0x400
#define TAG_REPORT  0x402
#define TAG_READY   0x803
#define TAG_WORK    0xc04
#define AC_BASE     0x20000400
#define AC_LINE     3
#define DATA_SEED   0x13570000
#define COOKIE      0x1d1e0000

  .macro SEND dst, tag
  .insn r 0x0b, 0x4, 0, x0, \dst, \tag
  .endm

  .macro INSTALL number, address, argc, quota, margin
  la    t0, \address
  csrw  (0x700 + \number), t0
  li    t0, (\quota << 16) | \margin
  csrw  (0x720 + \number), t0
  li    t0, (1 << 3) | \argc   # thread 0 owns all test state
  csrw  (0x710 + \number), t0
  .endm

  .section .text
  .globl _start
_start:
  bnez  a0, idle
  csrr  s0, 0xf14
  li    s1, 0                 # readiness bitmap, PUs 1..32
  li    s2, 0                 # readiness bitmap, PUs 33..64
  li    s3, 0                 # readiness count
  li    s4, 0                 # current epoch
  li    s5, 0                 # remote completion bitmap
  li    s6, 0                 # notifications received
  li    s8, 0                 # worker's last dispatched epoch
  li    s9, 0                 # worker's manual completion count

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

  li    t0, AC_BASE
  csrw  0x732, t0
  li    t1, 0xdeadbeef
  .set offset, AC_LINE * 64
  .rept 16
  sw    t1, offset(t0)
  .set offset, offset + 4
  .endr

  INSTALL 0, idle_handler, 1, 3, 3
  INSTALL 1, load_handler, 2, 1, 4
  INSTALL 2, report_handler, 2, 0, 0
  INSTALL 3, ready_handler, 1, 3, 7
  INSTALL 4, work_handler, 1, 1, 5

  mv    a0, s0
  li    t0, 1
  li    t1, TAG_READY
  SEND  t0, t1
idle:
  wfi
  j     idle

ready_handler:
  li    t0, 1
  bne   s0, t0, bad_protocol
  addi  t0, a0, -1
  li    t1, 64
  bgeu  t0, t1, bad_protocol
  li    t1, 1
  sll   t1, t1, t0
  li    t2, 32
  bgeu  t0, t2, ready_high
  and   t2, s1, t1
  bnez  t2, bad_protocol
  or    s1, s1, t1
  j     ready_count
ready_high:
  and   t2, s2, t1
  bnez  t2, bad_protocol
  or    s2, s2, t1
ready_count:
  addi  s3, s3, 1
  li    t0, 64
  bne   s3, t0, idle
  li    t0, -1
  bne   s1, t0, bad_protocol
  bne   s2, t0, bad_protocol
  li    s4, 1

start_epoch:
  li    s5, 0
  mv    a0, s4
  li    t0, 32
  li    t1, TAG_WORK
  SEND  t0, t1
  li    t0, 64
  SEND  t0, t1
  li    a0, (IDLE_GATE << 16) | TAG_IDLE
  li    t0, 0x8000
  li    t1, 0xcf0
  SEND  t0, t1
  j     idle

work_handler:
  li    t0, 32
  beq   s0, t0, worker_valid
  li    t0, 64
  bne   s0, t0, bad_protocol
worker_valid:
  addi  t0, s8, 1
  bne   a0, t0, bad_protocol
  li    t0, 2
  bgtu  a0, t0, bad_protocol
  mv    s4, a0
  mv    s8, a0
  li    t0, 1
  beq   s4, t0, busy_work

  # One manual ID-2 load, with a dedicated handler and no other outstanding
  # manual notifications. The issuer immediately idles to receive completion.
  la    a0, source_line
  li    t0, 0x80000000
  sub   a0, a0, t0
  li    a1, (6 << 16) | 0x8002
  li    a2, (1 << 16) | AC_LINE
  li    a3, COOKIE | 2
  li    t0, 0x8001
  li    t1, 64
  beq   s0, t1, load_issue
  li    t0, 0x8002
load_issue:
  li    t1, 0x400
  SEND  t0, t1
  j     idle

load_handler:
  # Validate all words immediately, without polling or retrying a stale line.
  li    t0, AC_BASE + AC_LINE * 64
  li    t1, DATA_SEED
  li    t2, 32
  bne   s0, t2, load_check
  li    t2, 0x10000
  add   t1, t1, t2
load_check:
  .set offset, 0
  .rept 16
  lw    t2, offset(t0)
  bne   t2, t1, bad_data
  addi  t1, t1, 1
  .set offset, offset + 4
  .endr
  li    t0, AC_LINE
  bne   a0, t0, bad_data
  li    t0, COOKIE | 2
  bne   a1, t0, bad_data
  li    t0, 2
  bne   s4, t0, bad_protocol
  bnez  s9, bad_protocol
  li    s9, 1

busy_work:
  # Keep a remote core active much longer than the idle gate. PU64 finishes
  # later than PU32, so receiving only the first completion is insufficient.
  slli  t0, s0, 4
  li    t1, 4096
  add   t0, t0, t1
busy_loop:
  addi  t0, t0, -1
  bnez  t0, busy_loop
  mv    a0, s0
  mv    a1, s4
  li    t0, 1
  li    t1, TAG_REPORT
  SEND  t0, t1
  j     idle

report_handler:
  li    t0, 1
  bne   s0, t0, bad_protocol
  bne   a1, s4, bad_protocol
  li    t0, 32
  li    t1, 1
  beq   a0, t0, record_report
  li    t0, 64
  bne   a0, t0, bad_protocol
  li    t1, 2
record_report:
  and   t0, s5, t1
  bnez  t0, bad_protocol
  or    s5, s5, t1
  j     idle

idle_handler:
  bnez  a0, bad_status
  li    t0, 3
  bne   s5, t0, bad_early     # never wait for the workers in this handler
  li    t0, 1
  bne   s0, t0, bad_protocol
  addi  s6, s6, 1
  bne   s6, s4, bad_protocol
  li    t0, RESULT_ADDR
  sw    s4, 0(t0)            # expose each notification's timing to cosim
  li    t0, 1
  bne   s4, t0, success
  li    s4, 2
  j     start_epoch
success:
  li    t0, 2
  bne   s6, t0, bad_protocol
  li    t0, RESULT_ADDR
  li    t1, 0x600d0002
  sw    t1, 0(t0)
  li    t0, STOP_ADDR
  sw    zero, 0(t0)
  j     idle

bad_config:
  li    t6, 1
  j     fail
bad_status:
  li    t6, 2
  j     fail
bad_early:
  li    t6, 3
  j     fail
bad_protocol:
  li    t6, 4
  j     fail
bad_data:
  li    t6, 5
fail:
  li    t0, RESULT_ADDR
  sw    s0, 0(t0)
  sw    s4, 0(t0)
  sw    s5, 0(t0)
  li    t0, STOP_ADDR
  sw    t6, 0(t0)
  j     idle

  .section .data
  .balign 64
source_line:
  .set word_index, 0
  .rept 16
  .word DATA_SEED + MC_INDEX * 0x10000 + word_index
  .set word_index, word_index + 1
  .endr
