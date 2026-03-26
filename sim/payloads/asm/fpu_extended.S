  # Comprehensive FPU test suite — 20 tests
  # Uses back-to-back FP ops (no NOP padding needed).
  # Test values from RISC-V riscv-tests/rv32uf suite.
  #
  # Shared FP/INT register file: f0-f31 aliases x0-x31.
  # x20 = pass count, x21 = fail count

  .section .text
  .globl _start
_start:
  li x20, 0         # pass count
  li x21, 0         # fail count

  ############################
  # Test 1: FADD 2.5 + 1.0 = 3.5 (riscv-tests fadd #2)
  ############################
  li x15, 0x40200000       # 2.5
  mv x10, x15
  li x15, 0x3F800000       # 1.0
  mv x11, x15
  fadd.s f12, f10, f11     # back-to-back with setup (mv is int)
  nop
  li x13, 0x40600000       # 3.5
  beq x12, x13, t1p
  addi x21, x21, 1
  j t2
t1p: addi x20, x20, 1

  ############################
  # Test 2: FSUB 2.5 - 1.0 = 1.5 (riscv-tests fsub #5)
  ############################
t2:
  fsub.s f12, f10, f11     # consecutive FP: f10=2.5, f11=1.0
  nop
  li x13, 0x3FC00000       # 1.5
  beq x12, x13, t2p
  addi x21, x21, 1
  j t3
t2p: addi x20, x20, 1

  ############################
  # Test 3: FMUL 2.5 * 1.0 = 2.5 (riscv-tests fmul #8)
  ############################
t3:
  fmul.s f12, f10, f11     # consecutive FP
  nop
  li x13, 0x40200000       # 2.5
  beq x12, x13, t3p
  addi x21, x21, 1
  j t4
t3p: addi x20, x20, 1

  ############################
  # Test 4: FCVT.S.W(3) then FCVT.S.W(4) then FADD = 7.0
  # back-to-back fcvt.s.w instructions
  ############################
t4:
  li x15, 3
  fcvt.s.w f10, x15
  li x15, 4
  fcvt.s.w f11, x15        # back-to-back FP
  fadd.s f12, f10, f11     # back-to-back FP
  nop
  li x13, 0x40E00000       # 7.0
  beq x12, x13, t4p
  addi x21, x21, 1
  j t5
t4p: addi x20, x20, 1

  ############################
  # Test 5: FSGNJ copy sign(+) to |-3.0| = +3.0
  ############################
t5:
  li x15, 0xC0400000       # -3.0
  mv x10, x15
  li x15, 0x40800000       # 4.0
  mv x11, x15
  fsgnj.s f12, f10, f11
  nop
  li x13, 0x40400000       # +3.0
  beq x12, x13, t5p
  addi x21, x21, 1
  j t6
t5p: addi x20, x20, 1

  ############################
  # Test 6: FSGNJN negate sign => -3.0
  ############################
t6:
  fsgnjn.s f12, f10, f11   # consecutive FP
  nop
  li x13, 0xC0400000       # -3.0
  beq x12, x13, t6p
  addi x21, x21, 1
  j t7
t6p: addi x20, x20, 1

  ############################
  # Test 7: FSGNJX xor signs => -3.0
  ############################
t7:
  fsgnjx.s f12, f10, f11   # consecutive FP
  nop
  li x13, 0xC0400000
  beq x12, x13, t7p
  addi x21, x21, 1
  j t8
t7p: addi x20, x20, 1

  ############################
  # Test 8: FMIN(-3.0, 4.0) = -3.0
  ############################
t8:
  fmin.s f12, f10, f11     # consecutive FP
  nop
  li x13, 0xC0400000
  beq x12, x13, t8p
  addi x21, x21, 1
  j t9
t8p: addi x20, x20, 1

  ############################
  # Test 9: FMAX(-3.0, 4.0) = 4.0
  ############################
t9:
  fmax.s f12, f10, f11     # consecutive FP
  nop
  li x13, 0x40800000
  beq x12, x13, t9p
  addi x21, x21, 1
  j t10
t9p: addi x20, x20, 1

  ############################
  # Test 10: FLE -3.0 <= 4.0 => 1
  ############################
t10:
  fle.s x14, f10, f11      # consecutive FP
  nop
  li x15, 1
  beq x14, x15, t10p
  addi x21, x21, 1
  j t11
t10p: addi x20, x20, 1

  ############################
  # Test 11: FLT -3.0 < 4.0 => 1
  ############################
t11:
  flt.s x14, f10, f11      # consecutive FP
  nop
  li x15, 1
  beq x14, x15, t11p
  addi x21, x21, 1
  j t12
t11p: addi x20, x20, 1

  ############################
  # Test 12: FEQ 4.0 == 4.0 => 1
  ############################
t12:
  feq.s x14, f11, f11      # consecutive FP
  nop
  li x15, 1
  beq x14, x15, t12p
  addi x21, x21, 1
  j t13
t12p: addi x20, x20, 1

  ############################
  # Test 13: FCVT.S.W(-5) => -5.0
  ############################
t13:
  li x15, -5
  fcvt.s.w f12, x15
  nop
  li x13, 0xC0A00000       # -5.0
  beq x12, x13, t13p
  addi x21, x21, 1
  j t14
t13p: addi x20, x20, 1

  ############################
  # Test 14: FCVT.W.S(-5.0) => -5
  ############################
t14:
  fcvt.w.s x14, f12        # consecutive FP after test 13
  nop
  li x15, -5
  beq x14, x15, t14p
  addi x21, x21, 1
  j t15
t14p: addi x20, x20, 1

  ############################
  # Test 15: FCVT.S.WU + FCVT.WU.S roundtrip
  # uint(100) => 100.0 => uint(100)
  ############################
t15:
  li x15, 100
  fcvt.s.wu f12, x15
  fcvt.wu.s x14, f12       # back-to-back FP
  nop
  li x15, 100
  beq x14, x15, t15p
  addi x21, x21, 1
  j t16
t15p: addi x20, x20, 1

  ############################
  # Test 16: FCVT.W.S with RTZ rounding
  # 3.7 => 3
  ############################
t16:
  li x15, 0x404CCCCD       # 3.7
  mv x12, x15
  fcvt.w.s x14, f12, rtz
  nop
  li x15, 3
  beq x14, x15, t16p
  addi x21, x21, 1
  j t17
t16p: addi x20, x20, 1

  ############################
  # Test 17: Negative truncation
  # -3.7 => -3 (RTZ)
  ############################
t17:
  li x15, 0xC04CCCCD       # -3.7
  mv x12, x15
  fcvt.w.s x14, f12, rtz   # consecutive FP
  nop
  li x15, -3
  beq x14, x15, t17p
  addi x21, x21, 1
  j t18
t17p: addi x20, x20, 1

  ############################
  # Test 18: 0.0 + 0.0 = 0.0
  ############################
t18:
  li x15, 0
  fcvt.s.w f10, x15
  fcvt.s.w f11, x15        # back-to-back FP
  fadd.s f12, f10, f11     # back-to-back FP
  nop
  beq x12, x0, t18p
  addi x21, x21, 1
  j t19
t18p: addi x20, x20, 1

  ############################
  # Test 19: Large multiply
  # 1000 * 1000 = 1000000
  ############################
t19:
  li x15, 1000
  fcvt.s.w f10, x15
  fmul.s f12, f10, f10     # back-to-back FP
  nop
  li x13, 0x49742400       # 1000000.0
  beq x12, x13, t19p
  addi x21, x21, 1
  j t20
t19p: addi x20, x20, 1

  ############################
  # Test 20: FADD chain: 1.0 + 1.0 + 1.0 + 1.0 = 4.0
  # Tests three consecutive FADD operations
  ############################
t20:
  li x15, 0x3F800000       # 1.0
  mv x10, x15
  mv x11, x15
  fadd.s f12, f10, f11     # 1.0 + 1.0 = 2.0
  fadd.s f13, f12, f10     # 2.0 + 1.0 = 3.0 (back-to-back FP)
  fadd.s f12, f13, f10     # 3.0 + 1.0 = 4.0 (back-to-back FP)
  nop
  li x13, 0x40800000       # 4.0
  beq x12, x13, t20p
  addi x21, x21, 1
  j done
t20p: addi x20, x20, 1

  ############################
  # Done: send results
  ############################
done:
  sw x20, 0(x0)
  sw x21, 4(x0)
  mv x10, x20
  li x11, 0xFFFF
  .word 0x00B5500B

halt:
  j halt
