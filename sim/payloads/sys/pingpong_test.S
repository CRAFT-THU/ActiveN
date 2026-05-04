  # Ping-pong ring test for System module
  # PU1 sends 0 around ring: PU1 -> PU2 -> ... -> PU_N -> PU1
  # PU1 increments on each round trip until value reaches 100.
  # Expected result: 100
  #
  # NUM_PU is read from config ROM at runtime.
  # Thread 0: setup + initiate, then WFI.
  # Thread 1: register init, then WFI.
  # Only one event per PU at a time (strict ring), so both threads idle is safe.
  # New ISA: SEND dst_reg, tag_reg — payload in a0-a3

#define STOP_ADDR       0x40000000
#define RESULT_ADDR     0x4000000C
#define CONFIG_ROM_ADDR 0x68000000
#define EXPECTED        100

  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .section .text
  .globl _start
_start:
  bnez  x10, thread1_init

  # ============ Thread 0: setup + initiate ============
  li    t0, CONFIG_ROM_ADDR
  lw    s2, 0(t0)          # s2 = NUM_PU

  # Register handler for tag 0
  la    t0, handler
  csrw  0x700, t0
  li    t0, 1
  csrw  0x710, t0           # 1 argument

  csrr  s0, 0xF14           # s0 = hartid (1..NUM_PU)

  # Compute next PU in ring: PU1->2, PU2->3, ..., PU_N->1
  addi  s1, s0, 1
  addi  t0, s2, 1           # NUM_PU + 1
  bne   s1, t0, 1f
  li    s1, 1               # Wrap: last PU -> PU1
1:
  # Only PU1 initiates
  li    t0, 1
  bne   s0, t0, do_wfi

  # PU1: send initial value 0 to PU2
  li    a0, 0
  li    t1, 0               # tag = 0
  SEND  s1, t1
  j     do_wfi

  # ============ Thread 1: register init ============
thread1_init:
  csrr  s0, 0xF14           # persistent: hartid
  li    t0, CONFIG_ROM_ADDR
  lw    s2, 0(t0)           # persistent: NUM_PU

  # Compute next PU in ring (same logic)
  addi  s1, s0, 1
  addi  t0, s2, 1
  bne   s1, t0, 2f
  li    s1, 1
2:

do_wfi:
  wfi

handler:
  # a0 = received value, s0 = hartid, s1 = next PU (persistent)
  li    t0, 1
  bne   s0, t0, forward

  # PU1: increment and check
  addi  a0, a0, 1
  li    t0, EXPECTED
  bge   a0, t0, done

forward:
  li    t1, 0               # tag = 0
  SEND  s1, t1
  j     do_wfi

done:
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
