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
 * 3. Event target / payload encoding
 *    rs2[15:0] = destination hartid,  rs2[31:16] = tag.
 *      Tag 0  test traffic  :  SEND1 payload, dst           (tag field = 0)
 *      Tag 1  completion    :  SEND1 0, (1<<16)|dst
 *      Tag 2  start / stop  :  SEND1 data, (2<<16)|LEADER
 *      Tag 3  local work    :  YIELD1 0, 0x00030000         (local, tag 3)
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

#define NUM_ITERS            16
#define LEADER               1
#define TARGET_CTRL_LEADER   ((2 << 16) | LEADER)
#define TARGET_WORK_LOCAL    0x00030000

/* --- instruction macros ------------------------------------------------- */
  .macro SEND1 data, target
  .insn r 0x0B, 0x5, 0x0, x0, \data, \target
  .endm

  .macro YIELD1 data, target
  .insn r 0x0B, 0x5, 0x1, x0, \data, \target
  .endm

  .macro XORSHIFT reg, tmp
  slli  \tmp, \reg, 13
  xor   \reg, \reg, \tmp
  srli  \tmp, \reg, 17
  xor   \reg, \reg, \tmp
  slli  \tmp, \reg, 5
  xor   \reg, \reg, \tmp
  .endm

  /* Kept for future use (currently unused):
  .macro QUEUE2 data0, data1
  .insn r 0x0B, 0x2, 0x0, x0, \data0, \data1
  .endm

  .macro FIRE0 target
  .insn r 0x0B, 0x4, 0x0, x0, x0, \target
  .endm

  .macro ASCII_PUTC reg
  sw    \reg, 0(s2)
  .endm
  */

/* ======================================================================== */
  .section .text
  .globl _start

_start:
  li    sp, SPM_BASE + SPM_SIZE - 128
  csrr  s0, 0xF14
  /* li    s2, ASCII_ADDR */          /* uncomment for debug output */

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
  /* Leader: write 1 to setup_ready so workers know handlers are installed */
  la    t0, setup_ready
  li    t1, 1
  sw    t1, 0(t0)
  j     send_start

wait_leader_ready:
  la    t0, setup_ready
  lw    t1, 0(t0)
  beqz  t1, wait_leader_ready

send_start:
  /* ---- send start to leader, then sleep ---- */
  li    a0, 0
  li    t0, TARGET_CTRL_LEADER
  SEND1 a0, t0
  j     do_wfi

/* ================================================================
 * Handlers
 * ================================================================ */

/* ---- tag 0: test traffic receiver (highest priority) ---- */
test_handler:
  li    t0, SPM_LOCAL_XOR
  lw    t1, 0(t0)
  xor   t1, t1, a0
  sw    t1, 0(t0)
  j     do_wfi

/* ---- tag 1: completion receiver ---- */
completion_handler:
  li    t0, SPM_COMP_COUNT
  lw    t1, 0(t0)
  addi  t1, t1, 1
  sw    t1, 0(t0)
  li    t2, NUM_PU
  bne   t1, t2, do_wfi
  /* all completions received -> send stop to leader with local XOR */
  li    t0, SPM_LOCAL_XOR
  lw    a0, 0(t0)
  li    t0, TARGET_CTRL_LEADER
  SEND1 a0, t0
  j     do_wfi

/* ---- tag 2: start handler (leader, later rebound to stop) ---- */
start_handler:
  li    t0, SPM_START_COUNT
  lw    t1, 0(t0)
  addi  t1, t1, 1
  sw    t1, 0(t0)
  li    t2, NUM_PU
  bne   t1, t2, do_wfi
  /* all PUs ready - rebind tag 2, start timer, broadcast work */
  la    a0, stop_handler
  csrw  0x702, a0
  li    t0, TIMER_ADDR
  li    t1, 1
  sw    t1, 0(t0)
  li    t0, 1
bcast_work:
  li    t1, NUM_PU + 1
  beq   t0, t1, do_wfi
  li    t2, (3 << 16)
  or    t2, t2, t0
  li    a0, 0
  SEND1 a0, t2
  addi  t0, t0, 1
  j     bcast_work

/* ---- tag 2 (rebound): stop handler (leader) ---- */
stop_handler:
  li    t0, SPM_STOP_XOR
  lw    t1, 0(t0)
  xor   t1, t1, a0
  sw    t1, 0(t0)
  li    t0, SPM_STOP_COUNT
  lw    t1, 0(t0)
  addi  t1, t1, 1
  sw    t1, 0(t0)
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
  lw    t1, 0(t0)
  li    t2, NUM_ITERS
  bge   t1, t2, work_done
  /* advance PRNG -> destination */
  li    t0, SPM_PRNG
  lw    t3, 0(t0)
  XORSHIFT t3, t4
  andi  t5, t3, NUM_PU - 1
  addi  t5, t5, 1
  /* advance PRNG -> payload */
  XORSHIFT t3, t4
  sw    t3, 0(t0)
  /* XOR payload into local accumulator */
  li    t0, SPM_LOCAL_XOR
  lw    t4, 0(t0)
  xor   t4, t4, t3
  sw    t4, 0(t0)
  /* send test message (tag 0) */
  mv    a0, t3
  SEND1 a0, t5
  /* increment iteration */
  li    t0, SPM_ITER
  addi  t1, t1, 1
  sw    t1, 0(t0)
  /* yield to continue work */
  li    a0, 0
  li    t0, TARGET_WORK_LOCAL
  YIELD1 a0, t0
  j     do_wfi

work_done:
  /* rebind tag 3 -> bcast_handler, reset index */
  la    a0, bcast_handler
  csrw  0x703, a0
  li    t0, SPM_BCAST_IDX
  sw    zero, 0(t0)
  /* yield to start broadcasting */
  li    a0, 0
  li    t0, TARGET_WORK_LOCAL
  YIELD1 a0, t0
  j     do_wfi

/* ---- tag 3 (rebound): completion broadcast ---- */
bcast_handler:
  li    t0, SPM_BCAST_IDX
  lw    t1, 0(t0)
  addi  t2, t1, 1
  li    t3, (1 << 16)
  or    t2, t2, t3
  li    a0, 0
  SEND1 a0, t2
  addi  t1, t1, 1
  sw    t1, 0(t0)
  li    t2, NUM_PU
  bge   t1, t2, do_wfi
  li    a0, 0
  li    t0, TARGET_WORK_LOCAL
  YIELD1 a0, t0
  j     do_wfi

/* ---- tag 2 (workers): noop ---- */
noop_handler:
  j     do_wfi

/* ---- idle ---- */
do_wfi:
  wfi
  j     do_wfi

  .section .data
  .align 2
setup_ready:
  .word 0