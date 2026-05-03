  # Comprehensive FPU test suite — 20 tests (Zfinx: shared FP/INT register file)
  # Branchless flow: no forward jumps (avoids ICache alignment issues).
  # x21 = fail count (0=success)

  .section .text
  .globl _start
_start:
  bnez x10, .park
  li x21, 0         # fail count

  # Test 1: FADD 2.5 + 1.0 = 3.5
  li x10, 0x40200000       # 2.5
  li x11, 0x3F800000       # 1.0
  fadd.s f12, f10, f11
  nop
  nop
  li x13, 0x40600000       # 3.5
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 2: FSUB 2.5 - 1.0 = 1.5
  fsub.s f12, f10, f11
  nop
  nop
  li x13, 0x3FC00000       # 1.5
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 3: FMUL 2.5 * 1.0 = 2.5
  fmul.s f12, f10, f11
  nop
  nop
  li x13, 0x40200000       # 2.5
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 4: FCVT.S.W(3)+FCVT.S.W(4)+FADD = 7.0
  li x15, 3
  fcvt.s.w f10, x15
  li x15, 4
  fcvt.s.w f11, x15
  nop
  fadd.s f12, f10, f11
  nop
  nop
  li x13, 0x40E00000       # 7.0
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 5: FSGNJ copy sign(+) to |-3.0| = +3.0
  li x10, 0xC0400000       # -3.0
  li x11, 0x40800000       # 4.0
  fsgnj.s f12, f10, f11
  nop
  nop
  li x13, 0x40400000       # +3.0
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 6: FSGNJN negate sign => -3.0
  fsgnjn.s f12, f10, f11
  nop
  nop
  li x13, 0xC0400000       # -3.0
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 7: FSGNJX xor signs => -3.0
  fsgnjx.s f12, f10, f11
  nop
  nop
  li x13, 0xC0400000
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 8: FMIN(-3.0, 4.0) = -3.0
  fmin.s f12, f10, f11
  nop
  nop
  li x13, 0xC0400000
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 9: FMAX(-3.0, 4.0) = 4.0
  fmax.s f12, f10, f11
  nop
  nop
  li x13, 0x40800000
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 10: FLE -3.0 <= 4.0 => 1
  fle.s x14, f10, f11
  nop
  nop
  li x15, 1
  xor x14, x14, x15
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 11: FLT -3.0 < 4.0 => 1
  flt.s x14, f10, f11
  nop
  nop
  li x15, 1
  xor x14, x14, x15
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 12: FEQ 4.0 == 4.0 => 1
  feq.s x14, f11, f11
  nop
  nop
  li x15, 1
  xor x14, x14, x15
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 13: FCVT.S.W(-5) => -5.0
  li x15, -5
  fcvt.s.w f12, x15
  nop
  nop
  li x13, 0xC0A00000       # -5.0
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 14: FCVT.W.S(-5.0) => -5
  fcvt.w.s x14, f12
  nop
  nop
  li x15, -5
  xor x14, x14, x15
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 15: FCVT.S.WU + FCVT.WU.S roundtrip uint(100)
  li x15, 100
  fcvt.s.wu f12, x15
  nop
  fcvt.wu.s x14, f12
  nop
  nop
  li x15, 100
  xor x14, x14, x15
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 16: FCVT.W.S with RTZ: 3.7 => 3
  li x12, 0x404CCCCD       # 3.7
  nop
  fcvt.w.s x14, f12, rtz
  nop
  nop
  li x15, 3
  xor x14, x14, x15
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 17: Negative truncation: -3.7 => -3 (RTZ)
  li x12, 0xC04CCCCD       # -3.7
  nop
  fcvt.w.s x14, f12, rtz
  nop
  nop
  li x15, -3
  xor x14, x14, x15
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 18: 0.0 + 0.0 = 0.0
  li x10, 0
  li x11, 0
  fcvt.s.w f10, x10
  fcvt.s.w f11, x11
  nop
  fadd.s f12, f10, f11
  nop
  nop
  sltu x14, x0, x12
  add x21, x21, x14

  # Test 19: Large multiply: 1000 * 1000 = 1000000
  li x15, 1000
  fcvt.s.w f10, x15
  nop
  fmul.s f12, f10, f10
  nop
  nop
  li x13, 0x49742400       # 1000000.0
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Test 20: FADD chain: 1.0+1.0+1.0+1.0 = 4.0
  li x10, 0x3F800000       # 1.0
  li x11, 0x3F800000       # 1.0
  fadd.s f12, f10, f11     # 2.0
  nop
  fadd.s f13, f12, f10     # 3.0
  nop
  fadd.s f12, f13, f10     # 4.0
  nop
  nop
  li x13, 0x40800000       # 4.0
  xor x14, x12, x13
  sltu x14, x0, x14
  add x21, x21, x14

  # Done: signal end via peripheral
  li t0, 0x40000000
  sw x21, 0(t0)              # result: 0=success, nonzero=failure count

halt:
  j halt

.park:
  wfi
  j .park
