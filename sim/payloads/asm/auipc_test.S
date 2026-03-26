  # Test AUIPC to various destination registers
  # Instead of checking exact values, compute difference between two AUIPCs
  .section .text
  .globl _start
_start:
  li x20, 0       # pass count

  # Test 1: AUIPC to x2 (sp), funct3 bits = 000
  auipc x2, 0     # x2 = PC_here
  auipc x3, 0     # x3 = PC_here (4 bytes later)
  sub x4, x3, x2  # should be 4
  li x5, 4
  bne x4, x5, fail
  addi x20, x20, 1

  # Test 2: AUIPC to x10 (a0), funct3 bits = 001
  auipc x10, 0    # x10 = PC_here
  auipc x3, 0     # x3 = PC_here + 4
  sub x4, x3, x10
  li x5, 4
  bne x4, x5, fail
  addi x20, x20, 1

  # Test 3: AUIPC to x18 (s2), funct3 bits = 010
  auipc x18, 0
  auipc x3, 0
  sub x4, x3, x18
  li x5, 4
  bne x4, x5, fail
  addi x20, x20, 1

  # Test 4: AUIPC to x26, funct3 bits = 011
  auipc x26, 0
  auipc x3, 0
  sub x4, x3, x26
  li x5, 4
  bne x4, x5, fail
  addi x20, x20, 1

  # Test 5: AUIPC with non-zero upper imm, to x10
  auipc x10, 1    # x10 = PC + 0x1000
  auipc x3, 0     # x3 = PC_here
  sub x4, x10, x3 # should be 0x1000 - 4 = 0xFFC
  li x5, 0xFFC
  bne x4, x5, fail
  addi x20, x20, 1

  # Done (expected 5)
  mv x10, x20
  li x11, 0xFFFF
  .word 0x00B5500B

fail:
  mv x10, x20
  li x11, 0xFFFF
  .word 0x00B5500B

wfi_loop:
  j wfi_loop
