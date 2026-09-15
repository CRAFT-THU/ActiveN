  # Fire-and-forget scalar-store regression for the 64-PU / 2-MC system.
  # PU1 issues 300 requests, more than the 256-entry DRAM MSHR capacity,
  # with tag 0x402 (priority 1, low-byte operation 0x02).  After the request
  # stream has drained, the test reads every word back through the ordinary
  # global-load path and checks the complete data set before exiting.

  .macro SEND dst, tag
  .insn r 0x0b, 0x4, 0, x0, \dst, \tag
  .endm

  .section .text
  .globl _start
_start:
  bnez  a0, idle
  csrr  s0, 0xf14
  li    t0, 1
  bne   s0, t0, idle

  # This regression intentionally targets the elaborated 64-PU / 2-MC setup.
  li    t0, 0x68000000
  lw    t1, 0(t0)
  li    t2, 64
  bne   t1, t2, fail_config
  lw    t1, 8(t0)
  li    t2, 2
  bne   t1, t2, fail_config

  li    s1, 0x8001       # MC0 destination
  li    s2, 0x402        # low byte 0x02, with software priority 1
  li    s3, 0x1000       # MC-local store address
  li    s4, 300
  li    s5, 0
  li    s6, 0xa5a50000
send_loop:
  mv    a0, s3
  li    a1, 0x20000      # size = log2(4 bytes), request ID ignored
  li    a2, 0
  add   a3, s6, s5
  SEND  s1, s2
  addi  s3, s3, 4
  addi  s5, s5, 1
  bltu  s5, s4, send_loop

  # Give the final accepted flits time to reach and retire at MC0 before the
  # architectural readback sequence begins.
  li    s5, 10000
drain_delay:
  addi  s5, s5, -1
  bnez  s5, drain_delay

  # Read back every value through the regular LSU global-memory path.  A
  # mismatch means at least one no-receipt store was not committed correctly.
  li    s3, 0x80001000   # global address corresponding to MC0-local 0x1000
  li    s5, 300
  li    s6, 0
read_loop:
  lw    t0, 0(s3)
  li    t1, 0xa5a50000
  add   t1, t1, s6
  bne   t0, t1, fail_data
  addi  s3, s3, 4
  addi  s6, s6, 1
  bltu  s6, s5, read_loop

  li    t0, 0x40000000
  sw    zero, 0(t0)

idle:
  wfi
  j     idle

fail_config:
  li    t6, 1
  j     fail
fail_data:
  li    t6, 2
fail:
  li    t0, 0x4000000c
  sw    t6, 0(t0)
  li    t0, 0x40000000
  sw    t6, 0(t0)
  j     idle
