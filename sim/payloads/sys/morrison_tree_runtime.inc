/* Forward completion without repeated global queries. Saved registers belong
 * to the existing producer/emitter; all handlers here use only scratch regs. */
.globl mb_range_done,mb_producer_closed
#if !BR_BULK_UNCOUNTED
.globl mb_receipt_arm,mb_receipt_delta,mb_receipt_service
#endif
mb_range_done:                 # SMT1, one ordinary P0 message per range
 li t0,MB_BASE
 lw t1,B_HOME_RETURNED(t0)
 addi t1,t1,1
 sw t1,B_HOME_RETURNED(t0)
 lw t2,B_HOME_SENT(t0)
 add t2,t2,a0
 sw t2,B_HOME_SENT(t0)
 j .Lmb_home_check

mb_producer_closed:            # SMT1, separate bounded local-only queue
 li t0,MB_BASE
 lw t1,B_HOME_CLOSED(t0)
 bnez t1,.Lboot_bad
 li t1,1
 sw t1,B_HOME_CLOSED(t0)
.Lmb_home_check:
 lw t1,B_HOME_CLOSED(t0)
 beqz t1,1f
 li t2,1
 bne t1,t2,.Lboot_bad
 lw t1,B_HOME_RETURNED(t0)
 lw t2,B_PRODUCED(t0)
 bgtu t1,t2,.Lboot_bad
 bne t1,t2,1f
 li t1,2                     # explicit reported state
 sw t1,B_HOME_CLOSED(t0)
 lw a1,B_HOME_SENT(t0)
 li a0,2
 YIELDI 7
1: j br_idle

#if !BR_BULK_UNCOUNTED
/* All requesters coalesce on one bit. Caller must have one local-send quota
 * if it cannot tail-yield (controller and initial receipt service). */
mb_receipt_wake:
 li t0,MB_BASE+B_RX_PENDING
 li t1,1
 amoor.w t1,t1,(t0)
 bnez t1,1f
 SENDI zero,13
1: ret

mb_receipt_arm:                # controller6 command4, SMT1, once per step
 csrw 0x71d,zero
 csrw 0x713,zero
 li t4,MB_BASE
 sw zero,B_RX_PENDING(t4)
 li t0,1
 sw t0,B_RX_INITIAL(t4)
 sw zero,B_RX_LAST(t4)
 sw zero,B_RX_CHILD_DELTA(t4)
 sw zero,B_RX_BASE(t4)
 sw zero,B_RX_LATE(t4)
 sw zero,B_RX_BASE_READY(t4)
 sw zero,B_RX_DONE(t4)
 la t0,mb_receipt_service
 csrw 0x70d,t0
 li t0,0x00010003
 csrw 0x72d,t0
 li t0,8
 csrw 0x71d,t0
 la t0,mb_spike_receipt
 csrw 0x705,t0               # cfg28 unchanged; both SMTs, four arguments
 la t0,mb_receipt_delta
 csrw 0x703,t0
 csrw 0x723,zero
 li t0,9
 csrw 0x713,t0               # only now can a child delta execute on SMT0
 jal ra,mb_receipt_wake
 j br_idle

mb_receipt_delta:              # SMT0, incoming positive subtree delta
 beqz a0,.Lboot_bad
 li t0,MB_BASE
 lw t1,B_RX_CHILD_DELTA(t0)
 add t1,t1,a0
 sw t1,B_RX_CHILD_DELTA(t0)
 addi t0,t0,B_RX_PENDING
 li t1,1
 amoor.w t1,t1,(t0)
 bnez t1,1f
 YIELDI 13
1: j br_idle

mb_receipt_service:            # SMT0; serializes after pre-arm old spikes
 li t0,MB_BASE+B_RX_PENDING
 amoswap.w zero,zero,(t0)
 lw t0,H_PHASE(gp)
 li t1,2
 bne t0,t1,1f               # reverse cleanup, no outgoing message
 li t4,MB_BASE
 lw t0,B_RX_INITIAL(t4)
 beqz t0,.Lmb_receipt_later
 sw zero,B_RX_INITIAL(t4)
 lw a1,H_RECEIVED(gp)
 sw a1,B_RX_LAST(t4)
 lw t0,B_RX_CHILD_DELTA(t4)
 beqz t0,2f
 jal ra,mb_receipt_wake      # initial reduction deliberately excludes deltas
2: li a0,4
 YIELDI 7
1: j br_idle
.Lmb_receipt_later:
 lw t0,H_RECEIVED(gp)
 lw t1,B_RX_LAST(t4)
 sw t0,B_RX_LAST(t4)
 sub a0,t0,t1
 lw t0,B_RX_CHILD_DELTA(t4)
 sw zero,B_RX_CHILD_DELTA(t4)
 add a0,a0,t0
 beqz a0,1b
 lw t0,H_PU(gp)
 li t1,1
 beq t0,t1,.Lmb_receipt_root_delta
 srli t0,t0,1
 YIELD_TO t0,3
 j br_idle
.Lmb_receipt_root_delta:
 lw t0,B_RX_LATE(t4)
 add t0,t0,a0
 sw t0,B_RX_LATE(t4)
mb_receipt_root_check:
 li t0,MB_BASE
 lw t1,B_RX_BASE_READY(t0)
 beqz t1,1f
 lw t1,B_RX_BASE(t0)
 lw t2,B_RX_LATE(t0)
 add t1,t1,t2
 lw t2,H_EXPECTED_FORWARD(gp)
 bgtu t1,t2,.Lboot_bad
 bne t1,t2,1f
 addi t0,t0,B_RX_DONE
 li t1,1
 amoswap.w t1,t1,(t0)
 bnez t1,1f
 li a0,3
 j br_broadcast
1: j br_idle
#endif
