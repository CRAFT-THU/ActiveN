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
  #   Handler 0 (tag=0): done  — leader collects done reports (argcnt=1)
  #   Handler 1 (tag=1): spike — incoming synapse event (argcnt=2)
  #   Handler 2 (tag=2): drain — drain phase tick (argcnt=1)
  #   Handler 3 (tag=3): work  — self-scheduled work loop (argcnt=1, margin=num_mc+1, quota=1)
  #
  # Scatter CSR uses handler_tag=1 (spike handler).
  # Work handler has margin=num_mc+1 (room for scatter sends + self-yield)
  # and quota=1 (yields after each neuron to let spikes run).
  #
  # Register assignments (s-registers, persistent across handlers):
  #   s0  = hartid (1-based)
  #   s1  = sentinel address (SPM_BASE + neuronCount * stride)
  #   s2  = neuron stride (bytes)
  #   s3  = threshold (FP32), accessed as fs3
  #   s4  = decay factor (FP32), accessed as fs4
  #   s5  = num_mc
  #   s6  = num_pu
  #
  # SPM layout per neuron (stride = 8 + 4*num_mc bytes):
  #   +0:  state (f32)
  #   +4:  input accumulator (f32)
  #   +8:  CSR start[mc0], +12: CSR start[mc1], ...
  # Sentinel neuron at end: past-the-end CSR addresses.
  #
  # SPM tail descriptor (end of 16KB SPM):
  #   end - 0x04: neuron count
  #   end - 0x08: stride bytes
  #   end - 0x0C: type (0 = FP32 LIF)
  #   end - 0x10: threshold (FP32)
  #   end - 0x14: decay factor (FP32)

#ifndef NUM_PU
#define NUM_PU 64
#endif

#define SPM_BASE        0x20000000
#define SPM_SIZE        16384
#define SPM_END         (SPM_BASE + SPM_SIZE)

#define SPM_NNCOUNT     (SPM_END - 0x04)
#define SPM_STRIDE_ADDR (SPM_END - 0x08)
#define SPM_THRESHOLD   (SPM_END - 0x10)
#define SPM_DECAY       (SPM_END - 0x14)

  /* Working memory below tail descriptor */
#define SPM_COUNTER     (SPM_END - 0x18)
#define SPM_INPUT_SUM   (SPM_END - 0x1C)
#define SPM_DRAIN_CNT   (SPM_END - 0x20)

#define STOP_ADDR       0x40000000
#define TIMER_ADDR      0x40000004
#define PRINT_PORT      0x4000000C

#define MC_DST_BASE     0x8001
#define SCATTER_TAG     0x010
#define DRAIN_LIMIT     1000

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
  # Init leader counters
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t0, SPM_INPUT_SUM
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

  # Reset counter for done phase
  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t0, SPM_INPUT_SUM
  sw    zero, 0(t0)

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
  # Handler 0: done_report (argcnt=1, margin=0, quota=0) — highest priority
  la    a0, done_report
  csrw  0x700, a0
  csrw  0x720, zero           # hanmargin: quota=0, margin=0
  li    a0, (1 << 3) | 1      # thread 0 only, argcnt=1
  csrw  0x710, a0             # hancfg (thread 0 only for now)

  # Handler 1: spike (argcnt=2, margin=0, quota=0) — second highest
  la    a0, spike
  csrw  0x701, a0
  csrw  0x721, zero           # hanmargin: quota=0, margin=0
  li    a0, (1 << 3) | 2      # thread 0 only, argcnt=2
  csrw  0x711, a0             # hancfg (thread 0 only for now)

  # Handler 2: work_drain (argcnt=1, margin=0, quota=0)
  la    a0, work_drain_tick
  csrw  0x702, a0
  csrw  0x722, zero           # hanmargin: quota=0, margin=0
  li    a0, (1 << 3) | 1      # thread 0 only, argcnt=1
  csrw  0x712, a0             # hancfg (thread 0 only for now)

  # Handler 3: work_update (argcnt=1, margin=num_mc+1, quota=0) — lowest priority
  la    a0, work_update
  csrw  0x703, a0
  addi  a0, s5, 1
  csrw  0x723, a0             # hanmargin: quota=0, margin=num_mc+1
  li    a0, (1 << 3) | 1      # thread 0 only, argcnt=1
  csrw  0x713, a0             # hancfg (thread 0 only for now)

  # Trigger spm_init_finished for thread 1 (local send, tag=4).
  # Thread 1 will call load_sregisters and then add itself to handlers 0-3.
  li    t1, 4
  SEND  zero, t1              # dst=x0 (local), tag=4

  # Reset drain counter
  li    t0, SPM_DRAIN_CNT
  sw    zero, 0(t0)

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
  csrs  0x710, a0             # add thread 1 to handler 0 (done_report)
  csrs  0x711, a0             # add thread 1 to handler 1 (spike)
  csrs  0x712, a0             # add thread 1 to handler 2 (work_drain)
  csrs  0x713, a0             # add thread 1 to handler 3 (work_update)
  j     do_wfi

  #============================================================
  # spike (tag=1, argcnt=2: a0=neuron_sub_idx, a1=weight)
  # Hardware delivers: a0=BcastBeat.idx (sub_idx), a1=BcastBeat.data (weight)
  #============================================================
  .globl spike
spike:
  bnez  s2, 1f
  jal   ra, setup_regs
1:
  mul   a0, a0, s2            # sub_idx * stride
  li    t0, SPM_BASE
  add   a0, a0, t0            # neuron address
  lw    t0, 4(a0)             # x5/f5 = input
  fadd.s f5, f5, fa1          # input += weight (fa1 = a1 via shared regs)
  sw    t0, 4(a0)             # store

  # Reset drain counter
  li    t0, SPM_DRAIN_CNT
  sw    zero, 0(t0)
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

  lw    t0, 0(a0)             # x5/f5 = state
  flt.s t1, f5, fs3           # t1 = (state < threshold)
  bnez  t1, work_not_fired

  # Fired: reset state, scatter CSR to all MCs
  sw    zero, 0(a0)

  li    t3, 0                 # mc index
  li    t4, MC_DST_BASE       # mc dst
scatter_loop:
  beq   t3, s5, scatter_done

  # csr_start: at neuron + 8 + 4*mc_idx
  slli  t2, t3, 2
  addi  t2, t2, 8
  add   t2, a0, t2
  lw    t5, 0(t2)             # csr_start (MC-local)

  # csr_end: next neuron's same field
  add   t2, t2, s2
  lw    t6, 0(t2)             # csr_end

  beq   t5, t6, scatter_next  # no synapses

  # length in beats = (end - start) / 32
  sub   t1, t6, t5
  srli  t1, t1, 5

  # Build scatter payload: a0=csr_start, a1=(beats<<16)|handler_tag(=1)
  # Save our neuron pointer
  mv    a2, a0
  mv    a0, t5                # a0 = csr_start
  slli  a1, t1, 16
  ori   a1, a1, 1             # handler_tag = 1 (spike)

  li    t1, SCATTER_TAG
  SEND  t4, t1

  mv    a0, a2                # restore neuron pointer

scatter_next:
  addi  t3, t3, 1
  addi  t4, t4, 1
  j     scatter_loop

scatter_done:
  # Advance to next neuron (after firing)
  add   a0, a0, s2
  mv    t0, s0
  li    t1, 3                 # tag = 3 (work_update)
  YIELD t0, t1
  j     do_wfi

work_not_fired:
  # Decay: state *= decay_factor
  fmul.s f5, f5, fs4
  sw    t0, 0(a0)

  # Advance
  add   a0, a0, s2
  mv    t0, s0
  li    t1, 3                 # tag = 3 (work_update)
  YIELD t0, t1
  j     do_wfi

work_update_done:
  # Enter drain phase
  li    t0, SPM_DRAIN_CNT
  sw    zero, 0(t0)
  j     work_drain_tick

  #------------------------------------------------------------
  # Drain phase: yield loop, count iterations.
  # Spike resets SPM_DRAIN_CNT. After DRAIN_LIMIT, report done.
  #------------------------------------------------------------
work_drain_tick:
  bnez  s2, 1f
  jal   ra, setup_regs
1:
  li    t0, SPM_DRAIN_CNT
  li    t1, 1
  amoadd.w t2, t1, (t0)
  addi  t2, t2, 1

  li    t3, DRAIN_LIMIT
  bge   t2, t3, work_drain_done

  # Yield back to drain
  mv    t0, s0
  li    t1, 2                 # tag = 2 (work_drain)
  YIELD t0, t1
  j     do_wfi

work_drain_done:
  # Compute sum of inputs across all neurons
  li    t0, SPM_BASE
  li    a0, 0
drain_sum_loop:
  beq   t0, s1, drain_report
  lw    t1, 4(t0)             # input field (raw FP32 bits)
  add   a0, a0, t1            # integer sum of FP32 bit patterns
  add   t0, t0, s2
  j     drain_sum_loop

drain_report:
  # Send done report to PU1 (tag=0)
  li    t0, 1
  li    t1, 0                 # tag = 0 (done_report)
  YIELD t0, t1
  j     do_wfi

  #============================================================
  # done_report (tag=3, argcnt=1: a0=input_sum)
  # Leader accumulates and checks completion.
  #============================================================
  .globl done_report
done_report:
  bnez  s2, 1f
  jal   ra, setup_regs
1:
  li    t0, SPM_INPUT_SUM
  amoadd.w t1, a0, (t0)       # *t0 += a0 (accumulate input sum)

  li    t0, SPM_COUNTER
  li    t1, 1
  amoadd.w t2, t1, (t0)       # old = t2, *t0 += 1
  addi  t2, t2, 1             # t2 = new value

  bne   t2, s6, do_wfi

  # All PUs done. Stop timer, output sum, exit.
  li    t0, TIMER_ADDR
  sw    zero, 0(t0)

  li    t0, SPM_INPUT_SUM
  lw    t1, 0(t0)
  li    t0, PRINT_PORT
  sw    t1, 0(t0)

  li    t0, STOP_ADDR
  sw    zero, 0(t0)
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
  ret

  #============================================================
  # load_sregisters: same as setup_regs (used during boot by thread 0)
  #============================================================
load_sregisters:
  j     setup_regs

