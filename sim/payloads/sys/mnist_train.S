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

#if MNIST_RULE < 1 || MNIST_RULE > 3
#error "MNIST_RULE must be 1, 2, or 3"
#endif

#if MNIST_RECURRENT_MODE < 0 || MNIST_RECURRENT_MODE > 2
#error "MNIST_RECURRENT_MODE must be 0, 1, or 2"
#endif

#if MNIST_TRACE_BUNDLE != 0 && MNIST_TRACE_BUNDLE != 1
#error "MNIST_TRACE_BUNDLE must be 0 or 1"
#endif

#define SPM_BASE                    0x20000000
#define STOP_ADDR                   0x40000000
#define TIMER_ADDR                  0x40000004
#define PRINT_ADDR                  0x4000000c
#define MC_DST_BASE                 0x8001

#define MT_MAGIC                    0x4d543036
#define MT_PU_COUNT                 512
#define MT_MC_COUNT                 16
#define MT_INPUT_COUNT              784
#define MT_EXC_COUNT                400
#define MT_INH_COUNT                400

#define MT_RULE_ONE                 1
#define MT_RULE_TWO                 2
#define MT_RULE_THREE               3

#define MT_REC_SCATTER              0
#define MT_REC_TREE                 1
#define MT_REC_TREE_AGGREGATE       2

#define PHASE_FRAME                 1
#define PHASE_INPUT_PREPARE         2
#define PHASE_INPUT_SEND            3
#define PHASE_NEURON_UPDATE         4
#define PHASE_WEIGHT_UPDATE         5
#define PHASE_FINAL_DRAIN           6

#define INPUT_FIRED                 1
#define INPUT_GLOBAL_REPLICATED     (1 << 30)
#define INPUT_GLOBAL_PRIMARY        (1 << 31)
#define NEURON_FIRED                1
#define NEURON_FF_INPUT             2

#define INPUT_SIZE                  28
#define EXC_SIZE                    36
#define INH_SIZE                    148
#define TARGET_SIZE                 4
#define TRACE_SLOT_NONE             511
#define TRACE_BYTE_MASK             (TRACE_SLOT_NONE << 2)
#define TRACE_EXPECT_SHIFT          20
#define TRACE_COMMON_CFG            ((3 << 3) | 4)
#define FF_SOURCE_MASK              1023
#define FF_SOURCE_MARK_SHIFT        26
#define OBSERVED_MASK_WORDS         (MT_PU_COUNT / 32)

/* mt_spm_header_t byte offsets. */
#define H_MAGIC                     0
#define H_RULE                      4
#define H_PU_INDEX                  8
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
#define H_TRACE_SEND_NEXT           252
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
#define H_OBSERVED_MASK_OFFSET      300
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
  .option nopic
  .align 2
  .globl _start

_start:
  /* snn_init() is the only routine that uses the two private SPM stacks. */
  mv    t6, a0
  slli  t0, t6, 11
  li    sp, SPM_BASE + 0x3800
  add   sp, sp, t0
  bnez  t6, thread1_wait

  call  snn_init
  li    gp, SPM_BASE
  call  install_handlers

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

  /* 1: thread-1 neuron result joins back onto thread 0. */
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

  /* 3: root completion, argc=3, thread 0, up to 16 sends plus phase start. */
  la    t0, root_complete_handler
  csrw  0x703, t0
  li    t0, (17 << 16) | 17
  csrw  0x723, t0
  li    t0, (1 << 3) | 3
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

  /* 7: local phase work, argc=3, thread 0. */
  la    t0, phase_handler
  csrw  0x707, t0
  li    t0, (1 << 16) | 1
  csrw  0x727, t0
  li    t0, (1 << 3) | 3
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

  /* 10: phase-start tree, argc=3, thread 0, nine children plus local work. */
  la    t0, phase_start_handler
  csrw  0x70a, t0
  li    t0, (10 << 16) | 10
  csrw  0x72a, t0
  li    t0, (1 << 3) | 3
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

  /* 15: update the second local E/I pair on thread 1. */
  la    t0, neuron_worker_handler
  csrw  0x70f, t0
  li    t0, (1 << 16) | 1
  csrw  0x72f, t0
  li    t0, (2 << 3) | 0
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
  lw    t1, H_PU_INDEX(gp)
  addi  t1, t1, 1
  bne   t0, t1, fail_layout

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
  lw    t2, H_OBSERVED_MASK_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_OBSERVED_VECTOR_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_INPUT_GROUP_MASK(gp)
  beqz  t2, fail_role
  andi  t3, t2, -16
  bnez  t3, fail_role
  lw    t2, H_TARGET_SEGMENT_COUNT(gp)
  beqz  t2, fail_role
  li    t3, 4
  bgtu  t2, t3, fail_role
  lw    t3, H_POSTS_PER_SEGMENT(gp)
  beqz  t3, fail_role
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
  li    t3, 3
  bgtu  t2, t3, fail_role
  lw    t2, H_INPUT_PU_COUNT(gp)
  beqz  t2, fail_role
  lw    t2, H_INPUT_DEST_OFFSET(gp)
  beqz  t2, fail_role
  lw    t2, H_EXC_TARGET_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_FIRED_VECTOR_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_OBSERVED_MASK_OFFSET(gp)
  beqz  t2, fail_role
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
  lw    t2, H_OBSERVED_MASK_OFFSET(gp)
  bnez  t2, fail_role
  lw    t2, H_OBSERVED_VECTOR_OFFSET(gp)
  bnez  t2, fail_role

report_send_ready:
  li    t0, 1
  SENDI t0, TAG_READY_I
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
  lw    t0, H_INPUT_COUNT(gp)
  bgeu  a0, t0, fail_frame
  slli  t1, a0, 5
  slli  t2, a0, 2
  sub   t1, t1, t2
  lw    t2, H_INPUT_OFFSET(gp)
  add   t1, t1, t2
  add   t1, t1, gp
  sw    a1, IN_THRESHOLD(t1)
  li    t0, 1
  li    t1, SPM_BASE + H_FRAME_RECEIVED
  amoadd.w zero, t0, (t1)
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
  li    t0, 1
  li    t1, SPM_BASE + H_ROOT_IDLE_COUNT
  amoadd.w t2, t0, (t1)
  addi  t2, t2, 1
  lw    t3, H_NUM_MC(gp)
  bne   t2, t3, do_wfi
  lw    t0, H_PHASE(gp)
  li    t1, PHASE_FRAME
  beq   t0, t1, start_step
  lw    t0, H_ROOT_STEP_DONE(gp)
  bnez  t0, complete_step
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
 * Phase-start tree: a0=phase, a1=low phase value, a2=high phase value.
 * s10/s11 are temporary only while this non-yielding handler fans out.
 */
phase_start_handler:
  mv    s10, a0
  mv    s11, a1
  sw    a2, H_NEURON_LOCAL_VALUE(gp)
  li    t0, PHASE_INPUT_PREPARE
  bltu  s10, t0, fail_phase_start
  li    t0, PHASE_FINAL_DRAIN
  bgtu  s10, t0, fail_phase_start

  sw    s10, H_PHASE(gp)
  sw    zero, H_REDUCE_COUNT(gp)
  sw    zero, H_REDUCE_VALUE(gp)
  sw    zero, H_REDUCE_VALUE_HI(gp)

  /* Handler 4 is opened below only on input PUs after their pre-decay. */
  li    t0, 4
  csrw  0x714, t0

  /*
   * Initialize feedforward accumulators one phase early.  Completion of the
   * input-prepare reduction then guarantees this state exists before any PU
   * can emit a spike, even when phase-start subtrees have different latency.
   */
  li    t0, PHASE_INPUT_PREPARE
  bne   s10, t0, phase_start_not_input_prepare
  lw    s7, H_EXC_COUNT(gp)
  beqz  s7, phase_start_not_input_prepare

  /* Forget last step's active input PUs before feedforward delivery begins. */
  sw    zero, H_OBSERVED_COUNT(gp)
  lw    t0, H_OBSERVED_MASK_OFFSET(gp)
  beqz  t0, fail_phase_start
  add   t0, t0, gp
  li    t1, OBSERVED_MASK_WORDS
phase_start_clear_observed_loop:
  sw    zero, 0(t0)
  addi  t0, t0, 4
  addi  t1, t1, -1
  bnez  t1, phase_start_clear_observed_loop

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
  li    t0, PHASE_INPUT_SEND
  bne   s10, t0, phase_start_not_input_send
  sw    s11, H_FF_EXPECTED(gp)
  li    t0, MT_INPUT_COUNT
  bgtu  s11, t0, fail_phase_start
  mv    s5, s11

phase_start_not_input_send:
  li    t0, PHASE_NEURON_UPDATE
  bne   s10, t0, phase_start_not_neuron
#if MNIST_TRACE_BUNDLE == 0
#if MNIST_RULE == MT_RULE_ONE
  sw    zero, H_TRACE_RECEIVED(gp)
#else
  lw    t0, H_FIRED_COUNT(gp)
  beqz  t0, phase_start_trace_count_up
  lw    t0, H_OWNED_POST_COUNT(gp)
  beqz  t0, fail_phase_start
  li    t1, MT_EXC_COUNT
  bgtu  t0, t1, fail_phase_start
  sw    t0, H_TRACE_RECEIVED(gp)
  j     phase_start_trace_count_ready
phase_start_trace_count_up:
  sw    zero, H_TRACE_RECEIVED(gp)
phase_start_trace_count_ready:
#endif
#else
  sw    zero, H_TRACE_RECEIVED(gp)
#endif
  sw    zero, H_DONE_SENT(gp)

  /*
   * Do this in the preceding phase.  Its full-tree reduction is the barrier
   * that makes every input row ready before any neuron PU can send a trace.
   */
  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, phase_start_not_neuron
  lw    t1, H_INPUT_OFFSET(gp)
  add   t1, t1, gp
  li    t2, F_PRE_DECAY
phase_start_pre_decay_loop:
  lw    t3, IN_PRE_TRACE(t1)
  fmul.s t3, t3, t2
  sw    t3, IN_PRE_TRACE(t1)
  addi  t1, t1, INPUT_SIZE
  addi  t0, t0, -1
  bnez  t0, phase_start_pre_decay_loop

  /* Select the per-PU reverse entry once instead of testing every message. */
#if MNIST_RULE == MT_RULE_ONE
  la    t0, trace_event_fast_handler
  li    t4, TRACE_COMMON_CFG
#else
  li    t4, TRACE_COMMON_CFG
  lw    t1, H_FIRED_COUNT(gp)
  la    t0, trace_event_fast_handler
  beqz  t1, phase_start_trace_entry_ready
  lw    t2, H_FIRED_VECTOR_OFFSET(gp)
  beqz  t2, fail_phase_start
  add   t2, t2, gp
  lw    t2, 0(t2)
  sw    t2, H_CURSOR0(gp)
  li    t2, 1
  bne   t1, t2, phase_start_trace_multi_pre
  la    t0, trace_event_one_pre_handler
  j     phase_start_trace_entry_ready
phase_start_trace_multi_pre:
  la    t0, trace_event_handler
  /* Per-thread registers and disjoint columns leave only the completion AMO shared. */
  li    t4, TRACE_COMMON_CFG
phase_start_trace_entry_ready:
#endif
  sw    t4, H_TRACE_CFG(gp)
  csrw  0x704, t0
  csrw  0x714, t4

phase_start_not_neuron:
  li    t0, PHASE_WEIGHT_UPDATE
  bne   s10, t0, phase_start_fanout
  sw    zero, H_DONE_SENT(gp)
  sw    zero, H_TRACE_SEND_NEXT(gp)
  sw    zero, H_TRACE_EXPECTED(gp)

#if MNIST_TRACE_BUNDLE == 0
  srli  t0, s11, 30
  bnez  t0, fail_phase_start
  andi  t1, s11, 1023
  li    t0, MT_INH_COUNT
  bgtu  t1, t0, fail_phase_start
  srli  t2, s11, 10
  andi  t2, t2, 1023
  li    t0, MT_EXC_COUNT
  bgtu  t2, t0, fail_phase_start
  srli  t3, s11, 20
  andi  t3, t3, 1023
  bgtu  t3, t0, fail_phase_start
  lw    a4, H_NEURON_LOCAL_VALUE(gp)
  srli  t5, a4, 20
  bnez  t5, fail_phase_start
  andi  a5, a4, 1023
  bgtu  a5, t0, fail_phase_start
  srli  a6, a4, 10
  andi  a6, a6, 1023
  bgtu  a6, t0, fail_phase_start
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  bnez  t1, fail_phase_start
#endif

  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, phase_start_fanout
  lw    t0, H_INPUT_GROUP_MASK(gp)
  beqz  t0, fail_phase_start
  li    t4, 0
  andi  t5, t0, 1
  beqz  t5, phase_start_trace_expect_group1
  add   t4, t4, t2
phase_start_trace_expect_group1:
  andi  t5, t0, 2
  beqz  t5, phase_start_trace_expect_group2
  add   t4, t4, t3
phase_start_trace_expect_group2:
  andi  t5, t0, 4
  beqz  t5, phase_start_trace_expect_group3
  add   t4, t4, a5
phase_start_trace_expect_group3:
  andi  t5, t0, 8
  beqz  t5, phase_start_trace_expect_ready
  add   t4, t4, a6
phase_start_trace_expect_ready:
  andi  t5, t0, -16
  bnez  t5, fail_phase_start
  sw    t4, H_TRACE_EXPECTED(gp)
#else
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  slli  t0, s11, 16
  bnez  t0, fail_phase_start
#else
  li    t0, MT_INH_COUNT
  slli  t1, s11, 16
  srli  t1, t1, 16
  bgtu  t1, t0, fail_phase_start
#endif
  srli  t1, s11, 16
  li    t0, MT_EXC_COUNT
  bgtu  t1, t0, fail_phase_start
#endif

  /* Input PUs keep the already-decayed, already-counting handler open. */
  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, phase_start_fanout
  lw    t0, H_TRACE_CFG(gp)
  csrw  0x714, t0

phase_start_fanout:
  csrr  t0, 0xf14
  addi  t1, t0, -1
  bnez  t1, phase_start_nonroot
  li    t2, MT_PU_COUNT / 2
  j     phase_start_child_loop
phase_start_nonroot:
  neg   t2, t1
  and   t2, t2, t1
  srli  t2, t2, 1

phase_start_child_loop:
  beqz  t2, phase_start_local
  or    t3, t1, t2
  addi  t3, t3, 1
  mv    a0, s10
  mv    a1, s11
  lw    a2, H_NEURON_LOCAL_VALUE(gp)
  li    a3, 0
  SENDI t3, TAG_PHASE_START
  srli  t2, t2, 1
  j     phase_start_child_loop

phase_start_local:
  mv    a0, s10
  mv    a1, s11
#if MNIST_TRACE_BUNDLE == 0
  /* Reverse events may run before the local phase handler after this yield. */
  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, phase_start_cursor_ready
  addi  s8, gp, H_TRACE_RECEIVED
  li    s9, -1
  li    s10, 1
phase_start_cursor_ready:
#endif
  lw    a2, H_NEURON_LOCAL_VALUE(gp)
  li    a3, 0
  YIELDI zero, TAG_PHASE
  j     do_wfi

fail_phase_start:
  li    a0, 0xbadb0000
  j     fail

/*
 * Phase reduction: a0=phase, a1=subtree size, a2=low value, a3=high value.
 * The tree count itself serializes this thread-0-only handler.
 */
reduce_handler:
  lw    t0, H_PHASE(gp)
  bne   a0, t0, fail_reduce
  beqz  a1, fail_reduce
  csrr  t0, 0xf14
  addi  t1, t0, -1
  bnez  t1, reduce_nonroot
  li    t2, MT_PU_COUNT
  j     reduce_expected_ready
reduce_nonroot:
  neg   t2, t1
  and   t2, t2, t1
reduce_expected_ready:
  lw    t3, H_REDUCE_COUNT(gp)
  add   t3, t3, a1
  bgtu  t3, t2, fail_reduce
  sw    t3, H_REDUCE_COUNT(gp)

  lw    t4, H_REDUCE_VALUE(gp)
  li    t5, PHASE_INPUT_PREPARE
  beq   a0, t5, reduce_add_value
  li    t5, PHASE_NEURON_UPDATE
  beq   a0, t5, reduce_add_value
  xor   t4, t4, a2
  j     reduce_store_value
reduce_add_value:
  add   t4, t4, a2
reduce_store_value:
  sw    t4, H_REDUCE_VALUE(gp)
  lw    t5, H_REDUCE_VALUE_HI(gp)
  add   t5, t5, a3
  sw    t5, H_REDUCE_VALUE_HI(gp)
  bne   t3, t2, do_wfi

  li    t3, 1
  bne   t0, t3, reduce_send_parent
  mv    a1, t4
  mv    a2, t5
  li    a3, 0
  YIELDI zero, TAG_ROOT_COMPLETE
  j     do_wfi

reduce_send_parent:
  sub   t0, t0, t2
  mv    a1, t2
  mv    a2, t4
  mv    a3, t5
  SENDI t0, TAG_REDUCE
  j     do_wfi

fail_reduce:
  li    a0, 0xbadc0000
  j     fail

/* Root phase completion: a0=phase, a1/a2=low/high aggregate value. */
root_complete_handler:
  lw    t0, H_PHASE(gp)
  bne   a0, t0, fail_root
  li    t0, PHASE_INPUT_PREPARE
  beq   a0, t0, root_start_input_send
  li    t0, PHASE_INPUT_SEND
  beq   a0, t0, root_start_neuron
  li    t0, PHASE_NEURON_UPDATE
  beq   a0, t0, root_neuron_done
  li    t0, PHASE_WEIGHT_UPDATE
  beq   a0, t0, root_weight_done
  li    t0, PHASE_FINAL_DRAIN
  beq   a0, t0, root_final_done
  j     fail_root

root_start_input_send:
  li    a0, PHASE_INPUT_SEND
  /* a1 already holds the global fired-input count. */
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_PHASE_START
  j     do_wfi

root_start_neuron:
  li    a0, PHASE_NEURON_UPDATE
  li    a1, 0
  li    a2, 0
  li    a3, 0
  YIELDI zero, TAG_PHASE_START
  j     do_wfi

root_neuron_done:
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  /* Re-register phase-scoped MC idleness for recurrent traffic. */
  sw    zero, H_ROOT_IDLE_COUNT(gp)
  sw    zero, H_ROOT_STEP_DONE(gp)
  li    t0, MC_DST_BASE
  li    t1, MC_DST_BASE + MT_MC_COUNT
  li    t2, TAG_IDLE_REGISTER
root_idle_register_loop:
  li    a0, (0x100 << 16) | TAG_IDLE_RETURN
  li    a1, 0
  li    a2, 0
  li    a3, 0
  SEND  t0, t2
  addi  t0, t0, 1
  bne   t0, t1, root_idle_register_loop
#endif
  li    a0, PHASE_WEIGHT_UPDATE
  /* Restore both words after optional idle-registration payload traffic. */
  lw    a1, H_REDUCE_VALUE(gp)
  lw    a2, H_REDUCE_VALUE_HI(gp)
  li    a3, 0
  YIELDI zero, TAG_PHASE_START
  j     do_wfi

root_weight_done:
#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
  li    t0, 1
  sw    t0, H_ROOT_STEP_DONE(gp)
  lw    t1, H_ROOT_IDLE_COUNT(gp)
  lw    t2, H_NUM_MC(gp)
  bne   t1, t2, do_wfi
#endif
  j     complete_step

root_final_done:
  li    t0, TIMER_ADDR
  sw    zero, 0(t0)
  li    t0, PRINT_ADDR
  sw    a1, 0(t0)
  lw    t1, H_EXPECTED_CHECKSUM(gp)
  beqz  t1, root_success
  bne   t1, a1, root_checksum_fail
root_success:
  li    t0, STOP_ADDR
  sw    zero, 0(t0)
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
  sw    a0, H_PHASE(gp)
  li    t0, PHASE_INPUT_PREPARE
  beq   a0, t0, phase_input_prepare
  li    t0, PHASE_INPUT_SEND
  beq   a0, t0, phase_input_send
  li    t0, PHASE_NEURON_UPDATE
  beq   a0, t0, phase_neuron_update
  li    t0, PHASE_WEIGHT_UPDATE
  beq   a0, t0, phase_weight_update
  li    t0, PHASE_FINAL_DRAIN
  beq   a0, t0, phase_final_drain
  li    a0, 0xbad90000
  j     fail

/* Advance each local xorshift stream and build a flat vector of weight rows. */
phase_input_prepare:
  lw    t0, H_FRAME_RECEIVED(gp)
  lw    t1, H_INPUT_COUNT(gp)
  bne   t0, t1, fail_input_prepare
  sw    zero, H_FF_EXPECTED(gp)
  sw    zero, H_FF_RECEIVED(gp)
  sw    zero, H_DONE_SENT(gp)
  sw    zero, H_FIRED_COUNT(gp)
  li    t6, 0
  li    a6, 0
  beqz  t1, input_prepare_done
  lw    t2, H_INPUT_OFFSET(gp)
  add   t2, t2, gp
  lw    a5, H_FIRED_VECTOR_OFFSET(gp)
  beqz  a5, fail_input_prepare
  add   a5, a5, gp
  li    t3, 0
input_prepare_loop:
  lw    t4, IN_RNG(t2)
  slli  t5, t4, 13
  xor   t4, t4, t5
  srli  t5, t4, 17
  xor   t4, t4, t5
  slli  t5, t4, 5
  xor   t4, t4, t5
  sw    t4, IN_RNG(t2)

  lw    t5, IN_THRESHOLD(t2)
  li    a4, -1
  beq   t5, a4, input_prepare_fired
  bgeu  t4, t5, input_prepare_not_fired
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
  addi  a5, a5, 4
  addi  t6, t6, 1
  j     input_prepare_next
input_prepare_not_fired:
  sw    zero, IN_FLAGS(t2)
input_prepare_next:
  addi  t3, t3, 1
  addi  t2, t2, INPUT_SIZE
  bne   t3, t1, input_prepare_loop

input_prepare_done:
  sw    t6, H_FIRED_COUNT(gp)
  mv    a2, a6
  li    a0, PHASE_INPUT_PREPARE
  j     reduce_local

fail_input_prepare:
  li    a0, 0xbad80000
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
  slli  t2, t2, 2
  add   a4, a5, t2
  lw    s8, 0(a4)
  csrr  t0, 0xf14
  slli  s11, t0, 16
  li    t0, (1 << FF_SOURCE_MARK_SHIFT)
  or    s11, s11, t0

input_work_start_row:
  lw    s0, H_EXC_TARGET_OFFSET(gp)
  beqz  s0, fail_input_work
  add   s0, s0, gp
  lw    s7, H_FIRST_WEIGHT_SLOT(gp)
  lw    s1, H_TARGET_SEGMENT_COUNT(gp)
  beqz  s1, fail_input_work
  addi  s1, s1, -1

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
  li    a2, 0
  li    a0, PHASE_INPUT_SEND
  j     reduce_local

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
  beqz  s1, input_work_row_done
  addi  s1, s1, -1
  mv    s0, s5
  lw    t0, H_POSTS_PER_SEGMENT(gp)
  add   s7, s7, t0
  j     input_work_prepare_segment

input_work_row_done:
  li    s11, 0
  beq   a4, a5, input_send_done
  addi  a4, a4, -4
  lw    s8, 0(a4)
  j     input_work_start_row

fail_input_work:
  li    a0, 0xbada0400
  j     fail

/* Feedforward delivery is serialized on thread 0 and accumulates in registers. */
ff_spike_handler:
  lw    t0, H_PHASE(gp)
  li    t1, PHASE_INPUT_PREPARE
  beq   t0, t1, ff_phase_ok
  li    t1, PHASE_INPUT_SEND
  bne   t0, t1, fail_ff_spike
ff_phase_ok:
  andi  t1, a0, 0xff
  bnez  t1, fail_ff_spike

  /* The first row from each active input PU atomically records its source. */
  srli  t0, a0, 27
  bnez  t0, fail_ff_spike
  srli  t0, a0, FF_SOURCE_MARK_SHIFT
  andi  t0, t0, 1
  beqz  t0, ff_source_ready
  srli  t0, a0, 16
  andi  t0, t0, FF_SOURCE_MASK
  beqz  t0, fail_ff_spike
  li    t1, MT_PU_COUNT
  bgtu  t0, t1, fail_ff_spike
  addi  t2, t0, -1
  srli  t3, t2, 5
  slli  t3, t3, 2
  lw    t4, H_OBSERVED_MASK_OFFSET(gp)
  beqz  t4, fail_ff_spike
  add   t4, t4, gp
  add   t4, t4, t3
  andi  t2, t2, 31
  li    t5, 1
  sll   t5, t5, t2
  amoor.w t6, t5, (t4)
  and   t6, t6, t5
  bnez  t6, ff_source_ready
  addi  t1, gp, H_OBSERVED_COUNT
  li    t2, 1
  amoadd.w t6, t2, (t1)
  lw    t2, H_PHYSICAL_INPUT_PU_COUNT(gp)
  bgeu  t6, t2, fail_ff_spike
  lw    t2, H_OBSERVED_VECTOR_OFFSET(gp)
  beqz  t2, fail_ff_spike
  add   t2, t2, gp
  slli  t6, t6, 1
  add   t2, t2, t6
  sh    t0, 0(t2)
ff_source_ready:
  beqz  s6, ff_mark_input
ff_accumulate:
  fadd.s s3, s3, a1
  srli  t1, a0, 8
  andi  t1, t1, 0xff
  li    t2, 0xff
  beq   t1, t2, ff_one_value
  li    t2, 1
  bne   t1, t2, fail_ff_spike
  li    t2, 2
  bne   s7, t2, fail_ff_spike
  fadd.s s4, s4, a2
ff_one_value:
  addi  s6, s6, 1
  lw    t0, H_PHASE(gp)
  li    t1, PHASE_INPUT_SEND
  bne   t0, t1, do_wfi
  lw    s5, H_FF_EXPECTED(gp)
  bgtu  s6, s5, fail_ff_count
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
  sw    s3, EXC_GE(s1)
  li    t0, 2
  bne   s7, t0, ff_finish_count
  sw    s4, EXC_SIZE + EXC_GE(s1)
ff_finish_count:
  sw    s6, H_FF_RECEIVED(gp)
  li    a2, 0
  li    a0, PHASE_INPUT_SEND
  j     reduce_local

fail_ff_spike:
  li    a0, 0xbad70000
  j     fail

fail_ff_count:
  li    a0, 0xbad73000
  j     fail

/* Update one pair locally, or two disjoint pairs across both SMT threads. */
phase_neuron_update:
  lw    t0, H_EXC_COUNT(gp)
  beqz  t0, neuron_update_done_empty
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

/* Thread-1-only handler for the second local pair. */
neuron_worker_handler:
  lw    t0, H_PHASE(gp)
  li    t1, PHASE_NEURON_UPDATE
  bne   t0, t1, fail_role
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

/* Always resume reduction and recurrent-scatter setup on thread 0. */
neuron_join_handler:
  lw    t0, H_PHASE(gp)
  li    t1, PHASE_NEURON_UPDATE
  bne   t0, t1, fail_role
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

/* s0=exc, s2=paired inh; return s9=exc fired, s8=inh fired. */
neuron_pair_update:
  li    s9, 0
  lw    t1, EXC_FLAGS(s0)
  andi  t1, t1, NEURON_FF_INPUT
  sw    t1, EXC_FLAGS(s0)
  lw    t2, EXC_REFRACTORY(s0)
  beqz  t2, neuron_pair_exc_active
  addi  t2, t2, -1
  sw    t2, EXC_REFRACTORY(s0)
  j     neuron_pair_exc_done

neuron_pair_exc_active:
  lw    t2, EXC_GE(s0)
  lw    t3, EXC_GI(s0)
  lw    t4, EXC_V(s0)
  li    a4, F_GE_HALF_DECAY
  fmul.s t5, t2, a4
  li    a4, F_GI_HALF_DECAY
  fmul.s t6, t3, a4

  li    a4, F_EXC_REST
  fsub.s a4, a4, t4
  fsub.s a5, zero, t4
  fmul.s a5, t5, a5
  fadd.s a4, a4, a5
  li    a5, F_EXC_INH_REV
  fsub.s a5, a5, t4
  fmul.s a5, t6, a5
  fadd.s a4, a4, a5
  li    a5, F_EXC_DT
  fmul.s a4, a4, a5
  fadd.s t4, t4, a4
  sw    t4, EXC_V(s0)

  li    a4, F_GE_DECAY
  fmul.s t2, t2, a4
  sw    t2, EXC_GE(s0)
  li    a4, F_GI_DECAY
  fmul.s t3, t3, a4
  sw    t3, EXC_GI(s0)
  lw    t2, EXC_THETA(s0)
  li    a4, F_THETA_DECAY
  fmul.s t2, t2, a4
  sw    t2, EXC_THETA(s0)
  li    a4, F_EXC_THRESHOLD_OFFSET
  fsub.s a4, t2, a4
  flt.s a5, a4, t4
  beqz  a5, neuron_pair_exc_done
  li    t4, F_EXC_RESET
  sw    t4, EXC_V(s0)
  li    t4, 10
  sw    t4, EXC_REFRACTORY(s0)
  li    a4, F_THETA_INCREMENT
  fadd.s t2, t2, a4
  sw    t2, EXC_THETA(s0)
  ori   t1, t1, NEURON_FIRED
  sw    t1, EXC_FLAGS(s0)
  li    s9, 1

neuron_pair_exc_done:
  li    s8, 0
  sw    zero, INH_FLAGS(s2)
  lw    t1, INH_REFRACTORY(s2)
  beqz  t1, neuron_pair_inh_active
  addi  t1, t1, -1
  sw    t1, INH_REFRACTORY(s2)
  j     neuron_pair_inh_done

neuron_pair_inh_active:
  lw    t2, INH_GE(s2)
  lw    t3, INH_GI(s2)
  lw    t4, INH_V(s2)
  li    a4, F_GE_HALF_DECAY
  fmul.s t5, t2, a4
  li    a4, F_GI_HALF_DECAY
  fmul.s t6, t3, a4

  li    a4, F_INH_REST
  fsub.s a4, a4, t4
  fsub.s a5, zero, t4
  fmul.s a5, t5, a5
  fadd.s a4, a4, a5
  li    a5, F_INH_INH_REV
  fsub.s a5, a5, t4
  fmul.s a5, t6, a5
  fadd.s a4, a4, a5
  li    a5, F_INH_DT
  fmul.s a4, a4, a5
  fadd.s t4, t4, a4
  sw    t4, INH_V(s2)

  li    a4, F_GE_DECAY
  fmul.s t2, t2, a4
  sw    t2, INH_GE(s2)
  li    a4, F_GI_DECAY
  fmul.s t3, t3, a4
  sw    t3, INH_GI(s2)
  li    a4, F_INH_THRESHOLD
  flt.s a5, a4, t4
  beqz  a5, neuron_pair_inh_done
  li    t4, F_INH_RESET
  sw    t4, INH_V(s2)
  li    t4, 4
  sw    t4, INH_REFRACTORY(s2)
  li    t4, NEURON_FIRED
  sw    t4, INH_FLAGS(s2)
  li    s8, 1

neuron_pair_inh_done:
  /* Preserve the model's one-step recurrent delay within this pair. */
  lw    t2, EXC_FLAGS(s0)
  andi  t2, t2, NEURON_FIRED
  beqz  t2, neuron_pair_done
  lw    t3, INH_GE(s2)
  li    t4, F_E_TO_I
  fadd.s t3, t3, t4
  sw    t3, INH_GE(s2)
neuron_pair_done:
  jr    s7

neuron_update_finalize:

#if MNIST_TRACE_BUNDLE == 0
  li    s10, 0
  li    s11, 0
  lw    t0, H_NEURON_GROUP(gp)
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
  li    a0, PHASE_NEURON_UPDATE
  j     reduce_local_pair
#endif

#if MNIST_RECURRENT_MODE == MT_REC_SCATTER
neuron_update_reduce_scatter:
  mv    a2, s10
  mv    a3, s11
  li    a0, PHASE_NEURON_UPDATE
  j     reduce_local_pair
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
  li    a2, 0
  li    a0, PHASE_NEURON_UPDATE
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
  li    a0, PHASE_NEURON_UPDATE
  j     reduce_local_pair

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
  lw    t0, H_EXC_COUNT(gp)
  beqz  t0, weight_phase_non_neuron
#if MNIST_TRACE_BUNDLE == 0
  andi  s11, a1, 1023
  li    t1, MT_INH_COUNT
  bgtu  s11, t1, fail_weight_phase
  lw    t0, H_NEURON_GROUP(gp)
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

  /* Post traces are sent in their already-decayed form. */
  mv    t0, s7
  lw    t1, H_EXC_COUNT(gp)
  li    t4, F_POST1_DECAY
  li    t5, F_POST2_DECAY
weight_post_decay_loop:
  lw    t2, EXC_POST1(t0)
  lw    t3, EXC_POST2(t0)
  fmul.s t2, t2, t4
  fmul.s t3, t3, t5
  sw    t2, EXC_POST1(t0)
  sw    t3, EXC_POST2(t0)
  addi  t0, t0, EXC_SIZE
  addi  t1, t1, -1
  bnez  t1, weight_post_decay_loop

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
  lw    a1, EXC_POST1(s7)
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
  slli  t2, t1, 18
  or    a0, a0, t2
  li    a2, 0
  li    a3, 0

  lw    t3, H_EXC_COUNT(gp)
  li    t4, 2
  bne   t3, t4, weight_trace_payload_ready
  addi  t4, s7, EXC_SIZE
  lw    t5, EXC_WEIGHT_SLOT(t4)
  lw    t6, EXC_POST1(t4)
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
  slli  t2, a4, 18
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
  srli  t0, a0, 18
  andi  t0, t0, 1
  bnez  t0, weight_trace_first_dest_ready
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
  srli  t0, s4, 18
  andi  t0, t0, 1
  bnez  t0, weight_trace_destinations_ready
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
  li    a4, 0
  sw    a4, H_TRACE_SEND_NEXT(gp)
  beq   s1, s2, weight_trace_first_empty
  YIELDI zero, TAG_TRACE_SEND_I
  j     do_wfi

weight_trace_first_empty:
  bnez  s3, trace_send_second_pass
  j     finish_neuron_traces

weight_phase_non_neuron:
  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, weight_phase_unused
#if MNIST_TRACE_BUNDLE == 0
#if MNIST_RULE != MT_RULE_ONE
  lw    t0, H_FIRED_COUNT(gp)
  beqz  t0, weight_phase_count_up
  lw    t1, 0(s8)
  lw    t2, H_OWNED_POST_COUNT(gp)
  beqz  t2, fail_trace_count
  bgtu  t1, t2, fail_trace_count
  bnez  t1, do_wfi
  j     finish_input_traces
weight_phase_count_up:
#endif
  lw    t2, H_TRACE_EXPECTED(gp)
  li    t0, MT_EXC_COUNT
  bgtu  t2, t0, fail_trace_count
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
  lw    t0, H_INPUT_OFFSET(gp)
  add   t0, t0, gp
  lw    t1, H_INPUT_COUNT(gp)
finish_input_trace_loop:
  lw    t2, IN_FLAGS(t0)
  andi  t2, t2, INPUT_FIRED
  beqz  t2, finish_input_trace_clear
#if MNIST_RULE == MT_RULE_THREE
  li    t3, F_ONE
  sw    t3, IN_PRE_TRACE(t0)
#else
  lw    t3, IN_PRE_TRACE(t0)
  li    t4, F_ONE
  fadd.s t3, t3, t4
  sw    t3, IN_PRE_TRACE(t0)
#endif
finish_input_trace_clear:
  sw    zero, IN_FLAGS(t0)
  addi  t0, t0, INPUT_SIZE
  addi  t1, t1, -1
  bnez  t1, finish_input_trace_loop
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
  j     reduce_local

/* No-local-pre entry; a postsynaptic spike redirects to the full updater. */
trace_event_fast_handler:
#if MNIST_TRACE_BUNDLE == 0
  /* Inactive input PUs receive only globally delivered firing posts. */
  srli  t0, a0, 19
  bnez  t0, fail_trace_event
  srli  t0, a0, 18
  andi  t0, t0, 1
  beqz  t0, fail_trace_event
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
  srli  t0, a0, 18
  andi  t0, t0, 3
  bnez  t0, trace_event_handler
  srli  t1, a0, 9
  andi  t1, t1, TRACE_SLOT_NONE
  li    t2, TRACE_SLOT_NONE
  li    t0, 1
  beq   t1, t2, trace_event_add_count_active
  li    t0, 2
  j     trace_event_add_count_active
#endif

#if MNIST_RULE != MT_RULE_ONE
/* Exactly one locally fired pre: direct row update, with no list scan. */
trace_event_one_pre_handler:
#if MNIST_TRACE_BUNDLE == 0
  srli  t0, a0, 18
  andi  t0, t0, 1
  bnez  t0, trace_event_handler
#if MNIST_RULE == MT_RULE_THREE
  andi  t1, a0, TRACE_BYTE_MASK
  lw    t0, H_CURSOR0(gp)
  add   tp, t0, t1
  lw    t5, 0(tp)
  fsub.s t5, t5, a1
  fmax.s t5, t5, zero
  sw    t5, 0(tp)
#else
  andi  s1, a0, TRACE_BYTE_MASK
  mv    s2, a1
  jal   s6, depress_one_pre
#endif
  j     trace_event_add_count_active
#else
  srli  t0, a0, 18
  andi  t0, t0, 3
  bnez  t0, trace_event_handler
  mv    s8, a0
  mv    s9, a2
  andi  s1, a0, TRACE_SLOT_NONE
  mv    s2, a1
  jal   s6, depress_one_pre
  srli  t0, s8, 9
  andi  s1, t0, TRACE_SLOT_NONE
  li    t1, TRACE_SLOT_NONE
  beq   s1, t1, trace_one_pre_count_one
  mv    s2, s9
  jal   s6, depress_one_pre
  li    t0, 2
  j     trace_event_add_count_active
trace_one_pre_count_one:
  li    t0, 1
  j     trace_event_add_count_active
#endif

depress_one_pre:
  lw    t0, H_CURSOR0(gp)
#if MNIST_TRACE_BUNDLE == 0
  add   tp, t0, s1
#else
  slli  t1, s1, 2
  add   tp, t0, t1
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

/* Reverse trace payload; bundled mode may carry a second weight slot. */
trace_event_handler:
#if MNIST_TRACE_BUNDLE == 0
  srli  t0, a0, 19
  bnez  t0, fail_trace_event
  andi  s1, a0, TRACE_BYTE_MASK
  mv    s2, a1
  srli  s4, a0, 18
  andi  s4, s4, 1
  li    s3, 0
  beqz  s4, trace_single_payload_ready
  mv    s3, a3
trace_single_payload_ready:
  jal   s7, apply_trace_slot
  j     trace_event_add_count_active
#else
  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, fail_trace_event
  srli  t0, a0, 20
  bnez  t0, fail_trace_event
  mv    s8, a0
  mv    s9, a2
  mv    s10, a3
  srli  s11, a0, 18
  andi  s11, s11, 3

  andi  s1, a0, TRACE_SLOT_NONE
  lw    t0, H_NEURON_PU_COUNT(gp)
  slli  t0, t0, 1
  bgeu  s1, t0, fail_trace_event
  mv    s2, a1
  andi  s4, s11, 1
  li    s3, 0
  beqz  s4, trace_first_payload_ready
  mv    s3, s10
trace_first_payload_ready:
  jal   s7, apply_trace_slot

  srli  t0, s8, 9
  andi  s1, t0, TRACE_SLOT_NONE
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
  li    t0, 2
  j     trace_event_add_count_active

trace_event_count_one:
  andi  t0, s11, 2
  bnez  t0, fail_trace_event
  li    t0, 1
#endif
trace_event_add_count_active:
#if MNIST_TRACE_BUNDLE == 0
  amoadd.w t2, s9, (s8)
  bne   t2, s10, do_wfi
#else
  addi  t1, gp, H_TRACE_RECEIVED
  amoadd.w t2, t0, (t1)
  add   t2, t2, t0
  li    t3, MT_EXC_COUNT
  bgtu  t2, t3, fail_trace_count
  bne   t2, t3, do_wfi
#endif
trace_event_count_complete:
  lw    t0, H_PHASE(gp)
  li    t1, PHASE_WEIGHT_UPDATE
  bne   t0, t1, do_wfi
  j     finish_input_traces

#if MNIST_TRACE_BUNDLE == 0
trace_event_add_count_inactive:
  addi  t1, gp, H_TRACE_RECEIVED
  li    t0, 1
  amoadd.w t2, t0, (t1)
  addi  t2, t2, 1
  lw    t0, H_PHASE(gp)
  li    t1, PHASE_WEIGHT_UPDATE
  bne   t0, t1, do_wfi
  lw    t3, H_TRACE_EXPECTED(gp)
  bgtu  t2, t3, fail_trace_count
  bne   t2, t3, do_wfi
  j     trace_event_count_complete
#endif

fail_trace_event:
  li    a0, 0xbada1000
  j     fail

/*
 * s1=weight slot, s2=depression delta, s3=potentiation factor,
 * s4=post-fired.
 * The routine preserves s0-s11 except for s5, which is its current input.
 */
apply_trace_slot:
  beqz  s4, apply_trace_not_post
  lw    t0, H_INPUT_COUNT(gp)
  beqz  t0, apply_trace_return
#if MNIST_RULE == MT_RULE_THREE
  lw    a6, H_WEIGHT_MAX(gp)
#endif
  lw    s5, H_INPUT_OFFSET(gp)
  add   s5, s5, gp
  slli  t1, t0, 5
  slli  t2, t0, 2
  sub   t1, t1, t2
  add   s0, s5, t1
apply_trace_all_loop:
  jal   s6, update_synapse
  addi  s5, s5, INPUT_SIZE
  bne   s5, s0, apply_trace_all_loop
  jr    s7

apply_trace_not_post:
#if MNIST_RULE == MT_RULE_ONE
  jr    s7
#else
  lw    t0, H_FIRED_COUNT(gp)
  beqz  t0, apply_trace_return
  lw    t2, H_INPUT_COUNT(gp)
  bgtu  t0, t2, fail_trace_list
  lw    s5, H_FIRED_VECTOR_OFFSET(gp)
  beqz  s5, fail_trace_list
  add   s5, s5, gp
  slli  t0, t0, 2
  add   s0, s5, t0
#if MNIST_TRACE_BUNDLE == 1
  lw    t2, H_DEPRESSION_RATE(gp)
  fmul.s s2, s2, t2
#endif
#if MNIST_RULE == MT_RULE_THREE
  lw    a6, H_WEIGHT_MAX(gp)
#endif
apply_trace_fired_loop:
  lw    t0, 0(s5)
#if MNIST_TRACE_BUNDLE == 0
  add   tp, t0, s1
#else
  slli  t1, s1, 2
  add   tp, t0, t1
#endif
  lw    t5, 0(tp)
#if MNIST_RULE == MT_RULE_TWO
  mv    a0, t5
  jal   t6, fifth_root
  fmul.s t2, s2, a0
#else
  mv    t2, s2
#endif
  fsub.s t5, t5, t2
  flt.s t3, t5, zero
  beqz  t3, apply_trace_dep_nonnegative
  li    t5, 0
apply_trace_dep_nonnegative:
#if MNIST_RULE == MT_RULE_THREE
  mv    t4, a6
#else
  lw    t4, H_WEIGHT_MAX(gp)
#endif
  flt.s t3, t4, t5
  beqz  t3, apply_trace_dep_store
  mv    t5, t4
apply_trace_dep_store:
  sw    t5, 0(tp)
  addi  s5, s5, 4
  bne   s5, s0, apply_trace_fired_loop
#endif
apply_trace_return:
  jr    s7

fail_trace_list:
  li    a0, 0xbada1100
  j     fail

/* Update exactly one dense input-to-E synapse. */
update_synapse:
  lw    t0, IN_FLAGS(s5)
  andi  t0, t0, INPUT_FIRED
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
  beqz  t0, update_three_no_depression
#if MNIST_TRACE_BUNDLE == 0
  fsub.s t5, t5, s2
#else
  lw    t2, H_DEPRESSION_RATE(gp)
  fmul.s t2, t2, s2
  fsub.s t5, t5, t2
#endif
  flt.s t3, t5, zero
  beqz  t3, update_three_dep_nonnegative
  li    t5, 0
update_three_dep_nonnegative:
  mv    t4, a6
  flt.s t3, t4, t5
  beqz  t3, update_three_no_depression
  mv    t5, t4
update_three_no_depression:
  beqz  t0, update_three_pre_ready
  li    t1, F_ONE
update_three_pre_ready:
  mv    t2, s3
  fmul.s t2, t2, t1
  fadd.s t5, t5, t2
  flt.s t3, t5, zero
  beqz  t3, update_three_pot_nonnegative
  li    t5, 0
update_three_pot_nonnegative:
  mv    t4, a6
  flt.s t3, t4, t5
  beqz  t3, update_synapse_store
  mv    t5, t4

#elif MNIST_RULE == MT_RULE_TWO
  mv    t4, t1
  beqz  t0, update_two_effective_ready
  li    t2, F_ONE
  fadd.s t4, t4, t2
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
  flt.s t3, t5, zero
  beqz  t3, update_two_dep_nonnegative
  li    t5, 0
update_two_dep_nonnegative:
  lw    t2, H_WEIGHT_MAX(gp)
  flt.s t3, t2, t5
  beqz  t3, update_two_effective_ready
  mv    t5, t2
update_two_effective_ready:
  lw    t2, H_PRE_TARGET(gp)
  fsub.s t4, t4, t2
  lw    t2, H_WEIGHT_MAX(gp)
  fsub.s a0, t2, t5
  jal   t6, fifth_root
  lw    t2, H_POTENTIATION_RATE(gp)
  fmul.s t2, t2, t4
  fmul.s t2, t2, a0
  fadd.s t5, t5, t2
  flt.s t3, t5, zero
  beqz  t3, update_two_pot_nonnegative
  li    t5, 0
update_two_pot_nonnegative:
  lw    t2, H_WEIGHT_MAX(gp)
  flt.s t3, t2, t5
  beqz  t3, update_synapse_store
  mv    t5, t2

#else
  mv    t4, t1
  beqz  t0, update_one_effective_ready
  li    t2, F_ONE
  fadd.s t4, t4, t2
update_one_effective_ready:
  lw    t2, H_PRE_TARGET(gp)
  fsub.s t4, t4, t2
  lw    t2, H_WEIGHT_MAX(gp)
  fsub.s a0, t2, t5
  jal   t6, fifth_root
  lw    t2, H_POTENTIATION_RATE(gp)
  fmul.s t2, t2, t4
  fmul.s t2, t2, a0
  fadd.s t5, t5, t2
  flt.s t3, t5, zero
  beqz  t3, update_one_nonnegative
  li    t5, 0
update_one_nonnegative:
  lw    t2, H_WEIGHT_MAX(gp)
  flt.s t3, t2, t5
  beqz  t3, update_synapse_store
  mv    t5, t2
#endif

update_synapse_store:
  sw    t5, 0(tp)
  jr    s6

#if MNIST_RULE != MT_RULE_THREE
/* FP32 fifth-root approximation used by the GeNN-compatible weight rule. */
fifth_root:
  beqz  a0, fifth_root_zero
  slli  t1, a0, 1
  beqz  t1, fifth_root_zero
  bltz  a0, fifth_root_zero
  mv    a4, a0
  li    a7, F_ONE
  srli  t1, a4, 23
  andi  t1, t1, 255
  bnez  t1, fifth_root_scaled
  li    t1, 0x71800000
  fmul.s a4, a4, t1
  li    a7, 0x35800000

fifth_root_scaled:
  srli  t2, a4, 23
  addi  t2, t2, 3
  li    t3, 0xcccccccd
  mulhu t3, t2, t3
  srli  t3, t3, 2
  slli  a6, t3, 2
  add   a6, a6, t3
  sub   t2, t2, a6
  addi  t3, t3, 101
  slli  t3, t3, 23

  slli  t1, a4, 9
  srli  t1, t1, 9
  li    a5, F_ONE
  or    t1, t1, a5
  fadd.s t1, t1, t1
  li    a5, 0x40400000
  fsub.s t1, t1, a5

  li    a0, 0x3726f685
  fmul.s a0, a0, t1
  li    a5, 0x38160302
  fsub.s a0, a0, a5
  fmul.s a0, a0, t1
  li    a5, 0x38ece097
  fadd.s a0, a0, a5
  fmul.s a0, a0, t1
  li    a5, 0x39ea06ba
  fsub.s a0, a0, a5
  fmul.s a0, a0, t1
  li    a5, 0x3afcba26
  fadd.s a0, a0, a5
  fmul.s a0, a0, t1
  li    a5, 0x3c1df301
  fsub.s a0, a0, a5
  fmul.s a0, a0, t1
  li    a5, 0x3d941108
  fadd.s a0, a0, a5
  fmul.s a0, a0, t1
  li    a5, 0x3f8acff9
  fadd.s a0, a0, a5

  li    a5, 1
  beq   t2, a5, fifth_root_rem1
  li    a5, 2
  beq   t2, a5, fifth_root_rem2
  li    a5, 3
  beq   t2, a5, fifth_root_rem3
  li    a5, 4
  beq   t2, a5, fifth_root_rem4
  j     fifth_root_finish
fifth_root_rem1:
  li    a5, 0x3f93088c
  fmul.s a0, a0, a5
  j     fifth_root_finish
fifth_root_rem2:
  li    a5, 0x3fa8e5a3
  fmul.s a0, a0, a5
  j     fifth_root_finish
fifth_root_rem3:
  li    a5, 0x3fc20300
  fmul.s a0, a0, a5
  j     fifth_root_finish
fifth_root_rem4:
  li    a5, 0x3fdedc67
  fmul.s a0, a0, a5
fifth_root_finish:
  fmul.s a0, a0, t3
  fmul.s a0, a0, a7
  jr    t6
fifth_root_zero:
  li    a0, 0
  jr    t6
#endif
