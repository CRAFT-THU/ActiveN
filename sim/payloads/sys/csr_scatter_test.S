  # CSR scatter broadcast test
  #
  # Tests the CSR-aware memory access / broadcast mechanism:
  #   1. All PUs configure spike handler (tag=1, argcnt=2) and pong handler (tag=0, argcnt=1)
  #   2. Workers send "ready" to PU1 (leader)
  #   3. PU1 waits for all ready messages, then triggers scatter
  #      (sends AM to MC with CSR data base/end addresses, tag=0xFF02)
  #   4. MC reads CSR data from DRAM, broadcasts to all PUs in zone
  #   5. Each PU's BIU filters entries matching its hartid, dispatches spike handler
  #   6. Spike handler sends pong (tag=0) to PU1
  #   7. PU1 counts pongs: when all 16 received, reports result and stops
  #
  # CSR data placed at CSR_DATA_OFFSET in binary (loaded by TEXT at 0x80000000):
  #   16 entries × 8 bytes each = 128 bytes (4 × 32-byte beats)
  #   Each entry: word0 = (pu_id << 16) | neuron_index, word1 = weight (f32)
  #   We use neuron_index=0 and weight=0x3F800000 (1.0f) for all entries.
  #
  # WFI-restart model: persistent state in SPM.
  #
  # AM instruction encodings:
  #   queue.2 a0, a1:  .word 0x00B5200B
  #   send    a2:      .word 0x00C0400B  (funct3=100, rs2=x12, rs1=x0)
  #   send.1  a0, a1:  .word 0x00B5500B  (funct3=101, rs2=x11, rs1=x10)

#ifndef NUM_PU
#define NUM_PU 16
#endif

#define SPM_STATE_ADDR  0x20003FF8
#define SPM_COUNTER     0x20003FF4

#define STOP_ADDR       0x40000000
#define TIMER_ADDR      0x40000004

#define MC_DST          0x8001
#define SCATTER_TAG     0xFF02

  # CSR data is placed at offset 0x1000 from TEXT_BASE (0x80000000)
  # So absolute address = 0x80001000
#define CSR_DATA_BASE   0x80001000
#define CSR_DATA_END    (CSR_DATA_BASE + NUM_PU * 8)

  .section .text
  .globl _start

_start:
  # Read hartid
  csrr  s0, 0xF14          # s0 = hartid (1-based, persistent in s0)

  # Check SPM state (WFI-restart safe)
  li    t0, SPM_STATE_ADDR
  lw    t1, 0(t0)
  bnez  t1, initialized

  #------------------------------------------------------------
  # First run: configure handlers
  #------------------------------------------------------------

  # Handler 0 (tag=0): pong/ready receiver, argcnt=1
  la    a0, handler_pong
  csrw  0x700, a0
  li    a0, 1
  csrw  0x710, a0

  # Handler 1 (tag=1): spike handler, argcnt=2
  la    a0, handler_spike
  csrw  0x701, a0
  li    a0, 2
  csrw  0x711, a0

  # Mark initialized (state = 1 for workers, special for leader)
  li    a0, 1
  beq   s0, a0, leader_init

  #--- Worker init ---
  li    t1, 1
  li    t0, SPM_STATE_ADDR
  sw    t1, 0(t0)

  # Send "ready" to PU1: send.1 a0=hartid, a1=0x00000001 (dst=1, tag=0)
  mv    a0, s0
  li    a1, 1
  .word 0x00B5500B          # send.1 a0, a1
  j     do_wfi

leader_init:
  # PU1: Initialize counter and state
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t1, 1               # state = 1 (waiting for ready)
  li    t0, SPM_STATE_ADDR
  sw    t1, 0(t0)
  j     do_wfi

initialized:
  # Already configured, just WFI
do_wfi:
  wfi

  #============================================================
  # Handler: pong / ready (tag=0, 1 arg: a0=sender data)
  #============================================================
handler_pong:
  # Only PU1 handles this meaningfully
  li    t0, 1
  bne   s0, t0, do_wfi

  # PU1: increment counter
  li    t0, SPM_COUNTER
  lw    t1, 0(t0)
  addi  t1, t1, 1
  sw    t1, 0(t0)

  # Check phase: state=1 → waiting for ready; state=2 → waiting for pongs
  li    t0, SPM_STATE_ADDR
  lw    t2, 0(t0)

  li    t3, 1
  beq   t2, t3, check_ready_done
  li    t3, 2
  beq   t2, t3, check_pong_done
  j     do_wfi

check_ready_done:
  # Need NUM_PU - 1 ready messages
  li    t3, NUM_PU - 1
  bne   t1, t3, do_wfi

  # All workers ready! Clear counter, move to phase 2.
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t2, 2
  li    t0, SPM_STATE_ADDR
  sw    t2, 0(t0)

  # Start timer
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)

  # Trigger scatter: queue(base, end), send to MC with tag=0xFF02
  li    a0, CSR_DATA_BASE
  li    a1, CSR_DATA_END
  .word 0x00B5200B          # queue.2 a0(x10), a1(x11)
  lui   a2, SCATTER_TAG     # a2[31:12] = SCATTER_TAG << 0 ... wrong
  # We need a2 = (SCATTER_TAG << 16) | MC_DST = 0xFF028001
  # lui loads upper 20 bits: 0xFF028 << 12 = 0xFF028000
  # addi adds 1: 0xFF028001
  lui   a2, 0xFF028
  addi  a2, a2, 1
  .word 0x00C0400B          # send a2(x12)

  j     do_wfi

check_pong_done:
  # Need NUM_PU pong messages (all PUs send pong after receiving spike)
  li    t3, NUM_PU
  bne   t1, t3, do_wfi

  # All pongs received! Stop timer and report.
  li    t0, TIMER_ADDR
  sw    zero, 0(t0)

  # Write result to stop register
  li    t0, STOP_ADDR
  li    t1, NUM_PU
  sw    t1, 0(t0)
  j     do_wfi

  #============================================================
  # Handler: spike (tag=1, 2 args: a0=neuron_index, a1=weight)
  #============================================================
handler_spike:
  # Send pong to PU1: send.1 a0=hartid, a1=0x00000001 (dst=1, tag=0)
  mv    a0, s0              # our hartid (preserved in s0)
  li    a1, 1               # dst=PU1, tag=0
  .word 0x00B5500B          # send.1 a0, a1
  j     do_wfi

  #============================================================
  # CSR data section at offset 0x1000 (32-byte aligned)
  #============================================================
  .org 0x1000
csr_data:
  # 16 entries, one per PU (hartid 1..16)
  # Each entry: .word (hartid << 16 | neuron_index), .word weight
  # neuron_index = 0 for all, weight = 0x3F800000 (1.0f)
  .word 0x00010000, 0x3F800000   # PU 1
  .word 0x00020000, 0x3F800000   # PU 2
  .word 0x00030000, 0x3F800000   # PU 3
  .word 0x00040000, 0x3F800000   # PU 4
  .word 0x00050000, 0x3F800000   # PU 5
  .word 0x00060000, 0x3F800000   # PU 6
  .word 0x00070000, 0x3F800000   # PU 7
  .word 0x00080000, 0x3F800000   # PU 8
  .word 0x00090000, 0x3F800000   # PU 9
  .word 0x000A0000, 0x3F800000   # PU 10
  .word 0x000B0000, 0x3F800000   # PU 11
  .word 0x000C0000, 0x3F800000   # PU 12
  .word 0x000D0000, 0x3F800000   # PU 13
  .word 0x000E0000, 0x3F800000   # PU 14
  .word 0x000F0000, 0x3F800000   # PU 15
  .word 0x00100000, 0x3F800000   # PU 16
