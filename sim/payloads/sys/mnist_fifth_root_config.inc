/* Constants private to each receiving SMT thread. Two-trace input PUs
 * restore these before completing input preparation/decay. The
 * phase-start, reduction, and frame handlers preserve them until reverse
 * completion. a0-a3 are message registers and cannot hold persistent values.
 * One-trace loads them only on a firing-post handler, before its row loop.
 * Initialization is complete before sp becomes an arithmetic constant here.
 */
#ifndef MNIST_FIFTH_ROOT_LUT
#define MNIST_FIFTH_ROOT_LUT 0
#endif
#if MNIST_FIFTH_ROOT_LUT != 0 && MNIST_FIFTH_ROOT_LUT != 1
#error "MNIST_FIFTH_ROOT_LUT must be 0 or 1"
#endif
#if MNIST_TRACE_BUNDLE != 0
#error "Shared root constants require the maintained unbundled receiver"
#endif

#define ROOT_LUT_BASE 0x20002000
#define ROOT_SQUARE_SCALE sp
#define ROOT_SQUARE_CENTER s11
#define ROOT_SQUARE_BASE a7
#define ROOT_DIVISOR a4
#define ROOT_MAGIC a5
#define ROOT_FIFTH a6
#define ROOT_NEWTON t0

.macro INIT_FIFTH_ROOT
  li ROOT_SQUARE_SCALE, 0x3f3851ec
#if MNIST_FIFTH_ROOT_LUT
  li ROOT_SQUARE_CENTER, 0x3fc71c72
  li ROOT_SQUARE_BASE, 0x3f471c72
  li ROOT_DIVISOR, ROOT_LUT_BASE
#else
  li ROOT_SQUARE_CENTER, 0x3fc71d15
  li ROOT_SQUARE_BASE, 0x3f471b6d
  li ROOT_DIVISOR, 0x33333333
  li ROOT_MAGIC, 0x4c2bac89
  li ROOT_FIFTH, 0x3e4ccccd
  li ROOT_NEWTON, 0x3f99ca19
#endif
.endm
