  # Ping-pong ring test for System module
  # PU1 sends 0 around ring: PU1 -> PU2 -> ... -> PU_N -> PU1
  # PU1 increments on each round trip until value reaches 100.
  # Expected result: 100

#ifndef NUM_PU
#define NUM_PU 16
#endif

  .section .text
  .globl _start
_start:
  # Register handler for tag 0
  la t0, handler
  csrw 0x700, t0
  li t0, 1
  csrw 0x710, t0      # 1 argument

  csrr s0, mhartid    # s0 = hartid (1..NUM_PU)

  # Compute next PU in ring: PU1->2, PU2->3, ..., PU_N->1
  addi s1, s0, 1
  li t0, NUM_PU + 1
  bne s1, t0, 1f
  li s1, 1            # Wrap: last PU -> PU1
1:
  # Only PU1 initiates
  li t0, 1
  bne s0, t0, wait

  # PU1: send initial value 0 to PU2
  li a0, 0
  mv x2, s1           # x2 = 2 (PU2), tag = 0
  .word 0x0025500b    # Send a0 to x2

wait:
  wfi

handler:
  # a0 = received value (from AM argument)
  # s0 = hartid, s1 = next PU (both preserved)

  # PU1 increments, others forward
  li t0, 1
  bne s0, t0, forward

  # PU1: increment and check
  addi a0, a0, 1
  li t0, 100
  bge a0, t0, done

forward:
  mv x2, s1
  .word 0x0025500b    # Send a0 to next PU
  wfi

done:
  # Signal end-of-simulation by writing result to peripheral stop register
  li t0, 0x40000000
  sw a0, 0(t0)
  wfi
