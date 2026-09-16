  # Exercise the scratchpad through the Core LSU, including byte masks and AMOs.
  .option arch, +a
  .section .text
  .globl _start
_start:
  bnez a0, .park
  li t0, 0x20003fc0       # Last scratchpad line
  li t6, 1
  li t1, 0x11223344
  sw t1, 60(t0)          # Last word and last byte lane
  li t1, 0xaa
  sb t1, 61(t0)
  li t1, 0xbbcc
  sh t1, 62(t0)
  lw t2, 60(t0)
  li t3, 0xbbccaa44
  bne t2, t3, .fail

  li t6, 2
  lbu t2, 61(t0)
  li t3, 0xaa
  bne t2, t3, .fail
  lhu t2, 62(t0)
  li t3, 0xbbcc
  bne t2, t3, .fail

  li t6, 3
  addi t0, t0, 60
  li t1, 1
  amoadd.w t2, t1, (t0)
  li t3, 0xbbccaa44
  bne t2, t3, .fail
  lw t2, 0(t0)
  addi t3, t3, 1
  bne t2, t3, .fail

  li t6, 4
  li t1, 0x13579bdf
  amoswap.w t2, t1, (t0)
  bne t2, t3, .fail
  lw t2, 0(t0)
  bne t2, t1, .fail

  li t6, 5
  lr.w t2, (t0)
  bne t2, t1, .fail
  li t1, 0xdeadbeef
  sc.w t2, t1, (t0)
  bnez t2, .fail
  lw t2, 0(t0)
  bne t2, t1, .fail

  li t6, 6
  li t0, 0x20000000      # Check the first row independently of the last row
  li t1, 0x2468ace0
  sw t1, 0(t0)
  lw t2, 0(t0)
  bne t2, t1, .fail
  li t0, 0x20003ffc
  lw t2, 0(t0)
  li t1, 0xdeadbeef
  bne t2, t1, .fail

  li t0, 0x40000000
  sw zero, 0(t0)
.park:
  wfi
  j .park
.fail:
  li t0, 0x40000000
  sw t6, 0(t0)
  j .park
