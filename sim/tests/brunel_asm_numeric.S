/* Hand-written cross-check for the assembly arithmetic, linked only by the
 * BR_ASM_SELFTEST build. Golden words come from the independently compiled C
 * numeric helper; the regular benchmark contains neither this code nor data. */
.option norelax
.text
.globl br_asm_selftest
br_asm_selftest:
 addi sp,sp,-32
 sw ra,0(sp)
 sw s0,4(sp)
 sw s1,8(sp)
 sw s2,12(sp)
 lw t0,12(gp)
 sw t0,16(sp)
 la s0,br_pow_vectors
 li s1,16
1: lw a0,0(s0)
 call br_pow04
 lw t0,4(s0)
 bne a0,t0,8f
 addi s0,s0,8
 addi s1,s1,-1
 bnez s1,1b
 la s0,br_update_vectors
 li s1,12
2: lw t0,0(s0)
 sw t0,12(gp)
 lw a0,4(s0)
 lw a1,8(s0)
 lw a2,12(s0)
 call br_update
 lw t0,16(s0)
 bne a0,t0,8f
 addi s0,s0,20
 addi s1,s1,-1
 bnez s1,2b
 lw t0,16(sp)
 sw t0,12(gp)
 lw ra,0(sp)
 lw s0,4(sp)
 lw s1,8(sp)
 lw s2,12(sp)
 addi sp,sp,32
 ret
8: li t1,0x4000000c
 sw a0,0(t1)
 sw t0,0(t1)
 sw s1,0(t1)
 li a0,0xb040
 j br_halt
.section .rodata
br_pow_vectors:
.word 0x00000000,0x00000000
.word 0x00000001,0x21a8e5af
.word 0x00000100,0x2342030e
.word 0x007fffff,0x264202f9
.word 0x00800000,0x2642030e
.word 0x3a800000,0x3d800009
.word 0x3f000000,0x3f42030e
.word 0x3f800000,0x3f800009
.word 0x40000000,0x3fa8e5af
.word 0x41300000,0x40270206
.word 0x4236703b,0x40937e4d
.word 0x43000000,0x40dedc77
.word 0x50000000,0x46130896
.word 0x60000000,0x4c800009
.word 0x70000000,0x52dedc77
.word 0x7f7fffff,0x59130887
br_update_vectors:
.word 0,0x00000000,0x3dcccccd,0,0x00000000
.word 0,0x3f800000,0x3dcccccd,1,0x3f8ccccd
.word 0,0x42b40000,0x41200000,0,0x42a00000
.word 0,0x42340000,0x42c80000,1,0x42b6703b
.word 0,0x1e3ce508,0x3dcccccd,0,0x00000000
.word 0,0x42b6703b,0x3dcccccd,1,0x42b6703b
.word 1,0x00000000,0x3dcccccd,0,0x00000000
.word 1,0x3f800000,0x3dcccccd,1,0x3f8cccce
.word 1,0x42b40000,0x41200000,0,0x00000000
.word 1,0x42340000,0x42c80000,1,0x43fbb8a8
.word 1,0x1e3ce508,0x3dcccccd,0,0x1e2a0154
.word 1,0x42b6703b,0x3dcccccd,1,0x42b7a79f
