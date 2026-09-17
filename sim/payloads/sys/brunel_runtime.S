/* Hand-written Brunel runtime, RV32I/Zmmul/Zfinx.
 * gp = immutable local SPM base on both SMT threads.
 * Direct event handlers end at br_idle. s0-s11 retain the cooperative worker's
 * state across yields; incoming handlers and private MSHR helpers preserve
 * them. Helper-specific scratch and return-link registers are documented below.
 * SMT0 owns forward/directory/MSHR work and credited memory requests;
 * integration and spike reception use both SMT threads.
 * Forward delivery uses only the 2 KiB ping-pong staging area; quiet LTD is
 * committed at the postsynaptic PU with a fire-and-forget scalar store.
 * Reverse events carry full MC-local addresses. Memory effects must be ordered.
 */
#include "brunel_asm_layout.inc"
.option norelax
.equ BUFFER, 0x900
.equ BR_BASE, 0x20000000
.equ MSHR, 0x2900
.equ READ, 0x2d00
.equ REPLAY, 0x2d80
.equ RECORD, 0x2f00
.equ DIRTY, 0x2e00
.equ PING, 0x3700
.equ POISSON, 0x2da4
.equ STREAM, 0x2dd0
.equ TREE_REPORTS, STREAM+20
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
.text
/* Completion handler is a leaf so it can retire notifications quickly. */
.globl br_completion
br_completion:
 li t0,0x8000
 and t0,a0,t0
 bnez t0,1f
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
 bltu a1,t0,.Lbad_completion
 li t0,READ
 bgeu a1,t0,.Lbad_completion
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
/* These leaf transports do not impose address-specific completion waits. */
br_load_start:
 slli a2,a1,5
 li t0,READ
 add a2,a2,t0
/* a0=DRAM address, a1=completion token, a2=SPM byte offset. */
br_line_start:
 li t0,4
 bgeu a1,t0,2f
 slli t0,a1,2
 add t0,gp,t0
 sw zero,80(t0)
2:
.Lload_credit_wait:
 lw t0,H_ISSUED(gp)
 lw t1,H_CONSUMED(gp)
 sub t1,t0,t1
 sltiu t1,t1,12
 beqz t1,.Lload_credit_wait
 addi t0,t0,1
 sw t0,H_ISSUED(gp)
 mv a3,a1
 srli a2,a2,5
 li a1,0x58002
 lw t0,H_MC(gp)
 li t1,0x8001
 add t0,t0,t1
 SENDI t0,-2048
 ret
br_load_wait:
 slli t0,a0,2
 add t0,gp,t0
1: lw t1,80(t0)
 beqz t1,1b
 ret
/* Arm a LoadManual AC and issue the matching bulk request to the selected MC.
 * a0=MC-local source address, a1=SPM offset, a2=byte length. */
br_ac_start:
 li t0,BR_BASE
 add t0,t0,a1
 csrw 0x732,t0
 csrw 0x733,a2
 li t0,1
 csrw 0x730,t0
 srli t0,a2,5
 slli t0,t0,16
 li t1,0x8001
 or a1,t0,t1
 li a2,0
 li a3,0
 lw t0,H_MC(gp)
 li t1,0x8001
 add t0,t0,t1
 SENDI t0,-2031
 ret
br_weight_load:
 DINC H_WEIGHT_LOADS,t0
 andi a0,a0,-32
 j br_load_start
br_store_word:
.Lstore_credit_wait: lw t0,H_ISSUED(gp)
 lw t1,H_CONSUMED(gp)
 sub t1,t0,t1
 sltiu t1,t1,12
 beqz t1,.Lstore_credit_wait
 addi t0,t0,1
 sw t0,H_ISSUED(gp)
 mv a3,a1
 li a1,0x28003
 li a2,0x8000
 lw t0,H_MC(gp)
 li t1,0x8001
 add t0,t0,t1
 SENDI t0,1025
 ret
br_drain:
1: lw t0,H_ISSUED(gp)
 lw t1,H_CONSUMED(gp)
 bne t0,t1,1b
 ret
br_continue:
 SENDI zero,8
 j br_idle
.globl br_halt
br_halt:
 li t0,0x40000000
 sw a0,0(t0)
1: wfi
 j 1b
/* update(a0=weight,a1=coefficient,a2=potentiation) -> updated/clipped a0. */
#ifdef BR_ASM_SELFTEST
.globl br_update
#endif
br_update:
 lw t0,H_RULE(gp)
 bnez t0,2f
 beqz a2,1f
 fadd.s a0,a0,a1
 j .Lclip_additive
1: fsub.s a0,a0,a1
.Lclip_additive:
 lw t0,H_WMAX(gp)
 fmax.s a0,a0,zero
 fmin.s a0,a0,t0
 ret
2: bnez a2,3f
 fmul.s a1,a1,a0
 fsub.s a0,a0,a1
 fmax.s a0,a0,zero
 ret
3: /* br_pow04 clobbers only a0,t0-t2,t4-t5. Keep the weight and link
    * in its unused argument registers instead of creating a stack frame. */
 mv a2,ra
 mv a3,a0
 call br_pow04
 fmul.s a0,a0,a1
 fadd.s a0,a0,a3
 fmax.s a0,a0,zero
 jr a2
/* One coalesced service event; only SMT0 owns ring indices and allocation.
 * SMT1 only changes an allocated descriptor from loading(1) to ready(2).
 * A full-pool allocator can retire the head itself, so it never waits for
 * its own lower-priority service event. Only fired-post scatter allocates
 * these descriptors; post-quiet updates are written at the receiving PU.
 * Private ABI: a4-a7 hold the head, descriptor, weight, and return link.
 * All callees preserve those registers and t6 (the allocator's return link).
 */
br_mshr_retire:
 lw t0,H_MSHR_COUNT(gp)
 beqz t0,9f
 lw a4,H_MSHR_HEAD(gp)
 slli a5,a4,6
 li t0,MSHR
 add a5,a5,t0
 add a5,a5,gp
 lw t0,44(a5)
 li t1,2
 bne t0,t1,9f
 mv a7,ra
 lw t0,32(a5)
 andi t0,t0,28
 add t0,t0,a5
 lw a6,0(t0)
 lw a1,36(a5)
 beqz a1,1f
 mv a0,a6
 li a2,0
 call br_update
 mv a6,a0
1: lw a1,40(a5)
 beqz a1,2f
 mv a0,a6
 li a2,1
 call br_update
 mv a6,a0
2: lw a0,32(a5)
 mv a1,a6
 call br_store_word
 DINC H_WRITE_WORDS,t0
 DINC H_MSHR_COMMITS,t0
 sw zero,44(a5)
 addi a4,a4,1
 andi a4,a4,15
 sw a4,H_MSHR_HEAD(gp)
 lw t0,H_MSHR_COUNT(gp)
 addi t0,t0,-1
 sw t0,H_MSHR_COUNT(gp)
 jr a7
9: ret
.globl br_mshr_service
br_mshr_service:
 call br_mshr_retire
 lw t0,H_MSHR_COUNT(gp)
 beqz t0,1f
 SENDI zero,6
 j br_idle
1: sw zero,H_MSHR_SCHEDULED(gp)
 j br_idle
/* a0=address, a1=LTD coefficient, a2=LTP coefficient. */
br_mshr_allocate:
 /* Called with jal t6 by br_reverse. Normal allocation keeps the update in
  * a4-a6 across br_line_start, which also preserves t6. Only the full-pool
  * path spills those inputs around retirement; s0-s11 remain untouched. */
 mv a4,a0
 mv a5,a1
 mv a6,a2
 lw t0,H_MSHR_COUNT(gp)
 li t1,BR_MSHR_LIMIT
 bltu t0,t1,2f
 addi sp,sp,-16
 sw a4,0(sp)
 sw a5,4(sp)
 sw a6,8(sp)
 DINC H_POOL_FULL,t0
 lw t0,H_MSHR_HEAD(gp)
 slli t0,t0,6
 li t1,MSHR
 add t0,t0,t1
 add t0,t0,gp
 lw t0,44(t0)
 li t1,2
 beq t0,t1,1f
 DINC H_MSHR_WAITS,t0
1: call br_mshr_retire
 lw t0,H_MSHR_COUNT(gp)
 li t1,BR_MSHR_LIMIT
 bgeu t0,t1,1b
 lw a4,0(sp)
 lw a5,4(sp)
 lw a6,8(sp)
 addi sp,sp,16
2: lw t0,H_MSHR_TAIL(gp)
 slli t2,t0,6
 li t1,MSHR
 add t2,t2,t1
 add t2,t2,gp
 addi t0,t0,1
 andi t0,t0,15
 sw t0,H_MSHR_TAIL(gp)
 sw a4,32(t2)
 sw a5,36(t2)
 sw a6,40(t2)
 li t0,1
 sw t0,44(t2)
 DINC H_MSHR_LOADS,t0
#if BR_DIAGNOSTIC
 sw t0,48(t2)
#endif
 lw t0,H_MSHR_COUNT(gp)
 addi t0,t0,1
 sw t0,H_MSHR_COUNT(gp)
#if BR_DIAGNOSTIC
 lw t1,H_MSHR_PEAK(gp)
 bgeu t1,t0,3f
 sw t0,H_MSHR_PEAK(gp)
#endif
3: andi a0,a4,-32
 sub a1,t2,gp
 mv a2,a1
 DINC H_WEIGHT_LOADS,t0
 call br_line_start
 lw t0,H_MSHR_SCHEDULED(gp)
 bnez t0,4f
 li t0,1
 sw t0,H_MSHR_SCHEDULED(gp)
 SENDI zero,6
4: jr t6
.globl br_reverse
br_reverse:
 li t0,80
 mul t0,a0,t0
 add t0,gp,t0
 addi t0,t0,256
 lw t1,N_PRE_FIRE(t0)
 bnez t1,1f
 li a2,0
1: lw t2,N_X(t0)
 fmul.s a3,a3,t2
 or t0,a2,a3
 slli t0,t0,1
 beqz t0,8f
 /* The forward buffer is gone.  Every post-active update uses an MSHR
  * line load, then the normal ordered scalar writeback path. */
 mv a0,a1
 mv a1,a2
 mv a2,a3
 jal t6,br_mshr_allocate
8: INC H_REVERSE_RECEIVED,t0
 DINC H_REVERSE_EVENTS,t0
 j br_idle
/* Range-reduced w^0.4, identical FP32 operation ordering to the C polynomial. */
.macro HORNER bits
 li t2,\bits
 fmul.s a0,a0,t1
 fadd.s a0,a0,t2
.endm
#ifdef BR_ASM_SELFTEST
.globl br_pow04
#endif
br_pow04:
 slli t0,a0,1
 beqz t0,9f
 srli t4,a0,23
 bnez t4,1f
 li t0,0x71800000
 fmul.s a0,a0,t0
 srli t4,a0,23
 addi t4,t4,-227
 j 2f
1: addi t4,t4,-127
2: li t0,0x7fffff
 and t0,a0,t0
 li t2,0x3f800000
 or t0,t0,t2
 li t2,0x3fc00000
 fsub.s t1,t0,t2
 li t2,0x3f2aaaab
 fmul.s t1,t1,t2
 li a0,0xbc784faa
 HORNER 0x3c967ddb
 HORNER 0xbcbc1d52
 HORNER 0x3cf55de6
 HORNER 0xbd2a64c3
 HORNER 0x3d83126f
 HORNER 0xbdf5c28f
 HORNER 0x3ecccccd
 HORNER 0x3f800000
 li t2,0x3f9689c2
 fmul.s a0,a0,t2
 li t5,0
3: bgez t4,4f
 addi t4,t4,5
 addi t5,t5,-2
 j 3b
4: li t0,5
 blt t4,t0,5f
 addi t4,t4,-5
 addi t5,t5,2
 j 4b
5: li t2,0x3f800000
 beqz t4,8f
 li t0,1
 li t2,0x3fa8e5a3
 beq t4,t0,8f
 li t0,2
 li t2,0x3fdedc67
 beq t4,t0,8f
 li t0,3
 li t2,0x4013088c
 beq t4,t0,8f
 li t2,0x40420300
8: addi t5,t5,127
 slli t5,t5,23
 fmul.s a0,a0,t2
 fmul.s a0,a0,t5
9: ret
/* Both SMT workers claim a neuron address with AMOADD. Constants and one
 * xorshift state stay in registers for the complete invocation. tp is the SMT
 * identity set at boot; no worker frame or per-neuron RNG spill is needed. */
.globl br_integrate
br_integrate:
 slli t0,tp,2
 add t0,t0,gp
 lw s11,H_RNG0(t0)
 li s0,80
 lw s1,H_NN(gp)
 mul s1,s1,s0
 add s1,s1,gp
 addi s1,s1,256
 lw s2,H_P11(gp)
 lw s3,H_P21(gp)
 lw s4,H_P31(gp)
 lw s5,H_P32(gp)
 lw s6,H_P33(gp)
 lw s7,H_EPSC(gp)
 lw t0,H_EXTERNAL_WEIGHT(gp)
 fmul.s s7,s7,t0
 lw s8,H_DECAY_PRE(gp)
 lw s9,H_DECAY_POST(gp)
 li s10,0x41a00000
 li t0,POISSON
 add t0,t0,gp
 lw a4,0(t0)
 lw a5,4(t0)
 lw a6,8(t0)
.Lintegrate_claim:
 addi t5,gp,H_WORK_NEURON
 amoadd.w t5,s0,(t5)
 bgeu t5,s1,.Lintegrate_done
 lw t0,N_DE(t5)
 lw t1,N_IE(t5)
 lw t2,N_DI(t5)
 lw t3,N_II(t5)
 lw t4,N_V(t5)
 lw t6,N_REFRAC(t5)
 beqz t6,.Lintegrate_voltage
 addi t6,t6,-1
 j .Lintegrate_currents
.Lintegrate_voltage:
 fadd.s a0,t0,t2
 fmul.s a0,s4,a0
 fadd.s a1,t1,t3
 fmul.s a1,s5,a1
 fadd.s a0,a0,a1
 fmul.s t4,s6,t4
 fadd.s t4,t4,a0
.Lintegrate_currents:
 fmul.s a0,s3,t0
 fmul.s t1,s2,t1
 fadd.s t1,t1,a0
 fmul.s a0,s3,t2
 fmul.s t3,s2,t3
 fadd.s t3,t3,a0
 sw t1,N_IE(t5)
 sw t3,N_II(t5)
 lw a0,N_INPUT_E(t5)
 lw a1,N_INPUT_I(t5)
 fmul.s t0,s2,t0
 fmul.s t2,s2,t2
 fadd.s t0,t0,a0
 fadd.s t2,t2,a1
 slli t1,s11,13
 xor s11,s11,t1
 srli t1,s11,17
 xor s11,s11,t1
 slli t1,s11,5
 xor s11,s11,t1
 li a0,0
 bltu s11,a4,.Lintegrate_poisson_done
 li a0,1
 bltu s11,a5,.Lintegrate_poisson_done
 li a0,2
 bltu s11,a6,.Lintegrate_poisson_done
 /* Rare tail: thresholds are generated from this rule's exact mean. */
 li a0,3
 li t1,POISSON+12
 add t1,t1,gp
.Lintegrate_poisson_tail:
 lw t3,0(t1)
 bltu s11,t3,.Lintegrate_poisson_done
 addi a0,a0,1
 addi t1,t1,4
 li t3,9
 bltu a0,t3,.Lintegrate_poisson_tail
.Lintegrate_poisson_done:
 fcvt.s.wu a0,a0
 fmul.s a0,s7,a0
 fadd.s t0,t0,a0
 sw t0,N_DE(t5)
 sw t2,N_DI(t5)
 sw zero,N_INPUT_E(t5)
 sw zero,N_INPUT_I(t5)
 li a0,0
 bnez t6,.Lintegrate_no_fire
 bltz t4,.Lintegrate_no_fire
 bltu t4,s10,.Lintegrate_no_fire
 li a0,1
 li t6,5
 li t4,0
.Lintegrate_no_fire:
 sw a0,N_FIRE(t5)
 sw t6,N_REFRAC(t5)
 sw t4,N_V(t5)
 lw t0,N_X(t5)
 lw t1,N_Y(t5)
 fmul.s t0,t0,s8
 fmul.s t1,t1,s9
 sw t0,N_X(t5)
 sw t1,N_Y(t5)
 j .Lintegrate_claim
.Lintegrate_done:
 slli t0,tp,2
 add t0,t0,gp
 sw s11,H_RNG0(t0)
 li t0,1
 addi t1,gp,H_JOIN_COUNT
 amoadd.w t2,t0,(t1)
 bne t2,t0,.Lintegrate_return
 li a0,1
 li a1,0
 li a2,0
 lw a3,H_PU(gp)
 SENDI zero,3
.Lintegrate_return:
 j br_idle
/* Direct event handler.  Input a0=input offset,a1=weight,a2=weight address
 * (zero for static), a3=prePU | preLocal<<16.  A quiet postsynaptic target
 * applies LTD locally and sends a no-receipt scalar store to the source MC. */
.globl br_spike
br_spike:
 add t0,gp,a0
.Lspike_accumulate:
 lr.w t1,(t0)
 fadd.s t1,t1,a1
 sc.w t2,t1,(t0)
 bnez t2,.Lspike_accumulate
 beqz a2,.Lspike_done
 /* Reuse the accumulator pointer to address the other neuron fields. */
 lw t1,N_FIRE-N_INPUT_E(t0)
 bnez t1,.Lspike_done       /* post-active path handles LTD and LTP */
 lw t1,N_Y-N_INPUT_E(t0)
 lw t2,H_DEP(gp)
 fmul.s t1,t1,t2
 lw t2,H_RULE(gp)
 bnez t2,.Lspike_morrison
 fsub.s a1,a1,t1
 lw t2,H_WMAX(gp)
 fmax.s a1,a1,zero
 fmin.s a1,a1,t2
 j .Lspike_writeback
.Lspike_morrison:
 fmul.s t2,a1,t1
 fsub.s a1,a1,t2
 fmax.s a1,a1,zero
.Lspike_writeback:
 addi t0,a3,-1
 srli t0,t0,5
 andi t0,t0,15            # 512-PU/16-MC ABI; discard preLocal bits
 li t1,0x8001
 add t0,t0,t1
 mv a0,a2
 mv a3,a1
 li a1,0x20000
 li a2,0
 SENDI t0,1026
.Lspike_done:
 /* Both SMT threads may deliver spikes; preserve the barrier count atomically. */
 addi t0,gp,H_RECEIVED
 li t1,1
 amoadd.w zero,t1,(t0)
 j br_idle
.globl br_control
br_control:
 sw zero,H_REPORT_COUNT(gp)
 sw zero,H_REPORT_SUM(gp)
 sw zero,H_REPORT_MCS(gp)
 lw t2,H_PU(gp)
 lw t3,H_NUM_PU(gp)
 slli t0,t2,1
 li a2,0
 li a3,0
 bgtu t0,t3,1f
 SENDI t0,2
 addi t0,t0,1
 bgtu t0,t3,1f
 SENDI t0,2
1: li t0,4
 beq a0,t0,2f
 li t0,5
 beq a0,t0,3f
 sw a0,H_PHASE(gp)
 sw zero,H_JOIN_COUNT(gp)
5: sw a1,H_ACTIVE_MCS(gp)
 sw zero,H_WORK_NEURON(gp)
 sw zero,H_WORK_LINE(gp)
 sw zero,H_WORK_MC(gp)
 sw zero,H_SENT(gp)
 /* Phase 1 is integration.  Phase 2 is forward delivery and static CSR
  * scatter.  Phase 3 remains post-active reverse learning. */
 li t0,1
 bne a0,t0,6f
 addi t0,gp,256
 sw t0,H_WORK_NEURON(gp)
 SENDI zero,9
 SENDI zero,9
 j br_idle
6: li t0,2
 bne a0,t0,7f
 /* Phase 2 reconstructs its mask while processing fired rows. */
 sw zero,H_ACTIVE_MCS(gp)
7: j br_continue
2: lw a1,H_RECEIVED(gp)
 j 4f
3: lw a1,H_REVERSE_RECEIVED(gp)
4: li a2,0
 lw a3,H_PU(gp)
 SENDI zero,3
 j br_idle
/* Root-only broadcast and the simulator's phase-marker ABI. */
br_broadcast:
 mv a4,a0
 mv a5,a1
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
2: sw zero,H_REPORT_COUNT(gp)
 sw zero,H_REPORT_SUM(gp)
 sw zero,H_REPORT_MCS(gp)
 mv a0,a4
 mv a1,a5
 li a2,0
 li a3,0
 SENDI zero,2
 j br_idle
.globl br_report
br_report:
 lw t0,H_REPORT_COUNT(gp)
 lw t1,H_REPORT_SUM(gp)
 lw t2,H_REPORT_MCS(gp)
 addi t0,t0,1
 add t1,t1,a1
 or t2,t2,a2
 sw t0,H_REPORT_COUNT(gp)
 sw t1,H_REPORT_SUM(gp)
 sw t2,H_REPORT_MCS(gp)
 lw t3,H_PU(gp)
 li t4,TREE_REPORTS
 add t4,t4,gp
 lw t6,0(t4)
 bne t0,t6,9f
 li t0,1
 beq t3,t0,.Lroot_report
 srli t0,t3,1
 mv a1,t1
 mv a2,t2
 mv a3,t3
 SENDI t0,3
9: j br_idle
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
 li a1,0
 j br_broadcast
.Lreport_integrate:
 /* The integration barrier advances directly to phase 2 forward work. */
 li a0,2
 lw a1,H_ACTIVE_MCS(gp)
 j br_broadcast
.Lreport_forward:
 lw t0,H_EXPECTED_FORWARD(gp)
 add t0,t0,t1
 sw t0,H_EXPECTED_FORWARD(gp)
 sw t2,H_ACTIVE_MCS(gp)
 li a0,4
 mv a1,t2
 j br_broadcast
.Lquery_forward:
 lw t0,H_EXPECTED_FORWARD(gp)
 bgtu t1,t0,.Lreport_bad
 li a0,4
 bne t0,t1,1f
 li a0,3
1: lw a1,H_ACTIVE_MCS(gp)
 j br_broadcast
.Lnext_reverse:
 li a0,3
 lw a1,H_ACTIVE_MCS(gp)
 j br_broadcast
.Lreport_reverse:
 lw t0,H_EXPECTED_REVERSE(gp)
 add t0,t0,t1
 sw t0,H_EXPECTED_REVERSE(gp)
 li a0,5
 lw a1,H_ACTIVE_MCS(gp)
 j br_broadcast
.Lquery_reverse:
 lw t0,H_EXPECTED_REVERSE(gp)
 bgtu t1,t0,.Lreport_bad
 li a0,5
 bne t0,t1,1f
 li a0,6
1: lw a1,H_ACTIVE_MCS(gp)
 j br_broadcast
.Lreport_bad:
 li a0,0xb009
 j br_halt
.globl br_boot
br_boot:
 ENTER 3
 li t0,0x3f00
 add t0,t0,gp
 li t1,0x19ca7ead
 sw t1,0(t0)
 sw t1,128(t0)
 lw t0,H_MAGIC(gp)
 li t1,0x42524e34
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
 li t0,TREE_REPORTS
 add t0,t0,gp
 sw t3,0(t0)
 li t0,REPLAY
 add t0,t0,gp
 lw t0,28(t0)
 beqz t0,.Lboot_bad
 li t1,256
 bgtu t0,t1,.Lboot_bad
 lw t0,H_PU(gp)
 li t1,1
 bne t0,t1,.Lboot_report
#ifdef BR_ASM_SELFTEST
 call br_asm_selftest
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
 call br_store_word
 addi s0,s0,1
 li t0,32
 bltu s0,t0,.Lboot_store
 call br_drain
 li s0,0
.Lboot_load:
 slli a0,s0,5
 li t0,0x8000
 add a0,a0,t0
 mv a1,s0
 call br_load_start
 addi s0,s0,1
 li t0,4
 bltu s0,t0,.Lboot_load
 li s0,0
.Lboot_verify:
 mv a0,s0
 call br_load_wait
 slli s2,s0,5
 li t0,READ
 add s2,s2,t0
 add s2,s2,gp
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
 call br_drain
 sw zero,H_ISSUED(gp)
 sw zero,H_CONSUMED(gp)
.Lboot_report:
 addi t0,gp,256
 lw t1,N_RNG(t0)
 sw t1,H_RNG0(gp)
 addi t0,t0,80
 lw t1,N_RNG(t0)
 sw t1,H_RNG1(gp)
 li a0,0
 li a1,0
 li a2,0
 lw a3,H_PU(gp)
 li t0,0
 li t1,3
 SEND t0,t1
 LEAVE 3
.Lboot_bad:
 li a0,0xb00b
 j br_halt
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
 li t0,2
 bne s0,t0,.Lwork_skip_reverse
 lw t0,H_WORK_LINE(gp)
 bnez t0,.Lwork_select
 /* Skip inactive forward rows and clear their pre_fire flags. */
 lw t2,H_NN(gp)
 li t0,80
 mul s2,s1,t0
 add s2,s2,gp
 addi s2,s2,256
1: bgeu s1,t2,2f
 lw t0,36(s2)
 bnez t0,2f
 sw zero,40(s2)
 addi s1,s1,1
 addi s2,s2,80
 j 1b
2: sw s1,H_WORK_NEURON(gp)
 bgeu s1,t2,.Lwork_report
 j .Lwork_neuron_ready
.Lwork_skip_reverse:
 /* No traffic is generated for an inactive row. Scan those rows together
  * instead of scheduling a separate work event for every local neuron. */
 lw t2,H_NN(gp)
 li t0,80
 mul s2,s1,t0
 add s2,s2,gp
 addi s2,s2,256
1: bgeu s1,t2,2f
 lw t0,N_FIRE(s2)
 bnez t0,2f
 addi s1,s1,1
 addi s2,s2,80
 j 1b
2: sw s1,H_WORK_NEURON(gp)
 bgeu s1,t2,.Lwork_report
 j .Lwork_neuron_ready
.Lwork_select:
 lw t0,H_NN(gp)
 bltu s1,t0,.Lwork_neuron
 j .Lwork_report
.Lwork_neuron:
 li t0,80
 mul s2,s1,t0
 add s2,s2,gp
 addi s2,s2,256
.Lwork_neuron_ready:
 li t0,2
 bne s0,t0,.Lwork_reverse_neuron
 /* Start a fired row once; later work events finish its static directories. */
 lw t0,H_WORK_LINE(gp)
 bnez t0,.Lwork_directory
 lw t0,N_FIRE(s2)
 sw t0,N_PRE_FIRE(s2)
 beqz t0,.Lwork_next_neuron
 /* Every local forward row belongs to this PU's MC.  Record that bit once
  * so the report handler does not need to rescan all neurons. */
 lw t1,H_MC(gp)
 li t2,1
 sll t1,t2,t1
 lw t2,H_ACTIVE_MCS(gp)
 or t2,t2,t1
 sw t2,H_ACTIVE_MCS(gp)
 lw s4,N_COUNT(s2)
 beqz s4,.Lwork_directory
 mv s7,s4
.Lforward_first:
 li s3,0                     # row synapse index at chunk start
 li s11,0                    # landing half, 0 or 1
 slli s10,s1,16
 lw t0,H_PU(gp)
 or s10,s10,t0
#if BR_DIAGNOSTIC
 li t0,STREAM
 add t0,t0,gp
 li t1,1
 sw t1,0(t0)
 lw t1,4(t0)
 addi t1,t1,1
 sw t1,4(t0)
#endif
 lw s8,N_WEIGHTS(s2)      # row base stays live across all chunk continuations
 mv a0,s8
 li a1,PING
 li a2,1024
 call br_ac_start
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
1: slli s5,s11,10
 li t0,PING
 add s5,s5,t0
 add s5,s5,gp
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
 li a2,1024
 call br_ac_start
 YIELDI 12
 j br_idle
.Lforward_last_chunk:
#if BR_DIAGNOSTIC
 li t0,STREAM
 add t0,t0,gp
 li t1,3
 sw t1,0(t0)
#endif
 YIELDI 12
 j br_idle
.globl br_forward_drain
br_forward_drain:
 bgeu s5,s9,.Lforward_chunk_done
 lw t0,512(s5)             # destination: high 16 input offset, low 16 PU
 lw a1,0(s5)               # already scaled FP32 weight
 srli a0,t0,16
 mv a2,s6
 mv a3,s10
 addi s5,s5,4
 addi s6,s6,4
 SENDI t0,-1023            # 0xc01; destination is truncated to 16 bits
 YIELDI 12                 # replenish quota and admit higher-priority events
 j br_idle
.Lforward_chunk_done:
 /* Sample AC readiness immediately at the drain boundary. */
#if BR_DIAGNOSTIC
 csrr t3,0x730
#endif
 lw t0,H_SENT(gp)
 add t0,t0,s4
 sw t0,H_SENT(gp)
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
 lw t0,44(s2)
 li t1,9000
 bgeu t0,t1,.Lwork_next_neuron
 lw t0,36(s2)
 beqz t0,.Lwork_next_neuron
 /* Reverse coefficients are constant for this row across all MC entries. */
 lw t1,H_WORK_MC(gp)
 bnez t1,.Lwork_directory
 lw s8,H_DEP(gp)
 lw t1,N_Y(s2)
 fmul.s s8,s8,t1
 lw s9,H_POT(gp)
br_work_directory:
.Lwork_directory:
 lw s6,H_WORK_MC(gp)
 li t0,2
 bne s0,t0,1f
 lw s7,60(s2)
 j 2f
1: lw s7,64(s2)
2: slli t0,s6,3
 add s7,s7,t0
.Ldirectory_resume:
 li t0,16
 bgeu s6,t0,.Lwork_next_neuron
 andi t0,s7,-32
 lw t1,H_DIRECTORY_LINE(gp)
 beq t0,t1,3f
 mv a0,t0
 li a1,2
 call br_load_start
 li a0,2
 call br_load_wait
 andi t0,s7,-32
 sw t0,H_DIRECTORY_LINE(gp)
3: andi t0,s7,31
 li t1,READ+64
 add t0,t0,t1
 add t0,t0,gp
 lw a0,0(t0)
 lw s5,4(t0)
 beqz s5,5f
 li a2,0
 li a3,0
 li t2,2
 beq s0,t2,.Lstatic_scatter_tag
 mv a2,s8
 mv a3,s9
 li t2,0xc04
 j 4f
.Lstatic_scatter_tag:
 li t2,0xc01
4: addi a1,s5,3
 srli a1,a1,2
 slli a1,a1,16
 or a1,a1,t2
 li t0,0x8001
 add t0,t0,s6
 SENDI t0,-1008
 lw t0,H_SENT(gp)
 add t0,t0,s5
 sw t0,H_SENT(gp)
5: addi s6,s6,1
 addi s7,s7,8
 sw s6,H_WORK_MC(gp)
 j .Lwork_continue
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
 call br_drain
 lw t0,H_MSHR_COUNT(gp)
 bnez t0,.Lboot_bad
 lw t0,H_MSHR_SCHEDULED(gp)
 bnez t0,.Lboot_bad
 li t0,0x3f00
 add t0,t0,gp
 li t1,0x19ca7ead
 lw t2,0(t0)
 bne t1,t2,.Lboot_bad
 lw t2,128(t0)
 bne t1,t2,.Lboot_bad
 addi s2,gp,256
 lw s1,H_NN(gp)
1: lw t0,36(s2)
 beqz t0,2f
 lw t1,24(s2)
 lw t2,28(s2)
 fcvt.s.wu t0,t0
 fadd.s t1,t1,t0
 fadd.s t2,t2,t0
 sw t1,24(s2)
 sw t2,28(s2)
2:
 addi s2,s2,80
 addi s1,s1,-1
 bnez s1,1b
 INC H_STEP,t0
 li a0,6
 li a1,0
 li a2,0
 j .Lwork_send_report
br_work_report:
.Lwork_report:
 li a2,0
 li t0,2
 bne s0,t0,3f
 lw a2,H_ACTIVE_MCS(gp)
3: mv a0,s0
 lw a1,H_SENT(gp)
.Lwork_send_report:
 lw a3,H_PU(gp)
 SENDI zero,3
 j .Lwork_return
br_work_continue:
.Lwork_continue:
 SENDI zero,8
.Lwork_return:
 j br_idle
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
