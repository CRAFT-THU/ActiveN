/*
 * random_noc_test — symmetric random-traffic stress test
 *
 * 1. Algorithm / traffic pattern
 *    All NUM_PU processing units (PU 1 .. NUM_PU) participate symmetrically.
 *    NUM_PU must be a power of two.  Each PU runs NUM_ITERS iterations.
 *    Per iteration the PU advances its xorshift PRNG twice:
 *      prng1 -> destination = (prng1 & (NUM_PU-1)) + 1
 *      prng2 -> 32-bit payload
 *    The sender XORs the payload into its local accumulator, then sends the
 *    payload as a tag-0 AM to the destination.  The receiver's test_handler
 *    likewise XORs the received payload into its own accumulator.  Because
 *    every payload is XORed exactly twice (once by sender, once by receiver),
 *    the global XOR of all local accumulators is zero.
 *
 * 2. Program phases
 *    Phase 0 - Setup
 *      Every PU installs four handler slots (tags 0-3) and clears SPM state.
 *    Phase 1 - Barrier
 *      Every PU sends "start" (tag 2) to the leader (PU 1), then WFIs.
 *      The leader's start_handler counts arrivals; when count == NUM_PU it
 *      rebinds tag 2 -> stop_handler, starts the cycle timer, and broadcasts
 *      "work" (tag 3) to every PU including itself.
 *    Phase 2 - Work
 *      work_handler runs one send per invocation and yields to itself on
 *      tag 3 (TARGET_WORK_LOCAL).  Between yields, incoming test traffic
 *      (tag 0) is serviced by test_handler at higher priority.
 *    Phase 3 - Completion broadcast
 *      After NUM_ITERS iterations each PU rebinds tag 3 -> bcast_handler and
 *      yields.  bcast_handler sends "completion" (tag 1) to one PU per
 *      invocation, yielding between sends so higher-priority events (tags 0
 *      and 1) are serviced.
 *    Phase 4 - Drain
 *      completion_handler counts arrivals.  When a PU has NUM_PU completions
 *      it sends "stop" (tag 2) to the leader carrying its local XOR as
 *      payload.
 *    Phase 5 - Finalize (leader only)
 *      stop_handler XORs each stop payload into a separate accumulator
 *      (SPM_STOP_XOR, initialized to zero).  When all NUM_PU stops arrive
 *      the leader stops the timer and writes SPM_STOP_XOR to STOP_ADDR.
 *      Zero means every payload was delivered and XORed correctly.
 *
 * 3. New ISA send encoding
 *    SEND  dst_reg, tag_reg   — rs1=dst, rs2=tag, payload in a0-a3
 *    YIELD dst_reg, tag_reg   — same but yields after send
 *    Local send: dst=0 (rs1=zero)
 *      Tag 0  test traffic  :  a0=payload, SEND dst_reg, tag0_reg
 *      Tag 1  completion    :  a0=0, SEND dst_reg, tag1_reg
 *      Tag 2  start / stop  :  a0=data, SEND leader_reg, tag2_reg
 *      Tag 3  local work    :  a0=0, YIELD zero, tag3_reg
 *
 * 4. Persistent register assignment
 *      s0  - hartid (loaded once from CSR 0xF14)
 *      sp  - per-PU stack, top of local SPM minus 128
 *
 * 5. SPM scratch layout (top of local SPM, addresses descending)
 *      SPM_LOCAL_XOR        local XOR accumulator
 *      SPM_ITER             work iteration counter
 *      SPM_PRNG             xorshift state
 *      SPM_COMP_COUNT       completion-received counter
 *      SPM_BCAST_IDX        broadcast loop index
 *      SPM_START_COUNT      (leader) start-received counter
 *      SPM_STOP_COUNT       (leader) stop-received counter
 *      SPM_STOP_XOR         (leader) XOR of stop payloads
 */

/* --- static assertions -------------------------------------------------- */
#if (NUM_PU & (NUM_PU - 1)) != 0
#error "NUM_PU must be a power of 2"
#endif

/* --- constants ---------------------------------------------------------- */
#define STOP_ADDR            0x40000000
#define TIMER_ADDR           0x40000004
#define ASCII_ADDR           0x40000008
#define RNG_ADDR             0x40000010
#define SPM_BASE             0x20000000
#define SPM_SIZE             16384

#define SPM_LOCAL_XOR        (SPM_BASE + SPM_SIZE - 4)
#define SPM_ITER             (SPM_BASE + SPM_SIZE - 8)
#define SPM_PRNG             (SPM_BASE + SPM_SIZE - 12)
#define SPM_COMP_COUNT       (SPM_BASE + SPM_SIZE - 16)
#define SPM_BCAST_IDX        (SPM_BASE + SPM_SIZE - 20)
#define SPM_START_COUNT      (SPM_BASE + SPM_SIZE - 24)
#define SPM_STOP_COUNT       (SPM_BASE + SPM_SIZE - 28)
#define SPM_STOP_XOR         (SPM_BASE + SPM_SIZE - 32)

#define NUM_ITERS            1024
#define LEADER               1

/* --- instruction macros ------------------------------------------------- */
/* New ISA: rs1 = destination (0 = local), rs2 = tag (12-bit),
 *          payload = a0-a3 (direct register file ports x10-x13).
 * funct3[0] = yield, funct3[1] = nonblocking.
 * SEND: blocking send without yield (funct3 = 0b100 = 4)
 * YIELD: blocking send with yield   (funct3 = 0b101 = 5)
 */
  .macro SEND dst_reg, tag_reg
  .insn r 0x0B, 0x4, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .macro YIELD dst_reg, tag_reg
  .insn r 0x0B, 0x5, 0x0, x0, \dst_reg, \tag_reg
  .endm

  .macro XORSHIFT reg, tmp
  slli  \tmp, \reg, 13
  xor   \reg, \reg, \tmp
  srli  \tmp, \reg, 17
  xor   \reg, \reg, \tmp
  slli  \tmp, \reg, 5
  xor   \reg, \reg, \tmp
  .endm

/* ======================================================================== */
  .section .text
  .globl _start

_start:
  bnez  x10, .park
  li    sp, SPM_BASE + SPM_SIZE - 128
  csrr  s0, 0xF14

  /* ---- install handlers ---- */
  /* tag 0 - test traffic (highest priority) */
  la    a0, test_handler
  csrw  0x700, a0
  li    a0, 1
  csrw  0x710, a0

  /* tag 1 - completion */
  la    a0, completion_handler
  csrw  0x701, a0
  li    a0, 1
  csrw  0x711, a0

  /* tag 2 - start (leader) / noop (workers) */
  li    t0, LEADER
  bne   s0, t0, 1f
  la    a0, start_handler
  j     2f
1:
  la    a0, noop_handler
2:
  csrw  0x702, a0
  li    a0, 1
  csrw  0x712, a0

  /* tag 3 - work (lowest priority) */
  la    a0, work_handler
  csrw  0x703, a0
  li    a0, 1
  csrw  0x713, a0

  /* ---- clear SPM state ---- */
  li    t0, SPM_LOCAL_XOR
  sw    zero, 0(t0)
  li    t0, SPM_ITER
  sw    zero, 0(t0)
  li    t0, SPM_COMP_COUNT
  sw    zero, 0(t0)
  li    t0, SPM_BCAST_IDX
  sw    zero, 0(t0)
  li    t0, SPM_START_COUNT
  sw    zero, 0(t0)
  li    t0, SPM_STOP_COUNT
  sw    zero, 0(t0)
  li    t0, SPM_STOP_XOR
  sw    zero, 0(t0)

  /* ---- seed PRNG ---- */
  li    t0, RNG_ADDR
  lw    t1, 0(t0)
  li    t0, SPM_PRNG
  sw    t1, 0(t0)

  /* ---- setup barrier: leader signals readiness via global flag ---- */
  li    t0, LEADER
  bne   s0, t0, wait_leader_ready
  la    t0, setup_ready
  li    t1, 1
  sw    t1, 0(t0)
  j     send_start

wait_leader_ready:
  la    t0, setup_ready
  lw    t1, 0(t0)
  beqz  t1, wait_leader_ready

send_start:
  /* ---- send start (tag 2) to leader, then sleep ---- */
  li    a0, 0             /* payload = 0 */
  li    t0, LEADER        /* dst = leader */
  li    t1, 2             /* tag = 2 */
  SEND  t0, t1
  j     do_wfi

/* ================================================================
 * Handlers
 * ================================================================ */

/* ---- tag 0: test traffic receiver (highest priority) ---- */
/* On entry: a0 = received payload word 0 */
test_handler:
  li    t0, SPM_LOCAL_XOR
  amoxor.w zero, a0, (t0)
  j     do_wfi

/* ---- tag 1: completion receiver ---- */
completion_handler:
  li    t0, SPM_COMP_COUNT
  li    t1, 1
  amoadd.w t1, t1, (t0)
  addi  t1, t1, 1
  li    t2, NUM_PU
  bne   t1, t2, do_wfi
  /* all completions received -> send stop (tag 2) to leader with local XOR */
  li    t0, SPM_LOCAL_XOR
  lw    a0, 0(t0)         /* payload = local XOR */
  li    t0, LEADER
  li    t1, 2             /* tag = 2 */
  SEND  t0, t1
  j     do_wfi

/* ---- tag 2: start handler (leader, later rebound to stop) ---- */
start_handler:
  li    t0, SPM_START_COUNT
  li    t1, 1
  amoadd.w t1, t1, (t0)
  addi  t1, t1, 1
  li    t2, NUM_PU
  bne   t1, t2, do_wfi
  /* all PUs ready - rebind tag 2 -> stop_handler, start timer, broadcast work */
  la    a0, stop_handler
  csrw  0x702, a0
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)
  /* broadcast work (tag 3) to all PUs */
  li    s1, 1             /* loop counter (PU id) */
bcast_work:
  li    t0, NUM_PU + 1
  beq   s1, t0, do_wfi
  li    a0, 0             /* payload = 0 */
  li    t1, 3             /* tag = 3 */
  SEND  s1, t1
  addi  s1, s1, 1
  j     bcast_work

/* ---- tag 2 (rebound): stop handler (leader) ---- */
stop_handler:
  /* a0 = stop payload (sender's local XOR) */
  li    t0, SPM_STOP_XOR
  amoxor.w zero, a0, (t0)
  li    t0, SPM_STOP_COUNT
  li    t1, 1
  amoadd.w t1, t1, (t0)
  addi  t1, t1, 1
  li    t2, NUM_PU
  bne   t1, t2, do_wfi
  /* all stops received - stop timer, write result */
  li    t0, TIMER_ADDR
  sw    zero, 0(t0)
  li    t0, SPM_STOP_XOR
  lw    t1, 0(t0)
  li    t0, STOP_ADDR
  sw    t1, 0(t0)
  j     do_wfi

/* ---- tag 3: work handler ---- */
work_handler:
  li    t0, SPM_ITER
  li    t1, 1
  amoadd.w t1, t1, (t0)
  /* t1 = old iter count */
  li    t2, NUM_ITERS
  bge   t1, t2, work_done
  /* advance PRNG -> destination */
  li    t0, SPM_PRNG
  lw    t3, 0(t0)
  XORSHIFT t3, t4
  andi  t5, t3, NUM_PU - 1
  addi  t5, t5, 1         /* t5 = destination PU */
  /* advance PRNG -> payload */
  XORSHIFT t3, t4
  sw    t3, 0(t0)         /* store updated PRNG state */
  /* XOR payload into local accumulator */
  li    t0, SPM_LOCAL_XOR
  amoxor.w zero, t3, (t0)
  /* send test message: dst=t5, tag=0, payload=t3 (in a0) */
  mv    a0, t3            /* payload in a0 */
  li    t1, 0             /* tag = 0 */
  SEND  t5, t1
  /* yield locally to continue work: dst=0 (local), tag=3 */
  li    a0, 0
  li    t1, 3             /* tag = 3 */
  YIELD zero, t1
  j     do_wfi

work_done:
  /* rebind tag 3 -> bcast_handler, reset index */
  la    a0, bcast_handler
  csrw  0x703, a0
  li    t0, SPM_BCAST_IDX
  sw    zero, 0(t0)
  /* yield locally to start broadcasting: dst=0 (local), tag=3 */
  li    a0, 0
  li    t1, 3
  YIELD zero, t1
  j     do_wfi

/* ---- tag 3 (rebound): completion broadcast ---- */
bcast_handler:
  li    t0, SPM_BCAST_IDX
  li    t1, 1
  amoadd.w t1, t1, (t0)
  /* t1 = old bcast_idx; destination PU = t1 + 1 */
  addi  t2, t1, 1         /* t2 = destination PU for completion */
  li    a0, 0             /* payload = 0 */
  li    t3, 1             /* tag = 1 (completion) */
  SEND  t2, t3
  addi  t1, t1, 1
  li    t2, NUM_PU
  bge   t1, t2, do_wfi
  /* yield locally to continue broadcasting */
  li    a0, 0
  li    t1, 3
  YIELD zero, t1
  j     do_wfi

/* ---- tag 2 (workers): noop ---- */
noop_handler:
  j     do_wfi

/* ---- idle ---- */
do_wfi:
  wfi
  j     do_wfi

.park:
  wfi
  j     .park

  .section .data
  .align 2
setup_ready:
  .word 0