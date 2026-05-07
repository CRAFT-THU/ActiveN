  # CSR scatter broadcast test — 64 PU, 2 MC
  #
  # Tests the CSR-aware memory access / broadcast mechanism with multiple MCs:
  #   1. All PUs configure spike handler (tag=1, argcnt=2) and report handler (tag=0, argcnt=1)
  #   2. PU1 (leader) immediately triggers scatter to BOTH MCs
  #   3. MC0 reads CSR0 from dram.0, broadcasts to PUs 1-32
  #      MC1 reads CSR1 from dram.1, broadcasts to PUs 33-64
  #   4. Each PU's BIU filters entries matching its hartid, dispatches spike handler
  #   5. Spike handler computes sub_idx * weight, sends result to PU1 (tag=0)
  #   6. PU1 collects all XOR values, compares global XOR against REF_RESULT
  #      Exit 0 on match, output wrong result + exit 1 on mismatch, exit 2 on bad config
  #
  # CSR entry format (8 bytes each, little-endian):
  #   word0 = (pu_id << 16) | sub_idx
  #   word1 = weight (32-bit)
  #   BIU delivers: a0=sub_idx, a1=weight to handler
  #
  # Memory layout:
  #   dram.0: [text including CSR0 data at .csr0]
  #   dram.1: [16-byte zero header | CSR1 data starting at offset 0x10]
  #
  # New ISA SEND format:
  #   .insn r 0x0B, 0x4, 0x0, rd, dst_reg, tag_reg   (blocking SEND)
  #   .insn r 0x0B, 0x5, 0x0, rd, dst_reg, tag_reg   (blocking SEND + yield)
  #   Payload always = {a0, a1, a2, a3}
  #
  # Scatter protocol (single flit to MC, tag[7:0] = 0x10):
  #   a0 = base address (MC-local DRAM address)
  #   a1 = (length_in_beats << 16) | response_handler_tag
  #   a2, a3 = carried data (passed through to receiving handlers)

#define NUM_PU          64
#define NUM_MC          2
#define PUS_PER_MC      32

#define SPM_BASE        0x20000000
#define SPM_SIZE        0x4000
#define SPM_XOR_ACC     (SPM_BASE + SPM_SIZE - 4)    /* local XOR accumulator */
#define SPM_COUNTER     (SPM_BASE + SPM_SIZE - 8)    /* message counter */
#define SPM_SPIKE_CNT   (SPM_BASE + SPM_SIZE - 12)   /* spike counter for this PU */

#define STOP_ADDR       0x40000000
#define TIMER_ADDR      0x40000004
#define RESULT_ADDR     0x4000000C
#define CONFIG_ROM_ADDR 0x68000000

#define MC0_DST         0x8001
#define MC1_DST         0x8002
#define SCATTER_TAG     0x010

  /* CSR0: located at csr0 symbol in this binary (dram.0). Address = symbol offset. */
  /* CSR1: located at offset 0x20 in dram.1 (after 32-byte zero header). */
#define CSR1_BASE       0x20
#define CSR_ENTRIES     64
#define CSR_BEATS       ((CSR_ENTRIES * 8) / 32)  /* 64*8/32 = 16 beats */

  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .macro YIELD dst_reg, tag_reg
  .insn r 0x0B, 0x5, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .section .text
  .globl _start

_start:
  bnez  x10, thread1_init
  csrr  s0, 0xF14             # s0 = hartid (1-based)

  # --- Check config ROM: must be 64 PU, 2 MC ---
  li    t0, CONFIG_ROM_ADDR
  lw    t1, 0(t0)            # word0 = numPU
  li    t2, NUM_PU
  bne   t1, t2, bad_config
  lw    t1, 8(t0)            # word2 = numMC
  li    t2, NUM_MC
  bne   t1, t2, bad_config

  #------------------------------------------------------------
  # Initialize SPM, then configure handlers
  #------------------------------------------------------------

  # Initialize SPM counters
  li    t0, SPM_XOR_ACC
  sw    zero, 0(t0)
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t0, SPM_SPIKE_CNT
  sw    zero, 0(t0)

  # Handler 0 (tag=0): report handler, argcnt=1
  la    a0, handler_report
  csrw  0x700, a0             # address
  csrw  0x720, zero           # hanmargin: quota=0, margin=0
  li    a0, (3 << 3) | 1      # all threads, argcnt=1
  csrw  0x710, a0             # hancfg (enables handler)

  # Handler 1 (tag=1): spike handler, argcnt=2
  la    a0, handler_spike
  csrw  0x701, a0             # address
  csrw  0x721, zero           # hanmargin: quota=0, margin=0
  li    a0, (3 << 3) | 2      # all threads, argcnt=2
  csrw  0x711, a0             # hancfg (enables handler)

  li    a0, 1
  bne   s0, a0, do_wfi       # workers: just WFI after arming handlers

  #--- Leader (PU1) triggers scatter immediately ---

  # Start timer
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)

  # --- Trigger scatter to MC0 ---
  la    a0, csr0              # CPU address of CSR0 in dram.0
  li    t2, 0x80000000
  sub   a0, a0, t2            # Convert to MC-local file offset
  li    a1, (CSR_BEATS << 16) | 1    # 16 beats, response handler tag=1
  li    t0, MC0_DST
  li    t1, SCATTER_TAG
  SEND  t0, t1

  # --- Trigger scatter to MC1 ---
  li    a0, CSR1_BASE         # MC-local base address in dram.1
  li    a1, (CSR_BEATS << 16) | 1    # 16 beats, response handler tag=1
  li    t0, MC1_DST
  li    t1, SCATTER_TAG
  SEND  t0, t1

  j     do_wfi

do_wfi:
  wfi

bad_config:
  li    t0, STOP_ADDR
  li    t1, 2
  sw    t1, 0(t0)
  j     do_wfi

  #============================================================
  # Handler: report (tag=0, 1 arg: a0=XOR value from a PU)
  # Accumulates XOR, counts reports. When all NUM_PU arrive, check.
  #============================================================
handler_report:
  li    t0, 1
  bne   s0, t0, do_wfi       # only PU1 processes reports

  # Accumulate XOR
  li    t0, SPM_XOR_ACC
  amoxor.w zero, a0, (t0)

  # Atomically increment counter
  li    t0, SPM_COUNTER
  li    t2, 1
  amoadd.w t1, t2, (t0)      # t1 = old counter
  addi  t1, t1, 1            # t1 = new counter

  li    t3, NUM_PU
  bne   t1, t3, do_wfi

  # All PUs reported! Stop timer.
  li    t0, TIMER_ADDR
  sw    zero, 0(t0)

  # Compare accumulated XOR against reference
  li    t0, SPM_XOR_ACC
  lw    t1, 0(t0)            # t1 = global XOR from all PU reports
  la    t0, ref_result
  lw    t2, 0(t0)            # t2 = expected
  bne   t1, t2, xor_mismatch

  # Match: exit success (0)
  li    t0, STOP_ADDR
  sw    zero, 0(t0)
  j     do_wfi

xor_mismatch:
  # Output wrong result, exit 1
  li    t0, RESULT_ADDR
  sw    t1, 0(t0)
  li    t0, STOP_ADDR
  li    t1, 1
  sw    t1, 0(t0)
  j     do_wfi

  #============================================================
  # Handler: spike (tag=1, 2 args: a0=sub_idx, a1=weight)
  # Computes sub_idx * weight, XORs into local accumulator.
  # After receiving 1 spike, sends XOR report to PU1.
  # (Each PU receives exactly 1 spike: 1 matching entry per reachable MC.)
  #============================================================
handler_spike:
  mul   t0, a0, a1            # t0 = sub_idx * weight (low 32 bits)

  # Atomically increment spike counter
  li    t1, SPM_SPIKE_CNT
  li    t4, 1
  amoadd.w t3, t4, (t1)      # t3 = old spike count
  addi  t3, t3, 1            # t3 = new spike count

  # After 1 spike, send XOR report to PU1
  li    t4, 1
  bne   t3, t4, do_wfi

  # Send XOR report: dst=1, tag=0, a0=sub_idx*weight
  mv    a0, t0
  li    t0, 1                 # dst = PU1
  li    t1, 0                 # tag = 0
  YIELD t0, t1
  j     do_wfi

# Thread 1: set up persistent registers, then WFI
thread1_init:
  csrr  s0, 0xF14             # s0 = hartid (shared with thread 0's handlers)
  wfi

  #============================================================
  # Data sections
  #============================================================

  # Reference result
  .balign 4
ref_result:
#include "csr_scatter_data.S"
  REF_RESULT

  # CSR0 data (embedded in dram.0 binary)
  CSR0

