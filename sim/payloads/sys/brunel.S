#include "brunel_rule.inc"
#include "brunel_proxy_config.h"
.section .text.boot
.globl _start
_start:
  mv tp,a0
#ifdef BR_ASM_RUNTIME
  li gp,0x20000000
#endif
  bnez a0, thread_one
  # Each 128-byte stack has a base guard. Steady-state work only spills on
  # the rare full-MSHR fallback; boot and numeric self-tests also use the stack.
  li sp, 0x20003f80
  jal ra,snn_init
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
  INSTALL 0,br_completion,18,0
  # Spike delivery is safe on both SMT threads: the input accumulator uses
  # LR/SC, while the learning writeback targets are independent.
  INSTALL 1,SPIKE_ENTRY,28,0x00020002
  INSTALL 2,br_control,20,0x00030005
  INSTALL 3,br_report,20,0x00020004
#if BR_RELAY_UPDATES
  # SMT1 relays immutable home traces while SMT0 updates proxy weights. The
  # relay reserves one send, leaving the existing update margin of 17 free.
  INSTALL 4,br_proxy_update,11,0x000a0011
  INSTALL 7,br_reverse,19,0x00010012
#else
  INSTALL 4,br_reverse,12,0x000a0011
#endif
#if BR_PROXY
  # Notifications only write metadata; SMT1 can receive while SMT0 sends.
  INSTALL 5,br_proxy_receive,19,0
  INSTALL 13,br_proxy_notify,8,0x00050008
  INSTALL 14,br_proxy_wait,8,0x00010002
#endif
  INSTALL 6,br_mshr_service,8,0x00020003
  INSTALL 8,br_work,8,0x0018001f
  INSTALL 9,br_integrate,24,0x00010002
  INSTALL 10,idle,16,0
  INSTALL 11,br_forward_wait,8,0x00020013
  INSTALL 12,br_forward_drain,8,0x00020013
  jal ra,br_boot
.globl br_idle
br_idle:
idle:
  wfi
  j idle
thread_one:
  li sp,0x20004000
  j idle
