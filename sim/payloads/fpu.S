  .section .text
  .globl _start
_start:
  li x10, 0x402df854 // This is e
  li x11, 1
  fcvt.S.W f11, x11
loop:
  fmul.S f11, f11, f10
  j loop
