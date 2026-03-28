  # Concurrent SHA-256 test for System module
  #
  # PU1 distributes integers 1..N to each PU via active message (tag 0).
  # Each PU computes SHA-256 of its received integer.
  # Each PU writes hash[0] to result_buffer[hartid-1].
  # Workers send "done" (tag 1) to PU1.
  # PU1 collects all done signals, then computes SHA-256 of the buffer.
  # PU1 sends final hash[0] to MC for end-of-simulation.

#ifndef NUM_PU
#define NUM_PU 16
#endif

  .section .text
  .globl _start
  .globl result_buffer

_start:
  csrr s0, mhartid          # s0 = hartid (1..NUM_PU)

  # Use local SPM for stack (each core has its own 16KB SPM at 0x20000000-0x20003FFF)
  li sp, 0x20004000         # Stack top = end of SPM (grows downward)

  # Register tag 0 handler (work assignment)
  la t0, work_handler
  csrw 0x700, t0
  li t0, 1
  csrw 0x710, t0            # 1 argument

  # Register tag 1 handler (done signal, PU1 only - harmless on others)
  la t0, done_handler
  csrw 0x701, t0
  li t0, 1
  csrw 0x711, t0            # 1 argument

  li s3, 0                   # done counter (PU1 uses this)

  # Only PU1 distributes work
  li t0, 1
  bne s0, t0, wait

  # PU1: send integer i to PU_i, for i = 1..NUM_PU
  # Use x3 (gp) for destination to avoid clobbering sp (x2)
  li s1, 1
distribute:
  mv a0, s1                  # value = PU ID
  mv x3, s1                  # destination = PU_i, tag = 0
  .word 0x0035500b           # Send a0 to x3
  addi s1, s1, 1
  li t0, NUM_PU + 1
  bne s1, t0, distribute

wait:
  wfi

  # --- Handler for tag 0: work assignment ---
  # Called on each PU when it receives its integer from PU1.
  # a0 = received integer, s0 = hartid (preserved)
work_handler:
  call sha256_word           # a0 = sha256_word(a0) -> hash[0]

  # Write hash[0] to result_buffer[hartid - 1]
  la t0, result_buffer
  addi t1, s0, -1
  slli t1, t1, 2
  add t0, t0, t1
  sw a0, 0(t0)

  # If PU1: own work done, increment counter
  li t0, 1
  beq s0, t0, self_done

  # Workers: send done to PU1 with tag 1
  # x3 = (1 << 16) | 1 = 0x10001 -> tag=1, dst=PU1
  li x3, 0x10001
  .word 0x0035500b           # Send a0 to PU1 with tag 1
  wfi

self_done:
  addi s3, s3, 1
  li t0, NUM_PU
  bge s3, t0, all_done
  wfi

  # --- Handler for tag 1: done signal (PU1 only) ---
  # a0 = worker's hash[0] (unused here)
done_handler:
  addi s3, s3, 1
  li t0, NUM_PU
  bge s3, t0, all_done
  wfi

  # --- All PUs done: PU1 computes final hash ---
all_done:
  la a0, result_buffer
  li a1, NUM_PU
  call sha256_buffer         # a0 = sha256_buffer(result_buffer, NUM_PU)

  # Signal end-of-simulation by writing result to peripheral stop register
  li t0, 0x40000000
  sw a0, 0(t0)
  wfi
