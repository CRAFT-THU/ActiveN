/* Shared image/runtime/observer ABI. All addresses except external offsets are
 * local SPM. */
#ifndef BRUNEL_H
#define BRUNEL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
#define BR_ASSERT static_assert
#else
#define BR_ASSERT _Static_assert
#endif
#define BR_MAGIC 0x42524e34u
#define BR_NEURONS 256u
#define BR_BUFFER 0x900u
#define BR_BUFFER_WORDS 2048u
#define BR_MSHR 0x2900u
#define BR_MSHRS 16u
#define BR_READ 0x2d00u
#define BR_SCRATCH 0x2d80u
#define BR_DIRTY_BITMAP 0x2e00u
#define BR_DATA_END 0x4000u /* complete diagnostic snapshot, including guards */
#define BR_INIT_END 0x3f00u
#define BR_RECORDS 0x2f00u
#define BR_PING 0x3700u
#define BR_PING_BYTES 2048u
#define BR_STACK0 0x3f00u
#define BR_STACK1 0x3f80u
#define BR_POISSON 0x2da4u
#define BR_STREAM 0x2dd0u
#define BR_RECORD_CAPACITY 256u
#define BR_MAX_RECORDS 8100u /* current 512-PU graph: at most 18 E * 450 */
#define BR_BASE 0x20000000u
#define BR_MAX_NEURONS 25u
#define BR_LINE 32u
#define BR_CREDITS 12u
/* Fields are deliberately all words for portable observers and packers. */
typedef struct {
  uint32_t magic, pu, num_pu, rule, nn, step, completed, phase;
  uint32_t warmup, steps, mc, active_mcs, work_neuron, work_line, work_mc, sent;
  uint32_t received, reverse_received, issued, consumed, ready[4];
  uint32_t report_count, report_sum, expected_forward, expected_reverse,
      report_mcs, join_count;
  uint32_t buffer_updates, mshr_loads, mshr_commits, overflow_rows, write_words,
      pool_full, forward_weights, reverse_events;
  uint32_t buffer_bytes, mshr_count, mshr_head, mshr_tail, mshr_scheduled,
      rng0, rng1, mshr_peak;
  uint32_t p11, p21, p31, p32, p33, epsc, delivery, external_mean;
  uint32_t external_limit, external_weight, dep, pot, wmax, decay_pre,
      decay_post;
  uint32_t directory_line, weight_loads, allocation_waits;
} BrHeader;
typedef struct {
  uint32_t v, ie, de, ii, di, refrac, x, y, rng, fire, pre_fire, gid;
  uint32_t weights, count, targets, static_dir, reverse_dir, input_e, input_i, buffer;
} BrNeuron;
typedef struct {
  uint32_t line[8];
  uint32_t address, depression, potentiation, state;
  uint32_t sequence, reserved[3];
} BrMshr;
/* Per-timestep incoming-spike recording descriptor. */
typedef struct {
  uint32_t count, peak, cursor, spill_base;
  uint32_t spilled, manual_reverse, scatter_reverse, capacity, spill_cached;
} BrReplay;
typedef struct {
  uint32_t state, copies, overlap_checks, overlap_ready, wait_polls;
} BrStream;
BR_ASSERT(BR_RECORDS + BR_RECORD_CAPACITY * 8 == BR_PING, "record/staging boundary");
BR_ASSERT(BR_PING + BR_PING_BYTES == BR_STACK0, "staging/stack boundary");
BR_ASSERT(BR_STREAM + sizeof(BrStream) <= BR_DIRTY_BITMAP, "stream metadata");
BR_ASSERT(sizeof(BrHeader) == 256, "header ABI");
BR_ASSERT(sizeof(BrNeuron) == 80, "neuron ABI");
BR_ASSERT(BR_NEURONS + BR_MAX_NEURONS * sizeof(BrNeuron) <= BR_BUFFER,
          "neuron capacity");
BR_ASSERT(BR_BUFFER + BR_BUFFER_WORDS * 4 <= BR_MSHR, "buffer capacity");
BR_ASSERT(sizeof(BrMshr) == 64, "MSHR ABI");
BR_ASSERT(BR_MSHR + BR_MSHRS * sizeof(BrMshr) == BR_READ, "MSHR capacity");
BR_ASSERT(offsetof(BrHeader, buffer_bytes) == 152, "buffer control ABI");
BR_ASSERT(offsetof(BrHeader, allocation_waits) == 252, "MSHR wait counter ABI");
BR_ASSERT(offsetof(BrMshr, state) == 44, "MSHR state ABI");
BR_ASSERT(offsetof(BrNeuron, buffer) == 76, "row buffer pointer ABI");
BR_ASSERT(offsetof(BrNeuron, pre_fire) == 40, "assembly pre_fire offset");
BR_ASSERT(offsetof(BrHeader, reverse_received) == 68,
          "assembly receive offset");
BR_ASSERT(offsetof(BrHeader, reverse_events) == 148, "assembly event offset");
#endif
