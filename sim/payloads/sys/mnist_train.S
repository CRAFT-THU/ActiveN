#ifndef MNIST_SPARSE_MODE
#define MNIST_SPARSE_MODE 0
#endif
#if MNIST_SPARSE_MODE
#include "mnist_sparse.inc"
#else
/*
 * Dense MNIST trace-STDP training for the 512-PU system.
 *
 * The complete timestep runtime is handwritten assembly. Only snn_init(),
 * which performs the one-time DRAM-to-SPM copy, is compiled from C.
 */

#ifndef MNIST_RULE
#define MNIST_RULE 3
#endif


#ifndef MNIST_RECURRENT_MODE
#define MNIST_RECURRENT_MODE 1
#endif

#ifndef MNIST_TRACE_BUNDLE
#define MNIST_TRACE_BUNDLE 0
#endif

#ifndef MNIST_PARALLEL_INPUT_PREPARE
#define MNIST_PARALLEL_INPUT_PREPARE 1
#endif

#ifndef MNIST_INPUT_DECAY_MODE
#define MNIST_INPUT_DECAY_MODE 1
#endif

#ifndef MNIST_FUSE_NEURON_UPDATE
#define MNIST_FUSE_NEURON_UPDATE 1
#endif

#ifndef MNIST_PIPELINE_INPUT_PREPARE
#define MNIST_PIPELINE_INPUT_PREPARE 1
#endif

#ifndef MNIST_HORIZONTAL_GROUP_COUNT
#define MNIST_HORIZONTAL_GROUP_COUNT 4
#endif

#if MNIST_RULE < 1 || MNIST_RULE > 3
#error "MNIST_RULE must be 1, 2, or 3"
#endif

#if MNIST_RECURRENT_MODE < 0 || MNIST_RECURRENT_MODE > 2
#error "MNIST_RECURRENT_MODE must be 0, 1, or 2"
#endif

#if MNIST_TRACE_BUNDLE != 0 && MNIST_TRACE_BUNDLE != 1
#error "MNIST_TRACE_BUNDLE must be 0 or 1"
#endif

#if MNIST_PARALLEL_INPUT_PREPARE != 0 && MNIST_PARALLEL_INPUT_PREPARE != 1
#error "MNIST_PARALLEL_INPUT_PREPARE must be 0 or 1"
#endif

#if MNIST_INPUT_DECAY_MODE < 0 || MNIST_INPUT_DECAY_MODE > 2
#error "MNIST_INPUT_DECAY_MODE must be 0, 1, or 2"
#endif

#if MNIST_FUSE_NEURON_UPDATE != 0 && MNIST_FUSE_NEURON_UPDATE != 1
#error "MNIST_FUSE_NEURON_UPDATE must be 0 or 1"
#endif

#if MNIST_PIPELINE_INPUT_PREPARE != 0 && MNIST_PIPELINE_INPUT_PREPARE != 1
#error "MNIST_PIPELINE_INPUT_PREPARE must be 0 or 1"
#endif

#if MNIST_PIPELINE_INPUT_PREPARE == 1 && MNIST_FUSE_NEURON_UPDATE == 0
#error "MNIST_PIPELINE_INPUT_PREPARE requires MNIST_FUSE_NEURON_UPDATE"
#endif

#if MNIST_HORIZONTAL_GROUP_COUNT != 4 && MNIST_HORIZONTAL_GROUP_COUNT != 8 && MNIST_HORIZONTAL_GROUP_COUNT != 16
#error "MNIST_HORIZONTAL_GROUP_COUNT must be 4, 8, or 16"
#endif

#if MNIST_HORIZONTAL_GROUP_COUNT >= 8 && MNIST_TRACE_BUNDLE != 0
#error "eight- and sixteen-way horizontal partitioning require unbundled traces"
#endif

#define SPM_BASE                    0x20000000
#define STOP_ADDR                   0x40000000
#define TIMER_ADDR                  0x40000004
#define PRINT_ADDR                  0x4000000c
#define MC_DST_BASE                 0x8001

#define MT_MAGIC                    0x4d543132
#define MT_PU_COUNT                 512
#define MT_MC_COUNT                 16
#define MT_INPUT_COUNT              784
#define MT_EXC_COUNT                400
#define MT_INH_COUNT                400

#define MT_RULE_ONE                 1
#define MT_RULE_TWO                 2
#define MT_RULE_THREE               3

#if MNIST_RULE == MT_RULE_THREE
/* No roots or stacked calls use sp after initialization in this rule. */
#define UPDATE_MAX sp
#else
#define UPDATE_MAX s6
#endif
#if MNIST_TRACE_BUNDLE == 0
#define TRACE_WAIT trace_wait
#else
#define TRACE_WAIT do_wfi
#endif

/* Control events may interleave with reverse events. For rule two, keep the
 * phase-cached root constants intact even when a trace beats local phase work.
 * Other rules retain their original register allocation and instruction bytes.
 */
#if MNIST_RULE == MT_RULE_TWO
#define PHASE_VALUE s0
#define PHASE_TMP t6
#define PHASE_HIGH s6
#define PHASE_COUNT1 s2
#define PHASE_COUNT2 s3
#define REDUCE_PARENT t6
#define REDUCE_EXTRA t1
#define REDUCE_PACK a3
#define ROOT_PHASE_TMP t3
#define DISPATCH_TMP t1
#define FRAME_TMP t3
#define WEIGHT_TMP t3
#define TRACE_TMP a2
#else
#define PHASE_VALUE s11
#define PHASE_TMP t0
#define PHASE_HIGH a4
#define PHASE_COUNT1 a5
#define PHASE_COUNT2 a6
#define REDUCE_PARENT t0
#define REDUCE_EXTRA a4
#define REDUCE_PACK a5
#define ROOT_PHASE_TMP t0
#define DISPATCH_TMP t0
#define FRAME_TMP t0
#define WEIGHT_TMP t0
#define TRACE_TMP t0
#endif
#if MNIST_RULE == MT_RULE_TWO && MNIST_RECURRENT_MODE == 0
#define IDLE_TMP a1
#else
#define IDLE_TMP t0
#endif

#define MT_REC_SCATTER              0
#define MT_REC_TREE                 1
#define MT_REC_TREE_AGGREGATE       2

#define PHASE_FRAME                 1
#define PHASE_INPUT_PREPARE         2
#define PHASE_INPUT_SEND            3
#define PHASE_NEURON_UPDATE         4
#define PHASE_WEIGHT_UPDATE         5
#define PHASE_FINAL_DRAIN           6

#if MNIST_FUSE_NEURON_UPDATE == 1
#define PHASE_NEURON_WORK           PHASE_INPUT_SEND
#else
#define PHASE_NEURON_WORK           PHASE_NEURON_UPDATE
#endif

#define INPUT_FIRED                 1
#define INPUT_GLOBAL_REPLICATED     (1 << 30)
#define INPUT_GLOBAL_PRIMARY        (1 << 31)
#define NEURON_FIRED                1
#define NEURON_FF_INPUT             2

#define INPUT_SIZE                  28
#define EXC_SIZE                    36
#define INH_SIZE                    148
#define TARGET_SIZE                 4
#define FIRED_VECTOR_SIZE           8
#define TRACE_SLOT_NONE             511
#define TRACE_BYTE_MASK             (TRACE_SLOT_NONE << 2)
#define TRACE_EXPECT_SHIFT          20
#define TRACE_COMMON_CFG            ((3 << 3) | 4)
#define FF_SOURCE_MASK              1023
#define FF_SOURCE_MARK              0x80000000
#define TRACE_POST_FIRED            0x80000000
#define NEXT_STEP_FLAG              0x80000000

/* mt_spm_header_t byte offsets. */
#define H_MAGIC                     0
#define H_RULE                      4
#define H_PU_ID                     8
#define H_NUM_PU                    12
#define H_NUM_MC                    16
#define H_LINE_SHIFT                20
#define H_INPUT_COUNT               24
#define H_INPUT_OFFSET              28
#define H_EXC_COUNT                 32
#define H_EXC_OFFSET                36
#define H_INH_COUNT                 40
#define H_INH_OFFSET                44
#define H_WEIGHT_COUNT              48
#define H_WEIGHT_OFFSET             52
#define H_FRAME_START               56
#define H_FRAME_LINES               120
#define H_FRAME_RECEIVED            184
#define H_PHASE                     188
#define H_CURSOR0                   192
#define H_TRACE_CFG                 196
#define H_FF_EXPECTED               200
#define H_FF_RECEIVED               204
#define H_DONE_SENT                 208
#define H_TRACE_RECEIVED            212
#define H_REDUCE_COUNT              216
#define H_REDUCE_VALUE              220
#define H_ROOT_IDLE_COUNT           224
#define H_ROOT_STEP_DONE            228
#define H_EXPECTED_CHECKSUM         232
#define H_WEIGHT_MAX                236
#define H_POTENTIATION_RATE         240
#define H_DEPRESSION_RATE           244
#define H_PRE_TARGET                248
#define H_REDUCE_PARENT             252
#define H_WARMUP_STEPS              256
#define H_TIMED_STEPS               260
#define H_COMPLETED_STEPS           264
#define H_INPUT_PU_COUNT            268
#define H_NEURON_PU_COUNT           272
#define H_INPUT_DEST_OFFSET         276
#define H_EXC_TARGET_OFFSET         280
#define H_RECURRENT_MODE            284
#define H_FIRED_COUNT               288
#define H_FF_START_RANK             292
#define H_FIRED_VECTOR_OFFSET       296
#define H_REDUCE_EXPECTED           300
#define H_OBSERVED_VECTOR_OFFSET    304
#define H_OBSERVED_COUNT            308
#define H_PHYSICAL_INPUT_PU_COUNT   312
#define H_TARGET_COUNT_PER_SEGMENT  316
#define H_POSTS_PER_SEGMENT         320
#define H_TARGET_SEGMENT_COUNT      324
#define H_INPUT_GROUP_MASK          328
#define H_FIRST_WEIGHT_SLOT         332
#define H_NEURON_GROUP              336
#define H_OWNED_POST_COUNT          340
#define H_TRACE_EXPECTED            344
#define H_NEURON_LOCAL_VALUE        348
#define H_REDUCE_VALUE_HI           352
#define H_REDUCE_VALUE_EXTRA        356
#define H_FIRED_VECTOR_BEGIN        360
#define H_FIRED_VECTOR_END          364
#define H_INPUT_SPLIT_COUNT        368
#define H_INPUT_SECOND_COUNT       372
#define H_INPUT_SECOND_BEGIN       376
#define H_INPUT_END                380
#define H_VECTOR_SECOND_BEGIN      384

/* Input record byte offsets. */
#define IN_GLOBAL                   0
#define IN_THRESHOLD                4
#define IN_PRE_TRACE                8
#define IN_RNG                      12
#define IN_FLAGS                    16
#define IN_WEIGHT_OFFSET            20
#define IN_WEIGHT_COUNT             24

/* Excitatory record byte offsets. */
#define EXC_GE                      0
#define EXC_GI                      4
#define EXC_V                       8
#define EXC_THETA                   12
#define EXC_POST1                   16
#define EXC_POST2                   20
#define EXC_REFRACTORY              24
#define EXC_FLAGS                   28
#define EXC_WEIGHT_SLOT             32

/* Inhibitory record byte offsets. */
#define INH_GE                      0
#define INH_GI                      4
#define INH_V                       8
#define INH_REFRACTORY              12
#define INH_FLAGS                   16
#define INH_CSR_START               20
#define INH_CSR_LINES               84

/* Active-message tags. Signed I-form aliases preserve the low 12 bits. */
#define TAG_FRAME_RESPONSE          0x400
#define TAG_NEURON_JOIN             0x001
#define TAG_FF_SPIKE                0xc02
#define TAG_FF_SPIKE_I              -1022
#define TAG_ROOT_COMPLETE           0x403
#define TAG_TRACE_EVENT             0x804
#define TAG_TRACE_EVENT_I           -2044
#define TAG_RECURRENT_RESPONSE      0x405
#define TAG_IDLE_RETURN             0x006
#define TAG_PHASE                   0x007
#define TAG_WORK                    0x008
#define TAG_REDUCE                  0x409
#define TAG_PHASE_START             0x00a
#define TAG_READY                   0xc0b
#define TAG_READY_I                 -1013
#define TAG_TRACE_SEND              0xc0d
#define TAG_TRACE_SEND_I            -1011
#define TAG_RECURRENT_SEND          0xc0e
#define TAG_RECURRENT_SEND_I        -1010
#define TAG_NEURON_WORKER           0x00f
#define TAG_IDLE_REGISTER           0xcf0
#define TAG_SCATTER_REQUEST         0x410

/* FP32 constants. */
#define F_ONE                       0x3f800000
#define F_TWO23                     0x4b000000
#define F_PRE_DECAY                 0x3f7cd1e6
#define F_POST1_DECAY               0x3f7cd1e6
#define F_POST2_DECAY               0x3f7e67ad
#define F_GE_HALF_DECAY             0x3f475f7d
#define F_GI_HALF_DECAY             0x3f61eb51
#define F_GE_DECAY                  0x3f1b4598
#define F_GI_DECAY                  0x3f475f7d
#define F_THETA_DECAY               0x3f7fffff
#define F_E_TO_I                    0x41266666
#define F_I_TO_E                    0x41cc0000
#define F_EXC_REST                  0xc2820000
#define F_INH_REST                  0xc2700000
#define F_EXC_INH_REV               0xc2c80000
#define F_INH_INH_REV               0xc2aa0000
#define F_EXC_THRESHOLD_OFFSET      0x42900000
#define F_INH_THRESHOLD             0xc2200000
#define F_EXC_RESET                 0xc2820000
#define F_INH_RESET                 0xc2340000
#define F_EXC_DT                    0x3ba3d70a
#define F_INH_DT                    0x3d4ccccd
#define F_THETA_INCREMENT           0x3d4ccccd

  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .macro YIELD dst_reg, tag_reg
  .insn r 0x0B, 0x5, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .macro SENDI dst_reg, tag_imm
  .insn i 0x2B, 0x4, x0, \dst_reg, \tag_imm
  .endm

  .macro YIELDI dst_reg, tag_imm
  .insn i 0x2B, 0x5, x0, \dst_reg, \tag_imm
  .endm

  .section .text
#if MNIST_RULE != MT_RULE_THREE
#include "mnist_fifth_root_config.inc"
#endif
  .option nopic
  .align 2
  .globl _start

_start:
  /* Reserve both private SPM stacks for startup; runtime reuses sp. */
  mv    t6, a0
  slli  t0, t6, 11
  li    sp, SPM_BASE + 0x3800
  add   sp, sp, t0
  bnez  t6, thread1_wait

  call  snn_init
  li    gp, SPM_BASE
  call  install_handlers
#if MNIST_RULE == MT_RULE_THREE
  /* Thread 0 does not execute setup_thread1/report_ready. */
  lw    UPDATE_MAX, H_WEIGHT_MAX(gp)
#endif

  /* Wake thread 1 only after SPM and all shared handler CSRs are ready. */
  li    t0, 12
  SEND  zero, t0
  j     do_wfi

thread1_wait:
  wfi
  j     thread1_wait

install_handlers:
  /* 0: frame scatter response, argc=2, both threads. */
  la    t0, frame_handler
  csrw  0x700, t0
  csrw  0x720, zero
  li    t0, (3 << 3) | 2
  csrw  0x710, t0

  /* 1: thread-1 phase-local result joins back onto thread 0. */
  la    t0, neuron_join_handler
  csrw  0x701, t0
  li    t0, (1 << 16) | 1
  csrw  0x721, t0
  li    t0, (1 << 3) | 1
  csrw  0x711, t0

  /* 2: feedforward spike, argc=3, thread 0, one final reduction. */
  la    t0, ff_spike_handler
  csrw  0x702, t0
  li    t0, (1 << 16) | 1
  csrw  0x722, t0
  li    t0, (1 << 3) | 3
  csrw  0x712, t0

  /* 3: root completion, thread 0, up to 16 sends plus phase start. */
  la    t0, root_complete_handler
  csrw  0x703, t0
  li    t0, (17 << 16) | 17
  csrw  0x723, t0
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  li    t0, (1 << 3) | 4
#else
  li    t0, (1 << 3) | 3
#endif
  csrw  0x713, t0

  /* 4: reverse trace, argc=4, enabled dynamically during weight update. */
  la    t0, trace_event_handler
  csrw  0x704, t0
  li    t0, (1 << 16) | 1
  csrw  0x724, t0
  li    t0, 4
  csrw  0x714, t0

#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  /* 5: recurrent scatter response, argc=3, thread 1 only. */
  la    t0, recurrent_handler
  csrw  0x705, t0
  csrw  0x725, zero
  li    t0, (2 << 3) | 3
  csrw  0x715, t0
#endif

  /* 6: MemIf idle notification, argc=1, thread 0. */
  la    t0, idle_handler
  csrw  0x706, t0
  li    t0, (1 << 16) | 1
  csrw  0x726, t0
  li    t0, (1 << 3) | 1
  csrw  0x716, t0

  /* 7: local phase work, thread 0. */
  la    t0, phase_handler
  csrw  0x707, t0
  li    t0, (1 << 16) | 1
  csrw  0x727, t0
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  li    t0, (1 << 3) | 4
#else
  li    t0, (1 << 3) | 3
#endif
  csrw  0x717, t0

  /* 8: feedforward sender continuation, argc=0, thread 0. */
  la    t0, work_handler
  csrw  0x708, t0
  li    t0, (1 << 16) | 1
  csrw  0x728, t0
  li    t0, (1 << 3) | 0
  csrw  0x718, t0

  /* 9: phase reduction, argc=4, thread 0. */
  la    t0, reduce_handler
  csrw  0x709, t0
  li    t0, (1 << 16) | 1
  csrw  0x729, t0
  li    t0, (1 << 3) | 4
  csrw  0x719, t0

  /* 10: phase-start tree, thread 0, nine children plus local work. */
  la    t0, phase_start_handler
  csrw  0x70a, t0
  li    t0, (10 << 16) | 10
  csrw  0x72a, t0
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  li    t0, (1 << 3) | 4
#else
  li    t0, (1 << 3) | 3
#endif
  csrw  0x71a, t0

  /* 11: boot-ready reduction at PU1, argc=0, thread 0. */
  la    t0, boot_ready_handler
  csrw  0x70b, t0
  li    t0, (32 << 16) | 32
  csrw  0x72b, t0
  li    t0, (1 << 3) | 0
  csrw  0x71b, t0

  /* 12: thread-1 setup, argc=0, thread 1 only. */
  la    t0, setup_thread1
  csrw  0x70c, t0
  li    t0, (1 << 16) | 1
  csrw  0x72c, t0
  li    t0, (2 << 3) | 0
  csrw  0x71c, t0

  /* 13: reverse sender continuation, argc=4, thread 0. */
  la    t0, trace_send_handler
  csrw  0x70d, t0
  li    t0, (1 << 16) | 1
  csrw  0x72d, t0
  li    t0, (1 << 3) | 4
  csrw  0x71d, t0

#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  /* 14: recurrent scatter sender continuation, argc=0, thread 0. */
  la    t0, recurrent_send_handler
  csrw  0x70e, t0
  li    t0, (1 << 16) | 1
  csrw  0x72e, t0
  li    t0, (1 << 3) | 0
  csrw  0x71e, t0
#endif

  /* 15: phase-local work, then the terminal XOR reduction, on thread 1. */
  la    t0, neuron_worker_handler
  csrw  0x70f, t0
  li    t0, (1 << 16) | 1
  csrw  0x72f, t0
  li    t0, (2 << 3) | 4
  csrw  0x71f, t0
  ret

/* Thread 1 establishes its private architectural constants, then reports ready. */
setup_thread1:
  li    gp, SPM_BASE
#if MNIST_TRACE_BUNDLE == 0
  li    s8, SPM_BASE + H_TRACE_RECEIVED
  li    s9, -1
  li    s10, 1
#endif
  j     report_ready

/* Validate all role/layout invariants once, outside every hot handler. */
report_ready:
  lw    t0, H_MAGIC(gp)
  li    t1, MT_MAGIC
  bne   t0, t1, fail_layout
  lw    t0, H_RULE(gp)
  li    t1, MNIST_RULE
  bne   t0, t1, fail_layout
  lw    t0, H_RECURRENT_MODE(gp)
  li    t1, MNIST_RECURRENT_MODE
  bne   t0, t1, fail_layout
  lw    t0, H_NUM_PU(gp)
  li    t1, MT_PU_COUNT
  bne   t0, t1, fail_layout
  lw    t0, H_NUM_MC(gp)
  li    t1, MT_MC_COUNT
  bne   t0, t1, fail_layout
  lw    t0, H_TIMED_STEPS(gp)
  beqz  t0, fail_layout
  lw    t0, H_PHYSICAL_INPUT_PU_COUNT(gp)
  beqz  t0, fail_layout
  li    t1, MT_PU_COUNT
  bgtu  t0, t1, fail_layout
  csrr  t0, 0xf14
  lw    t1, H_PU_ID(gp)
  bne   t0, t1, fail_layout
  addi  t1, t1, -1
  beqz  t1, report_reduce_expected_root
  neg   t2, t1
  and   t2, t2, t1
  j     report_reduce_expected_ready
report_reduce_expected_root:
  li    t2, MT_PU_COUNT
report_reduce_expected_ready:
  lw    t3, H_REDUCE_EXPECTED(gp)
  bne   t2, t3, fail_layout

  lw    t0, H_INPUT_COUNT(gp)
  lw    t1, H_EXC_COUNT(gp)
  lw    t2, H_INH_COUNT(gp)
  bne   t1, t2, fail_role
  li    t2, 2
  bgtu  t1, t2, fail_role
  beqz  t0, report_no_inputs
  bnez  t1, fail_role
  lw    t2, H_EXC_TARGET_OFFSET(gp)
  beqz  t2, fail_role
  lw    t2, H_INPUT_DEST_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_FIRED_VECTOR_OFFSET(gp)
  beqz  t2, fail_role
  lw    t2, H_OBSERVED_VECTOR_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_INPUT_GROUP_MASK(gp)
  beqz  t2, fail_role
#if MNIST_HORIZONTAL_GROUP_COUNT >= 8
  lw    t3, H_NEURON_GROUP(gp)
  li    t4, MNIST_HORIZONTAL_GROUP_COUNT - 1
  bgtu  t3, t4, fail_role
  li    t4, 1
  sll   t4, t4, t3
  bne   t2, t4, fail_role
#else
  andi  t3, t2, -16
  bnez  t3, fail_role
#endif
  lw    t2, H_TARGET_SEGMENT_COUNT(gp)
  beqz  t2, fail_role
#if MNIST_HORIZONTAL_GROUP_COUNT >= 8
  li    t3, 1
  bne   t2, t3, fail_role
#else
  li    t3, 4
  bgtu  t2, t3, fail_role
#endif
  lw    t3, H_POSTS_PER_SEGMENT(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT >= 8
  li    t4, MT_EXC_COUNT / MNIST_HORIZONTAL_GROUP_COUNT
  bne   t3, t4, fail_role
#else
  beqz  t3, fail_role
#endif
  mul   t4, t2, t3
  lw    t5, H_OWNED_POST_COUNT(gp)
  bne   t4, t5, fail_role
  li    t2, MT_EXC_COUNT
  bgtu  t5, t2, fail_role
  lw    t2, H_TARGET_COUNT_PER_SEGMENT(gp)
  beqz  t2, fail_role

  lw    t2, H_INPUT_OFFSET(gp)
  add   t2, t2, gp
  lw    t6, H_FIRST_WEIGHT_SLOT(gp)
  lw    t5, H_OWNED_POST_COUNT(gp)
  add   t6, t6, t5
  mv    t3, t0
report_input_loop:
  lw    t4, IN_WEIGHT_COUNT(t2)
  beqz  t4, fail_role
  li    t5, MT_EXC_COUNT
  bgtu  t4, t5, fail_role
  bltu  t4, t6, fail_role
  addi  t2, t2, INPUT_SIZE
  addi  t3, t3, -1
  bnez  t3, report_input_loop
  j     report_send_ready

report_no_inputs:
  beqz  t1, report_unused
  lw    t2, H_NEURON_GROUP(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT >= 8
  li    t3, MNIST_HORIZONTAL_GROUP_COUNT - 1
#else
  li    t3, 3
#endif
  bgtu  t2, t3, fail_role
  lw    t2, H_INPUT_PU_COUNT(gp)
  beqz  t2, fail_role
  lw    t2, H_INPUT_DEST_OFFSET(gp)
  beqz  t2, fail_role
  lw    t2, H_EXC_TARGET_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_FIRED_VECTOR_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_OBSERVED_VECTOR_OFFSET(gp)
  beqz  t2, fail_role
  lw    t2, H_EXC_OFFSET(gp)
  add   t2, t2, gp
  mv    t3, t1
report_exc_loop:
  lw    t4, EXC_WEIGHT_SLOT(t2)
  li    t5, MT_EXC_COUNT
  bgeu  t4, t5, fail_role
  addi  t2, t2, EXC_SIZE
  addi  t3, t3, -1
  bnez  t3, report_exc_loop
  j     report_send_ready

report_unused:
  lw    t2, H_INPUT_DEST_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_EXC_TARGET_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_FIRED_VECTOR_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_OBSERVED_VECTOR_OFFSET(gp)
  bnez  t2, fail_role

report_send_ready:
#if MNIST_RULE == MT_RULE_TWO
#if MNIST_FIFTH_ROOT_LUT
  lw    t1, H_INPUT_COUNT(gp)
  beqz  t1, report_root_lut_ready
  li    t1, ROOT_LUT_BASE
  lw    t1, 508(t1)
  li    t2, F_ONE
  bne   t1, t2, fail_layout
report_root_lut_ready:
#endif
  INIT_FIFTH_ROOT
  li    t1, 1
  SENDI t1, TAG_READY_I
#else
#if MNIST_RULE == MT_RULE_THREE
  /* Thread 1's private copy; thread 0 initialized its copy in _start. */
  lw    UPDATE_MAX, H_WEIGHT_MAX(gp)
#endif
  li    t0, 1
  SENDI t0, TAG_READY_I
#endif
  j     do_wfi

fail_layout:
  li    a0, 0xbad40000
  j     fail

fail_role:
  li    a0, 0xbad42000
  j     fail

/* a0 is the diagnostic code. */
fail:
  li    t0, PRINT_ADDR
  sw    a0, 0(t0)
  li    t1, 1
  li    t0, STOP_ADDR
  sw    t1, 0(t0)
  j     do_wfi

do_wfi:
  wfi
  j     do_wfi

/* Frame scatter response: a0=local input index, a1=Poisson threshold. */
frame_handler:
  lw    FRAME_TMP, H_INPUT_COUNT(gp)
  bgeu  a0, FRAME_TMP, fail_frame
  slli  t1, a0, 5
  slli  t2, a0, 2
  sub   t1, t1, t2
  lw    t2, H_INPUT_OFFSET(gp)
  add   t1, t1, t2
  add   t1, t1, gp
  sw    a1, IN_THRESHOLD(t1)
  li    FRAME_TMP, 1
  li    t1, SPM_BASE + H_FRAME_RECEIVED
  amoadd.w zero, FRAME_TMP, (t1)
  j     do_wfi

fail_frame:
  li    a0, 0xbad50000
  j     fail

/* PU1 counts all boot-ready messages, then scatters the static frame. */
boot_ready_handler:
  li    t0, 1
  li    t1, SPM_BASE + H_REDUCE_COUNT
  amoadd.w t2, t0, (t1)
  addi  t2, t2, 1
  li    t3, MT_PU_COUNT
  bne   t2, t3, do_wfi
  sw    zero, H_REDUCE_COUNT(gp)
  li    t0, PHASE_FRAME
  sw    t0, H_PHASE(gp)
  sw    zero, H_ROOT_IDLE_COUNT(gp)
  sw    zero, H_ROOT_STEP_DONE(gp)

  /* Register one phase-scoped idle notifier at every MC. */
  li    t0, MC_DST_BASE
  li    t1, MC_DST_BASE + MT_MC_COUNT
  li    t2, TAG_IDLE_REGISTER
boot_idle_loop:
  li    a0, (0x100 << 16) | TAG_IDLE_RETURN
  li    a1, 0
  li    a2, 0
  li    a3, 0
  SEND  t0, t2
  addi  t0, t0, 1
  bne   t0, t1, boot_idle_loop

  /* Issue the frame CSR/scatter rows that are nonempty. */
  li    t0, 0
  li    t1, MC_DST_BASE
  li    t6, TAG_SCATTER_REQUEST
boot_frame_loop:
  slli  t2, t0, 2
  add   t3, gp, t2
  lw    t4, H_FRAME_LINES(t3)
  beqz  t4, boot_frame_next
  lw    a0, H_FRAME_START(t3)
  slli  a1, t4, 16
  ori   a1, a1, TAG_FRAME_RESPONSE
  li    a2, 0
  li    a3, 0
  add   t5, t1, t0
  SEND  t5, t6
boot_frame_next:
  addi  t0, t0, 1
  li    t2, MT_MC_COUNT
  bne   t0, t2, boot_frame_loop
  j     do_wfi

/* MemIf idle return: a0=status. */
idle_handler:
  bnez  a0, fail_idle
  li    IDLE_TMP, 1
  li    t1, SPM_BASE + H_ROOT_IDLE_COUNT
  amoadd.w t2, IDLE_TMP, (t1)
  addi  t2, t2, 1
  lw    t3, H_NUM_MC(gp)
  bne   t2, t3, do_wfi
  lw    IDLE_TMP, H_PHASE(gp)
  li    t1, PHASE_FRAME
  beq   IDLE_TMP, t1, start_step
  lw    IDLE_TMP, H_ROOT_STEP_DONE(gp)
  bnez  IDLE_TMP, complete_step
  j     do_wfi

fail_idle:
  li    a0, 0xbad60000
  j     fail

/* Start a step and open the timer only after all warmup steps. */
start_step:
  lw    t0, H_COMPLETED_STEPS(gp)
  lw    t1, H_WARMUP_STEPS(gp)
  bltu  t0, t1, start_step_phase
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)
start_step_phase:
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  lw    t0, H_COMPLETED_STEPS(gp)
  beqz  t0, start_step_first_prepare
  li    a0, PHASE_INPUT_SEND
  lw    a1, H_FF_EXPECTED(gp)
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_PHASE_START
  j     do_wfi
start_step_first_prepare:
#endif
  li    a0, PHASE_INPUT_PREPARE
  li    a1, 0
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_PHASE_START
  j     do_wfi

/* Close one step, then start another or enter final checksum reduction. */
complete_step:
  lw    t0, H_COMPLETED_STEPS(gp)
  addi  t0, t0, 1
  sw    t0, H_COMPLETED_STEPS(gp)
  lw    t1, H_WARMUP_STEPS(gp)
  bleu  t0, t1, complete_timer_done
  li    t2, TIMER_ADDR
  sw    zero, 0(t2)
complete_timer_done:
  lw    t2, H_TIMED_STEPS(gp)
  add   t2, t2, t1
  bltu  t0, t2, start_step
  li    a0, PHASE_FINAL_DRAIN
  li    a1, 0
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_PHASE_START
  j     do_wfi

/*
 * Phase-start tree: a0=phase, a1/a2/a3=packed phase values.
 * s10/s11 are temporary only while this non-yielding handler fans out.
 */
phase_start_handler:
  mv    s10, a0
  mv    PHASE_VALUE, a1
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  mv    s9, a3
#endif
  sw    a2, H_NEURON_LOCAL_VALUE(gp)

  sw    s10, H_PHASE(gp)
  sw    zero, H_REDUCE_COUNT(gp)
  sw    zero, H_REDUCE_VALUE(gp)
  sw    zero, H_REDUCE_VALUE_HI(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  sw    zero, H_REDUCE_VALUE_EXTRA(gp)
#endif

  /* Start child subtrees before doing role-specific local setup. */
  mv    a0, s10
  mv    a1, PHASE_VALUE
  lw    a2, H_NEURON_LOCAL_VALUE(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  mv    a3, s9
#else
  li    a3, 0
#endif
  lw    t1, H_PU_ID(gp)
  /* No hot thread-0 path uses the ABI link register; cache this phase constant. */
  lw    ra, H_REDUCE_EXPECTED(gp)
  srli  t2, ra, 1

  beqz  t2, phase_start_initialize_local
phase_start_early_child_loop:
  add   t3, t1, t2
  SENDI t3, TAG_PHASE_START
  srli  t2, t2, 1
  bnez  t2, phase_start_early_child_loop

phase_start_initialize_local:
  /* Handler 4 is opened below only on input PUs after their pre-decay. */
  li    PHASE_TMP, 4
  csrw  0x714, PHASE_TMP

  /*
   * Initialize feedforward accumulators one phase early.  Completion of the
   * input-prepare reduction then guarantees this state exists before any PU
   * can emit a spike, even when phase-start subtrees have different latency.
   */
  li    PHASE_TMP, PHASE_INPUT_PREPARE
  bne   s10, PHASE_TMP, phase_start_not_input_prepare
  lw    s7, H_EXC_COUNT(gp)
  beqz  s7, phase_start_not_input_prepare

  /* The sender marks exactly one row per source PU; thread 0 appends directly. */
  sw    zero, H_OBSERVED_COUNT(gp)
  li    s2, 0
  li    s5, -1

  lw    s1, H_EXC_OFFSET(gp)
  add   s1, s1, gp
  lw    s3, EXC_GE(s1)
  li    s4, 0
  li    s6, 0
  sw    zero, EXC_FLAGS(s1)
  li    t1, 2
  bne   s7, t1, phase_start_not_input_prepare
  lw    s4, EXC_SIZE + EXC_GE(s1)
  sw    zero, EXC_SIZE + EXC_FLAGS(s1)

phase_start_not_input_prepare:
  li    PHASE_TMP, PHASE_INPUT_SEND
  bne   s10, PHASE_TMP, phase_start_not_input_send
  sw    PHASE_VALUE, H_FF_EXPECTED(gp)
  li    PHASE_TMP, MT_INPUT_COUNT
  bgtu  PHASE_VALUE, PHASE_TMP, fail_phase_start
  mv    s5, PHASE_VALUE

phase_start_not_input_send:
  li    PHASE_TMP, PHASE_NEURON_WORK
  bne   s10, PHASE_TMP, phase_start_not_neuron
#if MNIST_TRACE_BUNDLE == 0
#if MNIST_RULE == MT_RULE_ONE
  sw    zero, H_TRACE_RECEIVED(gp)
#else
  lw    PHASE_TMP, H_FIRED_COUNT(gp)
  beqz  PHASE_TMP, phase_start_trace_count_up
  lw    PHASE_TMP, H_OWNED_POST_COUNT(gp)
  beqz  PHASE_TMP, fail_phase_start
  li    t1, MT_EXC_COUNT
  bgtu  PHASE_TMP, t1, fail_phase_start
  sw    PHASE_TMP, H_TRACE_RECEIVED(gp)
  j     phase_start_trace_count_ready
phase_start_trace_count_up:
  sw    zero, H_TRACE_RECEIVED(gp)
phase_start_trace_count_ready:
#endif
#else
  sw    zero, H_TRACE_RECEIVED(gp)
#endif
  sw    zero, H_DONE_SENT(gp)

  lw    PHASE_TMP, H_INPUT_COUNT(gp)
  beqz  PHASE_TMP, phase_start_not_neuron

  /* Mode 0 decays here; modes 1 and 2 move decay into later local phases. */
#if MNIST_INPUT_DECAY_MODE == 0
  lw    t1, H_INPUT_OFFSET(gp)
  add   t1, t1, gp
  li    t2, F_PRE_DECAY
phase_start_pre_decay_loop:
  lw    t3, IN_PRE_TRACE(t1)
  fmul.s t3, t3, t2
  sw    t3, IN_PRE_TRACE(t1)
  addi  t1, t1, INPUT_SIZE
  addi  PHASE_TMP, PHASE_TMP, -1
  bnez  PHASE_TMP, phase_start_pre_decay_loop
#endif

  /* Select the per-PU reverse entry once instead of testing every message. */
#if MNIST_RULE == MT_RULE_ONE
  la    PHASE_TMP, trace_event_fast_handler
  li    t4, TRACE_COMMON_CFG
#else
  li    t4, TRACE_COMMON_CFG
  lw    t1, H_FIRED_COUNT(gp)
  lw    t2, H_INPUT_COUNT(gp)
  bgtu  t1, t2, fail_trace_list
  lw    t2, H_FIRED_VECTOR_OFFSET(gp)
  beqz  t2, fail_phase_start
  add   t2, t2, gp
  /* Both SMT receivers read these bounds until all local updates finish. */
  sw    t2, H_FIRED_VECTOR_BEGIN(gp)
  slli  t3, t1, 3
  add   t3, t3, t2
  sw    t3, H_FIRED_VECTOR_END(gp)
  la    PHASE_TMP, trace_event_fast_handler
  beqz  t1, phase_start_trace_entry_ready
  lw    t2, 0(t2)
  sw    t2, H_CURSOR0(gp)
  li    t2, 1
  bne   t1, t2, phase_start_trace_multi_pre
  la    PHASE_TMP, trace_event_one_pre_handler
  j     phase_start_trace_entry_ready
phase_start_trace_multi_pre:
  la    PHASE_TMP, trace_event_handler
  /* Per-thread registers and disjoint columns leave only the completion AMO shared. */
  li    t4, TRACE_COMMON_CFG
phase_start_trace_entry_ready:
#endif
  sw    t4, H_TRACE_CFG(gp)
  csrw  0x704, PHASE_TMP
  csrw  0x714, t4

phase_start_not_neuron:
  li    PHASE_TMP, PHASE_WEIGHT_UPDATE
  bne   s10, PHASE_TMP, phase_start_fanout
  sw    zero, H_DONE_SENT(gp)
  sw    zero, H_TRACE_EXPECTED(gp)

#if MNIST_TRACE_BUNDLE == 0
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  /*
   * Sixteen 25-neuron group counts use five bits each:
   *   low  = inh[9:0], group0<<10, ..., group3<<25
   *   high = group4, group5<<5, ..., group9<<25, next-step<<31
   *   extra= group10, group11<<5, ..., group15<<25
   */
  srli  PHASE_TMP, PHASE_VALUE, 30
  bnez  PHASE_TMP, fail_phase_start
  andi  t1, PHASE_VALUE, 1023
  li    PHASE_TMP, MT_INH_COUNT
  bgtu  t1, PHASE_TMP, fail_phase_start
  lw    PHASE_HIGH, H_NEURON_LOCAL_VALUE(gp)
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  li    t5, 0x7fffffff
  and   PHASE_HIGH, PHASE_HIGH, t5
#endif
  srli  t5, PHASE_HIGH, 30
  bnez  t5, fail_phase_start
  srli  t5, s9, 30
  bnez  t5, fail_phase_start
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  bnez  t1, fail_phase_start
#endif

  lw    PHASE_TMP, H_INPUT_COUNT(gp)
  beqz  PHASE_TMP, phase_start_fanout
  lw    t4, H_NEURON_GROUP(gp)
  li    t5, 4
  bltu  t4, t5, phase_start_trace_expect_low_word_16
  li    t5, 10
  bltu  t4, t5, phase_start_trace_expect_high_word_16
  addi  t4, t4, -10
  li    t5, 5
  mul   t4, t4, t5
  srl   t4, s9, t4
  j     phase_start_trace_expect_value_ready_16
phase_start_trace_expect_high_word_16:
  addi  t4, t4, -4
  li    t5, 5
  mul   t4, t4, t5
  srl   t4, PHASE_HIGH, t4
  j     phase_start_trace_expect_value_ready_16
phase_start_trace_expect_low_word_16:
  li    t5, 5
  mul   t4, t4, t5
  addi  t4, t4, 10
  srl   t4, PHASE_VALUE, t4
phase_start_trace_expect_value_ready_16:
  andi  t4, t4, 31
  li    t5, MT_EXC_COUNT / 16
  bgtu  t4, t5, fail_phase_start
  sw    t4, H_TRACE_EXPECTED(gp)
#elif MNIST_HORIZONTAL_GROUP_COUNT == 8
  /*
   * Eight 50-neuron group counts use six bits each.  The low word keeps the
   * inhibitory count in bits 9:0 for the simulator's activity observer:
   *   low  = inh[9:0], group0<<10, group1<<16, group2<<22
   *   high = group3, group4<<6, ..., group7<<24, next-step<<31
   */
  srli  PHASE_TMP, PHASE_VALUE, 28
  bnez  PHASE_TMP, fail_phase_start
  andi  t1, PHASE_VALUE, 1023
  li    PHASE_TMP, MT_INH_COUNT
  bgtu  t1, PHASE_TMP, fail_phase_start
  lw    PHASE_HIGH, H_NEURON_LOCAL_VALUE(gp)
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  li    t5, 0x7fffffff
  and   PHASE_HIGH, PHASE_HIGH, t5
#endif
  srli  t5, PHASE_HIGH, 30
  bnez  t5, fail_phase_start
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  bnez  t1, fail_phase_start
#endif

  lw    PHASE_TMP, H_INPUT_COUNT(gp)
  beqz  PHASE_TMP, phase_start_fanout
  lw    t4, H_NEURON_GROUP(gp)
  li    t5, 3
  bgeu  t4, t5, phase_start_trace_expect_high_word
  li    t5, 6
  mul   t4, t4, t5
  addi  t4, t4, 10
  srl   t4, PHASE_VALUE, t4
  j     phase_start_trace_expect_value_ready
phase_start_trace_expect_high_word:
  addi  t4, t4, -3
  li    t5, 6
  mul   t4, t4, t5
  srl   t4, PHASE_HIGH, t4
phase_start_trace_expect_value_ready:
  andi  t4, t4, 63
  li    t5, MT_EXC_COUNT / 8
  bgtu  t4, t5, fail_phase_start
  sw    t4, H_TRACE_EXPECTED(gp)
#else
  srli  PHASE_TMP, PHASE_VALUE, 30
  bnez  PHASE_TMP, fail_phase_start
  andi  t1, PHASE_VALUE, 1023
  li    PHASE_TMP, MT_INH_COUNT
  bgtu  t1, PHASE_TMP, fail_phase_start
  srli  t2, PHASE_VALUE, 10
  andi  t2, t2, 1023
  li    PHASE_TMP, MT_EXC_COUNT
  bgtu  t2, PHASE_TMP, fail_phase_start
  srli  t3, PHASE_VALUE, 20
  andi  t3, t3, 1023
  bgtu  t3, PHASE_TMP, fail_phase_start
  lw    PHASE_HIGH, H_NEURON_LOCAL_VALUE(gp)
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  li    t5, 0x7fffffff
  and   PHASE_HIGH, PHASE_HIGH, t5
#endif
  srli  t5, PHASE_HIGH, 20
  bnez  t5, fail_phase_start
  andi  PHASE_COUNT1, PHASE_HIGH, 1023
  bgtu  PHASE_COUNT1, PHASE_TMP, fail_phase_start
  srli  PHASE_COUNT2, PHASE_HIGH, 10
  andi  PHASE_COUNT2, PHASE_COUNT2, 1023
  bgtu  PHASE_COUNT2, PHASE_TMP, fail_phase_start
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  bnez  t1, fail_phase_start
#endif

  lw    PHASE_TMP, H_INPUT_COUNT(gp)
  beqz  PHASE_TMP, phase_start_fanout
  lw    PHASE_TMP, H_INPUT_GROUP_MASK(gp)
  beqz  PHASE_TMP, fail_phase_start
  li    t4, 0
  andi  t5, PHASE_TMP, 1
  beqz  t5, phase_start_trace_expect_group1
  add   t4, t4, t2
phase_start_trace_expect_group1:
  andi  t5, PHASE_TMP, 2
  beqz  t5, phase_start_trace_expect_group2
  add   t4, t4, t3
phase_start_trace_expect_group2:
  andi  t5, PHASE_TMP, 4
  beqz  t5, phase_start_trace_expect_group3
  add   t4, t4, PHASE_COUNT1
phase_start_trace_expect_group3:
  andi  t5, PHASE_TMP, 8
  beqz  t5, phase_start_trace_expect_ready
  add   t4, t4, PHASE_COUNT2
phase_start_trace_expect_ready:
  andi  t5, PHASE_TMP, -16
  bnez  t5, fail_phase_start
  sw    t4, H_TRACE_EXPECTED(gp)
#endif
#else
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  slli  PHASE_TMP, PHASE_VALUE, 16
  bnez  PHASE_TMP, fail_phase_start
#else
  li    PHASE_TMP, MT_INH_COUNT
  slli  t1, PHASE_VALUE, 16
  srli  t1, t1, 16
  bgtu  t1, PHASE_TMP, fail_phase_start
#endif
  srli  t1, PHASE_VALUE, 16
  li    PHASE_TMP, MT_EXC_COUNT
  bgtu  t1, PHASE_TMP, fail_phase_start
#endif

  /* Input PUs keep the already-decayed, already-counting handler open. */
  lw    PHASE_TMP, H_INPUT_COUNT(gp)
  beqz  PHASE_TMP, phase_start_fanout
  lw    PHASE_TMP, H_TRACE_CFG(gp)
  csrw  0x714, PHASE_TMP

phase_start_fanout:
  mv    a0, s10
  mv    a1, PHASE_VALUE
  lw    a2, H_NEURON_LOCAL_VALUE(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  mv    a3, s9
#else
  li    a3, 0
#endif
#if MNIST_TRACE_BUNDLE == 0
  /* Reverse events may run before the local phase handler after this yield. */
  lw    PHASE_TMP, H_INPUT_COUNT(gp)
  beqz  PHASE_TMP, phase_start_cursor_ready
  addi  s8, gp, H_TRACE_RECEIVED
  li    s9, -1
  li    s10, 1
phase_start_cursor_ready:
#endif
  YIELDI zero, TAG_PHASE
  j     do_wfi

fail_phase_start:
  li    a0, 0xbadb0000
  j     fail

  .balign 64

/*
 * Phase reduction: a0=phase, a1=subtree size, a2=low value, a3=high value.
 * Full-16 carries the extra word in a0[31:3] plus a1[31].
 * The tree count itself serializes this thread-0-only handler.
 */
reduce_handler:
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  srli  t6, a0, 3
  srli  REDUCE_EXTRA, a1, 31
  slli  REDUCE_EXTRA, REDUCE_EXTRA, 29
  or    t6, t6, REDUCE_EXTRA
  /* Bits 30:10 are unused; bit 31 is the split extra-word carry bit. */
  slli  REDUCE_EXTRA, a1, 1
  srli  REDUCE_EXTRA, REDUCE_EXTRA, 11
  bnez  REDUCE_EXTRA, fail_reduce
  andi  a0, a0, 7
  andi  a1, a1, 1023
#endif
  mv    t2, ra
  lw    t3, H_REDUCE_COUNT(gp)
  add   t3, t3, a1
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  /* Scatter ingress is external to the phase tree, so retain its guard. */
  bgtu  t3, t2, fail_reduce
#endif
  sw    t3, H_REDUCE_COUNT(gp)

  lw    t4, H_REDUCE_VALUE(gp)
#if MNIST_RECURRENT_MODE != MT_REC_SCATTER
  /* The terminal XOR reduction uses handler 15, so every phase here is a sum. */
  add   t4, t4, a2
#elif MNIST_PIPELINE_INPUT_PREPARE == 1
  li    t5, PHASE_FINAL_DRAIN
  beq   a0, t5, reduce_xor_value
  add   t4, t4, a2
  j     reduce_store_value
reduce_xor_value:
  xor   t4, t4, a2
#else
  li    t5, PHASE_INPUT_PREPARE
  beq   a0, t5, reduce_add_value
  li    t5, PHASE_NEURON_WORK
  beq   a0, t5, reduce_add_value
  xor   t4, t4, a2
  j     reduce_store_value
reduce_add_value:
  add   t4, t4, a2
#endif
reduce_store_value:
  sw    t4, H_REDUCE_VALUE(gp)
  lw    t5, H_REDUCE_VALUE_HI(gp)
  add   t5, t5, a3
  sw    t5, H_REDUCE_VALUE_HI(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  lw    REDUCE_EXTRA, H_REDUCE_VALUE_EXTRA(gp)
  add   REDUCE_EXTRA, REDUCE_EXTRA, t6
  sw    REDUCE_EXTRA, H_REDUCE_VALUE_EXTRA(gp)
#endif
  bne   t3, t2, do_wfi

  lw    REDUCE_PARENT, H_REDUCE_PARENT(gp)
  bnez  REDUCE_PARENT, reduce_send_parent
  mv    a1, t4
  mv    a2, t5
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  mv    a3, REDUCE_EXTRA
#else
  li    a3, 0
#endif
  YIELDI zero, TAG_ROOT_COMPLETE
  j     do_wfi

reduce_send_parent:
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  srli  REDUCE_PACK, REDUCE_EXTRA, 29
  slli  REDUCE_PACK, REDUCE_PACK, 31
  or    a1, t2, REDUCE_PACK
  slli  REDUCE_PACK, REDUCE_EXTRA, 3
  or    a0, a0, REDUCE_PACK
#else
  mv    a1, t2
#endif
  mv    a2, t4
  mv    a3, t5
  SENDI REDUCE_PARENT, TAG_REDUCE
  j     do_wfi

fail_reduce:
  li    a0, 0xbadc0000
  j     fail

#if MNIST_RECURRENT_MODE != MT_REC_SCATTER
final_reduce_dispatch:
  j     final_reduce_handler
#endif
  .balign 64

/* Root phase completion: a0=phase, a1/a2=low/high aggregate value. */
root_complete_handler:
  li    ROOT_PHASE_TMP, PHASE_INPUT_SEND
#if MNIST_FUSE_NEURON_UPDATE == 1
  beq   a0, ROOT_PHASE_TMP, root_neuron_done
#else
  beq   a0, ROOT_PHASE_TMP, root_start_neuron
#endif
  li    ROOT_PHASE_TMP, PHASE_WEIGHT_UPDATE
  beq   a0, ROOT_PHASE_TMP, root_weight_done
  li    ROOT_PHASE_TMP, PHASE_INPUT_PREPARE
  beq   a0, ROOT_PHASE_TMP, root_start_input_send
#if MNIST_FUSE_NEURON_UPDATE == 0
  li    ROOT_PHASE_TMP, PHASE_NEURON_UPDATE
  beq   a0, ROOT_PHASE_TMP, root_neuron_done
#endif
  li    ROOT_PHASE_TMP, PHASE_FINAL_DRAIN
  beq   a0, ROOT_PHASE_TMP, root_final_done
  j     fail_root

root_start_input_send:
  li    a0, PHASE_INPUT_SEND
  /* a1 already holds the global fired-input count. */
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_PHASE_START
  j     do_wfi

#if MNIST_FUSE_NEURON_UPDATE == 0
root_start_neuron:
  li    a0, PHASE_NEURON_UPDATE
  li    a1, 0
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_PHASE_START
  j     do_wfi
#endif

root_neuron_done:
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  /* Re-register phase-scoped MC idleness for recurrent traffic. */
  sw    zero, H_ROOT_IDLE_COUNT(gp)
  sw    zero, H_ROOT_STEP_DONE(gp)
  li    ROOT_PHASE_TMP, MC_DST_BASE
  li    t1, MC_DST_BASE + MT_MC_COUNT
  li    t2, TAG_IDLE_REGISTER
root_idle_register_loop:
  li    a0, (0x100 << 16) | TAG_IDLE_RETURN
  li    a1, 0
  li    a2, 0
  li    a3, 0
  SEND  ROOT_PHASE_TMP, t2
  addi  ROOT_PHASE_TMP, ROOT_PHASE_TMP, 1
  bne   ROOT_PHASE_TMP, t1, root_idle_register_loop
#endif
  li    a0, PHASE_WEIGHT_UPDATE
  /* Restore both words after optional idle-registration payload traffic. */
  lw    a1, H_REDUCE_VALUE(gp)
  lw    a2, H_REDUCE_VALUE_HI(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  lw    a3, H_REDUCE_VALUE_EXTRA(gp)
#endif
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  lw    ROOT_PHASE_TMP, H_COMPLETED_STEPS(gp)
  addi  ROOT_PHASE_TMP, ROOT_PHASE_TMP, 1
  lw    t1, H_WARMUP_STEPS(gp)
  lw    t2, H_TIMED_STEPS(gp)
  add   t1, t1, t2
  bgeu  ROOT_PHASE_TMP, t1, root_weight_last_step
  li    ROOT_PHASE_TMP, NEXT_STEP_FLAG
  or    a2, a2, ROOT_PHASE_TMP
root_weight_last_step:
#endif
#if MNIST_HORIZONTAL_GROUP_COUNT != 16
  li    a3, 0
#endif
  YIELDI zero, TAG_PHASE_START
  j     do_wfi

root_weight_done:
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  sw    a1, H_FF_EXPECTED(gp)
#endif
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  li    ROOT_PHASE_TMP, 1
  sw    ROOT_PHASE_TMP, H_ROOT_STEP_DONE(gp)
  lw    t1, H_ROOT_IDLE_COUNT(gp)
  lw    t2, H_NUM_MC(gp)
  bne   t1, t2, do_wfi
#endif
  j     complete_step

root_final_done:
  li    ROOT_PHASE_TMP, TIMER_ADDR
  sw    zero, 0(ROOT_PHASE_TMP)
  li    ROOT_PHASE_TMP, PRINT_ADDR
  sw    a1, 0(ROOT_PHASE_TMP)
  lw    t1, H_EXPECTED_CHECKSUM(gp)
  beqz  t1, root_success
  bne   t1, a1, root_checksum_fail
root_success:
  li    ROOT_PHASE_TMP, STOP_ADDR
  sw    zero, 0(ROOT_PHASE_TMP)
  j     do_wfi

root_checksum_fail:
  li    a0, 0xbadf0000
  j     fail

fail_root:
  li    a0, 0xbade0000
  j     fail

/* Terminal helpers: a0=phase, a2/a3=low/high value. */
reduce_local:
  li    a3, 0
reduce_local_pair:
  li    a1, 1
  YIELDI zero, TAG_REDUCE
  j     do_wfi

/* Local work for one phase: a0=phase, a1=phase value. */
phase_handler:
  li    DISPATCH_TMP, PHASE_INPUT_SEND
  beq   a0, DISPATCH_TMP, phase_input_send
  li    DISPATCH_TMP, PHASE_WEIGHT_UPDATE
  beq   a0, DISPATCH_TMP, phase_weight_update
  li    DISPATCH_TMP, PHASE_INPUT_PREPARE
  beq   a0, DISPATCH_TMP, phase_input_prepare
#if MNIST_FUSE_NEURON_UPDATE == 0
  li    DISPATCH_TMP, PHASE_NEURON_UPDATE
  beq   a0, DISPATCH_TMP, phase_neuron_update
#endif
  li    DISPATCH_TMP, PHASE_FINAL_DRAIN
  beq   a0, DISPATCH_TMP, phase_final_drain
  li    a0, 0xbad90000
  j     fail

/* Advance input state and build a flat vector of fired weight rows. */
phase_input_prepare:
phase_input_prepare_wait_frame:
  lw    t0, H_FRAME_RECEIVED(gp)
  lw    t1, H_INPUT_COUNT(gp)
  bne   t0, t1, phase_input_prepare_wait_frame
  sw    zero, H_FF_EXPECTED(gp)
  sw    zero, H_FF_RECEIVED(gp)
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  lw    t0, H_PHASE(gp)
  li    t2, PHASE_WEIGHT_UPDATE
  beq   t0, t2, input_prepare_keep_done
#endif
  sw    zero, H_DONE_SENT(gp)
input_prepare_keep_done:
  sw    zero, H_FIRED_COUNT(gp)
#if MNIST_PARALLEL_INPUT_PREPARE == 1
  li    t6, 0
  li    a6, 0
  beqz  t1, input_prepare_done
  lw    t2, H_INPUT_OFFSET(gp)
  add   t2, t2, gp
  lw    a5, H_FIRED_VECTOR_OFFSET(gp)
  beqz  a5, fail_input_vector
  add   a5, a5, gp

  /* Contiguous halves preserve serial ordering after the join compacts them. */
  li    t0, 2
  bltu  t1, t0, input_prepare_serial
  lw    t1, H_INPUT_SPLIT_COUNT(gp)
  li    a0, 0
  li    a1, 0
  li    a2, 0
  li    a3, 0
  SENDI zero, TAG_NEURON_WORKER
  jal   s7, input_prepare_range
  sw    t6, H_FIRED_COUNT(gp)
  sw    a6, H_NEURON_LOCAL_VALUE(gp)
  j     do_wfi

input_prepare_serial:
  jal   s7, input_prepare_range
  j     input_prepare_done
#else
  li    t6, 0
  li    a6, 0
  beqz  t1, input_prepare_done
  lw    t2, H_INPUT_OFFSET(gp)
  add   t2, t2, gp
  lw    a5, H_FIRED_VECTOR_OFFSET(gp)
  beqz  a5, fail_input_vector
  add   a5, a5, gp
#if MNIST_INPUT_DECAY_MODE == 2
  li    a7, F_PRE_DECAY
#endif
  li    t3, 0
input_prepare_serial_loop:
  lw    t4, IN_RNG(t2)
  slli  t5, t4, 13
  xor   t4, t4, t5
  srli  t5, t4, 17
  xor   t4, t4, t5
  slli  t5, t4, 5
  xor   t4, t4, t5
  sw    t4, IN_RNG(t2)

#if MNIST_INPUT_DECAY_MODE == 2
  lw    t5, IN_PRE_TRACE(t2)
  fmul.s t5, t5, a7
  sw    t5, IN_PRE_TRACE(t2)
#endif
  lw    t5, IN_THRESHOLD(t2)
  bltu  t5, t4, input_prepare_not_fired
input_prepare_fired:
  li    a4, INPUT_FIRED
  sw    a4, IN_FLAGS(t2)
  lw    a4, IN_GLOBAL(t2)
  bgez  a4, input_prepare_not_primary
  addi  a6, a6, 1
input_prepare_not_primary:
  lw    a4, IN_WEIGHT_OFFSET(t2)
  add   a4, a4, gp
  sw    a4, 0(a5)
  sw    t2, 4(a5)
  addi  a5, a5, FIRED_VECTOR_SIZE
  addi  t6, t6, 1
  j     input_prepare_next
input_prepare_not_fired:
  /* Weight-phase completion cleared every flag before this preparation. */
input_prepare_next:
  addi  t3, t3, 1
  addi  t2, t2, INPUT_SIZE
  bne   t3, t1, input_prepare_serial_loop
#endif

input_prepare_done:
  sw    t6, H_FIRED_COUNT(gp)
  mv    a2, a6
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  lw    a0, H_PHASE(gp)
#else
  li    a0, PHASE_INPUT_PREPARE
#endif
  j     reduce_local

#if MNIST_PARALLEL_INPUT_PREPARE == 1
/* t1=count, t2=input records, a5=private vector segment; return t6/a6 counts. */
input_prepare_range:
  li    t6, 0
  li    a6, 0
#if MNIST_INPUT_DECAY_MODE == 2
  li    a7, F_PRE_DECAY
#endif
  /* t2 identifies the immutable first or second contiguous range. */
  lw    a0, H_INPUT_SECOND_BEGIN(gp)
  bltu  t2, a0, input_prepare_range_loop
  lw    a0, H_INPUT_END(gp)
input_prepare_range_loop:
  lw    t4, IN_RNG(t2)
  slli  t5, t4, 13
  xor   t4, t4, t5
  srli  t5, t4, 17
  xor   t4, t4, t5
  slli  t5, t4, 5
  xor   t4, t4, t5
  sw    t4, IN_RNG(t2)

#if MNIST_INPUT_DECAY_MODE == 2
  lw    t5, IN_PRE_TRACE(t2)
  fmul.s t5, t5, a7
  sw    t5, IN_PRE_TRACE(t2)
#endif
  lw    t5, IN_THRESHOLD(t2)
  bltu  t5, t4, input_prepare_range_not_fired
input_prepare_range_fired:
  li    a4, INPUT_FIRED
  sw    a4, IN_FLAGS(t2)
  lw    a4, IN_GLOBAL(t2)
  bgez  a4, input_prepare_range_not_primary
  addi  a6, a6, 1
input_prepare_range_not_primary:
  lw    a4, IN_WEIGHT_OFFSET(t2)
  add   a4, a4, gp
  sw    a4, 0(a5)
  sw    t2, 4(a5)
  addi  a5, a5, FIRED_VECTOR_SIZE
  addi  t6, t6, 1
  j     input_prepare_range_next
input_prepare_range_not_fired:
  /* Weight-phase completion cleared every flag before this preparation. */
input_prepare_range_next:
  addi  t2, t2, INPUT_SIZE
  bne   t2, a0, input_prepare_range_loop
  jr    s7
#endif

fail_input_vector:
  csrr  t0, 0xf14
  li    a0, 0xbad82000
  or    a0, a0, t0
  j     fail

/* Input PUs send; neuron PUs wait for the globally known physical count. */
phase_input_send:
  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, input_send_non_input
  lw    t1, H_FIRED_COUNT(gp)
  beqz  t1, input_send_done
  bgtu  t1, t0, fail_input_work
  lw    a5, H_FIRED_VECTOR_OFFSET(gp)
  beqz  a5, fail_input_work
  add   a5, a5, gp
  addi  t2, t1, -1
#if MNIST_HORIZONTAL_GROUP_COUNT >= 8
  mv    s1, t2
#endif
  slli  t2, t2, 3
  add   a4, a5, t2
  lw    s8, 0(a4)
  csrr  t0, 0xf14
  slli  s11, t0, 16
  li    t0, FF_SOURCE_MARK
  or    s11, s11, t0

input_work_start_row:
  lw    s0, H_EXC_TARGET_OFFSET(gp)
  beqz  s0, fail_input_work
  add   s0, s0, gp
  lw    s7, H_FIRST_WEIGHT_SLOT(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT < 8
  lw    s1, H_TARGET_SEGMENT_COUNT(gp)
  beqz  s1, fail_input_work
  addi  s1, s1, -1
#endif

input_work_prepare_segment:
  lw    t0, H_TARGET_COUNT_PER_SEGMENT(gp)
  beqz  t0, fail_input_work
  slli  t0, t0, 2
  add   s5, s0, t0
  lw    t4, H_FF_START_RANK(gp)
  srli  t0, t0, 2
  bgeu  t4, t0, fail_input_work
  mv    t1, t4
  slli  t4, t4, 2
  add   s6, s0, t4
  lw    t2, H_POSTS_PER_SEGMENT(gp)
  beqz  t2, fail_input_work
  sub   t2, t2, t0
  bgeu  t1, t2, input_weight_start_single
  slli  t1, t1, 1
  j     input_weight_start_ready
input_weight_start_single:
  add   t1, t1, t2
input_weight_start_ready:
  add   t1, t1, s7
  slli  s10, t1, 2
  add   s4, s8, s10
  mv    s2, s6
  mv    s3, s5
  lw    t0, H_FF_START_RANK(gp)
  snez  s9, t0
  li    a0, 0
  li    a1, 0
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_WORK
  j     do_wfi

input_send_non_input:
  lw    t0, H_EXC_COUNT(gp)
  beqz  t0, input_send_done
  lw    s5, H_FF_EXPECTED(gp)
  bgtu  s6, s5, fail_ff_count
  bne   s6, s5, do_wfi
  j     ff_finish

input_send_done:
#if MNIST_FUSE_NEURON_UPDATE == 1
  j     phase_neuron_update
#else
  li    a2, 0
  li    a0, PHASE_INPUT_SEND
  j     reduce_local
#endif

/*
 * Stream a dense input row in cyclic target order.  Each descriptor's low
 * half is the destination and its high half is the receiver-local metadata.
 * The ordinary send blocks on egress backpressure; this bounded phase has no
 * return-message dependency, so it needs no per-destination local continuation.
 */
work_handler:
  lw    t3, 0(s2)
  lw    a1, 0(s4)
  srli  a0, t3, 16
  or    a0, a0, s11
  addi  s2, s2, TARGET_SIZE
  bltz  t3, work_send_single
  lw    a2, 4(s4)
  addi  s4, s4, 8
  SENDI t3, TAG_FF_SPIKE_I
  bne   s2, s3, work_handler
  j     work_segment_done
work_send_single:
  addi  s4, s4, 4
  SENDI t3, TAG_FF_SPIKE_I
  bne   s2, s3, work_handler

work_segment_done:
  /* The cyclic traversal wraps at most once per row. */
  beqz  s9, input_work_segment_complete
  li    s9, 0
  mv    s2, s0
  mv    s3, s6
  slli  t0, s7, 2
  add   s4, s8, t0
  j     work_handler

input_work_segment_complete:
#if MNIST_HORIZONTAL_GROUP_COUNT >= 8
  j     input_work_row_done
#else
  beqz  s1, input_work_row_done
  addi  s1, s1, -1
  mv    s0, s5
  lw    t0, H_POSTS_PER_SEGMENT(gp)
  add   s7, s7, t0
  j     input_work_prepare_segment
#endif

input_work_row_done:
  li    s11, 0
#if MNIST_HORIZONTAL_GROUP_COUNT >= 8
  beqz  s1, input_send_done
  addi  s1, s1, -1
  lw    t0, H_FIRED_VECTOR_OFFSET(gp)
  add   t0, t0, gp
  slli  t1, s1, 3
  add   t0, t0, t1
  lw    s8, 0(t0)
  /* Every local full-partition row shares this PU's target segment. */
  add   s4, s8, s10
  mv    s2, s6
  mv    s3, s5
  sub   s9, s6, s0
  snez  s9, s9
  j     work_handler
#else
  beq   a4, a5, input_send_done
  addi  a4, a4, -4
  lw    s8, 0(a4)
  j     input_work_start_row
#endif

fail_input_work:
  li    a0, 0xbada0400
  j     fail

/* Feedforward delivery is serialized on thread 0 and accumulates in registers. */
ff_spike_handler:
  /* The first row from each active input PU records its source exactly once. */
  bgez  a0, ff_source_ready
  srli  t0, a0, 16
  andi  t0, t0, FF_SOURCE_MASK
  lw    t2, H_OBSERVED_VECTOR_OFFSET(gp)
  add   t2, t2, gp
  slli  t3, s2, 1
  add   t2, t2, t3
  sh    t0, 0(t2)
  addi  s2, s2, 1
ff_source_ready:
  beqz  s6, ff_mark_input
ff_accumulate:
  fadd.s s3, s3, a1
  srli  t1, a0, 8
  andi  t1, t1, 255
  li    t2, 255
  beq   t1, t2, ff_one_value
  fadd.s s4, s4, a2
ff_one_value:
  addi  s6, s6, 1
  bne   s6, s5, do_wfi
  j     ff_finish

ff_mark_input:
  li    t1, NEURON_FF_INPUT
  sw    t1, EXC_FLAGS(s1)
  li    t2, 2
  bne   s7, t2, ff_accumulate
  sw    t1, EXC_SIZE + EXC_FLAGS(s1)
  j     ff_accumulate

ff_finish:
  lw    t0, H_DONE_SENT(gp)
  bnez  t0, do_wfi
  li    t0, 1
  sw    t0, H_DONE_SENT(gp)
  sw    s2, H_OBSERVED_COUNT(gp)
  sw    s3, EXC_GE(s1)
  li    t0, 2
  bne   s7, t0, ff_finish_count
  sw    s4, EXC_SIZE + EXC_GE(s1)
ff_finish_count:
  sw    s6, H_FF_RECEIVED(gp)
#if MNIST_FUSE_NEURON_UPDATE == 1
  j     phase_neuron_update
#else
  li    a2, 0
  li    a0, PHASE_INPUT_SEND
  j     reduce_local
#endif

fail_ff_count:
  li    a0, 0xbad73000
  j     fail

/* Update local neuron pairs, or decay an input PU's traces, then reduce. */
phase_neuron_update:
  lw    t0, H_EXC_COUNT(gp)
#if MNIST_INPUT_DECAY_MODE == 1
  beqz  t0, neuron_update_non_neuron
#else
  beqz  t0, neuron_update_done_empty
#endif
  li    t1, 1
  beq   t0, t1, neuron_update_single
  li    t1, 2
  bne   t0, t1, fail_role

  /* Thread 0 retains the first result; thread 1 returns the second by event. */
  sw    zero, H_NEURON_LOCAL_VALUE(gp)
  li    a0, 0
  li    a1, 0
  li    a2, 0
  li    a3, 0
  SENDI zero, TAG_NEURON_WORKER
  lw    s0, H_EXC_OFFSET(gp)
  add   s0, s0, gp
  lw    s2, H_INH_OFFSET(gp)
  add   s2, s2, gp
  jal   s7, neuron_pair_update
  slli  t0, s9, 16
  or    t0, t0, s8
  sw    t0, H_NEURON_LOCAL_VALUE(gp)
  j     do_wfi

neuron_update_single:
  lw    s0, H_EXC_OFFSET(gp)
  add   s0, s0, gp
  lw    s2, H_INH_OFFSET(gp)
  add   s2, s2, gp
  jal   s7, neuron_pair_update
  j     neuron_update_finalize

#if MNIST_INPUT_DECAY_MODE == 1
neuron_update_non_neuron:
  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, neuron_update_done_empty
  lw    t2, H_INPUT_OFFSET(gp)
  add   t2, t2, gp
  li    t1, 2
  bltu  t0, t1, input_pre_decay_serial

  lw    t1, H_INPUT_SPLIT_COUNT(gp)
  li    a0, 0
  li    a1, 0
  li    a2, 0
  li    a3, 0
  SENDI zero, TAG_NEURON_WORKER
  jal   s7, input_pre_decay_range
  j     do_wfi

input_pre_decay_serial:
  mv    t1, t0
  jal   s7, input_pre_decay_range
  j     neuron_update_done_empty

/* t1=count and t2=input records; each SMT thread receives a disjoint range. */
input_pre_decay_range:
  li    t3, F_PRE_DECAY
input_pre_decay_range_loop:
  lw    t4, IN_PRE_TRACE(t2)
  fmul.s t4, t4, t3
  sw    t4, IN_PRE_TRACE(t2)
  addi  t2, t2, INPUT_SIZE
  addi  t1, t1, -1
  bnez  t1, input_pre_decay_range_loop
  jr    s7
#endif

/* Thread-1-only handler for statically assigned phase-local work. */
neuron_worker_handler:
  lw    t0, H_PHASE(gp)
#if MNIST_PARALLEL_INPUT_PREPARE == 1
  li    t1, PHASE_INPUT_PREPARE
  beq   t0, t1, input_prepare_worker
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  li    t1, PHASE_WEIGHT_UPDATE
  beq   t0, t1, input_prepare_worker
#endif
#endif
  li    t1, PHASE_NEURON_WORK
#if MNIST_RECURRENT_MODE != MT_REC_SCATTER
  /* Phase 6 reuses this otherwise-idle queue for the terminal XOR tree. */
  bne   t0, t1, final_reduce_dispatch
#else
  bne   t0, t1, fail_role
#endif
#if MNIST_INPUT_DECAY_MODE == 1
  lw    t0, H_INPUT_COUNT(gp)
  bnez  t0, input_pre_decay_worker
#endif
  lw    t0, H_EXC_COUNT(gp)
  li    t1, 2
  bne   t0, t1, fail_role
  lw    s0, H_EXC_OFFSET(gp)
  add   s0, s0, gp
  addi  s0, s0, EXC_SIZE
  lw    s2, H_INH_OFFSET(gp)
  add   s2, s2, gp
  addi  s2, s2, INH_SIZE
  jal   s7, neuron_pair_update
  slli  a0, s9, 16
  or    a0, a0, s8
  li    a1, 0
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_NEURON_JOIN
  j     do_wfi

/* Always resume phase completion on thread 0. */
neuron_join_handler:
  lw    t0, H_PHASE(gp)
#if MNIST_PARALLEL_INPUT_PREPARE == 1
  li    t1, PHASE_INPUT_PREPARE
  beq   t0, t1, input_prepare_join
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  li    t1, PHASE_WEIGHT_UPDATE
  beq   t0, t1, input_prepare_join
#endif
#endif
  li    t1, PHASE_NEURON_WORK
  bne   t0, t1, fail_role
#if MNIST_INPUT_DECAY_MODE == 1
  lw    t0, H_INPUT_COUNT(gp)
  bnez  t0, input_pre_decay_join
#endif
  lw    t0, H_EXC_COUNT(gp)
  li    t1, 2
  bne   t0, t1, fail_role
  lw    t0, H_NEURON_LOCAL_VALUE(gp)
  add   t0, t0, a0
  slli  s8, t0, 16
  srli  s8, s8, 16
  srli  s9, t0, 16
  li    t1, 2
  bgtu  s8, t1, fail_role
  bgtu  s9, t1, fail_role
  j     neuron_update_finalize

#if MNIST_INPUT_DECAY_MODE == 1
input_pre_decay_worker:
  lw    t1, H_INPUT_SECOND_COUNT(gp)
  beqz  t1, fail_role
  lw    t2, H_INPUT_SECOND_BEGIN(gp)
  jal   s7, input_pre_decay_range
#if MNIST_RULE == MT_RULE_TWO
  INIT_FIFTH_ROOT
#endif
  li    a0, 0
  li    a1, 0
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_NEURON_JOIN
  j     do_wfi

input_pre_decay_join:
  bnez  a0, fail_role
  j     neuron_update_done_empty
#endif

#if MNIST_PARALLEL_INPUT_PREPARE == 1
/* Thread 1 owns the second ceil/floor partition and a fixed vector segment. */
input_prepare_worker:
  lw    t1, H_INPUT_SECOND_COUNT(gp)
  beqz  t1, fail_role
  lw    t2, H_INPUT_SECOND_BEGIN(gp)
  lw    a5, H_VECTOR_SECOND_BEGIN(gp)
  jal   s7, input_prepare_range

  slli  a0, a6, 16
  or    a0, a0, t6
#if MNIST_RULE == MT_RULE_TWO && MNIST_INPUT_DECAY_MODE != 1
  INIT_FIFTH_ROOT
#endif
  li    a1, 0
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_NEURON_JOIN
  j     do_wfi

/* Merge the two compact sublists once; no work allocation or append AMO is used. */
input_prepare_join:
  mv    a4, a0
  slli  a0, a4, 16
  srli  a0, a0, 16
  srli  a1, a4, 16
  lw    t0, H_INPUT_COUNT(gp)
  li    t1, 2
  bltu  t0, t1, fail_role
  lw    t2, H_INPUT_SPLIT_COUNT(gp)
  lw    t3, H_INPUT_SECOND_COUNT(gp)
  bgtu  a0, t3, fail_role
  bgtu  a1, a0, fail_role

  lw    t4, H_FIRED_COUNT(gp)
  bgtu  t4, t2, fail_role
  lw    t5, H_NEURON_LOCAL_VALUE(gp)
  bgtu  t5, t4, fail_role
  add   t6, t4, a0
  bgtu  t6, t0, fail_role
  add   a2, t5, a1
  bgtu  a2, t6, fail_role
  beqz  a0, input_prepare_join_done

  lw    a5, H_FIRED_VECTOR_OFFSET(gp)
  beqz  a5, fail_input_vector
  add   a5, a5, gp
  lw    a4, H_VECTOR_SECOND_BEGIN(gp)
  slli  a6, t4, 3
  add   a6, a5, a6
  beq   a4, a6, input_prepare_join_done
input_prepare_join_copy:
  lw    t1, 0(a4)
  sw    t1, 0(a6)
  lw    t1, 4(a4)
  sw    t1, 4(a6)
  addi  a4, a4, FIRED_VECTOR_SIZE
  addi  a6, a6, FIRED_VECTOR_SIZE
  addi  a0, a0, -1
  bnez  a0, input_prepare_join_copy

input_prepare_join_done:
  sw    t6, H_FIRED_COUNT(gp)
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  lw    a0, H_PHASE(gp)
#else
  li    a0, PHASE_INPUT_PREPARE
#endif
  j     reduce_local
#endif

/* s0=exc, s2=paired inh; return s9=exc fired, s8=inh fired. */
#include "mnist_neuron_pair.inc"

neuron_update_finalize:

#if MNIST_TRACE_BUNDLE == 0
  li    s10, 0
  li    s11, 0
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  li    a7, 0
#endif
  lw    t0, H_NEURON_GROUP(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  li    t1, 4
  bltu  t0, t1, neuron_trace_group_low_word_16
  li    t1, 10
  bltu  t0, t1, neuron_trace_group_high_word_16
  li    t1, 15
  bgtu  t0, t1, fail_role
  addi  t0, t0, -10
  li    t1, 5
  mul   t0, t0, t1
  sll   a7, s9, t0
  j     neuron_trace_count_ready
neuron_trace_group_high_word_16:
  addi  t0, t0, -4
  li    t1, 5
  mul   t0, t0, t1
  sll   s11, s9, t0
  j     neuron_trace_count_ready
neuron_trace_group_low_word_16:
  li    t1, 5
  mul   t0, t0, t1
  addi  t0, t0, 10
  sll   s10, s9, t0
  j     neuron_trace_count_ready
#elif MNIST_HORIZONTAL_GROUP_COUNT == 8
  li    t1, 3
  bgeu  t0, t1, neuron_trace_group_high_word
  li    t1, 6
  mul   t0, t0, t1
  addi  t0, t0, 10
  sll   s10, s9, t0
  j     neuron_trace_count_ready
neuron_trace_group_high_word:
  li    t1, 7
  bgtu  t0, t1, fail_role
  addi  t0, t0, -3
  li    t1, 6
  mul   t0, t0, t1
  sll   s11, s9, t0
  j     neuron_trace_count_ready
#else
  beqz  t0, neuron_trace_group0
  li    t1, 1
  beq   t0, t1, neuron_trace_group1
  li    t1, 2
  beq   t0, t1, neuron_trace_group2
  li    t1, 3
  bne   t0, t1, fail_role
  slli  s11, s9, 10
  j     neuron_trace_count_ready
neuron_trace_group2:
  mv    s11, s9
  j     neuron_trace_count_ready
neuron_trace_group1:
  slli  s10, s9, 20
  j     neuron_trace_count_ready
neuron_trace_group0:
  slli  s10, s9, 10
#endif
neuron_trace_count_ready:
#else
  li    s10, 0
  li    s11, 0
  beqz  s9, neuron_trace_target_ready
  lw    s10, H_EXC_COUNT(gp)
neuron_trace_target_ready:
  slli  s10, s10, 16
#endif

#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  beqz  s8, neuron_update_reduce_scatter
  lw    s0, H_INH_OFFSET(gp)
  add   s0, s0, gp
  lw    t0, H_INH_COUNT(gp)
  li    t1, INH_SIZE
  mul   t1, t0, t1
  add   s1, s0, t1
  li    s2, 0
  li    a0, 0
  li    a1, 0
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_RECURRENT_SEND_I
  j     do_wfi
#else
  or    a2, s8, s10
  mv    a3, s11
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  j     reduce_neuron_local_pair
#else
  li    a0, PHASE_NEURON_WORK
  j     reduce_local_pair
#endif
#endif

#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
neuron_update_reduce_scatter:
  mv    a2, s10
  mv    a3, s11
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  j     reduce_neuron_local_pair
#else
  li    a0, PHASE_NEURON_WORK
  j     reduce_local_pair
#endif
#endif

#if MNIST_HORIZONTAL_GROUP_COUNT == 16
/* Preserve all 30 extra-word bits while retaining phase and subtree metadata. */
reduce_neuron_local_pair:
  srli  a1, a7, 29
  slli  a1, a1, 31
  ori   a1, a1, 1
  slli  a0, a7, 3
  ori   a0, a0, PHASE_NEURON_WORK
  YIELDI zero, TAG_REDUCE
  j     do_wfi
#endif

neuron_update_done_empty:
#if MNIST_TRACE_BUNDLE == 0
  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, neuron_update_empty_count_ready
  addi  s8, gp, H_TRACE_RECEIVED
  li    s9, -1
  li    s10, 1
neuron_update_empty_count_ready:
#endif
#if MNIST_RULE == MT_RULE_TWO
  INIT_FIFTH_ROOT
#endif
  li    a2, 0
  li    a0, PHASE_NEURON_WORK
  j     reduce_local

#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
/* Issue all nonempty CSR rows for each locally firing inhibitory neuron. */
recurrent_send_handler:
recurrent_send_find_fired:
  beq   s0, s1, recurrent_send_done
  lw    t0, INH_FLAGS(s0)
  andi  t0, t0, NEURON_FIRED
  bnez  t0, recurrent_send_scan_mc
  addi  s0, s0, INH_SIZE
  li    s2, 0
  j     recurrent_send_find_fired

recurrent_send_scan_mc:
  li    t0, MT_MC_COUNT
  beq   s2, t0, recurrent_send_next_neuron
  slli  t1, s2, 2
  add   t2, s0, t1
  lw    t3, INH_CSR_LINES(t2)
  beqz  t3, recurrent_send_next_mc
  lw    a0, INH_CSR_START(t2)
  slli  a1, t3, 16
  ori   a1, a1, TAG_RECURRENT_RESPONSE
  li    a2, 1
  li    a3, 0
  li    t4, MC_DST_BASE
  add   t4, t4, s2
  addi  s2, s2, 1
  li    t5, TAG_SCATTER_REQUEST
  SEND  t4, t5
  YIELDI zero, TAG_RECURRENT_SEND_I
  j     do_wfi
recurrent_send_next_mc:
  addi  s2, s2, 1
  j     recurrent_send_scan_mc
recurrent_send_next_neuron:
  addi  s0, s0, INH_SIZE
  li    s2, 0
  j     recurrent_send_find_fired
recurrent_send_done:
  mv    a2, s10
  mv    a3, s11
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  j     reduce_neuron_local_pair
#else
  li    a0, PHASE_NEURON_WORK
  j     reduce_local_pair
#endif

/* Scatter responses are pinned to thread 1, so no float LR/SC loop is needed. */
recurrent_handler:
  li    t0, 1
  bne   a2, t0, fail_recurrent
  lw    t0, H_EXC_COUNT(gp)
  bgeu  a0, t0, fail_recurrent
  lw    t1, H_EXC_OFFSET(gp)
  add   t1, t1, gp
  beqz  a0, recurrent_target_ready
  addi  t1, t1, EXC_SIZE
recurrent_target_ready:
  lw    t2, EXC_GI(t1)
  fadd.s t2, t2, a1
  sw    t2, EXC_GI(t1)
  j     do_wfi

fail_recurrent:
  li    a0, 0xbad71000
  j     fail
#endif

/* Apply delayed recurrent inhibition, decay posts, and start reverse sending. */
phase_weight_update:
  lw    WEIGHT_TMP, H_EXC_COUNT(gp)
  beqz  WEIGHT_TMP, weight_phase_non_neuron
#if MNIST_TRACE_BUNDLE == 0
  andi  s11, a1, 1023
  li    t1, MT_INH_COUNT
  bgtu  s11, t1, fail_weight_phase
  lw    t0, H_NEURON_GROUP(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  li    t1, 4
  bltu  t0, t1, weight_trace_count_low_word_16
  li    t1, 10
  bltu  t0, t1, weight_trace_count_high_word_16
  addi  t0, t0, -10
  li    t1, 5
  mul   t0, t0, t1
  srl   a7, a3, t0
  j     weight_trace_count_ready_16
weight_trace_count_high_word_16:
  addi  t0, t0, -4
  li    t1, 5
  mul   t0, t0, t1
  srl   a7, a2, t0
  j     weight_trace_count_ready_16
weight_trace_count_low_word_16:
  li    t1, 5
  mul   t0, t0, t1
  addi  t0, t0, 10
  srl   a7, a1, t0
weight_trace_count_ready_16:
  andi  a7, a7, 31
  li    t1, MT_EXC_COUNT / 16
  bgtu  a7, t1, fail_weight_phase
  j     weight_trace_count_decoded
#elif MNIST_HORIZONTAL_GROUP_COUNT == 8
  li    t1, 3
  bgeu  t0, t1, weight_trace_count_high_word
  li    t1, 6
  mul   t0, t0, t1
  addi  t0, t0, 10
  srl   a7, a1, t0
  j     weight_trace_count_ready_8
weight_trace_count_high_word:
  addi  t0, t0, -3
  li    t1, 6
  mul   t0, t0, t1
  srl   a7, a2, t0
weight_trace_count_ready_8:
  andi  a7, a7, 63
  li    t1, MT_EXC_COUNT / 8
  bgtu  a7, t1, fail_weight_phase
  j     weight_trace_count_decoded
#else
  beqz  t0, weight_trace_count_group0
  li    t1, 1
  beq   t0, t1, weight_trace_count_group1
  li    t1, 2
  beq   t0, t1, weight_trace_count_group2
  li    t1, 3
  bne   t0, t1, fail_weight_phase
  srli  a7, a2, 10
  j     weight_trace_count_ready
weight_trace_count_group2:
  mv    a7, a2
  j     weight_trace_count_ready
weight_trace_count_group1:
  srli  a7, a1, 20
  j     weight_trace_count_ready
weight_trace_count_group0:
  srli  a7, a1, 10
weight_trace_count_ready:
  andi  a7, a7, 1023
#endif
weight_trace_count_decoded:
#else
  srli  a7, a1, 16
  li    t1, MT_EXC_COUNT
  bgtu  a7, t1, fail_weight_phase
  slli  s11, a1, 16
  srli  s11, s11, 16
  li    t1, MT_INH_COUNT
  bgtu  s11, t1, fail_weight_phase
#endif
  lw    s7, H_EXC_OFFSET(gp)
  add   s7, s7, gp
  lw    s8, H_INH_OFFSET(gp)
  add   s8, s8, gp

#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  bnez  s11, fail_weight_phase
#else
  mv    t0, s7
  mv    t1, s8
  lw    t2, H_EXC_COUNT(gp)
weight_recurrent_loop:
  lw    t3, INH_FLAGS(t1)
  andi  t3, t3, NEURON_FIRED
  sub   t3, s11, t3
  beqz  t3, weight_recurrent_next
  lw    t4, EXC_GI(t0)
#if MNIST_RECURRENT_MODE == MT_REC_TREE
  li    t5, F_I_TO_E
weight_recurrent_add_loop:
  fadd.s t4, t4, t5
  addi  t3, t3, -1
  bnez  t3, weight_recurrent_add_loop
#else
  li    t5, F_TWO23
  add   t6, t5, t3
  fsub.s t6, t6, t5
  li    t5, F_I_TO_E
  fmul.s t6, t6, t5
  fadd.s t4, t4, t6
#endif
  sw    t4, EXC_GI(t0)
weight_recurrent_next:
  addi  t0, t0, EXC_SIZE
  addi  t1, t1, INH_SIZE
  addi  t2, t2, -1
  bnez  t2, weight_recurrent_loop
#endif

  /* Decay only traces consumed by the selected learning rule. */
#if MNIST_RULE != MT_RULE_ONE
  mv    t0, s7
  lw    t1, H_EXC_COUNT(gp)
  li    t4, F_POST1_DECAY
#if MNIST_RULE == MT_RULE_THREE
  li    t5, F_POST2_DECAY
#endif
weight_post_decay_loop:
  lw    t2, EXC_POST1(t0)
#if MNIST_RULE == MT_RULE_THREE
  lw    t3, EXC_POST2(t0)
  fmul.s t3, t3, t5
  sw    t3, EXC_POST2(t0)
#endif
  fmul.s t2, t2, t4
  sw    t2, EXC_POST1(t0)
  addi  t0, t0, EXC_SIZE
  addi  t1, t1, -1
  bnez  t1, weight_post_decay_loop
#endif

  lw    s0, H_INPUT_DEST_OFFSET(gp)
  beqz  s0, fail_weight_phase
  add   s0, s0, gp
  lw    t0, H_INPUT_PU_COUNT(gp)
  beqz  t0, fail_weight_phase
  slli  t0, t0, 1
  add   s8, s0, t0
  li    s3, 0

  /* First local excitatory neuron's universal input-weight slot. */
  lw    t0, EXC_WEIGHT_SLOT(s7)
#if MNIST_RULE != MT_RULE_ONE
  lw    a1, EXC_POST1(s7)
#else
  li    a1, 0
#endif
  lw    t1, EXC_FLAGS(s7)
  andi  t1, t1, NEURON_FIRED
#if MNIST_TRACE_BUNDLE == 0
  /* Every destination reuses this byte offset and depression delta. */
  slli  a0, t0, 2
#if MNIST_RULE != MT_RULE_ONE
  lw    t2, H_DEPRESSION_RATE(gp)
  fmul.s a1, a1, t2
#endif
#else
  li    t2, TRACE_SLOT_NONE
  slli  t2, t2, 9
  or    a0, t0, t2
#endif
#if MNIST_TRACE_BUNDLE == 0
  slli  t2, t1, 31
#else
  slli  t2, t1, 18
#endif
  or    a0, a0, t2
  li    a2, 0
  li    a3, 0

  lw    t3, H_EXC_COUNT(gp)
  li    t4, 2
  bne   t3, t4, weight_trace_payload_ready
  addi  t4, s7, EXC_SIZE
  lw    t5, EXC_WEIGHT_SLOT(t4)
#if MNIST_RULE != MT_RULE_ONE
  lw    t6, EXC_POST1(t4)
#else
  li    t6, 0
#endif
  lw    a4, EXC_FLAGS(t4)
  andi  a4, a4, NEURON_FIRED

#if MNIST_TRACE_BUNDLE == 1
#if MNIST_RULE == MT_RULE_THREE
  and   a5, t1, a4
  bnez  a5, weight_trace_make_second
#endif
  /* A single physical message carries both logical columns. */
  li    t2, TRACE_SLOT_NONE
  slli  t2, t2, 9
  xor   a0, a0, t2
  slli  t2, t5, 9
  or    a0, a0, t2
  slli  t2, a4, 19
  or    a0, a0, t2
  mv    a2, t6
#if MNIST_RULE == MT_RULE_THREE
  bnez  t1, weight_trace_payload_ready
  beqz  a4, weight_trace_payload_ready
  lw    a3, EXC_POST2(t4)
#endif
  j     weight_trace_payload_ready
#endif

weight_trace_make_second:
  li    s3, 1
#if MNIST_TRACE_BUNDLE == 0
  slli  s4, t5, 2
#else
  li    t2, TRACE_SLOT_NONE
  slli  t2, t2, 9
  or    s4, t5, t2
#endif
#if MNIST_TRACE_BUNDLE == 0
  slli  t2, a4, 31
#else
  slli  t2, a4, 18
#endif
  or    s4, s4, t2
#if MNIST_TRACE_BUNDLE == 0 && MNIST_RULE != MT_RULE_ONE
  lw    t2, H_DEPRESSION_RATE(gp)
  fmul.s s5, t6, t2
#else
  mv    s5, t6
#endif
  li    s6, 0
#if MNIST_RULE == MT_RULE_THREE
  beqz  a4, weight_trace_payload_ready
  lw    s6, EXC_POST2(t4)
  lw    t2, H_POTENTIATION_RATE(gp)
  fmul.s s6, s6, t2
#endif

weight_trace_payload_ready:
  li    a3, 0
#if MNIST_RULE == MT_RULE_THREE
  /* The selected fired column's slow post trace was loaded above. */
  lw    t0, EXC_FLAGS(s7)
  andi  t0, t0, NEURON_FIRED
  beqz  t0, weight_trace_maybe_second_post
  lw    a3, EXC_POST2(s7)
  lw    t2, H_POTENTIATION_RATE(gp)
  fmul.s a3, a3, t2
  j     weight_trace_start
weight_trace_maybe_second_post:
#if MNIST_TRACE_BUNDLE == 1
  lw    t0, H_EXC_COUNT(gp)
  li    t1, 2
  bne   t0, t1, weight_trace_start
  lw    t0, EXC_SIZE + EXC_FLAGS(s7)
  andi  t0, t0, NEURON_FIRED
  beqz  t0, weight_trace_start
  lw    a3, EXC_SIZE + EXC_POST2(s7)
  lw    t2, H_POTENTIATION_RATE(gp)
  fmul.s a3, a3, t2
#endif
#endif
weight_trace_start:
#if MNIST_TRACE_BUNDLE == 0
  /* A firing post reaches every input PU; otherwise only observed sources. */
  mv    s1, s0
  mv    s2, s8
  bltz  a0, weight_trace_first_dest_ready
#if MNIST_RULE == MT_RULE_ONE
  mv    s1, s2
#else
  lw    s1, H_OBSERVED_VECTOR_OFFSET(gp)
  beqz  s1, fail_weight_phase
  add   s1, s1, gp
  lw    t0, H_OBSERVED_COUNT(gp)
  lw    t1, H_PHYSICAL_INPUT_PU_COUNT(gp)
  bgtu  t0, t1, fail_weight_phase
  slli  t0, t0, 1
  add   s2, s1, t0
#endif
weight_trace_first_dest_ready:
  beqz  s3, weight_trace_destinations_ready
  mv    s9, s0
  mv    s10, s8
  bltz  s4, weight_trace_destinations_ready
#if MNIST_RULE == MT_RULE_ONE
  mv    s9, s10
#else
  lw    s9, H_OBSERVED_VECTOR_OFFSET(gp)
  beqz  s9, fail_weight_phase
  add   s9, s9, gp
  lw    t0, H_OBSERVED_COUNT(gp)
  lw    t1, H_PHYSICAL_INPUT_PU_COUNT(gp)
  bgtu  t0, t1, fail_weight_phase
  slli  t0, t0, 1
  add   s10, s9, t0
#endif
weight_trace_destinations_ready:
#else
  mv    s1, s0
  mv    s2, s8
#endif
  beq   s1, s2, weight_trace_first_empty
#if MNIST_TRACE_BUNDLE == 0
  /* Avoid a local continuation event when this post has one destination. */
  addi  t0, s1, 2
  bne   t0, s2, weight_trace_yield
  lhu   t0, 0(s1)
  SENDI t0, TAG_TRACE_EVENT_I
  j     trace_send_pass_done
weight_trace_yield:
#endif
  YIELDI zero, TAG_TRACE_SEND_I
  j     do_wfi

weight_trace_first_empty:
  bnez  s3, trace_send_second_pass
  j     finish_neuron_traces

weight_phase_non_neuron:
  lw    WEIGHT_TMP, H_INPUT_COUNT(gp)
  beqz  WEIGHT_TMP, weight_phase_unused
#if MNIST_TRACE_BUNDLE == 0
#if MNIST_RULE != MT_RULE_ONE
  lw    WEIGHT_TMP, H_FIRED_COUNT(gp)
  beqz  WEIGHT_TMP, weight_phase_count_up
  lw    t1, 0(s8)
  lw    t2, H_OWNED_POST_COUNT(gp)
  beqz  t2, fail_trace_count
  bgtu  t1, t2, fail_trace_count
  bnez  t1, do_wfi
  j     finish_input_traces
weight_phase_count_up:
#endif
  lw    t2, H_TRACE_EXPECTED(gp)
  li    WEIGHT_TMP, MT_EXC_COUNT
  bgtu  t2, WEIGHT_TMP, fail_trace_count
  lw    t1, H_TRACE_RECEIVED(gp)
  bgtu  t1, t2, fail_trace_count
  bne   t1, t2, do_wfi
#else
  lw    t1, H_TRACE_RECEIVED(gp)
  li    t2, MT_EXC_COUNT
  bgtu  t1, t2, fail_trace_count
  bne   t1, t2, do_wfi
#endif
  j     finish_input_traces
weight_phase_unused:
  li    a2, 0
  li    a0, PHASE_WEIGHT_UPDATE
  j     reduce_local

fail_weight_phase:
  li    a0, 0xbad75000
  j     fail

/*
 * Steady path: one two-cycle SPM load, increment, send, mostly-not-taken
 * completion branch, and local yielding send.
 */
trace_send_handler:
  lhu   t0, 0(s1)
  addi  s1, s1, 2
  SENDI t0, TAG_TRACE_EVENT_I
  beq   s1, s2, trace_send_pass_done
  YIELDI zero, TAG_TRACE_SEND_I
  j     do_wfi

trace_send_pass_done:
  bnez  s3, trace_send_second_pass
  j     finish_neuron_traces

trace_send_second_pass:
  li    s3, 0
  mv    a0, s4
  mv    a1, s5
  li    a2, 0
  mv    a3, s6
  mv    s1, s9
  mv    s2, s10
  beq   s1, s2, finish_neuron_traces
  YIELDI zero, TAG_TRACE_SEND_I
  j     do_wfi

/* Sender-side trace update after every destination has accepted the payload. */
finish_neuron_traces:
  lw    t0, H_DONE_SENT(gp)
  bnez  t0, do_wfi
  li    t0, 1
  sw    t0, H_DONE_SENT(gp)
  lw    t0, H_EXC_OFFSET(gp)
  add   t0, t0, gp
  lw    t1, H_EXC_COUNT(gp)
finish_neuron_trace_loop:
  lw    t2, EXC_FLAGS(t0)
  andi  t2, t2, NEURON_FIRED
  beqz  t2, finish_neuron_trace_clear
#if MNIST_RULE == MT_RULE_THREE
  li    t3, F_ONE
  sw    t3, EXC_POST1(t0)
  sw    t3, EXC_POST2(t0)
#elif MNIST_RULE == MT_RULE_TWO
  lw    t3, EXC_POST1(t0)
  li    t4, F_ONE
  fadd.s t3, t3, t4
  sw    t3, EXC_POST1(t0)
#endif

finish_neuron_trace_clear:
  sw    zero, EXC_FLAGS(t0)
  addi  t0, t0, EXC_SIZE
  addi  t1, t1, -1
  bnez  t1, finish_neuron_trace_loop
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  lw    t0, H_NEURON_LOCAL_VALUE(gp)
  bgez  t0, finish_neuron_next_ready

  /* Current reverse sends are done, so prepare this PU for the next delivery. */
  sw    zero, H_OBSERVED_COUNT(gp)
  sw    zero, H_FF_RECEIVED(gp)
  li    s2, 0
  li    s5, -1

  lw    s7, H_EXC_COUNT(gp)
  lw    s1, H_EXC_OFFSET(gp)
  add   s1, s1, gp
  lw    s3, EXC_GE(s1)
  li    s4, 0
  li    s6, 0
  li    t1, 2
  bne   s7, t1, finish_neuron_next_ready
  lw    s4, EXC_SIZE + EXC_GE(s1)
finish_neuron_next_ready:
#endif
  li    a2, 0
  li    a0, PHASE_WEIGHT_UPDATE
  j     reduce_local

/* Receiver-side trace update after all 400 logical columns have completed. */
finish_input_traces:
  addi  t1, gp, H_DONE_SENT
  li    t0, 1
  amoor.w t2, t0, (t1)
  bnez  t2, do_wfi
  li    t0, 4
  csrw  0x714, t0
  lw    t1, H_FIRED_COUNT(gp)
  beqz  t1, finish_input_trace_done
  lw    t0, H_FIRED_VECTOR_OFFSET(gp)
  beqz  t0, fail_trace_list
  add   t0, t0, gp
finish_input_trace_loop:
  lw    t2, 4(t0)
#if MNIST_RULE == MT_RULE_THREE
  li    t3, F_ONE
  sw    t3, IN_PRE_TRACE(t2)
#else
  lw    t3, IN_PRE_TRACE(t2)
  li    t4, F_ONE
  fadd.s t3, t3, t4
  sw    t3, IN_PRE_TRACE(t2)
#endif
  sw    zero, IN_FLAGS(t2)
  addi  t0, t0, FIRED_VECTOR_SIZE
  addi  t1, t1, -1
  bnez  t1, finish_input_trace_loop
finish_input_trace_done:
#if MNIST_PIPELINE_INPUT_PREPARE == 1
  lw    t0, H_NEURON_LOCAL_VALUE(gp)
  bltz  t0, phase_input_prepare
#endif
  li    a2, 0
  li    a0, PHASE_WEIGHT_UPDATE
  j     reduce_local

fail_trace_count:
  li    a0, 0xbada2000
  j     fail

/* Final checksum intentionally covers only plastic input-to-E weights. */
phase_final_drain:
  lw    t0, H_WEIGHT_COUNT(gp)
  li    t6, 0
  beqz  t0, final_checksum_done
  lw    t1, H_WEIGHT_OFFSET(gp)
  add   t1, t1, gp
final_checksum_loop:
  lw    t2, 0(t1)
  xor   t6, t6, t2
  addi  t1, t1, 4
  addi  t0, t0, -1
  bnez  t0, final_checksum_loop
final_checksum_done:
  /* Replicated input states cancel pairwise when RNG and traces stay in sync. */
  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, final_state_checksum_done
  lw    t1, H_INPUT_OFFSET(gp)
  add   t1, t1, gp
final_state_checksum_loop:
  lw    t2, IN_GLOBAL(t1)
  li    t3, INPUT_GLOBAL_REPLICATED
  and   t3, t3, t2
  beqz  t3, final_state_checksum_next
  andi  t2, t2, 1023
  slli  t2, t2, 16
  lw    t3, IN_THRESHOLD(t1)
  xor   t2, t2, t3
  lw    t3, IN_PRE_TRACE(t1)
  xor   t2, t2, t3
  lw    t3, IN_RNG(t1)
  xor   t2, t2, t3
  xor   t6, t6, t2
final_state_checksum_next:
  addi  t1, t1, INPUT_SIZE
  addi  t0, t0, -1
  bnez  t0, final_state_checksum_loop
final_state_checksum_done:
  mv    a2, t6
  li    a0, PHASE_FINAL_DRAIN
#if MNIST_RECURRENT_MODE != MT_REC_SCATTER
  j     final_reduce_local
#else
  j     reduce_local
#endif

/* No-local-pre entry; a postsynaptic spike redirects to the full updater. */
trace_event_fast_handler:
#if MNIST_TRACE_BUNDLE == 0
  /* Inactive input PUs receive only globally delivered firing posts. */
  bgez  a0, fail_trace_event
  andi  s1, a0, TRACE_BYTE_MASK
  mv    s2, a1
  mv    s3, a3
  li    s4, 1
  jal   s7, apply_trace_slot
  j     trace_event_add_count_inactive
#else
  /*
   * Avoid metadata setup and two helper dispatches when neither endpoint can
   * update a weight; only the logical completion count matters.
   */
  srli  TRACE_TMP, a0, 18
  andi  TRACE_TMP, TRACE_TMP, 3
  bnez  TRACE_TMP, trace_event_handler
  srli  t1, a0, 9
  andi  t1, t1, TRACE_SLOT_NONE
  li    t2, TRACE_SLOT_NONE
  li    TRACE_TMP, 1
  beq   t1, t2, trace_event_add_count_active
  li    TRACE_TMP, 2
  j     trace_event_add_count_active
#endif

#if MNIST_RULE != MT_RULE_ONE
/* Exactly one locally fired pre: direct row update, with no list scan. */
#if MNIST_RULE == MT_RULE_THREE && MNIST_TRACE_BUNDLE == 0
  .balign 64
#endif
trace_event_one_pre_handler:
#if MNIST_TRACE_BUNDLE == 0
  bltz  a0, trace_event_handler
#if MNIST_RULE == MT_RULE_THREE
  andi  t1, a0, TRACE_BYTE_MASK
  lw    TRACE_TMP, H_CURSOR0(gp)
  add   tp, TRACE_TMP, t1
  lw    t5, 0(tp)
  fsub.s t5, t5, a1
  fmax.s t5, t5, zero
  fmin.s t5, t5, UPDATE_MAX
  sw    t5, 0(tp)
#else
  andi  s1, a0, TRACE_BYTE_MASK
  mv    s2, a1
  lw    TRACE_TMP, H_CURSOR0(gp)
  add   tp, TRACE_TMP, s1
  lw    t5, 0(tp)
  mv    a0, t5
  jal   t6, fifth_root
  fmul.s t2, s2, a0
  fsub.s t5, t5, t2
  fmax.s t5, t5, zero
  lw    t4, H_WEIGHT_MAX(gp)
  fmin.s t5, t5, t4
  sw    t5, 0(tp)
#endif
  /* Fall through to the adjacent completion counter and WFI. */
#else
  srli  TRACE_TMP, a0, 18
  andi  TRACE_TMP, TRACE_TMP, 3
  bnez  TRACE_TMP, trace_event_handler
  mv    s8, a0
  mv    s9, a2
  andi  s1, a0, TRACE_SLOT_NONE
  mv    s2, a1
  jal   s6, depress_one_pre
  srli  TRACE_TMP, s8, 9
  andi  s1, TRACE_TMP, TRACE_SLOT_NONE
  li    t1, TRACE_SLOT_NONE
  beq   s1, t1, trace_one_pre_count_one
  mv    s2, s9
  jal   s6, depress_one_pre
  li    TRACE_TMP, 2
  j     trace_event_add_count_active
trace_one_pre_count_one:
  li    TRACE_TMP, 1
  j     trace_event_add_count_active
#endif

#if MNIST_TRACE_BUNDLE == 1
depress_one_pre:
  lw    TRACE_TMP, H_CURSOR0(gp)
#if MNIST_TRACE_BUNDLE == 0
  add   tp, TRACE_TMP, s1
#else
  slli  t1, s1, 2
  add   tp, TRACE_TMP, t1
#endif
  lw    t5, 0(tp)
#if MNIST_RULE == MT_RULE_TWO
  mv    a0, t5
  jal   t6, fifth_root
#endif
#if MNIST_TRACE_BUNDLE == 0
  mv    t2, s2
#else
  lw    t2, H_DEPRESSION_RATE(gp)
  fmul.s t2, t2, s2
#endif
#if MNIST_RULE == MT_RULE_TWO
  fmul.s t2, t2, a0
#endif
  fsub.s t5, t5, t2
  flt.s t3, t5, zero
  beqz  t3, depress_one_pre_store
  li    t5, 0
depress_one_pre_store:
  sw    t5, 0(tp)
  jr    s6
#endif
#endif

/* Reverse trace payload; bundled mode may carry a second weight slot. */
  .pushsection .text.trace_general, "ax", @progbits
trace_event_handler:
#if MNIST_TRACE_BUNDLE == 0
  andi  s1, a0, TRACE_BYTE_MASK
  mv    s2, a1
  slt   s4, a0, zero
  li    s3, 0
  beqz  s4, trace_single_payload_ready
  mv    s3, a3
trace_single_payload_ready:
  jal   s7, apply_trace_slot
  j     trace_event_add_count_active
#else
  lw    TRACE_TMP, H_INPUT_COUNT(gp)
  beqz  TRACE_TMP, fail_trace_event
  srli  TRACE_TMP, a0, 20
  bnez  TRACE_TMP, fail_trace_event
  mv    s8, a0
  mv    s9, a2
  mv    s10, a3
  srli  s11, a0, 18
  andi  s11, s11, 3

  andi  s1, a0, TRACE_SLOT_NONE
  lw    TRACE_TMP, H_NEURON_PU_COUNT(gp)
  slli  TRACE_TMP, TRACE_TMP, 1
  bgeu  s1, TRACE_TMP, fail_trace_event
  mv    s2, a1
  andi  s4, s11, 1
  li    s3, 0
  beqz  s4, trace_first_payload_ready
  mv    s3, s10
trace_first_payload_ready:
  jal   s7, apply_trace_slot

  srli  TRACE_TMP, s8, 9
  andi  s1, TRACE_TMP, TRACE_SLOT_NONE
  li    t1, TRACE_SLOT_NONE
  beq   s1, t1, trace_event_count_one
  lw    t1, H_NEURON_PU_COUNT(gp)
  slli  t1, t1, 1
  bgeu  s1, t1, fail_trace_event
  andi  t1, s8, TRACE_SLOT_NONE
  beq   s1, t1, fail_trace_event
  srli  s4, s11, 1
#if MNIST_RULE == MT_RULE_THREE
  li    t1, 3
  beq   s11, t1, fail_trace_event
#endif
  mv    s2, s9
  li    s3, 0
  beqz  s4, trace_second_payload_ready
  mv    s3, s10
trace_second_payload_ready:
  jal   s7, apply_trace_slot
  li    TRACE_TMP, 2
  j     trace_event_add_count_active

trace_event_count_one:
  andi  TRACE_TMP, s11, 2
  bnez  TRACE_TMP, fail_trace_event
  li    TRACE_TMP, 1
  j     trace_event_add_count_active
#endif
  .popsection
trace_event_add_count_active:
#if MNIST_TRACE_BUNDLE == 0
  amoadd.w t2, s9, (s8)
  beq   t2, s10, trace_event_count_complete
trace_wait:
  wfi
  j     trace_wait
#else
  addi  t1, gp, H_TRACE_RECEIVED
  amoadd.w t2, TRACE_TMP, (t1)
  add   t2, t2, TRACE_TMP
  li    t3, MT_EXC_COUNT
  bgtu  t2, t3, fail_trace_count
  bne   t2, t3, do_wfi
#endif
trace_event_count_complete:
  lw    TRACE_TMP, H_PHASE(gp)
  li    t1, PHASE_WEIGHT_UPDATE
  bne   TRACE_TMP, t1, TRACE_WAIT
  j     finish_input_traces

#if MNIST_TRACE_BUNDLE == 0
trace_event_add_count_inactive:
  addi  t1, gp, H_TRACE_RECEIVED
  li    TRACE_TMP, 1
  amoadd.w t2, TRACE_TMP, (t1)
  addi  t2, t2, 1
  lw    TRACE_TMP, H_PHASE(gp)
  li    t1, PHASE_WEIGHT_UPDATE
  bne   TRACE_TMP, t1, TRACE_WAIT
  lw    t3, H_TRACE_EXPECTED(gp)
  bgtu  t2, t3, fail_trace_count
  bne   t2, t3, TRACE_WAIT
  j     trace_event_count_complete
#endif

fail_trace_event:
  li    a0, 0xbada1000
  j     fail

/*
 * s1=weight slot, s2=depression delta, s3=potentiation factor,
 * s4=post-fired.
 * s0/s5 bound the current input range; UPDATE_MAX holds the weight maximum.
 * Root rules reuse the dead post-fired flag s4 for the pre target, and s3
 * for the pot rate. The reducer's persistent ra must remain untouched.
 */
apply_trace_slot:
  beqz  s4, apply_trace_not_post
  lw    TRACE_TMP, H_INPUT_COUNT(gp)
  beqz  TRACE_TMP, apply_trace_return
  lw    s5, H_INPUT_OFFSET(gp)
  add   s5, s5, gp
  lw    s0, H_INPUT_END(gp)
#if MNIST_RULE != MT_RULE_THREE
  lw    UPDATE_MAX, H_WEIGHT_MAX(gp)
  lw    s4, H_PRE_TARGET(gp)
  lw    s3, H_POTENTIATION_RATE(gp)
#if MNIST_RULE == MT_RULE_ONE
  /* One-trace needs roots only for a firing post, once per handler's rows. */
  INIT_FIFTH_ROOT
#endif
#endif
  j     update_synapse_inline

apply_trace_not_post:
#if MNIST_RULE == MT_RULE_ONE
  jr    s7
#else
  lw    s5, H_FIRED_VECTOR_BEGIN(gp)
  lw    s0, H_FIRED_VECTOR_END(gp)
#if MNIST_TRACE_BUNDLE == 1
  beq   s5, s0, apply_trace_return
#endif
#if MNIST_TRACE_BUNDLE == 1
  lw    t2, H_DEPRESSION_RATE(gp)
  fmul.s s2, s2, t2
#endif
#if MNIST_RULE != MT_RULE_THREE
  lw    t4, H_WEIGHT_MAX(gp)
#endif
apply_trace_fired_loop:
  lw    TRACE_TMP, 0(s5)
#if MNIST_TRACE_BUNDLE == 0
  add   tp, TRACE_TMP, s1
#else
  slli  t1, s1, 2
  add   tp, TRACE_TMP, t1
#endif
  lw    t5, 0(tp)
#if MNIST_RULE == MT_RULE_TWO
  mv    a0, t5
  jal   t6, fifth_root
  fmul.s t2, s2, a0
  fsub.s t5, t5, t2
#else
  fsub.s t5, t5, s2
#endif
  fmax.s t5, t5, zero
#if MNIST_RULE == MT_RULE_THREE
  fmin.s t5, t5, UPDATE_MAX
#else
  fmin.s t5, t5, t4
#endif
  sw    t5, 0(tp)
  addi  s5, s5, FIRED_VECTOR_SIZE
  bne   s5, s0, apply_trace_fired_loop
#endif
apply_trace_return:
  jr    s7

fail_trace_list:
  li    a0, 0xbada1100
  j     fail

/* Fall through a common per-row update; no call/return around each synapse. */
update_synapse_inline:
  lw    a2, IN_FLAGS(s5)
  andi  a2, a2, INPUT_FIRED
  lw    t1, IN_PRE_TRACE(s5)
  lw    t2, IN_WEIGHT_OFFSET(s5)
  add   t2, t2, gp
#if MNIST_TRACE_BUNDLE == 0
  add   tp, t2, s1
#else
  slli  t3, s1, 2
  add   tp, t2, t3
#endif
  lw    t5, 0(tp)

#if MNIST_RULE == MT_RULE_THREE
  beqz  a2, update_three_no_depression
#if MNIST_TRACE_BUNDLE == 0
  fsub.s t5, t5, s2
#else
  lw    t2, H_DEPRESSION_RATE(gp)
  fmul.s t2, t2, s2
  fsub.s t5, t5, t2
#endif
  fmax.s t5, t5, zero
  fmin.s t5, t5, UPDATE_MAX
update_three_no_depression:
  beqz  a2, update_three_pre_ready
  li    t1, F_ONE
update_three_pre_ready:
  fmul.s t2, s3, t1
  fadd.s t5, t5, t2

#else
  mv    t4, t1
  beqz  a2, update_root_effective_ready
  li    t2, F_ONE
  fadd.s t4, t4, t2
#if MNIST_RULE == MT_RULE_TWO
  mv    a0, t5
  jal   t6, fifth_root
#if MNIST_TRACE_BUNDLE == 0
  mv    t2, s2
#else
  lw    t2, H_DEPRESSION_RATE(gp)
  fmul.s t2, t2, s2
#endif
  fmul.s t2, t2, a0
  fsub.s t5, t5, t2
  fmax.s t5, t5, zero
  fmin.s t5, t5, UPDATE_MAX
#endif
update_root_effective_ready:
  fsub.s t4, t4, s4
  fsub.s a0, s6, t5
  jal   t6, fifth_root
  fmul.s t2, s3, t4
  fmul.s t2, t2, a0
  fadd.s t5, t5, t2
#endif
  fmax.s t5, t5, zero
  fmin.s t5, t5, UPDATE_MAX
  sw    t5, 0(tp)
  addi  s5, s5, INPUT_SIZE
  bne   s5, s0, update_synapse_inline
  jr    s7

#if MNIST_RECURRENT_MODE != MT_REC_SCATTER
  .pushsection .text.final_reduce, "ax", @progbits
/*
 * The terminal checksum is the only XOR reduction. Reusing handler 15 keeps
 * the per-step sum reducer branch-free without consuming another event queue.
 */
final_reduce_local:
  li    a3, 0
  li    a1, 1
  YIELDI zero, TAG_NEURON_WORKER
  j     do_wfi

final_reduce_handler:
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  srli  t6, a0, 3
  srli  a4, a1, 31
  slli  a4, a4, 29
  or    t6, t6, a4
  slli  a4, a1, 1
  srli  a4, a4, 11
  bnez  a4, fail_final_reduce
  andi  a0, a0, 7
  andi  a1, a1, 1023
#endif
  lw    t2, H_REDUCE_EXPECTED(gp)
  lw    t3, H_REDUCE_COUNT(gp)
  add   t3, t3, a1
  bgtu  t3, t2, fail_final_reduce
  sw    t3, H_REDUCE_COUNT(gp)

  lw    t4, H_REDUCE_VALUE(gp)
  xor   t4, t4, a2
  sw    t4, H_REDUCE_VALUE(gp)
  lw    t5, H_REDUCE_VALUE_HI(gp)
  add   t5, t5, a3
  sw    t5, H_REDUCE_VALUE_HI(gp)
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  lw    a4, H_REDUCE_VALUE_EXTRA(gp)
  add   a4, a4, t6
  sw    a4, H_REDUCE_VALUE_EXTRA(gp)
#endif
  bne   t3, t2, do_wfi

  lw    t0, H_REDUCE_PARENT(gp)
  bnez  t0, final_reduce_send_parent
  li    a0, PHASE_FINAL_DRAIN
  mv    a1, t4
  mv    a2, t5
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  mv    a3, a4
#else
  li    a3, 0
#endif
  YIELDI zero, TAG_ROOT_COMPLETE
  j     do_wfi

final_reduce_send_parent:
#if MNIST_HORIZONTAL_GROUP_COUNT == 16
  srli  a5, a4, 29
  slli  a5, a5, 31
  or    a1, t2, a5
  slli  a5, a4, 3
  li    a0, PHASE_FINAL_DRAIN
  or    a0, a0, a5
#else
  li    a0, PHASE_FINAL_DRAIN
  mv    a1, t2
#endif
  mv    a2, t4
  mv    a3, t5
  SENDI t0, TAG_NEURON_WORKER
  j     do_wfi

fail_final_reduce:
  li    a0, 0xbadd0000
  j     fail
  .popsection
#endif

#if MNIST_RULE != MT_RULE_THREE
#include "mnist_fifth_root.inc"
#endif

#endif /* MNIST_SPARSE_MODE */
