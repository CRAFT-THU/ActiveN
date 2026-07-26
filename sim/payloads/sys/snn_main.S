  # SNN main loop — new ISA
  #
  # Uses shared integer/FP register model:
  #   lw t0, addr → x5 = raw bits → f5 = float interpretation
  #
  # Boot sequence:
  #   1. PU1 runs snn_init() for itself, then sends token to PU2, etc.
  #   2. Each PU calls snn_init() when it receives the token, then reports ready.
  #   3. PU1 waits for all ready, then sends "go" to all PUs.
  #   4. All PUs install runtime handlers and begin the simulation.
  #
  # Runtime handler assignments (lower index = higher priority):
  #   Handler 0 (tag=0): termination control — leader collects phase reports (argcnt=1)
  #   Handler 1 (tag=1): spike — incoming synapse event (argcnt=2)
  #   Handler 2 (tag=2): termination ping — drain queued spike events, then pong (argcnt=0)
  #   Handler 3 (tag=3): work  — self-scheduled work loop (argcnt=1, margin=max(num_mc+1, 4), quota=0)
  #
  # Scatter CSR uses handler_tag=1 (spike handler).
  # Work handler reserves room for all scatter sends and the four slots needed
  # to enqueue the lowest-priority completion message.
  #
  # Register assignments (s-registers, persistent across handlers):
  #   s0  = hartid (1-based)
  #   s1  = sentinel address (SPM_BASE + neuronCount * stride)
  #   s2  = neuron stride (bytes)
  #   s3  = threshold (FP32), accessed as fs3
  #   s4  = decay factor (FP32), accessed as fs4
  #   s5  = num_mc
  #   s6  = num_pu
  #   s10 = log2(memory-line bytes)
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
#define SPM_PHASE       (SPM_END - 0x20)

#define STOP_ADDR       0x40000000
#define TIMER_ADDR      0x40000004
#define PRINT_PORT      0x4000000C
#define CONFIG_ROM_ADDR 0x68000000

#define MC_DST_BASE     0x8001
#define SCATTER_TAG     0x410  /* prio=1, scatter type=0x10 */
#define SCATTER_RESP_TAG 0x401 /* prio=1, handler=1 (spike) */
#define UPDATE_DONE_TAG 0xC00  /* prio=3, handler=0 */
#define IDLE_REGISTER_TAG 0xCF0 /* prio=3, register idle notification */
#define IDLE_RETURN_TAG 0x400  /* prio=1, handler=0 */
#define IDLE_GATE        0x100
#define IDLE_REGISTER_DATA ((IDLE_GATE << 16) | IDLE_RETURN_TAG)
#define PING_TAG        0xC02  /* prio=3, handler=2 */
#define PONG_TAG        0x400  /* prio=1, handler=0 */

#define PHASE_UPDATES   0
#define PHASE_MC_IDLE   1
#define PHASE_PONGS     2

#define SNN_CHECKSUM_SCALE 0x41200000 /* 10.0f */
#define SNN_CHECKSUM_TOL   15

#ifdef CONDUCTANCE
#define NN_ACCUM_OFFSET 0
#define NN_V_OFFSET     8
#define NN_META_OFFSET  12
#define NN_CSR_OFFSET   16
#else
#define NN_ACCUM_OFFSET 4
#define NN_V_OFFSET     0
#define NN_CSR_OFFSET   8
#endif

  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .macro YIELD dst_reg, tag_reg
  .insn r 0x0B, 0x5, 0x0, x0, \dst_reg, \tag_reg
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

  # Install boot handler: tag0 = boot_go (thread 0 only, argcnt=1).
  # hanmargin=0 (no sends). Handler armed by writing hancfg last.
  csrw  0x720, zero           # hanmargin: quota=0, margin=0

  # PU1 (leader): init counters first, then install handler
  li    t0, 1
  beq   s0, t0, leader_boot

  # Non-leader: install handler, send "ready" to PU1, wfi
  la    a0, boot_go
  csrw  0x700, a0             # address
  li    a0, (1 << 3) | 1      # thread 0 only, argcnt=1
  csrw  0x710, a0             # hancfg (enables handler)

  li    a0, 0                 # ready signal (a0=0)
  li    t0, 1                 # dst = PU1
  li    t1, 0xC00             # tag = 0, priority 3 (lowest)
  SEND  t0, t1
  j     do_wfi

leader_boot:
  # Init leader termination state
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t0, SPM_RESULT_XOR
  sw    zero, 0(t0)
  li    t0, SPM_PHASE
  sw    zero, 0(t0)

  # Install handler now that counters are zeroed
  la    a0, boot_go
  csrw  0x700, a0             # address
  li    a0, (1 << 3) | 1      # thread 0 only, argcnt=1
  csrw  0x710, a0             # hancfg (enables handler)

  # If only 1 PU, go directly
  li    t0, 1
  beq   s6, t0, boot_all_ready
  j     do_wfi

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
  # boot_go handler (tag=0, argcnt=1)
  # a0=0: "ready" from non-leader PU
  # a0=1: "go" signal (leader broadcasts to start runtime)
  #============================================================
  .globl boot_go
boot_go:
  bnez  a0, install_runtime

  # Leader: count ready messages (atomic)
  li    t0, SPM_COUNTER
  li    t1, 1
  amoadd.w t2, t1, (t0)       # old = t2, *t0 += 1
  addi  t2, t2, 1             # t2 = new value

  addi  t3, s6, -1            # need num_pu - 1 ready msgs
  bne   t2, t3, do_wfi
  # fall through

boot_all_ready:
  # Debug: leader reports all PUs ready
  li    t0, PRINT_PORT
  li    t1, 0xA110001         # "all ready" marker
  sw    t1, 0(t0)

  # Start timer
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)

  # Reset termination state for the update-completion phase
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t0, SPM_RESULT_XOR
  sw    zero, 0(t0)
  li    t0, SPM_PHASE
  li    t1, PHASE_UPDATES
  sw    t1, 0(t0)

  # Broadcast "go" to all other PUs
  li    t3, 2
boot_go_loop:
  bgt   t3, s6, boot_go_self
  li    a0, 1                 # a0=1 means "go"
  mv    t0, t3
  li    t1, 0                 # tag=0
  SEND  t0, t1
  addi  t3, t3, 1
  j     boot_go_loop

boot_go_self:
  j     install_runtime

  #============================================================
  # install_runtime: set up handler CSRs and start simulation
  #============================================================
install_runtime:
  # Handler 0: termination_control (argcnt=1, margin=0, quota=0) — highest priority
  la    a0, termination_control
  csrw  0x700, a0
  csrw  0x720, zero           # hanmargin: quota=0, margin=0
  li    a0, (1 << 3) | 1      # thread 0 only, argcnt=1
  csrw  0x710, a0             # hancfg (thread 0 only for now)

  # Handler 1: spike — second highest. Conductance mode also receives a2,
  # the presynaptic E/I metadata carried by the scatter request.
  la    a0, spike
  csrw  0x701, a0
  csrw  0x721, zero           # hanmargin: quota=0, margin=0
#ifdef CONDUCTANCE
  li    a0, (1 << 3) | 3      # thread 0 only, argcnt=3
#else
  li    a0, (1 << 3) | 2      # thread 0 only, argcnt=2
#endif
  csrw  0x711, a0             # hancfg (thread 0 only for now)

  # Handler 2: termination_ping (argcnt=0, margin=1, quota=0)
  la    a0, termination_ping
  csrw  0x702, a0
  li    a0, 1
  csrw  0x722, a0             # hanmargin: quota=0, margin=1
  li    a0, (1 << 3) | 0      # thread 0 only, argcnt=0
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

  # Trigger spm_init_finished for thread 1 (local send, tag=4).
  # Thread 1 will call load_sregisters and then add itself to handlers 0-3.
  li    t1, 4
  SEND  zero, t1              # dst=x0 (local), tag=4

  # Start: enter update phase directly (snn_init already did state+=input)
  li    a0, SPM_BASE
  j     work_update

do_wfi:
  wfi

  #============================================================
  # spm_init_finished (tag=4, argcnt=0, thread 1 only)
  # Triggered by thread 0 after install_runtime. Sets up s-registers
  # from the already-initialized SPM and adds thread 1 to all runtime
  # handler bitmasks, then enters the idle WFI loop.
  #============================================================
spm_init_finished:
  call  load_sregisters       # set s0-s6 from SPM tail + DRAM header
  li    a0, (1 << 4)          # bit 4 = thread 1
  csrs  0x710, a0             # add thread 1 to handler 0 (termination control)
  csrs  0x711, a0             # add thread 1 to handler 1 (spike)
  csrs  0x712, a0             # add thread 1 to handler 2 (termination ping)
  csrs  0x713, a0             # add thread 1 to handler 3 (work_update)
  j     do_wfi

  #============================================================
  # spike: a0=neuron_sub_idx, a1=weight. Conductance mode also receives
  # a2=presynaptic metadata through the scatter carried-data field.
  #============================================================
  .globl spike
spike:
  bnez  s2, 1f
  jal   ra, setup_regs
1:
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
  # work_update (tag=0, argcnt=1: a0=neuron_address)
  # Entered directly from install_runtime or via self-yield.
  #------------------------------------------------------------
  .globl work_update
work_update:
  bnez  s2, 1f
  jal   ra, setup_regs
1:
  beq   a0, s1, work_update_done

  # Debug: print a0 when at neuron 0 (start of each pass)
  li    t0, SPM_BASE
  bne   a0, t0, 2f
  li    t0, PRINT_PORT
  li    t1, 0xA0000000           # "work pass start" marker
  or    t1, t1, s0               # | hartid
  sw    t1, 0(t0)
2:

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
  # Debug: report this PU finished the firing loop
  li    t0, PRINT_PORT
  li    t1, 0xF1000000           # "firing done" marker
  or    t1, t1, s0               # | hartid
  sw    t1, 0(t0)

  # Report update completion to the leader at the lowest NoC priority.
  # Earlier priority-1 scatter requests leave this PU's send queue first.
  li    a0, 0
  li    t0, 1
  li    t1, UPDATE_DONE_TAG
  YIELD t0, t1
  j     do_wfi

  #------------------------------------------------------------
  # termination_ping (tag=2, argcnt=0)
  # The ping is lowest priority, below queued spike events. Compute the local
  # numerical checksum only after it schedules, then pong the leader at
  # priority 1. Performance builds compile out the neuron scan.
  #------------------------------------------------------------
termination_ping:
  bnez  s2, 1f
  jal   ra, setup_regs
1:
#if SNN_VERIFY_RESULT
  li    t0, SPM_BASE
  li    a0, 0
  li    t2, SNN_CHECKSUM_SCALE
termination_checksum_loop:
  beq   t0, s1, termination_pong

  lw    t1, 0(t0)
  fmul.s f6, f6, f7, rne
  fcvt.w.s t1, f6, rne
  xor   a0, a0, t1

  lw    t1, 4(t0)
  fmul.s f6, f6, f7, rne
  fcvt.w.s t1, f6, rne
  xor   a0, a0, t1

#ifdef CONDUCTANCE
  lw    t1, 8(t0)
  fmul.s f6, f6, f7, rne
  fcvt.w.s t1, f6, rne
  xor   a0, a0, t1
#endif

  add   t0, t0, s2
  j     termination_checksum_loop
#else
  li    a0, 0
#endif

termination_pong:
  li    t0, 1
  li    t1, PONG_TAG
  YIELD t0, t1
  j     do_wfi

  #============================================================
  # termination_control (tag=0, argcnt=1)
  # Phase 0: count PU update-completion reports.
  # Phase 1: count successful DRAM MemIf idle notifications.
  # Phase 2: accumulate PU checksums and stop the workload.
  #============================================================
  .globl termination_control
termination_control:
  bnez  s2, 1f
  jal   ra, setup_regs
1:
  li    t0, SPM_PHASE
  lw    t1, 0(t0)
  beqz  t1, termination_updates
  li    t2, PHASE_MC_IDLE
  beq   t1, t2, termination_mc_idle
  li    t2, PHASE_PONGS
  beq   t1, t2, termination_pongs
  j     termination_fail

termination_updates:
  li    t0, SPM_COUNTER
  li    t1, 1
  amoadd.w t2, t1, (t0)       # old = t2, *t0 += 1
  addi  t2, t2, 1             # t2 = new value
  bne   t2, s6, do_wfi

  # All PUs completed their neuron updates. Reset the shared counter and move
  # to the MC-idle phase before sending any registrations, so an early return
  # cannot be interpreted as another update report.
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t0, SPM_PHASE
  li    t1, PHASE_MC_IDLE
  sw    t1, 0(t0)

  # Register one accumulated-idle notifier on each DRAM MC. The peripheral
  # MemIf at destination 0x8000 is intentionally excluded.
  li    t3, 0
  li    t4, MC_DST_BASE
termination_register_loop:
  beq   t3, s5, do_wfi
  li    a0, IDLE_REGISTER_DATA
  li    t1, IDLE_REGISTER_TAG
  SEND  t4, t1
  addi  t3, t3, 1
  addi  t4, t4, 1
  j     termination_register_loop

termination_mc_idle:
  # A nonzero result is a registration rejection, not an idle notification.
  bnez  a0, termination_fail

  li    t0, SPM_COUNTER
  li    t1, 1
  amoadd.w t2, t1, (t0)
  addi  t2, t2, 1
  bne   t2, s5, do_wfi

  # All DRAM MCs have idled. Enter the pong phase before emitting pings and
  # reset the final result accumulator.
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
#if SNN_VERIFY_RESULT
  li    t0, SPM_RESULT_XOR
  sw    zero, 0(t0)
#endif
  li    t0, SPM_PHASE
  li    t1, PHASE_PONGS
  sw    t1, 0(t0)

  # Send a lowest-priority ping to every PU, including the leader itself.
  li    t3, 1
termination_ping_loop:
  bgt   t3, s6, do_wfi
  li    a0, 0
  mv    t0, t3
  li    t1, PING_TAG
  SEND  t0, t1
  addi  t3, t3, 1
  j     termination_ping_loop

termination_pongs:
#if SNN_VERIFY_RESULT
  li    t0, SPM_RESULT_XOR
  amoxor.w zero, a0, (t0)
#endif

  li    t0, SPM_COUNTER
  li    t1, 1
  amoadd.w t2, t1, (t0)
  addi  t2, t2, 1
  bne   t2, s6, do_wfi

  # All PUs drained their events and ponged. Stop the timer before reporting
  # or checking the numerical result.
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
