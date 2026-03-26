.section .text
.globl _start
_start:
  la sp, _stack_top
  call main

  # Send result via AM: x10 = return value from main
  li x11, 0xFFFF
  .word 0x00B5500B     # AM send: funct7=0, rs2=x11, rs1=x10, funct3=101, rd=x0

halt:
  j halt
