/* Functional helper test: real RV32 low-word carries, executed only by PU1.
 * (2^32 - 1) + 2 + (2^32 - 1) = 2^33; the host checks the emitted frame.
 */
#include "spike_count.h"
.section .text
.globl _start
_start:
  bnez a0, idle
  csrr t0, 0xf14
  li t1, 1
  bne t0, t1, idle
  SPIKE_COUNT_INIT (0x20000000 + SPIKE_COUNT_MNIST), t0
  li t0, 0x40000004
  sw t1, 0(t0)
  li a0, -1
  SPIKE_COUNT_ADD a0, (0x20000000 + SPIKE_COUNT_MNIST), t0, t1, t2, t3
  li a0, 2
  SPIKE_COUNT_ADD a0, (0x20000000 + SPIKE_COUNT_MNIST), t0, t1, t2, t3
  li a0, -1
  SPIKE_COUNT_ADD a0, (0x20000000 + SPIKE_COUNT_MNIST), t0, t1, t2, t3
  li t0, 0x40000004
  sw zero, 0(t0)
  SPIKE_COUNT_EMIT (0x20000000 + SPIKE_COUNT_MNIST), t0, t1, t2
  li t0, 0x40000000
  sw zero, 0(t0)
idle:
  wfi
  j idle
