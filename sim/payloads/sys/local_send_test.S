  # Local send test (dst=0 → local event queue)
  #
  # Tests that sending with dst=0 pushes directly to the local EvQueue.
  # PU1 sends itself a message (tag=0, dst=0, data=42).
  # The handler receives it and checks the result.
  #
  # Thread 0: setup + send, then WFI.
  # Thread 1: init registers, then WFI.
  # Only one event ever arrives, so both threads idle is safe.

#define STOP_ADDR       0x40000000
#define RESULT_ADDR     0x4000000C
#define EXPECTED        42

  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .section .text
  .globl _start

_start:
  bnez  x10, thread1_init
  csrr  s0, 0xF14          # s0 = hartid

  # Configure handler 0 (tag=0): local_handler, argcnt=1
  la    a0, local_handler
  csrw  0x700, a0           # address
  li    a0, (3 << 3) | 1    # all threads, argcnt=1
  csrw  0x710, a0           # hancfg (enables handler)

  # Only PU1 does the local send test
  li    t0, 1
  bne   s0, t0, do_wfi

  # PU1: send to self via local send (dst=0, tag=0)
  li    a0, 42
  li    t1, 0               # tag = 0
  SEND  zero, t1            # dst = x0 (local)
  j     do_wfi

thread1_init:
  csrr  s0, 0xF14

do_wfi:
  wfi

  #============================================================
  # Handler: local (tag=0, 1 arg: a0=data)
  #============================================================
local_handler:
  # a0 should be 42 (the data we sent)
  li    t0, RESULT_ADDR
  sw    a0, 0(t0)
  # Exit 0 if pass, 1 if fail
  li    t0, STOP_ADDR
  li    t1, EXPECTED
  beq   a0, t1, .pass
  li    t1, 1
  sw    t1, 0(t0)
  j     do_wfi
.pass:
  sw    zero, 0(t0)
  j     do_wfi
