/* Hand-written Brunel runtime, RV32I/Zmmul/Zfinx.
 * gp = immutable local SPM base on both SMT threads.
 * Direct handlers yield or end at br_idle. s0-s11 retain the cooperative worker's
 * state across yields; incoming handlers and private MSHR helpers preserve
 * them. Helper-specific scratch and return-link registers are documented below.
 * SMT0 owns forward/directory/MSHR work and credited memory requests;
 * integration and spike reception use both SMT threads.
 * Brunel stages forward delivery in 2 KiB; bulk Morrison shares two 2 KiB
 * banks with its reverse descriptors. Quiet LTD is
 * committed at the postsynaptic PU with a fire-and-forget scalar store.
 * Reverse events carry full MC-local addresses. Memory effects must be ordered.
 */
#include "brunel_asm_layout.inc"
#include "brunel_rule.inc"
.option norelax
.equ BUFFER, 0x900
#if BR_MORRISON
.equ FIRED_LIST, MR_FIRED_LIST
#else
.equ FIRED_LIST, BUFFER
#endif
.equ BR_BASE, 0x20000000
.equ MSHR, 0x2900
.equ READ, 0x2d00
.equ REPLAY, 0x2d80
.equ RECORD, 0x2f00
.equ DIRTY, 0x2e00
.equ PING, 0x3700
#if BR_MORRISON
.equ POISSON, MR_POISSON
#else
.equ POISSON, 0x2da4
#endif
.equ STREAM, 0x2dd0
#ifndef BR_MSHR_LIMIT
#define BR_MSHR_LIMIT 16
#endif
#ifndef BR_BUFFER_LIMIT
#define BR_BUFFER_LIMIT 8192
#endif
#ifndef BR_DIAGNOSTIC
#define BR_DIAGNOSTIC 0
#endif
#if BR_MSHR_LIMIT < 1 || BR_MSHR_LIMIT > 16
#error Invalid MSHR test limit
#endif
#if BR_BUFFER_LIMIT < 0 || BR_BUFFER_LIMIT > 8192 || (BR_BUFFER_LIMIT & 31)
#error Invalid forward buffer test limit
#endif
.macro ENTER n
 addi sp,sp,-(((\n+1)*4+15)&-16)
 sw ra,0(sp)
 .if \n>0
 sw s0,4(sp)
 .endif
 .if \n>1
 sw s1,8(sp)
 .endif
 .if \n>2
 sw s2,12(sp)
 .endif
 .if \n>3
 sw s3,16(sp)
 .endif
 .if \n>4
 sw s4,20(sp)
 .endif
 .if \n>5
 sw s5,24(sp)
 .endif
 .if \n>6
 sw s6,28(sp)
 .endif
 .if \n>7
 sw s7,32(sp)
 .endif
 .if \n>8
 sw s8,36(sp)
 .endif
 .if \n>9
 sw s9,40(sp)
 .endif
 .if \n>10
 sw s10,44(sp)
 .endif
 .if \n>11
 sw s11,48(sp)
 .endif
.endm
.macro LEAVE n
 lw ra,0(sp)
 .if \n>0
 lw s0,4(sp)
 .endif
 .if \n>1
 lw s1,8(sp)
 .endif
 .if \n>2
 lw s2,12(sp)
 .endif
 .if \n>3
 lw s3,16(sp)
 .endif
 .if \n>4
 lw s4,20(sp)
 .endif
 .if \n>5
 lw s5,24(sp)
 .endif
 .if \n>6
 lw s6,28(sp)
 .endif
 .if \n>7
 lw s7,32(sp)
 .endif
 .if \n>8
 lw s8,36(sp)
 .endif
 .if \n>9
 lw s9,40(sp)
 .endif
 .if \n>10
 lw s10,44(sp)
 .endif
 .if \n>11
 lw s11,48(sp)
 .endif
 addi sp,sp,(((\n+1)*4+15)&-16)
 ret
.endm
.macro SEND dst,tag
 .insn r 0x0b,4,0,x0,\dst,\tag
.endm
.macro YIELD dst,tag
 .insn r 0x0b,5,0,x0,\dst,\tag
.endm
.macro SENDI dst,tag
 .insn i 0x2b,4,x0,\dst,\tag
.endm
.macro YIELDI tag
 .insn i 0x2b,5,x0,zero,\tag
.endm
.macro YIELD_TO dst,tag
 .insn i 0x2b,5,x0,\dst,\tag
.endm
.macro INC offset,tmp
 lw \tmp,\offset(gp)
 addi \tmp,\tmp,1
 sw \tmp,\offset(gp)
.endm
.macro DINC offset,tmp
#if BR_DIAGNOSTIC
 INC \offset,\tmp
#endif
.endm
/* Inlined arithmetic shares the exact FP order with the numeric self-test.
 * Reverse work reserves s3/s4/s10 for seed/divisor/Newton constants (Morrison)
 * or s3 for WMAX (Additive). Forward completion establishes them before it
 * reports, so reverse messages may safely arrive before local phase control.
 * POW04 changes a0,t0-t5 only and preserves all constants and caller state. */
.macro POW04 cached=0
 beqz a0,.Lpow_done\@
 li t5,0
 li t0,0x02000000
 bgeu a0,t0,.Lpow_normal\@
 li t0,0x71800000
 fmul.s a0,a0,t0
 li t5,0x14000000
.Lpow_normal\@:
.if \cached
 mulhu t1,a0,s4
 srli t1,t1,2
 sub t1,s3,t1
.else
 li t0,0x4c2bac89
 li t2,0xcccccccd
 mulhu t1,a0,t2
 srli t1,t1,2
 sub t1,t0,t1
.endif
 li t0,0x3e4ccccd
 fmul.s t2,a0,t0
 fmul.s t3,t1,t1
 fmul.s t4,t3,t3
 fmul.s t2,t2,t3
 fmul.s t2,t2,t4
.if \cached
 fmul.s t1,t1,s10
.else
 li t0,0x3f99ca19
 fmul.s t1,t1,t0
.endif
 fsub.s t1,t1,t2
 fmul.s t3,t1,t1
 fmul.s a0,a0,t3
 fmul.s a0,a0,t1
 fmul.s t1,a0,t3
 li t0,0x3f800000
 fsub.s t1,t0,t1
 li t0,0x3ef5c28f
 fmul.s t2,t1,t0
 li t0,0x3f199ae9
 fadd.s t2,t2,t0
 fmul.s t1,a0,t1
 fmul.s t1,t1,t2
 fadd.s a0,a0,t1
 sub a0,a0,t5
.Lpow_done\@:
.endm
.macro LTD dst,weight,coefficient,morrison
.if \morrison
 fmul.s \coefficient,\coefficient,\weight
.endif
 fsub.s \dst,\weight,\coefficient
 fmax.s \dst,\dst,zero
.endm
/* Morrison preserves its input weight in a3; output may alias a3.
 * Additive also uses a3 in production, and keeps its upper clamp. */
.macro LTP dst,weight,coefficient,morrison,cached=0
.if \morrison
 mv a0,\weight
 POW04 \cached
 fmul.s a0,a0,\coefficient
 fadd.s \dst,a0,\weight
.else
 fadd.s \dst,\weight,\coefficient
.if \cached
 fmin.s \dst,\dst,s3
.else
 lw t0,H_WMAX(gp)
 fmin.s \dst,\dst,t0
.endif
.endif
.endm
/* a0=DRAM address, a3=FP32 data; retain acknowledgements and credit bound. */
.macro STORE_WORD
.Lstore_credit_wait\@:
#if BR_BULK
 li t0,BR_BASE+MB_READY_CONTROL+R_CREDIT
 lw t1,0(t0)
 sltiu t1,t1,MB_READ_CREDITS
 beqz t1,.Lstore_credit_wait\@
 li t1,1
 amoadd.w zero,t1,(t0)
#else
 lw t0,H_ISSUED(gp)
 lw t1,H_CONSUMED(gp)
 sub t1,t0,t1
 sltiu t1,t1,12
 beqz t1,.Lstore_credit_wait\@
 addi t0,t0,1
 sw t0,H_ISSUED(gp)
#endif
 li a1,0x28003
 li a2,0x8000
 lw t0,H_MC_DEST(gp)
 SENDI t0,1025
.endm
.text
/* Completion handler is a leaf so it can retire notifications quickly. */
.globl br_completion
br_completion:
#if BR_BULK
#if BR_BULK_WORKER
 lw t0,H_PHASE(gp)
 li t1,2
 bne t0,t1,.Lworker_manual_completion
 # Directory scatter carries an SPM byte offset and a relocated row base.
 # Unlike ID2 DMA, this path is independent of the live bulk AC base CSR.
 add t0,gp,a0
 sw a1,0(t0)
 li t0,BR_BASE+MB_CONTROL+B_HOME_DIRECTORY_RECEIVED
 lw t1,0(t0)
 addi t1,t1,1
 sw t1,0(t0)
 li t2,4
 bne t1,t2,br_idle
 csrsi 0x71d,8
 j br_idle
.Lworker_manual_completion:
#endif
 # Ordinary boot/directory completion; reverse installs the cached producer.
 slli t0,a0,16
 bltz t0,1f
 slli t0,a1,2
 add t0,gp,t0
 li t1,1
 sw t1,H_READY0(t0)
1: li t0,BR_BASE+MB_READY_CONTROL+R_CREDIT
 li t1,-1
 amoadd.w zero,t1,(t0)
 j br_idle
#else
 slli t0,a0,16
 bltz t0,1f
 li t0,3
 bgtu a1,t0,.Lcompletion_mshr
 slli t0,a1,2
 add t0,gp,t0
 li t1,1
 sw t1,80(t0)
1: INC H_CONSUMED,t0
 j br_idle
.Lcompletion_mshr:
 li t0,MSHR
 sub t0,a1,t0
 sltiu t0,t0,(READ-MSHR)
 beqz t0,.Lbad_completion
 andi t0,a1,63
 bnez t0,.Lbad_completion
 add t0,gp,a1
 li t1,2
 sw t1,44(t0)
 INC H_CONSUMED,t0
 j br_idle
.Lbad_completion:
 li a0,0xb002
 j br_halt
#endif
/* These leaf transports do not impose address-specific completion waits. */
br_load_start:
 addi a2,a1,(READ/32)
/* Directory/boot entry: a1 is always a token in 0..3. MSHRs enter below
 * the ready-flag reset. a0=DRAM address, a2=SPM line index. */
br_line_start:
#if BR_HOME_UPDATES
 /* Directory/boot reads are local; the MSHR entry below receives a7 explicitly. */
 lw a7,H_MC_DEST(gp)
#endif
 slli t0,a1,2
 add t0,gp,t0
 sw zero,80(t0)
br_mshr_line_start:
.Lload_credit_wait:
#if BR_BULK
 li t2,BR_BASE+MB_READY_CONTROL
 lw t1,R_CREDIT(t2)
 sltiu t1,t1,MB_READ_CREDITS
 beqz t1,.Lload_credit_wait
 addi t2,t2,R_CREDIT
 li t1,1
 amoadd.w zero,t1,(t2)
#else
 lw t0,H_ISSUED(gp)
 lw t1,H_CONSUMED(gp)
 sub t1,t0,t1
 sltiu t1,t1,12
 beqz t1,.Lload_credit_wait
 addi t0,t0,1
 sw t0,H_ISSUED(gp)
#endif
 mv a3,a1
 li a1,0x58002
#if BR_HOME_UPDATES
 mv t0,a7
#else
 lw t0,H_MC_DEST(gp)
#endif
#if BR_RELAY_UPDATES
 /* Priority 3 scatter -> priority 2 relay -> priority 1 scalar read.
  * Completion uses the dedicated, bounded queue on SMT1 as before. */
 SENDI t0,1024
#else
#if BR_BULK_RELAY
 # Only descriptor reads need P1. Directory/boot/route-marker tokens 0..3
 # retain P2, and every route still uses the caller's explicit a7 MC.
 li t1,3
 bleu a3,t1,1f
 SENDI t0,1024
 ret
1:
#endif
 SENDI t0,-2048
#endif
 ret
br_load_wait:
 slli t0,a0,2
 add t0,gp,t0
1: lw t1,80(t0)
 beqz t1,1b
 ret
/* Arm LoadManual AC: fixed 1 KiB for Brunel, compact final block for Morrison.
 * a0=MC-local address, a1=SPM offset, a2=remaining edges for Morrison. */
br_ac_start:
 add t0,gp,a1
 csrw 0x732,t0
#if BR_MORRISON
 /* a2 is the number of remaining edges. The final two arrays are padded
  * separately to 32 bytes; request length must equal acLen. */
 li t0,128
 bleu a2,t0,1f
 mv a2,t0
1: addi a2,a2,7
 srli a2,a2,3
 slli a2,a2,1              # two lines for each group of eight edges
 slli t0,a2,5
 csrw 0x733,t0
 slli a1,a2,16
 li t0,0x8001
 or a1,a1,t0
 li t0,1
 csrw 0x730,t0
#else
 li t0,1
 csrw 0x730,t0
 li a1,0x208001             # 32 lines, manual AC response ID 0x8001
#endif
 lw t0,H_MC_DEST(gp)
 SENDI t0,-2031
 ret
br_weight_load:
 DINC H_WEIGHT_LOADS,t0
 andi a0,a0,-32
 j br_load_start
br_store_word:
 mv a3,a1
 STORE_WORD
 ret
br_drain:
#if BR_BULK
 li t0,BR_BASE+MB_READY_CONTROL+R_CREDIT
1: lw t1,0(t0)
 bnez t1,1b
#else
1: lw t0,H_ISSUED(gp)
 lw t1,H_CONSUMED(gp)
 bne t0,t1,1b
#endif
 ret
br_continue:
 YIELDI BR_WORK_ID
 j br_idle
.globl br_halt
br_halt:
 li t0,0x40000000
 sw a0,0(t0)
1: wfi
 j 1b
/* Test-only generic entry exercises both rules; production calls the
 * compile-time-selected leaves directly. Weights and coefficients are >= 0. */
#ifdef BR_ASM_SELFTEST
.globl br_update
br_update:
 lw t0,H_RULE(gp)
 bnez t0,1f
 beqz a2,br_ltd_additive
 j br_ltp_additive
1: beqz a2,br_ltd_morrison
 j br_ltp_morrison
br_ltd_additive:
 LTD a0,a0,a1,0
 ret
br_ltp_additive:
 LTP a0,a0,a1,0
 ret
br_ltd_morrison:
 LTD a0,a0,a1,1
 ret
br_ltp_morrison:
 mv a3,a0
 LTP a0,a3,a1,1
 ret
#endif
/* One coalesced service event; only SMT0 owns ring indices and allocation.
 * SMT1 only changes an allocated descriptor from loading(1) to ready(2).
 * A full-pool allocator can retire the head itself, so it never waits for
 * its own lower-priority service event. Only fired-post scatter allocates
 * these descriptors; post-quiet updates are written at the receiving PU.
 * Private ABI: a4-a6 hold the head, descriptor and address. There are no calls;
 * ra and t6 (the allocator's return link) survive all arithmetic and stores.
 * Returns the remaining descriptor count in t0 on every path.
 */
#if BR_BULK
#include "morrison_ready_runtime.inc"
#else
br_mshr_retire:
 lw t0,H_MSHR_COUNT(gp)
 beqz t0,9f
 lw a4,H_MSHR_HEAD(gp)
 slli a5,a4,6
 li t1,BR_BASE+MSHR
 add a5,a5,t1
 lw t1,44(a5)
 li t2,2
 bne t1,t2,9f             # t0 still holds the count on an unsuccessful poll
 lw a6,32(a5)
 andi t0,a6,28
 add t0,t0,a5
 lw a3,0(t0)
 lw a1,36(a5)
 beqz a1,1f
 LTD a3,a3,a1,BR_RULE
1: lw a1,40(a5)
 beqz a1,2f
 LTP a3,a3,a1,BR_RULE,1
2:
 mv a0,a6
#if BR_BULK
 /* The timestep marker/idle barrier closes posted stores before reuse. */
 li a1,0x20000
 lw t0,52(a5)
 SENDI t0,2
#elif BR_HOME_UPDATES
 /* The descriptor remembers the proxy's MC. No receipt or write credit. */
 li a1,0x20000
 lw t0,52(a5)
 SENDI t0,1026
#elif BR_RELAY_UPDATES
 /* Updates execute at the weight owner, with a local no-receipt write.
  * Priority 0 leaves stores independent of the request/relay queues. */
 li a1,0x20000
 lw t0,H_MC_DEST(gp)
 SENDI t0,2
#else
 STORE_WORD
#endif
 DINC H_WRITE_WORDS,t0
 DINC H_MSHR_COMMITS,t0
 sw zero,44(a5)
 addi a4,a4,1
 andi a4,a4,15
 sw a4,H_MSHR_HEAD(gp)
 lw t0,H_MSHR_COUNT(gp)
 addi t0,t0,-1
 sw t0,H_MSHR_COUNT(gp)
9: ret
.globl br_mshr_service
br_mshr_service:
 jal ra,br_mshr_retire
 beqz t0,1f
 /* Enqueue before becoming idle: an immediate yield can schedule an already
  * pending drain worker before this service event is visible to the scheduler.
  * Drain relies on service priority to finish and clear its scheduled flag. */
 SENDI zero,BR_SERVICE_ID
 j br_idle
1: sw zero,H_MSHR_SCHEDULED(gp)
 j br_idle
/* a4=address, a5=LTD coefficient, a6=LTP coefficient; home updates use a7=MC.
 * Retirement preserves a7 as well as t6, including the full-pool fallback. */
br_mshr_allocate:
 /* Called with jal t6 by br_reverse. Normal allocation keeps the update in
  * a4-a6 across br_line_start, which also preserves t6. Only the full-pool
  * path spills those inputs around retirement; s0-s11 remain untouched. */
 lw t3,H_MSHR_COUNT(gp)
 li t1,BR_MSHR_LIMIT
 bltu t3,t1,2f
 addi sp,sp,-16
 sw a4,0(sp)
 sw a5,4(sp)
 sw a6,8(sp)
 DINC H_POOL_FULL,t0
#if BR_DIAGNOSTIC
 lw t0,H_MSHR_HEAD(gp)
 slli t0,t0,6
 li t1,BR_BASE+MSHR
 add t0,t0,t1
 lw t0,44(t0)
 li t1,2
 beq t0,t1,1f
 DINC H_MSHR_WAITS,t0
#endif
1: jal ra,br_mshr_retire
 li t1,BR_MSHR_LIMIT
 bgeu t0,t1,1b
 mv t3,t0
 lw a4,0(sp)
 lw a5,4(sp)
 lw a6,8(sp)
 addi sp,sp,16
2: lw t0,H_MSHR_TAIL(gp)
 slli t2,t0,6
 li t1,BR_BASE+MSHR
 add t2,t2,t1
 addi t0,t0,1
 andi t0,t0,15
 sw t0,H_MSHR_TAIL(gp)
 sw a4,32(t2)
 sw a5,36(t2)
 sw a6,40(t2)
#if BR_HOME_UPDATES
 sw a7,52(t2)
#endif
 li t0,1
 sw t0,44(t2)
 DINC H_MSHR_LOADS,t0
#if BR_DIAGNOSTIC
 sw t0,48(t2)
#endif
 addi t3,t3,1
 sw t3,H_MSHR_COUNT(gp)
#if BR_DIAGNOSTIC
 lw t1,H_MSHR_PEAK(gp)
 bgeu t1,t3,3f
 sw t3,H_MSHR_PEAK(gp)
#endif
3: andi a0,a4,-32
 sub a1,t2,gp
 srli a2,a1,5
 DINC H_WEIGHT_LOADS,t0
 jal ra,br_mshr_line_start
 lw t0,H_MSHR_SCHEDULED(gp)
 bnez t0,4f
 li t0,1
 sw t0,H_MSHR_SCHEDULED(gp)
 SENDI zero,BR_SERVICE_ID
4: jr t6
#endif
.globl br_reverse
br_reverse:
#if BR_HOME_UPDATES || BR_RELAY_UPDATES
 /* Aligned neuron offsets pack the weight MC (home) or proxy rank (relay)
  * in their spare low nibble, retaining the full MC-local weight address. */
 andi t0,a0,-16
 add t0,gp,t0
#else
 add t0,gp,a0              # reverse CSR carries the neuron SPM byte offset
#endif
#if BR_PROXY && BR_PROXY_TRACES
 lw t1,P_FIRE(t0)
 lw t2,P_X(t0)
#else
 STATUS_LOAD t1,N_FIRE(t0)
 lw t2,N_X(t0)
#endif
 /* Nonnegative X already is the LTP coefficient. Quiet pre is fallthrough. */
 bnez t1,.Lreverse_active
 beqz t2,8f
 li a2,0
.Lreverse_allocate:
#if BR_BULK
#if BR_BULK_RELAY
 # Any group member in the weight MC can update this address locally.
 # Keep address/LTD in a1/a2; a0 is unused by the proxy accept handler.
 mv a3,t2
 andi t0,a0,15
 slli t0,t0,5
 lw t1,H_PU(gp)
 addi t1,t1,-1
 andi t1,t1,31
 add t0,t0,t1
 addi t0,t0,1
 SENDI t0,(-2048+MB_ACCEPT_ID) # P2 reception -> P1 read -> P0 store
 j br_idle
#else
 j mr_accept
#endif
#else
#if BR_RELAY_UPDATES
 /* One targeted packet per useful update; the address already identifies
  * the synapse, so no post ID or CSR search is needed. a0=weight address,
  * a1=conditional LTD coefficient, a2=LTP coefficient, a3 unused.
  * Do not count receipt here: the proxy must accept the update before the
  * global reverse barrier may complete and home traces may advance. */
 andi t0,a0,15
 slli t0,t0,2
 li t1,BR_BASE+PROXY_CONFIG
 add t0,t0,t1
 lw t0,0(t0)
 mv a0,a1
 mv a1,a2
 mv a2,t2
 SENDI t0,-2044          # 0x804, priority 2, proxy update handler 4
 j br_idle
#else
#if BR_HOME_UPDATES
 andi a7,a0,15
 li t0,0x8001
 add a7,a7,t0
#endif
 /* The forward buffer is gone.  Every post-active update uses an MSHR
  * line load, then the normal ordered scalar writeback path. */
 mv a4,a1
 mv a5,a2
 mv a6,t2
 jal t6,br_mshr_allocate
#endif
#endif
8:
.Lreverse_received:
#if BR_RELAY_UPDATES || BR_BULK_RELAY
 /* Home no-ops and accepted proxy updates may count on different SMTs. */
 addi t0,gp,H_REVERSE_RECEIVED
 li t1,1
 amoadd.w zero,t1,(t0)
#if BR_DIAGNOSTIC
 addi t0,gp,H_REVERSE_EVENTS
 amoadd.w zero,t1,(t0)
#endif
#else
 INC H_REVERSE_RECEIVED,t0
 DINC H_REVERSE_EVENTS,t0
#endif
 j br_idle
.Lreverse_active:
 or t0,a2,t2
 beqz t0,8b
 j .Lreverse_allocate
#if BR_RELAY_UPDATES
.globl br_proxy_update
br_proxy_update:
 /* Same SMT0 ownership, allocator and arithmetic as ordinary proxy updates.
  * Persistent learning traces are absent; this event holds their snapshot. */
 mv a4,a0
 mv a5,a1
 mv a6,a2
 jal t6,br_mshr_allocate
 j .Lreverse_received
#endif
/* Positive finite FP32 w^(2/5), plus exact +0. Scratch a0,t0-t5 only.
 * Bit seed for the inverse fifth root, one modified Newton step, then a
 * quadratic correction of q=w*r^3 using d=1-q*r^2. Every FP op rounds
 * separately. Exhaustive positive-FP32 validation bounds the error at 2 ULP.
 * Scale tiny inputs by 2^100 and reduce the result exponent by 40. */
#ifdef BR_ASM_SELFTEST
.globl br_pow04
br_pow04:
 POW04
 ret
#endif
/* Both loops share the same neuron equations. Only E neurons carry learning
 * traces; the I loop emits no trace loads, FP operations, or stores. */
.macro INTEGRATE_NEURON traces
 lw t0,N_D_TOTAL(t5)
 lw t1,N_I_TOTAL(t5)
 lw t4,N_V(t5)
 STATUS_LOAD t6,N_REFRAC(t5)
 beqz t6,.Lneuron_voltage\@
 addi t6,t6,-1
 STATUS_STORE t6,N_REFRAC(t5)
 j .Lneuron_currents\@
.Lneuron_voltage\@:
 fmul.s a0,s4,t0
 fmul.s a1,s5,t1
 fadd.s a0,a0,a1
 fmul.s t4,s6,t4
 fadd.s t4,t4,a0
.Lneuron_currents\@:
 fmul.s a0,s3,t0
 fmul.s t1,s2,t1
 fadd.s t1,t1,a0
 sw t1,N_I_TOTAL(t5)
 lw a0,N_INPUT_E(t5)
 lw a1,N_INPUT_I(t5)
 fmul.s t0,s2,t0
 or t1,a0,a1
 beqz t1,.Lneuron_no_recurrent\@
 fadd.s a0,a0,a1
 fadd.s t0,t0,a0
 sw zero,N_INPUT_E(t5)
 sw zero,N_INPUT_I(t5)
.Lneuron_no_recurrent\@:
 slli t1,s11,13
 xor s11,s11,t1
 srli t1,s11,17
 xor s11,s11,t1
 slli t1,s11,5
 xor s11,s11,t1
#if BR_MORRISON
 bleu s11,a4,.Lneuron_no_external\@
#else
 bltu s11,a4,.Lneuron_no_external\@
#endif
 mv a0,s7
#if BR_MORRISON
 bleu s11,a5,.Lneuron_add_external\@
#else
 bltu s11,a5,.Lneuron_add_external\@
#endif
 mv a0,a7
#if BR_MORRISON
 bleu s11,a6,.Lneuron_add_external\@
#else
 bltu s11,a6,.Lneuron_add_external\@
#endif
 /* Rare tail: thresholds are generated from this rule's exact mean. */
 li a0,3
 li t1,POISSON+12
 add t1,t1,gp
.Lneuron_poisson_tail\@:
 lw t3,0(t1)
#if BR_MORRISON
 bleu s11,t3,.Lneuron_poisson_done\@
#else
 bltu s11,t3,.Lneuron_poisson_done\@
#endif
 addi a0,a0,1
 addi t1,t1,4
#if BR_MORRISON
 /* The last table entry is UINT32_MAX and is necessarily taken. */
 j .Lneuron_poisson_tail\@
#else
 li t3,9
 bltu a0,t3,.Lneuron_poisson_tail\@
#endif
.Lneuron_poisson_done\@:
 fcvt.s.wu a0,a0
 fmul.s a0,s7,a0
.Lneuron_add_external\@:
 fadd.s t0,t0,a0
.Lneuron_no_external\@:
 sw t0,N_D_TOTAL(t5)
 bnez t6,.Lneuron_no_fire\@
 blt t4,s10,.Lneuron_no_fire\@
 li t6,5
 STATUS_STORE t6,N_REFRAC(t5)
 li t4,0
 li t1,1
 STATUS_STORE t1,N_FIRE(t5)
 /* Only firing neurons append; the integration join publishes the complete
  * list before forward/reverse/trace-finalization readers can run. */
 amoadd.w t3,t1,(a2)
 slli t3,t3,2
 add t3,t3,a3
 sw t5,0(t3)
.Lneuron_no_fire\@:
 sw t4,N_V(t5)
.if \traces
 lw t0,N_X(t5)
 lw t1,N_Y(t5)
 fmul.s t0,t0,s8
 fmul.s t1,t1,s9
 sw t0,N_X(t5)
 sw t1,N_Y(t5)
.endif
.endm
/* Both SMT workers claim a neuron address with AMOADD. Constants and one
 * xorshift state stay in registers for the complete invocation. tp is the SMT
 * identity set at boot; no worker frame or per-neuron RNG spill is needed. */
.globl br_integrate
br_integrate:
 slli t0,tp,2
 add t0,t0,gp
 lw s11,H_RNG0(t0)
 li s0,N_BYTES
 lw s1,H_EXCITATORY_END(gp)
 lw s2,H_P11(gp)
 lw s3,H_P21(gp)
 lw s4,H_P31(gp)
 lw s5,H_P32(gp)
 lw s6,H_P33(gp)
 lw s7,H_EXTERNAL_CURRENT(gp)
 fadd.s a7,s7,s7           # two external events, private to this SMT worker
 lw s8,H_DECAY_PRE(gp)
 lw s9,H_DECAY_POST(gp)
 li s10,0x41a00000
 li t0,BR_BASE+POISSON
 lw a4,0(t0)
 lw a5,4(t0)
 lw a6,8(t0)
 addi ra,gp,H_WORK_NEURON  # private until the worker returns to the event loop
 addi a2,gp,H_FIRE_COUNT
 li a3,BR_BASE+FIRED_LIST
.Lintegrate_claim:
 amoadd.w t5,s0,(ra)
 bgeu t5,s1,.Lintegrate_inhibitory
 INTEGRATE_NEURON 1
 j .Lintegrate_claim
.Lintegrate_inhibitory:
 /* The AMO has already claimed this I neuron. Process it before claiming
  * again, even if the other SMT worker is still finishing its last E neuron. */
 lw s1,H_NEURON_END(gp)
 j .Lintegrate_inhibitory_check
.Lintegrate_inhibitory_claim:
 amoadd.w t5,s0,(ra)
.Lintegrate_inhibitory_check:
 bgeu t5,s1,.Lintegrate_done
 INTEGRATE_NEURON 0
 j .Lintegrate_inhibitory_claim
.Lintegrate_done:
#if BR_PROXY && BR_PROXY_TRACES
 /* Both SMT workers also decay the group's E traces, with exactly the
  * home neuron's FP32 recurrence. Only trace metadata is replicated. */
 li a4,BR_BASE+PROXY_CONFIG
 lw a5,G_EXC_END(a4)
 addi a4,a4,G_TRACE_WORK
 li a6,32
.Lproxy_trace_claim:
 amoadd.w t0,a6,(a4)
 bgeu t0,a5,.Lproxy_trace_done
 lw t1,P_X(t0)
 fmul.s t1,t1,s8
 sw t1,P_X(t0)
 j .Lproxy_trace_claim
.Lproxy_trace_done:
#endif
 slli t0,tp,2
 add t0,t0,gp
 sw s11,H_RNG0(t0)
 li t0,1
 addi t1,gp,H_JOIN_COUNT
 amoadd.w t2,t0,(t1)
 bne t2,t0,.Lintegrate_return
 li a0,1
 li a1,0
 YIELDI BR_REPORT_ID
.Lintegrate_return:
 j br_idle
/* Direct event handler, selected at compile time. Input a0=input offset,
 * a1=weight,a2=weight address (zero for static),a3=source MC destination ID.
 * A quiet postsynaptic target
 * applies LTD locally and sends a no-receipt scalar store to the source MC. */
.macro SPIKE_BODY morrison,notify=0
 add t0,gp,a0
1:
 lr.w t1,(t0)
 fadd.s t1,t1,a1
 sc.w t2,t1,(t0)
 bnez t2,1b
 beqz a2,9f
 /* Reuse the accumulator pointer to address the other neuron fields. */
 STATUS_LOAD t1,N_FIRE-N_INPUT_E(t0)
 bnez t1,9f                /* post-active path handles LTD and LTP */
 lw t1,N_Y-N_INPUT_E(t0)
 beqz t1,9f                /* no learning effect; still count this delivery */
 /* Save the destination before computing directly into the store payload.
  * Y already contains the LTD coefficient, including its learning rate. */
 mv t0,a3
 LTD a3,a1,t1,\morrison
 mv a0,a2
#if BR_BULK
 li a1,0x20000
 SENDI t0,2
#else
 li a1,0x20000
 SENDI t0,1026
#endif
9:
#if BR_BULK_UNCOUNTED
 /* Passive observation may watch this existing final jump. No receiver
  * counter, wake, or completion reduction is executed by this variant. */
 .globl mb_spike_done
mb_spike_done:
#else
 /* Both SMT threads may deliver spikes; preserve the barrier count atomically. */
 addi t0,gp,H_RECEIVED
 li t1,1
 amoadd.w zero,t1,(t0)
.if \notify
 /* A bounded local yield remains usable after the posted store uses quota.
  * Only the receiver-closing entry executes this tail. */
 li t0,BR_BASE+MB_CONTROL+B_RX_PENDING
 li t1,1
 amoor.w t1,t1,(t0)
 bnez t1,8f
 YIELDI 13
8:
.endif
#endif
 j br_idle
.endm
#if BR_RULE == 1
.globl SPIKE_ENTRY
SPIKE_ENTRY:
 SPIKE_BODY 1
#else
.globl SPIKE_ENTRY
SPIKE_ENTRY:
 SPIKE_BODY 0
#endif
#if BR_BULK_TREE && !BR_BULK_UNCOUNTED
.globl mb_spike_receipt
mb_spike_receipt:
 SPIKE_BODY 1,1
#endif
#if !BR_BULK
.globl br_control
br_control:
 sw zero,H_REPORT_COUNT(gp)
 sw zero,H_REPORT_SUM(gp)
 lw t2,H_PU(gp)
 lw t3,H_NUM_PU(gp)
 slli t0,t2,1
 bgtu t0,t3,1f
 SENDI t0,2
 addi t0,t0,1
 bgtu t0,t3,1f
 SENDI t0,2
1:
/* Boot selects this entry directly on nodes whose report handler is a leaf. */
br_control_leaf:
 li t0,4
 beq a0,t0,2f
 li t0,5
 beq a0,t0,3f
 sw a0,H_PHASE(gp)
 /* Phase 1 is integration.  Phase 2 is forward delivery and static CSR
  * scatter.  Phase 3 remains post-active reverse learning. */
 li t0,1
 bne a0,t0,6f
 sw zero,H_JOIN_COUNT(gp)
#if BR_MORRISON
 /* Prior phase 6 completed globally. New deliveries cannot start until
  * every PU has reported this integration, so these resets cannot race. */
 sw zero,H_RECEIVED(gp)
 sw zero,H_REVERSE_RECEIVED(gp)
 sw zero,H_EXPECTED_FORWARD(gp)
 sw zero,H_EXPECTED_REVERSE(gp)
#endif
 /* Clear only last step's firing flags, after the global drain and before
  * either integration worker starts. Keep this step's flags through dumps. */
 lw t1,H_FIRE_COUNT(gp)
 li t0,BR_BASE+FIRED_LIST
 beqz t1,.Lclear_fire_done
.Lclear_fire:
 lw t2,0(t0)
 STATUS_STORE zero,N_FIRE(t2)
 addi t0,t0,4
 addi t1,t1,-1
 bnez t1,.Lclear_fire
.Lclear_fire_done:
 sw zero,H_FIRE_COUNT(gp)
#if BR_PROXY
 li t0,BR_BASE+PROXY_CONFIG
#if BR_MORRISON
 li t1,MR_PROXY_SEEN_WORDS
 li t2,BR_BASE+MR_PROXY_SEEN
.Lproxy_clear:
 sw zero,0(t2)
#else
 lw t1,G_JOBS(t0)
 li t2,BR_BASE+PROXY_JOBS
 beqz t1,.Lproxy_clear_done
.Lproxy_clear:
 lw t3,0(t2)
 sw zero,P_FIRE(t3)
#endif
 addi t2,t2,4
 addi t1,t1,-1
 bnez t1,.Lproxy_clear
.Lproxy_clear_done:
 sw zero,G_JOBS(t0)
 sw zero,G_MARKERS(t0)
 sw zero,G_EXPECTED(t0)
 sw zero,G_NOTIFY_CURSOR(t0)
#if BR_PROXY_TRACES
 li t1,BR_BASE+PROXY_ROWS
 sw t1,G_TRACE_WORK(t0)
#endif
#endif
 addi t0,gp,256
 sw t0,H_WORK_NEURON(gp)
 SENDI zero,9
 YIELDI 9
 j br_idle
6: sw zero,H_WORK_LINE(gp)
 sw zero,H_WORK_MC(gp)
 sw zero,H_SENT(gp)
 sw zero,H_WORK_NEURON(gp)
#if BR_PROXY
 li t0,2
 beq a0,t0,.Lproxy_start
#endif
 j br_continue
#if BR_PROXY
.Lproxy_start:
 YIELDI 13
 j br_idle
#endif
2: lw a1,H_RECEIVED(gp)
 j 4f
3: lw a1,H_REVERSE_RECEIVED(gp)
4: YIELDI BR_REPORT_ID
 j br_idle
/* Root-only broadcast and the simulator's phase-marker ABI. */
br_broadcast:
 mv a4,a0
 li t0,4
 beq a0,t0,2f
 li t0,5
 beq a0,t0,2f
 li t0,6
 beq a0,t0,1f
 addi a0,a0,1
 j 3f
1: li a0,5
3: lw a1,H_STEP(gp)
 addi a1,a1,1
 li a2,0
 li a3,0
 li t0,1
 SENDI t0,10
2: mv a0,a4
 YIELDI 2
 j br_idle
.globl br_report
br_report:
 lw t0,H_REPORT_COUNT(gp)
 lw t1,H_REPORT_SUM(gp)
 addi t0,t0,1
 add t1,t1,a1
 lw t6,H_TREE_REPORTS(gp)
 bne t0,t6,.Lreport_partial
 lw t3,H_PU(gp)
 li t0,1
 beq t3,t0,.Lroot_report
 srli t0,t3,1
 mv a1,t1
 YIELD_TO t0,3
 j br_idle
.Lreport_partial:
 sw t0,H_REPORT_COUNT(gp)
 sw t1,H_REPORT_SUM(gp)
 j br_idle
/* Boot selects this handler for nodes without children. Their local report
 * is already the complete aggregate; a0/a1 pass straight to the parent. */
.globl br_report_leaf
br_report_leaf:
 lw t0,H_PU(gp)
 srli t0,t0,1
 YIELD_TO t0,3
 j br_idle
.Lroot_report:
 beqz a0,.Lreport_boot
 li t0,1
 beq a0,t0,.Lreport_integrate
 li t0,2
 beq a0,t0,.Lreport_forward
 li t0,3
 beq a0,t0,.Lreport_reverse
 li t0,4
 beq a0,t0,.Lquery_forward
 li t0,5
 beq a0,t0,.Lquery_reverse
 /* Finish: all nodes and memory notifications are drained. */
 lw t0,H_STEP(gp)
 sw t0,H_COMPLETED(gp)
 lw t1,H_WARMUP(gp)
 bne t0,t1,1f
 li t1,0x40000004
 li t2,1
 sw t2,0(t1)
1: lw t1,H_STEPS(gp)
 bne t0,t1,.Lnext_forward
 li t1,0x40000004
 sw zero,0(t1)
 li a0,0
 j br_halt
.Lreport_boot:
 lw t0,H_WARMUP(gp)
 bnez t0,.Lnext_forward
 li t0,0x40000004
 li t1,1
 sw t1,0(t0)
.Lnext_forward:
 li a0,1
 j br_broadcast
.Lreport_integrate:
 /* The integration barrier advances directly to phase 2 forward work. */
 li a0,2
 j br_broadcast
.Lreport_forward:
 lw t0,H_EXPECTED_FORWARD(gp)
 add t0,t0,t1
 sw t0,H_EXPECTED_FORWARD(gp)
 li a0,4
 j br_broadcast
.Lquery_forward:
 lw t0,H_EXPECTED_FORWARD(gp)
 bgtu t1,t0,.Lreport_bad
 li a0,4
 bne t0,t1,1f
 li a0,3
1: j br_broadcast
.Lnext_reverse:
 li a0,3
 j br_broadcast
.Lreport_reverse:
 lw t0,H_EXPECTED_REVERSE(gp)
 add t0,t0,t1
 sw t0,H_EXPECTED_REVERSE(gp)
 li a0,5
 j br_broadcast
.Lquery_reverse:
 lw t0,H_EXPECTED_REVERSE(gp)
 bgtu t1,t0,.Lreport_bad
 li a0,5
 bne t0,t1,1f
 li a0,6
1: j br_broadcast
.Lreport_bad:
 li a0,0xb009
 j br_halt
#endif
.globl br_boot
br_boot:
 ENTER 3
 li t0,BR_BASE+0x3f00
 li t1,0x19ca7ead
 sw t1,0(t0)
 sw t1,128(t0)
 lw t0,H_MAGIC(gp)
#if BR_MORRISON
 li t1,MR_MAGIC
#else
 li t1,0x42524e34
#endif
 bne t0,t1,.Lboot_bad
 /* Fixed transport parameters; the image must match this compiled rule. */
 lw t0,H_MC(gp)
 li t1,0x8001
 add t0,t0,t1
 sw t0,H_MC_DEST(gp)
 li t0,1024
 csrw 0x733,t0
 lw t0,H_RULE(gp)
 li t1,BR_RULE
 bne t0,t1,.Lboot_bad
 /* The reduction tree is fixed: every report waits for itself plus its
  * existing children. Compute that once, rather than at every barrier. */
 lw t0,H_PU(gp)
 lw t1,H_NUM_PU(gp)
 slli t2,t0,1
 sltu t3,t1,t2
 xori t3,t3,1
 addi t3,t3,1
 addi t2,t2,1
 sltu t2,t1,t2
 xori t2,t2,1
 add t3,t3,t2
 sw t3,H_TREE_REPORTS(gp)
 li t0,1
 bne t3,t0,.Lboot_tree_ready
 la t0,br_report_leaf
 csrw (0x700+BR_REPORT_ID),t0
 la t0,br_control_leaf
 csrw (0x700+BR_CONTROL_ID),t0
.Lboot_tree_ready:
 lw t0,H_EPSC(gp)
 lw t1,H_EXTERNAL_WEIGHT(gp)
 fmul.s t0,t0,t1
 sw t0,H_EXTERNAL_CURRENT(gp)
 /* Seed the clearing list from the presimulated image's firing flags. */
 li t0,BR_BASE+FIRED_LIST
 li t1,0
 addi t2,gp,256
 sw t2,H_EXCITATORY_END(gp)
 lw t3,H_NN(gp)
#if BR_MORRISON
 li t4,MR_MAX_NEURONS
#else
 li t4,25
#endif
 bgtu t3,t4,.Lboot_bad
 beqz t3,.Lboot_fire_done
 lw a4,H_POT(gp)
 lw a5,H_DEP(gp)
#if BR_MORRISON
 li t4,BR_BASE+MR_PARAMETERS
 lw t5,16(t4)
#if BR_BULK
 li t6,MB_ABI_VERSION
#else
 li t6,MR_ABI_VERSION
#endif
 bne t5,t6,.Lboot_bad
 lw t5,20(t4)
#if BR_BULK
 beqz t5,.Lboot_bad
 addi t6,t5,-1
 and t6,t6,t5
 bnez t6,.Lboot_bad
 lw t6,32(t4)
 beqz t6,.Lboot_bulk_legacy_width
 li a6,MB_DISPATCH_BALANCED
 bne t6,a6,.Lboot_bad
 lw a6,H_NUM_PU(gp)
 lw a7,8(t4)
 beqz a7,.Lboot_bad
 addi t6,a7,-1
 and t6,t6,a7
 bnez t6,.Lboot_bad
.Lboot_bulk_domain_width:
 srli a7,a7,1
 beqz a7,.Lboot_bulk_width_ready
 srli a6,a6,1
 j .Lboot_bulk_domain_width
.Lboot_bulk_width_ready:
 bgtu t5,a6,.Lboot_bad
 j .Lboot_bulk_width_done
.Lboot_bulk_legacy_width:
 li t6,4
 bgtu t5,t6,.Lboot_bad
.Lboot_bulk_width_done:
#if BR_BULK_TREE
 lw t6,36(t4)
 li a6,MB_FORWARD_TREE
 bne t6,a6,.Lboot_bad
#endif
#else
 li t6,BR_PROXY_SIZE
 bne t5,t6,.Lboot_bad
#endif
 lw t5,8(t4)
 beqz t5,.Lboot_bad
 li t6,16
 bgtu t5,t6,.Lboot_bad
 lw a6,H_PU(gp)
 addi a6,a6,-1
 lw a7,H_NUM_PU(gp)
#endif
.Lboot_fire_list:
#if !BR_MORRISON
 /* Both channels have the same alpha filter. Keep their sum in two words;
  * incoming E/I accumulators remain separate. Convert the image only once. */
 lw t4,N_IE(t2)
 lw t5,N_II(t2)
 fadd.s t4,t4,t5
 sw t4,N_I_TOTAL(t2)
 lw t4,N_DE(t2)
 lw t5,N_DI(t2)
 fadd.s t4,t4,t5
 sw t4,N_D_TOTAL(t2)
 sw zero,N_II(t2)
 sw zero,N_DI(t2)
#endif
 /* E records must be a contiguous prefix. Keep its end for both dynamic
  * integration workers; I traces are unused by this E->E plastic graph. */
#if BR_MORRISON
 mv t4,a6
 add a6,a6,a7
 li t5,BR_BASE+MR_PARAMETERS
 lw t5,0(t5)
#else
 lw t4,N_GID(t2)
 li t5,9000
#endif
 bgeu t4,t5,.Lboot_inhibitory_traces
 lw t4,H_EXCITATORY_END(gp)
 bne t4,t2,.Lboot_bad
 addi t4,t2,N_BYTES
 sw t4,H_EXCITATORY_END(gp)
#if !BR_MORRISON
 /* Images retain canonical traces. E runtime traces carry their learning
  * rates, avoiding repeated coefficient multiplications at receivers. */
 lw t4,N_X(t2)
 lw t5,N_Y(t2)
 fmul.s t4,t4,a4
 fmul.s t5,t5,a5
 sw t4,N_X(t2)
 sw t5,N_Y(t2)
#endif
 j .Lboot_traces_ready
.Lboot_inhibitory_traces:
 sw zero,N_X(t2)
 sw zero,N_Y(t2)
.Lboot_traces_ready:
 STATUS_LOAD t4,N_FIRE(t2)
 beqz t4,1f
 sw t2,0(t0)
 addi t0,t0,4
 addi t1,t1,1
1: addi t2,t2,N_BYTES
 addi t3,t3,-1
 bnez t3,.Lboot_fire_list
.Lboot_fire_done:
 sw t1,H_FIRE_COUNT(gp)
 sw t2,H_NEURON_END(gp)
 li t0,BR_BASE+PROXY_CONFIG
 lw t1,G_MAGIC(t0)
#if BR_PROXY
 li t2,BR_PROXY_MAGIC
 bne t1,t2,.Lboot_bad
 lw t1,G_COUNT(t0)
 li t2,PROXY_CAPACITY
 bgtu t1,t2,.Lboot_bad
#if BR_PROXY_TRACES
 lw t1,G_EXC_END(t0)
 li t0,BR_BASE+PROXY_ROWS
 lw t3,H_POT(gp)
.Lboot_proxy_traces:
 bgeu t0,t1,.Lboot_proxy_done
 lw t2,P_X(t0)
 fmul.s t2,t2,t3
 sw t2,P_X(t0)
 addi t0,t0,32
 j .Lboot_proxy_traces
.Lboot_proxy_done:
#endif
#else
 bnez t1,.Lboot_bad
#endif
#if !BR_MORRISON
 li t0,BR_BASE+REPLAY
 lw t0,28(t0)
 beqz t0,.Lboot_bad
 li t1,256
 bgtu t0,t1,.Lboot_bad
#endif
 lw t0,H_PU(gp)
 li t1,1
 bne t0,t1,.Lboot_report
#ifdef BR_ASM_SELFTEST
 jal ra,br_asm_selftest
#endif
 li s0,0
.Lboot_store:
 slli t0,s0,2
 li a0,0x8000
 add a0,a0,t0
 andi t0,s0,-8
 slli t0,t0,5
 andi t1,s0,7
 add t0,t0,t1
 li a1,0x19260000
 add a1,a1,t0
 jal ra,br_store_word
 addi s0,s0,1
 li t0,32
 bltu s0,t0,.Lboot_store
 jal ra,br_drain
 li s0,0
.Lboot_load:
 slli a0,s0,5
 li t0,0x8000
 add a0,a0,t0
 mv a1,s0
 jal ra,br_load_start
 addi s0,s0,1
 li t0,4
 bltu s0,t0,.Lboot_load
 li s0,0
.Lboot_verify:
 mv a0,s0
 jal ra,br_load_wait
 slli s2,s0,5
 li t0,BR_BASE+READ
 add s2,s2,t0
 li s1,0
1: lw t0,0(s2)
 slli t1,s0,8
 add t1,t1,s1
 li t2,0x19260000
 add t1,t1,t2
 bne t0,t1,.Lboot_bad
 addi s2,s2,4
 addi s1,s1,1
 li t0,8
 bltu s1,t0,1b
 addi s0,s0,1
 li t0,4
 bltu s0,t0,.Lboot_verify
 jal ra,br_drain
 sw zero,H_ISSUED(gp)
 sw zero,H_CONSUMED(gp)
.Lboot_report:
#if !BR_MORRISON
 addi t0,gp,256
 lw t1,N_RNG(t0)
 sw t1,H_RNG0(gp)
 addi t0,t0,N_BYTES
 lw t1,N_RNG(t0)
 sw t1,H_RNG1(gp)
#else
 /* Compact images seed the two workers directly, with nonzero states. */
 lw t0,H_RNG0(gp)
 beqz t0,.Lboot_bad
 lw t0,H_RNG1(gp)
 beqz t0,.Lboot_bad
#endif
 li a0,0
 li a1,0
 li a2,0
 li t0,0
 li t1,BR_REPORT_ID
 SEND t0,t1
 LEAVE 3
.Lboot_bad:
 li a0,0xb00b
 j br_halt
#if !BR_BULK
#if BR_PROXY
/* One sender per home PU, one receiver thread per proxy. Last notification
 * carries the sender's total job count (a2); -1 means more will follow.
 * Waiting for all group end markers and their total job count does not
 * assume that different NoC paths or queues preserve arrival order. */
.globl br_proxy_notify
br_proxy_notify:
 lw t0,H_WORK_NEURON(gp)
 lw t1,H_FIRE_COUNT(gp)
 li a0,0
 li a1,0
 li a2,0
 beqz t1,.Lproxy_notify_send
 slli t2,t0,2
 li t3,BR_BASE+FIRED_LIST
 add t2,t2,t3
 lw t2,0(t2)
#if BR_MORRISON
 /* Local source slot + 1, reserving zero for an empty notification. */
 sub a0,t2,gp
 addi a0,a0,-256
 srli a0,a0,5
 li t2,BR_PROXY_SIZE
 mul a0,a0,t2
 lw t2,H_PU(gp)
 addi t2,t2,-1
 andi t2,t2,(BR_PROXY_SIZE-1)
 add a0,a0,t2
 addi a0,a0,1
#else
 lw a0,N_BUFFER(t2)
#endif
#if BR_PROXY_TRACES
 lw a1,N_X(t2)
#endif
 addi t0,t0,1
 li a2,-1
 bne t0,t1,.Lproxy_notify_send
 mv a2,t1
.Lproxy_notify_send:
 li t2,BR_BASE+PROXY_CONFIG
#if BR_PROXY_SIZE == 8
 lw t3,G_NOTIFY_CURSOR(t2)
 add t2,t2,t3
#endif
 lw t3,0(t2)
 SENDI t3,-1019              # 0xc05, same traffic class as spike delivery
#if BR_PROXY_SIZE >= 2
 lw t3,4(t2)
 SENDI t3,-1019
#endif
#if BR_PROXY_SIZE >= 4
 lw t3,8(t2)
 SENDI t3,-1019
 lw t3,12(t2)
 SENDI t3,-1019
#endif
#if BR_PROXY_SIZE == 8
 /* Four sends plus the continuation fit the unchanged quota of five. */
 li t2,BR_BASE+PROXY_CONFIG
 lw t3,G_NOTIFY_CURSOR(t2)
 xori t3,t3,16
 sw t3,G_NOTIFY_CURSOR(t2)
 bnez t3,.Lproxy_notify_more
#endif
 lw t0,H_WORK_NEURON(gp)
 addi t0,t0,1
 sw t0,H_WORK_NEURON(gp)
 bltz a2,.Lproxy_notify_more
 sw zero,H_WORK_NEURON(gp)
 YIELDI 14
 j br_idle
.Lproxy_notify_more:
 YIELDI 13
 j br_idle
.globl br_proxy_receive
br_proxy_receive:
 li t0,BR_BASE+PROXY_CONFIG
 beqz a0,.Lproxy_receive_marker
#if BR_MORRISON
 addi t1,a0,-1
 li t2,PROXY_CAPACITY
 bgeu t1,t2,.Lboot_bad
 srli t2,t1,5
 slli t2,t2,2
 li t3,BR_BASE+MR_PROXY_SEEN
 add t2,t2,t3
 andi t1,t1,31
 li t3,1
 sll t3,t3,t1
 lw t1,0(t2)
 and t4,t1,t3
 bnez t4,.Lboot_bad
 or t1,t1,t3
 sw t1,0(t2)
#else
 add t1,gp,a0
 lw t2,P_FIRE(t1)
 bnez t2,.Lboot_bad        # each neuron is notified once per timestep
#if BR_PROXY_TRACES
 sw a1,P_X(t1)
#endif
 li t2,1
 sw t2,P_FIRE(t1)
#endif
 lw t2,G_JOBS(t0)
 li t3,PROXY_CAPACITY
 bgeu t2,t3,.Lboot_bad
#if BR_MORRISON
 slli t3,t2,1
#else
 slli t3,t2,2
#endif
 li t4,BR_BASE+PROXY_JOBS
 add t3,t3,t4
#if BR_MORRISON
 sh a0,0(t3)
#else
 sw t1,0(t3)
#endif
 addi t2,t2,1
 sw t2,G_JOBS(t0)
.Lproxy_receive_marker:
 bltz a2,.Lproxy_receive_done
 lw t1,G_MARKERS(t0)
 addi t1,t1,1
 sw t1,G_MARKERS(t0)
 lw t1,G_EXPECTED(t0)
 add t1,t1,a2
 sw t1,G_EXPECTED(t0)
.Lproxy_receive_done:
 j br_idle
.globl br_proxy_wait
br_proxy_wait:
 li t0,BR_BASE+PROXY_CONFIG
 lw t1,G_MARKERS(t0)
 li t2,BR_PROXY_SIZE
 bne t1,t2,.Lproxy_wait_again
 lw t1,G_EXPECTED(t0)
 lw t2,G_JOBS(t0)
 bne t1,t2,.Lproxy_wait_again
 YIELDI BR_WORK_ID
 j br_idle
.Lproxy_wait_again:
 YIELDI 14
 j br_idle
#endif
/* Cooperative work: bounded sends per invocation, descriptor ownership on
 * SMT0. All intervening handlers preserve s0-s11, so an unfinished directory
 * keeps its row pointer, coefficients, and next MC in registers. */
.globl br_work
br_work:
 lw t0,H_WORK_MC(gp)
 bnez t0,.Ldirectory_resume
 lw s0,H_PHASE(gp)
 lw s1,H_WORK_NEURON(gp)
 li t0,6
 beq s0,t0,.Lwork_finish
 /* H_WORK_NEURON indexes the compact list throughout forward/reverse work. */
#if BR_PROXY
 li t0,2
 bne s0,t0,.Lhome_fired_list
 li t1,BR_BASE+PROXY_CONFIG
 lw t2,G_JOBS(t1)
 li t1,BR_BASE+PROXY_JOBS
 j .Lfired_list_ready
.Lhome_fired_list:
#endif
 lw t2,H_FIRE_COUNT(gp)
 li t1,BR_BASE+FIRED_LIST
#if BR_PROXY
.Lfired_list_ready:
#endif
 bgeu s1,t2,.Lwork_report
#if BR_MORRISON
 li t0,2
 bne s0,t0,.Lcompact_home_row
 slli t0,s1,1
 add t0,t0,t1
 lhu a0,0(t0)
 addi a0,a0,-1
 lw t0,H_WORK_LINE(gp)
 bnez t0,.Lcompact_descriptor_ready
 jal ra,br_descriptor
.Lcompact_descriptor_ready:
 li s2,BR_BASE+MR_DESCRIPTOR
 j .Lwork_neuron_ready
.Lcompact_home_row:
#endif
 slli t0,s1,2
 add t0,t0,t1
 lw s2,0(t0)
.Lwork_neuron_ready:
 li t0,2
 bne s0,t0,.Lwork_reverse_neuron
 /* Start a fired row once; later work events finish its static directories. */
 lw t0,H_WORK_LINE(gp)
 bnez t0,.Lwork_directory
#if BR_PROXY
 lw s7,P_COUNT(s2)
#else
 lw s7,N_COUNT(s2)
#endif
 beqz s7,.Lwork_directory
.Lforward_first:
 li s3,0                     # row synapse index at chunk start
 li s11,0                    # landing half, 0 or 1
 lw s10,H_MC_DEST(gp)       # receiver can write back without decoding identity
#if BR_DIAGNOSTIC
 li t0,STREAM
 add t0,t0,gp
 li t1,1
 sw t1,0(t0)
 lw t1,4(t0)
 addi t1,t1,1
 sw t1,4(t0)
#endif
#if BR_PROXY
 lw s8,P_WEIGHTS(s2)
#else
 lw s8,N_WEIGHTS(s2)      # row base stays live across all chunk continuations
#endif
 mv a0,s8
 li a1,PING
#if BR_MORRISON
 mv a2,s7
#endif
 jal ra,br_ac_start
 YIELDI 11
 j br_idle
/* Frame-free local continuations preserve s0-s11. Higher-priority SMT0
 * reverse/service handlers preserve these registers with their bounded ABI. */
.globl br_forward_wait
br_forward_wait:
 csrr t0,0x730
 beqz t0,.Lforward_ready
#if BR_DIAGNOSTIC
 li t0,STREAM
 add t0,t0,gp
 lw t1,16(t0)
 addi t1,t1,1
 sw t1,16(t0)
#endif
 YIELDI 11
 j br_idle
.Lforward_ready:
 sub s4,s7,s3
 li t0,128
 bleu s4,t0,1f
 mv s4,t0
1:
#if BR_MORRISON
 /* Descriptor is no longer needed while draining. Keep the target-half
  * displacement in s2; the work continuation reacquires the staged row. */
 addi s2,s4,7
 andi s2,s2,-8
 slli s2,s2,2
#endif
 slli s5,s11,10
 li t0,BR_BASE+PING
 add s5,s5,t0
 slli t0,s4,2
 add s9,s5,t0              # weight-half end
 slli t0,s3,3             # chunk starts are multiples of 128 synapses
 add s6,s8,t0             # 1 KiB (weights+targets) per 128-synapse chunk
 add t0,s3,s4
 bgeu t0,s7,.Lforward_last_chunk
 /* The sole AC engine now fills the other half while this half drains. */
#if BR_DIAGNOSTIC
 li t0,STREAM
 add t0,t0,gp
 li t1,2
 sw t1,0(t0)
 lw t1,4(t0)
 addi t1,t1,1
 sw t1,4(t0)
#endif
 addi a0,s6,1024
 xori a1,s11,1
 slli a1,a1,10
 li t0,PING
 add a1,a1,t0
#if BR_MORRISON
 add a2,s3,s4
 sub a2,s7,a2
#endif
 jal ra,br_ac_start
 /* Keep the affine DRAM-to-SPM delta, advancing only one pointer per spike. */
 sub s6,s6,s5
 YIELDI 12
 j br_idle
.Lforward_last_chunk:
#if BR_DIAGNOSTIC
 li t0,STREAM
 add t0,t0,gp
 li t1,3
 sw t1,0(t0)
#endif
 sub s6,s6,s5
 YIELDI 12
 j br_idle
.globl br_forward_drain
br_forward_drain:
 bgeu s5,s9,.Lforward_chunk_done
#if BR_MORRISON
 add t0,s5,s2
 lw t0,0(t0)
#else
 lw t0,512(s5)             # destination: high 16 input offset, low 16 PU
#endif
 lw a1,0(s5)               # already scaled FP32 weight
 srli a0,t0,16
 add a2,s5,s6
 mv a3,s10
 addi s5,s5,4
 SENDI t0,-1023            # 0xc01; destination is truncated to 16 bits
 YIELDI 12                 # replenish quota and admit higher-priority events
 j br_idle
.Lforward_chunk_done:
 /* Sample AC readiness immediately at the drain boundary. */
#if BR_DIAGNOSTIC
 csrr t3,0x730
#endif
#if BR_DIAGNOSTIC
 lw t0,H_FORWARD_WEIGHTS(gp)
 add t0,t0,s4
 sw t0,H_FORWARD_WEIGHTS(gp)
#endif
 add s3,s3,s4
 bgeu s3,s7,.Lforward_row_done
#if BR_DIAGNOSTIC
 li t0,STREAM
 add t0,t0,gp
 lw t1,8(t0)
 addi t1,t1,1
 sw t1,8(t0)
 bnez t3,1f
 lw t1,12(t0)
 addi t1,t1,1
 sw t1,12(t0)
1: li t1,1
 sw t1,0(t0)
#endif
 xori s11,s11,1
 YIELDI 11
 j br_idle
br_forward_row_done:
.Lforward_row_done:
 lw t0,H_SENT(gp)
 add t0,t0,s3
 sw t0,H_SENT(gp)
#if BR_DIAGNOSTIC
 li t0,STREAM
 add t0,t0,gp
 sw zero,0(t0)
#endif
 sw s3,H_WORK_LINE(gp)
 /* Scalar ID-2 refills use the same base: restore it before any directory
  * or MSHR loads. There is no AC outstanding after draining the last half. */
 csrw 0x732,gp
 sw zero,H_WORK_MC(gp)
 j .Lwork_continue
br_work_reverse_row:
.Lwork_reverse_neuron:
#if BR_MORRISON
 lw t0,H_EXCITATORY_END(gp)
 bgeu s2,t0,.Lwork_next_neuron
 lw s8,N_Y(s2)
 sub a0,s2,gp
 addi a0,a0,-256
 srli a0,a0,5
 li t0,BR_PROXY_SIZE
 mul a0,a0,t0
 lw t0,H_PU(gp)
 addi t0,t0,-1
 andi t0,t0,(BR_PROXY_SIZE-1)
 add a0,a0,t0
 jal ra,br_descriptor
 li s2,BR_BASE+MR_DESCRIPTOR
#else
 lw t0,44(s2)
 li t1,9000
 bgeu t0,t1,.Lwork_next_neuron
 /* Reverse coefficients are constant for this row across all MC entries. */
 lw s8,N_Y(s2)
#endif
br_work_directory:
.Lwork_directory:
 lw s9,H_SENT(gp)         # accumulator persists across directory continuations
 li s6,0
 li t0,2
 bne s0,t0,1f
#if BR_PROXY
 lw s7,P_STATIC_DIR(s2)
#else
 lw s7,N_STATIC_DIR(s2)
#endif
 li s8,0
 li s11,0xc01
 j 2f
1:
#if BR_MORRISON
 lw s7,24(s2)
#else
 lw s7,N_REVERSE_DIR(s2)
#endif
#if BR_RELAY_UPDATES
 li s11,0xc07           # scatter to the home trace relay, not weight handler
#else
 li s11,0xc04
#endif
2:
 /* A directory is 16 eight-byte descriptors, aligned to a 32-byte line.
  * Issue all four scalar refills into READ[0..3], then consume each line
  * after its completion. The 12-request credit limit covers these requests
  * together with concurrent MSHR writebacks/refills. No AC is active here. */
#if BR_MORRISON
 li t0,BR_BASE+MR_PARAMETERS
 lw s5,8(t0)
 addi s5,s5,3
 srli s5,s5,2
.Ldirectory_load:
 slli a0,s6,5
 add a0,a0,s7
 mv a1,s6
 jal ra,br_load_start
 addi s6,s6,1
 bltu s6,s5,.Ldirectory_load
 li s6,0
#else
 mv a0,s7
 li a1,0
 jal ra,br_load_start
 addi a0,s7,32
 li a1,1
 jal ra,br_load_start
 addi a0,s7,64
 li a1,2
 jal ra,br_load_start
 addi a0,s7,96
 li a1,3
 jal ra,br_load_start
#endif
 li s7,BR_BASE+READ
.Ldirectory_resume:
#if BR_MORRISON
 li t0,BR_BASE+MR_PARAMETERS
 lw t0,8(t0)
#else
 li t0,16
#endif
 bgeu s6,t0,.Ldirectory_done
 andi t0,s6,3
 bnez t0,3f
 srli a0,s6,2
 jal ra,br_load_wait
3: lw a0,0(s7)
 lw s5,4(s7)
 beqz s5,5f
 mv a2,s8
 addi a1,s5,3
 srli a1,a1,2
 slli a1,a1,16
 or a1,a1,s11
 li t0,0x8001
 add t0,t0,s6
 SENDI t0,-1008
 add s9,s9,s5
5: addi s6,s6,1
 addi s7,s7,8
 sw s6,H_WORK_MC(gp)
 j .Lwork_continue
.Ldirectory_done:
 sw s9,H_SENT(gp)
br_work_advance:
.Lwork_next_neuron:
 addi s1,s1,1
 sw s1,H_WORK_NEURON(gp)
 sw zero,H_WORK_LINE(gp)
 sw zero,H_WORK_MC(gp)
 j .Lwork_continue
br_work_flush:
.Lwork_finish:
.Lflush_done:
 jal ra,br_drain
 lw t0,H_MSHR_COUNT(gp)
 bnez t0,.Lboot_bad
 lw t0,H_MSHR_SCHEDULED(gp)
 bnez t0,.Lboot_bad
 li t0,BR_BASE+0x3f00
 li t1,0x19ca7ead
 lw t2,0(t0)
 bne t1,t2,.Lboot_bad
 lw t2,128(t0)
 bne t1,t2,.Lboot_bad
 li s2,BR_BASE+FIRED_LIST
 lw s1,H_FIRE_COUNT(gp)
 beqz s1,.Ltrace_done
 lw t3,H_POT(gp)
 lw t4,H_DEP(gp)
 lw t5,H_EXCITATORY_END(gp)
1: lw t0,0(s2)
 bgeu t0,t5,2f
 lw t1,N_X(t0)
 lw t2,N_Y(t0)
 fadd.s t1,t1,t3
 fadd.s t2,t2,t4
 sw t1,N_X(t0)
 sw t2,N_Y(t0)
2: addi s2,s2,4
 addi s1,s1,-1
 bnez s1,1b
.Ltrace_done:
#if BR_PROXY && BR_PROXY_TRACES
 li t0,BR_BASE+PROXY_CONFIG
 lw t1,G_JOBS(t0)
 li t0,BR_BASE+PROXY_JOBS
 lw t5,H_POT(gp)
 beqz t1,.Lproxy_increment_done
.Lproxy_increment:
 lw t2,0(t0)
 lw t3,P_GID(t2)
 li t4,9000
 bgeu t3,t4,.Lproxy_increment_next
 lw t3,P_X(t2)
 fadd.s t3,t3,t5
 sw t3,P_X(t2)
.Lproxy_increment_next:
 addi t0,t0,4
 addi t1,t1,-1
 bnez t1,.Lproxy_increment
.Lproxy_increment_done:
#endif
 INC H_STEP,t0
 li a0,6
 li a1,0
 j .Lwork_send_report
br_work_report:
.Lwork_report:
 /* Establish reverse-only constants before reporting forward completion.
  * Another PU may scatter reverse events before our phase-3 control arrives. */
 li t0,2
 bne s0,t0,.Lwork_constants_ready
#if BR_RULE == 1
 li s3,0x4c2bac89
 li s4,0xcccccccd
 li s10,0x3f99ca19
#else
 lw s3,H_WMAX(gp)
#endif
.Lwork_constants_ready:
 mv a0,s0
 lw a1,H_SENT(gp)
.Lwork_send_report:
 YIELDI BR_REPORT_ID
 j .Lwork_return
br_work_continue:
.Lwork_continue:
 YIELDI BR_WORK_ID
.Lwork_return:
 j br_idle
#if BR_MORRISON
/* a0=zero-based group source slot. Scalar ID-2 loads use the live AC base;
 * wait for completion before returning to a caller that may start streaming. */
br_descriptor:
 ENTER 0
 slli a0,a0,5
 li t0,BR_BASE+MR_PARAMETERS
 lw t0,12(t0)
 add a0,a0,t0
 li a1,0
 li a2,(MR_DESCRIPTOR/32)
 jal ra,br_line_start
 li a0,0
 jal ra,br_load_wait
 LEAVE 0
#endif
#endif
/* Shared image format, hand-written boot copy. No C code in this payload. */
.globl snn_init
snn_init:
 csrr a0,0xf14
 li t0,0x80000000
 lw t1,4(t0)
 lw t2,12(t0)
 beqz a0,.Linit_bad
 bgtu a0,t2,.Linit_bad
 add t1,t1,t0
 addi t2,a0,-1
 slli t2,t2,3
 add t2,t2,t1
 lw t3,0(t2)
 lw t4,4(t2)
 add t3,t3,t1
 or t0,t3,t4
 andi t0,t0,31
 bnez t0,.Linit_bad
 csrw 0x731,t3
 csrw 0x732,gp
 csrw 0x733,t4
 csrw 0x730,zero
1: csrr t0,0x730
 bnez t0,1b
 ret
.Linit_bad:
 li a0,0xbad30000
 j br_halt

#if BR_BULK
#include "morrison_bulk_runtime.inc"
#endif
