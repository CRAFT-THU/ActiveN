  # Local send test (dst=0 → local event queue)
  #
  # Tests that sending with dst=0 pushes directly to the local EvQueue.
  # PU1 sends itself a message (tag=0, dst=0, data=42).
  # The handler receives it and writes the result to the stop register.
  #
  # All other PUs just WFI forever.

#define STOP_ADDR 0x40000000
#define SPM_STATE_ADDR 0x20003FF8

  .section .text
  .globl _start

_start:
  bnez  x10, .park
  csrr  s0, 0xF14          # s0 = hartid

  # Check if already initialized
  li    t0, SPM_STATE_ADDR
  lw    t1, 0(t0)
  bnez  t1, do_wfi

  # Configure handler 0 (tag=0): local_handler, argcnt=1
  la    a0, local_handler
  csrw  0x700, a0
  li    a0, 1
  csrw  0x710, a0

  # Mark initialized
  li    t1, 1
  li    t0, SPM_STATE_ADDR
  sw    t1, 0(t0)

  # Only PU1 does the local send test
  li    t0, 1
  bne   s0, t0, do_wfi

  # PU1: send to self via local send (dst=0, tag=0)
  # data = 42 in a0, target = (tag=0 << 16) | dst=0 = 0x00000000 in a1
  li    a0, 42
  li    a1, 0               # dst=0, tag=0
  .word 0x00B5500B          # send.1 a0(x10), a1(x11)

do_wfi:
  wfi

  #============================================================
  # Handler: local (tag=0, 1 arg: a0=data)
  #============================================================
local_handler:
  # a0 should be 42 (the data we sent)
  # Write it to stop register to end simulation
  li    t0, STOP_ADDR
  sw    a0, 0(t0)
  j     do_wfi

.park:
  wfi
  j     .park
