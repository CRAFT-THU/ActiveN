  # Concurrent SHA-256 test for System module
  #
  # PU1 distributes integers 1..NUM_PU to each PU via active message (tag 0).
  # Each PU computes SHA-256 of its received integer.
  # Each PU writes hash[0] to result_buffer[hartid-1] (local copy).
  # Workers send "done" (tag 1) to PU1 with (hash, hartid) as payload.
  # PU1 collects all hashes into its own result_buffer, then computes
  # SHA-256 of the entire buffer (NUM_PU * 4 bytes).
  # PU1 writes final hash[0] to peripheral for end-of-simulation.
  #
  # State layout:
  #   Registers (read-only, both threads): s0=hartid, s1=NUM_PU
  #   SPM (mutable): SPM_DONE_COUNT — atomic counter for collected results
  # Both threads WFI. done_handler uses amoadd for concurrent access.

#define STOP_ADDR       0x40000000
#define RESULT_ADDR     0x4000000C
#define CONFIG_ROM_ADDR 0x68000000
#define SPM_BASE        0x20000000
#define SPM_SIZE        16384
#define SPM_DONE_COUNT  (SPM_BASE)

  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .section .text
  .globl _start
  .globl result_buffer

_start:
  bnez  x10, thread1_init
  csrr  s0, 0xF14             # s0 = hartid (1..NUM_PU)

  # Use local SPM for stack (each core has its own 16KB SPM at 0x20000000-0x20003FFF)
  li    sp, 0x20004000        # Stack top = end of SPM (grows downward)

  # Read-only config
  li    t0, CONFIG_ROM_ADDR
  lw    s1, 0(t0)             # s1 = NUM_PU

  # Register tag 0 handler (work assignment, 1 arg)
  la    t0, work_handler
  csrw  0x700, t0           # address
  li    t0, (3 << 3) | 1    # all threads, argcnt=1
  csrw  0x710, t0           # hancfg (enables handler)

  # Register tag 1 handler (done signal, 2 args: hash + hartid)
  la    t0, done_handler
  csrw  0x701, t0           # address
  li    t0, (3 << 3) | 2    # all threads, argcnt=2
  csrw  0x711, t0           # hancfg (enables handler)

  # Clear SPM done counter
  li    t0, SPM_DONE_COUNT
  sw    zero, 0(t0)

  # Only PU1 distributes work
  li    t0, 1
  bne   s0, t0, do_wfi

  # PU1: send integer i to PU_i, for i = 1..NUM_PU
  li    t2, 1                  # loop counter
  li    t3, 0                  # tag = 0
distribute:
  mv    a0, t2                 # payload = PU ID
  SEND  t2, t3
  addi  t2, t2, 1
  addi  t0, s1, 1             # NUM_PU + 1
  bne   t2, t0, distribute
  j     do_wfi

  # ============ Thread 1: init read-only registers ============
thread1_init:
  csrr  s0, 0xF14
  li    sp, 0x20004000
  li    t0, CONFIG_ROM_ADDR
  lw    s1, 0(t0)             # s1 = NUM_PU

do_wfi:
  wfi

  # --- Handler for tag 0: work assignment ---
  # Called on each PU when it receives its integer from PU1.
  # a0 = received integer, s0 = hartid, s1 = NUM_PU
work_handler:
  call  sha256_word            # a0 = sha256_word(a0) -> hash[0]

  # Write hash[0] to local result_buffer[hartid - 1]
  la    t0, result_buffer
  addi  t1, s0, -1
  slli  t1, t1, 2
  add   t0, t0, t1
  sw    a0, 0(t0)

  # If PU1: own work done, increment counter atomically
  li    t0, 1
  beq   s0, t0, self_done

  # Workers: send done to PU1 with tag 1, payload = (hash, hartid)
  mv    a1, s0                 # a1 = hartid
  li    t0, 1                  # dst = PU1
  li    t1, 1                  # tag = 1 (handler 1)
  SEND  t0, t1
  j     do_wfi

self_done:
  li    t0, SPM_DONE_COUNT
  li    t1, 1
  amoadd.w t1, t1, (t0)       # t1 = old count
  addi  t1, t1, 1
  bge   t1, s1, all_done
  j     do_wfi

  # --- Handler for tag 1: done signal (PU1 only) ---
  # a0 = worker's hash[0], a1 = worker's hartid
done_handler:
  # Store worker's hash into PU1's result_buffer
  la    t0, result_buffer
  addi  t1, a1, -1
  slli  t1, t1, 2
  add   t0, t0, t1
  sw    a0, 0(t0)

  li    t0, SPM_DONE_COUNT
  li    t1, 1
  amoadd.w t1, t1, (t0)       # t1 = old count
  addi  t1, t1, 1
  bge   t1, s1, all_done
  j     do_wfi

  # --- All PUs done: PU1 computes final hash ---
all_done:
  la    a0, result_buffer
  mv    a1, s1                 # n_words = NUM_PU
  call  sha256_buffer          # a0 = sha256_buffer(result_buffer, NUM_PU)

  # Output result to RESULT_ADDR
  li    t0, RESULT_ADDR
  sw    a0, 0(t0)

  # SHA-256 result is config-dependent; exit 0 (computation completed)
  li    t0, STOP_ADDR
  sw    zero, 0(t0)
  j     do_wfi
