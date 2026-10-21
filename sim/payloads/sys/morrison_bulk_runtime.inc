.globl mb_memory_idle,mb_copy_ready,mb_dispatch,mb_job,mb_produce,mb_check,mb_emit,br_control,br_report
/* ABI3. SMT0 s0-s3 own the issuing range. SMT1 s0-s11 own emission.
 * All other forward handlers preserve saved registers. State words B_* are
 * relative to 0x20002000. Local continuations have arity zero. */
.equ MB_BASE, BR_BASE+MB_CONTROL

/* One backpressurable idle reply per MC leader, after all route markers.
 * Nonzero means registration failed and must never satisfy the barrier. */
mb_memory_idle:
 bnez a0,.Lboot_bad
 li a0,14
 li a1,0
 YIELDI 7
 j br_idle

/* Each request owns one queued check, in addition to a currently executing
 * check. Clearing before the state check pairs with the final emitter wake. */
mb_request_check:
 li t0,MB_BASE+B_CHECK_PENDING
 li t1,1
 amoor.w t1,t1,(t0)
 bnez t1,1f
 SENDI zero,14
1: ret

/* SMT0 only. At most one local completion send; never touches job s0-s3. */
mb_probe:
 li t0,MB_BASE
 lw t1,B_EMITTING(t0)
 bnez t1,1f
 lw t1,B_COPY(t0)
 beqz t1,1f
 csrr t2,0x730
 bnez t2,1f
 li t2,B_READY
 sw t2,B_STATE(t1)
 sw zero,B_COPY(t0)
 li t2,1
 sw t2,B_COMPLETE_PENDING(t0)
 mv a0,t1
 SENDI zero,8
1: ret
mb_check:
 li t0,MB_BASE+B_CHECK_PENDING
 amoswap.w zero,zero,(t0)
 lw t0,H_PHASE(gp)
 li t1,2
 bne t0,t1,2f
 jal ra,mb_probe
 li t0,MB_BASE
 lw t1,B_EMITTING(t0)
 bnez t1,2f
 lw t1,B_COPY(t0)
 beqz t1,2f
 jal ra,mb_request_check
2: j br_idle

mb_job:
 csrci (0x710+MB_JOB_ID),8
 mv s0,a0
 mv s1,a1
 mv s2,a2
 mv s3,a3
 bgeu s0,s1,.Lboot_bad
 or t0,s0,s1
 andi t0,t0,31
 bnez t0,.Lboot_bad
 li t0,MB_BASE
 li t1,1
 sw t1,B_ISSUING(t0)
 YIELDI 9
 j br_idle

mb_dispatch:
 csrci 0x719,8
 li t0,MB_BASE
 lw t1,B_COPY(t0)
 bnez t1,.Lboot_bad
 lw t1,B_COMPLETE_PENDING(t0)
 bnez t1,.Lboot_bad
 csrr t1,0x730
 bnez t1,.Lboot_bad
 addi t3,t0,B_BANK0
 lw t1,B_STATE(t3)
 beqz t1,1f
 addi t3,t3,B_BANK_BYTES
 lw t1,B_STATE(t3)
 bnez t1,.Lboot_bad
1: sub t2,s1,s0
 li t1,MB_COPY_BYTES
 bleu t2,t1,2f
 mv t2,t1
2: sw s0,B_ADDRESS(t3)
 sw t2,B_LENGTH(t3)
 li t1,65535
 and t1,t1,s3
 sw t1,B_MC(t3)
 srli t1,s3,16
#if BR_BULK_TREE
 andi t1,t1,1
#endif
 sw t1,B_KIND(t3)
 sw s2,B_AUX(t3)
#if BR_BULK_TREE
 add t1,s0,t2
 xor t1,t1,s1
 seqz t1,t1
 slli t1,t1,16
 srli t4,s3,17
 or t1,t1,t4
 sw t1,B_RETURN(t3)
#endif
 li a4,BR_BASE+MB_SHARED0
 addi t1,t0,B_BANK0
 beq t3,t1,4f
 li a4,BR_BASE+MB_SHARED1
4:
 sw a4,B_LANDING(t3)
 li t1,B_FILLING
 sw t1,B_STATE(t3)
 sw t3,B_COPY(t0)
 csrw 0x732,a4
 csrw 0x733,t2
 li t1,1
 csrw 0x730,t1
 mv a0,s0
 srli a1,t2,5
 slli a1,a1,16
 li t1,0x8001
 or a1,a1,t1
 lw t1,B_MC(t3)
 SENDI t1,-2031
 add s0,s0,t2
 srli t1,t2,3
 add s2,s2,t1             # static auxiliary advances in directory entries
 jal ra,mb_request_check
 bltu s0,s1,3f
 li t0,MB_BASE
 sw zero,B_ISSUING(t0)
 lw t1,B_EXPANDED(t0)
 addi t1,t1,1
 sw t1,B_EXPANDED(t0)
 csrsi (0x710+MB_JOB_ID),8
 j br_idle
3: YIELDI 9
 j br_idle

mb_copy_ready:
 csrci 0x718,16
 mv s5,a0
 lw t0,B_STATE(s5)
 li t1,B_READY
 bne t0,t1,.Lboot_bad
 li t0,B_DRAINING
 sw t0,B_STATE(s5)
 li t0,MB_BASE
 sw s5,B_EMITTING(t0)
 sw zero,B_COMPLETE_PENDING(t0)
 lw s0,B_LANDING(s5)
 lw s2,B_LENGTH(s5)
 lw s3,B_ADDRESS(s5)
 sub s3,s3,s0
 lw s4,B_MC(s5)
 lw s6,B_KIND(s5)
 lw s7,B_AUX(s5)
#if BR_BULK_TREE
 lw s9,B_RETURN(s5)        # capture before the bank can be recycled
#endif
 add s11,s0,s2
 bnez s6,1f
 li t0,1024
 bleu s2,t0,2f
 mv s2,t0
2: srli s2,s2,1
1: add s1,s0,s2
 csrsi 0x719,8
 YIELDI 15
 j br_idle

mb_emit:
 li a6,4
 bnez s6,.Lmb_emit_static
.Lmb_emit_plastic:
 bgeu s0,s1,.Lmb_next_chunk
 add t0,s0,s2
 lw t0,0(t0)
 li t1,-1
 beq t0,t1,1f
 lw a1,0(s0)
 srli a0,t0,16
 add a2,s0,s3
 mv a3,s4
 SENDI t0,1029             # P1 spike handler5
#if BR_BULK_TREE
 addi s8,s8,1
#else
 INC H_SENT,t1
#endif
1: addi s0,s0,4
 addi a6,a6,-1
 bnez a6,.Lmb_emit_plastic
 j .Lmb_emit_continue
.Lmb_emit_static:
 bgeu s0,s1,.Lmb_emitted
 lw a0,0(s0)
 lw a5,4(s0)
 beqz a5,2f
 li t0,BR_BASE+MR_PARAMETERS
 lw t0,8(t0)
 bgeu s7,t0,.Lboot_bad
 addi a1,a5,3
 srli a1,a1,2
 slli a1,a1,16
 ori a1,a1,1029
 li a2,0
 li a3,0
 li t0,0x8001
 add t0,t0,s7
 SENDI t0,1040             # P1 static scatter; terminal delivery has a2=0
#if BR_BULK_TREE
 add s8,s8,a5
#else
 lw t0,H_SENT(gp)
 add t0,t0,a5
 sw t0,H_SENT(gp)
#endif
2: addi s0,s0,8
 addi s7,s7,1
 addi a6,a6,-1
 bnez a6,.Lmb_emit_static
.Lmb_emit_continue:
 bltu s0,s1,3f
 bnez s6,.Lmb_emitted
.Lmb_next_chunk:
 add s0,s0,s2              # skip this chunk's destination half
 bgeu s0,s11,.Lmb_emitted
 sub s2,s11,s0
 li t0,1024
 bleu s2,t0,5f
 mv s2,t0
5: srli s2,s2,1
 add s1,s0,s2
 j 3f
.Lmb_emitted:
 sw zero,B_STATE(s5)
 li t0,MB_BASE
 sw zero,B_EMITTING(t0)
 csrsi 0x718,16
#if BR_BULK_TREE
 srli t1,s9,16
 beqz t1,4f
 mv a0,s8
 li s8,0
 slli t1,s9,16
 srli t1,t1,16
 SENDI t1,2               # P0 completion; no data-bank or MSHR held
4:
#endif
 jal ra,mb_request_check
 j br_idle
3: YIELDI 15
 j br_idle

/* One P3 range per invocation; all producer context is SPM so it cannot
 * overwrite the current executor's s0-s3 when its own job returns locally. */
mb_produce:
 li t3,MB_BASE
 lw t0,B_HOME_STAGE(t3)
 bnez t0,.Lmb_send_job
.Lmb_home_next:
 lw t0,B_HOME_INDEX(t3)
 lw t1,H_FIRE_COUNT(gp)
 bgeu t0,t1,.Lmb_producer_done
 slli t0,t0,2
 li t1,BR_BASE+FIRED_LIST
 add t0,t0,t1
 lw t0,0(t0)
 sub t1,t0,gp
 addi t1,t1,-256
 srli t1,t1,5              # local neuron slot
 sw zero,B_HOME_RANK(t3)
 sw zero,B_HOME_AUX(t3)
 lw t2,H_EXCITATORY_END(gp)
 bgeu t0,t2,.Lmb_prepare_static
#if BR_BULK_WORKER
 li t2,BR_BASE+MR_PARAMETERS
 lw t5,40(t2)
 lw t2,20(t2)
 mul t2,t1,t2
 slli t2,t2,3
 add t5,t5,t2
 sw t5,B_HOME_DIRECTORY(t3)
 li t5,-1
 sw t5,B_HOME_DIRECTORY_LINE(t3)
#endif
 slli t0,t1,2
 li t2,BR_BASE+MB_ROW_POINTERS
 add t0,t0,t2
 lw t1,0(t0)
 lw t2,4(t0)
 sw t1,B_HOME_CUR(t3)
 sw t2,B_HOME_END(t3)
 sub t2,t2,t1
 addi t2,t2,1023
 srli t2,t2,10
 li t0,10                 # log2 bytes per plastic work unit
 sw zero,B_HOME_KIND(t3)
 li t1,1
 sw t1,B_HOME_STAGE(t3)
 j .Lmb_span
.Lmb_prepare_static:
 lw t2,B_DIR_STRIDE(t3)
 mul t1,t1,t2
 lw t0,B_STATIC_BASE(t3)
 add t1,t1,t0
 sw t1,B_HOME_CUR(t3)
 add t1,t1,t2
 sw t1,B_HOME_END(t3)
 srli t2,t2,5
 li t0,5                  # log2 bytes per directory work unit
 li t1,1
 sw t1,B_HOME_KIND(t3)
 li t1,2
 sw t1,B_HOME_STAGE(t3)
.Lmb_span:
 jal ra,mb_home_span
.Lmb_send_job:
#if BR_BULK_WORKER
 lw t0,B_HOME_KIND(t3)
 bnez t0,.Lmb_directory_ready
 lw t0,B_HOME_RANK(t3)
 srli t0,t0,2
 lw t1,B_HOME_DIRECTORY_LINE(t3)
 beq t0,t1,.Lmb_directory_ready
 sw t0,B_HOME_DIRECTORY_LINE(t3)
 slli a0,t0,5
 lw t1,B_HOME_DIRECTORY(t3)
 add a0,a0,t1
 csrci 0x71d,8           # clear before issue; callback may return early
 sw zero,B_HOME_DIRECTORY_RECEIVED(t3)
 li a1,0x10800          # one scatter line, P2 response to handler0
 li a2,0
 li a3,0
 lw t0,H_MC_DEST(gp)
 SENDI t0,-2032         # P2 scatter; four pointer events, no SPM DMA
 YIELDI 13
 j br_idle
.Lmb_directory_ready:
#endif
 lw a0,B_HOME_CUR(t3)
 lw a1,B_HOME_SPAN(t3)
 lw t0,B_HOME_RANK(t3)
 lw t1,B_HOME_REMAINDER(t3)
 sltu t0,t0,t1
 lw t1,B_HOME_KIND(t3)
 li t2,10
 beqz t1,4f
 li t2,5
4: sll t0,t0,t2
 add a1,a1,t0
 add a1,a1,a0
 lw t0,B_HOME_END(t3)
 bleu a1,t0,1f
 mv a1,t0
1: lw a2,B_HOME_AUX(t3)
 lw a3,B_HOME_KIND(t3)
 slli a3,a3,16
 lw t0,H_MC_DEST(gp)
 or a3,a3,t0
#if BR_BULK_TREE
 lw t0,H_PU(gp)
 slli t0,t0,17
 or a3,a3,t0
#endif
 lw t0,B_HOME_RANK(t3)
 lw t1,B_GROUP_RANK(t3)
 add t1,t1,t0
 lw t2,B_HOME_WIDTH(t3)
 addi t2,t2,-1
 and t1,t1,t2
#if BR_BULK_DISTANT
 lw t2,B_GROUP_SHIFT(t3)
 sll t1,t1,t2
#endif
 lw t2,B_GROUP_BASE(t3)
 add t1,t1,t2
#if BR_BULK_DISTANT
 lw t2,0(t3)             # optional fixture XOR applies to complete worker ID
 xor t1,t1,t2
#endif
 addi t1,t1,1
#if BR_BULK_WORKER
 mv a5,a1               # keep the logical cursor across address translation
 lw t0,B_HOME_KIND(t3)
 bnez t0,.Lmb_worker_address_ready
 sub a1,a1,a0
 lw t0,B_HOME_RANK(t3)
 andi t0,t0,3
 slli t0,t0,2
 li t2,BR_BASE+READ
 add t2,t2,t0
 lw a0,0(t2)
 add a1,a1,a0
 addi t0,t1,-1
 srli t0,t0,5           # both supported systems have 32 PUs per MC
 li t2,0x8001
 add t0,t0,t2
 li t2,0xffff0000
 and a3,a3,t2
 or a3,a3,t0
.Lmb_worker_address_ready:
#endif
 SENDI t1,(-1024+MB_JOB_ID) # P3 range, including actual self PU ID
#if BR_BULK_WORKER
 sw a5,B_HOME_CUR(t3)
#else
 sw a1,B_HOME_CUR(t3)
#endif
 lw t0,B_PRODUCED(t3)
 addi t0,t0,1
 sw t0,B_PRODUCED(t3)
 sub t0,a1,a0
 srli t0,t0,3
 add a2,a2,t0
 sw a2,B_HOME_AUX(t3)
 lw t0,B_HOME_RANK(t3)
 addi t0,t0,1
 sw t0,B_HOME_RANK(t3)
 lw t0,B_HOME_END(t3)
#if BR_BULK_WORKER
 bne a5,t0,.Lmb_producer_yield
#else
 bne a1,t0,.Lmb_producer_yield
#endif
 lw t0,B_HOME_STAGE(t3)
 li t1,2
 beq t0,t1,2f
 # Finished plastic: prepare static for this same source on next invocation.
 lw t0,B_HOME_INDEX(t3)
 slli t0,t0,2
 li t1,BR_BASE+FIRED_LIST
 add t0,t0,t1
 lw t1,0(t0)
 sub t1,t1,gp
 addi t1,t1,-256
 srli t1,t1,5
 sw zero,B_HOME_RANK(t3)
 sw zero,B_HOME_AUX(t3)
 # Prepare without sending another packet under this invocation's quota.
 lw t2,B_DIR_STRIDE(t3)
 mul t1,t1,t2
 lw t0,B_STATIC_BASE(t3)
 add t1,t1,t0
 sw t1,B_HOME_CUR(t3)
 add t1,t1,t2
 sw t1,B_HOME_END(t3)
 srli t2,t2,5
 li t0,1
 sw t0,B_HOME_KIND(t3)
 li t0,2
 sw t0,B_HOME_STAGE(t3)
 li t0,5
 jal ra,mb_home_span
 j .Lmb_producer_yield
2: lw t0,B_HOME_INDEX(t3)
 addi t0,t0,1
 sw t0,B_HOME_INDEX(t3)
 sw zero,B_HOME_STAGE(t3)
.Lmb_producer_yield:
 jal ra,mb_probe
 YIELDI 13
 j br_idle
.Lmb_producer_done:
 jal ra,mb_probe
#if BR_BULK_TREE
 YIELDI 3                 # separate local queue: remote completions use 2
#else
 li t0,MB_BASE
 lw a1,B_PRODUCED(t0)
 li a0,2
 YIELDI 7
#endif
 j br_idle

/* Producer-only setup. t2 counts 1 KiB plastic chunks or 32-byte directory
 * lines; t0 is their byte shift. Static work retains its at-most-four peers.
 * No executor register is used: another incoming range owns SMT0 s0-s3. */
mb_home_span:
 li t1,BR_BASE+MR_PARAMETERS
 lw t1,20(t1)
 lw t4,B_HOME_KIND(t3)
 beqz t4,1f
 li t4,4
 bleu t1,t4,1f
 mv t1,t4
1: sw t1,B_HOME_WIDTH(t3)
 addi t4,t1,-1
#if !BR_BULK_DISTANT
 lw a4,H_PU(gp)
 addi a4,a4,-1
 and a5,a4,t4
 sw a5,B_GROUP_RANK(t3)
 not a5,t4
 and a5,a4,a5
 lw a4,0(t3)              # functional cross-MC override, normally zero
 xor a5,a5,a4
 sw a5,B_GROUP_BASE(t3)
#endif
 li a4,0
2: srli t1,t1,1
 beqz t1,3f
 addi a4,a4,1
 j 2b
3: sw a4,B_GROUP_LOG(t3)
#if BR_BULK_DISTANT
 lw a5,H_NUM_PU(gp)
 srl a5,a5,a4            # stride = number of PUs / number of workers
 addi t1,a5,-1
 lw t5,H_PU(gp)
 addi t5,t5,-1
 and t1,t5,t1
 sw t1,B_GROUP_BASE(t3)
 li t1,0
6: srli a5,a5,1
 beqz a5,7f
 addi t1,t1,1
 j 6b
7: sw t1,B_GROUP_SHIFT(t3)
 srl t5,t5,t1
 lw t1,B_HOME_KIND(t3)
 beqz t1,8f
 li t5,0                # static directory quarter j goes to worker quarter j
8: sw t5,B_GROUP_RANK(t3)
#endif
 li t1,BR_BASE+MB_DISPATCH_POLICY
 lw t1,0(t1)
 beqz t1,4f
 and t1,t2,t4
 sw t1,B_HOME_REMAINDER(t3)
 j 5f
4: sw zero,B_HOME_REMAINDER(t3)
 add t2,t2,t4
5: srl t2,t2,a4
 sll t2,t2,t0
 sw t2,B_HOME_SPAN(t3)
 ret

/* Binary reduction tree. One query epoch is in flight globally; each parent
 * resets its accumulator before forwarding that epoch to its children. */
br_control:
 li t4,MB_BASE
 sw a0,B_QUERY(t4)
 sw zero,H_REPORT_COUNT(gp)
 sw zero,H_REPORT_SUM(gp)
 lw t2,H_PU(gp)
 lw t3,H_NUM_PU(gp)
 slli t0,t2,1
 bgtu t0,t3,1f
 SENDI t0,6
 addi t0,t0,1
 bgtu t0,t3,1f
 SENDI t0,6
1:
br_control_leaf:
 li t0,SPIKE_COUNT_REPORT
 beq a0,t0,.Lmb_spike_report_local
 li t0,1
 bne a0,t0,.Lmb_spike_measure_start_done
 lw t1,H_STEP(gp)
 lw t2,H_WARMUP(gp)
 bne t1,t2,.Lmb_spike_measure_start_done
 li t0,BR_BASE+SPIKE_COUNT_BRUNEL
 lw t1,SPIKE_COUNT_STATE(t0)
 andi t1,t1,SPIKE_COUNT_STATE_ACTIVE
 bnez t1,.Lmb_spike_measure_start_done
 SPIKE_COUNT_RESET (BR_BASE + SPIKE_COUNT_BRUNEL), t0, t1
.Lmb_spike_measure_start_done:
 li t4,MB_BASE
 sw a0,B_QUERY(t4)
 li t0,1
 beq a0,t0,.Lmb_integrate_start
 li t0,2
 beq a0,t0,.Lmb_forward_start
 li t0,3
 beq a0,t0,.Lmb_reverse_start
 li t0,6
 beq a0,t0,.Lmb_finish_start
 li t0,4
 beq a0,t0,.Lmb_receipt_forward
 li t0,5
 beq a0,t0,.Lmb_receipt_reverse
 li t0,7
 beq a0,t0,.Lmb_expanded_query
 li t0,8
 beq a0,t0,.Lmb_bulk_query
 li t0,9
 beq a0,t0,.Lmb_sent_query
 li t0,11
 beq a0,t0,.Lmb_reverse_query
 li t0,14
 beq a0,t0,.Lmb_idle_start
 li t0,15
 beq a0,t0,.Lmb_finalize_start
 j .Lboot_bad
.Lmb_receipt_forward:
#if BR_BULK_UNCOUNTED
 j .Lboot_bad             # no forward receiver query exists in this variant
#elif BR_BULK_TREE
 j mb_receipt_arm
#else
 lw a1,H_RECEIVED(gp)
 j .Lmb_control_report
#endif
.Lmb_receipt_reverse:
 lw a1,H_REVERSE_RECEIVED(gp)
 j .Lmb_control_report
.Lmb_expanded_query:
 lw a1,B_EXPANDED(t4)
 j .Lmb_control_report
.Lmb_sent_query:
 lw a1,H_SENT(gp)
 j .Lmb_control_report
.Lmb_bulk_query:
 lw a1,B_ISSUING(t4)
 lw t0,B_COPY(t4)
 or a1,a1,t0
 lw t0,B_EMITTING(t4)
 or a1,a1,t0
 lw t0,B_COMPLETE_PENDING(t4)
 or a1,a1,t0
 lw t0,B_BANK0+B_STATE(t4)
 or a1,a1,t0
 lw t0,B_BANK1+B_STATE(t4)
 or a1,a1,t0
 csrr t0,0x730
 or a1,a1,t0
 snez a1,a1
 j .Lmb_control_report
.Lmb_reverse_query:
 li t0,R_BASE
#if !BR_BULK_SCHEDULED
 li t1,1
 sw t1,R_CLOSING(t0)
 csrsi 0x718,8
 csrsi 0x71f,16
#endif
 lw a1,R_PENDING(t0)
 lw t0,H_MSHR_COUNT(gp)
 or a1,a1,t0
#if !BR_BULK_SCHEDULED
 lw t0,H_MSHR_SCHEDULED(gp)
 or a1,a1,t0
#endif
 li t0,R_BASE
 lw t0,R_CREDIT(t0)
 or a1,a1,t0
#if BR_BULK_TREE && !BR_BULK_UNCOUNTED
 lw t0,B_RX_PENDING(t4)
 or a1,a1,t0
#endif
 snez a1,a1
.Lmb_control_report:
 YIELDI 7
 j br_idle
.Lmb_integrate_start:
 sw a0,H_PHASE(gp)
#if BR_BULK_SCHEDULED
 # Marker callbacks can re-enable the now-empty updater queue. Mask it before
 # joining integration: an early next-step job must wait for mb_job binding.
 csrci (0x710+MB_UPDATER0_ID),24
#endif
#if BR_BULK_RELAY
 # Global reverse acceptance and pool drain retired every relay token.
 csrw (0x710+MB_INTEGRATE_ID),zero
 la t0,br_integrate
 csrw (0x700+MB_INTEGRATE_ID),t0
 li t0,0x00010002
 csrw (0x720+MB_INTEGRATE_ID),t0
 li t0,24
 csrw (0x710+MB_INTEGRATE_ID),t0
#endif
 # Updater tokens are gone; restore callback before saved registers change.
 la t0,br_completion
 csrw 0x700,t0
 sw zero,H_JOIN_COUNT(gp)
#if BR_BULK_UNCOUNTED
 csrci 0x715,24           # receiver is re-enabled after integration
#else
 sw zero,H_RECEIVED(gp)
#endif
 sw zero,H_REVERSE_RECEIVED(gp)
 sw zero,H_EXPECTED_FORWARD(gp)
 sw zero,H_EXPECTED_REVERSE(gp)
 sw zero,B_EXPANDED(t4)
 sw zero,B_PRODUCED(t4)
 sw zero,B_HOME_INDEX(t4)
 sw zero,B_HOME_STAGE(t4)
 sw zero,B_REVERSE_STARTED(t4)
#if BR_BULK_TREE
 sw zero,B_HOME_CLOSED(t4)
 sw zero,B_HOME_RETURNED(t4)
 sw zero,B_HOME_SENT(t4)
#if !BR_BULK_UNCOUNTED
 sw zero,B_RX_INITIAL(t4)
 sw zero,B_RX_LAST(t4)
 sw zero,B_RX_CHILD_DELTA(t4)
 sw zero,B_RX_BASE(t4)
 sw zero,B_RX_LATE(t4)
 sw zero,B_RX_BASE_READY(t4)
 sw zero,B_RX_DONE(t4)
 # RX_PENDING was drained during reverse; never discard a queued wake.
#endif
#endif
 # The check flag persists until its already-queued event retires.
 lw t1,H_FIRE_COUNT(gp)
 li t0,BR_BASE+FIRED_LIST
 beqz t1,2f
1: lw t2,0(t0)
 STATUS_STORE zero,N_FIRE(t2)
 addi t0,t0,4
 addi t1,t1,-1
 bnez t1,1b
2: sw zero,H_FIRE_COUNT(gp)
 addi t0,gp,256
 sw t0,H_WORK_NEURON(gp)
 SENDI zero,MB_INTEGRATE_ID
 YIELDI MB_INTEGRATE_ID
 j br_idle
.Lmb_forward_start:
 sw a0,H_PHASE(gp)
#if BR_BULK_RELAY
 csrci (0x710+MB_INTEGRATE_ID),24         # integration joined; early reverse relays may queue
#endif
 la t0,SPIKE_ENTRY
 csrw 0x705,t0
 li t0,0x00010003
 csrw 0x725,t0
 la t0,mb_emit
 csrw 0x70f,t0
#if BR_BULK_TREE
 li t0,0x0006000b
#else
 li t0,0x0005000a
#endif
 csrw 0x72f,t0
 sw zero,H_SENT(gp)
#if BR_BULK_TREE
 li s8,0
 la t0,SPIKE_ENTRY
 csrw 0x705,t0
 csrw 0x713,zero
 la t0,mb_producer_closed
 csrw 0x703,t0
 csrw 0x723,zero
 li t0,16
 csrw 0x713,t0
 csrw 0x71d,zero
 la t0,mb_produce
 csrw 0x70d,t0
 li t0,0x00030018
 csrw 0x72d,t0
#endif
 li t0,BR_BASE+MR_PARAMETERS
 lw t1,12(t0)
 sw t1,B_STATIC_BASE(t4)
 lw t1,24(t0)
 sw t1,B_REVERSE_BASE(t4)
 lw t1,28(t0)
 sw t1,B_DIR_STRIDE(t4)
 lw t1,20(t0)
 li t2,0
 mv t0,t1
1: srli t0,t0,1
 beqz t0,2f
 addi t2,t2,1
 j 1b
2: sw t2,B_GROUP_LOG(t4)
 addi t1,t1,-1
 lw t0,H_PU(gp)
 addi t0,t0,-1
 and t2,t0,t1
 sw t2,B_GROUP_RANK(t4)
 not t1,t1
 and t0,t0,t1
 lw t2,0(t4)             # optional functional cross-MC dispatch XOR
 xor t0,t0,t2
 sw t0,B_GROUP_BASE(t4)
 la t0,mb_copy_ready
 csrw 0x708,t0
 li t0,0x00010001
 csrw 0x728,t0
 li t0,17
 csrw 0x718,t0
 la t0,mb_dispatch
 csrw 0x709,t0
 li t0,0x00030012
 csrw 0x729,t0
 li t0,8
 csrw 0x719,t0
#if BR_BULK_RELAY
 csrci 0x714,24
#else
 csrci 0x714,8
#endif
#if BR_BULK_UNCOUNTED
 la t0,mb_job
 csrw (0x700+MB_JOB_ID),t0           # reverse issuer or updater reused this slot
 li t0,0x00010012
 csrw (0x720+MB_JOB_ID),t0
#endif
 li t0,28
 csrw 0x715,t0
 li t0,12
 csrw (0x710+MB_JOB_ID),t0
 li t0,16
 csrw 0x71f,t0
 li t0,8
 csrw 0x71d,t0
 YIELDI 13
 j br_idle
.Lmb_reverse_start:
 sw a0,H_PHASE(gp)
#if BR_BULK_RELAY
 # Install disabled: mb_reverse_work first initializes SMT0 FP constants.
 csrw (0x710+MB_INTEGRATE_ID),zero
#if BR_BULK_RELAY_ORDERED
 la t0,br_reverse
#else
 la t0,mr_proxy_accept
#endif
 csrw (0x700+MB_INTEGRATE_ID),t0
#if BR_BULK_RELAY_ORDERED
 li t0,0x00010008
#else
 li t0,0x00010007
#endif
 csrw (0x720+MB_INTEGRATE_ID),t0
 li t0,4
 csrw (0x710+MB_INTEGRATE_ID),t0
#endif
#if BR_BULK_TREE
 csrw 0x713,zero
#if BR_BULK_UNCOUNTED
 csrw 0x71d,zero         # no receiver service; producer13 has closed
#endif
 # Service13 stays enabled on SMT0 to consume residual zero-delta wakes.
#endif
 csrw 0x732,gp
 csrci (0x710+MB_JOB_ID),8
 csrci 0x71f,16
#if !BR_BULK_UNCOUNTED
 csrci 0x715,24
#endif
 # Forward owns both banks until global closure. Clear only descriptor flags.
 li t0,BR_BASE+MB_SHARED0+44
 li t1,32
1: sw zero,0(t0)
 addi t0,t0,64
 addi t1,t1,-1
 bnez t1,1b
 li t0,BR_BASE+MB_SHARED1+44
 li t1,32
2: sw zero,0(t0)
 addi t0,t0,64
 addi t1,t1,-1
 bnez t1,2b
 li t0,R_BASE
 sw zero,R_HEAD(t0)
 sw zero,R_TAIL(t0)
 sw zero,R_ALLOC(t0)
 sw zero,R_PENDING(t0)
 sw zero,R_DIR_STAGE(t0)
#if !BR_BULK_SCHEDULED
 sw zero,R_CLOSING(t0)
 li t1,3
 sw t1,H_MSHR_SCHEDULED(gp)
#endif
 li s0,0
 li s1,BR_BASE+MB_READY_RING
 mv s2,t0
 la t1,mr_completion
 csrw 0x700,t1
 li t1,1
 sw t1,R_DIR_ACTIVE(t0)
 li s3,0x4c2bac89
 li s4,0xcccccccd
 li s10,0x3f99ca19
 la t0,mr_issue
 csrw (0x700+MB_ISSUER_ID),t0
 li t0,0x00010007
 csrw (0x720+MB_ISSUER_ID),t0
 li t0,8
 csrw (0x710+MB_ISSUER_ID),t0
 la t0,mr_update
 csrw (0x700+MB_UPDATER0_ID),t0
#if !BR_BULK_SCHEDULED
 csrw (0x700+MB_UPDATER1_ID),t0
#endif
 li t0,0x00030006
 csrw (0x720+MB_UPDATER0_ID),t0
#if BR_BULK_SCHEDULED
 li t0,24
 csrw (0x710+MB_UPDATER0_ID),t0
#else
 csrw (0x720+MB_UPDATER1_ID),t0
 li t0,8
 csrw (0x710+MB_UPDATER0_ID),t0
 li t0,16
 csrw (0x710+MB_UPDATER1_ID),t0
#endif
 la t0,mb_reverse_work
 csrw 0x709,t0
 li t0,0x0018001f
 csrw 0x729,t0
 li t0,8
 csrw 0x719,t0
 sw zero,H_WORK_NEURON(gp)
 sw zero,H_WORK_MC(gp)
 sw zero,H_SENT(gp)
 YIELDI 9
 j br_idle
.Lmb_finish_start:
 sw a0,H_PHASE(gp)
#if BR_BULK_RELAY
 csrci 0x714,24
 csrci (0x710+MB_INTEGRATE_ID),24
#else
 csrci 0x714,8
#endif
 csrci (0x710+MB_ISSUER_ID),8
#if BR_BULK_SCHEDULED
 # Work counts exclude the final updater scheduling tail on the other SMT.
 # Bind future tokens to a consumer, then cross to SMT0 to fence that tail.
 csrci (0x710+MB_UPDATER0_ID),24
 la t0,br_idle
 csrw (0x700+MB_UPDATER0_ID),t0
 csrw (0x720+MB_UPDATER0_ID),zero
 li t0,24
 csrw (0x710+MB_UPDATER0_ID),t0
 la t0,mb_update_tail_drained
 csrw 0x70d,t0
 csrw 0x72d,zero
 li t0,8
 csrw 0x71d,t0
 la t0,mb_update_tokens_drained
 csrw 0x70f,t0
 csrw 0x72f,zero
 li t0,16
 csrw 0x71f,t0
 YIELDI 13
 j br_idle

mb_update_tail_drained:    # SMT0; no old real updater can enqueue after this
 csrsi (0x710+MB_UPDATER0_ID),24           # repair a possible late mask clear by that updater
 YIELDI 15
 j br_idle

mb_update_tokens_drained: # SMT1; zero-margin updater queue had priority
 csrci (0x710+MB_UPDATER0_ID),24
 csrci 0x71d,8
 csrci 0x71f,16
#else
 csrci 0x718,8
 csrci 0x71f,16
#endif
 sw zero,H_WORK_MC(gp)
 la t0,mb_fence_routes
 csrw 0x709,t0
 csrsi 0x719,8
 YIELDI 9
 j br_idle
.Lmb_idle_start:
 # One leader per contiguous MC domain. Compute domain width from the image.
 lw t0,H_NUM_PU(gp)
 li t1,BR_BASE+MR_PARAMETERS
 lw t1,8(t1)
1: srli t1,t1,1
 beqz t1,2f
 srli t0,t0,1
 j 1b
2: addi t0,t0,-1
 lw t1,H_PU(gp)
 addi t1,t1,-1
 and t1,t1,t0
 beqz t1,3f
 li a1,0
 j .Lmb_control_report
3: li a0,0x001             # threshold zero, ordinary NoC return to handler1
 lw t0,H_MC_DEST(gp)
 SENDI t0,-784             # tag0xcf0: existing MemIf idle registration
 j br_idle
.Lmb_finalize_start:
 la t0,mb_finish
 csrw 0x709,t0
 YIELDI 9
 j br_idle

br_broadcast:
 li t0,SPIKE_COUNT_REPORT
 beq a0,t0,.Lmb_spike_report_broadcast
 mv a4,a0
 li t0,3
 bleu a0,t0,1f
 li t0,6
 bne a0,t0,3f
 li a0,4
1: addi a0,a0,1
 lw a1,H_STEP(gp)
 addi a1,a1,1
 li a2,0
 li a3,0
 li t0,1
 SENDI t0,10
3: mv a0,a4
 YIELDI 6
 j br_idle
.Lmb_spike_report_broadcast:
 li a1,0
 li a2,0
 li a3,0
 li t0,1
 SENDI t0,10
 YIELDI 6
 j br_idle

br_report:
 lw t0,H_REPORT_COUNT(gp)
 lw t1,H_REPORT_SUM(gp)
 addi t0,t0,1
 add t1,t1,a1
 lw t6,H_TREE_REPORTS(gp)
 bne t0,t6,1f
 lw t3,H_PU(gp)
 li t0,1
 beq t3,t0,.Lmb_root_report
 srli t0,t3,1
 mv a1,t1
 YIELD_TO t0,7
 j br_idle
1: sw t0,H_REPORT_COUNT(gp)
 sw t1,H_REPORT_SUM(gp)
 j br_idle
br_report_leaf:
 lw t0,H_PU(gp)
 srli t0,t0,1
 YIELD_TO t0,7
 j br_idle
.Lmb_spike_report_local:
 SPIKE_COUNT_EMIT (BR_BASE + SPIKE_COUNT_BRUNEL), t0, t1, t2
 li a0,SPIKE_COUNT_ACK_PHASE
 li a1,0
 li a2,0
 li a3,0
 YIELDI 7
 j br_idle
.Lmb_root_report:
 li t0,SPIKE_COUNT_ACK_PHASE
 beq a0,t0,.Lmb_spike_report_ack_root
 beqz a0,.Lmb_boot_report
 li t0,1
 beq a0,t0,.Lmb_begin_forward
 li t0,2
 beq a0,t0,.Lmb_producers_closed
 li t0,7
 beq a0,t0,.Lmb_jobs_expanded
 li t0,8
 beq a0,t0,.Lmb_copies_drained
 li t0,9
 beq a0,t0,.Lmb_freeze_forward
 li t0,4
 beq a0,t0,.Lmb_delivered_forward
 li t0,3
 beq a0,t0,.Lmb_reverse_issued
 li t0,5
 beq a0,t0,.Lmb_delivered_reverse
 li t0,11
 beq a0,t0,.Lmb_reverse_drained
 li t0,13
 beq a0,t0,.Lmb_routes_drained
 li t0,14
 beq a0,t0,.Lmb_memory_drained
 # Finish6: all posted stores drained and all PUs incremented the step.
 lw t0,H_STEP(gp)
 lw t2,H_WARMUP(gp)
 bleu t0,t2,.Lmb_spikes_counted
.Lmb_spikes_counted:
 sw t0,H_COMPLETED(gp)
 lw t1,H_WARMUP(gp)
 bne t0,t1,1f
 li t1,0x40000004
 li t2,1
 sw t2,0(t1)
1: lw t1,H_STEPS(gp)
 bne t0,t1,.Lmb_next_step
 li t1,0x40000004
 sw zero,0(t1)
 li a0,SPIKE_COUNT_REPORT
 j br_broadcast
.Lmb_spike_report_ack_root:
 li a0,0
 j br_halt
.Lmb_boot_report:
 lw t0,H_WARMUP(gp)
 bnez t0,.Lmb_next_step
 li t0,0x40000004
 li t1,1
 sw t1,0(t0)
.Lmb_next_step:
 li a0,1
 j br_broadcast
.Lmb_begin_forward:
 li a0,2
 j br_broadcast
.Lmb_producers_closed:
#if BR_BULK_TREE
 sw t1,H_EXPECTED_FORWARD(gp)
#if BR_BULK_UNCOUNTED
 li a0,3                 # all plastic delivery reads have completed
#else
 li a0,4
#endif
 j br_broadcast
#else
 li t0,MB_BASE
 sw t1,B_EXPECTED(t0)
 li a0,7
 j br_broadcast
#endif
.Lmb_jobs_expanded:
 li t0,MB_BASE
 lw t0,B_EXPECTED(t0)
 bgtu t1,t0,.Lboot_bad
 bne t1,t0,1f
 li a0,8
1: j br_broadcast
.Lmb_copies_drained:
 bnez t1,1f
 li a0,9
1: j br_broadcast
.Lmb_freeze_forward:
 sw t1,H_EXPECTED_FORWARD(gp)
 li a0,4
 j br_broadcast
.Lmb_delivered_forward:
#if BR_BULK_UNCOUNTED
 j .Lboot_bad
#elif BR_BULK_TREE
 li t0,MB_BASE
 sw t1,B_RX_BASE(t0)
 li t1,1
 sw t1,B_RX_BASE_READY(t0)
 j mb_receipt_root_check
#else
 lw t0,H_EXPECTED_FORWARD(gp)
 bgtu t1,t0,.Lboot_bad
 bne t1,t0,1f
 li a0,3
1: j br_broadcast
#endif
.Lmb_reverse_issued:
 sw t1,H_EXPECTED_REVERSE(gp)
 li a0,5
 j br_broadcast
.Lmb_delivered_reverse:
 lw t0,H_EXPECTED_REVERSE(gp)
 bgtu t1,t0,.Lboot_bad
 bne t1,t0,1f
 li a0,11
1: j br_broadcast
.Lmb_reverse_drained:
 bnez t1,1f
 li a0,6
1: j br_broadcast
.Lmb_routes_drained:
 li a0,14
 j br_broadcast
.Lmb_memory_drained:
 li a0,15
 j br_broadcast

#if BR_BULK_TREE
#include "morrison_tree_runtime.inc"
#endif

mb_reverse_work:
 csrci 0x719,8
 li t0,MB_BASE
 lw t1,B_REVERSE_STARTED(t0)
 bnez t1,1f
 li s3,0x4c2bac89
 li s4,0xcccccccd
 li s10,0x3f99ca19
 li t1,1
 sw t1,B_REVERSE_STARTED(t0)
 SENDI zero,MB_UPDATER0_ID
 SENDI zero,MB_UPDATER1_ID
#if BR_BULK_RELAY
 csrsi (0x710+MB_ACCEPT_ID),8
#if BR_BULK_SCHEDULED && !BR_BULK_RELAY_SMT1
 csrsi (0x710+MB_RELAY_ID),24 # trace reads/no-op accounting are safe on both
#else
 csrsi (0x710+MB_RELAY_ID),16
#endif
#else
 csrsi 0x714,8
#endif
1: li t0,R_BASE
 lw t1,R_DIR_STAGE(t0)
 li t2,1
 beq t1,t2,.Lmb_reverse_load
 bnez t1,.Lmb_reverse_directory_resume
 lw s1,H_WORK_NEURON(gp)
 lw t0,H_FIRE_COUNT(gp)
 bgeu s1,t0,.Lmb_reverse_report
 slli t0,s1,2
 li t1,BR_BASE+FIRED_LIST
 add t0,t0,t1
 lw t0,0(t0)
 lw t1,H_EXCITATORY_END(gp)
 bgeu t0,t1,.Lmb_reverse_next
 lw s8,N_Y(t0)
 sub t0,t0,gp
 addi t0,t0,-256
 srli t0,t0,5
 li t1,MB_BASE
 lw t2,B_DIR_STRIDE(t1)
 mul t0,t0,t2
 lw t1,B_REVERSE_BASE(t1)
 add s7,t0,t1
 li s6,0
 srli s5,t2,5
 li t0,R_BASE
 li t1,1
 sw t1,R_DIR_STAGE(t0)
.Lmb_reverse_load:
 li t0,R_BASE
 lw t0,R_CREDIT(t0)
 li t1,MB_READ_CREDITS
 bgeu t0,t1,.Lmb_reverse_park
 slli a0,s6,5
 add a0,a0,s7
 mv a1,s6
 jal ra,br_load_start
 addi s6,s6,1
 bltu s6,s5,.Lmb_reverse_load
 li s6,0
 li s7,BR_BASE+READ
 lw s9,H_SENT(gp)
 li t0,R_BASE
 li t1,2
 sw t1,R_DIR_STAGE(t0)
.Lmb_reverse_directory_resume:
 li t0,BR_BASE+MR_PARAMETERS
 lw t0,8(t0)
 bgeu s6,t0,.Lmb_reverse_directory_done
 andi t0,s6,3
 bnez t0,2f
 srli t0,s6,2
 slli t0,t0,2
 add t0,gp,t0
 lw t0,H_READY0(t0)
 beqz t0,.Lmb_reverse_park
2: lw a0,0(s7)
 lw s5,4(s7)
 beqz s5,3f
 mv a2,s8
 li a3,0
 addi a1,s5,3
 srli a1,a1,2
 slli a1,a1,16
 li t0,(0xc00+MB_RELAY_ID)
 or a1,a1,t0
 li t0,0x8001
 add t0,t0,s6
 SENDI t0,-1008
 add s9,s9,s5
3: addi s6,s6,1
 addi s7,s7,8
 csrsi 0x719,8
.Lmb_reverse_park:
 YIELDI 9
 j br_idle
.Lmb_reverse_directory_done:
 sw s9,H_SENT(gp)
.Lmb_reverse_next:
 addi s1,s1,1
 sw s1,H_WORK_NEURON(gp)
 li t0,R_BASE
 sw zero,R_DIR_STAGE(t0)
 csrsi 0x719,8
 YIELDI 9
 j br_idle
.Lmb_reverse_report:
 li t0,R_BASE
 sw zero,R_DIR_ACTIVE(t0)
 li a0,3
 lw a1,H_SENT(gp)
 YIELDI 7
 j br_idle

/* All old store producers have closed before phase6. P0 posted stores
 * precede these P2 markers on each identical PU->MC route, even though the
 * queues do not preserve equal-priority FIFO order. Global marker closure
 * precedes the idle-registration epoch; idle alone cannot fence late traffic.
 * Reuse at most four READ lines. Handler0 on SMT1 consumes their ID2 replies. */
mb_fence_routes:
 lw s7,H_WORK_MC(gp)
 li t0,BR_BASE+MR_PARAMETERS
 lw s5,8(t0)
 bgeu s7,s5,.Lmb_routes_report
 sub s5,s5,s7
 li t0,4
 bleu s5,t0,1f
 mv s5,t0
1: li s6,0
.Lmb_marker_issue:
 slli t0,s6,2
 add t0,gp,t0
 sw zero,H_READY0(t0)
 li t0,BR_BASE+MR_PARAMETERS
 lw t0,8(t0)
 addi t0,t0,-1
 lw a7,H_MC(gp)
 add a7,a7,s7
 and a7,a7,t0
 li t0,0x8001
 add a7,a7,t0
 li a0,0
 mv a1,s6
 addi a2,s6,(READ/32)
 # br_load_start would replace a7 with the local MC: enter below that setup.
 jal ra,br_mshr_line_start
 addi s6,s6,1
 addi s7,s7,1
 bltu s6,s5,.Lmb_marker_issue
 li s6,0
.Lmb_marker_wait:
 mv a0,s6
 jal ra,br_load_wait
 addi s6,s6,1
 bltu s6,s5,.Lmb_marker_wait
 sw s7,H_WORK_MC(gp)
 YIELDI 9
 j br_idle
.Lmb_routes_report:
 li a0,13
 li a1,0
 YIELDI 7
 j br_idle

mb_finish:
 li t0,BR_BASE+0x3f00
 li t1,0x19ca7ead
 lw t2,0(t0)
 bne t1,t2,.Lboot_bad
 lw t2,128(t0)
 bne t1,t2,.Lboot_bad
 li t0,BR_BASE+FIRED_LIST
 lw t1,H_FIRE_COUNT(gp)
 lw t3,H_POT(gp)
 lw t4,H_DEP(gp)
 lw t5,H_EXCITATORY_END(gp)
 beqz t1,3f
1: lw t2,0(t0)
 bgeu t2,t5,2f
 lw a0,N_X(t2)
 lw a1,N_Y(t2)
 fadd.s a0,a0,t3
 fadd.s a1,a1,t4
 sw a0,N_X(t2)
 sw a1,N_Y(t2)
2: addi t0,t0,4
 addi t1,t1,-1
 bnez t1,1b
3: INC H_STEP,t0
 li a0,6
 lw a1,H_FIRE_COUNT(gp)
 YIELDI 7
 j br_idle
