.section .text
.globl _start
_start:
  bnez x10, .park
  la sp, _stack_top
  call main

  # Signal end-of-simulation via peripheral write
  # a0 (x10) = return value from main (0 = success)
  li t0, 0x40000000
  sw a0, 0(t0)

halt:
  j halt

.park:
  wfi
  j .park
