  # SNN main loop — ASM event handlers + C initialization
  #
  # This uses the SHARED integer/FP register model:
  #   lw t0, addr → x5 = raw bits → f5 = float interpretation
  #   fadd.s f5, f5, f6 → modifies x5 through shared register
  #   sw t0, addr → stores x5 (modified by FP operation)
  #
  # Startup is split into two phases:
  #   1. Untimed boot barrier: PU1 serializes snn_init() across all PUs and
  #      waits until each PU reports that its SPM is initialized.
  #   2. Runtime start: PU1 broadcasts a start event, every PU installs the
  #      workload handlers, rebuilds its s-register state from SPM metadata,
  #      and begins updateOne.
  #
  # Register assignments (s-registers, set when runtime starts):
  #   s0  = first MC scatter target base (0xFF028001, MC dst=0x8001)
  #   s1  = end of neuron data (SPM_BASE + neuronCount * stride)
  #   s2  = self-fire target (0x00020000 = local send, tag=2)
  #   s3  = threshold (15.0f = 0x41700000), accessed as fs3
  #   s4  = tau decay (e^{-0.1} = 0x3f67a36d), accessed as fs4
  #   s5  = local drain target (0x00030000 = local send, tag=3)
  #   s6  = neuron stride (bytes per neuron entry)
  #   s7  = num_mc (number of memory controllers)
  #   s8  = sync tree expected count
  #   s9  = hartid (1-based)
  #   s10 = num_pu (total number of PUs)
  #   s11 = XOR quantization scale (1.0f = 0x3f800000), accessed as fs11
  #
  # SPM layout per neuron (stride = 8 + 4*num_mc bytes):
  #   +0:  state (f32 raw bits)
  #   +4:  input accumulator (f32 raw bits)
  #   +8:  CSR start for MC0 (MC-local address)
  #   +12: CSR start for MC1 (MC-local address, if num_mc >= 2)
  #   ...
  # Sentinel neuron at end has past-the-end CSR addresses.
  # End address for neuron N in MC m = neuron (N+1)'s CSR start for MC m.
  #
  # SPM metadata (at top of 16KB SPM):
  #   0x20003FFC: neuron count
  #   0x20003FF8: init state (0=need init, 1=boot-wait, 2=configured)
  #   0x20003FF4: sync counter / boot ready counter
  #   0x20003FF0: num_mc
  #   0x20003FEC: neuron stride (bytes)
  #   0x20003FE8: num_pu
  #   0x20003FE4: expected XOR checksum
  #   0x20003FE0: subtree XOR accumulator
  #   0x20003FDC: boot cookie

#ifndef NUM_PU
#define NUM_PU 16
#endif

#define SPM_BASE        0x20000000
#define SPM_SIZE        16384
#define SPM_STATE_ADDR  (SPM_BASE + SPM_SIZE - 8)   /* 0x20003FF8 */
#define SPM_COUNTER     (SPM_BASE + SPM_SIZE - 12)   /* 0x20003FF4 */
#define SPM_NUM_MC      (SPM_BASE + SPM_SIZE - 16)   /* 0x20003FF0 */
#define SPM_STRIDE      (SPM_BASE + SPM_SIZE - 20)   /* 0x20003FEC */
#define SPM_NUM_PU      (SPM_BASE + SPM_SIZE - 24)   /* 0x20003FE8 */
#define SPM_EXPECTED_XOR (SPM_BASE + SPM_SIZE - 28)  /* 0x20003FE4 */
#define SPM_XOR_ACC     (SPM_BASE + SPM_SIZE - 32)   /* 0x20003FE0 */
#define SPM_BOOT_COOKIE (SPM_BASE + SPM_SIZE - 36)   /* 0x20003FDC */
#define SNN_BOOT_COOKIE 0x534e4e31
#define BOOT_START_DRAIN 16384
#define XOR_CHECK_TOL   15
#define XOR_DRAIN_STEPS  16384

#define STOP_ADDR       0x40000000
#define TIMER_ADDR      0x40000004
#define PRINT_PORT      0x4000000C

  .section .text
  .globl _start

_start:
  bnez  x10, .park
  li    sp, SPM_BASE + SPM_SIZE - 64   # Leave room for metadata (36 bytes + stack)

  # If runtime is already configured, just wait for events.
  li    t0, SPM_BOOT_COOKIE
  lw    t1, 0(t0)
  li    t0, SNN_BOOT_COOKIE
  bne   t1, t0, boot_entry

  li    t0, SPM_STATE_ADDR
  lw    t1, 0(t0)
  li    t0, 2
  beq   t1, t0, do_wfi        # State=2 -> WFI (wait for events)

boot_entry:
  # Temporary boot handlers: tag0=boot token, tag1=ready, tag2=start.
  la    t0, boot_token
  csrw  0x700, t0
  li    t0, 1
  csrw  0x710, t0

  la    t0, boot_ready
  csrw  0x701, t0
  li    t0, 1
  csrw  0x711, t0

  la    t0, boot_go
  csrw  0x702, t0
  li    t0, 1
  csrw  0x712, t0

  # If this PU already finished snn_init(), just wait for the start event.
  li    t0, SPM_BOOT_COOKIE
  lw    t1, 0(t0)
  li    t0, SNN_BOOT_COOKIE
  bne   t1, t0, boot_wait

  li    t0, SPM_STATE_ADDR
  lw    t1, 0(t0)
  li    t0, 1
  beq   t1, t0, do_wfi

boot_wait:
  csrr  t0, mhartid
  li    t1, 1
  beq   t0, t1, boot_init
  j     do_wfi

  #============================================================
  # Untimed boot stage
  #============================================================
  .globl boot_token
boot_token:
  j     boot_init

boot_init:
  li    sp, SPM_BASE + SPM_SIZE - 64
  call  snn_init               # Returns a0 = hartid

  mv    t2, a0                 # t2 = hartid
  li    t0, 1
  beq   t2, t0, boot_leader_done

  li    t0, SPM_NUM_PU
  lw    t1, 0(t0)
  addi  t3, t2, 1              # next hartid
  blt   t1, t3, boot_report_ready
  mv    t4, t3
  li    a0, 0
  .insn r 0x0B, 0x5, 0x0, x0, a0, t4   # send.1 dummy, next(tag=0)

boot_report_ready:
  lui   t0, 0x00010
  addi  t0, t0, 1              # target = (1 << 16) | 1 (root, tag=1)
  li    a0, 1
  .insn r 0x0B, 0x5, 0x0, x0, a0, t0   # send.1 ready, root(tag=1)
  j     do_wfi

boot_leader_done:
  li    t0, SPM_COUNTER
  li    t1, 1                  # count self as ready
  sw    t1, 0(t0)

  li    t0, SPM_NUM_PU
  lw    t1, 0(t0)
  li    t2, 2
  blt   t1, t2, boot_all_ready
  mv    t4, t2
  li    a0, 0
  .insn r 0x0B, 0x5, 0x0, x0, a0, t4   # send.1 dummy, PU2(tag=0)
  j     do_wfi

  .globl boot_ready
boot_ready:
  li    t0, SPM_COUNTER
  lw    t1, 0(t0)
  add   t1, t1, a0
  sw    t1, 0(t0)

  li    t0, SPM_NUM_PU
  lw    t2, 0(t0)
  beq   t1, t2, boot_all_ready
  j     do_wfi

boot_all_ready:
  li    a0, BOOT_START_DRAIN
  li    t0, SPM_NUM_PU
  lw    t2, 0(t0)
  li    t1, 2
boot_go_loop:
  blt   t2, t1, boot_go
  lui   t0, 0x00020
  or    t0, t0, t1            # target = (2 << 16) | hartid
  li    a0, BOOT_START_DRAIN
  .insn r 0x0B, 0x5, 0x0, x0, a0, t0   # send.1 drain, target(tag=2)
  addi  t1, t1, 1
  j     boot_go_loop

  .globl boot_go
boot_go:
  bnez  a0, boot_go_wait

  # Install the runtime handlers now that every PU has completed snn_init().
  la    t0, syncinc
  csrw  0x700, t0
  li    t0, 1
  csrw  0x710, t0

  la    t0, spike
  csrw  0x701, t0
  li    t0, 2
  csrw  0x711, t0

  la    t0, updateOne
  csrw  0x702, t0
  li    t0, 1
  csrw  0x712, t0

  la    t0, xorwait
  csrw  0x703, t0
  li    t0, 1
  csrw  0x713, t0

  la    t0, xoracc
  csrw  0x704, t0
  li    t0, 1
  csrw  0x714, t0

  # Rebuild the runtime register state from SPM metadata.
  csrr  s9, mhartid

  lui   s0, 0xFF028            # s0 = 0xFF028001 (MC scatter: tag=0xFF02, dst=MC0+)
  addi  s0, s0, 1

  li    t0, SPM_STRIDE
  lw    s6, 0(t0)             # s6 = neuron stride (bytes)
  li    t0, SPM_NUM_MC
  lw    s7, 0(t0)             # s7 = num_mc
  li    t0, SPM_NUM_PU
  lw    s10, 0(t0)            # s10 = num_pu

  li    t0, SPM_BASE + SPM_SIZE - 4
  lw    t0, 0(t0)             # neuron count
  mul   t0, t0, s6            # * stride bytes
  li    s1, SPM_BASE
  add   s1, s1, t0            # s1 = end of neuron data (sentinel addr)

  lui   s2, 0x00020            # s2 = 0x00020000 (local send, tag=2)

  li    s3, 0x41700000         # threshold = 15.0f (shared -> fs3)
  li    s4, 0x3f67a36d         # tau = e^{-0.1} (shared -> fs4)
  lui   s5, 0x00030            # s5 = 0x00030000 (local send, tag=3)
  li    s11, 0x3f800000        # 1.0f (shared -> fs11)

  addi  t3, s9, -1             # t3 = tree_id (0-based)
  li    s8, 0
  beqz  t3, runtime_tree_done  # tree_id=0 -> root
  li    s8, 1
runtime_tree_loop:
  and   t0, t3, s8
  bnez  t0, runtime_tree_done
  slli  s8, s8, 1
  j     runtime_tree_loop
runtime_tree_done:

  li    t0, SPM_COUNTER
  sw    zero, 0(t0)
  li    t0, SPM_XOR_ACC
  sw    zero, 0(t0)
  li    t0, SPM_STATE_ADDR
  li    t1, 2
  sw    t1, 0(t0)

  # Debug: print (hartid << 16) | 0 — handlers configured
  slli  t0, s9, 16
  li    t1, PRINT_PORT
  sw    t0, 0(t1)

  li    t2, 1
  bne   s9, t2, wait_leader_flag

  # PU1: write sentinel flag so other PUs know PU1 is ready
  la    t0, runtime_ready_flag
  li    t1, 1
  sw    t1, 0(t0)
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)             # start timer after all handlers installed
  j     runtime_start

  # Non-PU1: spin until PU1 has written the flag
wait_leader_flag:
  la    t0, runtime_ready_flag
  lw    t1, 0(t0)
  beqz  t1, wait_leader_flag

runtime_start:
  li    a0, SPM_BASE
  j     updateOne

boot_go_wait:
  addi  a0, a0, -1
  lui   t0, 0x00020
  .insn r 0x0B, 0x5, 0x1, x0, a0, t0   # yield.1 a0, local tag=2

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
  .insn r 0x0B, 0x5, 0x0, x0, t1, t0   # send.1 t1, parent (tag=0)
  j     do_wfi

check_root_done:
  bne   t1, s10, do_wfi       # s10 = num_pu: wait until all PUs done
  # All PUs done! Stop timer and start the checksum request phase.
  li    t0, TIMER_ADDR
  sw    zero, 0(t0)           # stop timer

  li    t0, SPM_COUNTER
  sw    zero, 0(t0)           # reuse as XOR report counter
  li    t0, SPM_XOR_ACC
  sw    zero, 0(t0)

  li    a0, XOR_DRAIN_STEPS
  .insn r 0x0B, 0x5, 0x0, x0, a0, s5   # send.1 a0, local tag=3

  li    t0, 2
xor_request_loop:
  blt   s10, t0, do_wfi
  lui   t1, 0x00030
  or    t1, t1, t0            # target = (3 << 16) | hartid
  li    a0, XOR_DRAIN_STEPS
  .insn r 0x0B, 0x5, 0x0, x0, a0, t1   # send.1 a0, target(tag=3)
  addi  t0, t0, 1
  j     xor_request_loop

  #============================================================
  # Handler: spike (tag=1, 2 args: a0=neuron_id, a1=weight)
  # Shared register: a1/fa1 = weight as float
  #============================================================
  .globl spike
spike:
  mul   a0, a0, s6             # neuron_id * stride
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
  add   a0, a0, s6            # advance by stride
  .insn r 0x0B, 0x5, 0x1, x0, a0, s2   # yield.1 a0, s2

neuron_fired:
  # Reset state to 0
  sw    zero, 0(a0)

  # Scatter to all MCs: for each MC m, read start from +8+4*m, end from next neuron
  li    t3, 0                  # t3 = mc index (0-based)
  mv    t4, s0                 # t4 = scatter target (0xFF028001 + mc_idx)

scatter_mc_loop:
  beq   t3, s7, scatter_done  # if mc_idx == num_mc, done

  # CSR start for this MC: at offset 8 + 4*mc_idx from current neuron
  slli  t2, t3, 2             # t2 = mc_idx * 4
  addi  t2, t2, 8             # t2 = 8 + mc_idx * 4
  add   t2, a0, t2            # t2 = addr of csr_start[mc]
  lw    t0, 0(t2)             # t0(x5) = csr_start[mc]

  # CSR end for this MC: same offset in NEXT neuron
  add   t2, t2, s6            # next neuron's same field
  lw    t1, 0(t2)             # t1(x6) = csr_end[mc] (next neuron's start)

  # If start == end, no synapses for this MC
  beq   t0, t1, scatter_mc_next

  # readCSR: queue(start, end), fire to this MC
  .insn r 0x0B, 0x2, 0x0, x0, t0, t1   # queue.2 t0(x5), t1(x6)
  .insn r 0x0B, 0x4, 0x0, x0, x0, t4   # fire.0 t4 (scatter target for this MC)

scatter_mc_next:
  addi  t3, t3, 1             # mc_idx++
  addi  t4, t4, 1             # next MC's scatter target (+1 to dst)
  j     scatter_mc_loop

scatter_done:
  add   a0, a0, s6            # advance by stride
  .insn r 0x0B, 0x5, 0x1, x0, a0, s2   # yield.1 a0, s2

  #============================================================
  # Done: all neurons processed -> sync tree
  #============================================================
done:
  # Debug: print (hartid << 16) | 2 — neuron updates complete
  slli  t0, s9, 16
  ori   t0, t0, 2
  li    t1, PRINT_PORT
  sw    t0, 0(t1)

  li    a0, 1                  # count = 1 (this PU done)
  j     syncinc

  #============================================================
  # Handler: xorwait (tag=3, 1 arg: a0=remaining drain steps)
  # Allows late spike events to interleave before final checksum.
  #============================================================
  .globl xorwait
xorwait:
  beqz  a0, xor_begin
  addi  a0, a0, -1
  .insn r 0x0B, 0x5, 0x1, x0, a0, s5   # yield.1 a0, s5

xor_begin:
  li    t0, SPM_BASE           # current neuron address
  li    a1, 0                  # local XOR accumulator

xor_loop:
  beq   t0, s1, xor_done

  lw    t1, 0(t0)              # x6/f6 = state
  fmul.s f6, f6, fs11, rne
  fcvt.w.s t1, f6, rne
  xor   a1, a1, t1

  lw    t1, 4(t0)              # x6/f6 = input
  fmul.s f6, f6, fs11, rne
  fcvt.w.s t1, f6, rne
  xor   a1, a1, t1

  add   t0, t0, s6
  j     xor_loop

xor_done:
  lui   t0, 0x00040
  addi  t0, t0, 1             # target = (4 << 16) | 1 (root, tag=4)
  .insn r 0x0B, 0x5, 0x0, x0, a1, t0   # send.1 a1, root(tag=4)
  j     do_wfi

  #============================================================
  # Handler: xoracc (tag=4, 1 arg: a0=local_xor)
  # Root accumulates all per-PU checksums and compares once all arrive.
  #============================================================
  .globl xoracc
xoracc:
  li    t0, SPM_COUNTER
  lw    t1, 0(t0)
  addi  t1, t1, 1
  sw    t1, 0(t0)

  li    t0, SPM_XOR_ACC
  lw    t2, 0(t0)
  xor   t2, t2, a0
  sw    t2, 0(t0)

  bne   t1, s10, do_wfi

  li    t0, SPM_EXPECTED_XOR
  lw    t3, 0(t0)
  xor   t4, t2, t3
  li    t5, XOR_CHECK_TOL
  bleu  t4, t5, xor_match

xor_mismatch:
  li    t0, STOP_ADDR
  sw    t2, 0(t0)
  j     do_wfi

xor_match:
  li    t0, STOP_ADDR
  sw    s10, 0(t0)
  j     do_wfi

.park:
  wfi
  j     .park

  .section .data
  .align 2
runtime_ready_flag:
  .word 0

