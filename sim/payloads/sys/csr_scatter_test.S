  # Parameterized CSR scatter broadcast test.
  #
  # Tests the CSR-aware memory access / broadcast mechanism with multiple MCs:
  #   1. All PUs configure a spike handler (tag=1, argcnt=2) and a report
  #      handler (tag=0, argcnt=1).
  #   2. PU1 triggers one scatter request to every MC.
  #   3. Every MC reads the common CSR table and broadcasts it into its PU zone.
  #   4. Each PU's BIU filters entries matching its hartid and dispatches the
  #      spike handler exactly once.
  #   5. The spike handler computes sub_idx * weight and reports it to PU1.
  #   6. PU1 XORs all reports and compares the result against REF_RESULT.
  #      Exit 0 indicates a match, exit 1 an XOR mismatch, exit 2 a bad system
  #      shape, and exit 3 a duplicate spike delivery.
  #
  # CSR entry format (8 bytes each, little-endian):
  #   word0 = (sub_idx << 16) | pu_id
  #   word1 = weight
  #   BIU delivers a0=sub_idx and a1=weight to the handler.
  #
  # Every MC stores the same table containing one entry per system PU. A MemIf
  # broadcasts only into its own PU zone, so every PU receives exactly one
  # matching entry while entries for other zones are filtered out.
  #
  # Memory layout:
  #   dram.0: executable followed by an aligned common CSR table
  #   dram.N: one empty memory line followed by the same aligned table
  #
  # SEND format:
  #   .insn r 0x0B, 0x4, 0x0, rd, dst_reg, tag_reg   (blocking SEND)
  #   .insn r 0x0B, 0x5, 0x0, rd, dst_reg, tag_reg   (blocking SEND + yield)
  #   Payload is always {a0, a1, a2, a3}.
  #
  # Scatter protocol (single flit to an MC, tag[7:0] = 0x10):
  #   a0 = MC-local base address
  #   a1 = (length_in_lines << 16) | response_handler_tag
  #   a2, a3 = carried data passed through to receiving handlers

#ifndef NUM_PU
#define NUM_PU 64
#endif
#ifndef NUM_MC
#define NUM_MC 2
#endif
#ifndef MEM_LINE_BYTES
#define MEM_LINE_BYTES 64
#endif

#define SPM_BASE          0x20000000
#define SPM_SIZE          0x4000
#define SPM_XOR_ACC       (SPM_BASE + SPM_SIZE - 4)
#define SPM_REPORT_COUNT  (SPM_BASE + SPM_SIZE - 8)
#define SPM_SPIKE_COUNT   (SPM_BASE + SPM_SIZE - 12)

#define STOP_ADDR         0x40000000
#define TIMER_ADDR        0x40000004
#define RESULT_ADDR       0x4000000C
#define CONFIG_ROM_ADDR   0x68000000

#define MC_DST_BASE       0x8001
#define SCATTER_TAG       0x010
#define CSR_BYTES         (NUM_PU * 8)

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

  # Check the target shape and retain the runtime memory-line shift in s1.
  li    t0, CONFIG_ROM_ADDR
  lw    t1, 0(t0)             # number of PUs
  li    t2, NUM_PU
  bne   t1, t2, bad_config
  lw    t1, 8(t0)             # number of MCs
  li    t2, NUM_MC
  bne   t1, t2, bad_config
  lw    s1, 24(t0)            # log2(memory-line bytes)

  # Initialize the per-PU result state before enabling handlers.
  li    t0, SPM_XOR_ACC
  sw    zero, 0(t0)
  li    t0, SPM_REPORT_COUNT
  sw    zero, 0(t0)
  li    t0, SPM_SPIKE_COUNT
  sw    zero, 0(t0)

  # Handler 0: result report, all threads, argcnt=1.
  la    a0, handler_report
  csrw  0x700, a0             # address
  csrw  0x720, zero           # quota=0, margin=0
  li    a0, (3 << 3) | 1
  csrw  0x710, a0             # enable handler

  # Handler 1: delivered CSR entry, all threads, argcnt=2.
  la    a0, handler_spike
  csrw  0x701, a0             # address
  csrw  0x721, zero           # quota=0, margin=0
  li    a0, (3 << 3) | 2
  csrw  0x711, a0             # enable handler

  li    a0, 1
  bne   s0, a0, do_wfi       # workers wait after arming handlers

  # PU1 can issue immediately: disabled handlers still buffer ingress events.
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)

  # MC0's table is embedded after the executable.
  la    a0, csr_data
  li    t2, 0x80000000
  sub   a0, a0, t2            # convert CPU address to MC-local file offset
  li    t1, CSR_BYTES
  srl   t1, t1, s1            # byte count to memory-line count
  slli  a1, t1, 16
  ori   a1, a1, 1             # response handler tag=1
  li    t0, MC_DST_BASE
  li    t1, SCATTER_TAG
  SEND  t0, t1

  # Every secondary image stores its table after one empty memory line.
  li    t3, 1                 # MC index
  li    t4, MC_DST_BASE + 1   # destination of MC1
  li    t5, NUM_MC
scatter_mc_loop:
  bge   t3, t5, do_wfi
  li    a0, 1
  sll   a0, a0, s1            # one runtime-sized memory line
  li    t1, CSR_BYTES
  srl   t1, t1, s1
  slli  a1, t1, 16
  ori   a1, a1, 1             # response handler tag=1
  li    t1, SCATTER_TAG
  SEND  t4, t1
  addi  t3, t3, 1
  addi  t4, t4, 1
  j     scatter_mc_loop

do_wfi:
  wfi

bad_config:
  li    t0, STOP_ADDR
  li    t1, 2
  sw    t1, 0(t0)
  j     do_wfi

  # Report handler (tag=0, one argument).
  # PU1 accumulates one XOR result from every PU and validates the final value.
handler_report:
  li    t0, 1
  bne   s0, t0, do_wfi       # only PU1 processes reports

  # Accumulate the report and atomically count it.
  li    t0, SPM_XOR_ACC
  amoxor.w zero, a0, (t0)
  li    t0, SPM_REPORT_COUNT
  li    t2, 1
  amoadd.w t1, t2, (t0)      # t1 = old report count
  addi  t1, t1, 1            # t1 = new report count
  li    t3, NUM_PU
  bne   t1, t3, do_wfi

  # All PUs reported, so stop the timer and compare the accumulated XOR.
  li    t0, TIMER_ADDR
  sw    zero, 0(t0)
  li    t0, SPM_XOR_ACC
  lw    t1, 0(t0)
  la    t0, ref_result
  lw    t2, 0(t0)
  bne   t1, t2, xor_mismatch

  li    t0, STOP_ADDR
  sw    zero, 0(t0)
  j     do_wfi

xor_mismatch:
  li    t0, RESULT_ADDR
  sw    t1, 0(t0)
  li    t0, STOP_ADDR
  li    t1, 1
  sw    t1, 0(t0)
  j     do_wfi

  # Spike handler (tag=1, two arguments).
  # Compute sub_idx * weight and report it to PU1. Any second delivery to the
  # same PU is a test failure rather than an event to ignore silently.
handler_spike:
  mul   t0, a0, a1            # low 32 bits of sub_idx * weight
  li    t1, SPM_SPIKE_COUNT
  li    t4, 1
  amoadd.w t3, t4, (t1)      # t3 = old spike count
  bnez  t3, duplicate_spike

  mv    a0, t0
  li    t0, 1                 # destination PU1
  li    t1, 0                 # report handler tag
  YIELD t0, t1
  j     do_wfi

duplicate_spike:
  li    t0, RESULT_ADDR
  li    t1, 0xBAD5
  sw    t1, 0(t0)
  li    t0, STOP_ADDR
  li    t1, 3
  sw    t1, 0(t0)
  j     do_wfi

# Thread 1 sets up the persistent hartid register before waiting for handlers.
thread1_init:
  csrr  s0, 0xF14
  wfi

  # Reference result followed by the MC0 copy of the common CSR table.
  .balign 4
ref_result:
#include "csr_scatter_data.S"
  REF_RESULT

  CSR_DATA
