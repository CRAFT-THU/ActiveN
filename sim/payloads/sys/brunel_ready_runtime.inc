/* Consecutive home-G4 reverse pipeline. This include is inside the shared
 * arithmetic/transport runtime. All incoming callbacks and updaters preserve
 * s0..s11, which belong to the SMT0 directory continuation. Header aliases
 * are private to BR_READY; legacy directory/boot credits remain separate.
 *
 * Exactly one token is created for each permanent updater handler (6/7).
 * Empty tokens disable their own mask before checking the ring, then park.
 * The tokens are never rebound or recreated at a timestep boundary.
 * Handler14 is a separate, single pending issuer after forward closure;
 * parking a job never pushes into the backpressured incoming handler4 queue.
 */
.equ BP_BASE, BR_BASE+BR_READY_PENDING
.globl br_ready_completion,br_ready_update

br_ready_boot:
 lw t0,H_NUM_PU(gp)
 li t1,512
 bne t0,t1,.Lboot_bad
 lw t0,H_PU(gp)
 addi t0,t0,-1
 andi t0,t0,-4
 addi t0,t0,1
 li t1,BR_BASE+PROXY_CONFIG
 li t2,4
1: lw t3,0(t1)
 bne t0,t3,.Lboot_bad
 addi t0,t0,1
 addi t1,t1,4
 addi t2,t2,-1
 bnez t2,1b
 sw zero,BR_READY_ALLOC(gp)
 sw zero,BR_READY_COUNT(gp)
 sw zero,BR_READY_HEAD(gp)
 sw zero,BR_READY_TAIL(gp)
 sw zero,BR_READY_ACTIVE(gp)
 sw zero,BR_READY_CREDIT(gp)
 li t0,BP_BASE
 sw zero,BR_READY_VALID(t0)
 SENDI zero,6
 SENDI zero,7
 ret

/* a4=word address, a5=LTD, a6=LTP, a7=encoded MC. Only SMT0 enters. */
br_ready_accept:
 csrci 0x714,8
 li t0,BP_BASE
 lw t1,BR_READY_VALID(t0)
 bnez t1,.Lready_bad
 sw a4,BR_READY_ADDRESS(t0)
 sw a5,BR_READY_LTD(t0)
 sw a6,BR_READY_LTP(t0)
 sw a7,BR_READY_MC(t0)
 li t1,1
 sw t1,BR_READY_VALID(t0)
br_ready_issue:
 csrci 0x71e,8
 li a4,BP_BASE
 lw t0,BR_READY_VALID(a4)
 beqz t0,br_idle
 lw t0,BR_READY_CREDIT(gp)
 li t1,BR_READY_READ_CREDITS
 bgeu t0,t1,.Lready_issue_park
 lw t0,BR_READY_COUNT(gp)
 li t1,BR_READY_SLOTS
 bgeu t0,t1,.Lready_issue_park
 lw t2,BR_READY_ALLOC(gp)
 slli a5,t2,6
 li t0,BR_BASE+BR_READY_MSHR
 add a5,a5,t0
 lw t0,44(a5)
 bnez t0,.Lready_issue_park
 lw a0,BR_READY_ADDRESS(a4)
 lw t0,BR_READY_LTD(a4)
 lw t1,BR_READY_LTP(a4)
 lw a7,BR_READY_MC(a4)
 sw a0,32(a5)
 sw t0,36(a5)
 sw t1,40(a5)
 sw a7,52(a5)
 li t0,1
 sw t0,44(a5)
 addi t1,gp,BR_READY_COUNT
 amoadd.w zero,t0,(t1)
 addi t1,gp,BR_READY_CREDIT
 amoadd.w zero,t0,(t1)
 addi t2,t2,1
 andi t2,t2,63
 sw t2,BR_READY_ALLOC(gp)
 andi a0,a0,-32
 sub a3,a5,gp            # callback a1 = exact descriptor byte offset
 srli a2,a3,5           # low half = DMA landing-line index
 li t0,(15<<16)         # high half = dedicated completion handler15
 or a2,a2,t0
 li a1,0x58002
 SENDI a7,1024          # P1 scalar read, distinct from P0 posted writeback
 sw zero,BR_READY_VALID(a4)
 INC H_REVERSE_RECEIVED,t0
 csrsi 0x714,8
 j br_idle
.Lready_issue_park:
 YIELDI 14
 j br_idle

/* Dedicated volatile completion queue: at most 16 unhandled notifications.
 * Directory/boot/marker notifications target handler0, never this queue.
 * DMA writes the first 32 bytes before the LSU publishes the callback.
 */
br_ready_completion:
 li t0,BR_READY_MSHR
 sub t0,a1,t0
 li t1,(BR_READY_SLOTS*64)
 bgeu t0,t1,.Lready_bad
 andi t0,a1,63
 bnez t0,.Lready_bad
 srli t0,a1,5
 bne a0,t0,.Lready_bad
 add t0,gp,a1
 lw t1,44(t0)
 li t2,1
 bne t1,t2,.Lready_bad
 li t1,2
 sw t1,44(t0)
 lw t0,BR_READY_TAIL(gp)
 lw t1,BR_READY_HEAD(gp)
 sub t1,t0,t1
 li t2,BR_READY_SLOTS
 bgeu t1,t2,.Lready_bad
 andi t1,t0,63
 slli t1,t1,2
 li t2,BR_BASE+BR_READY_RING
 add t1,t1,t2
 sw a1,0(t1)
 addi t0,t0,1
 sw t0,BR_READY_TAIL(gp) # publish only after writing the ring entry
 addi t0,gp,BR_READY_CREDIT
 li t1,-1
 amoadd.w t2,t1,(t0)
 beqz t2,.Lready_bad
 csrsi 0x71e,8
#ifdef BR_READY_TEST
 /* Stress fixture holds consumers until all 64 descriptors are ready. */
 li t0,BR_BASE+0x2a00
 lw t0,0(t0)
 li t1,3
 bltu t0,t1,br_idle
#endif
 csrsi 0x716,8
 csrsi 0x717,16
 j br_idle

br_ready_update:
 bnez tp,1f
 csrci 0x716,8
 j 2f
1: csrci 0x717,16
2: addi a4,gp,BR_READY_HEAD
.Lready_claim:
 lr.w t0,(a4)
 lw t1,BR_READY_TAIL(gp)
 beq t0,t1,.Lready_park
 andi t1,t0,63
 slli t1,t1,2
 li t2,BR_BASE+BR_READY_RING
 add t1,t1,t2
 lw a5,0(t1)            # copy entry before releasing the ring position
 addi t0,t0,1
 sc.w t1,t0,(a4)
 bnez t1,.Lready_claim
 add a5,a5,gp
 lw t0,44(a5)
 li t1,2
 bne t0,t1,.Lready_bad
 li t0,3
 sw t0,44(a5)
 addi t0,gp,BR_READY_ACTIVE
 li t1,1
 amoadd.w zero,t1,(t0)
#ifdef BR_READY_TEST
 li t0,BR_BASE+0x2a04
 slli t2,tp,2
 add t0,t0,t2
 amoadd.w zero,t1,(t0)
#endif
 lw a6,32(a5)
 andi t0,a6,28
 add t0,t0,a5
 lw a3,0(t0)
 lw a1,36(a5)
 beqz a1,3f
 LTD a3,a3,a1,BR_RULE
3: lw a1,40(a5)
 beqz a1,4f
 /* No saved-register constants: the SMT0 directory owns all saved regs. */
 LTP a3,a3,a1,BR_RULE
4: mv a0,a6
 li a1,0x20000
 lw t0,52(a5)
 SENDI t0,2
 sw zero,44(a5)         # captured store no longer refers to this descriptor
 li t1,-1
 addi t0,gp,BR_READY_ACTIVE
 amoadd.w t2,t1,(t0)
 beqz t2,.Lready_bad
 addi t0,gp,BR_READY_COUNT
 amoadd.w t2,t1,(t0)
 beqz t2,.Lready_bad
 csrsi 0x71e,8
 bnez tp,5f
 csrsi 0x716,8
 YIELDI 6
 j br_idle
5: csrsi 0x717,16
 YIELDI 7
 j br_idle
.Lready_park:
 /* Completion may re-enable this mask after our clear and before this yield.
  * The single successor is queued even when disabled, so no wake is lost. */
 bnez tp,6f
 YIELDI 6
 j br_idle
6: YIELDI 7
 j br_idle

br_ready_finish:
 lw t0,BR_READY_COUNT(gp)
 lw t1,BR_READY_ACTIVE(gp)
 or t0,t0,t1
 lw t1,BR_READY_CREDIT(gp)
 or t0,t0,t1
 li t1,BP_BASE
 lw t1,BR_READY_VALID(t1)
 or t0,t0,t1
 bnez t0,.Lready_finish_wait
 lw t0,BR_READY_HEAD(gp)
 lw t1,BR_READY_TAIL(gp)
 bne t0,t1,.Lready_bad
 jal ra,br_drain
 csrci 0x714,8
 csrci 0x71e,8
 csrci 0x716,8
 csrci 0x717,16
 /* All producers closed; P0/P1 stores precede P2 markers on each PU->MC
  * route. Marker completion establishes admission, then a global reduction
  * and one idle registration per MC establish completion of all stores. */
 li s0,0
.Lready_markers:
 li s1,0
1: slli t0,s1,2
 add t0,gp,t0
 sw zero,H_READY0(t0)
 lw a7,H_MC(gp)
 add a7,a7,s0
 add a7,a7,s1
 andi a7,a7,15
 li t0,0x8001
 add a7,a7,t0
 li a0,0
 mv a1,s1
 addi a2,s1,(READ/32)
 /* Enter below br_line_start's local-MC default, preserving explicit a7. */
 jal ra,br_mshr_line_start
 addi s1,s1,1
 li t0,4
 bltu s1,t0,1b
 li s1,0
2: mv a0,s1
 jal ra,br_load_wait
 addi s1,s1,1
 li t0,4
 bltu s1,t0,2b
 addi s0,s0,4
 li t0,16
 bltu s0,t0,.Lready_markers
 li a0,6
 li a1,0
 YIELDI BR_REPORT_ID
 j br_idle
.Lready_finish_wait:
 YIELDI BR_WORK_ID
 j br_idle

br_ready_routes_closed:       # root: all 512 route sweeps returned
 li a0,7
 j br_broadcast
br_ready_memory_closed:       # root: every MC acknowledged idle
 li a0,8
 j br_broadcast

br_ready_idle_start:          # control on SMT1, after global route closure
 la t0,br_ready_memory_idle
 csrw 0x705,t0
 li t0,17
 csrw 0x715,t0
 lw t0,H_PU(gp)
 addi t0,t0,-1
 andi t0,t0,31
 beqz t0,1f
 li a0,7
 li a1,0
 YIELDI BR_REPORT_ID
 j br_idle
1: li a0,5                  # zero idle threshold, return via NoC handler5
 lw t0,H_MC_DEST(gp)
 SENDI t0,-784              # P3 registration; Q4/M6 includes child controls
 j br_idle
br_ready_memory_idle:
 bnez a0,.Lready_bad
 li a0,7
 li a1,0
 YIELDI BR_REPORT_ID
 j br_idle

br_ready_finalize_start:
 /* No callbacks or descriptors remain; permanent worker tokens stay parked.
  * Head/tail are reset only after global marker and memory-idle closure. */
 csrci 0x716,8
 csrci 0x717,16
 sw zero,BR_READY_ALLOC(gp)
 sw zero,BR_READY_HEAD(gp)
 sw zero,BR_READY_TAIL(gp)
 sw zero,H_WORK_MC(gp)
 la t0,br_ready_trace_finish
 csrw 0x708,t0
 YIELDI BR_WORK_ID
 j br_idle
.Lready_bad:
#ifdef BR_READY_TEST
 /* Failure-only diagnostics; no production-payload instructions. */
 li a6,0x4000000c
 sw t0,0(a6)
 sw t1,0(a6)
 sw t2,0(a6)
 sw a0,0(a6)
 sw a1,0(a6)
 lw t0,H_PU(gp)
 sw t0,0(a6)
 sw tp,0(a6)
 lw t0,H_PHASE(gp)
 sw t0,0(a6)
 li t0,BR_BASE+0x2a00
 lw t1,0(t0)
 sw t1,0(a6)
 lw t1,4(t0)
 sw t1,0(a6)
 lw t1,8(t0)
 sw t1,0(a6)
#endif
 li a0,0xb064
 j br_halt
