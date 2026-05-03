  # Memory copy test - copies data between SPM and global memory
  # Branchless verification: avoids ICache forward-branch alignment bug
  # x20 = pass count, result = 10 - x20 (0=success when all 10 pass)
  # SPM: 0x20000000+

  .section .text
  .globl _start
_start:
  bnez x10, .park
  li x20, 0       # pass counter

  # Phase 1: Write test pattern to SPM
  li x10, 0x20000000
  li x11, 0xDEAD0001
  sw x11, 0(x10)
  li x11, 0xBEEF0002
  sw x11, 4(x10)
  li x11, 0xCAFE0003
  sw x11, 8(x10)
  li x11, 0xF00D0004
  sw x11, 12(x10)

  # Phase 2: Copy SPM -> Global memory
  li x10, 0x20000000
  la x12, data_region

  lw x11, 0(x10)
  sw x11, 0(x12)
  lw x11, 4(x10)
  sw x11, 4(x12)
  lw x11, 8(x10)
  sw x11, 8(x12)
  lw x11, 12(x10)
  sw x11, 12(x12)

  # Phase 3: Verify global memory (4 checks, branchless)
  lw x11, 0(x12)
  li x13, 0xDEAD0001
  xor x14, x11, x13
  sltiu x14, x14, 1
  add x20, x20, x14

  lw x11, 4(x12)
  li x13, 0xBEEF0002
  xor x14, x11, x13
  sltiu x14, x14, 1
  add x20, x20, x14

  lw x11, 8(x12)
  li x13, 0xCAFE0003
  xor x14, x11, x13
  sltiu x14, x14, 1
  add x20, x20, x14

  lw x11, 12(x12)
  li x13, 0xF00D0004
  xor x14, x11, x13
  sltiu x14, x14, 1
  add x20, x20, x14

  # Phase 4: Modify in global memory, copy back to SPM
  li x11, 0x11111111
  sw x11, 0(x12)
  li x11, 0x22222222
  sw x11, 4(x12)
  li x11, 0x33333333
  sw x11, 8(x12)
  li x11, 0x44444444
  sw x11, 12(x12)

  li x10, 0x20000000
  lw x11, 0(x12)
  sw x11, 0(x10)
  lw x11, 4(x12)
  sw x11, 4(x10)
  lw x11, 8(x12)
  sw x11, 8(x10)
  lw x11, 12(x12)
  sw x11, 12(x10)

  # Phase 5: Verify SPM has updated values (4 checks, branchless)
  lw x11, 0(x10)
  li x13, 0x11111111
  xor x14, x11, x13
  sltiu x14, x14, 1
  add x20, x20, x14

  lw x11, 4(x10)
  li x13, 0x22222222
  xor x14, x11, x13
  sltiu x14, x14, 1
  add x20, x20, x14

  lw x11, 8(x10)
  li x13, 0x33333333
  xor x14, x11, x13
  sltiu x14, x14, 1
  add x20, x20, x14

  lw x11, 12(x10)
  li x13, 0x44444444
  xor x14, x11, x13
  sltiu x14, x14, 1
  add x20, x20, x14

  # Phase 6: Second round trip (2 checks)
  li x11, 0xAAAA5555
  sw x11, 0(x10)
  li x11, 0x5555AAAA
  sw x11, 4(x10)

  lw x11, 0(x10)
  sw x11, 16(x12)
  lw x11, 4(x10)
  sw x11, 20(x12)

  lw x11, 16(x12)
  li x13, 0xAAAA5555
  xor x14, x11, x13
  sltiu x14, x14, 1
  add x20, x20, x14

  lw x11, 20(x12)
  li x13, 0x5555AAAA
  xor x14, x11, x13
  sltiu x14, x14, 1
  add x20, x20, x14

  # Done: result = 10 - pass_count (0=success)
  li t0, 0x40000000
  li t1, 10
  sub t1, t1, x20
  sw t1, 0(t0)

wfi_loop:
  j wfi_loop

.park:
  wfi
  j .park

  .align 4
data_region:
  .space 64
