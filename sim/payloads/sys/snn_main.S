  # SNN main loop — ASM event handlers + C initialization
  #
  # This uses the SHARED integer/FP register model:
  #   lw t0, addr → x5 = raw bits → f5 = float interpretation
  #   fadd.s f5, f5, f6 → modifies x5 through shared register
  #   sw t0, addr → stores x5 (modified by FP operation)
  #
  # Startup: _start calls snn_init() (C) which loads SPM, configures
  #          handlers, sets s-registers, returns a0 = SPM_BASE.
  #          Then jumps straight into updateOne.
  #
  # Register assignments (s-registers, set by snn_init):
  #   s0  = MC scatter target (0xFF028001)
  #   s1  = end of neuron data (SPM_BASE + neuronCount * 16)
  #   s2  = self-fire target (0x00020000 = local send, tag=2)
  #   s3  = threshold (15.0f = 0x41700000), accessed as fs3
  #   s4  = tau decay (e^{-0.1} = 0x3f67a36d), accessed as fs4
  #   s5  = 1.0f (unused here but available)
  #   s8  = sync tree expected count
  #   s9  = hartid (1-based)
  #
  # SPM layout per neuron (16 bytes):
  #   +0:  state (f32 raw bits)
  #   +4:  input accumulator (f32 raw bits)
  #   +8:  CSR row start pointer (global DRAM address)
  #   +12: CSR row end pointer (global DRAM address)
  #
  # SPM metadata (at top of 16KB SPM):
  #   0x20003FFC: neuron count
  #   0x20003FF8: init state (0=uninit, 1=running)
  #   0x20003FF4: sync counter

#ifndef NUM_PU
#define NUM_PU 16
#endif

#define SPM_BASE        0x20000000
#define SPM_SIZE        16384
#define SPM_STATE_ADDR  (SPM_BASE + SPM_SIZE - 8)   /* 0x20003FF8 */
#define SPM_COUNTER     (SPM_BASE + SPM_SIZE - 12)   /* 0x20003FF4 */

#define STOP_ADDR       0x40000000
#define TIMER_ADDR      0x40000004

  .section .text
  .globl _start

_start:
  # Check if fully configured (state=2 means handlers + s-regs done)
  li    t0, SPM_STATE_ADDR
  lw    t1, 0(t0)
  li    t0, 2
  beq   t1, t0, do_wfi        # State=2 -> WFI (wait for events)

  # State=0: uninit (need SPM copy + handler config)
  # State=1: SPM pre-loaded by driver (skip copy, still need handler config)
  # In both cases: call snn_init which handles both
  li    sp, SPM_BASE + SPM_SIZE - 16   # Leave room for metadata
  call  snn_init               # Returns a0 = hartid

  # snn_init configured handlers and did SPM copy + init loop.
  # Now set up s-registers in ASM (C can't preserve callee-saved regs).
  mv    s9, a0                 # s9 = hartid (1-based)

  lui   s0, 0xFF028            # s0 = 0xFF028001 (MC1 scatter target)
  addi  s0, s0, 1

  li    t0, SPM_BASE + SPM_SIZE - 4
  lw    t0, 0(t0)             # neuron count
  slli  t0, t0, 4             # * 16 bytes
  li    s1, SPM_BASE
  add   s1, s1, t0            # s1 = end of neuron data

  lui   s2, 0x00020            # s2 = 0x00020000 (local send, tag=2)

  li    s3, 0x41700000         # threshold = 15.0f (shared -> fs3)
  li    s4, 0x3f67a36d         # tau = e^{-0.1} (shared -> fs4)
  li    s5, 0x3f800000         # 1.0f (shared -> fs5)

  # Sync tree: s8 = expected count (for 1-based hartids)
  addi  t3, s9, -1             # t3 = tree_id (0-based)
  li    s8, 0
  beqz  t3, tree_done          # tree_id=0 -> root
  li    s8, 1
tree_loop:
  and   t0, t3, s8
  bnez  t0, tree_done
  slli  s8, s8, 1
  j     tree_loop
tree_done:

  # Start processing: jump directly to updateOne
  li    a0, SPM_BASE
  j     updateOne

do_wfi:
  wfi

  #============================================================
  # Handler: syncinc (tag=0, 1 arg: a0=count)
  #============================================================
  .globl syncinc
syncinc:
  li    t0, SPM_COUNTER
  lw    t1, 0(t0)
  add   t1, t1, a0
  sw    t1, 0(t0)

  # Root (s8=0): check if all PUs done
  beqz  s8, check_root_done

  # Non-root: check if subtree done
  bne   t1, s8, do_wfi

  # Subtree done -> report to parent
  addi  t0, s9, -1            # tree_id (0-based)
  xor   t0, s8, t0            # parent_tree_id
  addi  t0, t0, 1             # parent_hartid (1-based)
  .insn r 0x0B, 0x5, 0x0, x0, t1, t0   # send.1 t1(x6), t0(x5)
  j     do_wfi

check_root_done:
  li    t2, NUM_PU
  bne   t1, t2, do_wfi
  # All done! Stop simulation.
  li    t0, STOP_ADDR
  sw    t1, 0(t0)             # result = NUM_PU
  j     do_wfi

  #============================================================
  # Handler: spike (tag=1, 2 args: a0=neuron_id, a1=weight)
  # Shared register: a1/fa1 = weight as float
  #============================================================
  .globl spike
spike:
  slli  a0, a0, 4             # neuron_id * 16
  li    t0, SPM_BASE
  add   a0, a0, t0
  lw    t0, 4(a0)             # x5/f5 = input
  fadd.s f5, f5, fa1          # f5 += weight (fa1 = a1)
  sw    t0, 4(a0)             # store updated input
  wfi

  #============================================================
  # Handler: updateOne (tag=2, 1 arg: a0=current SPM address)
  #============================================================
  .globl updateOne
updateOne:
  beq   a0, s1, done

  lw    t0, 0(a0)             # x5/f5 = state (as float)

  # Check: is state >= threshold? (fired)
  flt.s t1, f5, fs3           # t1 = (state < threshold)
  beqz  t1, neuron_fired      # state >= threshold -> fired

  # Not fired: decay state
  fmul.s f5, f5, fs4          # state *= tau
  sw    t0, 0(a0)             # store decayed state (x5 modified by fmul)
  addi  a0, a0, 16
  .insn r 0x0B, 0x5, 0x1, x0, a0, s2   # yield.1 a0, s2

neuron_fired:
  # Reset state to 0
  sw    zero, 0(a0)

  # Load CSR row pointers
  lw    t0, 8(a0)             # x5 = CSR start
  lw    t1, 12(a0)            # x6 = CSR end

  # If no synapses, skip readCSR
  beq   t0, t1, no_synapses

  # readCSR: queue(start, end), send to MC with scatter tag
  .insn r 0x0B, 0x2, 0x0, x0, t0, t1   # queue.2 t0(x5), t1(x6)
  .insn r 0x0B, 0x4, 0x0, x0, x0, s0   # fire.0 s0

no_synapses:
  addi  a0, a0, 16
  .insn r 0x0B, 0x5, 0x1, x0, a0, s2   # yield.1 a0, s2

  #============================================================
  # Done: all neurons processed -> sync tree
  #============================================================
done:
  li    a0, 1                  # count = 1 (this PU done)
  j     syncinc
