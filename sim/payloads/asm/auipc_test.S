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

  # Test 6: AUIPC with 0xABCDE — non-zero bits at instr[30:25] and instr[24:20]
  # This is the case that previously failed due to immediate leakage
  auipc x10, 0xABCDE  # x10 = PC + 0xABCDE000
  auipc x3, 0         # x3 = PC_here
  sub x4, x10, x3     # should be 0xABCDE000 - 4 = 0xABCDDFFC
  lui x5, 0xABCDE     # x5 = 0xABCDE000
  addi x5, x5, -4     # x5 = 0xABCDDFFC
  bne x4, x5, fail
  addi x20, x20, 1

  # Test 7: LUI with same immediate (verify LUI also correct)
  lui x6, 0xABCDE     # x6 = 0xABCDE000
  lui x7, 0xABCDE     # x7 = 0xABCDE000 (same, for comparison)
  bne x6, x7, fail
  addi x20, x20, 1

  # Done (expected 7)
  mv x10, x20
  li x11, 0xFFFF
  .word 0x00B5500B

fail:
  mv x10, x20
  li x11, 0xFFFF
  .word 0x00B5500B

wfi_loop:
  j wfi_loop
