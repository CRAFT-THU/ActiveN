  # FPU verification test (Zfinx: shared FP/INT register file)
  # x21 = fail count (0=success)

  .section .text
  .globl _start
_start:
  bnez x10, .park
  li x21, 0         # fail count

  ############################
  # Test 1: FCVT.S.W + FADD
  # Convert 3 and 4 to float, add them = 7.0
  ############################
  li x15, 3
  fcvt.s.w f10, x15          # x10 = 3.0 = 0x40400000
  li x15, 4
  fcvt.s.w f11, x15          # x11 = 4.0 = 0x40800000
  nop
  fadd.s f12, f10, f11       # x12 = 7.0 = 0x40E00000
  nop
  nop
  li x13, 0x40E00000
  beq x12, x13, 1f
  addi x21, x21, 1
1:

  ############################
  # Test 2: FSUB
  # 7.0 - 4.0 = 3.0
  ############################
  fsub.s f13, f12, f11       # x13 = 3.0 = 0x40400000
  nop
  nop
  li x14, 0x40400000
  beq x13, x14, 1f
  addi x21, x21, 1
1:

  ############################
  # Test 3: FMUL
  # 3.0 * 4.0 = 12.0
  ############################
  fmul.s f12, f10, f11       # x12 = 12.0 = 0x41400000
  nop
  nop
  li x13, 0x41400000
  beq x12, x13, 1f
  addi x21, x21, 1
1:

  ############################
  # Test 4: FCVT.W.S (float to int)
  # Convert 12.0 to int => 12
  ############################
  fcvt.w.s x14, f12          # x14 = 12
  nop
  nop
  li x15, 12
  beq x14, x15, 1f
  addi x21, x21, 1
1:

  ############################
  # Test 5: FEQ
  # Reload 3.0, compare with itself => 1
  ############################
  li x15, 3
  fcvt.s.w f10, x15         # x10 = 3.0
  nop
  feq.s x14, f10, f10       # x14 = 1
  nop
  nop
  li x15, 1
  beq x14, x15, 1f
  addi x21, x21, 1
1:

  ############################
  # Test 6: FLT
  # 3.0 < 4.0 => 1
  ############################
  li x15, 4
  fcvt.s.w f11, x15         # x11 = 4.0
  nop
  flt.s x14, f10, f11       # x14 = 1
  nop
  nop
  li x15, 1
  beq x14, x15, 1f
  addi x21, x21, 1
1:

  ############################
  # Done: signal end via peripheral
  ############################
  li t0, 0x40000000
  sw x21, 0(t0)              # result: 0=success, nonzero=failure count

halt:
  j halt

.park:
  wfi
  j .park
