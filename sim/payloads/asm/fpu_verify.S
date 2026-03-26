  # FPU verification test (careful about shared FP/INT register file)
  # x20 = pass count, x21 = fail count
  
  .section .text
  .globl _start
_start:
  li x20, 0         # pass count
  li x21, 0         # fail count

  ############################
  # Test 1: FCVT.S.W + FADD
  # Convert 3 and 4 to float, add them = 7.0
  ############################
  li x15, 3
  fcvt.s.w f10, x15          # f10/x10 = 3.0 = 0x40400000
  nop
  li x15, 4
  fcvt.s.w f11, x15          # f11/x11 = 4.0 = 0x40800000
  nop
  fadd.s f12, f10, f11       # f12/x12 = 7.0 = 0x40E00000
  nop
  nop
  # Check: x12 should be 0x40E00000
  li x13, 0x40E00000
  beq x12, x13, test1_pass
  addi x21, x21, 1
  j test2
test1_pass:
  addi x20, x20, 1

  ############################
  # Test 2: FSUB
  # 7.0 - 4.0 = 3.0
  ############################
test2:
  # f12 = 7.0, f11 = 4.0 (from test1)
  fsub.s f13, f12, f11       # f13/x13 = 3.0 = 0x40400000
  nop
  nop
  li x14, 0x40400000
  beq x13, x14, test2_pass
  addi x21, x21, 1
  j test3
test2_pass:
  addi x20, x20, 1

  ############################
  # Test 3: FMUL
  # 3.0 * 4.0 = 12.0
  ############################
test3:
  # f10 = 3.0, f11 = 4.0
  fmul.s f12, f10, f11       # f12/x12 = 12.0 = 0x41400000
  nop
  nop
  li x13, 0x41400000
  beq x12, x13, test3_pass
  addi x21, x21, 1
  j test4
test3_pass:
  addi x20, x20, 1

  ############################
  # Test 4: FCVT.W.S (float to int)
  # Convert 12.0 to int => 12
  ############################
test4:
  # f12 = 12.0
  fcvt.w.s x14, f12          # x14 = 12
  nop
  nop
  li x15, 12
  beq x14, x15, test4_pass
  addi x21, x21, 1
  j test5
test4_pass:
  addi x20, x20, 1

  ############################
  # Test 5: FEQ
  # Reload 3.0, compare with itself => 1
  ############################
test5:
  li x15, 3
  fcvt.s.w f10, x15         # f10 = 3.0
  nop
  feq.s x14, f10, f10       # x14 = (3.0 == 3.0) ? 1 : 0
  nop
  nop
  li x15, 1
  beq x14, x15, test5_pass
  addi x21, x21, 1
  j test6
test6_pass_jmp:
  j test6_pass
test5_pass:
  addi x20, x20, 1

  ############################
  # Test 6: FLT
  # 3.0 < 4.0 => 1
  ############################
test6:
  li x15, 4
  fcvt.s.w f11, x15         # f11 = 4.0
  nop
  flt.s x14, f10, f11       # x14 = (3.0 < 4.0) ? 1 : 0
  nop
  nop
  li x15, 1
  beq x14, x15, test6_pass_jmp
  addi x21, x21, 1
  j done
test6_pass:
  addi x20, x20, 1

  ############################
  # Done: send results
  ############################
done:
  sw x20, 0(x0)              # scratchpad[0] = pass count
  sw x21, 4(x0)              # scratchpad[4] = fail count

  # Send pass count
  mv x10, x20
  li x11, 0xFFFF
  .word 0x00B5500B

halt:
  j halt
