/* Sparse trace-STDP: fixed post groups, compact forward CSR, four reverse ABIs.
 * Continuations store cursors in SPM; input handlers preserve immutable root
 * constants. Weight updates wait for complete feedforward/neuron processing.
 */
#if MNIST_SPARSE_MODE < 1 || MNIST_SPARSE_MODE > 4
#error "MNIST_SPARSE_MODE must be 1, 2, 3 or 4"
#endif
#if MNIST_RECURRENT_MODE != 1 || MNIST_TRACE_BUNDLE != 0
#error "Sparse mode requires tree recurrence and unbundled traces"
#endif
#if MNIST_FIFTH_ROOT_LUT
#error "Sparse SPM ABI currently supports the bit seed only"
#endif
#ifndef MNIST_SPARSE_FUSE_FORWARD
#define MNIST_SPARSE_FUSE_FORWARD 1
#endif
#if MNIST_SPARSE_FUSE_FORWARD != 0 && MNIST_SPARSE_FUSE_FORWARD != 1
#error "MNIST_SPARSE_FUSE_FORWARD must be 0 or 1"
#endif
#ifndef MNIST_SPARSE_COMPACT_SCHEDULE
#define MNIST_SPARSE_COMPACT_SCHEDULE 1
#endif
#ifndef MNIST_SPARSE_FUSE_NEURON
#define MNIST_SPARSE_FUSE_NEURON (MNIST_SPARSE_COMPACT_SCHEDULE && MNIST_RULE != 1)
#endif
#if MNIST_SPARSE_FUSE_NEURON && !MNIST_SPARSE_COMPACT_SCHEDULE
#error "Neuron fusion requires compact scheduling"
#endif
#ifndef MNIST_SPARSE_BINARY_REVERSE
#define MNIST_SPARSE_BINARY_REVERSE 2
#endif
#ifndef MNIST_SPARSE_ACTIVE_PREFIX
#define MNIST_SPARSE_ACTIVE_PREFIX 0
#endif
#ifndef MNIST_SPARSE_CACHE_REVERSE
#define MNIST_SPARSE_CACHE_REVERSE MNIST_SPARSE_COMPACT_SCHEDULE
#endif
#if MNIST_SPARSE_CACHE_REVERSE && !MNIST_SPARSE_COMPACT_SCHEDULE
#error "Reverse phase caching requires compact scheduling"
#endif
#ifndef MNIST_SPARSE_RELEVANT_MASKS
#define MNIST_SPARSE_RELEVANT_MASKS 0
#endif
#ifndef MNIST_SPARSE_MC_DRAIN
#define MNIST_SPARSE_MC_DRAIN 0
#endif
#ifndef MNIST_SPARSE_IDLE_GATE
#define MNIST_SPARSE_IDLE_GATE 256
#endif
#if MNIST_SPARSE_IDLE_GATE < 0 || MNIST_SPARSE_IDLE_GATE >= 65535
#error "Idle gate must be in 0..65534"
#endif
#if MNIST_SPARSE_MC_DRAIN && (MNIST_SPARSE_MODE != 2 && MNIST_SPARSE_MODE != 3)
#error "MC drain requires a scatter mode"
#endif
#if MNIST_SPARSE_RELEVANT_MASKS && (MNIST_SPARSE_MODE == 2 || MNIST_SPARSE_MODE == 3) && !MNIST_SPARSE_MC_DRAIN
#error "Relevant MC masks require MC drain completion"
#endif
#if (MNIST_SPARSE_RELEVANT_MASKS || MNIST_SPARSE_MC_DRAIN) && (!MNIST_SPARSE_CACHE_REVERSE || MNIST_SPARSE_ACTIVE_PREFIX)
#error "Mask/drain experiments require reverse caching and no active-prefix partition"
#endif
/* Experimental scratch: 32 bytes above the initialized-data limit, below
 * the startup stack at 0x3800. No synapse-index directory is added.
 * R: two per-post u64 PU masks, or two per-post u32 MC masks at stride 8.
 * T: the input PU's u64 set of posts reached by its fired inputs.
 */
#define X_BASE 0x20003000
#define X_T_LO 16
#define X_T_HI 20
#define X_ISSUED 24
#define X_JOIN 28
#define TAG_ISSUED 0x400
#define TAG_IDLE_CONTROL -1023 /* 0xc01, bounded notifier installation token */
#define TAG_DRAIN -1010        /* 0xc0e, domain-local reverse chain */
#if MNIST_SPARSE_CACHE_REVERSE && MNIST_SPARSE_MODE == 4
#define FIRED_SIZE 8
#define FIRED_SHIFT 3
#else
#define FIRED_SIZE 4
#define FIRED_SHIFT 2
#endif
#if MNIST_SPARSE_COMPACT_SCHEDULE && !MNIST_SPARSE_FUSE_FORWARD
#error "Compact scheduling requires fused preparation/feedforward"
#endif
#define MT_RULE_ONE 1
#define MT_RULE_TWO 2
#define MT_RULE_THREE 3
#define SPM_BASE 0x20000000
#define STOP_ADDR 0x40000000
#define TIMER_ADDR 0x40000004
#define PRINT_ADDR 0x4000000c
#include "spike_count.h"
#define INPUT_SIZE 28
#define EXC_SIZE 44
#define INH_SIZE 20
#define IN_GLOBAL 0
#define IN_THRESHOLD 4
#define IN_PRE_TRACE 8
#define IN_RNG 12
#define IN_FLAGS 16
#define IN_WEIGHT_OFFSET 20
#define IN_WEIGHT_COUNT 24
#define EXC_GE 0
#define EXC_GI 4
#define EXC_V 8
#define EXC_THETA 12
#define EXC_POST1 16
#define EXC_POST2 20
#define EXC_REFRACTORY 24
#define EXC_FLAGS 28
#define EXC_SLOT 32
#define EXC_DESC 36
#define EXC_DESCS 40
#define INH_GE 0
#define INH_GI 4
#define INH_V 8
#define INH_REFRACTORY 12
#define INH_FLAGS 16
#define NEURON_FIRED 1
#define NEURON_FF_INPUT 2
#define PH_PREPARE 1
#define PH_FORWARD 2
#define PH_NEURON 3
#define PH_REVERSE 4
#define PH_FINAL 5
/* Work(3) -> FF/reverse(2) -> ack/control(1). Scatter requests use priority
 * 2, and their receivers can emit only priority-1 phase completion.
 * Binomial and group traffic is bounded by the current phase.
 */
#define TAG_FF -2046
#define TAG_REVERSE -2044
#define TAG_POST -2043
#define TAG_ACK 0x406
#define TAG_PHASE 0x407
#define TAG_FF_WORK -1016
#define TAG_REDUCE 0x409
#define TAG_START 0x40a
#define TAG_GROUP 0x40b
#define TAG_MASK 0x40c
#define TAG_REV_WORK -1011
#define TAG_GROUP_WORK -1010
#define TAG_SCATTER -2032
#include "mnist_sparse_layout.inc"
#include "mnist_numeric.inc"
#if MNIST_RULE != MT_RULE_THREE
#include "mnist_fifth_root_config.inc"
#endif
.macro INSTALL index, entry, threads, argc, quota, margin
  la t0, \entry
  csrw (0x700 + \index), t0
  li t0, ((\quota << 16) | \margin)
  csrw (0x720 + \index), t0
  li t0, ((\threads << 3) | \argc)
  csrw (0x710 + \index), t0
.endm
/* Input-only registers, installed before the neuron/group barrier completes.
 * ra/tp and the three-trace cache registers are untouched by input control,
 * preparation, and reduction handlers. One/two-trace s3/s9/s10 remain live
 * throughout reverse handling; fifth_root preserves them.
 */
.macro INIT_REVERSE_PHASE
  lw ra, H_REVERSE_PTR(gp)
  add ra, ra, gp
  lw tp, H_WMAX_BASE(gp)
  add tp, tp, gp
#if MNIST_RULE == MT_RULE_THREE
  lw a4, H_FIRED_BASE(gp)
  add a4, a4, gp
  lw a5, H_FIRED_END(gp)
  lw a6, H_INPUT_BASE(gp)
  add a6, a6, gp
  addi sp, gp, H_RECEIVED
  lw s11, H_EXPECTED(gp)
#if MNIST_SPARSE_MODE == 4
  lw a7, H_REV_CURSOR(gp)
#endif
#else
  lw s3, H_FIRED_END(gp)
  lw s9, H_POT(gp)
  lw s10, H_PRE_TARGET(gp)
#endif
.endm
  .section .text
  .option nopic
  .globl _start
_start:
  bnez a0, sparse_wait_thread1
  li sp, SPM_BASE + 0x3800
  call snn_init
  li gp, SPM_BASE
  SPIKE_COUNT_INIT (SPM_BASE + SPIKE_COUNT_MNIST), t0
  lw t0, H_MAGIC(gp)
  li t1, 0x4d535034
  bne t0, t1, sparse_fail
  lw t0, H_MODE(gp)
  li t1, MNIST_SPARSE_MODE
  bne t0, t1, sparse_fail
  lw t0, H_RULE(gp)
  li t1, MNIST_RULE
  bne t0, t1, sparse_fail
#if MNIST_SPARSE_RELEVANT_MASKS || MNIST_SPARSE_MC_DRAIN
  li t0, X_BASE
  sw zero, 0(t0)
  sw zero, 4(t0)
  sw zero, 8(t0)
  sw zero, 12(t0)
  sw zero, X_T_LO(t0)
  sw zero, X_T_HI(t0)
  sw zero, X_ISSUED(t0)
  sw zero, X_JOIN(t0)
#endif
#if MNIST_SPARSE_MC_DRAIN
  INSTALL 0, sparse_issued, 1, 1, 2, 2
  INSTALL 1, sparse_idle_control, 1, 1, 2, 2
#endif
  INSTALL 2, sparse_ff_receive, 1, 3, 1, 1
  INSTALL 3, sparse_root, 1, 4, 1, 1
  INSTALL 4, sparse_reverse_quiet, 3, 4, 1, 1
  INSTALL 5, sparse_reverse_post, 3, 4, 1, 1
  INSTALL 6, sparse_ff_ack, 1, 0, 1, 1
  INSTALL 7, sparse_phase, 1, 2, 1, 1
  INSTALL 8, sparse_ff_send, 1, 0, 1, 2
  INSTALL 9, sparse_reduce, 1, 4, 1, 1
  INSTALL 10, sparse_start, 1, 2, 10, 10
  INSTALL 11, sparse_group, 1, 4, 1, 1
  INSTALL 12, sparse_mask, 1, 4, 1, 1
#if MNIST_SPARSE_MC_DRAIN
  /* Reverse commit emits one issuer contribution. Drain work emits a chain
   * token plus a local join (input), or a completion reduction (neuron).
   * Reserve two further slots for higher-priority idle/control work.
   */
  INSTALL 13, sparse_reverse_send, 1, 0, 1, 3
  INSTALL 14, sparse_group_send, 1, 0, 2, 4
#else
  INSTALL 13, sparse_reverse_send, 1, 0, 1, 2
  INSTALL 14, sparse_group_send, 1, 0, 1, 2
#endif
  INSTALL 15, sparse_setup_thread1, 2, 2, 1, 1
#if MNIST_SPARSE_COMPACT_SCHEDULE && MNIST_RULE != MT_RULE_THREE
  /* Input handlers preserve sp, s11 and a4-a7 throughout the runtime. */
  INIT_FIFTH_ROOT
#endif
  li t0, 15
  SEND zero, t0
  j sparse_wait
sparse_wait_thread1:
  wfi
  j sparse_wait_thread1
sparse_setup_thread1:
  li gp, SPM_BASE
#if MNIST_SPARSE_MC_DRAIN
  li t0, -2
  beq a1, t0, sparse_drain_thread1
#endif
#if MNIST_SPARSE_COMPACT_SCHEDULE
#if MNIST_SPARSE_CACHE_REVERSE
  bltz a1, sparse_reverse_setup_thread1
#endif
  lw t0, H_PHASE(gp)
  bnez t0, sparse_prepare_worker
#endif
#if MNIST_SPARSE_COMPACT_SCHEDULE && MNIST_RULE != MT_RULE_THREE
  INIT_FIFTH_ROOT
#endif
  li a0, -1
  li a1, 0
  li a2, 0
  li a3, 0
  li t0, 1
  SENDI t0, TAG_GROUP
#if MNIST_SPARSE_CACHE_REVERSE
  j sparse_wait
sparse_reverse_setup_thread1:
  INIT_REVERSE_PHASE
  /* Both caches are ready before this PU contributes to the existing barrier.
   * This uses the existing thread-zero reducer and one reserved outgoing slot.
   */
  lw a0, H_PHASE(gp)
  li a1, 1
  li a2, 0
  li a3, 0
  YIELDI zero, TAG_REDUCE
#endif
sparse_wait:
  wfi
  j sparse_wait
sparse_fail:
  li t0, PRINT_ADDR
  csrr t1, 0xf14
  sw t1, 0(t0)
  lw t1, H_PHASE(gp)
  sw t1, 0(t0)
  li t0, STOP_ADDR
  li t1, 1
  sw t1, 0(t0)
  j sparse_wait

/* Start locally before sending children; reverse completion may race with
 * this handler on thread 1. Reset DONE before publishing the phase.
 */
sparse_start:
  li t0, SPIKE_COUNT_REPORT
  beq a0, t0, sparse_report_start
  li t0, SPIKE_COUNT_START
  bne a1, t0, sparse_start_marker_done
  SPIKE_COUNT_RESET (SPM_BASE + SPIKE_COUNT_MNIST), t0, t1
  li t0, SPM_BASE + SPIKE_COUNT_MNIST
  lw t1, SPIKE_COUNT_STATE(t0)
  ori t1, t1, SPIKE_COUNT_STATE_START_PENDING
  sw t1, SPIKE_COUNT_STATE(t0)
  li a1, 0
sparse_start_marker_done:
#if MNIST_SPARSE_MC_DRAIN
  /* The preceding global reverse reduction joins both threads everywhere.
   * Restore receive enables before any new reverse responses can be issued.
   */
  li t0, PH_REVERSE
  bne a0, t0, 1f
  li t0, X_BASE
  sw zero, X_JOIN(t0)
  li t0, (3 << 3) | 4
  csrw 0x714, t0
  csrw 0x715, t0
1:
#endif
  sw zero, H_DONE(gp)
  sw zero, H_REDUCE_COUNT(gp)
  sw zero, H_REDUCE_VALUE(gp)
  sw zero, H_CHECKSUM(gp)
  sw a1, H_LOCAL_INH(gp)
  sw a0, H_PHASE(gp)
  lw t0, H_TREE_SIZE(gp)
  srli t0, t0, 1
  lw t1, H_ID(gp)
  beqz t0, sparse_start_local
sparse_start_children:
  add t2, t1, t0
  li t3, SPM_BASE + SPIKE_COUNT_MNIST
  lw t4, SPIKE_COUNT_STATE(t3)
  andi t4, t4, SPIKE_COUNT_STATE_START_PENDING
  beqz t4, sparse_start_child_normal
  li a1, SPIKE_COUNT_START
sparse_start_child_normal:
  SENDI t2, TAG_START
  srli t0, t0, 1
  bnez t0, sparse_start_children
sparse_start_local:
  li a1, 0
  li t0, SPM_BASE + SPIKE_COUNT_MNIST
  lw t1, SPIKE_COUNT_STATE(t0)
  li t2, -1
  xori t2, t2, SPIKE_COUNT_STATE_START_PENDING
  and t1, t1, t2
  sw t1, SPIKE_COUNT_STATE(t0)
  YIELDI zero, TAG_PHASE
  j sparse_wait

sparse_report_start:
  lw t1, H_ID(gp)
  lw t0, H_TREE_SIZE(gp)
  srli t0, t0, 1
sparse_report_children:
  beqz t0, sparse_report_local
  add t2, t1, t0
  li a0, SPIKE_COUNT_REPORT
  li a1, 0
  li a2, 0
  li a3, 0
  SENDI t2, TAG_START
  srli t0, t0, 1
  j sparse_report_children
sparse_report_local:
  SPIKE_COUNT_EMIT (SPM_BASE + SPIKE_COUNT_MNIST), t0, t1, t2
  li t0, SPM_BASE + SPIKE_COUNT_MNIST
  li t1, 1
  sw t1, SPIKE_COUNT_ACK(t0)
  lw t0, H_TREE_PARENT(gp)
  beqz t0, sparse_report_root_wait
  li a0, SPIKE_COUNT_ACK_PHASE
  li a1, 1
  li a2, 0
  li a3, 0
  SENDI t0, TAG_REDUCE
sparse_report_root_wait:
  j sparse_wait

/* Reduction input: phase, subtree size, additive value, and terminal XOR. */
sparse_reduce_local:
  lw a0, H_PHASE(gp)
  li a1, 1
sparse_reduce:
  li t0, SPIKE_COUNT_ACK_PHASE
  beq a0, t0, sparse_count_ack
  lw t0, H_PHASE(gp)
  bne a0, t0, sparse_fail
  lw t0, H_REDUCE_COUNT(gp)
  add t0, t0, a1
  lw t1, H_TREE_SIZE(gp)
  bgtu t0, t1, sparse_fail
  sw t0, H_REDUCE_COUNT(gp)
  lw t2, H_REDUCE_VALUE(gp)
  add t2, t2, a2
  sw t2, H_REDUCE_VALUE(gp)
  lw t3, H_CHECKSUM(gp)
  xor t3, t3, a3
sparse_reduce_store:
  sw t3, H_CHECKSUM(gp)
  bne t0, t1, sparse_wait
  mv a1, t1
  mv a2, t2
  mv a3, t3
  lw t0, H_TREE_PARENT(gp)
  beqz t0, sparse_reduce_root
  SENDI t0, TAG_REDUCE
  j sparse_wait
sparse_reduce_root:
  YIELDI zero, 0x403
  j sparse_wait

sparse_count_ack:
  li t0, SPM_BASE + SPIKE_COUNT_MNIST
  lw t1, SPIKE_COUNT_ACK(t0)
  add t1, t1, a1
  sw t1, SPIKE_COUNT_ACK(t0)
  lw t2, H_TREE_SIZE(gp)
  bne t1, t2, sparse_wait
  lw t3, H_TREE_PARENT(gp)
  beqz t3, sparse_count_ack_root
  mv a1, t2
  li a0, SPIKE_COUNT_ACK_PHASE
  li a2, 0
  li a3, 0
  SENDI t3, TAG_REDUCE
  j sparse_wait
sparse_count_ack_root:
  li t0, STOP_ADDR
  sw zero, 0(t0)
  j sparse_wait
sparse_root:
#if MNIST_SPARSE_FUSE_NEURON
  li t0, PH_FORWARD
  beq a0, t0, sparse_root_reverse
#endif
  li t0, PH_REVERSE
  beq a0, t0, sparse_step_done
  li t0, PH_FINAL
  beq a0, t0, sparse_final_done
  li t0, PH_PREPARE
  bne a0, t0, sparse_root_next_phase
#if !MNIST_SPARSE_FUSE_FORWARD
  bnez a2, sparse_root_next_phase
  /* No input fired: there can be no outstanding feedforward delivery. */
#else
  /* Preparation includes every source's acknowledged feedforward delivery. */
#endif
#if MNIST_SPARSE_FUSE_NEURON
  li a0, PH_FORWARD
#else
  li a0, PH_NEURON
#endif
  li a1, 0
  YIELDI zero, TAG_START
  j sparse_wait
#if MNIST_SPARSE_FUSE_NEURON
sparse_root_reverse:
  li a0, PH_REVERSE
  mv a1, a2
  YIELDI zero, TAG_START
  j sparse_wait
#endif
sparse_root_next_phase:
  addi a0, a0, 1
  mv a1, a2
  YIELDI zero, TAG_START
  j sparse_wait
sparse_step_done:
  lw t0, H_STEPS(gp)
  addi t0, t0, 1
  sw t0, H_STEPS(gp)
  lw t1, H_WARMUP(gp)
  bleu t0, t1, sparse_timer_stopped
  li t2, TIMER_ADDR
  sw zero, 0(t2)
sparse_timer_stopped:
  lw t2, H_TIMED(gp)
  add t2, t2, t1
  bltu t0, t2, sparse_step_start
  li a0, PH_FINAL
  li a1, 0
  YIELDI zero, TAG_START
  j sparse_wait
sparse_step_start:
  lw t0, H_STEPS(gp)
  lw t1, H_WARMUP(gp)
  bltu t0, t1, sparse_step_start_phase
  li t0, TIMER_ADDR
  li t1, 1
  sw t1, 0(t0)
sparse_step_start_phase:
#if MNIST_SPARSE_FUSE_NEURON
  lw t0, H_STEPS(gp)
  li a0, PH_FORWARD
  bnez t0, sparse_step_launch
#endif
  li a0, PH_PREPARE
sparse_step_launch:
  li a1, 0
  lw t0, H_STEPS(gp)
  lw t1, H_WARMUP(gp)
  bne t0, t1, sparse_step_send
  li a1, SPIKE_COUNT_START
sparse_step_send:
  YIELDI zero, TAG_START
  j sparse_wait
sparse_final_done:
  li t0, PRINT_ADDR
  sw a3, 0(t0)
  lw t1, H_EXPECTED_CHECKSUM(gp)
  beqz t1, sparse_final_success
  bne a3, t1, sparse_fail
sparse_final_success:
  li a0, SPIKE_COUNT_REPORT
  li a1, 0
  li a2, 0
  li a3, 0
  YIELDI zero, TAG_START
  j sparse_wait
sparse_phase:
#if MNIST_SPARSE_COMPACT_SCHEDULE
  li t0, 6 /* Local next-step preparation; H_PHASE remains PH_REVERSE. */
  beq a0, t0, sparse_prepare_inputs
#endif
  li t0, PH_PREPARE
  beq a0, t0, sparse_prepare
  li t0, PH_FORWARD
#if MNIST_SPARSE_FUSE_NEURON
  beq a0, t0, sparse_fused_forward
#else
  beq a0, t0, sparse_forward
#endif
  li t0, PH_NEURON
  beq a0, t0, sparse_neuron
  li t0, PH_REVERSE
  beq a0, t0, sparse_weight
  li t0, PH_FINAL
  beq a0, t0, sparse_checksum
  j sparse_fail

/* Per-neuron-PU FF counts allow a last-packet acknowledgement once per
 * destination, irrespective of how many local input neurons fired.
 */
sparse_prepare:
  sw zero, H_GROUP_COUNT(gp)
  sw zero, H_POST_LO(gp)
  sw zero, H_POST_HI(gp)
  sw zero, H_ACTIVE_LO(gp)
  sw zero, H_ACTIVE_HI(gp)
  sw zero, H_RECEIVED(gp)
#if !MNIST_SPARSE_COMPACT_SCHEDULE
  sw zero, H_FF_PENDING(gp)
#endif
  sw zero, H_FF_ACKS(gp)
  sw zero, H_WORKER_COUNT(gp)
#if !MNIST_SPARSE_COMPACT_SCHEDULE
  sw zero, H_FIRED_COUNT(gp)
#endif
#if MNIST_SPARSE_COMPACT_SCHEDULE
  lw t0, H_INPUT_COUNT(gp)
#if MNIST_SPARSE_FUSE_NEURON
  bnez t0, sparse_prepare_input_role
  li t0, 1
  sw t0, H_WORKER_COUNT(gp)
  j sparse_prepare_empty
sparse_prepare_input_role:
#else
  beqz t0, sparse_prepare_empty
#endif
  lw t0, H_PREPARED(gp)
  bnez t0, sparse_forward
sparse_prepare_inputs:
  sw zero, H_FF_ACKS(gp)
  sw zero, H_FIRED_COUNT(gp)
  li t0, -1
  sw t0, H_FF_PENDING(gp) /* ACK events are the two preparation joins. */
  li t0, 2
  sw t0, H_WORKER_COUNT(gp)
  lw t0, H_INPUT_COUNT(gp)
  srli t1, t0, 1
  lw a0, H_INPUT_BASE(gp)
  add a0, a0, gp
  li t2, INPUT_SIZE
  mul t2, t1, t2
  add a0, a0, t2
  sub a1, t0, t1
  li t2, 0x40f
  SEND zero, t2
  lw a0, H_INPUT_BASE(gp)
  add a0, a0, gp
  lw a1, H_INPUT_COUNT(gp)
  srli a1, a1, 1
sparse_prepare_worker:
  mv s1, a0
  mv s0, a1
  lw s3, H_FIRED_BASE(gp)
  add s3, s3, gp
  li s4, F_PRE_DECAY
  beqz s0, sparse_prepare_worker_done
sparse_prepare_row:
  lw t0, IN_RNG(s1)
  slli t1, t0, 13
  xor t0, t0, t1
  srli t1, t0, 17
  xor t0, t0, t1
  slli t1, t0, 5
  xor t0, t0, t1
  sw t0, IN_RNG(s1)
  lw t1, IN_THRESHOLD(s1)
  sltu t0, t1, t0
  xori t0, t0, 1
  sw t0, IN_FLAGS(s1)
  lw t1, IN_PRE_TRACE(s1)
  fmul.s t1, t1, s4
  sw t1, IN_PRE_TRACE(s1)
  beqz t0, sparse_prepare_next
  lw t3, IN_GLOBAL(s1)
  bgez t3, sparse_prepare_primary_done
  SPIKE_COUNT_INC (SPM_BASE + SPIKE_COUNT_MNIST), t0, t1
sparse_prepare_primary_done:
  addi t1, gp, H_FIRED_COUNT
  li t2, 1
  amoadd.w t2, t2, (t1)
  slli t2, t2, FIRED_SHIFT
  add t2, s3, t2
  sw s1, 0(t2)
#if MNIST_SPARSE_CACHE_REVERSE && MNIST_SPARSE_MODE == 4
  /* Cache the dense row byte index once per firing, rather than per trace. */
  lw t0, H_INPUT_BASE(gp)
  add t0, t0, gp
  sub t0, s1, t0
  srli t0, t0, 2
  li t1, 0xb6db6db7
  mul t0, t0, t1
  slli t0, t0, 2
  sw t0, 4(t2)
#endif
sparse_prepare_next:
  addi s1, s1, INPUT_SIZE
  addi s0, s0, -1
  bnez s0, sparse_prepare_row
sparse_prepare_worker_done:
  YIELDI zero, TAG_ACK
  j sparse_wait
sparse_prepare_join:
  lw t0, H_WORKER_COUNT(gp)
  addi t0, t0, -1
  sw t0, H_WORKER_COUNT(gp)
  bnez t0, sparse_wait
  lw s3, H_FIRED_BASE(gp)
  add s3, s3, gp
  lw t0, H_FIRED_COUNT(gp)
  slli t0, t0, FIRED_SHIFT
  add s4, s3, t0
  sw s4, H_FIRED_END(gp)
  /* Atomic append permits SMT interleaving. Sort only the fired vector;
   * connection rows are already strictly sorted by input-state offset. */
  addi t0, s3, FIRED_SIZE
sparse_fired_sort:
  bgeu t0, s4, sparse_fired_sorted
  lw t1, 0(t0)
#if FIRED_SIZE == 8
  lw t4, 4(t0)
#endif
  mv t2, t0
sparse_fired_insert:
  beq t2, s3, sparse_fired_insert_store
  lw t3, -FIRED_SIZE(t2)
  bleu t3, t1, sparse_fired_insert_store
  sw t3, 0(t2)
#if FIRED_SIZE == 8
  lw t5, -4(t2)
  sw t5, 4(t2)
#endif
  addi t2, t2, -FIRED_SIZE
  j sparse_fired_insert
sparse_fired_insert_store:
  sw t1, 0(t2)
#if FIRED_SIZE == 8
  sw t4, 4(t2)
#endif
  addi t0, t0, FIRED_SIZE
  j sparse_fired_sort
sparse_fired_sorted:
#if MNIST_SPARSE_RELEVANT_MASKS && (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4) && MNIST_RULE != MT_RULE_ONE
  li s6, 0
  li s7, 0
#endif
  lw s2, H_FF_COUNTS(gp)
  add s2, s2, gp
  li s5, 0
  /* Every count was drained to zero by the preceding FF sender. Count a
   * destination when first encountered, avoiding two full table scans. */
sparse_prepare_count_row:
  beq s3, s4, sparse_prepare_counts_done
  lw t0, 0(s3)
  addi s3, s3, FIRED_SIZE
  lw t1, IN_WEIGHT_COUNT(t0)
  lw t0, IN_WEIGHT_OFFSET(t0)
  add t0, t0, gp
  beqz t1, sparse_prepare_count_row
sparse_prepare_edges:
  lhu t2, 2(t0)
#if MNIST_SPARSE_RELEVANT_MASKS && (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4) && MNIST_RULE != MT_RULE_ONE
  srli t3, t2, 6 /* Logical post slot, encoded in spare descriptor bits. */
  li t4, 1
  sll t4, t4, t3
  li t5, 32
  bgeu t3, t5, 1f
  or s6, s6, t4
  j 2f
1:
  or s7, s7, t4
2:
#endif
#if MNIST_SPARSE_RELEVANT_MASKS && (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4)
  andi t2, t2, 63
#endif
  srli t2, t2, 1
  slli t2, t2, 2
  add t2, s2, t2
  lw t3, 0(t2)
  seqz t4, t3
  add s5, s5, t4
#if MNIST_SPARSE_FUSE_NEURON
  bnez t3, sparse_prepare_destination_known
  lhu t3, 0(t0)
  slli t3, t3, 16 /* Upper half: destination PU; lower half: packet count. */
sparse_prepare_destination_known:
#endif
  addi t3, t3, 1
  sw t3, 0(t2)
  addi t0, t0, 8
  addi t1, t1, -1
  bnez t1, sparse_prepare_edges
  j sparse_prepare_count_row
sparse_prepare_counts_done:
#if MNIST_SPARSE_RELEVANT_MASKS && (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4) && MNIST_RULE != MT_RULE_ONE
  li t0, X_BASE
  sw s6, X_T_LO(t0)
  sw s7, X_T_HI(t0)
#endif
  sw s5, H_FF_PENDING(gp)
  lw t0, H_PREPARED(gp)
  addi t0, t0, 1
  sw t0, H_PREPARED(gp)
#if MNIST_SPARSE_FUSE_NEURON
  beqz s5, sparse_prepare_empty
  sw zero, H_FF_CURSOR(gp)
  YIELDI zero, TAG_FF_WORK
  j sparse_wait
#endif
  lw t0, H_PHASE(gp)
  li t1, PH_REVERSE
  beq t0, t1, sparse_prepare_empty
  j sparse_forward
#else
  lw s0, H_INPUT_COUNT(gp)
  beqz s0, sparse_prepare_empty
  lw s1, H_INPUT_BASE(gp)
  add s1, s1, gp
  lw s2, H_FF_COUNTS(gp)
  add s2, s2, gp
  li t0, 28
  mv t1, s2
sparse_prepare_zero:
  sw zero, 0(t1)
  addi t1, t1, 4
  addi t0, t0, -1
  bnez t0, sparse_prepare_zero
  lw s3, H_FIRED_BASE(gp)
  add s3, s3, gp
  li s4, 0
#if !MNIST_SPARSE_FUSE_FORWARD
  li s5, 0
#endif
sparse_prepare_row:
  lw t0, IN_RNG(s1)
  slli t1, t0, 13
  xor t0, t0, t1
  srli t1, t0, 17
  xor t0, t0, t1
  slli t1, t0, 5
  xor t0, t0, t1
  sw t0, IN_RNG(s1)
  lw t1, IN_THRESHOLD(s1)
  sltu t0, t1, t0
  xori t0, t0, 1
  sw t0, IN_FLAGS(s1)
  lw t1, IN_PRE_TRACE(s1)
  li t2, F_PRE_DECAY
  fmul.s t1, t1, t2
  sw t1, IN_PRE_TRACE(s1)
  beqz t0, sparse_prepare_next
  sw s1, 0(s3)
  addi s3, s3, 4
  addi s4, s4, 1
#if !MNIST_SPARSE_FUSE_FORWARD
  lw t0, IN_GLOBAL(s1)
  bgez t0, sparse_prepare_counted
  addi s5, s5, 1
sparse_prepare_counted:
#endif
  lw t0, IN_WEIGHT_OFFSET(s1)
  add t0, t0, gp
  lw t1, IN_WEIGHT_COUNT(s1)
  beqz t1, sparse_prepare_next
sparse_prepare_edges:
  lhu t2, 2(t0)
  srli t2, t2, 1
  slli t2, t2, 2
  add t2, s2, t2
  lw t3, 0(t2)
  addi t3, t3, 1
  sw t3, 0(t2)
  addi t0, t0, 8
  addi t1, t1, -1
  bnez t1, sparse_prepare_edges
sparse_prepare_next:
  addi s1, s1, INPUT_SIZE
  addi s0, s0, -1
  bnez s0, sparse_prepare_row
  sw s3, H_FIRED_END(gp)
  sw s4, H_FIRED_COUNT(gp)
  li t0, 28
  li t1, 0
sparse_prepare_targets:
  lw t2, 0(s2)
  snez t2, t2
  add t1, t1, t2
  addi s2, s2, 4
  addi t0, t0, -1
  bnez t0, sparse_prepare_targets
  sw t1, H_FF_PENDING(gp)
#if MNIST_SPARSE_FUSE_FORWARD
  /* Neuron preparation leaves GE intact, so input PUs may deliver as soon
   * as their own counts are ready. Acks keep the integration boundary exact. */
  j sparse_forward
#else
  mv a2, s5
  li a3, 0
  j sparse_reduce_local
#endif
#endif
sparse_prepare_empty:
  li a2, 0
  li a3, 0
  j sparse_reduce_local
#if MNIST_SPARSE_FUSE_NEURON
/* The preceding reverse barrier includes acknowledgements for every count
 * announcement. FF may arrive before the local phase-start event; never clear
 * its receive count here. Both FF and phase handlers execute on thread zero. */
sparse_fused_forward:
  sw zero, H_RECEIVED(gp)
  sw zero, H_WORKER_COUNT(gp)
  lw t0, H_INPUT_COUNT(gp)
  bnez t0, sparse_forward
  lw t0, H_FF_PENDING(gp)
  sw t0, H_FF_END(gp)
  sw zero, H_FF_PENDING(gp)
  j sparse_fused_neuron_ready
#endif
sparse_forward:
#if MNIST_SPARSE_COMPACT_SCHEDULE
  /* Preserve completed-step activity for the timer observer while the next
   * input state is prepared. These sender fields are unused on input PUs. */
  sw zero, H_REV_MINUS(gp)
  sw zero, H_REV_PLUS(gp)
  lw t0, H_FIRED_BASE(gp)
  add t0, t0, gp
  lw t1, H_FIRED_COUNT(gp)
  li t2, 0
  li t3, 0
  beqz t1, sparse_forward_stats_done
sparse_forward_stats:
  lw t4, 0(t0)
  lw t5, IN_GLOBAL(t4)
  srli t5, t5, 31
  add t2, t2, t5
  lw t5, IN_WEIGHT_COUNT(t4)
  add t3, t3, t5
  addi t0, t0, FIRED_SIZE
  addi t1, t1, -1
  bnez t1, sparse_forward_stats
sparse_forward_stats_done:
  sw t2, H_REV_MINUS(gp)
  sw t3, H_REV_PLUS(gp)
#endif
  lw t0, H_INPUT_COUNT(gp)
  beqz t0, sparse_prepare_empty
  lw t0, H_FIRED_COUNT(gp)
#if MNIST_SPARSE_FUSE_NEURON
  beqz t0, sparse_neuron_input
#else
  beqz t0, sparse_prepare_empty
#endif
  lw t0, H_FIRED_BASE(gp)
  add t0, t0, gp
  sw t0, H_FF_ROW(gp)
  sw zero, H_FF_CURSOR(gp)
  sw zero, H_FF_END(gp)
  YIELDI zero, TAG_FF_WORK
  j sparse_wait
sparse_ff_send:
#if MNIST_SPARSE_FUSE_NEURON
  lw t0, H_PHASE(gp)
  li t1, PH_FORWARD
  bne t0, t1, sparse_announce_send
#endif
  lw t0, H_FF_CURSOR(gp)
  lw t1, H_FF_END(gp)
  bne t0, t1, sparse_ff_send_edge
sparse_ff_next_row:
  lw t2, H_FF_ROW(gp)
  lw t3, H_FIRED_END(gp)
  beq t2, t3, sparse_ff_sent
  lw t3, 0(t2)
  addi t2, t2, FIRED_SIZE
  sw t2, H_FF_ROW(gp)
  lw t0, IN_WEIGHT_OFFSET(t3)
  add t0, t0, gp
  lw t1, IN_WEIGHT_COUNT(t3)
  slli t1, t1, 3
  add t1, t1, t0
  sw t1, H_FF_END(gp)
  beq t0, t1, sparse_ff_next_row
sparse_ff_send_edge:
  lhu t1, 0(t0)
  lhu a0, 2(t0)
  lw a1, 4(t0)
  addi t0, t0, 8
  sw t0, H_FF_CURSOR(gp)
#if MNIST_SPARSE_RELEVANT_MASKS && (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4)
  andi t0, a0, 63
  srli t0, t0, 1
#else
  srli t0, a0, 1
#endif
  slli t0, t0, 2
  lw t2, H_FF_COUNTS(gp)
  add t2, t2, gp
  add t2, t2, t0
  lw t0, 0(t2)
  beqz t0, sparse_fail
  addi t0, t0, -1
#if MNIST_SPARSE_FUSE_NEURON
  slli t3, t0, 16
  bnez t3, sparse_ff_count_remaining
  li t0, 0
sparse_ff_count_remaining:
#endif
  sw t0, 0(t2)
  lw a2, H_ID(gp)
#if MNIST_SPARSE_RELEVANT_MASKS && (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4)
  lw t3, H_RANK(gp)
  slli t3, t3, 16
  or a2, a2, t3 /* Preserve physical destination for existing acknowledgements. */
#endif
  seqz t0, t0
  slli t0, t0, 31
  or a2, a2, t0
  andi a0, a0, 1
  SENDI t1, TAG_FF
  YIELDI zero, TAG_FF_WORK
  j sparse_wait
sparse_ff_sent:
#if MNIST_SPARSE_FUSE_NEURON
  j sparse_neuron_input
#endif
  li t0, 1
  sw t0, H_WORKER_COUNT(gp)
  j sparse_ff_maybe_done
sparse_ff_ack:
#if MNIST_SPARSE_COMPACT_SCHEDULE
  lw t0, H_FF_PENDING(gp)
  bltz t0, sparse_prepare_join
#endif
  lw t0, H_FF_ACKS(gp)
  addi t0, t0, 1
  sw t0, H_FF_ACKS(gp)
sparse_ff_maybe_done:
  lw t0, H_FF_ACKS(gp)
  lw t1, H_FF_PENDING(gp)
  bgtu t0, t1, sparse_fail
  bne t0, t1, sparse_wait
  lw t0, H_WORKER_COUNT(gp)
  beqz t0, sparse_wait
  sw zero, H_WORKER_COUNT(gp)
  j sparse_prepare_empty
#if MNIST_SPARSE_FUSE_NEURON
/* Count announcements are bounded by actual active destinations and reuse the
 * existing FF/ACK priority relationship. No connection entries are added. */
sparse_announce_send:
  lw t0, H_FF_CURSOR(gp)
  lw t1, H_FF_COUNTS(gp)
  add t1, t1, gp
  add t1, t1, t0
sparse_announce_next:
  li t2, 112
  beq t0, t2, sparse_announce_sent
  lw t2, 0(t1)
  addi t0, t0, 4
  addi t1, t1, 4
  beqz t2, sparse_announce_next
  sw t0, H_FF_CURSOR(gp)
  srli t3, t2, 16
  slli a1, t2, 16
  srli a1, a1, 16
  li a0, -1
  lw a2, H_ID(gp)
  SENDI t3, TAG_FF
  YIELDI zero, TAG_FF_WORK
  j sparse_wait
sparse_announce_sent:
  li t0, 1
  sw t0, H_WORKER_COUNT(gp)
  j sparse_ff_maybe_done
sparse_announce_receive:
  lw t0, H_FF_PENDING(gp)
  add t0, t0, a1
  sw t0, H_FF_PENDING(gp)
  SENDI a2, TAG_ACK
  j sparse_wait
#endif
sparse_ff_receive:
#if MNIST_SPARSE_FUSE_NEURON
  bltz a0, sparse_announce_receive
#endif
  lw t0, H_EXC_COUNT(gp)
  bgeu a0, t0, sparse_fail
  li t0, EXC_SIZE
  mul t0, a0, t0
  lw t1, H_EXC_BASE(gp)
  add t0, t0, t1
  add t0, t0, gp
  lw t1, EXC_GE(t0)
  fadd.s t1, t1, a1
  sw t1, EXC_GE(t0)
#if MNIST_SPARSE_RELEVANT_MASKS && MNIST_RULE != MT_RULE_ONE
  /* All forward delivery is on thread zero: these ORs need no atomics. */
  li t0, X_BASE
  slli t1, a0, 3
  add t0, t0, t1
#if MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4
  srli t1, a2, 16
  andi t1, t1, 63
  srli t2, t1, 5
  slli t2, t2, 2
  add t0, t0, t2
#else
  addi t1, a2, -1
  srli t1, t1, 5
  andi t1, t1, 15
#endif
  li t2, 1
  sll t2, t2, t1
  lw t1, 0(t0)
  or t1, t1, t2
  sw t1, 0(t0)
#endif
#if MNIST_SPARSE_FUSE_NEURON
  lw t0, H_FF_ACKS(gp)
  addi t0, t0, 1
  sw t0, H_FF_ACKS(gp)
sparse_fused_neuron_ready:
  lw t0, H_PHASE(gp)
  li t1, PH_FORWARD
  bne t0, t1, sparse_wait
  lw t0, H_WORKER_COUNT(gp)
  bnez t0, sparse_wait
  lw t0, H_FF_ACKS(gp)
  lw t1, H_FF_END(gp)
  bgtu t0, t1, sparse_fail
  bne t0, t1, sparse_wait
  li t0, 1
  sw t0, H_WORKER_COUNT(gp)
  sw zero, H_FF_ACKS(gp)
  j sparse_neuron
#else
  bgez a2, sparse_wait
  SENDI a2, TAG_ACK
  j sparse_wait
#endif

/* A binomial group tree collects and distributes two post-mask words and two
 * input-PU activity words before the neuron barrier. A node consumes one
 * report from itself and one from each immediate child.
 */
sparse_neuron:
  lw s3, H_EXC_COUNT(gp)
  beqz s3, sparse_neuron_input
  lw s0, H_EXC_BASE(gp)
  add s0, s0, gp
  lw s2, H_INH_BASE(gp)
  add s2, s2, gp
  li s4, 0
  li s5, 0
  li s6, 0
sparse_neuron_loop:
  jal s7, neuron_pair_update
  add s6, s6, s8
  lw t0, EXC_SLOT(s0)
  li t1, 32
  bgeu t0, t1, sparse_neuron_hi
  sll t1, s9, t0
  or s4, s4, t1
  j sparse_neuron_next
sparse_neuron_hi:
  sll t1, s9, t0
  or s5, s5, t1
sparse_neuron_next:
  addi s0, s0, EXC_SIZE
  addi s2, s2, INH_SIZE
  addi s3, s3, -1
  bnez s3, sparse_neuron_loop
  sw s6, H_LOCAL_SPIKES(gp)
  mv a0, s4
  mv a1, s5
  li a2, 0
  li a3, 0
  j sparse_neuron_report
sparse_neuron_input:
  sw zero, H_LOCAL_SPIKES(gp)
  li a0, 0
  li a1, 0
  li a2, 0
  li a3, 0
  lw t0, H_FIRED_COUNT(gp)
  beqz t0, sparse_neuron_report
  lw t0, H_RANK(gp)
  li t1, 1
  sll t1, t1, t0
  li t2, 32
  bgeu t0, t2, sparse_neuron_active_hi
  mv a2, t1
  j sparse_neuron_report
sparse_neuron_active_hi:
  mv a3, t1
sparse_neuron_report:
  SENDI zero, TAG_GROUP
  j sparse_wait
sparse_group:
  /* Every 32-bit firing mask is valid, including all bits set. Boot is
   * identified by the phase, never by overloading one of the mask words. */
  lw t0, H_PHASE(gp)
  beqz t0, sparse_boot_ready
#if MNIST_SPARSE_FUSE_NEURON
  /* A group report can beat local next-phase start. Reset on its first
   * arrival, using the sentinel set at the preceding reverse start. */
  lw t0, H_GROUP_COUNT(gp)
  bgez t0, sparse_group_accumulate
  sw zero, H_GROUP_COUNT(gp)
  sw zero, H_POST_LO(gp)
  sw zero, H_POST_HI(gp)
  sw zero, H_ACTIVE_LO(gp)
  sw zero, H_ACTIVE_HI(gp)
sparse_group_accumulate:
#endif
  lw t0, H_POST_LO(gp)
  or t0, t0, a0
  sw t0, H_POST_LO(gp)
  lw t0, H_POST_HI(gp)
  or t0, t0, a1
  sw t0, H_POST_HI(gp)
  lw t0, H_ACTIVE_LO(gp)
  or t0, t0, a2
  sw t0, H_ACTIVE_LO(gp)
  lw t0, H_ACTIVE_HI(gp)
  or t0, t0, a3
  sw t0, H_ACTIVE_HI(gp)
  lw t0, H_GROUP_COUNT(gp)
  addi t0, t0, 1
  sw t0, H_GROUP_COUNT(gp)
  lw t1, H_MASK_CHILDREN(gp)
  addi t1, t1, 1
  bgtu t0, t1, sparse_fail
  bne t0, t1, sparse_wait
  lw a0, H_POST_LO(gp)
  lw a1, H_POST_HI(gp)
  lw a2, H_ACTIVE_LO(gp)
  lw a3, H_ACTIVE_HI(gp)
  lw t0, H_GROUP_PARENT(gp)
  beqz t0, sparse_mask
  SENDI t0, TAG_GROUP
  j sparse_wait
sparse_boot_ready:
  lw t0, H_GROUP_COUNT(gp)
  addi t0, t0, 1
  sw t0, H_GROUP_COUNT(gp)
  li t1, 512
  bne t0, t1, sparse_wait
  j sparse_step_start
sparse_group_send:
#if MNIST_SPARSE_MC_DRAIN
  lw t0, H_PHASE(gp)
  li t1, PH_REVERSE
  beq t0, t1, sparse_drain_thread0
#endif
  lw t0, H_BCAST_CURSOR(gp)
  lw t1, H_PEERS(gp)
  add t1, t1, gp
  slli t2, t0, 1
  add t1, t1, t2
  lhu t1, 0(t1)
  addi t0, t0, 1
  sw t0, H_BCAST_CURSOR(gp)
  lw a0, H_POST_LO(gp)
  lw a1, H_POST_HI(gp)
  lw a2, H_ACTIVE_LO(gp)
  lw a3, H_ACTIVE_HI(gp)
  SENDI t1, TAG_MASK
  lw t1, H_MASK_CHILDREN(gp)
  beq t0, t1, sparse_mask_local
  YIELDI zero, TAG_GROUP_WORK
  j sparse_wait

/* Local degree counts are completion metadata, not reverse synapse lists.
 * An MC-active, nonfiring post scatters to all structural targets in that MC.
 */
sparse_mask:
  sw zero, H_BCAST_CURSOR(gp)
  sw a0, H_POST_LO(gp)
  sw a1, H_POST_HI(gp)
  sw a2, H_ACTIVE_LO(gp)
  sw a3, H_ACTIVE_HI(gp)
  /* Publish to children before scanning degrees; otherwise local CSR work
   * accumulates along the mask tree's critical path. */
  lw t0, H_MASK_CHILDREN(gp)
  beqz t0, sparse_mask_local
  YIELDI zero, TAG_GROUP_WORK
  j sparse_wait
sparse_mask_local:
  lw a0, H_POST_LO(gp)
  lw a1, H_POST_HI(gp)
  lw a2, H_ACTIVE_LO(gp)
  lw a3, H_ACTIVE_HI(gp)
  lw t0, H_INPUT_COUNT(gp)
  beqz t0, sparse_mask_done
#if MNIST_SPARSE_MC_DRAIN
  j sparse_mask_done /* MC drain replaces all expected-degree computations. */
#elif MNIST_SPARSE_RELEVANT_MASKS
#if MNIST_RULE != MT_RULE_ONE
  li t0, X_BASE
  lw t1, X_T_LO(t0)
  or a0, a0, t1
  lw t1, X_T_HI(t0)
  or a1, a1, t1
#endif
  /* |T union F|, with each set bit counted exactly once. */
  li t2, 0
1:
  beqz a0, 2f
  addi t0, a0, -1
  and a0, a0, t0
  addi t2, t2, 1
  j 1b
2:
  beqz a1, sparse_mask_store
  addi t0, a1, -1
  and a1, a1, t0
  addi t2, t2, 1
  j 2b
#endif
  li s0, 0
#if MNIST_RULE != MT_RULE_ONE
#if (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4)
  lw s0, H_FIRED_COUNT(gp)
#else
  lw t0, H_RANK(gp)
  li t1, 32
  bltu t0, t1, sparse_mask_low_active
  mv a2, a3
sparse_mask_low_active:
  andi t0, t0, 28
  srl s0, a2, t0
  andi s0, s0, 15
#endif
#endif
  beqz s0, sparse_mask_posts_only
#if (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4)
  li t2, 50
#elif MNIST_SPARSE_MODE == 2
  lw t2, H_CONNECTED_POSTS(gp)
#else
  lw t2, H_FORWARD_COUNT(gp)
#endif
  j sparse_mask_store
sparse_mask_posts_only:
  li t2, 0
  or t0, a0, a1
  beqz t0, sparse_mask_store
  lw t0, H_DEGREES(gp)
  add t0, t0, gp
  li t1, 0
  li t2, 0
sparse_mask_count:
  lhu t3, 0(t0)
#if (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4)
  li t3, 1
#elif MNIST_SPARSE_MODE == 2
  snez t3, t3
#endif
  bnez s0, sparse_mask_add
  li t4, 32
  bgeu t1, t4, sparse_mask_high_post
  srl t4, a0, t1
  j sparse_mask_test_post
sparse_mask_high_post:
  srl t4, a1, t1
sparse_mask_test_post:
  andi t4, t4, 1
  beqz t4, sparse_mask_next
sparse_mask_add:
  add t2, t2, t3
sparse_mask_next:
  addi t0, t0, 2
  addi t1, t1, 1
  li t3, 50
  bne t1, t3, sparse_mask_count
sparse_mask_store:
  sw t2, H_EXPECTED(gp)
sparse_mask_done:
#if MNIST_SPARSE_COMPACT_SCHEDULE && MNIST_SPARSE_ACTIVE_PREFIX && (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4) && MNIST_RULE != MT_RULE_ONE
  lw t0, H_EXC_COUNT(gp)
  beqz t0, sparse_mask_report
  /* Partition the existing destination records in place. Firing posts visit
   * all records; quiet posts visit only the active prefix. No extra table. */
  lw s0, H_DEST_BASE(gp)
  add s0, s0, gp
  mv s2, s0
  addi s1, s0, 144
  li s3, 0
  lw s4, H_ACTIVE_LO(gp)
  lw s5, H_ACTIVE_HI(gp)
  or t0, s4, s5
  beqz t0, sparse_active_partition_done
sparse_active_partition:
  lw t0, 0(s0)
  srli t1, t0, 16
  li t2, 32
  bgeu t1, t2, sparse_active_partition_high
  srl t2, s4, t1
  j sparse_active_partition_test
sparse_active_partition_high:
  srl t2, s5, t1
sparse_active_partition_test:
  andi t2, t2, 1
  beqz t2, sparse_active_partition_next
  lw t3, 0(s2)
  sw t3, 0(s0)
  sw t0, 0(s2)
  addi s2, s2, 4
  addi s3, s3, 1
sparse_active_partition_next:
  addi s0, s0, 4
  bne s0, s1, sparse_active_partition
sparse_active_partition_done:
  sw s3, H_FIRED_COUNT(gp) /* Unused by neuron state/observers. */
#endif
sparse_mask_report:
#if MNIST_SPARSE_CACHE_REVERSE
  lw t0, H_INPUT_COUNT(gp)
  beqz t0, sparse_mask_report_neuron
#if MNIST_SPARSE_MODE == 4 && MNIST_RULE != MT_RULE_ONE
  la t0, sparse_reverse_quiet
  lw t1, H_FIRED_COUNT(gp)
  li t2, 1
  bne t1, t2, sparse_reverse_select
  la t0, sparse_reverse_one_pre
  lw t1, H_FIRED_BASE(gp)
  add t1, t1, gp
  lw t1, 4(t1)
  sw t1, H_REV_CURSOR(gp) /* Sender-only slot is unused on input PUs. */
sparse_reverse_select:
  csrw 0x704, t0
#endif
  INIT_REVERSE_PHASE
  li a0, 0
  li a1, -1
  SENDI zero, 0x40f
  j sparse_wait
sparse_mask_report_neuron:
#endif
  lw a2, H_LOCAL_SPIKES(gp)
  li a3, 0
  j sparse_reduce_local

sparse_weight:
#if MNIST_SPARSE_FUSE_NEURON
  li t0, -1
  sw t0, H_GROUP_COUNT(gp)
#endif
  lw t0, H_INPUT_COUNT(gp)
#if MNIST_SPARSE_MC_DRAIN
  beqz t0, 1f
  li a0, 1
  YIELDI zero, TAG_ISSUED
  j sparse_wait
1:
#else
  bnez t0, sparse_reverse_maybe_done
#endif
  lw s0, H_EXC_BASE(gp)
  add s0, s0, gp
  lw s2, H_INH_BASE(gp)
  add s2, s2, gp
  lw s3, H_EXC_COUNT(gp)
  mv s4, s0
sparse_post_prepare:
  lw t0, H_LOCAL_INH(gp)
  lw t1, INH_FLAGS(s2)
  sub t0, t0, t1
  lw t1, EXC_GI(s0)
  li t2, F_I_TO_E
  beqz t0, sparse_post_recurrent_done
sparse_post_recurrent:
  fadd.s t1, t1, t2
  addi t0, t0, -1
  bnez t0, sparse_post_recurrent
sparse_post_recurrent_done:
  sw t1, EXC_GI(s0)
#if MNIST_RULE != MT_RULE_ONE
  lw t0, EXC_POST1(s0)
  li t1, F_POST1_DECAY
  fmul.s t0, t0, t1
  sw t0, EXC_POST1(s0)
#endif
#if MNIST_RULE == MT_RULE_THREE
  lw t0, EXC_POST2(s0)
  li t1, F_POST2_DECAY
  fmul.s t0, t0, t1
  sw t0, EXC_POST2(s0)
#endif
  addi s0, s0, EXC_SIZE
  addi s2, s2, INH_SIZE
  addi s3, s3, -1
  bnez s3, sparse_post_prepare
  sw s4, H_REV_POST(gp)
  sw s0, H_REV_END(gp)
  li t0, -1
  sw t0, H_REV_CURSOR(gp)
  YIELDI zero, TAG_REV_WORK
  j sparse_wait

sparse_reverse_send:
  lw t0, H_REV_POST(gp)
  lw t1, H_REV_END(gp)
  beq t0, t1, sparse_post_commit
  lw t1, H_REV_CURSOR(gp)
  bgez t1, sparse_reverse_dest
  lw t2, EXC_FLAGS(t0)
  andi t2, t2, 1
  sw t2, H_REV_FIRED(gp)
#if MNIST_RULE == MT_RULE_ONE
  beqz t2, sparse_reverse_next_post
#else
  bnez t2, sparse_reverse_coefficients
#if MNIST_SPARSE_RELEVANT_MASKS
  lw t3, H_EXC_BASE(gp)
  add t3, t3, gp
  sub t3, t0, t3
  snez t3, t3 /* At most two local posts. */
  slli t3, t3, 3
  li t4, X_BASE
  add t4, t4, t3
  lw t3, 0(t4)
  lw t4, 4(t4)
  /* Cache the selected post's mask once for the destination continuation.
   * Group activity is no longer needed on a neuron PU after reverse starts.
   */
  sw t3, H_ACTIVE_LO(gp)
  sw t4, H_ACTIVE_HI(gp)
#else
  lw t3, H_ACTIVE_LO(gp)
  lw t4, H_ACTIVE_HI(gp)
#endif
  or t3, t3, t4
  beqz t3, sparse_reverse_next_post
#endif
sparse_reverse_coefficients:
#if MNIST_RULE == MT_RULE_ONE
  sw zero, H_REV_MINUS(gp)
#else
  lw t3, EXC_POST1(t0)
  lw t4, H_DEP(gp)
  fmul.s t3, t3, t4
  sw t3, H_REV_MINUS(gp)
#endif
  li t3, 0
#if MNIST_RULE == MT_RULE_THREE
  beqz t2, sparse_reverse_plus_ready
  lw t3, EXC_POST2(t0)
  lw t4, H_POT(gp)
  fmul.s t3, t3, t4
sparse_reverse_plus_ready:
#endif
  sw t3, H_REV_PLUS(gp)
  li t1, 0
sparse_reverse_dest:
#if (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4)
  li t2, 36
#if MNIST_SPARSE_COMPACT_SCHEDULE && MNIST_SPARSE_ACTIVE_PREFIX && MNIST_RULE != MT_RULE_ONE
  lw t3, H_REV_FIRED(gp)
  bnez t3, sparse_reverse_dest_limit
  lw t2, H_FIRED_COUNT(gp)
sparse_reverse_dest_limit:
#endif
#else
  lw t2, EXC_DESCS(t0)
#endif
  beq t1, t2, sparse_reverse_next_post
  addi t2, t1, 1
  sw t2, H_REV_CURSOR(gp)
  lw a2, H_REV_MINUS(gp)
  lw a3, H_REV_PLUS(gp)
  lw t2, H_REV_FIRED(gp)
  li t6, TAG_REVERSE
  beqz t2, sparse_reverse_tag_ready
  li t6, TAG_POST
sparse_reverse_tag_ready:
#if (MNIST_SPARSE_MODE == 1 || MNIST_SPARSE_MODE == 4)
  lw t3, H_DEST_BASE(gp)
  add t3, t3, gp
  slli t4, t1, 2
  add t3, t3, t4
  lhu t4, 0(t3)
#if !MNIST_SPARSE_COMPACT_SCHEDULE || (!MNIST_SPARSE_ACTIVE_PREFIX && MNIST_RULE != MT_RULE_ONE)
  lhu t5, 2(t3)
  bnez t2, sparse_reverse_local_send
  li t1, 32
  bgeu t5, t1, sparse_reverse_local_hi
  lw t1, H_ACTIVE_LO(gp)
  j sparse_reverse_local_test
sparse_reverse_local_hi:
  lw t1, H_ACTIVE_HI(gp)
sparse_reverse_local_test:
  srl t1, t1, t5
  andi t1, t1, 1
  beqz t1, sparse_reverse_send
#endif
sparse_reverse_local_send:
  li a0, 0
  lw a1, EXC_SLOT(t0)
  SEND t4, t6
#else
  lw t3, EXC_DESC(t0)
  add t3, t3, gp
  slli t4, t1, 4
  add t3, t3, t4
  bnez t2, sparse_reverse_scatter_send
#if MNIST_SPARSE_RELEVANT_MASKS
  lw t4, H_ACTIVE_LO(gp)
  lhu t5, 0(t3)
  addi t5, t5, -1 /* MC destination 0x8001 + mc; shift masks low five bits. */
  srl t4, t4, t5
  andi t4, t4, 1
#else
  lw t4, H_ACTIVE_LO(gp)
  lw t5, 8(t3)
  and t4, t4, t5
  lw t2, H_ACTIVE_HI(gp)
  lw t5, 12(t3)
  and t2, t2, t5
  or t4, t4, t2
#endif
  beqz t4, sparse_reverse_send
sparse_reverse_scatter_send:
  lhu t4, 0(t3)
  lhu a1, 2(t3)
  slli a1, a1, 16
  slli t6, t6, 20
  srli t6, t6, 20
  or a1, a1, t6
  lw a0, 4(t3)
  SENDI t4, TAG_SCATTER
#endif
sparse_reverse_continue:
  YIELDI zero, TAG_REV_WORK
  j sparse_wait
sparse_reverse_next_post:
  addi t0, t0, EXC_SIZE
  sw t0, H_REV_POST(gp)
  li t1, -1
  sw t1, H_REV_CURSOR(gp)
  j sparse_reverse_continue
sparse_post_commit:
#if MNIST_SPARSE_RELEVANT_MASKS
  li t0, X_BASE
  sw zero, 0(t0)
  sw zero, 4(t0)
  sw zero, 8(t0)
  sw zero, 12(t0)
#endif
#if MNIST_SPARSE_MC_DRAIN
  li a0, 1
  SENDI zero, TAG_ISSUED
#endif
  lw t0, H_EXC_BASE(gp)
  add t0, t0, gp
  lw t1, H_EXC_COUNT(gp)
sparse_post_commit_loop:
  lw t2, EXC_FLAGS(t0)
  andi t2, t2, 1
  beqz t2, sparse_post_commit_next
  li t3, F_ONE
#if MNIST_RULE == MT_RULE_THREE
  sw t3, EXC_POST1(t0)
  sw t3, EXC_POST2(t0)
#elif MNIST_RULE == MT_RULE_TWO
  lw t2, EXC_POST1(t0)
  fadd.s t3, t3, t2
  sw t3, EXC_POST1(t0)
#endif
sparse_post_commit_next:
  sw zero, EXC_FLAGS(t0)
  addi t0, t0, EXC_SIZE
  addi t1, t1, -1
  bnez t1, sparse_post_commit_loop
#if MNIST_SPARSE_MC_DRAIN
  /* Neuron-only MC domains must also drain before the global step advances. */
  j sparse_wait
#else
  j sparse_prepare_empty
#endif

/* s0=input state, s1=weight, s2=d_minus, s3=d_plus, s4=post fired,
 * s5/s6=reverse row bounds, s8=wmax, s9=pot rate, s10=pre target.
 */
sparse_reverse_quiet:
#if MNIST_SPARSE_MODE == 2
  lw t0, H_FIRED_COUNT(gp)
  beqz t0, sparse_reverse_received
#elif MNIST_SPARSE_MODE == 3
  add t0, a0, gp
  lw t0, IN_FLAGS(t0)
  beqz t0, sparse_reverse_received
#endif
  li s4, 0
  j sparse_reverse_decode
sparse_reverse_post:
  li s4, 1
sparse_reverse_decode:
  mv s2, a2
#if !MNIST_SPARSE_CACHE_REVERSE || MNIST_RULE == MT_RULE_THREE
  mv s3, a3
#endif
#if !MNIST_SPARSE_CACHE_REVERSE
  lw s9, H_POT(gp)
  lw s10, H_PRE_TARGET(gp)
#endif
#if MNIST_SPARSE_MODE == 3
  add s0, a0, gp
  add s1, a1, gp
  lhu t2, -2(s1) /* Forward descriptor, immediately before the weight. */
#else
  li t1, 50
  bgeu a1, t1, sparse_fail
#if MNIST_SPARSE_CACHE_REVERSE
  slli t2, a1, 1
  add t1, ra, t2
#else
  lw t1, H_REVERSE_PTR(gp)
  add t1, t1, gp
  slli t2, a1, 1
  add t1, t1, t2
#endif
  lhu s5, 0(t1)
  lhu s6, 2(t1)
  add s5, s5, gp
  add s6, s6, gp
  beq s5, s6, sparse_reverse_received
  mv t2, a1
#endif
  slli t2, t2, 2
#if MNIST_SPARSE_CACHE_REVERSE
  add s8, tp, t2
#else
  lw s8, H_WMAX_BASE(gp)
  add s8, s8, gp
  add s8, s8, t2
#endif
  lw s8, 0(s8)
#if MNIST_RULE != MT_RULE_THREE
#if MNIST_SPARSE_COMPACT_SCHEDULE
  li ROOT_NEWTON, 0x3f99ca19 /* t0 is the only nonpersistent root constant. */
#else
  INIT_FIFTH_ROOT
#endif
#endif
#if MNIST_SPARSE_MODE == 4
#if MNIST_SPARSE_CACHE_REVERSE
  beqz s4, sparse_reverse_dense_quiet_setup
#if MNIST_RULE == MT_RULE_THREE
  mv s7, a6
#else
  lw s7, H_INPUT_BASE(gp)
  add s7, s7, gp
#endif
  j sparse_reverse_entry
sparse_reverse_dense_quiet_setup:
#if MNIST_RULE == MT_RULE_THREE
  mv s7, a4
#else
  lw s7, H_FIRED_BASE(gp)
  add s7, s7, gp
#endif
#else
  lw s7, H_INPUT_BASE(gp)
  add s7, s7, gp
  bnez s4, sparse_reverse_entry
  mv s10, s7 /* Quiet posts do not use the pre-trace target. */
  lw s7, H_FIRED_BASE(gp)
  add s7, s7, gp
  lw s9, H_FIRED_END(gp)
  li s6, 0xb6db6db7 /* Multiplicative inverse of 7 modulo 2^32. */
#endif
sparse_reverse_dense_next:
#if MNIST_SPARSE_CACHE_REVERSE
#if MNIST_RULE == MT_RULE_THREE
  beq s7, a5, sparse_reverse_received
#else
  beq s7, s3, sparse_reverse_received
#endif
  lw s0, 0(s7)
  lw t1, 4(s7)
  addi s7, s7, FIRED_SIZE
#else
  beq s7, s9, sparse_reverse_received
  lw s0, 0(s7)
  addi s7, s7, 4
  sub t1, s0, s10
  srli t1, t1, 2
  mul t1, t1, s6 /* Exact local index: (state - base) / 28. */
  slli t1, t1, 2
#endif
  add t1, s5, t1
  lhu s1, 0(t1)
  beqz s1, sparse_reverse_dense_next
  add s1, s1, gp
  li t4, 1
  j sparse_reverse_apply
#endif
#if MNIST_SPARSE_BINARY_REVERSE && MNIST_SPARSE_MODE <= 2
  bnez s4, sparse_reverse_entry
#if MNIST_SPARSE_CACHE_REVERSE && MNIST_RULE == MT_RULE_THREE
  mv s7, a4
#else
  lw s7, H_FIRED_BASE(gp)
  add s7, s7, gp
#endif
#if !MNIST_SPARSE_CACHE_REVERSE
  lw s9, H_FIRED_END(gp) /* Quiet posts do not use the potentiation rate. */
#endif
  j sparse_reverse_search_next

/* Both vectors are sorted by local input-state offset. Search only the
 * remaining suffix; a hit or insertion position advances the lower bound.
 * Storage remains the existing CSR plus the O(local inputs) fired vector. */
sparse_reverse_search_next:
#if MNIST_SPARSE_CACHE_REVERSE
#if MNIST_RULE == MT_RULE_THREE
  beq s7, a5, sparse_reverse_received
#else
  beq s7, s3, sparse_reverse_received
#endif
#else
  beq s7, s9, sparse_reverse_received
#endif
  beq s5, s6, sparse_reverse_received
  lw s0, 0(s7)
  addi s7, s7, 4
  sub s0, s0, gp
  mv t1, s5
  mv t2, s6
sparse_reverse_search:
  bgeu t1, t2, sparse_reverse_search_miss
  sub t3, t2, t1
#if MNIST_SPARSE_BINARY_REVERSE == 2
  li t4, 32
  bleu t3, t4, sparse_reverse_search_tail
#endif
  srli t3, t3, 3
  slli t3, t3, 2
  add t3, t3, t1
  lhu t4, 0(t3)
  beq t4, s0, sparse_reverse_search_hit
  bltu t4, s0, sparse_reverse_search_right
  mv t2, t3
  j sparse_reverse_search
sparse_reverse_search_right:
  addi t1, t3, 4
  j sparse_reverse_search
#if MNIST_SPARSE_BINARY_REVERSE == 2
/* A bounded tail avoids binary-search setup on tiny rows. Larger rows retain
 * logarithmic search; at most eight entries are tested sequentially. */
sparse_reverse_search_tail:
  lhu t4, 0(t1)
  bgeu t4, s0, sparse_reverse_search_tail_stop
  addi t1, t1, 4
  bltu t1, t2, sparse_reverse_search_tail
  j sparse_reverse_search_miss
sparse_reverse_search_tail_stop:
  bne t4, s0, sparse_reverse_search_miss
  mv t3, t1
  j sparse_reverse_search_hit
#endif
sparse_reverse_search_miss:
  mv s5, t1
  j sparse_reverse_search_next
sparse_reverse_search_hit:
  addi s5, t3, 4
  lhu s1, 2(t3)
  add s1, s1, gp
  li t4, 1
  j sparse_reverse_apply
#endif
sparse_reverse_entry:
#if MNIST_SPARSE_MODE == 4
  beq s5, s6, sparse_reverse_received
  lhu s1, 0(s5)
  lhu t1, 2(s5)
  mv s0, s7
  slli t2, t1, 2
  add s5, s5, t2
  slli t1, t1, 5
  sub t1, t1, t2 /* Advance input state by skip * 28 bytes. */
  add s7, s7, t1
  beqz s1, sparse_reverse_entry
  add s1, s1, gp
#elif MNIST_SPARSE_MODE != 3
  lhu s0, 0(s5)
  lhu s1, 2(s5)
  add s0, s0, gp
  add s1, s1, gp
  addi s5, s5, 4
#endif
  lw t4, IN_FLAGS(s0)
  or t5, t4, s4
  beqz t5, sparse_reverse_entry_done
sparse_reverse_apply:
  lw t5, 0(s1)
#if MNIST_RULE != MT_RULE_ONE
  beqz t4, sparse_update_post
#if MNIST_RULE == MT_RULE_TWO
  mv a0, t5
  jal t6, fifth_root
  fmul.s t2, s2, a0
#else
  mv t2, s2
#endif
  fsub.s t5, t5, t2
  fmax.s t5, t5, zero
  fmin.s t5, t5, s8
#endif
sparse_update_post:
  beqz s4, sparse_update_store
  lw t4, IN_PRE_TRACE(s0)
  lw t1, IN_FLAGS(s0)
  beqz t1, sparse_update_pre_ready
  li t1, F_ONE
#if MNIST_RULE == MT_RULE_THREE
  mv t4, t1
#else
  fadd.s t4, t4, t1
#endif
sparse_update_pre_ready:
#if MNIST_RULE == MT_RULE_THREE
  fmul.s t2, s3, t4
#else
  fsub.s t4, t4, s10
  fsub.s a0, s8, t5
  jal t6, fifth_root
  fmul.s t2, s9, t4
  fmul.s t2, t2, a0
#endif
  fadd.s t5, t5, t2
  fmax.s t5, t5, zero
  fmin.s t5, t5, s8
sparse_update_store:
  sw t5, 0(s1)
sparse_reverse_entry_done:
#if MNIST_SPARSE_MODE == 4
  beqz s4, sparse_reverse_dense_next
  j sparse_reverse_entry
#elif MNIST_SPARSE_MODE != 3
#if MNIST_SPARSE_BINARY_REVERSE
  beqz s4, sparse_reverse_search_next
#endif
  bne s5, s6, sparse_reverse_entry
#endif
#if MNIST_SPARSE_CACHE_REVERSE && MNIST_SPARSE_MODE == 4 && MNIST_RULE != MT_RULE_ONE
  j sparse_reverse_received
/* Selected once before the reverse barrier when precisely one local pre fired.
 * Quiet depression needs neither its state pointer nor a fired-vector loop.
 */
sparse_reverse_one_pre:
  slli t1, a1, 1
  add t1, ra, t1
  lhu t1, 0(t1)
  add t1, t1, gp
#if MNIST_RULE == MT_RULE_THREE
  add t1, t1, a7
#else
  lw t2, H_REV_CURSOR(gp)
  add t1, t1, t2
#endif
  lhu s1, 0(t1)
  beqz s1, sparse_reverse_received
  add s1, s1, gp
  slli t1, a1, 2
  add t1, tp, t1
  lw s8, 0(t1)
  lw t5, 0(s1)
#if MNIST_RULE == MT_RULE_TWO
  mv s2, a2
  li ROOT_NEWTON, 0x3f99ca19
  mv a0, t5
  jal t6, fifth_root
  fmul.s t2, s2, a0
#else
  mv t2, a2
#endif
  fsub.s t5, t5, t2
  fmax.s t5, t5, zero
  fmin.s t5, t5, s8
  sw t5, 0(s1)
#endif
sparse_reverse_received:
#if MNIST_SPARSE_MC_DRAIN
  /* Diagnostic receipt count only; no expected count or completion decision. */
  addi t0, gp, H_RECEIVED
  li t1, 1
  amoadd.w zero, t1, (t0)
  j sparse_wait
#endif
#if MNIST_SPARSE_CACHE_REVERSE && MNIST_RULE == MT_RULE_THREE
  li t1, 1
  amoadd.w t2, t1, (sp)
  addi t2, t2, 1
  bgtu t2, s11, sparse_fail
  bne t2, s11, sparse_wait
#else
  addi t0, gp, H_RECEIVED
  li t1, 1
  amoadd.w t2, t1, (t0)
  addi t2, t2, 1
  lw t3, H_EXPECTED(gp)
  bgtu t2, t3, sparse_fail
  bne t2, t3, sparse_wait
#endif
sparse_reverse_maybe_done:
  lw t0, H_PHASE(gp)
  li t1, PH_REVERSE
  bne t0, t1, sparse_wait
  lw t0, H_RECEIVED(gp)
  lw t1, H_EXPECTED(gp)
  bne t0, t1, sparse_wait
sparse_reverse_drained:
  addi t0, gp, H_DONE
  li t1, 1
  amoswap.w t2, t1, (t0)
  bnez t2, sparse_wait
  lw t0, H_INPUT_BASE(gp)
  add t0, t0, gp
  lw t1, H_INPUT_COUNT(gp)
sparse_pre_commit_loop:
  lw t2, IN_FLAGS(t0)
  beqz t2, sparse_pre_commit_next
  li t3, F_ONE
#if MNIST_RULE != MT_RULE_THREE
  lw t2, IN_PRE_TRACE(t0)
  fadd.s t3, t3, t2
#endif
  sw t3, IN_PRE_TRACE(t0)
sparse_pre_commit_next:
  addi t0, t0, INPUT_SIZE
  addi t1, t1, -1
  bnez t1, sparse_pre_commit_loop
#if MNIST_SPARSE_COMPACT_SCHEDULE
  /* All reverse handlers have finished their weight/state reads before the
   * last received increment. Prepare the next input step before reducing,
   * but keep FF sends behind the global next-step boundary. */
  lw t0, H_PREPARED(gp)
  lw t1, H_WARMUP(gp)
  lw t2, H_TIMED(gp)
  add t1, t1, t2
  bgeu t0, t1, sparse_reverse_terminal
  li a0, 6
  li a1, 0
  YIELDI zero, TAG_PHASE
  j sparse_wait
sparse_reverse_terminal:
#endif
  /* The last update can be on either thread; the reducer is thread 0 only. */
  li a0, PH_REVERSE
  li a1, 1
  li a2, 0
  li a3, 0
  YIELDI zero, TAG_REDUCE
  j sparse_wait
#if MNIST_SPARSE_MC_DRAIN
/* Separate binomial reduction: input PUs contribute at reverse start, neuron
 * PUs after their last scatter. It may overlap ordinary reverse completion.
 * Reset after forwarding: no next-step contribution exists before the global
 * reverse/preparation barrier. Reports can beat local phase-start delivery.
 */
sparse_issued:
  li t0, X_BASE
  lw t1, X_ISSUED(t0)
  add t1, t1, a0
  lw t2, H_TREE_SIZE(gp)
  bgtu t1, t2, sparse_fail
  sw t1, X_ISSUED(t0)
  bne t1, t2, sparse_wait
  sw zero, X_ISSUED(t0)
  mv a0, t2
  lw t0, H_TREE_PARENT(gp)
  bnez t0, 1f
  li a0, 1
  YIELDI zero, TAG_IDLE_CONTROL
  j sparse_wait
1:
  SENDI t0, TAG_ISSUED
  j sparse_wait

/* Bounded priority-three installation chain, as in snn_main.S. The callback
 * is priority one and only launches a drain; it never changes neuron state.
 * The 257-cycle idle interval is an experimental transport-drain assumption,
 * not a hardware acknowledgement that downstream queues are empty.
 */
sparse_idle_control:
  bgtz a0, sparse_idle_install
  bnez a0, sparse_fail
  lw t0, H_ID(gp)
  addi t0, t0, 31
  SENDI t0, TAG_DRAIN
  j sparse_wait
sparse_idle_install:
  lw t2, H_ID(gp)
  addi t0, t2, -1
  srli t0, t0, 5
  li t1, 0x8001
  add t0, t0, t1
  li a0, (MNIST_SPARSE_IDLE_GATE << 16) | 0x401
  li t1, 0xcf0
  SEND t0, t1
  addi t0, t2, 32
  li t1, 512
  bgtu t0, t1, sparse_wait
  li a0, 1
  SENDI t0, TAG_IDLE_CONTROL
  j sparse_wait

/* Handler 14 is thread zero only and below reverse handlers 4/5. Pass the
 * domain token downward, then join a handler-15 token pinned to thread one.
 * Each thread disables only its own reverse enable bits after queued reverse
 * work. The join ensures a sibling's in-flight weight update has finished.
 */
sparse_drain_thread0:
  lw t0, H_ID(gp)
  addi t1, t0, -1
  andi t2, t1, 31
  beqz t2, 1f
  SENDI t1, TAG_DRAIN
1:
  lw t0, H_INPUT_COUNT(gp)
  beqz t0, sparse_prepare_empty /* Neuron PUs contribute only after this token. */
  li t0, 8
  csrc 0x714, t0
  csrc 0x715, t0
  li a0, 0
  li a1, -2
  SENDI zero, 0x40f
  j sparse_drain_join
sparse_drain_thread1:
  li t0, 16
  csrc 0x714, t0
  csrc 0x715, t0
sparse_drain_join:
  li t0, X_BASE + X_JOIN
  li t1, 1
  amoadd.w t2, t1, (t0)
  bgtu t2, t1, sparse_fail
  bne t2, t1, sparse_wait
  j sparse_reverse_drained
#endif
sparse_checksum:
  lw t0, H_FORWARD_BASE(gp)
  add t0, t0, gp
  lw t1, H_FORWARD_COUNT(gp)
  li a3, 0
  beqz t1, sparse_checksum_inputs
sparse_checksum_weights:
  lw t2, 4(t0)
  xor a3, a3, t2
  addi t0, t0, 8
  addi t1, t1, -1
  bnez t1, sparse_checksum_weights
sparse_checksum_inputs:
  lw t0, H_INPUT_BASE(gp)
  add t0, t0, gp
  lw t1, H_INPUT_COUNT(gp)
  beqz t1, sparse_checksum_done
sparse_checksum_input_loop:
  lw t2, IN_GLOBAL(t0)
  bgez t2, sparse_checksum_input_next
  lw t2, IN_RNG(t0)
  lw t3, IN_PRE_TRACE(t0)
  xor a3, a3, t2
  xor a3, a3, t3
sparse_checksum_input_next:
  addi t0, t0, INPUT_SIZE
  addi t1, t1, -1
  bnez t1, sparse_checksum_input_loop
sparse_checksum_done:
  li a2, 0
  j sparse_reduce_local
#include "mnist_neuron_pair.inc"
#if MNIST_RULE != MT_RULE_THREE
#include "mnist_fifth_root.inc"
#endif
