.section .text.boot
.globl _start
_start:
  mv tp,a0
#ifdef BR_ASM_RUNTIME
  li gp,0x20000000
#endif
  bnez a0, thread_one
  # The deepest call chain is 96 bytes. Each 128-byte stack has a base guard.
  li sp, 0x20003f80
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
  INSTALL 1,br_spike,20,0x00020002
  INSTALL 2,control,20,0x00030005
  INSTALL 3,report,20,0x00020004
  INSTALL 4,reverse,12,0x000a0011
  INSTALL 6,mshr_service,8,0x00020003
  INSTALL 8,work,8,0x0018001f
  INSTALL 9,integrate,24,0x00010002
  INSTALL 10,idle,16,0
  INSTALL 11,br_forward_wait,8,0x00020013
  INSTALL 12,br_forward_drain,8,0x00020013
  call br_boot
.globl br_idle
br_idle:
idle:
  wfi
  j idle
thread_one:
  li sp,0x20004000
  j idle
completion:
  call br_completion
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
  j br_work
integrate:
  call br_integrate
  j idle
mshr_service:
  call br_mshr_service
  j idle
