  # SPM initialization test
  #
  # Flow:
  #   1. Each PU reads its SPM init descriptor from DRAM image 0.
  #   2. Each PU copies its SPM init data from DRAM (full spm_size).
  #   3. Each PU XORs all SPM words, sends result to PU1 (tag 0).
  #   4. PU1 collects all XOR values (including its own), computes global XOR,
  #      outputs to RESULT_ADDR, and exits success.
  #
  # DRAM image 0 layout (at 0x80000000):
  #   +0x00: jump instruction (jumps after header to text)
  #   +0x04: offset to SPM init section
  #   +0x08: num_mc
  #   +0x0C: num_pu
  #   Then: text binary, CSR data, SPM init section
  #
  # Threading model:
  #   Both threads WFI. Handler is installed on PU1 AFTER copy+XOR,
  #   so events won't be scheduled until PU1 is ready (handler PC=0 → no dispatch).
  #   Handler uses AMO on SPM for global XOR (both threads can run handler).
  #
  # Registers:
  #   s0 = hartid (both threads)
  #   s1 = NUM_PU (both threads)
  #
  # SPM handler state (PU1 only, zeroed after copy):
  #   SPM_BASE + 0: done counter
  #   SPM_BASE + 4: global XOR accumulator

#define STOP_ADDR           0x40000000
#define RESULT_ADDR         0x4000000C
#define CONFIG_ROM_ADDR     0x68000000
#define DRAM_BASE           0x80000000
#define SPM_BASE            0x20000000
#define SPM_SIZE            0x4000
#define DONE_COUNT          (SPM_BASE)
#define XOR_ACCUM           (SPM_BASE + 4)
#define LEADER              1

  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .section .text
  .globl _start

_start:
  bnez  x10, thread1_init
  csrr  s0, 0xF14             /* s0 = hartid (1..NUM_PU) */

  /* ---- read NUM_PU from config ROM ---- */
  li    t0, CONFIG_ROM_ADDR
  lw    s1, 0(t0)             /* s1 = NUM_PU */

  /* ---- quotas/margins (set before handler install) ---- */
  csrw  0x730, zero
  csrw  0x720, zero
  /* handler PC stays 0 — events not scheduled until we install it */

  /* ---- read SPM init descriptor ---- */
  li    t0, DRAM_BASE
  lw    t1, 4(t0)             /* t1 = spm_init_offset (byte offset in DRAM image) */
  add   t1, t0, t1            /* t1 = absolute addr of SPM init section */

  /* Descriptor for this PU: base + (hartid-1) * 8 */
  addi  t2, s0, -1
  slli  t2, t2, 3             /* * 8 */
  add   t2, t1, t2            /* t2 -> this PU's descriptor */
  lw    t3, 0(t2)             /* t3 = data offset (relative to spm_init_section start) */
  lw    t4, 4(t2)             /* t4 = data size (bytes) */

  /* Compute absolute source address */
  add   t3, t1, t3            /* t3 = absolute DRAM addr of SPM data for this PU */

  /* ---- copy SPM data ---- */
  li    t5, SPM_BASE
  add   t6, t5, t4            /* t6 = end address */
  beqz  t4, copy_done
copy_loop:
  lw    a0, 0(t3)
  sw    a0, 0(t5)
  addi  t3, t3, 4
  addi  t5, t5, 4
  blt   t5, t6, copy_loop
copy_done:

  /* ---- compute XOR of all copied SPM words ---- */
  li    t0, SPM_BASE
  add   t1, t0, t4            /* t1 = SPM_BASE + size */
  li    t2, 0                  /* t2 = XOR accumulator */
xor_loop:
  lw    t3, 0(t0)
  xor   t2, t2, t3
  addi  t0, t0, 4
  blt   t0, t1, xor_loop

  /* ---- PU1: zero handler state in SPM, then install handler ---- */
  /* This must happen AFTER copy+XOR so the handler doesn't race with us. */
  li    t0, LEADER
  bne   s0, t0, send_xor

  /* Zero done counter and XOR accumulator */
  li    t0, DONE_COUNT
  sw    zero, 0(t0)
  li    t0, XOR_ACCUM
  sw    zero, 0(t0)

  /* Now install the handler — enables dispatch on PU1 */
  /* Set argcount BEFORE handler PC to avoid race: if handler PC is set
     first, a message could dispatch before argcount is configured. */
  li    a0, 1
  csrw  0x710, a0
  la    a0, done_handler
  csrw  0x700, a0

send_xor:
  /* ---- send XOR result to PU1 (tag 0, 1 arg) ---- */
  mv    a0, t2
  li    t0, LEADER
  li    t1, 0                  /* tag = 0 */
  SEND  t0, t1

  j     do_wfi

/* ---- Thread 1: init read-only registers, then WFI ---- */
thread1_init:
  csrr  s0, 0xF14
  li    t0, CONFIG_ROM_ADDR
  lw    s1, 0(t0)             /* s1 = NUM_PU */

do_wfi:
  wfi

/* ================================================================
 * Handler: tag 0 — done signal (PU1 only)
 * a0 = sender's local XOR
 * Uses AMO on SPM — safe with both SMT threads running handlers.
 * ================================================================ */
done_handler:
  /* Load both addresses (keep t0 stable for amoadd) */
  li    t0, DONE_COUNT
  addi  t2, t0, 4             /* t2 = XOR_ACCUM */

  /* XOR payload into global accumulator atomically */
  amoxor.w zero, a0, (t2)

  /* Increment done counter atomically */
  li    t1, 1
  amoadd.w t1, t1, (t0)       /* t1 = old count */
  addi  t1, t1, 1             /* t1 = new count */
  bge   t1, s1, all_done
  j     do_wfi

/* ---- All PUs reported: output global XOR and exit ---- */
all_done:
  li    t0, XOR_ACCUM
  lw    t1, 0(t0)             /* global XOR */
  li    t0, RESULT_ADDR
  sw    t1, 0(t0)
  /* Exit success */
  li    t0, STOP_ADDR
  sw    zero, 0(t0)
  j     do_wfi
