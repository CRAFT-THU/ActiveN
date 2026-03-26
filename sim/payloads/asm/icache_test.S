  # ICache eviction test
  # This program is larger than the ICache (>1024 bytes = 256 instructions)
  # to force eviction. It loops back to the beginning multiple times.
  # Uses scratchpad memory to track loop count.
  # Signals end-of-simulation via AM send to dst=0xFFFF, tag=0 with loop count as data.

  .section .text
  .globl _start
_start:
  # Initialize: store loop counter = 0 to scratchpad addr 0
  li x10, 0           # x10 = loop counter
  sw x10, 0(x0)       # scratchpad[0] = 0

  # Total iterations target
  li x12, 3           # x12 = total number of loops

loop_start:
  # Load counter from scratchpad
  lw x10, 0(x0)

  # Increment counter
  addi x10, x10, 1

  # Store updated counter
  sw x10, 0(x0)

  # NOPs to fill code beyond ICache capacity
  # ICache = 8 sets x 2 ways x 64 bytes = 1024 bytes = 256 instructions
  # We need > 256 instructions. Let's put 300+ NOPs.
  .rept 320
  nop
  .endr

  # Check if we've looped enough
  lw x10, 0(x0)       # reload counter from scratchpad
  blt x10, x12, loop_start

  # Done: send result via AM instruction
  # x10 = loop count (result), x11 = 0x0000FFFF (tag=0, dst=0xFFFF)
  li x11, 0xFFFF
  .word 0x00B5500B     # AM send: funct7=0, rs2=x11, rs1=x10, funct3=101, rd=x0

  # Halt: infinite loop (WFI)
wfi_loop:
  j wfi_loop
