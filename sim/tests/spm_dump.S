/* Functional SPM snapshot fixture. All bytes have a deterministic value.
 * Only SMT0 runs; PU1 waits for every PU's post-copy report before stopping.
 */
#ifndef SPM_BYTES
#define SPM_BYTES 16384
#endif
#ifndef LINE_BYTES
#define LINE_BYTES 32
#endif
#ifndef FAIL_CODE
#define FAIL_CODE 0
#endif
.option norelax
.section .text
.globl _start
_start:
  bnez a0, idle
  csrr s0, 0xf14
  li t0, 0x68000000
  lw s1, 0(t0)
  li s2, 0
  li t0, 1
  bne s0, t0, fill_start
  li t0, 0x40000004
  li t1, 1
  sw t1, 0(t0)
fill_start:
  slli s3, s0, 20
  li t0, 0x9e3779b9
  xor s3, s3, t0
  li t0, 0x20000000
  li t1, SPM_BYTES / 4
  li t2, 0
fill:
  xor t3, s3, t2
  sw t3, 0(t0)
  addi t0, t0, 4
  addi t2, t2, 1
  bne t2, t1, fill

  // Exercise every byte lane and a line boundary using byte writes.
  li t0, 0x20000000
  li t1, 2 * LINE_BYTES
  li t2, 0
byte_writes:
  slli t3, t2, 1
  add t3, t3, t2
  add t3, t3, s0
  sb t3, 0(t0)
  addi t0, t0, 1
  addi t2, t2, 1
  bne t2, t1, byte_writes

  // Cover halfword masks through the final byte of SPM.
  li t0, 0x20000000 + SPM_BYTES - LINE_BYTES
  li t1, SPM_BYTES
  li t2, SPM_BYTES - LINE_BYTES
  li t4, 0xbeef
halfword_writes:
  xor t3, t4, s0
  xor t3, t3, t2
  sh t3, 0(t0)
  addi t0, t0, 2
  addi t2, t2, 2
  bne t2, t1, halfword_writes

  // Ordinary LSU asynchronous copy, including multiple memory lines.
  la t0, copy_source
  csrw 0x731, t0
  li t0, 0x20000100
  csrw 0x732, t0
  li t0, 3 * LINE_BYTES
  csrw 0x733, t0
  csrw 0x730, zero
copy_wait:
  csrr t0, 0x730
  bnez t0, copy_wait

  li t0, 1
  bne s0, t0, report
  la t0, done
  csrw 0x700, t0
  csrw 0x720, zero
  li t0, (1 << 3) | 1
  csrw 0x710, t0
report:
  mv a0, s0
  li t0, 1
  li t1, 0
  .insn r 0x0b, 4, 0, x0, t0, t1
idle:
  wfi
  j idle
done:
  addi s2, s2, 1
  bne s2, s1, idle
  li t0, 0x40000004
  sw zero, 0(t0)
  sw s2, 8(t0)
  li t0, 0x40000000
  li t1, FAIL_CODE
  sw t1, 0(t0)
  j idle

.section .data
.balign LINE_BYTES
copy_source:
  .set word_index, 0
  .rept 3 * LINE_BYTES / 4
    .word (0x76543210 + word_index * 0x01020304) & 0xffffffff
    .set word_index, word_index + 1
  .endr
