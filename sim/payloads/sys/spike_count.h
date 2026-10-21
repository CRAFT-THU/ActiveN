/* Workload-owned firing statistics. No simulator observation is required. */
#ifndef SPIKE_COUNT_H
#define SPIKE_COUNT_H

#define SPIKE_COUNT_LOCAL       0
#define SPIKE_COUNT_STATE       4
#define SPIKE_COUNT_ACK         8
#define SPIKE_COUNT_BYTES       12
#define SPIKE_COUNT_TAG         0x40000000 /* one tagged local count per PU */
#define SPIKE_COUNT_START       0x53504b53 /* start measured counting */
#define SPIKE_COUNT_REPORT      0x53504b52 /* terminate, emit and acknowledge */
#define SPIKE_COUNT_ACK_PHASE   0x53504b41 /* report acknowledgement payload */
#define SPIKE_COUNT_STATE_ACTIVE 0x00000001
#define SPIKE_COUNT_STATE_START_PENDING 0x00000002
#define SPIKE_COUNT_MNIST       0x3020
#define SPIKE_COUNT_BRUNEL      0x2de8

#ifdef __ASSEMBLER__
.macro SPIKE_COUNT_INIT base, ptr
  li \ptr, \base
  sw zero, SPIKE_COUNT_LOCAL(\ptr)
  sw zero, SPIKE_COUNT_STATE(\ptr)
  sw zero, SPIKE_COUNT_ACK(\ptr)
.endm

.macro SPIKE_COUNT_RESET base, ptr, value
  li \ptr, \base
  sw zero, SPIKE_COUNT_LOCAL(\ptr)
  li \value, SPIKE_COUNT_STATE_ACTIVE
  sw \value, SPIKE_COUNT_STATE(\ptr)
.endm

.macro SPIKE_COUNT_INC base, ptr, value
  li \ptr, \base
  li \value, 1
  amoadd.w zero, \value, (\ptr)
.endm

/* One tagged auxiliary word per PU, emitted after the measured timer stops. */
.macro SPIKE_COUNT_EMIT base, ptr, value, port
  li \port, 0x4000000c
  li \ptr, \base
  lw \value, SPIKE_COUNT_LOCAL(\ptr)
  li \ptr, SPIKE_COUNT_TAG
  or \value, \value, \ptr
  sw \value, 0(\port)
.endm
#endif
#endif
