  # ICache eviction test
  # This program is larger than the ICache (>1024 bytes = 256 instructions)
  # to force eviction. It loops back to the beginning multiple times.
  # Uses scratchpad memory to track loop count.
  # Signals end-of-simulation via AM send to dst=0xFFFF, tag=0 with loop count as data.

  .section .text
  .globl _start
_start:
  bnez x10, .park
  # Initialize: store loop counter = 0 to scratchpad addr 0x20000000
  li x10, 0           # x10 = loop counter
  li x9, 0x20000000   # x9 = SPM base
  sw x10, 0(x9)       # scratchpad[0] = 0

  # Total iterations target
  li x12, 3           # x12 = total number of loops

loop_start:
  # Load counter from scratchpad
  lw x10, 0(x9)

  # Increment counter
  addi x10, x10, 1

  # Store updated counter
  sw x10, 0(x9)

  # NOPs to fill code beyond ICache capacity
  # ICache = 8 sets x 2 ways x 64 bytes = 1024 bytes = 256 instructions
  # We need > 256 instructions. Let's put 300+ NOPs.
  .rept 320
  nop
  .endr

  # Check if we've looped enough
  lw x10, 0(x9)       # reload counter from scratchpad
  blt x10, x12, loop_start

  # Done: signal success via peripheral
  li t0, 0x40000000
  sw zero, 0(t0)       # result=0 → success

  # Halt: infinite loop (WFI)
wfi_loop:
  j wfi_loop

.park:
  wfi
  j .park
