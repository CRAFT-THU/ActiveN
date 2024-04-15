  .section .text
  .globl _start
_start:

  la t0, spike
  csrw 0x700, t0 # Handler for tag0
  li t0, 2
  csrw 0x710, t0 # Argcnt for tag0

  la t0, updateOne
  csrw 0x701, t0 # Handler for tag1
  li t0, 1
  csrw 0x711, t0 # Argcnt for tag1

  csrr s2, mhartid
  li t0, 0x00010000
  or s2, s2, t0

  li s3, 0x41700000 // 10
  li s4, 0x3f67a36d // e^{-0.1}
  li s5, 0x3f800000 // 1

  // s0 iterator, s1 end, s2 self fire, s3 threshold, s4 tau, s5 injective

  li t0, 16384 - 4;
  lw t0, 0(t0) // Neuron count
  # slli s1, t0, 2 // * 20
  # add s1, s1, t0
  # slli s1, s1, 2
  slli s1, t0, 4 // * 16
  li s0, 0

# adder:
#   lw t0, 0(s0)
#   lw t1, 4(s0)
#   fadd.S f5, f5, f6
#   sw t0, 0(s0)
#   sw s5, 4(s0)
#   addi s0, s0, 16
#   bne s1, s0, adder
# 
#   li s0, 0

updateOne:
  beq s0, s1, loopwfi
  lw t0, 0(s0)
  lw t1, 4(s0)
  fadd.S f5, f5, f6

  flt.s t1, f5, fs3
  beqz t1, 1f // Fired

  fmul.s f5, f5, fs4
  sw t0, 0(s0)
  addi s0, s0, 16
  .word 0x0320500b // fire.yield.1 x0, s2
  // wfi

1:
  sw x0, 0(s0)
  lw t0, 8(s0)
  lw t1, 12(s0)
  .word 0x0062a00b // queue.2 t0, t1
  .word 0x0000400b // fire.0 x0
  addi s0, s0, 16
  .word 0x0320500b // fire.yield.1 x0, s2
  # wfi

spike:
  // a0 is neuron id
  // a1 is weight
  slli a0, a0, 4 // x16
  lw t0, 4(a0)
  fadd.s f5, f5, fa1
  sw t0, 4(a0)
  wfi

loopwfi:
  wfi
