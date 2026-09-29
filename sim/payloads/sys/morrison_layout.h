/* Compact full-Morrison ABI v1. Constants are shared by assembly and observers.
 * All offsets are relative to the 16 KiB per-PU scratchpad. */
#ifndef MORRISON_LAYOUT_H
#define MORRISON_LAYOUT_H
#define MR_MAGIC 0x4d4f5231
#define MR_MAX_NEURONS 220
#define MR_NEURON_BYTES 32
#define MR_FIRED_LIST 0x1c80
#define MR_PROXY_CONFIG 0x2000
#define MR_PROXY_JOBS 0x2040
#define MR_PROXY_SEEN 0x2720
#define MR_PROXY_SEEN_WORDS 28
#define MR_DESCRIPTOR 0x27a0
#define MR_PARAMETERS 0x2e00
#define MR_POISSON 0x2e40
#define MR_POISSON_COUNT 20
#define MR_ABI_VERSION 1
#define MR_STATUS_REFRAC 28
#define MR_STATUS_FIRE 29
#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
typedef struct {
  uint32_t v, current, derivative, x, y, input_e, input_i;
  uint8_t refractory, fired;
  uint16_t reserved;
} MrNeuron;
/* HBM row: slot = local neuron slot * group size + home rank. One table per
 * proxy PU. Only the home PU uses reverse_dir. Words 0/1 stay zero. */
typedef struct {
  uint32_t reserved[2], gid, weights, count, static_dir, reverse_dir, reserved2;
} MrRow;
typedef struct {
  uint32_t ne, ni, num_mc, rows, abi_version, group_size, reserved[2];
} MrParameters;
#ifdef __cplusplus
static_assert(sizeof(MrNeuron) == MR_NEURON_BYTES);
static_assert(offsetof(MrNeuron, refractory) == MR_STATUS_REFRAC);
static_assert(offsetof(MrNeuron, fired) == MR_STATUS_FIRE);
static_assert(sizeof(MrRow) == 32 && sizeof(MrParameters) == 32);
static_assert(256 + MR_MAX_NEURONS * sizeof(MrNeuron) == MR_FIRED_LIST);
static_assert(MR_FIRED_LIST + MR_MAX_NEURONS * 4 <= MR_PROXY_CONFIG);
static_assert(MR_PROXY_JOBS + MR_MAX_NEURONS * 4 * 2 == MR_PROXY_SEEN);
static_assert(MR_PROXY_SEEN + MR_PROXY_SEEN_WORDS * 4 <= MR_DESCRIPTOR);
static_assert(MR_DESCRIPTOR + sizeof(MrRow) <= 0x2900);
static_assert(MR_POISSON + MR_POISSON_COUNT * 4 <= 0x3700);
#endif
#endif
#endif
