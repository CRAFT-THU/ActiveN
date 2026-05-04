  # Barrier test
  #
  # All PUs send a "start" message (tag 2) to the leader (PU 1).
  # The leader counts arrivals in SPM. When count == NUM_PU, test passes.
  #
  # Read-only config in registers (s0=hartid, s1=NUM_PU).
  # Mutable counter in SPM (shared between threads).
  # Both threads WFI after setup.

#define STOP_ADDR            0x40000000
#define RESULT_ADDR          0x4000000C
#define CONFIG_ROM_ADDR      0x68000000
#define LEADER               1
#define TAG_START            0xC02   /* prio 3, handler 2 */

#define SPM_BASE             0x20000000
#define SPM_SIZE             16384
#define SPM_COUNTER          (SPM_BASE + SPM_SIZE - 4)

  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .section .text
  .globl _start

_start:
  bnez  x10, thread1_init

  # ---- Thread 0: configure + send ----
  csrr  s0, 0xF14
  li    t0, CONFIG_ROM_ADDR
  lw    s1, 0(t0)           # s1 = NUM_PU (read-only)

  # Handler 2: start_handler on LEADER, noop on others
  li    t0, LEADER
  bne   s0, t0, 1f
  la    a0, start_handler
  j     2f
1:
  la    a0, noop_handler
2:
  csrw  0x702, a0
  li    a0, 1
  csrw  0x712, a0

  # Clear counter (leader only)
  li    t0, LEADER
  bne   s0, t0, 3f
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
3:
  # Quotas / margins for handler 2
  li    a0, 1
  csrw  0x732, a0
  li    a0, 2
  csrw  0x722, a0

  # Send start to LEADER
  li    a0, 0
  li    t0, LEADER
  li    t1, TAG_START
  SEND  t0, t1
  j     do_wfi

  # ---- Thread 1: init read-only registers ----
thread1_init:
  csrr  s0, 0xF14
  li    t0, CONFIG_ROM_ADDR
  lw    s1, 0(t0)           # s1 = NUM_PU

do_wfi:
  wfi

start_handler:
  li    t0, SPM_COUNTER
  li    t1, 1
  amoadd.w t1, t1, (t0)       # t1 = old count
  addi  t1, t1, 1             # t1 = new count
  bge   t1, s1, start_done
  j     do_wfi

start_done:
  li    t0, RESULT_ADDR
  sw    t1, 0(t0)
  # Exit 0 if count >= NUM_PU, else 1
  li    t0, STOP_ADDR
  bge   t1, s1, .pass
  li    t1, 1
  sw    t1, 0(t0)
  j     do_wfi
.pass:
  sw    zero, 0(t0)
  j     do_wfi

noop_handler:
  j     do_wfi
