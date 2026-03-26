  # Memory copy test - copies data between global memory and SPM
  # Tests the global memory access path through the crossbar
  #
  # x20 = pass count, x21 = fail count
  # SPM: addresses 0x0000 - 0x3FFF
  # Global mem: starts at 0x80000000

  .section .text
  .globl _start
_start:
  li x20, 0       # pass counter
  li x21, 0       # fail counter

  ########################################
  # Phase 1: Write test pattern to SPM
  ########################################
  li x10, 0       # SPM base address
  li x11, 0xDEAD0001
  sw x11, 0(x10)
  li x11, 0xBEEF0002
  sw x11, 4(x10)
  li x11, 0xCAFE0003
  sw x11, 8(x10)
  li x11, 0xF00D0004
  sw x11, 12(x10)

  ########################################
  # Phase 2: Copy SPM -> Global memory
  # Copy 4 words from SPM[0..15] to global mem[data_region]
  ########################################
  li x10, 0             # SPM source
  la x12, data_region   # Global memory destination

  lw x11, 0(x10)
  sw x11, 0(x12)
  lw x11, 4(x10)
  sw x11, 4(x12)
  lw x11, 8(x10)
  sw x11, 8(x12)
  lw x11, 12(x10)
  sw x11, 12(x12)

  ########################################
  # Phase 3: Verify global memory
  ########################################
  lw x11, 0(x12)
  li x13, 0xDEAD0001
  bne x11, x13, fail1
  addi x20, x20, 1
  j check2
fail1:
  addi x21, x21, 1

check2:
  lw x11, 4(x12)
  li x13, 0xBEEF0002
  bne x11, x13, fail2
  addi x20, x20, 1
  j check3
fail2:
  addi x21, x21, 1

check3:
  lw x11, 8(x12)
  li x13, 0xCAFE0003
  bne x11, x13, fail3
  addi x20, x20, 1
  j check4
fail3:
  addi x21, x21, 1

check4:
  lw x11, 12(x12)
  li x13, 0xF00D0004
  bne x11, x13, fail4
  addi x20, x20, 1
  j phase4
fail4:
  addi x21, x21, 1

  ########################################
  # Phase 4: Modify in global memory, copy back to SPM
  ########################################
phase4:
  li x11, 0x11111111
  sw x11, 0(x12)
  li x11, 0x22222222
  sw x11, 4(x12)
  li x11, 0x33333333
  sw x11, 8(x12)
  li x11, 0x44444444
  sw x11, 12(x12)

  # Copy global memory -> SPM
  li x10, 0             # SPM destination
  lw x11, 0(x12)
  sw x11, 0(x10)
  lw x11, 4(x12)
  sw x11, 4(x10)
  lw x11, 8(x12)
  sw x11, 8(x10)
  lw x11, 12(x12)
  sw x11, 12(x10)

  ########################################
  # Phase 5: Verify SPM has updated values
  ########################################
  lw x11, 0(x10)
  li x13, 0x11111111
  bne x11, x13, fail5
  addi x20, x20, 1
  j check6
fail5:
  addi x21, x21, 1

check6:
  lw x11, 4(x10)
  li x13, 0x22222222
  bne x11, x13, fail6
  addi x20, x20, 1
  j check7
fail6:
  addi x21, x21, 1

check7:
  lw x11, 8(x10)
  li x13, 0x33333333
  bne x11, x13, fail7
  addi x20, x20, 1
  j check8
fail7:
  addi x21, x21, 1

check8:
  lw x11, 12(x10)
  li x13, 0x44444444
  bne x11, x13, fail8
  addi x20, x20, 1
  j phase6
fail8:
  addi x21, x21, 1

  ########################################
  # Phase 6: Second round trip (SPM -> global -> SPM)
  ########################################
phase6:
  # Write new pattern to SPM
  li x11, 0xAAAA5555
  sw x11, 0(x10)
  li x11, 0x5555AAAA
  sw x11, 4(x10)

  # Copy SPM -> global (offset +16)
  lw x11, 0(x10)
  sw x11, 16(x12)
  lw x11, 4(x10)
  sw x11, 20(x12)

  # Verify global
  lw x11, 16(x12)
  li x13, 0xAAAA5555
  bne x11, x13, fail9
  addi x20, x20, 1
  j check10
fail9:
  addi x21, x21, 1

check10:
  lw x11, 20(x12)
  li x13, 0x5555AAAA
  bne x11, x13, fail10
  addi x20, x20, 1
  j done
fail10:
  addi x21, x21, 1

  ########################################
  # Done: send result (pass count)
  # Expected: 10 passes
  ########################################
done:
  mv x10, x20
  li x11, 0xFFFF
  .word 0x00B5500B     # AM send: funct7=0, rs2=x11, rs1=x10, funct3=101, rd=x0

wfi_loop:
  j wfi_loop

  .align 4
data_region:
  .space 64
