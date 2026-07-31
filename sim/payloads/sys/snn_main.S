  # SNN main loop — new ISA
  #
  # Uses shared integer/FP register model:
  #   lw t0, addr → x5 = raw bits → f5 = float interpretation
  #
  # Boot sequence:
  #   1. Every PU runs snn_init() concurrently using async global-to-SPM copy.
  #   2. Every PU installs its runtime handlers.
  #   3. Each non-leader PU reports ready after its copy and handler setup.
  #   4. PU1 waits for all ready, then passes a start token through all PUs.
  #
  # Runtime handler assignments (lower index = higher priority):
  #   Handler 0 (tag=0): update-completion tree reduction (argcnt=1)
  #   Handler 1 (tag=1): spike — incoming synapse event (argcnt=2)
  #   Handler 2 (tag=2): domain-local drain token (argcnt=2)
  #   Handler 3 (tag=3): work  — self-scheduled work loop (argcnt=1, margin=max(num_mc+1, 4), quota=0)
  #   Handler 4 (tag=4): thread-1 runtime setup (argcnt=0)
  #   Handler 5 (tag=5): boot readiness and start token (argcnt=1)
  #   Handler 6 (tag=6): idle-notifier installation chain (argcnt=1)
  #   Handler 7 (tag=7): MemIf idle return (argcnt=1)
  #   Handler 8 (tag=8): completed-domain reduction (argcnt=2)
  #   Handler 9 (tag=9): boot-readiness reduction at PU1 (argcnt=0)
  #   Handler 10 (tag=10): low-priority next-step preparation (argcnt=1)
  #
  # Scatter CSR uses handler_tag=1 (spike handler).
  # Work handler reserves room for all scatter sends and its following
  # self-schedule or completion-reduction message, with a minimum margin of 4.
  #
  # Register assignments (s-registers, persistent across handlers):
  #   s0  = hartid (1-based)
  #   s1  = sentinel address (SPM_BASE + neuronCount * stride)
  #   s2  = neuron stride (bytes)
  #   s3  = threshold (FP32), accessed as fs3
  #   s4  = decay factor (FP32), accessed as fs4
  #   s5  = num_mc
  #   s6  = num_pu
  #   s8  = next neuron awaiting next-step preparation on thread 0 (current mode)
  #   s9  = local preparation handler tag (current mode)
  #   s10 = log2(memory-line bytes)
  #   s11 = zero-based simulation step on thread 0
  #
  # Current SPM layout per neuron (stride = 8 + 4*num_mc bytes):
  #   +0:  state (f32)
  #   +4:  input accumulator (f32)
  #   +8:  CSR start[mc0], +12: CSR start[mc1], ...
  # Conductance SPM layout (stride = 16 + 4*num_mc bytes):
  #   +0:  g_e, +4: g_i, +8: membrane potential (all f32)
  #   +12: metadata (bit 0: 0=excitatory, 1=inhibitory)
  #   +16: CSR start[mc0], +20: CSR start[mc1], ...
  # Sentinel neuron at end: past-the-end CSR addresses.
  #
  # SPM tail descriptor (end of 16KB SPM):
  #   end - 0x04: neuron count
  #   end - 0x08: stride bytes
  #   end - 0x0C: type (0 = FP32 LIF)
  #   end - 0x10: threshold (FP32)
  #   end - 0x14: decay factor (FP32)
  #   end - 0x24: expected post-timestep checksum

#ifndef NUM_PU
#define NUM_PU 64
#endif

#ifndef SNN_VERIFY_RESULT
#define SNN_VERIFY_RESULT 1
#endif

#if SNN_VERIFY_RESULT != 0 && SNN_VERIFY_RESULT != 1
#error "SNN_VERIFY_RESULT must be 0 or 1"
#endif

#ifndef SNN_WARMUP_STEPS
#define SNN_WARMUP_STEPS 1
#endif

#ifndef SNN_TIMED_STEPS
#define SNN_TIMED_STEPS 4
#endif

#if SNN_WARMUP_STEPS < 0
#error "SNN_WARMUP_STEPS must be nonnegative"
#endif

#if SNN_TIMED_STEPS < 1
#error "SNN_TIMED_STEPS must be positive"
#endif

#define SNN_TOTAL_STEPS (SNN_WARMUP_STEPS + SNN_TIMED_STEPS)

#define SPM_BASE        0x20000000
#define SPM_SIZE        16384
#define SPM_END         (SPM_BASE + SPM_SIZE)

#define SPM_NNCOUNT     (SPM_END - 0x04)
#define SPM_STRIDE_ADDR (SPM_END - 0x08)
#define SPM_THRESHOLD   (SPM_END - 0x10)
#define SPM_DECAY       (SPM_END - 0x14)
#define SPM_EXPECTED_XOR (SPM_END - 0x24)

  /* Working memory below tail descriptor */
#define SPM_COUNTER     (SPM_END - 0x18)
#define SPM_RESULT_XOR  (SPM_END - 0x1C)
#define SPM_FIRED       (SPM_END - 0x20)
#define SPM_PREP_STATE  (SPM_END - 0x28)

#define PREP_DONE       0
#define PREP_QUEUED     1
#define PREP_RUNNING    2

#define STOP_ADDR       0x40000000
#define TIMER_ADDR      0x40000004
#define PRINT_PORT      0x4000000C
#define CONFIG_ROM_ADDR 0x68000000

#define MC_DST_BASE     0x8001
#define SCATTER_TAG     0x410  /* prio=1, scatter type=0x10 */
#define SCATTER_RESP_TAG 0x401 /* prio=1, handler=1 (spike) */
#define UPDATE_DONE_TAG 0xC00  /* prio=3, handler=0 */
#define IDLE_REGISTER_TAG 0xCF0 /* prio=3, register idle notification */
#define IDLE_RETURN_TAG 0x407  /* prio=1, handler=7 */
#define IDLE_GATE        0x100
#define IDLE_REGISTER_DATA ((IDLE_GATE << 16) | IDLE_RETURN_TAG)
#define DRAIN_TAG       0xC02  /* prio=3, handler=2 */
#define START_TAG       0x005  /* prio=0, handler=5 */
#define READY_TAG       0xC09  /* prio=3, handler=9 */
#define NOTIFY_TAG      0xC06  /* prio=3, handler=6 */
#define DOMAIN_DONE_TAG 0xC08  /* prio=3, handler=8 */
#define PREP_TAG        0x00A  /* local-only handler=10 */

#define SNN_CHECKSUM_SCALE 0x41200000 /* 10.0f */
#define SNN_CHECKSUM_TOL   15

#ifdef CONDUCTANCE
#define SPIKE_ARGC 3
#define NN_ACCUM_OFFSET 0
#define NN_V_OFFSET     8
#define NN_META_OFFSET  12
#define NN_CSR_OFFSET   16
#else
#define SPIKE_ARGC 2
#define NN_ACCUM_OFFSET 4
#define NN_V_OFFSET     0
#define NN_CSR_OFFSET   8
#endif

#define SPIKE_CFG_DISABLED SPIKE_ARGC
#define SPIKE_CFG_ENABLED  ((3 << 3) | SPIKE_ARGC)

  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .macro YIELD dst_reg, tag_reg
  .insn r 0x0B, 0x5, 0x0, x0, \dst_reg, \tag_reg
  .endm

  # PU and MC counts are powers of two. Compute num_pu / num_mc without the
  # unsupported RISC-V division extension.
  .macro DOMAIN_SIZE dst_reg, tmp_reg
  mv    \dst_reg, s6
  mv    \tmp_reg, s5
.Ldomain_size_loop_\@:
  srli  \tmp_reg, \tmp_reg, 1
  beqz  \tmp_reg, .Ldomain_size_done_\@
  srli  \dst_reg, \dst_reg, 1
  j     .Ldomain_size_loop_\@
.Ldomain_size_done_\@:
  .endm

  .section .text
  .globl _start

_start:
  bnez  x10, thread1_start
  csrr  s0, 0xF14             # s0 = hartid

  # Stack in global memory: sp = 0xF8000000 - 1024*(hartid-1)
  li    sp, 0xF8000000
  addi  t0, s0, -1
  slli  t0, t0, 10            # * 1024
  sub   sp, sp, t0

  # All PUs: run SPM init in parallel (like spm_init test)
  call  snn_init
  jal   ra, load_sregisters

  # Each PU owns one reduction counter in its local SPM.
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t0, SPM_RESULT_XOR
  sw    zero, 0(t0)
  li    t0, SPM_PREP_STATE
  li    t1, PREP_QUEUED       # step zero still needs synchronous preparation
  sw    t1, 0(t0)
  j     install_runtime

thread1_start:
  csrr  s0, 0xF14
  # Register spm_init_finished handler (tag 4): thread 1 only, argcnt=0.
  # This handler is triggered by thread 0 after install_runtime to let
  # thread 1 set up its s-registers before joining the schedulable pool.
  la    a0, spm_init_finished
  csrw  0x704, a0           # handler 4 address
  csrw  0x724, zero         # hanmargin: quota=0, margin=0
  li    a0, (2 << 3) | 0    # thread 1 only, argcnt=0
  csrw  0x714, a0           # hancfg (enables handler on thread 1 only)
  wfi

  #============================================================
  # install_runtime: set up all handler CSRs before reporting ready
  #============================================================
install_runtime:
  # Handler 0: update-completion reduction (argcnt=1, margin=2).
  la    a0, termination_reduce
  csrw  0x700, a0
  li    a0, 2
  csrw  0x720, a0
  li    a0, (1 << 3) | 1      # thread 0 only, argcnt=1
  csrw  0x710, a0             # hancfg (thread 0 only for now)

  # Handler 1: spike — second highest. It remains disabled until this PU has
  # prepared its state for the first simulation step.
  la    a0, spike
  csrw  0x701, a0
  csrw  0x721, zero           # hanmargin: quota=0, margin=0
  li    a0, SPIKE_CFG_DISABLED
  csrw  0x711, a0

  # Handler 2: domain-local drain token (argcnt=2, margin=1).
  la    a0, termination_drain
  csrw  0x702, a0
  li    a0, 1
  csrw  0x722, a0             # hanmargin: quota=0, margin=1
  li    a0, (1 << 3) | 2      # thread 0 only, argcnt=2
  csrw  0x712, a0             # hancfg (thread 0 only for now)

  # Handler 3: work_update (argcnt=1, margin=max(num_mc+1, 4), quota=0) — lowest priority
  la    a0, work_update
  csrw  0x703, a0
  addi  a0, s5, 1
  li    t0, 4
  bgeu  a0, t0, 1f
  mv    a0, t0
1:
  csrw  0x723, a0             # hanmargin: quota=0, margin=max(num_mc+1, 4)
  li    a0, (1 << 3) | 1      # thread 0 only, argcnt=1
  csrw  0x713, a0             # hancfg (thread 0 only for now)

  # Handler 5: boot ready/start token (argcnt=1, margin=0). Start tokens form
  # a bounded priority-0 tree: each PU receives one token and only sends to its
  # children, so blocking sends cannot form a dependency cycle.
  la    a0, boot_control
  csrw  0x705, a0
  csrw  0x725, zero
  li    a0, (1 << 3) | 1
  csrw  0x715, a0

  # Handler 6: notifier-installation chain (argcnt=1, margin=2).
  la    a0, termination_install
  csrw  0x706, a0
  li    a0, 2
  csrw  0x726, a0
  li    a0, (1 << 3) | 1
  csrw  0x716, a0

  # Handler 7: MemIf idle return (argcnt=1, margin=1).
  la    a0, termination_idle
  csrw  0x707, a0
  li    a0, 1
  csrw  0x727, a0
  li    a0, (1 << 3) | 1
  csrw  0x717, a0

  # Handler 8: completed-domain reduction at PU1 (argcnt=2, margin=1).
  la    a0, termination_domain_done
  csrw  0x708, a0
  li    a0, 1
  csrw  0x728, a0
  li    a0, (1 << 3) | 2
  csrw  0x718, a0

  # Handler 9: boot-readiness reduction at PU1 (argcnt=0, margin=1).
  la    a0, boot_ready
  csrw  0x709, a0
  li    a0, 1
  csrw  0x729, a0
  li    a0, (1 << 3) | 0
  csrw  0x719, a0

  # Handler 10: low-priority next-step preparation (argcnt=1, margin=0).
  # Its index keeps every existing runtime handler ahead of it.
  la    a0, prepare_next
  csrw  0x70A, a0
  csrw  0x72A, zero
  li    a0, (1 << 3) | 1
  csrw  0x71A, a0

  # Trigger spm_init_finished for thread 1 (local send, tag=4).
  # Thread 1 will call load_sregisters and then add itself to handlers 0-3.
  li    t1, 4
  SEND  zero, t1              # dst=x0 (local), tag=4

  # Thread 1 sends the handler-5 prewarm event after its persistent registers
  # and runtime handler masks are initialized. Receiving that event therefore
  # proves that both hardware threads are ready.
  j     do_wfi

  #============================================================
  # boot_control (tag=5, argcnt=1)
  # a0=0: prewarm the start path, then report readiness
  # a0=step+1: high-priority start token
  #============================================================
  .p2align 5
boot_control:
  beqz  a0, boot_prepared

  addi  s11, a0, -1

  # Reset this PU before forwarding the token. This prevents a child update
  # completion from reaching its parent before the parent's counter is ready.
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t0, SPM_FIRED
  sw    zero, 0(t0)

  # Build the broadcast tree over zero-based PU indices. The root starts at
  # num_pu/2; every other node starts at lowbit(index)/2. Descending bits send
  # the child with the largest remaining subtree first.
  addi  t2, s0, -1
  bnez  t2, boot_start_nonroot
  srli  t3, s6, 1
  j     boot_start_children

boot_start_nonroot:
  neg   t3, t2
  and   t3, t2, t3
  srli  t3, t3, 1

boot_start_children:
  beqz  t3, boot_prepare_state
  or    t0, t2, t3
  addi  t0, t0, 1
  li    t1, START_TAG
  SEND  t0, t1
  srli  t3, t3, 1
  j     boot_start_children

boot_prepare_state:
#ifndef CONDUCTANCE
  # Finish any preparation that did not fit under the preceding drain/start
  # interval before accepting a spike belonging to this step. A queued state
  # means the low-priority handler never started, so resume from the base.
  li    t0, SPM_PREP_STATE
  lw    t1, 0(t0)
  andi  t1, t1, 3
  li    t2, PREP_QUEUED
  beq   t1, t2, boot_prepare_from_base
  mv    t3, s8
  j     boot_prepare_loop
boot_prepare_from_base:
  li    t3, SPM_BASE
boot_prepare_loop:
  beq   t3, s1, boot_prepare_done
  lw    t0, NN_V_OFFSET(t3)
  lw    t1, NN_ACCUM_OFFSET(t3)
  fadd.s f5, f5, f6
  sw    t0, NN_V_OFFSET(t3)
  sw    zero, NN_ACCUM_OFFSET(t3)
  add   t3, t3, s2
  j     boot_prepare_loop
#endif

boot_prepare_done:
  mv    s8, s1                  # any preempted preparation event is now stale
  li    t0, SPM_PREP_STATE
  slli  t1, s11, 2
  sw    t1, 0(t0)               # current step, PREP_DONE
  li    t0, SPIKE_CFG_ENABLED
  csrw  0x711, t0
  li    a0, SPM_BASE
  j     work_update

boot_prepared:
  li    t0, 1
  beq   s0, t0, leader_prepared
  li    t0, 1
  li    t1, READY_TAG
  YIELD t0, t1
  j     do_wfi

leader_prepared:
  li    t0, 1
  beq   s6, t0, boot_all_ready
  j     do_wfi

  #============================================================
  # boot_ready (tag=9, argcnt=0)
  #============================================================
boot_ready:
  li    t0, SPM_COUNTER
  li    t1, 1
  amoadd.w t2, t1, (t0)
  addi  t2, t2, 1
  addi  t3, s6, -1
  bne   t2, t3, do_wfi

boot_all_ready:
#if SNN_WARMUP_STEPS == 0
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)
#endif

  li    a0, 1
  j     boot_control

do_wfi:
  wfi

  #============================================================
  # spm_init_finished (tag=4, argcnt=0, thread 1 only)
  # Triggered by thread 0 after install_runtime. Sets up s-registers
  # from the already-initialized SPM and adds thread 1 to all runtime
  # handler bitmasks, then enters the idle WFI loop. The spike handler remains
  # disabled until the start token prepares this PU's state.
  #============================================================
spm_init_finished:
  call  load_sregisters       # set s0-s6 from SPM tail + DRAM header
  li    a0, (1 << 4)          # bit 4 = thread 1
  csrs  0x710, a0             # add thread 1 to handler 0 (completion reduction)
  csrs  0x712, a0             # add thread 1 to handler 2 (domain drain)
  csrs  0x713, a0             # add thread 1 to handler 3 (work_update)
  li    a0, 0
  li    t1, START_TAG
  YIELD zero, t1              # prewarm/report readiness on thread 0
  j     do_wfi

  #============================================================
  # spike: a0=neuron_sub_idx, a1=weight. Conductance mode also receives
  # a2=presynaptic metadata through the scatter carried-data field.
  #============================================================
  .globl spike
spike:
  mul   a0, a0, s2            # sub_idx * stride
  li    t0, SPM_BASE
  add   a0, a0, t0            # neuron address
#ifdef CONDUCTANCE
  andi  t2, a2, 1             # source type: 0=excitatory, 1=inhibitory
  slli  t2, t2, 2             # select g_e (+0) or g_i (+4)
  add   t2, a0, t2
#else
  addi  t2, a0, NN_ACCUM_OFFSET
#endif
spike_lrsc:
  lr.w  t0, (t2)              # t0/f5 = accumulator (load reserved)
  fadd.s f5, f5, fa1          # f5 += weight (fa1 = a1 via shared regs)
  sc.w  t3, t0, (t2)          # try store; t3=0 on success
  bnez  t3, spike_lrsc        # retry if reservation lost

  wfi

  #------------------------------------------------------------
  # work_update (tag=3, argcnt=1: a0=neuron_address)
  # Entered directly from install_runtime or via self-yield.
  #------------------------------------------------------------
  .globl work_update
work_update:
  beq   a0, s1, work_update_done

#ifdef CONDUCTANCE
  # Preserve the neuron pointer while the shared integer/FP a-registers are
  # used by the midpoint update and software exponential.
  mv    s7, a0

  lw    t0, 0(s7)             # f5  = g_e
  lw    t1, 4(s7)             # f6  = g_i
  lw    t2, 8(s7)             # f7  = v
  lw    s8, 12(s7)            # source metadata

  # Midpoint conductances for dt=0.5 ms, tau_e=1 ms, tau_i=2 ms.
  li    a1, 0x3f475f7d        # exp(-dt/(2*tau_e))
  fmul.s f28, f5, fa1         # f28 = g_e midpoint
  li    a1, 0x3f61eb51        # exp(-dt/(2*tau_i))
  fmul.s f29, f6, fa1         # f29 = g_i midpoint
  fadd.s f30, f28, f29
  li    a1, 0x3f800000        # 1.0 leak conductance
  fadd.s f30, f30, fa1        # f30 = g_mid

  # Commit full-step conductance decay before the exponential helper reuses
  # the shared t/f register bank.
  li    a1, 0x3f1b4598        # exp(-dt/tau_e)
  fmul.s f5, f5, fa1
  sw    t0, 0(s7)
  li    a1, 0x3f475f7d        # exp(-dt/tau_i)
  fmul.s f6, f6, fa1
  sw    t1, 4(s7)

  # Select the one-trace model's E/I membrane constants.
  andi  s9, s8, 1
  bnez  s9, conductance_inhibitory
  li    a1, 0xbd851eb8        # excitatory v_rest = -65 mV
  li    a2, 0xbdcccccd        # excitatory E_inh  = -100 mV
  li    a3, 0xbba3d70a        # -dt/tau_m = -0.005
  j     conductance_constants_ready
conductance_inhibitory:
  li    a1, 0xbd75c28f        # inhibitory v_rest = -60 mV
  li    a2, 0xbdae147b        # inhibitory E_inh  = -85 mV
  li    a3, 0xbd4ccccd        # -dt/tau_m = -0.05
conductance_constants_ready:
  fmul.s f31, f29, fa2
  fadd.s f31, f31, fa1
  fdiv.s f31, f31, f30        # f31 = v_inf at midpoint
  fmul.s fa0, f30, fa3        # fa0 = -dt*g_mid/tau_m
  jal   ra, snn_exp_approx    # fa0 = exp(-dt*g_mid/tau_m)
  lw    t2, 8(s7)             # reload v after helper reused f7
  fsub.s fa1, f7, f31
  fmul.s fa1, fa1, fa0
  fadd.s f7, f31, fa1         # f7 = updated membrane potential
  sw    t2, 8(s7)

  bnez  s9, conductance_inhibitory_threshold
  li    a1, 0xbd54fdf4        # excitatory threshold = -52 mV
  flt.s t1, f7, fa1
  bnez  t1, work_not_fired
  li    t2, 0xbd851eb8        # excitatory reset = -65 mV
  j     conductance_fired
conductance_inhibitory_threshold:
  li    a1, 0xbd23d70a        # inhibitory threshold = -40 mV
  flt.s t1, f7, fa1
  bnez  t1, work_not_fired
  li    t2, 0xbd3851ec        # inhibitory reset = -45 mV
conductance_fired:
  sw    t2, NN_V_OFFSET(s7)
  mv    a0, s7
#else
  lw    t0, NN_V_OFFSET(a0)   # x5/f5 = state
  flt.s t1, f5, fs3           # t1 = (state < threshold)
  bnez  t1, work_not_fired

  # Fired: reset state, scatter CSR to all MCs
  sw    zero, NN_V_OFFSET(a0)
#endif

  # Count fired neurons independently of which thread executes this handler.
  li    t0, SPM_FIRED
  li    t1, 1
  amoadd.w zero, t1, (t0)

  # Preserve the source pointer and carry its E/I metadata to every response.
  mv    t0, a0
#ifdef CONDUCTANCE
  lw    a2, NN_META_OFFSET(t0)
#else
  li    a2, 0                 # current-based neurons are excitatory by ABI
#endif
  li    a3, 0
  li    t3, 0                 # mc index
  li    t4, MC_DST_BASE       # mc dst
scatter_loop:
  beq   t3, s5, scatter_done

  # csr_start: at neuron + NN_CSR_OFFSET + 4*mc_idx
  slli  t2, t3, 2
  addi  t2, t2, NN_CSR_OFFSET
  add   t2, t0, t2
  lw    t5, 0(t2)             # csr_start (MC-local)

  # csr_end: next neuron's same field
  add   t2, t2, s2
  lw    t6, 0(t2)             # csr_end

  beq   t5, t6, scatter_next  # no synapses

  # length in memory lines = (end - start) >> line-byte shift
  sub   t1, t6, t5
  srl   t1, t1, s10

  # Build scatter payload: a0=csr_start, a1=(beats<<16)|handler_tag(=1)
  mv    a0, t5                # a0 = csr_start
  slli  a1, t1, 16
  ori   a1, a1, SCATTER_RESP_TAG # prio=1, handler_tag=1 (spike)

  li    t1, SCATTER_TAG
  SEND  t4, t1

  mv    a0, t0                # restore neuron pointer

scatter_next:
  addi  t3, t3, 1
  addi  t4, t4, 1
  j     scatter_loop

scatter_done:
  # Advance to next neuron (after firing)
  add   a0, a0, s2
  li    t1, 3                 # tag = 3 (work_update)
  YIELD zero, t1
  j     do_wfi

work_not_fired:
#ifdef CONDUCTANCE
  mv    a0, s7
#else
  # Decay: state *= decay_factor
  fmul.s f5, f5, fs4
  sw    t0, NN_V_OFFSET(a0)
#endif

  # Advance
  add   a0, a0, s2
  li    t1, 3                 # tag = 3 (work_update)
  YIELD zero, t1
  j     do_wfi

work_update_done:
  # Contribute this PU to its local completion subtree. Child reports may have
  # arrived before local work completed, but the subtree cannot complete until
  # this contribution is included.
  li    a0, 1
  j     termination_reduce

  #------------------------------------------------------------
  # termination_reduce (tag=0, argcnt=1)
  # Every nonroot node reports one accumulated subtree size to its parent.
  # For zero-based index i, lowbit(i) is the node's subtree size and subtracting
  # it from the 1-based hartid gives the parent hartid.
  #------------------------------------------------------------
  .globl termination_reduce
termination_reduce:
  li    t0, SPM_COUNTER
  amoadd.w t2, a0, (t0)
  add   t2, t2, a0             # new accumulated subtree size

  li    t4, 1
  beq   s0, t4, termination_root_expected
  addi  t3, s0, -1             # zero-based PU index
  neg   t4, t3
  and   t3, t3, t4             # expected = lowbit(index)
  j     termination_reduce_check

termination_root_expected:
  mv    t3, s6                  # root covers the complete tree

termination_reduce_check:
  bltu  t2, t3, do_wfi
  bgtu  t2, t3, termination_fail

  li    t4, 1
  beq   s0, t4, termination_reduction_done

  mv    a0, t3                  # report the complete subtree size
  sub   t0, s0, t3              # parent hartid
  li    t1, UPDATE_DONE_TAG
  YIELD t0, t1
  j     do_wfi

termination_reduction_done:
  # Reuse the root counter for completed domains and initialize the checksum
  # accumulator before any notifier can return.
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t0, SPM_RESULT_XOR
  sw    zero, 0(t0)

  # PU1 is the leader of domain zero, so it installs that notifier directly.
  li    a0, IDLE_REGISTER_DATA
  li    t0, MC_DST_BASE
  li    t1, IDLE_REGISTER_TAG
  SEND  t0, t1

  # Pass installation to the next domain leader. The token carries its
  # zero-based domain index.
  li    t3, 1
  bgeu  t3, s5, do_wfi
  DOMAIN_SIZE t4, t2            # PUs per MC domain
  addi  t0, t4, 1               # first PU in domain one
  mv    a0, t3
  li    t1, NOTIFY_TAG
  SEND  t0, t1
  j     do_wfi

  #------------------------------------------------------------
  # termination_install (tag=6, argcnt=1)
  # a0 is this leader's zero-based MC/domain index.
  #------------------------------------------------------------
termination_install:
  mv    a2, a0
  li    a0, IDLE_REGISTER_DATA
  li    t0, MC_DST_BASE
  add   t0, t0, a2
  li    t1, IDLE_REGISTER_TAG
  SEND  t0, t1

  addi  a2, a2, 1
  bgeu  a2, s5, do_wfi
  DOMAIN_SIZE t2, t3
  mul   t0, a2, t2
  addi  t0, t0, 1               # first PU in the next domain
  mv    a0, a2
  li    t1, NOTIFY_TAG
  SEND  t0, t1
  j     do_wfi

  #------------------------------------------------------------
  # termination_idle (tag=7, argcnt=1)
  # Start the local drain token immediately after this domain's MC reports
  # success. The leader contributes its checksum before sending the token.
  #------------------------------------------------------------
termination_idle:
  bnez  a0, termination_fail

  # No old-step spike can remain ahead of this low-priority notification.
  # Disable the handler before passing the drain token so next-step spikes are
  # retained in its event queue until this PU has prepared the next state.
  li    t0, SPIKE_CFG_DISABLED
  csrw  0x711, t0

  li    a0, 0
  call  termination_checksum

  li    t0, SPM_FIRED
  lw    a1, 0(t0)
  li    t1, 1
  bne   s0, t1, 1f
  sw    zero, 0(t0)            # PU1 reuses this word for domain aggregation
1:

  DOMAIN_SIZE t2, t3
  li    t3, 1
  beq   t2, t3, termination_report_domain_prepare
  addi  t0, s0, 1
  li    t1, DRAIN_TAG
  SEND  t0, t1
  j     termination_schedule_prepare

  #------------------------------------------------------------
  # termination_drain (tag=2, argcnt=2)
  # The lowest-priority token visits every nonleader PU in this MC domain and
  # returns the accumulated checksum to the domain leader.
  #------------------------------------------------------------
termination_drain:
  DOMAIN_SIZE t2, t3             # domain size
  addi  t0, s0, -1
  addi  t1, t2, -1
  and   t1, t0, t1
  beqz  t1, termination_report_domain

  li    t0, SPIKE_CFG_DISABLED
  csrw  0x711, t0
  mv    a2, t2
  call  termination_checksum
  li    t0, SPM_FIRED
  lw    t1, 0(t0)
  add   a1, a1, t1

  addi  t0, s0, -1
  addi  t3, a2, -1
  or    t3, t0, t3
  addi  t3, t3, 1               # last hartid in this domain
  bne   s0, t3, termination_drain_next
  sub   t0, t3, a2
  addi  t0, t0, 1               # wrap to this domain's leader
  j     termination_drain_send

termination_drain_next:
  addi  t0, s0, 1

termination_drain_send:
  li    t1, DRAIN_TAG
  SEND  t0, t1
  j     termination_schedule_prepare

termination_report_domain:
  # Domain zero reports locally; every other domain sends one result to PU1.
  li    t0, 1
  bne   s0, t0, 1f
  li    t0, 0
1:
  li    t1, DOMAIN_DONE_TAG
  YIELD t0, t1
  j     do_wfi

termination_report_domain_prepare:
  # A single-PU domain has no returning token, so report without yielding and
  # then start its preparation just like a multi-PU domain leader.
  li    t0, 1
  bne   s0, t0, 1f
  li    t0, 0
1:
  li    t1, DOMAIN_DONE_TAG
  SEND  t0, t1
  # fall through

termination_schedule_prepare:
#ifndef CONDUCTANCE
  # The state word carries the current step above its two phase bits, so this
  # works even when the drain handler was scheduled on thread 1.
  li    t0, SPM_PREP_STATE
  lw    t2, 0(t0)
  srli  t3, t2, 2
  addi  t3, t3, 1
  li    t4, SNN_TOTAL_STEPS
  bgeu  t3, t4, do_wfi
  andi  t2, t2, -4
  ori   a0, t2, PREP_QUEUED
  sw    a0, 0(t0)
  YIELD zero, s9
#endif
  j     do_wfi

  #------------------------------------------------------------
  # prepare_next (tag=10, argcnt=1)
  # Prepare one neuron, then yield so all runtime control and start handlers
  # can run promptly. A stale generation performs no work.
  #------------------------------------------------------------
prepare_next:
#ifndef CONDUCTANCE
  li    t0, SPM_PREP_STATE
  lw    t1, 0(t0)
  bne   a0, t1, do_wfi         # preempted event from an older step

  andi  t2, a0, 3
  li    t3, PREP_QUEUED
  bne   t2, t3, prepare_next_running
  li    s8, SPM_BASE
  andi  a0, a0, -4
  ori   a0, a0, PREP_RUNNING
  sw    a0, 0(t0)

prepare_next_running:
  beq   s8, s1, do_wfi
  lw    t0, NN_V_OFFSET(s8)
  lw    t1, NN_ACCUM_OFFSET(s8)
  fadd.s f5, f5, f6
  sw    t0, NN_V_OFFSET(s8)
  sw    zero, NN_ACCUM_OFFSET(s8)
  add   s8, s8, s2
  bne   s8, s1, 1f
  li    t0, SPM_PREP_STATE
  andi  t1, a0, -4
  sw    t1, 0(t0)               # current step, PREP_DONE
  j     do_wfi
1:
  YIELD zero, s9
#endif
  j     do_wfi

  #------------------------------------------------------------
  # Fold this PU's numerical checksum into the accumulator in a0.
  # Performance builds compile out the neuron scan.
  #------------------------------------------------------------
termination_checksum:
#if SNN_VERIFY_RESULT
  mv    a3, a0
  li    t0, SPM_BASE
  li    t2, SNN_CHECKSUM_SCALE
termination_checksum_loop:
  beq   t0, s1, termination_checksum_done

  lw    t1, 0(t0)
  fmul.s f6, f6, f7, rne
  fcvt.w.s t1, f6, rne
  xor   a3, a3, t1

  lw    t1, 4(t0)
  fmul.s f6, f6, f7, rne
  fcvt.w.s t1, f6, rne
  xor   a3, a3, t1

#ifdef CONDUCTANCE
  lw    t1, 8(t0)
  fmul.s f6, f6, f7, rne
  fcvt.w.s t1, f6, rne
  xor   a3, a3, t1
#endif

  add   t0, t0, s2
  j     termination_checksum_loop

termination_checksum_done:
  mv    a0, a3
#endif
  ret

  #------------------------------------------------------------
  # termination_domain_done (tag=8, argcnt=2)
  # PU1 combines one result per MC domain and advances the simulation.
  #------------------------------------------------------------
termination_domain_done:
  li    t1, 1
  li    t0, SPM_COUNTER
  amoadd.w t2, t1, (t0)
  addi  t2, t2, 1

#if SNN_VERIFY_RESULT
  li    t0, SPM_RESULT_XOR
  amoxor.w zero, a0, (t0)
#endif
  li    t0, SPM_FIRED
  amoadd.w zero, a1, (t0)
  bne   t2, s5, do_wfi

  # Every domain has drained and reported. Emit one firing count per step.
  li    t0, SPM_FIRED
  lw    t1, 0(t0)
  li    t0, PRINT_PORT
  sw    t1, 0(t0)

  addi  t2, s11, 1
  li    t3, SNN_TOTAL_STEPS
  beq   t2, t3, termination_all_steps_done

  # The timer begins immediately before preparation of the first timed step.
  li    t3, SNN_WARMUP_STEPS
  bne   t2, t3, 1f
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)
1:
  addi  a0, t2, 1
  j     boot_control

termination_all_steps_done:
  li    t0, TIMER_ADDR
  sw    zero, 0(t0)

#if SNN_VERIFY_RESULT
  li    t0, SPM_RESULT_XOR
  lw    t1, 0(t0)
  li    t0, PRINT_PORT
  sw    t1, 0(t0)

  li    t0, SPM_EXPECTED_XOR
  lw    t3, 0(t0)
  xor   t4, t1, t3
  li    t5, SNN_CHECKSUM_TOL
  bgtu  t4, t5, termination_checksum_fail
#endif

  li    t0, STOP_ADDR
  sw    zero, 0(t0)
  j     do_wfi

#if SNN_VERIFY_RESULT
termination_checksum_fail:
  li    t0, PRINT_PORT
  sw    t3, 0(t0)
  li    t1, 0xBADCA11
  sw    t1, 0(t0)
  li    t0, STOP_ADDR
  li    t1, 1
  sw    t1, 0(t0)
  j     do_wfi
#endif

termination_fail:
  li    t0, TIMER_ADDR
  sw    zero, 0(t0)
  li    t0, PRINT_PORT
  li    t1, 0xBAD1D1E
  sw    t1, 0(t0)
  li    t0, STOP_ADDR
  li    t1, 1
  sw    t1, 0(t0)
  j     do_wfi

  #============================================================
  # setup_regs: initialize s-registers from SPM tail + DRAM header.
  # Called by handlers when thread 1 is dispatched (s2 == 0).
  # Preserves a0, a1. Uses t-regs only (plus setting s-regs).
  #============================================================
setup_regs:
  csrr  s0, 0xF14             # hartid
  li    t0, SPM_NNCOUNT
  lw    t1, 0(t0)
  li    t0, SPM_STRIDE_ADDR
  lw    s2, 0(t0)
  li    t0, SPM_THRESHOLD
  lw    s3, 0(t0)
  li    t0, SPM_DECAY
  lw    s4, 0(t0)

  mul   t2, t1, s2
  li    s1, SPM_BASE
  add   s1, s1, t2

  li    t0, 0x80000000
  lw    s5, 8(t0)             # num_mc
  lw    s6, 12(t0)            # num_pu
  li    t0, CONFIG_ROM_ADDR
  lw    s10, 24(t0)           # log2(memory-line bytes)
  li    s8, SPM_BASE
  li    s9, PREP_TAG
  ret

  #============================================================
  # load_sregisters: same as setup_regs (used during boot by thread 0)
  #============================================================
load_sregisters:
  j     setup_regs

#ifdef CONDUCTANCE
  # exp(x) for x <= 0, using x = n*ln(2) + r and a fifth-order polynomial
  # over r in [0, ln(2)). Input/output: fa0. No stack or saved registers.
snn_exp_approx:
  li    t0, 0x3fb8aa3b        # 1/ln(2)
  fmul.s f7, fa0, f5
  fcvt.w.s t1, f7, rdn        # n = floor(x/ln(2))
  fcvt.s.w f7, t1
  li    t0, 0x3f317218        # ln(2)
  fmul.s f7, f7, f5
  fsub.s f7, fa0, f7          # r = x - n*ln(2)

  li    t3, 0x3c088889        # 1/120
  fmul.s f28, f28, f7
  li    t0, 0x3d2aaaab        # 1/24
  fadd.s f28, f28, f5
  fmul.s f28, f28, f7
  li    t0, 0x3e2aaaab        # 1/6
  fadd.s f28, f28, f5
  fmul.s f28, f28, f7
  li    t0, 0x3f000000        # 1/2
  fadd.s f28, f28, f5
  fmul.s f28, f28, f7
  li    t0, 0x3f800000        # 1
  fadd.s f28, f28, f5
  fmul.s f28, f28, f7
  fadd.s f28, f28, f5

  addi  t2, t1, 127
  blez  t2, snn_exp_zero
  li    t0, 255
  bge   t2, t0, snn_exp_saturate
  slli  t2, t2, 23            # f7 = 2^n
  fmul.s fa0, f28, f7
  ret
snn_exp_zero:
  li    a0, 0
  ret
snn_exp_saturate:
  li    a0, 0x7f7fffff
  ret
#endif
