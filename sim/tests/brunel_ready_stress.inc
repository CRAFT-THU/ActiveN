/* Test-only use of the actual ready issuer/callback/updaters and final fence.
 * PU1 reads 65 distinct words across all MCs. Hold both completion handlers
 * until 16 reverse and four directory DMA lines have landed, then hold both
 * consumers until the 64-entry pool/ring is full and request65 is parked.
 * All other PUs participate in the actual route-marker/MC-idle reductions.
 * Fixture bytes (weights at0xa000, 65 scatter entries at0xc000) are supplied
 * by brunel_ready_stress.py; no simulator changes are involved.
 */
br_ready_test_start:
 li t0,3
 sw t0,H_PHASE(gp)
 lw t0,H_PU(gp)
 li t1,1
 beq t0,t1,1f
 li a0,3
 li a1,0
 YIELDI BR_REPORT_ID
 j br_idle
1:
 la t0,br_ready_test_accept
 csrw 0x704,t0
 csrsi 0x714,8
 csrw 0x71e,zero
 la t0,br_ready_issue
 csrw 0x70e,t0
 li t0,0x00010004
 csrw 0x72e,t0
 csrci 0x710,16
 csrci 0x71f,16
 la t0,br_ready_test_poll
 csrw 0x708,t0
 li t0,BR_BASE+0x2a00
 sw zero,0(t0)
 sw zero,4(t0)
 sw zero,8(t0)
 sw zero,12(t0)
 YIELDI 8
 j br_idle

br_ready_test_accept:         # a0=slot, a1=address, no neural state mutation
 mv a4,a1
 li a5,0x3e800000             # 0.25 LTD
 li a6,0x3f000000             # 0.5 LTP
 andi a7,a0,15
 li t0,0x8001
 add a7,a7,t0
 j br_ready_accept

br_ready_test_poll:
 li t0,BR_BASE+0x2a00
 lw t1,0(t0)
 beqz t1,.Lready_test_setup
 li t2,1
 beq t1,t2,.Lready_test_callbacks
 li t2,2
 beq t1,t2,.Lready_test_full
 /* Exercise SMT1 deterministically before permitting SMT0 to contend. The
  * normal production policy is unchanged; this is an affinity control. */
 lw t1,8(t0)
 beqz t1,.Lready_test_again
 csrsi 0x716,8
 lw t1,H_REVERSE_RECEIVED(gp)
 li t2,65
 bne t1,t2,.Lready_test_again
 lw t1,BR_READY_COUNT(gp)
 bnez t1,.Lready_test_again
 la t0,br_work
 csrw 0x708,t0
 li a0,3
 li a1,65
 YIELDI BR_REPORT_ID
 j br_idle
.Lready_test_setup:
 li t1,1
 sw t1,0(t0)
 /* Four handler0 completions, while handler15 also accumulates sixteen. */
 li s0,0
1: slli t0,s0,5
 li t1,BR_BASE+READ
 add t0,t0,t1
 sw zero,0(t0)
 li a0,0xa000
 mv a1,s0
 jal ra,br_load_start
 addi s0,s0,1
 li t0,4
 bltu s0,t0,1b
 li a0,0xc000
 li a1,((17<<16)|0xc04)
 li a2,0
 li a3,0
 li t0,0x8001
 SENDI t0,-1008
 j .Lready_test_again
.Lready_test_callbacks:
 lw t0,BR_READY_CREDIT(gp)
 li t1,16
 bne t0,t1,.Lready_test_again
 li t0,BP_BASE
 lw t0,BR_READY_VALID(t0)
 beqz t0,.Lready_test_again
 /* Each known-nonzero DMA line must have arrived while callbacks were held. */
 li t0,BR_BASE+BR_READY_MSHR
 li t1,16
1: lw t2,0(t0)
 beqz t2,.Lready_test_again
 addi t0,t0,64
 addi t1,t1,-1
 bnez t1,1b
 li t0,BR_BASE+READ
 li t1,4
2: lw t2,0(t0)
 beqz t2,.Lready_test_again
 addi t0,t0,32
 addi t1,t1,-1
 bnez t1,2b
 li t0,BR_BASE+0x2a00
 li t1,2
 sw t1,0(t0)
 csrsi 0x710,16
 csrsi 0x71f,16
 j .Lready_test_again
.Lready_test_full:
 lw t0,BR_READY_COUNT(gp)
 li t1,64
 bne t0,t1,.Lready_test_again
 lw t0,BR_READY_CREDIT(gp)
 bnez t0,.Lready_test_again
 lw t0,BR_READY_TAIL(gp)
 bne t0,t1,.Lready_test_again
 li t0,BP_BASE
 lw t0,BR_READY_VALID(t0)
 beqz t0,.Lready_test_again
 /* Record whether the actual callback order differs from allocation order. */
 li t0,BR_BASE+BR_READY_RING
 li t1,BR_READY_MSHR
 li t2,64
 li t3,0
 li t5,BR_BASE+0x2b00
1: lw t4,0(t0)
 sw t4,0(t5)
 addi t5,t5,4
 xor t4,t4,t1
 or t3,t3,t4
 addi t0,t0,4
 addi t1,t1,64
 addi t2,t2,-1
 bnez t2,1b
 li t0,BR_BASE+0x2a00
 snez t3,t3
 sw t3,12(t0)
 li t1,3
 sw t1,0(t0)
 csrsi 0x717,16
.Lready_test_again:
 YIELDI 8
 j br_idle

br_ready_test_done:
 li t0,BR_BASE+0x2a00
 lw t1,0(t0)
 li t2,3
 bne t1,t2,.Lready_bad
 lw t1,4(t0)
 lw t2,8(t0)
 beqz t1,.Lready_bad
 beqz t2,.Lready_bad
 add t1,t1,t2
 li t2,65
 bne t1,t2,.Lready_bad
 li t0,1
 sw t0,H_COMPLETED(gp)
 li a0,0
 j br_halt
