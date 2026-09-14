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
#define BR_MAGIC 0x42524e32u
#define BR_NEURONS 256u
#define BR_TABLE 2304u
#define BR_SLOTS 800u
#define BR_READ 8704u
#define BR_WB 8832u
#define BR_SCRATCH 9216u
#define BR_DATA_END 10240u
#define BR_RECORDS 10240u
#define BR_RECORD_CAPACITY 256u
#define BR_MAX_RECORDS 8100u /* current 512-PU graph: at most 18 E * 450 */
#define BR_BASE 0x20000000u
#define BR_MAX_NEURONS 25u
#define BR_LINE 32u
#define BR_CREDITS 12u
#define BR_WBS 16u
#define BR_CLEAN 0u
#define BR_DIRTY 1u
#define BR_PENDING_LTD 2u
#define BR_PENDING_LTP 3u
#define BR_STATE 3u
#define BR_EMPTY 0u
/* Fields are deliberately all words for portable observers and packers. */
typedef struct {
  uint32_t magic, pu, num_pu, rule, nn, step, completed, phase;
  uint32_t warmup, steps, mc, active_mcs, work_neuron, work_line, work_mc, sent;
  uint32_t received, reverse_received, issued, consumed, ready[4];
  uint32_t report_count, report_sum, expected_forward, expected_reverse,
      report_mcs, join_count;
  uint32_t hits, misses, pending, evictions, write_words, forced, probes,
      reverse_events;
  uint32_t buckets[8];
  uint32_t p11, p21, p31, p32, p33, epsc, delivery, external_mean;
  uint32_t external_limit, external_weight, dep, pot, wmax, decay_pre,
      decay_post;
  uint32_t reserved[3];
} BrHeader;
typedef struct {
  uint32_t v, ie, de, ii, di, refrac, x, y, rng, fire, pre_fire, gid;
  uint32_t weights, count, targets, static_dir, reverse_dir, pad[3];
} BrNeuron;
typedef struct {
  uint32_t tag, data;
} BrEntry;
typedef struct {
  uint32_t tag, left, next, pad;
} BrWriteback;
/* Kept outside the frozen simulator's fixed header ABI. */
typedef struct {
  uint32_t count, peak, cursor, spill_base;
  uint32_t spilled, manual_reverse, scatter_reverse, capacity, spill_cached;
} BrReplay;
BR_ASSERT(BR_RECORDS + BR_RECORD_CAPACITY * 8 == 0x3000, "record/stack boundary");
BR_ASSERT(sizeof(BrHeader) == 256, "header ABI");
BR_ASSERT(sizeof(BrNeuron) == 80, "neuron ABI");
BR_ASSERT(sizeof(BrEntry) == 8, "entry ABI");
BR_ASSERT(sizeof(BrWriteback) == 16, "writeback ABI");
BR_ASSERT(BR_NEURONS + BR_MAX_NEURONS * sizeof(BrNeuron) <= BR_TABLE,
          "neuron capacity");
BR_ASSERT(BR_TABLE + BR_SLOTS * sizeof(BrEntry) <= BR_READ, "table capacity");
BR_ASSERT(BR_WB + BR_WBS * sizeof(BrWriteback) <= BR_SCRATCH, "writeback capacity");
BR_ASSERT(offsetof(BrNeuron, pre_fire) == 40, "assembly pre_fire offset");
BR_ASSERT(offsetof(BrHeader, reverse_received) == 68,
          "assembly receive offset");
BR_ASSERT(offsetof(BrHeader, reverse_events) == 148, "assembly event offset");
#endif
