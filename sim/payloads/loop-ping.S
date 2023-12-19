_start:
  la x3, main
  csrw 0x700, x3 # Handler for tag0
  li x3, 1
  csrw 0x710, x3 # Argcnt for tag0
  li a0, 0

  csrr x2, mhartid
  addi x2, x2, -1 # Target
  beqz x2, 1f
  wfi

1: # I'm hart 1
  li x2, 99
main:
  addi a0, a0, 1
  .word 0x0025500b # Send a0 to x2
  wfi
