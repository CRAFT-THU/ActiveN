  .section .text
  .globl _start
_start:
  mv s8, a0
  la t0, spike
  csrw 0x700, t0 # Handler for tag0
  li t0, 2
  csrw 0x710, t0 # Argcnt for tag0

  la t0, updateOne
  csrw 0x701, t0 # Handler for tag1
  li t0, 1
  csrw 0x711, t0 # Argcnt for tag1

  csrr s2, mhartid
#   li t0, CORE_CNT / 4
#   blt s2, t0, 0f
#   li t0, CORE_CNT / 4 * 3
#   bge s2, t0, 0f
#   li s0, 0x0000FFFE
#   j skip
# 0:
  li s0, 0x0000FFFF
# skip:

  li t0, 0x00010000
  or s2, s2, t0

  li s3, 0x41700000 // 10
  li s4, 0x3f67a36d // e^{-0.1}
  li s5, 0x3f800000 // 1

  // a0 iterator, s0 dram target, s1 end, s2 self fire, s3 threshold, s4 tau, s5 injective, s8 <- a0

  li t0, 16384 - 4;
  lw t0, 0(t0) // Neuron count
  # slli s1, t0, 2 // * 20
  # add s1, s1, t0
  # slli s1, s1, 2
  slli s1, t0, 4 // * 16
  li a0, 0
  bnez s8, loopwfi

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

init:
  lw t0, 0(a0)
  lw t1, 4(a0)
  fadd.S f5, f5, f6
  sw t0, 0(a0)
  sw x0, 4(a0)
  addi a0, a0, 16
  bne a0, s1, init

  li a0, 0

updateOne:
  beq a0, s1, loopwfi
  lw t0, 0(a0)

  flt.s t1, f5, fs3
  beqz t1, 1f // Fired

  fmul.s f5, f5, fs4
  sw t0, 0(a0)
  addi a0, a0, 16
  .word 0x0325500b // fire.yield.1 a0, s2
  // wfi

1:
  sw x0, 0(a0)
  lw t0, 8(a0)
  lw t1, 12(a0)
  .word 0x0062a00b // queue.2 t0, t1
  .word 0x0080400b // fire.0 s0
  addi a0, a0, 16
  .word 0x0325500b // fire.yield.1 a0, s2
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
