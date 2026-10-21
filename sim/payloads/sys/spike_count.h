/* Workload-owned firing statistics. No simulator observation is required. */
#ifndef SPIKE_COUNT_H
#define SPIKE_COUNT_H

#define SPIKE_COUNT_LOCAL       0
#define SPIKE_COUNT_TOTAL_LO    4
#define SPIKE_COUNT_TOTAL_HI    8
#define SPIKE_COUNT_BYTES       12
#define SPIKE_COUNT_MARKER      0x53504b31 /* SPK1, followed by low/high words */
#define SPIKE_COUNT_MNIST       0x3020
#define SPIKE_COUNT_BRUNEL      0x2de8

#ifdef __ASSEMBLER__
/* Explicit temporaries keep cached neuron/rule registers and return links live.
 * ADD requires distinct temporaries, none aliasing value. Value is preserved.
 * Call ADD once per measured step at the root, after the ordinary reduction.
 */
.macro SPIKE_COUNT_INIT base, ptr
  li \ptr, \base
  sw zero, SPIKE_COUNT_LOCAL(\ptr)
  sw zero, SPIKE_COUNT_TOTAL_LO(\ptr)
  sw zero, SPIKE_COUNT_TOTAL_HI(\ptr)
.endm

.macro SPIKE_COUNT_ADD value, base, ptr, low, carry, high
  li \ptr, \base
  lw \low, SPIKE_COUNT_TOTAL_LO(\ptr)
  add \low, \low, \value
  sltu \carry, \low, \value
  lw \high, SPIKE_COUNT_TOTAL_HI(\ptr)
  add \high, \high, \carry
  sw \low, SPIKE_COUNT_TOTAL_LO(\ptr)
  sw \high, SPIKE_COUNT_TOTAL_HI(\ptr)
.endm

/* Last auxiliary-output frame on success, emitted outside the workload timer.
 * The ordinary exit result still determines success. No normal stack is used.
 */
.macro SPIKE_COUNT_EMIT base, ptr, value, port
  li \port, 0x4000000c
  li \value, SPIKE_COUNT_MARKER
  sw \value, 0(\port)
  li \ptr, \base
  lw \value, SPIKE_COUNT_TOTAL_LO(\ptr)
  sw \value, 0(\port)
  lw \value, SPIKE_COUNT_TOTAL_HI(\ptr)
  sw \value, 0(\port)
.endm
#endif
#endif
