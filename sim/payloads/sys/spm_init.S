  # SPM initialization boot payload (restart-safe)
  #
  # This payload works with the WFI-restart model: WFI either dispatches
  # a pending event (branch to handler) or restarts from _start.
  # All persistent state is kept in SPM to survive restarts.
  #
  # SPM layout reserved words (at top, set AFTER copy):
  #   0x20003FF8: init_state (0=need copy, 1=copied+sent, 2+=done)
  #   0x20003FF4: done_counter (leader only)
  #
  # Descriptor table at DESCRIPTOR_BASE. Each descriptor = 8 bytes:
  #   +0: spm_src  (u32) - global memory address of SPM init data
  #   +4: spm_size (u32) - size in bytes (typically 16384)
  #
  # AM send encoding: .word 0x0035500b = send rs1(a0) to rs2(x3)

#ifndef NUM_PU
#define NUM_PU 16
#endif

#ifndef DESCRIPTOR_BASE
#define DESCRIPTOR_BASE 0x80100000
#endif

#define SPM_STATE_ADDR 0x20003FF8
#define SPM_COUNTER_ADDR 0x20003FF4

  .section .text
  .globl _start

_start:
  bnez x10, .park
  csrr s0, mhartid            # s0 = hartid (1..NUM_PU)
  li   sp, 0x20004000         # Stack at top of SPM

  # Register tag 0 handler (init_done, 1 arg) - must do every restart
  la   t0, init_done_handler
  csrw 0x700, t0
  li   t0, 1
  csrw 0x710, t0

  # Check init state in SPM
  li   t0, SPM_STATE_ADDR
  lw   t1, 0(t0)
  bnez t1, already_init       # Nonzero = skip copy

  ############################################
  # State 0: First run - copy SPM from DRAM
  ############################################

  # Compute descriptor address: DESCRIPTOR_BASE + (hartid-1) * 8
  li   t0, DESCRIPTOR_BASE
  addi t1, s0, -1
  slli t1, t1, 3              # * 8
  add  t0, t0, t1

  # Load descriptor fields
  lw   t1, 0(t0)              # spm_src (global addr)
  lw   t2, 4(t0)              # spm_size (bytes)

  # Copy from global memory (t1) to SPM (0x20000000)
  li   t3, 0x20000000
  add  t4, t3, t2
  beq  t2, zero, copy_done

copy_loop:
  lw   t5, 0(t1)
  sw   t5, 0(t3)
  addi t1, t1, 4
  addi t3, t3, 4
  blt  t3, t4, copy_loop

copy_done:
  # Mark state = 1 (copy complete) - set AFTER copy so it persists
  li   t0, SPM_STATE_ADDR
  li   t1, 1
  sw   t1, 0(t0)

  # Leader: init counter = 1 (count self)
  li   t0, 1
  bne  s0, t0, worker_send

  li   t0, SPM_COUNTER_ADDR
  li   t1, 1
  sw   t1, 0(t0)
  wfi                         # Wait for init_done events (or restart)

worker_send:
  # Worker: send init_done to PU1 (tag=0, dst=1)
  mv   a0, s0                 # payload = hartid
  li   x3, 1                  # dst=1, tag=0
  .word 0x0035500b            # send a0 to x3

  # Mark state = 2 (AM sent)
  li   t0, SPM_STATE_ADDR
  li   t1, 2
  sw   t1, 0(t0)
  wfi                         # Restart -> already_init -> worker_spin

  ############################################
  # Already initialized - check what to do
  ############################################
already_init:
  li   t0, 1
  bne  s0, t0, worker_spin    # Workers just spin

  # Leader: reload counter from SPM and check
  li   t0, SPM_COUNTER_ADDR
  lw   s1, 0(t0)
  li   t0, NUM_PU
  bge  s1, t0, all_init_done  # All done!
  wfi                         # Wait for more events (or restart)

worker_spin:
  wfi                         # Restart -> already_init -> worker_spin

  ############################################
  # Handler: init_done (tag 0, 1 arg)
  # Invoked on PU1 when a worker sends done
  ############################################
init_done_handler:
  li   t0, SPM_COUNTER_ADDR
  lw   s1, 0(t0)
  addi s1, s1, 1
  sw   s1, 0(t0)              # Persist in SPM
  li   t0, NUM_PU
  bge  s1, t0, all_init_done
  wfi                         # Wait for more (or restart)

  ############################################
  # All PUs done - start timer and terminate
  ############################################
all_init_done:
  # Start timer
  li   t0, 0x40000004
  li   t1, 1
  sw   t1, 0(t0)

  # Stop timer (measure just the overhead for now)
  sw   zero, 0(t0)

  # End simulation
  li   t0, 0x40000000
  sw   s1, 0(t0)              # result = done count
  wfi                         # Harmless restart (counter persists >= NUM_PU)

.park:
  wfi
  j    .park
