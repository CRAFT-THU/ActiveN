.section .text.boot
.globl _start
_start:
#ifdef BR_ASM_RUNTIME
  li gp,0x20000000
#endif
  bnez a0, thread_one
  li sp, 0x20003800
  call snn_init
  li t0, 0x20000000
  csrw 0x732,t0
  .macro INSTALL id,func,cfg,margin
  la t0,\func
  csrw (0x700+\id),t0
  li t0,\margin
  csrw (0x720+\id),t0
  li t0,\cfg
  csrw (0x710+\id),t0
  .endm
  INSTALL 0,completion,18,0
  INSTALL 1,spike,20,0x00020002
  INSTALL 2,control,20,0x00030005
  INSTALL 3,report,20,0x00020004
  INSTALL 4,reverse,12,0x000a0011
  INSTALL 8,work,8,0x0018001f
  INSTALL 10,idle,16,0
  call br_boot
idle:
  wfi
  j idle
thread_one:
  li sp,0x20004000
  j idle
completion:
  call br_completion
  j idle
spike:
  call br_spike
  j idle
control:
  call br_control
  j idle
report:
  call br_report
  j idle
reverse:
  # Most nonfiring-post deliveries target a presynaptic neuron that did not
  # fire. Count them without a C frame, lookup, or floating-point work.
  bnez a3, reverse_active
  li t0,0x20000000
  li t1,80
  mul t1,a0,t1
  add t1,t1,t0
  lw t1,296(t1)             # BR_NEURONS + offsetof(BrNeuron, pre_fire)
  bnez t1,reverse_active
  lw t1,68(t0)              # reverse_received
  addi t1,t1,1
  sw t1,68(t0)
  lw t1,148(t0)             # reverse_events
  addi t1,t1,1
  sw t1,148(t0)
  j idle
reverse_active:
  call br_reverse
  j idle
work:
  call br_work
  j idle
/* Normal C ABI arguments: destination, tag, payload0..3. */
.globl br_send
br_send:
  mv t0,a0
  mv t1,a1
  mv a0,a2
  mv a1,a3
  mv a2,a4
  mv a3,a5
  .insn r 0x0b,4,0,x0,t0,t1
  ret
