/* Shared image/runtime/observer ABI. All addresses except external offsets are
 * local SPM. */
#ifndef BRUNEL_H
#define BRUNEL_H
#include <stddef.h>
#include <stdint.h>
#include "brunel_proxy_config.h"
#include "spike_count.h"
#ifdef __cplusplus
#define BR_ASSERT static_assert
#else
#define BR_ASSERT _Static_assert
#endif
#define BR_MAGIC 0x42524e34u
#define BR_NEURONS 256u
#define BR_BUFFER 0x900u
#define BR_BUFFER_WORDS 2048u
#define BR_FIRED_LIST BR_BUFFER /* 25 neuron pointers; count in header word 53 */
#define BR_FIRED_COUNT 212u
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
#define BR_TREE_REPORTS 216u /* boot-computed self + child report count */
#define BR_RECORD_CAPACITY 256u
#define BR_MAX_RECORDS 8100u /* current 512-PU graph: at most 18 E * 450 */
#define BR_BASE 0x20000000u
#define BR_MAX_NEURONS 25u
#define BR_LINE 32u
#define BR_CREDITS 12u
/* Fields are deliberately all words for portable observers and packers. */
typedef struct {
  uint32_t magic, pu, num_pu, rule, nn, step, completed, phase;
  uint32_t warmup, steps, mc;
  union { uint32_t active_mcs, excitatory_end; };
  uint32_t work_neuron, work_line, work_mc, sent;
  uint32_t received, reverse_received, issued, consumed, ready[4];
  uint32_t report_count, report_sum, expected_forward, expected_reverse;
  /* The image's unused report word becomes the fixed neuron-end pointer. */
  union { uint32_t report_mcs, neuron_end; };
  uint32_t join_count;
  uint32_t buffer_updates, mshr_loads, mshr_commits, overflow_rows, write_words,
      pool_full, forward_weights, reverse_events;
  uint32_t buffer_bytes, mshr_count, mshr_head, mshr_tail, mshr_scheduled,
      rng0, rng1, mshr_peak;
  uint32_t p11, p21, p31, p32, p33, epsc;
  /* Boot replaces these image constants with their runtime meanings. */
  union { uint32_t delivery, external_current; };
  union { uint32_t external_mean, fired_count; };
  union { uint32_t external_limit, tree_reports; };
  uint32_t external_weight, dep, pot, wmax, decay_pre,
      decay_post;
  uint32_t mc_destination, weight_loads, allocation_waits;
} BrHeader;
typedef struct {
  /* fire is authoritative from integration through reverse learning.
   * Offset 40 retains its image value and is reserved for ABI compatibility. */
  /* Initial images retain split currents. Boot sums IE+II and DE+DI into the
   * first pair; II/DI become zero reserved words in runtime snapshots. */
  uint32_t v;
  union { uint32_t ie, i_total; };
  union { uint32_t de, d_total; };
  /* Images contain raw x/y; E runtime x=pot*pre_trace, y=dep*post_trace.
   * I runtime traces are reserved zeros; only E->E synapses are plastic. */
  uint32_t ii, di, refrac, x, y, rng, fire, reserved_fire, gid;
  /* buffer is unused normally; proxy images store the local descriptor offset. */
  uint32_t weights, count, targets, static_dir, reverse_dir, input_e, input_i, buffer;
} BrNeuron;
typedef struct {
  uint32_t line[8];
  uint32_t address, depression, potentiation, state;
  uint32_t sequence, mc_destination, reserved[2];
} BrMshr;
/* Per-timestep incoming-spike recording descriptor. */
typedef struct {
  uint32_t count, peak, cursor, spill_base;
  uint32_t spilled, manual_reverse, scatter_reverse, capacity, spill_cached;
} BrReplay;
typedef struct {
  uint32_t state, copies, overlap_checks, overlap_ready, wait_polls;
} BrStream;
/* Optional proxy placement. Home-update variants leave x unused and zero. */
typedef struct {
  uint32_t x, fire, gid, weights, count, static_dir, reserved[2];
} BrProxyRow;
typedef struct {
  uint32_t peers[8], count, excitatory_end, jobs, markers, expected, trace_work;
  uint32_t magic, notify_cursor;
} BrProxyConfig;
BR_ASSERT(sizeof(BrProxyRow) == 32, "proxy descriptor ABI");
BR_ASSERT(sizeof(BrProxyConfig) == 64, "proxy control ABI");
BR_ASSERT(BR_FIRED_LIST + BR_MAX_NEURONS * 4 <= BR_PROXY_CONFIG, "home fired list");
BR_ASSERT(BR_PROXY_JOBS + BR_PROXY_CAPACITY * 4 <= BR_PROXY_ROWS, "proxy job list");
BR_ASSERT(BR_PROXY_ROWS + BR_PROXY_CAPACITY * sizeof(BrProxyRow) <= BR_MSHR, "proxy descriptors");
BR_ASSERT(BR_RECORDS + BR_RECORD_CAPACITY * 8 == BR_PING, "record/staging boundary");
BR_ASSERT(BR_PING + BR_PING_BYTES == BR_STACK0, "staging/stack boundary");
BR_ASSERT(BR_STREAM + sizeof(BrStream) <= SPIKE_COUNT_BRUNEL, "stream metadata");
BR_ASSERT(SPIKE_COUNT_BRUNEL + SPIKE_COUNT_BYTES <= BR_DIRTY_BITMAP,
          "spike totals must precede the dirty bitmap/Morrison parameters");
BR_ASSERT(sizeof(BrHeader) == 256, "header ABI");
BR_ASSERT(sizeof(BrNeuron) == 80, "neuron ABI");
BR_ASSERT(BR_NEURONS + BR_MAX_NEURONS * sizeof(BrNeuron) <= BR_BUFFER,
          "neuron capacity");
BR_ASSERT(BR_BUFFER + BR_BUFFER_WORDS * 4 <= BR_MSHR, "buffer capacity");
BR_ASSERT(BR_FIRED_LIST + BR_MAX_NEURONS * 4 <= BR_MSHR, "fired list capacity");
BR_ASSERT(sizeof(BrMshr) == 64, "MSHR ABI");
#if BR_READY
BR_ASSERT(BR_PROXY_ROWS + BR_PROXY_CAPACITY * sizeof(BrProxyRow) == BR_READY_MSHR,
          "ready descriptors follow G4 proxy rows");
BR_ASSERT(BR_READY_MSHR + BR_READY_SLOTS * sizeof(BrMshr) == BR_READY_RING,
          "ready descriptor pool");
BR_ASSERT(BR_READY_RING + BR_READY_SLOTS * 4 == BR_READY_PENDING, "ready ring");
BR_ASSERT(BR_READY_PENDING + BR_READY_PENDING_BYTES <= BR_READ, "pending issuer");
BR_ASSERT(offsetof(BrHeader, buffer_bytes) == BR_READY_ALLOC, "ready allocation cursor");
BR_ASSERT(offsetof(BrHeader, mshr_count) == BR_READY_COUNT, "ready descriptor count");
BR_ASSERT(offsetof(BrHeader, mshr_head) == BR_READY_HEAD, "ready head");
BR_ASSERT(offsetof(BrHeader, mshr_tail) == BR_READY_TAIL, "ready tail");
BR_ASSERT(offsetof(BrHeader, mshr_scheduled) == BR_READY_ACTIVE, "ready active updaters");
BR_ASSERT(offsetof(BrHeader, mshr_peak) == BR_READY_CREDIT, "ready read credits");
#endif
BR_ASSERT(BR_MSHR + BR_MSHRS * sizeof(BrMshr) == BR_READ, "MSHR capacity");
BR_ASSERT(offsetof(BrHeader, buffer_bytes) == 152, "buffer control ABI");
BR_ASSERT(offsetof(BrHeader, neuron_end) == 112, "neuron-end pointer ABI");
BR_ASSERT(offsetof(BrHeader, excitatory_end) == 44, "excitatory-end pointer ABI");
BR_ASSERT(offsetof(BrHeader, fired_count) == BR_FIRED_COUNT, "fired count ABI");
BR_ASSERT(offsetof(BrHeader, tree_reports) == BR_TREE_REPORTS, "report count ABI");
BR_ASSERT(offsetof(BrHeader, allocation_waits) == 252, "MSHR wait counter ABI");
BR_ASSERT(offsetof(BrMshr, state) == 44, "MSHR state ABI");
BR_ASSERT(offsetof(BrNeuron, buffer) == 76, "row buffer pointer ABI");
BR_ASSERT(offsetof(BrNeuron, reserved_fire) == 40, "reserved neuron offset");
BR_ASSERT(offsetof(BrHeader, mc_destination) == 244, "precomputed MC destination");
BR_ASSERT(offsetof(BrHeader, reverse_received) == 68,
          "assembly receive offset");
BR_ASSERT(offsetof(BrHeader, reverse_events) == 148, "assembly event offset");
#endif
