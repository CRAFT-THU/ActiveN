/* Reverse-only descriptor ownership. Handler4 accepts home updates, or
 * handler11 accepts relayed updates; handler5
 * (or12 in the uncounted variant, or8 with shared scheduling) resumes its
 * issuer on SMT0.
 * Handler0 is the sole ready-queue producer.
 * Handlers8/15 run the same updater on SMT0/1. H_MSHR_SCHEDULED has one
 * live-chain bit per updater (1/2), retained across every yield. Each chain
 * has exactly one active/queued token. Only its final empty retirement clears
 * its bit. The issuer retains its job token while parked. Clear the own mask
 * before inspecting work, so publication racing with sleep cannot lose a wake.
 * Shared scheduling instead uses two anonymous tokens in MB_UPDATER0_ID, enabled
 * on both SMTs. Empty tokens park until completion wakes them; phase cleanup
 * consumes them through a WFI stub, without live-chain bookkeeping.
 * Updaters preserve saved registers. The SMT1 producer owns s0-s2; the
 * separate SMT0 register bank retains the directory continuation. */
.equ R_BASE, BR_BASE+MB_READY_CONTROL

/* Reverse/marker-only SMT1 producer: s0=tail, s1=ring, s2=control.
 * Control/report/updater handlers preserve them until the next integration. */
mr_completion:
 li t0,3
 bleu a1,t0,.Lmr_directory_done
#if BR_DIAGNOSTIC
 li t0,MB_SHARED0
 sub t0,a1,t0
 srli t0,t0,11
 beqz t0,1f
 li t0,MB_SHARED1
 sub t0,a1,t0
 srli t0,t0,11
 bnez t0,.Lboot_bad
1: andi t0,a1,63
 bnez t0,.Lboot_bad
#endif
 andi t0,s0,63
 slli t0,t0,2
 add t0,t0,s1
 sw a1,0(t0)
 addi s0,s0,1
 sw s0,R_TAIL(s2)         # publish before credit and wake masks
 j .Lmr_return_credit
.Lmr_directory_done:
 slli t0,a1,2
 add t0,gp,t0
 li t1,1
 sw t1,H_READY0(t0)
.Lmr_return_credit:
 addi t0,s2,R_CREDIT
 li t1,-1
 amoadd.w zero,t1,(t0)
 csrsi (0x710+MB_ISSUER_ID),8
#if BR_BULK_SCHEDULED
 csrsi (0x710+MB_UPDATER0_ID),24
#else
 csrsi (0x710+MB_UPDATER0_ID),8
 csrsi (0x710+MB_UPDATER1_ID),16
#endif
 # This callback is installed only in reverse and marker phases. An enable
 # after directory closure cannot create a new continuation token.
 csrsi 0x719,8
 j br_idle

/* Home execution: a0 packs the weight MC; a1=address, a2=LTD, t2=LTP. */
#if BR_BULK_RELAY
#if BR_BULK_RELAY_ORDERED
.globl mr_proxy_accept
#endif
mr_proxy_accept:          # a1=weight address, a2=LTD, a3=LTP
#endif
mr_accept:
 csrci (0x710+MB_ACCEPT_ID),8
 li t0,R_BASE
 sw a1,R_ADDRESS(t0)
 sw a2,R_LTD(t0)
#if BR_BULK_RELAY
 sw a3,R_LTP(t0)
 lw t1,H_MC_DEST(gp)
#else
 sw t2,R_LTP(t0)
 andi t1,a0,15
 li t2,0x8001
 add t1,t1,t2
#endif
 sw t1,R_MC(t0)
 li t1,1
 sw t1,R_PENDING(t0)
 j mr_issue

mr_issue:
 csrci (0x710+MB_ISSUER_ID),8
 li t3,R_BASE
 lw t0,R_PENDING(t3)
 beqz t0,9f
 lw t0,R_CREDIT(t3)
 li t1,MB_READ_CREDITS
 bgeu t0,t1,.Lmr_issue_park
 lw a4,R_ALLOC(t3)
 andi t0,a4,31
 slli t0,t0,6
 li a5,BR_BASE+MB_SHARED0
 li t1,32
 bltu a4,t1,1f
 li a5,BR_BASE+MB_SHARED1
1: add a5,a5,t0
 lw t0,44(a5)
 bnez t0,.Lmr_issue_park
 lw a0,R_ADDRESS(t3)
 lw t0,R_LTD(t3)
 lw t1,R_LTP(t3)
 lw a7,R_MC(t3)
 sw a0,32(a5)
 sw t0,36(a5)
 sw t1,40(a5)
 sw a7,52(a5)
 li t0,1
 sw t0,44(a5)
 addi t1,gp,H_MSHR_COUNT
 amoadd.w zero,t0,(t1)
 addi a4,a4,1
 andi a4,a4,63
 sw a4,R_ALLOC(t3)
 sub a1,a5,gp
 srli a2,a1,5
 andi a0,a0,-32
 jal ra,br_mshr_line_start # prechecked credit, sole SMT0 request issuer
 li t0,R_BASE
 sw zero,R_PENDING(t0)
#if BR_BULK_RELAY
 addi t1,gp,H_REVERSE_RECEIVED
 li t2,1
 amoadd.w zero,t2,(t1)
#else
 INC H_REVERSE_RECEIVED,t1
#endif
 csrsi (0x710+MB_ACCEPT_ID),8
9: j br_idle
.Lmr_issue_park:
 YIELDI MB_ISSUER_ID
 j br_idle

mr_update:
 bnez tp,1f
 csrci (0x710+MB_UPDATER0_ID),8
 j 2f
1: csrci (0x710+MB_UPDATER1_ID),16
2:
 li a4,R_BASE
.Lmr_update_claim:
 lr.w t0,(a4)
 lw t1,R_TAIL(a4)
 beq t0,t1,.Lmr_update_empty
 andi t1,t0,63
 slli t1,t1,2
 li t2,BR_BASE+MB_READY_RING
 add t1,t1,t2
 lw a5,0(t1)             # copy before releasing this ring position
 addi t0,t0,1
 sc.w t1,t0,(a4)
 bnez t1,.Lmr_update_claim
 add a5,a5,gp
 lw a6,32(a5)
 andi t0,a6,28
 add t0,t0,a5
 lw a3,0(t0)
 lw a1,36(a5)
 beqz a1,2f
 LTD a3,a3,a1,BR_RULE
2: lw a1,40(a5)
 beqz a1,3f
 LTP a3,a3,a1,BR_RULE,1
3: mv a0,a6
 li a1,0x20000
 lw t0,52(a5)
 SENDI t0,2
 sw zero,44(a5)          # store captured; no descriptor access follows
 csrsi (0x710+MB_ISSUER_ID),8
 addi t0,gp,H_MSHR_COUNT
 li t1,-1
 amoadd.w zero,t1,(t0)
 # Keep exactly one successor token, including while it is parked.
 bnez tp,4f
 csrsi (0x710+MB_UPDATER0_ID),8
 YIELDI MB_UPDATER0_ID
 j br_idle
4: csrsi (0x710+MB_UPDATER1_ID),16
 YIELDI MB_UPDATER1_ID
 j br_idle
.Lmr_update_empty:
#if !BR_BULK_SCHEDULED
 lw t0,R_CLOSING(a4)
 beqz t0,.Lmr_update_park
 lw t0,R_CREDIT(a4)
 bnez t0,.Lmr_update_park
 # All issuance is closed and every completion published before credit0.
 # Recheck after observing credit0; the earlier empty observation can be stale.
 lw t0,R_HEAD(a4)
 lw t1,R_TAIL(a4)
 bne t0,t1,.Lmr_update_claim
 li t1,1
 sll t1,t1,tp
 not t1,t1
 addi t0,gp,H_MSHR_SCHEDULED
 amoand.w zero,t1,(t0)
 j br_idle
#endif
.Lmr_update_park:
 bnez tp,5f
 YIELDI MB_UPDATER0_ID
 j br_idle
5: YIELDI MB_UPDATER1_ID
 j br_idle
