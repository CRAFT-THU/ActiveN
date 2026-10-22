/* Opt-in 80-byte-neuron Brunel ABI; consecutive home G4 only. */
#ifndef BRUNEL_READY_LAYOUT_H
#define BRUNEL_READY_LAYOUT_H
#define BR_READY_MSHR 0x1800
#define BR_READY_SLOTS 64
#define BR_READY_RING 0x2800
#define BR_READY_PENDING 0x2900
#define BR_READY_PENDING_BYTES 32
#define BR_READY_READ_CREDITS 16
#define BR_READY_ALLOC 152
#define BR_READY_COUNT 156
#define BR_READY_HEAD 160
#define BR_READY_TAIL 164
#define BR_READY_ACTIVE 168
#define BR_READY_CREDIT 180
#define BR_READY_ADDRESS 0
#define BR_READY_LTD 4
#define BR_READY_LTP 8
#define BR_READY_MC 12
#define BR_READY_VALID 16
#define BR_READY_MAGIC_BITS 0xc00 /* bit10 ready, bit11 consecutive placement */
/* These checks also run while preprocessing the assembly-only payload. */
#if BR_PROXY_ROWS + BR_PROXY_CAPACITY * 32 != BR_READY_MSHR
#error Ready descriptor pool overlaps G4 proxy rows
#endif
#if BR_READY_MSHR + BR_READY_SLOTS * 64 != BR_READY_RING
#error Ready descriptor/ring boundary mismatch
#endif
#if BR_READY_RING + BR_READY_SLOTS * 4 != BR_READY_PENDING
#error Ready ring/pending boundary mismatch
#endif
#if BR_READY_PENDING + BR_READY_PENDING_BYTES > 0x2d00
#error Pending issuer overlaps directory read buffers
#endif
#endif
