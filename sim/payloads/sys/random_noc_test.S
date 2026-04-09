#define STOP_ADDR            0x40000000
#define TIMER_ADDR           0x40000004
#define RNG_ADDR             0x40000010
#define SPM_BASE             0x20000000
#define SPM_SIZE             16384

#define SPM_STATE            (SPM_BASE + SPM_SIZE - 4)
#define SPM_ITER             (SPM_BASE + SPM_SIZE - 8)
#define SPM_PRNG             (SPM_BASE + SPM_SIZE - 12)
#define SPM_RECV_XOR         (SPM_BASE + SPM_SIZE - 16)
#define SPM_MEM_XOR          (SPM_BASE + SPM_SIZE - 20)
#define SPM_MSG_COUNT        (SPM_BASE + SPM_SIZE - 24)
#define SPM_READY_COUNT      (SPM_BASE + SPM_SIZE - 28)
#define SPM_REPORT_COUNT     (SPM_BASE + SPM_SIZE - 32)
#define SPM_MEM_REPORT_COUNT (SPM_BASE + SPM_SIZE - 36)
#define SPM_ACT_RECV_XOR     (SPM_BASE + SPM_SIZE - 40)
#define SPM_ACT_MSG_COUNT    (SPM_BASE + SPM_SIZE - 44)
#define SPM_ACT_MEM_XOR      (SPM_BASE + SPM_SIZE - 48)
#define SPM_EXP_RECV_XOR     (SPM_BASE + SPM_SIZE - 52)
#define SPM_EXP_MEM_XOR      (SPM_BASE + SPM_SIZE - 56)
#define SPM_DONE             (SPM_BASE + SPM_SIZE - 60)

#define NUM_ITERS            16
#define ROOT_POLL_DELAY      8192
#define REGION_WORDS         16
#define REGION_SHIFT         6
#define DATA_PATTERN_BASE    0x13570000
#define JITTER_MASK          0x00000fff

#define TARGET_STEP_LOCAL    0x00010000
  .macro SEND1 data, target
  .insn r 0x0B, 0x5, 0x0, x0, \data, \target
  .endm

  .macro YIELD1 data, target
  .insn r 0x0B, 0x5, 0x1, x0, \data, \target
  .endm

  .macro QUEUE2 data0, data1
  .insn r 0x0B, 0x2, 0x0, x0, \data0, \data1
  .endm

  .macro FIRE0 target
  .insn r 0x0B, 0x4, 0x0, x0, x0, \target
  .endm

  .macro XORSHIFT reg, tmp
  slli  \tmp, \reg, 13
  xor   \reg, \reg, \tmp
  srli  \tmp, \reg, 17
  xor   \reg, \reg, \tmp
  slli  \tmp, \reg, 5
  xor   \reg, \reg, \tmp
  .endm

  .section .text
  .globl _start

_start:
  li    sp, SPM_BASE + SPM_SIZE - 128
  csrr  s0, 0xF14

  li    t0, SPM_STATE
  lw    t1, 0(t0)
  bnez  t1, do_wfi

  li    t0, 1
  bne   s0, t0, select_handler_setup
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)

select_handler_setup:
  li    t0, 1
  beq   s0, t0, root_handler_setup

  la    a0, msg_handler
  csrw  0x700, a0
  li    a0, 1
  csrw  0x710, a0

  la    a0, step_handler
  csrw  0x701, a0
  li    a0, 1
  csrw  0x711, a0

  la    a0, ready_handler
  csrw  0x702, a0
  li    a0, 1
  csrw  0x712, a0

  la    a0, ready_handler
  csrw  0x703, a0
  li    a0, 1
  csrw  0x713, a0

  la    a0, ready_handler
  csrw  0x704, a0
  li    a0, 1
  csrw  0x714, a0
  j     handlers_done

root_handler_setup:
  la    a0, report_handler
  csrw  0x700, a0
  li    a0, 1
  csrw  0x710, a0

  la    a0, ready_handler
  csrw  0x701, a0
  li    a0, 1
  csrw  0x711, a0

  la    a0, ready_handler
  csrw  0x702, a0
  li    a0, 1
  csrw  0x712, a0

  la    a0, ready_handler
  csrw  0x703, a0
  li    a0, 1
  csrw  0x713, a0

  la    a0, ready_handler
  csrw  0x704, a0
  li    a0, 1
  csrw  0x714, a0

handlers_done:

  li    t0, SPM_ITER
  sw    zero, 0(t0)
  li    t0, SPM_RECV_XOR
  sw    zero, 0(t0)
  li    t0, SPM_MEM_XOR
  sw    zero, 0(t0)
  li    t0, SPM_MSG_COUNT
  sw    zero, 0(t0)
  li    t0, SPM_DONE
  sw    zero, 0(t0)

  li    t0, SPM_READY_COUNT
  sw    zero, 0(t0)
  li    t0, SPM_REPORT_COUNT
  sw    zero, 0(t0)
  li    t0, SPM_MEM_REPORT_COUNT
  sw    zero, 0(t0)
  li    t0, SPM_ACT_RECV_XOR
  sw    zero, 0(t0)
  li    t0, SPM_ACT_MSG_COUNT
  sw    zero, 0(t0)
  li    t0, SPM_ACT_MEM_XOR
  sw    zero, 0(t0)
  li    t0, SPM_EXP_RECV_XOR
  sw    zero, 0(t0)
  li    t0, SPM_EXP_MEM_XOR
  sw    zero, 0(t0)

  li    t0, SPM_STATE
  li    t1, 1
  sw    t1, 0(t0)

  li    t0, RNG_ADDR
  lw    t1, 0(t0)
  li    t2, JITTER_MASK
  and   t2, t1, t2
  addi  t2, t2, 1
  li    t3, 3
  li    t4, 7

startup_jitter_loop:
  beqz  t2, startup_jitter_done
  mul   t3, t3, t4
  addi  t2, t2, -1
  j     startup_jitter_loop

startup_jitter_done:
  li    t0, SPM_PRNG
  sw    t1, 0(t0)

  li    t0, 1
  beq   s0, t0, root_prepare_launch

  addi  t2, s0, -2
  slli  t2, t2, 2
  la    t0, seed_words
  add   t0, t0, t2
  sw    t1, 0(t0)
  la    t0, ready_flags
  add   t0, t0, t2
  li    t3, 1
  sw    t3, 0(t0)
  j     launch_local_step

root_prepare_launch:
  li    t0, 2
  li    t1, 0
  li    t2, 0

expect_core_loop:
  addi  t6, t0, -2
  slli  t6, t6, 2
  la    a0, ready_flags
  add   a0, a0, t6

expect_wait_seed:
  lw    a1, 0(a0)
  beqz  a1, expect_wait_seed

  la    a0, seed_words
  add   a0, a0, t6
  lw    t3, 0(a0)
  li    t4, 0

expect_iter_loop:
  li    t5, NUM_ITERS
  beq   t4, t5, expect_next_core

  XORSHIFT t3, t6
  slli  t6, t0, 16
  xor   t6, t6, t3
  xor   t6, t6, t4
  xor   t1, t1, t6

  XORSHIFT t3, t6
  andi  t6, t3, REGION_WORDS - 1
  addi  a2, t0, -1
  slli  a2, a2, 4
  add   a2, a2, t6
  li    a3, DATA_PATTERN_BASE
  add   a3, a3, a2
  xor   t2, t2, a3

  addi  t4, t4, 1
  j     expect_iter_loop

expect_next_core:
  addi  t0, t0, 1
  li    t3, NUM_PU + 1
  bne   t0, t3, expect_core_loop

  xor   t1, t1, t2
  li    t0, NUM_PU - 1
  andi  t0, t0, 1
  beqz  t0, 1f
  xori  t1, t1, NUM_ITERS
1:
  li    t0, SPM_EXP_RECV_XOR
  sw    t1, 0(t0)
  li    t0, SPM_EXP_MEM_XOR
  sw    zero, 0(t0)

  li    t0, ROOT_POLL_DELAY
root_poll_delay:
  addi  t0, t0, -1
  bnez  t0, root_poll_delay

root_wait_reports:
  li    t0, 0
  li    t1, 0
  li    t2, 0
  la    t3, report_xors
  la    t4, report_done

root_wait_loop:
  li    t5, NUM_PU - 1
  beq   t0, t5, root_wait_check
  slli  t6, t0, 2
  add   a0, t4, t6
  lw    a1, 0(a0)
  add   t2, t2, a1
  add   a0, t3, t6
  lw    a1, 0(a0)
  xor   t1, t1, a1
  addi  t0, t0, 1
  j     root_wait_loop

root_wait_check:
  li    t5, NUM_PU - 1
  bne   t2, t5, root_wait_reports

  li    t0, TIMER_ADDR
  sw    zero, 0(t0)

  li    t0, SPM_EXP_RECV_XOR
  lw    t2, 0(t0)
  bne   t1, t2, root_fail

  li    t0, STOP_ADDR
  sw    t1, 0(t0)
  j     do_wfi

root_fail:
  li    t1, 0xdead0002
  li    t0, STOP_ADDR
  sw    t1, 0(t0)
  j     do_wfi

launch_local_step:
  li    a0, 0
  li    t0, TARGET_STEP_LOCAL
  YIELD1 a0, t0
  j     do_wfi

do_wfi:
  wfi

msg_handler:
  li    t0, SPM_RECV_XOR
  lw    t1, 0(t0)
  xor   t1, t1, a0
  sw    t1, 0(t0)

  li    t0, SPM_MSG_COUNT
  lw    t1, 0(t0)
  addi  t1, t1, 1
  sw    t1, 0(t0)

  li    t2, NUM_ITERS
  bne   t1, t2, do_wfi

  addi  t4, s0, -2
  slli  t4, t4, 2
  la    t0, debug_msg16
  add   t0, t0, t4
  li    t3, 1
  sw    t3, 0(t0)

  li    t0, SPM_DONE
  lw    t2, 0(t0)
  bnez  t2, step_finish
  li    a0, 0
  li    t0, TARGET_STEP_LOCAL
  YIELD1 a0, t0
  j     do_wfi

step_handler:
  li    t0, SPM_DONE
  lw    t1, 0(t0)
  bnez  t1, do_wfi

  li    t0, SPM_ITER
  lw    t1, 0(t0)
  li    t2, NUM_ITERS
  bltu  t1, t2, step_do_work

  li    t0, SPM_MSG_COUNT
  lw    t3, 0(t0)
  bne   t3, t2, do_wfi
  j     step_finish

step_do_work:

  li    t0, SPM_PRNG
  lw    t3, 0(t0)
  XORSHIFT t3, t4
  sw    t3, 0(t0)

  slli  t4, s0, 16
  xor   a0, t3, t4
  xor   a0, a0, t1

  slli  t2, t1, 3
  sub   t2, t2, t1
  addi  t2, t2, 1
  li    t4, NUM_PU
  bltu  t2, t4, 1f
  addi  t2, t2, -(NUM_PU - 1)
1:
  add   t2, t2, s0
  li    t4, NUM_PU + 1
  bltu  t2, t4, step_have_dst
  addi  t2, t2, -(NUM_PU - 1)
step_have_dst:
  SEND1 a0, t2

  li    t0, SPM_PRNG
  lw    t3, 0(t0)
  XORSHIFT t3, t4
  sw    t3, 0(t0)

  andi  t4, t3, REGION_WORDS - 1
  la    t5, test_data
  addi  t6, s0, -1
  slli  t6, t6, REGION_SHIFT
  add   t5, t5, t6
  slli  t4, t4, 2
  add   t5, t5, t4
  lw    t6, 0(t5)

  li    t0, SPM_MEM_XOR
  lw    t2, 0(t0)
  xor   t2, t2, t6
  sw    t2, 0(t0)

  li    t0, SPM_ITER
  addi  t1, t1, 1
  sw    t1, 0(t0)

  li    t2, NUM_ITERS
  bne   t1, t2, step_schedule_next

  li    t0, SPM_DONE
  li    t3, 1
  sw    t3, 0(t0)

  addi  t4, s0, -2
  slli  t4, t4, 2
  la    t0, debug_done
  add   t0, t0, t4
  sw    t3, 0(t0)

  li    t0, SPM_MSG_COUNT
  lw    t3, 0(t0)
  bne   t3, t2, do_wfi
  j     step_finish

step_schedule_next:
  li    a0, 0
  li    t0, TARGET_STEP_LOCAL
  YIELD1 a0, t0
  j     do_wfi

step_finish:
  li    t0, SPM_DONE
  li    t1, 1
  sw    t1, 0(t0)

  li    t0, SPM_RECV_XOR
  lw    a0, 0(t0)
  li    t0, SPM_MEM_XOR
  lw    t1, 0(t0)
  xor   a0, a0, t1
  li    t0, SPM_MSG_COUNT
  lw    t1, 0(t0)
  xor   a0, a0, t1

  addi  t1, s0, -2
  slli  t1, t1, 2
  la    t0, report_xors
  add   t0, t0, t1
  sw    a0, 0(t0)
  la    t0, report_done
  add   t0, t0, t1
  li    t2, 1
  sw    t2, 0(t0)
  j     do_wfi

ready_handler:
  j     do_wfi

start_handler:
  j     do_wfi

report_handler:
  li    t0, 1
  bne   s0, t0, do_wfi

  li    t0, SPM_ACT_RECV_XOR
  lw    t1, 0(t0)
  xor   t1, t1, a0
  sw    t1, 0(t0)

  li    t0, SPM_REPORT_COUNT
  lw    t1, 0(t0)
  addi  t1, t1, 1
  sw    t1, 0(t0)
  j     maybe_finish

mem_report_handler:
  j     do_wfi

maybe_finish:
  li    t0, SPM_REPORT_COUNT
  lw    t1, 0(t0)
  li    t2, NUM_PU - 1
  bne   t1, t2, do_wfi

  li    t0, TIMER_ADDR
  sw    zero, 0(t0)

  li    t0, SPM_ACT_RECV_XOR
  lw    t1, 0(t0)
  li    t0, SPM_EXP_RECV_XOR
  lw    t2, 0(t0)
  bne   t1, t2, fail_recv

  li    t0, SPM_ACT_RECV_XOR
  lw    t1, 0(t0)
  li    t0, STOP_ADDR
  sw    t1, 0(t0)
  j     do_wfi

fail_recv:
  li    t1, 0xdead0002
  li    t0, STOP_ADDR
  sw    t1, 0(t0)
  j     do_wfi

  .section .data
  .align 2
start_flag:
  .word 0

  .align 5
ready_flags:
  .rept NUM_PU - 1
  .word 0
  .endr

  .align 5
seed_words:
  .rept NUM_PU - 1
  .word 0
  .endr

  .align 5
report_xors:
  .rept NUM_PU - 1
  .word 0
  .endr

  .align 5
report_done:
  .rept NUM_PU - 1
  .word 0
  .endr

  .align 5
debug_done:
  .rept NUM_PU - 1
  .word 0
  .endr

  .align 5
debug_msg16:
  .rept NUM_PU - 1
  .word 0
  .endr

  .align 5
test_data:
  .set i, 0
  .rept NUM_PU * REGION_WORDS
  .word DATA_PATTERN_BASE + i
  .set i, i + 1
  .endr