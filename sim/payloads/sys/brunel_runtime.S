/* Hand-written Brunel runtime, RV32I/Zmmul/Zfinx.
 * gp = immutable local SPM base on both SMT threads.
 * a0-a7,t0-t6 are caller-clobbered; s0-s11 follow the usual callee-save ABI.
 * Only SMT0 mutates the weight cache and issues credited memory requests.
 * SMT1 consumes completions and records spikes until the local buffer fills.
 * All cache keys are full MC-local addresses. Memory effects must be ordered.
 */
#include "brunel_asm_layout.inc"
.option norelax
.equ TABLE, 0x900
.equ READ, 0x2200
.equ REPLAY, 0x2400
.equ RECORD, 0x2800
.equ SLOTS, 800
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
/* Address/tag -> home index. State bits disappear in the word-address shift. */
.macro HASH out,addr,tmp
 srli \out,\addr,2
 li \tmp,2654435761
 mul \out,\out,\tmp
 srli \out,\out,16
 li \tmp,SLOTS
 mul \out,\out,\tmp
 srli \out,\out,16
.endm
.macro SEND dst,tag
 .insn r 0x0b,4,0,x0,\dst,\tag
.endm
.macro INC offset,tmp
 lw \tmp,\offset(gp)
 addi \tmp,\tmp,1
 sw \tmp,\offset(gp)
.endm
.text
/* Completion handler is a leaf so it can retire notifications quickly. */
.globl br_completion
br_completion:
 li t0,0x8000
 and t0,a0,t0
 bnez t0,1f
 li t0,3
 bgtu a1,t0,.Lbad_completion
 slli t0,a1,2
 add t0,gp,t0
 li t1,1
 sw t1,80(t0)
1: INC H_CONSUMED,t0
 ret
.Lbad_completion:
 li a0,0xb002
 j br_halt
/* These leaf transports do not impose address-specific completion waits. */
br_load_start:
 slli t0,a1,2
 add t0,gp,t0
 sw zero,80(t0)
1: lw t0,H_ISSUED(gp)
 lw t1,H_CONSUMED(gp)
 sub t1,t0,t1
 sltiu t1,t1,12
 beqz t1,1b
 addi t0,t0,1
 sw t0,H_ISSUED(gp)
 mv a3,a1
 addi a2,a1,(READ>>5)
 li a1,0x58002
 lw t0,H_MC(gp)
 li t1,0x8001
 add t0,t0,t1
 li t1,0x800
 SEND t0,t1
 ret
br_load_wait:
 slli t0,a0,2
 add t0,gp,t0
1: lw t1,80(t0)
 beqz t1,1b
 ret
br_weight_load:
 INC H_WEIGHT_LOADS,t0
 andi a0,a0,-32
 j br_load_start
br_store_word:
1: lw t0,H_ISSUED(gp)
 lw t1,H_CONSUMED(gp)
 sub t1,t0,t1
 sltiu t1,t1,12
 beqz t1,1b
 addi t0,t0,1
 sw t0,H_ISSUED(gp)
 mv a3,a1
 li a1,0x28003
 li a2,0x8000
 lw t0,H_MC(gp)
 li t1,0x8001
 add t0,t0,t1
 li t1,0x401
 SEND t0,t1
 ret
br_drain:
1: lw t0,H_ISSUED(gp)
 lw t1,H_CONSUMED(gp)
 bne t0,t1,1b
 ret
br_continue:
 li t0,0
 li t1,8
 SEND t0,t1
 ret
.globl br_halt
br_halt:
 li t0,0x40000000
 sw a0,0(t0)
1: wfi
 j 1b
/* lookup(a0=address) -> a0=entry or zero. Count all examined slots once. */
br_lookup:
 HASH t0,a0,t1
 slli t0,t0,3
 li t1,TABLE
 add t0,t0,t1
 add t0,t0,gp
 li t3,0
 li t4,READ
 add t4,t4,gp
1: lw t1,0(t0)
 addi t3,t3,1
 andi t1,t1,-4
 beq t1,a0,3f
 addi t0,t0,8
 bne t0,t4,2f
 li t0,TABLE
 add t0,t0,gp
2: li t2,4
 bltu t3,t2,1b
 li t0,0
3: lw t1,H_PROBES(gp)
 add t1,t1,t3
 mv a0,t0
 sw t1,H_PROBES(gp)
 ret
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
3: ENTER 2
 mv s0,a1
 mv s1,a0
 call br_pow04
 fmul.s a0,a0,s0
 fadd.s a0,a0,s1
 fmax.s a0,a0,zero
 LEAVE 2
/* Materialize exactly one pending coefficient, using refill slot zero. */
br_materialize:
 lw t0,0(a0)
 andi t1,t0,2
 beqz t1,9f
 ENTER 2
 mv s0,a0
 mv s1,t0
 andi a0,t0,-4
 li a1,0
 call br_weight_load
 li a0,0
 call br_load_wait
 andi t0,s1,28
 li t1,READ
 add t0,t0,t1
 add t0,t0,gp
 lw a0,0(t0)
 lw a1,4(s0)
 andi a2,s1,1
 call br_update
 andi t0,s1,-4
 ori t0,t0,1
 sw a0,4(s0)
 sw t0,0(s0)
 LEAVE 2
9: ret
br_evict:
 lw t0,0(a0)
 beqz t0,9f
 ENTER 1
 mv s0,a0
 INC H_EVICTIONS,t1
 andi t1,t0,2
 beqz t1,1f
 INC H_FORCED,t1
 call br_materialize
1: lw t0,0(s0)
 andi t1,t0,3
 beqz t1,8f
 andi a0,t0,-4
 lw a1,4(s0)
 call br_store_word
 INC H_WRITE_WORDS,t0
8: LEAVE 1
9: ret
/* Four probes total, including displacements. Scratch held entry at sp+40. */
br_insert:
 ENTER 8
 HASH s2,a0,t0
 slli s5,s2,3
 li t0,TABLE
 add s5,s5,t0
 add s5,s5,gp
 lw s0,0(s5)
 lw s1,4(s5)
 sw a0,0(s5)
 sw a1,4(s5)
 INC H_PROBES,t0
 beqz s0,.Linsert_done
 HASH t0,s0,t1
 sub s3,s2,t0
 bgez s3,1f
 addi s3,s3,SLOTS
1: li s4,3
.Linsert_probe:
 addi s3,s3,1
 li t0,4
 beq s3,t0,.Linsert_evict
 HASH s2,s0,t0
 add s2,s2,s3
 li t0,SLOTS
 bltu s2,t0,2f
 addi s2,s2,-SLOTS
2: slli s5,s2,3
 li t0,TABLE
 add s5,s5,t0
 add s5,s5,gp
 INC H_PROBES,t0
 lw s6,0(s5)
 lw s7,4(s5)
 beqz s6,.Linsert_empty
 HASH t0,s6,t1
 sub t0,s2,t0
 bgez t0,3f
 addi t0,t0,SLOTS
3: bgtu t0,s3,4f
 sw s0,0(s5)
 sw s1,4(s5)
 mv s0,s6
 mv s1,s7
 mv s3,t0
4: addi s4,s4,-1
 bnez s4,.Linsert_probe
.Linsert_evict:
 sw s0,40(sp)
 sw s1,44(sp)
 addi a0,sp,40
 call br_evict
 j .Linsert_done
.Linsert_empty:
 sw s0,0(s5)
 sw s1,4(s5)
.Linsert_done:
 LEAVE 8
/* apply(address,coefficient,potentiation). A missing key stores one coefficient. */
br_apply:
 slli t0,a1,1
 beqz t0,9f
 ENTER 4
 mv s0,a0
 mv s1,a1
 mv s2,a2
 call br_lookup
 mv s3,a0
 beqz a0,3f
 lw t0,0(a0)
 andi t0,t0,2
 bnez t0,2f
 INC H_HITS,t0
 lw a0,4(s3)
 mv a1,s1
 mv a2,s2
 call br_update
 sw a0,4(s3)
 ori t0,s0,1
 sw t0,0(s3)
 j 8f
2: INC H_MISSES,t0
 INC H_FORCED,t0
 call br_materialize
 lw a0,4(s3)
 mv a1,s1
 mv a2,s2
 call br_update
 sw a0,4(s3)
 j 8f
3: INC H_MISSES,t0
 addi t0,s2,2
 or a0,s0,t0
 mv a1,s1
 call br_insert
 INC H_PENDING,t0
8: LEAVE 4
9: ret
.globl br_reverse
br_reverse:
 ENTER 4
 li t0,80
 mul t0,a0,t0
 add s0,gp,t0
 addi s0,s0,256
 mv s1,a1
 mv s2,a2
 mv s3,a3
 lw t0,N_PRE_FIRE(s0)
 beqz t0,1f
 mv a0,s1
 mv a1,s2
 li a2,0
 call br_apply
1: beqz s3,2f
 lw t0,N_X(s0)
 mv a0,s1
 fmul.s a1,s3,t0
 li a2,1
 call br_apply
2: INC H_REVERSE_RECEIVED,t0
 INC H_REVERSE_EVENTS,t0
 LEAVE 4
/* A single 32B fill feeds eight independently tagged slots. Resolve every
 * pending lane before admitting any absent lane, including parallel edges. */
br_forward_weights:
 ENTER 8
 addi sp,sp,-64
 mv s0,a0
 mv s1,a1
 mv s2,a2
 li s3,0
 li s4,0
 li s5,0
.Lstage_lane:
 slli t0,s5,2
 add a0,s0,t0
 call br_lookup
 slli t0,s5,3
 add s6,sp,t0
 beqz a0,1f
 lw t0,0(a0)
 lw t1,4(a0)
 sw t0,0(s6)
 sw t1,4(s6)
 andi t0,t0,2
 bnez t0,2f
 INC H_HITS,t0
 j 3f
1: sw zero,0(s6)
 li t0,1
 sll t0,t0,s5
 or s4,s4,t0
2: li s3,1
 INC H_MISSES,t0
3: addi s5,s5,1
 bltu s5,s1,.Lstage_lane
 beqz s3,.Lresolve_begin
 mv a0,s0
 li a1,3
 call br_weight_load
 li a0,3
 call br_load_wait
.Lresolve_begin:
 li s5,0
.Lresolve_lane:
 slli t0,s5,3
 add s6,sp,t0
 lw t0,0(s6)
 bnez t0,1f
 slli t0,s5,2
 add t1,s0,t0
 sw t1,0(s6)
 li t1,READ+96
 add t1,t1,gp
 add t1,t1,t0
 lw t0,0(t1)
 sw t0,4(s6)
 j 3f
1: andi t1,t0,2
 beqz t1,3f
 andi a2,t0,1
 lw a1,4(s6)
 slli t0,s5,2
 li t1,READ+96
 add t1,t1,gp
 add t1,t1,t0
 lw a0,0(t1)
 call br_update
 sw a0,4(s6)
 slli t0,s5,2
 add a0,s0,t0
 ori t1,a0,1
 sw t1,0(s6)
 call br_lookup
 lw t0,0(s6)
 lw t1,4(s6)
 sw t0,0(a0)
 sw t1,4(a0)
3: lw t0,4(s6)
 slli t1,s5,2
 add t1,t1,s2
 sw t0,0(t1)
 addi s5,s5,1
 bltu s5,s1,.Lresolve_lane
 li s5,0
.Ladmit_lane:
 andi t0,s4,1
 srli s4,s4,1
 beqz t0,1f
 slli t0,s5,3
 add t0,t0,sp
 lw a0,0(t0)
 lw a1,4(t0)
 call br_insert
1: addi s5,s5,1
 bltu s5,s1,.Ladmit_lane
 addi sp,sp,64
 LEAVE 8
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
/* Poisson(a0=rng) -> a0=count, a1=updated rng. Positive float bit ordering. */
br_poisson:
 mv t0,a0
 li t1,0x3f800000
 li t2,0
 li a2,0x33800000
 lw a3,H_EXTERNAL_LIMIT(gp)
1: slli t3,t0,13
 xor t0,t0,t3
 srli t3,t0,17
 xor t0,t0,t3
 slli t3,t0,5
 xor t0,t0,t3
 srli t3,t0,8
 addi t3,t3,1
 fcvt.s.wu t3,t3
 fmul.s t3,t3,a2
 addi t2,t2,1
 fmul.s t1,t1,t3
 bgtu t1,a3,1b
 addi a0,t2,-1
 mv a1,t0
 ret
/* Integration hoists immutable constants across all local neurons. */
br_integrate:
 ENTER 10
 addi s0,gp,256
 lw s1,H_NN(gp)
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
.Lintegrate_neuron:
 lw t0,N_REFRAC(s0)
 beqz t0,1f
 addi t0,t0,-1
 sw t0,N_REFRAC(s0)
 j 2f
1: lw t0,N_DE(s0)
 lw t1,N_IE(s0)
 lw t2,N_DI(s0)
 lw t3,N_II(s0)
 lw t4,N_V(s0)
 fmul.s a0,s4,t0
 fmul.s a1,s5,t1
 fmul.s a2,s4,t2
 fadd.s a0,a0,a1
 fmul.s a3,s5,t3
 fadd.s a0,a0,a2
 fmul.s a4,s6,t4
 fadd.s a0,a0,a3
 fadd.s a0,a0,a4
 sw a0,N_V(s0)
2: lw t0,N_DE(s0)
 lw t1,N_IE(s0)
 lw t2,N_DI(s0)
 lw t3,N_II(s0)
 fmul.s a0,s3,t0
 fmul.s a1,s2,t1
 fmul.s a2,s3,t2
 fmul.s a3,s2,t3
 fadd.s a0,a0,a1
 fadd.s a2,a2,a3
 sw a0,N_IE(s0)
 sw a2,N_II(s0)
 lw t1,N_INPUT_E(s0)
 lw t3,N_INPUT_I(s0)
 fmul.s t0,s2,t0
 fmul.s t2,s2,t2
 fadd.s t0,t0,t1
 fadd.s t2,t2,t3
 sw t0,N_DE(s0)
 sw t2,N_DI(s0)
 sw zero,N_INPUT_E(s0)
 sw zero,N_INPUT_I(s0)
 lw a0,N_RNG(s0)
 call br_poisson
 sw a1,N_RNG(s0)
 fcvt.s.wu a0,a0
 lw t0,N_DE(s0)
 fmul.s a0,s7,a0
 fadd.s t0,t0,a0
 sw t0,N_DE(s0)
 lw t0,N_REFRAC(s0)
 lw t1,N_V(s0)
 li t2,0
 bnez t0,3f
 bltz t1,3f
 li t0,0x41a00000
 bltu t1,t0,3f
 li t2,1
 li t0,5
 sw t0,N_REFRAC(s0)
 sw zero,N_V(s0)
3: sw t2,N_FIRE(s0)
 lw t0,N_X(s0)
 lw t1,N_Y(s0)
 fmul.s t0,t0,s8
 fmul.s t1,t1,s9
 sw t0,N_X(s0)
 sw t1,N_Y(s0)
 addi s0,s0,80
 addi s1,s1,-1
 bnez s1,.Lintegrate_neuron
 LEAVE 10
/* Input a0=input offset,a1=weight,a2=weight address (zero for static),
 * a3=prePU | preLocal<<16. Recording uses full addresses, no CSR search. */
.globl br_spike
br_spike:
 add t0,gp,a0
 lw t1,0(t0)
 fadd.s t1,t1,a1
 sw t1,0(t0)
 beqz a2,.Lspike_received
 li t2,REPLAY
 add t2,t2,gp
 lw t3,0(t2)
 lw t4,28(t2)
 addi t0,a0,-256
 li t1,0xcccccccd
 mulhu t0,t0,t1
 srli t0,t0,6
 slli t0,t0,24
 or a3,a3,t0
 bgeu t3,t4,.Lspill_spike
 slli t0,t3,3
 li t1,RECORD
 add t0,t0,t1
 add t0,t0,gp
 sw a2,0(t0)
 sw a3,4(t0)
 addi t3,t3,1
 sw t3,0(t2)
 lw t0,4(t2)
 bgeu t0,t3,1f
 sw t3,4(t2)
1: INC H_RECEIVED,t0
 bne t3,t4,2f
 li t0,12
 csrw 0x711,t0
2: ret
.Lspike_received:
 INC H_RECEIVED,t0
 ret
.Lspill_spike:
 li t0,8100
 bgeu t3,t0,.Lspill_bad
 ENTER 5
 mv s0,t2
 mv s1,t3
 mv s2,a3
 mv s3,a2
 sub t0,t3,t4
 slli t0,t0,3
 lw s4,12(s0)
 add s4,s4,t0
 mv a0,s4
 mv a1,s3
 call br_store_word
 addi a0,s4,4
 mv a1,s2
 call br_store_word
 lw t0,16(s0)
 addi t0,t0,1
 sw t0,16(s0)
 addi s1,s1,1
 sw s1,0(s0)
 lw t0,4(s0)
 bgeu t0,s1,1f
 sw s1,4(s0)
1: INC H_RECEIVED,t0
 LEAVE 5
.Lspill_bad:
 li a0,0xb00c
 j br_halt
.globl br_control
br_control:
 sw zero,H_REPORT_COUNT(gp)
 sw zero,H_REPORT_SUM(gp)
 sw zero,H_REPORT_MCS(gp)
 lw t2,H_PU(gp)
 lw t3,H_NUM_PU(gp)
 slli t0,t2,1
 li t1,2
 li a2,0
 li a3,0
 bgtu t0,t3,1f
 SEND t0,t1
 addi t0,t0,1
 bgtu t0,t3,1f
 SEND t0,t1
1: li t0,4
 beq a0,t0,2f
 li t0,5
 beq a0,t0,3f
 sw a0,H_PHASE(gp)
 sw a1,H_ACTIVE_MCS(gp)
 sw zero,H_WORK_NEURON(gp)
 sw zero,H_WORK_LINE(gp)
 sw zero,H_WORK_MC(gp)
 sw zero,H_SENT(gp)
 j br_continue
2: lw a1,H_RECEIVED(gp)
 j 4f
3: lw a1,H_REVERSE_RECEIVED(gp)
4: li a2,0
 lw a3,H_PU(gp)
 li t0,0
 li t1,3
 SEND t0,t1
 ret
/* Root-only broadcast and the frozen simulator's existing phase-marker ABI. */
br_broadcast:
 ENTER 2
 mv s0,a0
 mv s1,a1
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
 li t1,10
 SEND t0,t1
2: sw zero,H_REPORT_COUNT(gp)
 sw zero,H_REPORT_SUM(gp)
 sw zero,H_REPORT_MCS(gp)
 mv a0,s0
 mv a1,s1
 li a2,0
 li a3,0
 li t0,0
 li t1,2
 SEND t0,t1
 LEAVE 2
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
 lw t4,H_NUM_PU(gp)
 slli t5,t3,1
 sltu t6,t4,t5
 xori t6,t6,1
 addi t6,t6,1
 addi t5,t5,1
 sltu t5,t4,t5
 xori t5,t5,1
 add t6,t6,t5
 bne t0,t6,9f
 li t0,1
 beq t3,t0,.Lroot_report
 srli t0,t3,1
 mv a1,t1
 mv a2,t2
 mv a3,t3
 li t1,3
 SEND t0,t1
9: ret
.Lroot_report:
 beqz a0,.Lnext_forward
 li t0,1
 beq a0,t0,.Lreport_forward
 li t0,2
 beq a0,t0,.Lnext_reverse
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
.Lnext_forward:
 li a0,1
 li a1,0
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
 li a0,2
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
 lw t0,H_MAGIC(gp)
 li t1,0x42524e32
 bne t0,t1,.Lboot_bad
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
.Lboot_report:
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
/* Cooperative work: bounded sends per invocation, cache mutation on SMT0. */
.globl br_work
br_work:
 ENTER 8
 lw s0,H_PHASE(gp)
 lw s1,H_WORK_NEURON(gp)
 li t0,2
 beq s0,t0,.Lwork_integrate
 li t0,6
 beq s0,t0,.Lwork_finish
 li t0,1
 bne s0,t0,.Lwork_select
 lw t0,H_WORK_LINE(gp)
 lw t1,H_WORK_MC(gp)
 or t0,t0,t1
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
.Lwork_select:
 lw t0,H_NN(gp)
 bltu s1,t0,.Lwork_neuron
 li t0,3
 bne s0,t0,.Lwork_report
 li s2,REPLAY
 add s2,s2,gp
 lw t0,8(s2)
 lw t1,0(s2)
 bltu t0,t1,.Lwork_replay
 j .Lwork_report
.Lwork_neuron:
 li t0,80
 mul s2,s1,t0
 add s2,s2,gp
 addi s2,s2,256
 li t0,1
 bne s0,t0,.Lwork_reverse_neuron
 lw t0,H_WORK_LINE(gp)
 lw t1,H_WORK_MC(gp)
 or t0,t0,t1
 bnez t0,1f
 lw t0,36(s2)
 sw t0,40(s2)
1: lw t0,40(s2)
 beqz t0,.Lwork_next_neuron
 lw s3,H_WORK_LINE(gp)
 lw t0,52(s2)
 bgeu s3,t0,.Lwork_directory
 sub s4,t0,s3
 li t0,8
 bleu s4,t0,2f
 li s4,8
2: lw t0,H_FORWARD_PREPARED(gp)
 bnez t0,.Lwork_send_segment
 lw s5,H_ISSUED(gp)
 lw a0,56(s2)
 slli t0,s3,2
 add a0,a0,t0
 li a1,1
 call br_load_start
 lw a0,48(s2)
 slli t0,s3,2
 add a0,a0,t0
 mv a1,s4
 li a2,READ+96
 add a2,a2,gp
 call br_forward_weights
 li a0,1
 call br_load_wait
 lw t0,H_ISSUED(gp)
 sub t0,t0,s5
 add t0,t0,s4
 addi t0,t0,1
 li t1,24
 bleu t0,t1,.Lwork_send_segment
 li t0,1
 sw t0,H_FORWARD_PREPARED(gp)
 j .Lwork_continue
.Lwork_send_segment:
 lw s7,H_EPSC(gp)
 lw t0,H_DELIVERY(gp)
 fmul.s s7,s7,t0
 lw a2,48(s2)
 slli t0,s3,2
 add a2,a2,t0
 slli a3,s1,16
 lw t0,H_PU(gp)
 or a3,a3,t0
 li s5,0
 li s6,READ+32
 add s6,s6,gp
1: lw t2,0(s6)
 lw a1,64(s6)
 srli a0,t2,16
 slli t0,t2,16
 srli t0,t0,16
 fmul.s a1,s7,a1
 li t1,1
 SEND t0,t1
 addi a2,a2,4
 addi s6,s6,4
 addi s5,s5,1
 bltu s5,s4,1b
 lw t0,H_SENT(gp)
 add t0,t0,s4
 sw t0,H_SENT(gp)
 sw zero,H_FORWARD_PREPARED(gp)
 addi s3,s3,8
 sw s3,H_WORK_LINE(gp)
 j .Lwork_continue
.Lwork_reverse_neuron:
 lw t0,44(s2)
 li t1,9000
 bgeu t0,t1,.Lwork_next_neuron
 lw t0,36(s2)
 beqz t0,.Lwork_next_neuron
.Lwork_directory:
 lw s6,H_WORK_MC(gp)
 li t0,16
 bgeu s6,t0,.Lwork_next_neuron
 li t0,1
 bne s0,t0,1f
 lw s7,60(s2)
 j 2f
1: lw s7,64(s2)
2: slli t0,s6,3
 add s7,s7,t0
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
 li t2,1
 beq s0,t2,4f
 lw t0,H_DEP(gp)
 lw t1,28(s2)
 fmul.s a2,t0,t1
 lw a3,H_POT(gp)
 li t2,4
 li t0,REPLAY
 add t0,t0,gp
 lw t1,24(t0)
 add t1,t1,s5
 sw t1,24(t0)
4: addi a1,s5,3
 srli a1,a1,2
 slli a1,a1,16
 or a1,a1,t2
 li t0,0x8001
 add t0,t0,s6
 li t1,0x810
 SEND t0,t1
 lw t0,H_SENT(gp)
 add t0,t0,s5
 sw t0,H_SENT(gp)
5: addi s6,s6,1
 sw s6,H_WORK_MC(gp)
 j .Lwork_continue
.Lwork_next_neuron:
 addi s1,s1,1
 sw s1,H_WORK_NEURON(gp)
 sw zero,H_WORK_LINE(gp)
 sw zero,H_WORK_MC(gp)
 j .Lwork_continue
.Lwork_integrate:
 call br_integrate
 li a0,2
 li a1,0
 li a2,0
 j .Lwork_send_report
.Lwork_finish:
 call br_drain
 addi s2,gp,256
 lw s1,H_NN(gp)
1: lw t0,24(s2)
 lw t1,40(s2)
 lw t2,28(s2)
 lw t3,36(s2)
 fcvt.s.wu t1,t1
 fcvt.s.wu t3,t3
 fadd.s t0,t0,t1
 fadd.s t2,t2,t3
 sw t0,24(s2)
 sw t2,28(s2)
 addi s2,s2,80
 addi s1,s1,-1
 bnez s1,1b
 li t0,REPLAY
 add t0,t0,gp
 sw zero,0(t0)
 sw zero,8(t0)
 sw zero,32(t0)
 li t0,20
 csrw 0x711,t0
 INC H_STEP,t0
 li a0,6
 li a1,0
 li a2,0
 j .Lwork_send_report
.Lwork_report:
 li a2,0
 li t0,1
 bne s0,t0,3f
 addi t0,gp,256
 lw t1,H_NN(gp)
 li t2,9000
1: lw t3,44(t0)
 bgeu t3,t2,2f
 lw t3,40(t0)
 beqz t3,2f
 lw t3,H_MC(gp)
 li a2,1
 sll a2,a2,t3
 j 3f
2: addi t0,t0,80
 addi t1,t1,-1
 bnez t1,1b
3: mv a0,s0
 lw a1,H_SENT(gp)
.Lwork_send_report:
 lw a3,H_PU(gp)
 li t0,0
 li t1,3
 SEND t0,t1
 j .Lwork_return
.Lwork_continue:
 call br_continue
.Lwork_return:
 LEAVE 8
/* s2=replay header, four records per invocation. Spill reads are acknowledged
 * before consumption; cached entries remain on SMT0 throughout replay. */
.Lwork_replay:
 li s3,4
.Lreplay_record:
 lw s4,X_CURSOR(s2)
 lw t0,X_COUNT(s2)
 bgeu s4,t0,.Lwork_continue
 addi t0,s4,1
 sw t0,X_CURSOR(s2)
 lw t0,X_CAPACITY(s2)
 bgeu s4,t0,.Lreplay_spill
 slli t0,s4,3
 li t1,RECORD
 add t0,t0,t1
 add t0,t0,gp
 lw s5,0(t0)
 lw s6,4(t0)
 j .Lreplay_dispatch
.Lreplay_spill:
 sub t0,s4,t0
 slli t0,t0,3
 lw s7,X_SPILL_BASE(s2)
 add s7,s7,t0
 andi t0,s7,-32
 lw t1,X_SPILL_CACHED(s2)
 beq t0,t1,1f
 call br_drain
 andi a0,s7,-32
 li a1,1
 call br_load_start
 li a0,1
 call br_load_wait
 andi t0,s7,-32
 sw t0,X_SPILL_CACHED(s2)
1: andi t0,s7,31
 li t1,READ+32
 add t0,t0,t1
 add t0,t0,gp
 lw s5,0(t0)
 lw s6,4(t0)
.Lreplay_dispatch:
 srli t0,s6,24
 li t1,80
 mul t0,t0,t1
 add t0,t0,gp
 lw t1,292(t0)
 bnez t1,2f
 lw t1,284(t0)
 lw t2,H_DEP(gp)
 fmul.s a2,t1,t2
 srli a0,s6,16
 andi a0,a0,255
 mv a1,s5
 li a3,0
 slli t0,s6,16
 srli t0,t0,16
 li t1,4
 SEND t0,t1
 INC H_SENT,t0
 lw t0,X_MANUAL_REVERSE(s2)
 addi t0,t0,1
 sw t0,X_MANUAL_REVERSE(s2)
2: addi s3,s3,-1
 bnez s3,.Lreplay_record
 j .Lwork_continue
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
