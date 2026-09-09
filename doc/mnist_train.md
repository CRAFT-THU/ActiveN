# MNIST dense trace-STDP training

The MNIST training payload targets the generated 512-PU, 16-MC system. Each PU
has 16 KiB of scratchpad, two SMT threads, and an 8 KiB two-way instruction
cache. The model contains 784 Poisson inputs, 400 excitatory neurons, and 400
inhibitory neurons and advances in 0.5 ms steps.

This payload implements dense input-to-excitatory connectivity only. Sparse
connectivity deliberately has no runtime path here; it will use a separate
reverse/transposed representation in DRAM.

## Placement

Input PUs and neuron PUs are disjoint. An input PU owns only Poisson inputs and
their trainable rows. A neuron PU owns matched excitatory/inhibitory pairs, so a
pair always has the same local index and E-to-I delivery is local.

The generator exposes:

- `--input-pus N` and `--neuron-pus M`;
- `--placement-policy near|edge|balanced` for choosing input PUs outside the
  neuron zones;
- `--neuron-zone-policy central|network-balanced|network-balanced-320|
  network-balanced-352` for choosing neuron MC zones;
- `--input-assignment contiguous|rate-balanced` for assigning the 784 global
  input rows to the selected input PUs;
- `--horizontal-partition none|hot-partial|full|full-4|full-8|full-16` for
  retaining whole rows, splitting the 368 highest-score rows, or splitting
  every row into two, four, eight, or sixteen compact column groups.

`M` must be a multiple of 32, and every neuron PU owns at most two matched
pairs. Every selected group of 32 consecutive PU IDs is therefore a complete
MC broadcast range. This preserves the DRAM-scatter comparison without sending
recurrent rows to MCs that own no excitatory target. PU IDs follow the system
Hilbert mapping; they are not row-major mesh indices. The generator also checks
the exact per-PU data end against the first stack, so the usable upper bound on
`M` depends on `N` rather than an obsolete fixed limit of 256.

The selected benchmark layout uses `N=288`, `M=224`, `placement-policy=near`,
the `network-balanced` neuron-zone policy, and `full-8` horizontal partitioning.
The input PUs occupy nine complete MC zones and the neuron PUs occupy the other
seven. The eight groups contain 36 input PUs and 28 neuron PUs each. Every
logical input has one physical record in each group and each copy owns 50
excitatory columns. A physical input PU consequently owns 21 or 22 records;
each neuron PU owns one or two matched E/I pairs.

Full horizontal modes deliberately use rate-independent index-cyclic input
assignment, even when `--input-assignment=rate-balanced` is supplied. Replica
groups are rotated within each MC zone with prime stride 3 so nearby groups do
not place the same input on nearby PUs. The manifest records this effective
assignment as `index-cyclic`. Rate-balanced assignment remains available for
unpartitioned and partial layouts.

The `network-balanced` zone policy retains whole MC ranges but spatially
interleaves the seven neuron zones to reduce deterministic-XY trace traffic.
A tested 256-input/256-neuron split is also valid, but snapshot 065 was 12.2%
slower than 288/224 because each input spike needed 256 feedforward packets and
the smaller input groups produced same-PU fired-row collisions.

Neuron placement puts the PUs with two pairs first in the logical send stream
and spreads those PUs through the selected MC zones. This makes each compact
weight row a sequence of one or two values per target descriptor while avoiding
a physical cluster of the heavier neuron owners.

## Scratchpad layout

Only input-to-E weights are trainable. An unpartitioned row is a bare array of
exactly 400 FP32 values:

```text
slot = compact position of (neuron_pu_rank, local_excitatory_index)
row[slot] = FP32 weight for input -> that excitatory neuron
```

The unpartitioned layout has no padding, stored column index, or search. The
selected `full-8` layout stores eight physical 50-slot row segments, so the
copies together occupy exactly the same weight bytes as one unpartitioned row.
An input PU stores one 4-byte descriptor for each of its 28 destination neuron
PUs. Its low 16 bits are the destination PU and its upper 16 bits identify the
receiver's first and optional second local neuron. Descriptors consume the next
one or two values from the compact row. Each excitatory record stores its
reverse-path byte offset. A neuron PU stores a 16-bit universal destination
table for the 36 input PUs in its group and a 16-bit observed-source vector
with capacity for all 288 input PUs.

For the selected `N=288`, `M=224`, `full-8` layout, a fullest input PU uses:

```text
header                         368 bytes
22 input records               616 bytes
22 * 50 FP32 weights         4,400 bytes
28 target descriptors          112 bytes
22 fired-vector records
  * 8 bytes                    176 bytes
total                         5,672 bytes
```

The first SMT stack begins at byte `0x3000` (12,288), leaving 6,616 bytes
between the fullest selected layout and stack 0. Thread 0's 2 KiB stack occupies
`0x3000-0x37ff`. Thread 1 has a reserved downward-growing range at
`0x3800-0x3fff`, but no thread-1 handler or initializer uses the stack. The
generator checks every PU and rejects a layout that reaches byte `0x3000`.

A two-neuron PU uses at most 1,376 bytes. This includes 72 bytes of universal
input destinations and a 576-byte observed-source vector. The current direct
source-mark protocol requires no observed-source bitmask.

Aggregate logical and physical weight storage is:

```text
784 * 400 * 4-byte logical weight       = 1,254,400 bytes
6,272 records * 50 * 4-byte physical    = 1,254,400 bytes
512 * 16 KiB total scratchpad           = 8,388,608 bytes
```

The original 8-byte weight-plus-column estimate is 2,508,800 bytes for the
actual 784 inputs, still below aggregate capacity. The dense runtime needs only
FP32 weights because the compact slot is implicit, cutting this to 1,254,400
bytes. Aggregate capacity alone is not sufficient evidence; the generator's
per-PU check against `0x3000` is the binding safety check. The selected manifest
reports `max_spm_bytes=5672` and `min_spm_free_bytes=10712`, where the latter is
measured to the 16 KiB SPM end and includes the reserved stack region.

DRAM contains the executable and one-time SPM initialization image. At runtime
it supplies frame scatter records. In `scatter` recurrent mode it also stores
fixed I-to-E CSR rows. No trainable weight is fetched from or written to DRAM.

### Runtime header ABI

The current 368-byte header uses magic `MT11` (`0x4d543131`) and the generated
manifest schema is `activen-mnist-training-v12`. The phase-tree fields most
relevant to the runtime are:

| Offset | Field | Meaning |
| ---: | --- | --- |
| 0 | magic | Runtime/image compatibility guard |
| 8 | PU ID | One-based active-message destination ID |
| 252 | reduction parent | One-based parent destination, or zero at PU1 |
| 300 | reduction expected | This PU's complete subtree size |
| 348 | phase-local value | Cross-thread/next-step staging word |
| 352 | reduction high word | Upper packed firing-group accumulator |
| 356 | reduction extra word | Full-16-only packed accumulator |
| 360 | fired-vector begin | Validated absolute start pointer for reverse updates |
| 364 | fired-vector end | Absolute one-past-end pointer for the current fired list |

For PU1 the expected subtree size is 512; for other PUs it is the least
significant set bit of `PU_ID - 1`. The phase-start handler loads it into the
otherwise-unused thread-0 `ra` register. All hot helper calls name a different
link register explicitly, so the normal reducer can reuse the value without an
SPM load. Startup still recomputes and validates the header value, and the
terminal thread-1 checksum reducer loads its own copy from SPM.

For two- and three-trace rules, the phase-start handler validates the local
fired count and vector address, then publishes both absolute vector bounds
before enabling reverse handlers. Both SMT threads read these immutable bounds;
they are not shared cursors. The next input preparation starts only after all
local reverse updates complete, so it cannot overwrite a vector still being
scanned. The unbundled general depression path is selected only for a nonempty
list; the bundled path retains an empty-range check for its optional slot.

## Step protocol

The workload uses phase-scoped tree synchronization and no per-spike or
per-trace acknowledgements:

1. Scatter the frame thresholds and wait for the MCs to become idle.
2. Every input PU splits its input records into fixed contiguous halves. SMT
   thread 0 owns the first `ceil(n / 2)` records and thread 1 owns the remaining
   `floor(n / 2)`. They advance disjoint RNG states and write fired weight-row
   pointers into disjoint fixed vector segments, so neither work allocation nor
   append needs an AMO. Thread 1 returns its fired and primary counts in one
   packed handler argument. Thread 0 validates both halves, compacts at most
   eleven second-half records in the selected eight-way layout while preserving
   serial record order, and contributes the logical fired count to a 512-PU
   tree sum. With replicated input state, only the primary copy contributes to
   this sum; every physical copy advances the same local RNG once. PUs with
   zero or one input use the serial path.
3. The summed count starts feedforward delivery. Each firing physical input
   copy streams 28 descriptors and corresponding one- or two-weight groups from
   its local 50-value row segment. Across eight copies, one logical input spike
   still produces 224 packets and reaches all 400 excitatory neurons. The packet
   carries one or two old FP32 weights for that destination PU's local neurons.
   A neuron PU completes after receiving exactly `global_fired_inputs` packets
   and accumulates its one or two conductances in registers. The first fired row
   from each physical input PU marks that source ID. Since the receiver is
   serialized on thread 0 and sees exactly one such marker per source and phase,
   it appends directly to the observed-source vector without a bitmask or AMO.
4. Excitatory and inhibitory neurons update. The E-to-I weight is fixed at
   10.4 and is added locally after the inhibitory update, preserving the
   reference model's one-step recurrent delay.
5. I-to-E inhibition is produced by the selected recurrent mode, then the
   weight phase starts.
6. In unbundled mode, a nonfiring excitatory neuron sends its reverse message
   only to the physical input PUs observed during feedforward delivery. A firing
   excitatory neuron sends to all 36 input PUs that own its 50-column group. The
   message contains its precomputed weight byte offset, depression delta, fired
   bit, and the precomputed slow-post potentiation factor needed by a firing
   three-trace neuron. Active eight-way owners count exactly 50 traces; inactive
   owners use the phase reduction's group-specific firing counts. Two 32-bit
   reduction words hold the inhibitory count and all eight excitatory-group
   counts. An optional compile-time mode
   packs two colocated traces into one physical message and retains universal
   reverse delivery for comparison.
7. Input PUs split pre-trace decay across both SMT threads during the neuron
   phase. The existing neuron-update tree reduction is the barrier that makes
   every decayed trace visible before reverse messages begin. After all required
   synapses are updated, firing pre and post traces consume the current spike,
   and each PU contributes one phase-completion tree message.
8. With `MNIST_PIPELINE_INPUT_PREPARE=1`, an input PU prepares the next step only
   after its current reverse deliveries and trace commits are complete. The
   weight-phase tree reduction carries the next global fired-input count and is
   the barrier before the root starts the next feedforward phase. Scatter mode
   also waits for recurrent DRAM traffic to drain. The root then starts the next
   step or invokes a separate XOR-only final reducer. Replicated
   `(global input, RNG, threshold, pre-trace)` fingerprints cancel pairwise in
   that checksum, helping detect replica divergence without constituting a
   collision-free state comparison.

All feedforward deliveries therefore use old weights, and plastic writes cannot
begin until every feedforward spike has been delivered. Decayed old traces are
ready before the reverse path opens. An input PU commits current spikes to its
pre traces only after finishing its own plastic writes; postsynaptic trace
commits follow transmission of the old trace values. Global completion waits
for all PUs before any next-step spike delivery.

For an unpartitioned layout with `A` active input PUs, `P` firing posts, and `N`
input PUs, the default unbundled two-/three-trace path sends
`(400 - P) * A + P * N` physical messages. For a horizontal layout, group `g`
sends `(C_g - P_g) * A_g + P_g * D_g`, where `C_g` is its column count, `A_g`
is the number of observed physical owners, and `D_g` is the universal
destination count for a firing post. Two-way full partitioning has `C_g=200`
and `D_g=144`; four-way full partitioning has `C_g=100` and `D_g=72`; the
selected eight-way layout has `C_g=50` and `D_g=36`; sixteen-way has `C_g=25`
and `D_g=18`; the partial layout has `C_g=200` and `D_g=196`. A zero-spike
frame sends none. One-trace sends
only the firing-post terms because it has no depression.
`MNIST_TRACE_BUNDLE=1` remains an unpartitioned comparison mode. If
both colocated neurons fire under the three-trace rule, their distinct slow-post
values do not fit in one four-word payload, so that neuron PU falls back to two
messages for each destination.

Each neuron PU stores a cyclically rotated input-PU destination table, offset
by 83 times its rank among neuron PUs. Input-PU descriptor tables stay in the
compact weight-stream order, while each sender starts at a rank-dependent cyclic
offset of 167 and wraps once. Both prime strides preserve the complete
destination set while separating nearby senders' starting positions; neither
changes deterministic XY routing.

### Fired-input short list

For a nonfiring postsynaptic neuron, one- and two-/three-trace behavior differs:

- one-trace has no depression, so a nonfiring post requires no row visit;
- two- and three-trace iterate only the flat vector of direct fired-row
  pointers;
- a firing post scans all local rows because potentiation may affect every
  presynaptic input.

For the deterministic first frame, 12 inputs fire. The no-post scan therefore
visits `12 * 400 = 4,800` synapses globally instead of naively checking
`784 * 400 = 313,600`.

Contiguous assignment can put two simultaneously firing inputs on one PU. At
`M=224`, that PU streams 448 physical feedforward packets instead of 224 and
later performs 800 reverse weight updates instead of 400. Rate-balanced
assignment reduces this local concentration while preserving exactly four rows
per PU.

## Recurrent modes

The runtime and image must select the same compile-time mode:

- `scatter`: a fired inhibitory neuron issues its fixed all-to-all-except-pair
  CSR rows from DRAM. Responses are pinned to SMT thread 1 and directly add
  25.5 to target `gi`; serialization removes the need for a floating-point
  atomic loop.
- `tree`: the neuron-update reduction sums inhibitory spikes. Each
  excitatory owner repeatedly adds 25.5 exactly
  `global_count - paired_inhibitory_fired` times.
- `tree-aggregate`: it uses the same count, converts the small integer without
  an FCVT instruction, and performs one `gi += count * 25.5` operation.

`tree` preserves the repeated-add FP32 behavior of scatter. Aggregate mode is
an explicit performance/rounding experiment.

## Plasticity rules

Let `x`, `y1`, and `y2` be the stored pre, fast-post, and slow-post traces. The
input owner forms `xd` during neuron-phase setup, before its reduction barrier,
and the neuron owner forms `y1d`, `y2d`, and the depression delta once before
sending reverse messages:

```text
xd      = x  * pre_decay
y1d     = y1 * post1_decay
y2d     = y2 * post2_decay
d_minus = eta_minus * y1d
p_plus  = eta_plus * y2d
```

The neuron owner also converts its compact slot to a byte offset once. All of
its destination messages then reuse that offset, `d_minus`, and `p_plus`, so
neither the shift nor those postsynaptic multiplies are repeated on every
presynaptic PU. Forming `p_plus` reassociates the original left-associated FP32
expression:
the runtime evaluates `(eta_plus * y2d) * effective_pre`, not
`(eta_plus * effective_pre) * y2d`. The equations are algebraically identical,
but a forced-post checksum can differ in low bits. After weight updates finish,
a firing trace is reset or incremented according to the selected rule.

Three-trace STDP uses reset-on-spike traces. A presynaptic spike applies
`w -= eta_minus * y1d`. A postsynaptic spike applies
`w += (eta_plus * y2d) * (pre_spike ? 1 : xd)`.

Two-trace STDP uses accumulating traces. A presynaptic spike applies
`w -= eta_minus * y1d * w^0.2`. A postsynaptic spike applies
`w += eta_plus * (xd + pre_spike - 0.4) * (wmax - w)^0.2`.

One-trace STDP retains only the accumulating presynaptic trace and the
two-trace potentiation expression; it has no depression term. Every rule clamps
the result to `[0, wmax]`.

These equations match the zero-delay references under `../stdp-experiments`:
same-cycle post events see the effective current pre trace, while depression
samples the old decayed post trace.

## Neuron and Poisson updates

The conductance neuron step uses midpoint-decayed conductances and an FP32
first-order membrane update at `dt = 0.5 ms`. The Poisson threshold for pixel
`p` and intensity `i` is generated from:

```text
rate_hz     = (p / 8) * i
probability = rate_hz * 0.0005
threshold   = floor(probability * 2^32)
```

Each input owns a persistent `xorshift32` state. Every step advances it once
and fires when the unsigned state is below the integer threshold. Sampling
therefore needs no runtime float/integer conversion.

## Concurrency and code generation

The complete timestep runtime is handwritten `rv32ima_zfinx_zicsr` assembly.
The only C code in the binary is the one-time DRAM-to-SPM initializer, compiled
with `-O3 -fomit-frame-pointer -ffp-contract=off`. Both SMT stacks are reserved
in SPM, but no runtime handler calls a stacked procedure or accesses a stack.

Feedforward sending and receipt run on thread 0; the receiver keeps its one or
two conductance accumulators in registers. All neuron PUs use the same
`ff_spike_handler`, which decodes the optional second value from each packet;
there is no startup selection of a one-neuron or two-neuron receiver. This
removes the experimental receiver specialization, not the existing two-value
packet format or the placement limit of two E/I pairs per neuron PU.
Reverse sending also runs on thread
0. Reverse receipt uses both SMT threads for zero-, one-, and multiple-local-fire
handlers. Each thread has private working registers, the fired vector is read-only
during the reverse phase, distinct trace messages address distinct columns, and
the shared completion count uses an AMO. Feedforward source discovery is pinned
to thread 0 and appends one explicitly marked packet per source directly to a
flat vector; it needs neither a bitmask nor an atomic allocator. An earlier
N=160 run appeared to show
that the generic updater was unsafe on two threads. After fixing the independent
phase-transition completion-register bug described below, the same N=160
seven-step stress sequence produces the exact `0x07df2e3a` reference checksum
with both threads enabled.

An active unbundled input PU keeps its countdown address, decrement, and terminal
value in `s8`, `s9`, and `s10`. An inactive PU counts upward against the global
post count carried by each reverse message. This handles a message arriving
before the PU's local weight-phase event: either that event or the terminal
message can observe equality, while an atomic done bit permits only one phase
completion. Scatter responses are serialized on thread 1 and directly update
inhibitory conductance. Phase-tree,
reduction, and other low-frequency control handlers remain on thread 0.

A neuron PU with two local E/I pairs dispatches the second pair to thread 1
while thread 0 updates the first. The pair states and conductance accumulators
are disjoint. Thread 1 returns only its two firing bits to a thread-0 join
handler, which combines both results before recurrent delivery and phase
reduction. Input PUs similarly use fixed, disjoint record halves for Poisson
preparation and pre-trace decay. Their thread-0 join merges only counts and the
short fired-pointer sublists; it does not merge RNG or trace state. Single-pair
neuron PUs stay on thread 0.

One plasticity rule, one recurrent mode, and bundled or unbundled reverse
messages are selected with compile-time definitions. The default
`MNIST_PARALLEL_INPUT_PREPARE=1` enables the static two-thread preparation
split. `MNIST_INPUT_DECAY_MODE=1` moves decay from serialized phase-start
fanout into the parallel neuron phase; mode 0 retains the legacy phase-start
loop and mode 2 fuses decay into input preparation for controlled comparisons.
`MNIST_FUSE_NEURON_UPDATE=1` performs neuron work in the feedforward-completion
phase, and `MNIST_PIPELINE_INPUT_PREPARE=1` prepares the following input state at
the safe tail of the current weight phase. The selected benchmark explicitly
sets `MNIST_HORIZONTAL_GROUP_COUNT=8`; the Makefile retains four as its
backward-compatible default, so image generation and payload compilation must
always select the same group count.
Startup rejects a rule or recurrent-mode mismatch; bundling uses the same SPM
layout, and the generator's `--trace-bundle` flag records the matching traffic
count in the manifest. FP32 constants are materialized as integer immediates.
The nine default unbundled rule/mode binaries contain no FLW, FSW, FCVT,
floating divide, or floating square-root instruction.

The serial compile smoke matrix before removing receiver specialization
produced these linked text sizes:

| Rule/mode | Horizontal groups | Text bytes |
| --- | ---: | ---: |
| One trace, tree | 4 | 6,624 |
| Two trace, aggregate tree | 4 | 7,068 |
| Three trace, scatter | 8 | 6,888 |
| Three trace, tree | 16 | 6,928 |
| Three trace, tree, before receiver simplification | 8 | 6,672 |

All fit the 8,192-byte I-cache. Because text starts 16 bytes into a cache line,
the largest compiled binary spans 111 of the cache's 128 64-byte lines. The
current shared-receiver binary is 6,608 bytes, spans 104 lines, and leaves
1,584 payload bytes unused. The
contiguous text therefore fits the 64-set, two-way organization without
capacity or conflict eviction. After a PU executes a fixed dynamic path, stack
and weight data remain in SPM; later unicast requests are compulsory
instruction fills for stochastic paths that PU has not executed before.

Static instruction inspection, excluding handler scheduling and NoC
backpressure, gives these hot-path costs:

- reverse sender continuation: 6 cycles;
- feedforward sender: 9 retired instructions for a two-weight destination and
  8 for a one-weight destination, including the send and loop branch;
- feedforward receiver: the shared handler performs four more retired
  instructions per packet than the removed specialized handlers, plus two
  extra instructions on the first packet of the phase; historical specialized
  estimates of 22/28 cycles are not measurements of the current handler;
- inactive reverse receiver: one full post update followed by its completion
  AMO; nonfiring posts are not delivered there;
- unbundled three-trace depression with exactly one local fired input: 13
  retired instructions on the common path, including completion handling.

Firing-post scans and two-trace fifth-root evaluation are longer because they
perform the required per-synapse arithmetic. Two-trace now uses the table-free
Newton implementation described in the 2026-09-09 section below; one-trace
retains the polynomial.

## Validation

The independent FP32 host model and system simulator agree on these one-step
weight checksums:

| Case | Rule | Checksum |
| --- | --- | ---: |
| Natural deterministic input | One trace | `0x032674ca` |
| Natural deterministic input | Two trace, cached bit seed | `0x02eb6e72` |
| Natural deterministic input | Three trace | `0x0239bec0` |
| Forced same-step post column 1 | One trace | `0x0561f569` |
| Forced same-step post column 1 | Two trace, cached bit seed | `0x04ac8ff3` |
| Forced same-step post column 1 | Three trace | `0x0480e684` |
| Forced same-PU post columns 2 and 3 | One trace | `0x049c8764` |
| Forced same-PU post columns 2 and 3 | Two trace, cached bit seed | `0x05506231` |
| Forced same-PU post columns 2 and 3 | Three trace | `0x0409fc8f` |
| Forced post columns 0 and 224 | One trace | `0x0400db44` |
| Forced post columns 0 and 224 | Two trace, cached bit seed | `0x05cbe32a` |
| Forced post columns 0 and 224 | Three trace | `0x07851cb3` |

The forced-post cases cover potentiation, depression, and same-step
pre-before-post ordering. The columns-2-and-3 three-trace case specifically
exercises the two-pass unbundled sender when two colocated firing neurons have
distinct slow-post values. It reproduces `0x0409fc8f` with the generic reverse
handler enabled on both SMT threads. Repeated warm runs use a final XOR
checksum to catch SMT races.

The single-post three-trace fixture also passes in scatter mode at
`0x0480e6ff`. Scatter's MC-idle registration requests reuse the argument
registers, so the root explicitly reloads the packed neuron-reduction value
before starting the weight phase. This preserves the global firing-post count
that inactive input PUs use as their reverse-message completion target.

A separate two-step test forces inhibitory neuron 1 to fire, holds excitatory
neurons 1 and 2 just below threshold for the first step, and then checks that
the paired excitatory neuron is spared while the nonpaired neuron is inhibited.
Scatter, repeated tree, and aggregate tree all produce the independently
predicted `0x059831b7` weight checksum.

A four-warm/twelve-timed run reaches `0x0542959e` after 16 total steps. A full
SRAM dump has zero mismatches across all 313,600 weights against the independent
host calculation for the observed no-post sequence.

Checkpoint 65 provides a realistic 16-step presentation window with five
excitatory spikes in the timed portion. Unpartitioned and full horizontal
layouts produce identical input/excitatory spike sequences and final checksums
for all three plasticity rules:

| Rule | Unpartitioned | Full horizontal |
| --- | ---: | ---: |
| One trace | `0x0f136d57` | `0x0f136d57` |
| Two trace | `0x0f1c82ee` | `0x0f1c82ee` |
| Three trace | `0x0288fdd9` | `0x0288fdd9` |

The same three-trace checkpoint produces `0x0288fdd9` in both unpartitioned
and full-horizontal scatter mode. Thus the grouped layout preserves the
fixed-weight DRAM recurrent path as well as repeated-tree recurrence. All nine
unbundled rule/recurrent-mode combinations compile without stack-relative
memory operations, floating-point loads/stores, conversion, divide, or square
root instructions.

## Performance

This section records historical synthetic-input experiments, not the current
runtime-checkpoint corpus average. Measurements use the cycle-aligned soft
system simulator, flat memory, 512 PUs,
16 MCs, the deterministic synthetic frame at intensity 2.0, and eight host
worker threads per simulator process. Host parallelism changes wall time, not
modeled cycles.

The historical progression for the unbundled three-trace runtime is:

| Runtime | Timed frame cycles | Mean cycles/frame |
| --- | --- | ---: |
| Previous feedforward loop | 25,696; 25,522; 25,709; 26,119 | 25,761.50 |
| Fixed-slot blocking feedforward loop | 19,455; 19,564; 19,250; 19,222 | 19,372.75 |
| Shared delta and byte offset, padded row | 14,212; 14,363; 13,960; 14,389 | 14,231.00 |
| Final compact row and phase-register fix | 14,408; 13,928; 14,052; 14,516 | 14,226.00 |
| Activity-aware reverse and direct fired-row vector | 14,979; 14,784; 15,008; 15,096 | 14,966.75 |

The first two runs end at checksum `0x03e483d8`; the later implementations were
also checked against that same eight-step reference. The fixed-slot loop removes
24.80% from the original sender. Sharing the postsynaptic delta and byte offset
then removes another 26.57% from the fixed-loop mean, for 1.811 times the
original throughput. The compact row is within 0.04% of the padded-row result
while removing every padding slot.

The activity-aware 16-step run measured eleven ordinary timed frames at 14,979,
14,784, 15,008, 15,096, 15,150, 15,110, 14,962, 14,616, 14,809, 14,887, and
14,695 cycles, averaging 14,917.82. The remaining timed frame contains two fired
inputs on one PU and falls from 45,432 to 35,354 cycles with the direct-pointer
vector, a 22.18% reduction. Total timed execution falls from 202,269 to 199,450
cycles, or 16,620.83 cycles per frame. The run reproduces the final
`0x0542959e` checksum exactly.

The ordinary 14,408-cycle profile spends 6,506 cycles in the continuous reverse
traffic epoch, or 45.16% of the frame. That epoch accounts for 1,114,217 of the
frame's 1,165,430 accepted link traversals, or 95.61%, and 578,279 blocked
injection presentations. No router input queue becomes full, the five most
blocked links contribute only 1.68% of blocking, and 450 links are needed to
cover 90%. Backpressure is therefore distributed and coupled to endpoint trace
consumption rather than caused by one link or DRAM.

In the matched activity-aware frame, accepted link traversals fall from
1,165,648 to 117,842 and retired instructions from 1,243,362 to 653,810. The
frame nevertheless rises from 14,212 to 14,979 cycles: active input PUs still
receive all 400 columns and remain the reverse critical path, while atomically
discovering source PUs adds work to the earlier feedforward phase. This change
is therefore primarily a communication, dynamic-work, and zero-activity win,
not an ordinary-frame latency win.

The old 45,432-cycle multiple-local-fire frame had PU 34 reconstruct each row
address and reload input metadata for every trace. The direct-pointer vector
removes those operations and reduces the same frame to 35,354 cycles while
preserving the exact checksum.

In the matched fifth-frame feedforward profiles, all 13 active input senders
retire exactly 4,941 fewer instructions, which is 22.06 fewer per destination
and agrees with replacing the approximately 30-instruction path by the
8-instruction loop. Total retired instructions fall from 1,734,151 to 1,669,898.

The shared reverse arithmetic is done once per excitatory neuron, before its
destination loop. With 12 fired inputs and no postsynaptic spikes, moving both
the `slot << 2` operation and `eta_minus * y1d` out of receiver updates replaces
4,800 shifts and 4,800 multiplies with 400 of each at the senders. Count-only
receivers still avoid all plasticity arithmetic.

The three-trace firing-post loop hoists the precomputed potentiation coefficient
and maximum weight once per message and removes the now-impossible non-post
branch from each row. This deliberately reassociates the two FP32 multiplies;
the host oracle models that order explicitly and the realistic snapshot-65
smoke test retains its `0x0288fdd9` checksum.

The optimized loop deliberately uses ordinary blocking remote sends and stays
in the same handler until the complete local fired-input list is delivered. A
remote yielding send would idle the source without scheduling its continuation.
This phase has an acyclic dependency: disjoint input PUs only produce
feedforward packets, neuron PUs only consume them, and the receiver count enters
the phase barrier before reverse traffic can begin.

The fifth deterministic timestep of the previous sender was used for the
matched placement sweep:

| Neuron zones | Trace stride | Cycles |
| --- | ---: | ---: |
| Central | 97 | 26,193 |
| Network-balanced | 53 | 26,022 |
| Network-balanced | 83 | 25,696 |
| Network-balanced | 97 | 26,321 |
| Network-balanced | 113 | 26,213 |
| Network-balanced | 139 | 25,872 |

Every candidate ends at checksum `0x01d1afd9`. Network-balanced stride 83 is
1.90% faster than the central stride-97 control. It also lowers the modeled
mean input/neuron path from 15.182 to 14.246 hops.

The unbundled handlers are compile-time specialized: they do not parse a second
trace or perform bundled-payload validity checks. `MNIST_TRACE_BUNDLE=1` remains
a functional comparison mode, but performance measurements use the unbundled
configuration so the communication pattern is not tied to this placement's
two-neuron grouping.

The remaining network cost is backpressure rather than hidden receiver work.
All network-balanced stride candidates with that sender retire exactly
1,734,151 instructions and accept exactly 85,489 injections in the profiled
fifth frame. Stride 83 records 1,368,000 blocked injection presentations,
compared with 1,415,569 for stride 97. The new blocking sender accepts 85,467
injections and records 1,420,020 blocked injection presentations in its shorter
19,455-cycle interval. A blocked packet remains presented on each subsequent
cycle, so these counts are PU-cycles rather than distinct packets. Blocking is
broadly distributed: the optimized profile needs 478 links to cover 90% of
blocked link cycles and its busiest link contributes only 0.303%. The tighter
sender thus exposes slightly more backpressure but still shortens the critical
path substantially.

Prime-stride cyclic destination tables remain important: nearby neuron senders
start 83 positions apart in the input-PU list, and nearby input senders start
167 positions apart in the neuron-PU list. This preserves every destination
while avoiding the deterministic-XY synchronization that previously made many
senders contend for the same horizontal links.

A fixed no-spike control with one warm and two timed frames now takes 5,343 and
5,343 cycles, exactly half the previous 10,692.5-cycle mean. Both intervals
have exactly zero unicast
memory requests, zero I-cache fills, and zero I-cache stall cycles. This proves
that a warmed fixed runtime path neither accesses a DRAM stack nor refills the
I-cache, and that the empty observed-source list suppresses every reverse trace.

The activity-aware run's four ordinary stochastic intervals have 66, 36, 60,
and 48 I-cache fills, respectively, producing 132, 72, 120, and 96 unicast
requests.
These are first
executions of per-PU stochastic branches that the four warm frames did not
exercise, not capacity refills: the entire text fits in cache and the fixed-path
control stays at zero. With tree recurrence, frame data is the only runtime
DRAM input; the remaining hot-frame cost is local neuron/STDP computation plus
active-message dispatch rather than sustained DRAM traffic.

## Build and run

```sh
nix develop --command bash -c \
  "cd /root/workspace/ActiveN/sim/payloads && \
   make sys/mnist_train.bin \
     MNIST_RULE=three-trace MNIST_RECURRENT_MODE=tree \
     MNIST_TRACE_BUNDLE=0 MNIST_PARALLEL_INPUT_PREPARE=1 \
     MNIST_INPUT_DECAY_MODE=1 MNIST_FUSE_NEURON_UPDATE=1 \
     MNIST_PIPELINE_INPUT_PREPARE=1 MNIST_HORIZONTAL_GROUP_COUNT=8"

PYTHONDONTWRITEBYTECODE=1 python3 datagen/mnist_train.py \
  --rule three-trace --recurrent-mode tree \
  --input-pus 288 --neuron-pus 224 \
  --placement-policy near --neuron-zone-policy network-balanced \
  --input-assignment rate-balanced \
  --horizontal-partition full-8 --horizontal-replica-stride 3 \
  --feedforward-destination-stride 167 --trace-destination-stride 83 \
  --warmup-steps 4 --timed-steps 12 \
  --output copilot/tmp/mnist-train
```

Use `--frame` and `--frame-index` for an IDX MNIST image. The manifest
records the role PU lists, complete neuron MC zones, per-PU storage, estimated
hop counts, warm/timed step counts, and initial weight checksum.

## Runtime checkpoint corpus

`mnist_genn_snapshots.py` trains a dense FP32 GeNN network from initial weights
or resumes learned parameters from a portable checkpoint. `--rule` selects
one-, two-, or three-trace STDP (default: three-trace). The default
`--collection-mode single-pass` collects runtime state during one live training
trajectory. Separate Algorithm R reservoirs select presentation and rest
starts uniformly within each phase, including retry attempts. For 100 snapshots
the default capacities are 70 presentation and 30 rest. Reservoir decisions use
a separate host RNG and do not advance the network RNG. Only the retained
snapshot slots occupy disk; replacements are written atomically. Snapshot
indices identify reservoir slots, not chronological order. Within each phase,
only the last replacement of a given slot needs a GPU state copy.

A nonzero model seed is checked against the seed compiled into `librunner.so`,
but it does not guarantee a bitwise-repeatable CUDA trajectory. The final
weights/theta digest and retained snapshot hashes are recorded at completion.
The old two-pass procedure remains available as `--collection-mode replay`,
with its strict per-attempt and final-digest checks; it is not the default
because CUDA spike ordering can make a replay diverge.

The portable checkpoint contains weights and theta but no neuron or RNG state.
Consequently, resuming one initializes a new trajectory from learned
parameters, not the hidden state of the historical training process.
Every exported snapshot includes current E/I membrane and
conductance state, refractory counters, rule-specific pre/post traces, pending
GeNN synaptic input folded into conductance, frame/intensity metadata, and a
deterministic nonzero ActiveN xorshift state for each Poisson neuron.

For three-trace STDP, the exporter reconstructs nearest-spike traces from the
latest spike times. For the additive one-/two-trace rules, it pulls the live
presynaptic `x` and, for two-trace, postsynaptic `y` state. These are equivalent
to accumulating contributions from the spike history, not to exponentiating
only the latest spike's age. Unused trace fields are zero. Portable checkpoints
still contain only weights and theta; traces are needed in runtime snapshots,
not in those checkpoints. `--initial-weights` starts at accepted sample zero,
and training saves checkpoints every 10,000 accepted samples
and at its endpoint. A fresh run does not require a previously trained checkpoint.

```sh
python3 datagen/mnist_genn_snapshots.py \
  --checkpoint /path/to/checkpoint_030000.npz \
  --reuse-build /path/to/nonzero-seed-genn-build \
  --model-seed 19260817 --end-sample 40000 \
  --snapshots 100 --presentation-snapshots 70 \
  --window-steps 16 --output copilot/tmp/mnist-runtime-snapshots

PYTHONDONTWRITEBYTECODE=1 python3 datagen/mnist_snapshot_benchmark.py \
  --snapshots copilot/tmp/mnist-runtime-snapshots \
  --output copilot/tmp/mnist-runtime-benchmark \
  --warmup-steps 4 --timed-steps 12 \
  --jobs 2 --soft-threads 8
```

The benchmark driver retains one image, generation log, simulation log, and
parsed result per snapshot so interrupted runs are resumable. The summary
reports all intervals and a strict hot subset for which both I-cache fills and
unicast memory requests are zero. It also stratifies presentation/rest and
excitatory-active/zero-spike intervals and derives input, excitatory, and
inhibitory spike counts from timer-scoped message traffic without adding
instructions to the payload.

### Initial realistic 30k-to-40k benchmark

The initial unpartitioned 100-snapshot run in
`copilot/tmp/mnist-runtime-30k-40k-100-benchmark-20260903` uses the trained 30k
weights and theta, deterministic replay through accepted sample 40,000, and a
70-presentation/30-rest random selection. The selection spans accepted samples
30,008 through 39,961. Replay encountered 31 retry attempts among 10,031 total
attempts, and both passes ended at FP32 state digest
`f0430a5e32ce3a67bdc2a7df6244451cd4baaaabbaa173ceb822328741ff8618`.

The measured configuration is:

| Property | Value |
| --- | --- |
| Hardware shape | 512 PUs, 16 MCs, 16 KiB SPM per PU |
| PU roles | 196 input PUs, 224 paired excitatory/inhibitory PUs, 92 unused PUs |
| Plasticity | Dense, unbundled, three-trace STDP |
| Recurrent delivery | Repeated tree |
| Placement | Network-balanced neuron zones, rate-balanced input assignment |
| Timestep | 0.5 ms |
| Simulator | Cycle-aligned soft system backend, flat memory, eight host workers |
| Sampling | 100 independent runtime snapshots, four warm-up and twelve timed steps each |

This run predates the generalized horizontal-layout payload and has payload
SHA-256 `5ad93b8e0d357b3b49002f306791d0438ac95235c0b4c6eb6045ba655467f1bf`.
It establishes the realistic workload and earlier behavior, but it is not used
as the performance control for the current layouts. The comparison below
reruns the unpartitioned layout with the identical current payload and
simulator used by both horizontal candidates.

The rate-balanced assignment uses the per-pixel mean intensity over the MNIST
training set stored in each snapshot's `placement_scores`; it does not infer a
new placement from the selected frame. This is useful for evaluating the
current implementation, but it is prior workload knowledge and must not be
assumed by a rate-independent partitioning design.

Each independent simulator run performs four untimed warm-up steps and twelve
timed steps. All 100 runs completed successfully, all 100 final checksums are
distinct, and all 1,200 interval totals pass the parser's consistency checks.
The 1,200 steps contain 2,543 input spikes, 24 excitatory spikes, and 66
inhibitory spikes.

| Interval subset | Steps | Mean cycles | Median | P95 | Maximum |
| --- | ---: | ---: | ---: | ---: | ---: |
| Realistic presentation/rest mix | 1,200 | 11,465.36 | 14,405 | 15,109 | 36,237 |
| Presentation | 840 | 14,089.19 | 14,572 | 15,157 | 36,237 |
| Rest | 360 | 5,343.08 | 5,343 | 5,343 | 5,357 |
| Input active | 783 | 14,725.83 | 14,591 | 15,164 | 36,237 |
| Excitatory active | 20 | 16,382.95 | 16,698 | 17,802 | 17,904 |
| Strictly hot, all phases | 470 | 6,334.15 | 5,343 | 14,113 | 14,581 |
| Strictly hot, presentation | 110 | 9,577.67 | 5,357 | 14,348 | 14,581 |
| Strictly hot and input active | 53 | 14,131.15 | 14,090 | 14,437 | 14,581 |

The 840 presentation steps contain 57 zero-input steps and 783 active steps.
An active presentation step has 3.248 fired inputs on average. The full input
spike-count distribution is:

| Fired inputs in one step | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Presentation steps | 57 | 137 | 175 | 166 | 130 | 83 | 51 | 22 | 12 | 4 | 2 | 1 |

Under the actual rate-balanced placement, only 3.244 of the 196 input PUs are
active in an average active step, or 1.655%. The work is therefore globally
sparse even though the active work is well distributed: 780 active steps have
exactly one fired input on every participating PU, and only three have two
fired inputs on one PU. No step has more than two.

Replaying the exact same 2,543 Poisson firings through a contiguous assignment
provides a mapping-only counterfactual. Its cycle time was not simulated.

| Placement statistic | Rate-balanced | Contiguous |
| --- | ---: | ---: |
| Expected-load coefficient of variation | 1.08% | 123.58% |
| Expected maximum / mean | 1.047 | 3.824 |
| Realized spikes per PU, minimum / median / maximum | 3 / 13 / 22 | 0 / 5 / 62 |
| Realized-load coefficient of variation | 28.94% | 127.52% |
| Active steps with a same-PU collision | 3 | 55 |
| Maximum simultaneous fired inputs on one PU | 2 | 3 |
| Mean active PUs per active step | 3.244 | 3.175 |

The current placement has therefore already removed almost all accidental
same-PU concentration. Horizontal partitioning would address a different
problem: even a PU with exactly one fired input must serially update that
input's 400-synapse row, while more than 98% of input PUs are idle.

"Strictly hot" means exactly zero unicast memory requests and zero I-cache
fills in the interval. Across the full corpus, 29,549 fills produce exactly
59,098 unicast requests, or two requests per fill. Every strictly hot interval
also has zero I-cache stall cycles. Because every snapshot starts an independent
simulator with empty per-PU caches, four stochastic warm-up steps cannot execute
every sparse path on every PU. Consequently, none of the 20 excitatory-active
steps is strictly hot; their cost includes first execution of the firing-post
handler across the input PUs.

A deterministic 100-tick continuation of snapshot 65 is retained in
`copilot/tmp/mnist-runtime-snapshot065-long-benchmark-20260904`. Its first twelve
intervals exactly reproduce the corresponding corpus result. The first two
active intervals contain three and two excitatory spikes; no later interval
fires an excitatory neuron, so this trajectory still does not provide a strictly
hot excitatory event. The first event takes 16,899 cycles and cold-starts the
reverse path with 1,166 fills. The second takes 14,540 cycles with only eight
remaining fills. For comparison, twelve fully hot one-input, excitatory-zero
steps in the same continuation average 14,086.5 cycles. The 453.5-cycle
difference is not a pure excitatory cost because the second firing interval also
contains three inhibitory spikes, but it bounds the warmed-path overhead much
more closely than the independent-snapshot mean. Across the full continuation,
39 strictly hot input-active steps average 14,168.33 cycles.

Three of the 1,200 steps contain two fired inputs colocated on one input PU.
They average 36,097.67 cycles, compared with 14,597.86 cycles for input-active,
noncolliding, excitatory-zero steps. The reverse receiver already uses a flat
vector of direct weight-row pointers. The remaining penalty is load imbalance:
the colliding PU must update two rows for each of 400 columns and becomes the
phase critical path. These three rare steps add about 54 cycles to the overall
mean.

All 100 unpartitioned layouts use 1,254,400 bytes for the dense FP32 weight
matrix. The fullest input PU ends at SPM byte 7,772, leaving 4,516 bytes before
stack 0 at byte 12,288. Both 2 KiB SMT stacks therefore remain disjoint from
data. The three-trace tree payload text is 5,648 bytes and fits completely in
the 8 KiB I-cache.

The random presentation subset includes all ten digits. Presentation means are
not normalized for spike count or cold-path coverage, so they describe this
sample rather than an intrinsic per-digit execution cost.

| Digit | Snapshots | Input spikes | Excitatory spikes | Mean cycles |
| --- | ---: | ---: | ---: | ---: |
| 0 | 3 | 140 | 0 | 15,418.25 |
| 1 | 7 | 176 | 0 | 13,105.60 |
| 2 | 6 | 240 | 1 | 14,539.75 |
| 3 | 11 | 434 | 1 | 14,386.77 |
| 4 | 6 | 206 | 2 | 13,882.82 |
| 5 | 5 | 214 | 4 | 14,388.62 |
| 6 | 11 | 433 | 7 | 14,412.58 |
| 7 | 6 | 201 | 3 | 13,971.82 |
| 8 | 8 | 329 | 0 | 14,436.15 |
| 9 | 7 | 170 | 6 | 12,808.30 |

### Controlled horizontal-partition benchmark

The controlled comparison uses the same 100 snapshots, the same four warm-up
and twelve timed steps per snapshot, repeated-tree recurrence, and one current
unbundled three-trace payload. All three result sets record payload SHA-256
`1b92161a40b4ad748245d13793a134be2c0925c0f3e0ecbb751b0c7fb7ecfa59`
and simulator SHA-256
`c035d5248edd1374d114b901e0a35c786e419a995ad3ab491f3bd9b53c44050e`.
Every one of the 300 simulator runs succeeds. All 100 final checksums match
across all three layouts, including the horizontal replica-state fingerprint.

The rate-informed partial layout uses all 288 non-neuron PUs as three
homogeneous classes: 92 first-half owners, 92 second-half owners, and 104
whole-row owners. It duplicates and splits the 368 inputs with the largest
training-set placement scores; the other 416 retain one 400-column owner. The
selected set accounts for 2,477 of the 2,543 measured input spikes, or 97.40%.
Each physical input PU holds four records.

The rate-independent full layout divides the nine input-only MC zones into two
interleaved 144-PU classes and duplicates every input. The seven complete
neuron MC zones are divided into two interleaved 112-PU groups, each owning 200
matched E/I pairs. Every logical input therefore has one 200-column owner in
each class, with five or six records per input PU. Neuron state and update work
are not duplicated; only input RNG, firing, and pre-trace state are replicated.

Both copies start with the same RNG and pre-trace state, receive the same
threshold, and advance exactly once inside the phase-scoped input-prepare
barrier. Only the primary copy contributes to the global logical spike count.
The final checksum XORs a fingerprint of every replicated `(input ID,
threshold, pre trace, RNG)` tuple, so equal pairs cancel and divergent replicas
change the result.

| Layout property | Unpartitioned | Hot partial | Full |
| --- | ---: | ---: | ---: |
| Physical input PUs | 196 | 288 | 288 |
| Physical input records | 784 | 1,152 | 1,568 |
| Physical weight bytes | 1,254,400 | 1,843,200 | 2,508,800 |
| Maximum SPM data end | 7,772 | 7,772 | 10,588 |
| Headroom before stack 0 | 4,516 | 4,516 | 1,700 |
| Universal destinations per firing post | 196 | 196 | 144 |
| Mean input-to-neuron hops | 14.246 | 16.356 | 16.177 |

The horizontal rows currently retain 400 physical FP32 slots so a reverse
message can carry the same global byte offset in every layout; the unowned half
is zero. Logical weights are partitioned, not duplicated. Even the padded full
layout uses only 2,508,800 of the machine's 8,388,608 aggregate SPM bytes, and
the per-PU data end remains below the stricter 12 KiB stack boundary.

End-to-end interval results are:

| Interval subset | Steps | Unpartitioned | Hot partial | Full |
| --- | ---: | ---: | ---: | ---: |
| Realistic 70/30 mix | 1,200 | 11,904.08 | 9,390.82 (-21.11%) | 9,050.55 (-23.97%) |
| Presentation | 840 | 14,677.79 | 11,058.70 (-24.66%) | 10,545.61 (-28.15%) |
| Rest | 360 | 5,432.08 | 5,499.08 (+1.23%) | 5,562.08 (+2.39%) |
| Input active | 783 | 15,350.80 | 11,463.38 (-25.32%) | 10,908.35 (-28.94%) |
| Excitatory active | 20 | 16,770.35 | 12,508.40 (-25.41%) | 12,866.65 (-23.28%) |
| Jointly strict-hot and input-active | 44 | 14,624.34 | 10,137.41 (-30.68%) | 9,955.05 (-31.93%) |

For all 1,200 intervals, the unpartitioned, partial, and full P95 values are
15,844.0, 15,225.1, and 11,283.1 cycles. Their maxima are 36,971, 25,702, and
22,052 cycles. Full partitioning therefore improves both the mean and tail,
while partial partitioning retains a long collision tail. The 20
excitatory-active samples are all independently cold; two full-layout row
collisions coincide with those events, so their small-sample mean should not be
interpreted as a general partial-layout advantage.

The reconstructed instantaneous input load explains the result:

| Input-load statistic | Unpartitioned | Hot partial | Full |
| --- | ---: | ---: | ---: |
| Split spike events | 0% | 97.40% | 100% |
| Mean active physical input PUs | 3.244 | 6.309 | 6.455 |
| Steps with a local fired-row collision | 3 | 37 | 16 |
| Mean critical presynaptic synapses | 401.53 | 225.54 | 204.09 |
| P95 critical presynaptic synapses | 400 | 400 | 200 |
| Maximum critical presynaptic synapses | 800 | 600 | 400 |
| Maximum local firing-post scan | 1,600 | 1,600 | 1,200 |

Rate-balanced unpartitioned placement already removes long-term identity
imbalance: 780 of 783 active steps place at most one fired row on each active
PU. The instantaneous workload is nevertheless sparse and structurally
unbalanced. Only 3.244 of 196 input PUs are active in an average active step,
and each must serially process a full 400-column row. Full horizontal
partitioning addresses that per-spike critical path without knowing any input
rate. Partial partitioning concentrates its hot halves onto only 92 PUs per
class, increasing collisions despite a good prior. The full layout is therefore
the preferred implementation among these original two-way candidates. The
four-way result below supersedes it as the current end-to-end configuration.

All three layouts send the same 569,632 feedforward packets. Total reverse
trace packets are 1,020,629, 1,005,829, and 1,014,183 respectively; source-PU
deduplication makes collision-heavy partial slightly lower. The independent
snapshot runs encounter 37,565, 51,031, and 47,578 I-cache fills, each producing
exactly two unicast requests. Every jointly strict-hot interval has exactly
zero unicast requests, fills, and I-cache stalls. Thus the 31.93% joint-hot
full-layout improvement is not a cache-miss artifact, and the larger number of
physical owners does not create warmed-cache refill traffic.

The generalized neuron reduction packs the inhibitory count and up to four
group-specific excitatory counts into two words. The original timer probe
masked 16 low bits and therefore misreported group-0 posts as inhibitory
spikes; the benchmark parser now masks the actual 10-bit field, and the
simulator probe has the same correction. A replay of checkpoint 65 after
rebuilding the probe reproduces every original cycle count and checksum while
reporting the expected inhibitory sequence, so this was a statistics-only
defect. The rebuilt simulator used for that replay has SHA-256
`895a1d437b9732b91e6e56d94b004002948b50d2d189dcae68b56b9e4e5ca529`.

### Dual-SMT, sender-precompute, and four-way benchmark

The next controlled run first adds two approved changes to the preferred
two-way layout: neuron PUs update their second local E/I pair on SMT thread 1,
and a firing post computes `eta_plus * y2d` once before its destination loop.
The latter changes FP32 association as documented above. Four of 100 final
checksums differ from the preoptimization binary because a post fired during
the warm or timed window. The independent forced-post host oracle validates the
reassociated arithmetic and its expected low-bit differences. All 1,200 input,
excitatory, and inhibitory activity records remain identical.

Those two changes together reduce the realistic mean from 9,050.553 to
8,956.139 cycles, or 1.043%. The 66 input-active intervals that are strictly hot
in all compared binaries move from 9,951.939 to 9,943.333 cycles, only 0.086%.
The 20 independently cold excitatory-active intervals rise by 2.761% while
their I-cache stall total rises, so that subset does not establish a hot
potentiation-path regression. This optimization is correct but small at the
current phase critical path.

The final run then changes only the generated layout and matching generic
payload path from two 200-column owners to four compact 100-column owners. It
uses the same 100 snapshots, four warm-up and twelve timed steps per snapshot,
unbundled three-trace rule, repeated-tree recurrence, simulator binary, and
eight host workers. Every simulation succeeds. All 100 final checksums and all
1,200 activity records match the optimized two-way control exactly.

| Interval subset | Steps | Two-way before SMT/precompute | Optimized two-way | Optimized four-way |
| --- | ---: | ---: | ---: | ---: |
| Realistic 70/30 mix | 1,200 | 9,050.55 | 8,956.14 (-1.04%) | 8,311.55 (-7.20%) |
| Presentation | 840 | 10,545.61 | 10,414.59 (-1.24%) | 9,119.21 (-12.44%) |
| Rest | 360 | 5,562.08 | 5,553.08 (-0.16%) | 6,427.01 (+15.74%) |
| Input active | 783 | 10,908.35 | 10,768.45 (-1.28%) | 9,313.61 (-13.51%) |
| Excitatory active | 20 | 12,866.65 | 13,221.90 (+2.76%) | 10,124.25 (-23.43%) |
| Strictly hot input active in all binaries | 66 | 9,951.94 | 9,943.33 (-0.09%) | 8,411.17 (-15.41%) |

Percentages in the optimized two-way column are relative to the preceding
two-way binary. Percentages in the four-way column are relative to optimized
two-way. Four-way is 8.165% faster than the original two-way binary over the
complete mix. It also improves the optimized two-way median, P95, P99, and
maximum from 10,260, 11,057.05, 20,641.38, and 22,044 cycles to 8,751, 9,868,
14,310.09, and 15,080 cycles respectively.

The layout change has the following physical effects:

| Property | Optimized two-way | Optimized four-way |
| --- | ---: | ---: |
| Physical input records | 1,568 | 3,136 |
| Physical input firings in corpus | 5,086 | 10,172 |
| Physical weight bytes | 2,508,800 | 1,254,400 |
| Maximum per-PU SPM data end | 10,596 | 5,332 |
| Maximum all-post reverse messages | 57,600 | 28,800 |
| Mean critical presynaptic synapses | 204.09 | 104.85 |
| P95 / maximum critical presynaptic synapses | 200 / 400 | 100 / 200 |
| Steps with a local fired-row collision | 16 | 38 |

Four-way keeps the 569,632 feedforward packets unchanged and reduces reverse
packets from 1,014,183 to 1,003,255. Its compact rows remove all padding and
restore physical weight storage to the 1,254,400-byte logical minimum. Although
the number of collision steps rises, their mean falls from 21,027.75 to
14,170.03 cycles because a colliding PU now owns two 100-column quarters rather
than two 200-column halves. Noncollision active steps average 9,065.90 cycles.

Independent four-way runs execute more first-use paths across the extra input
replicas: I-cache fills rise from 41,693 to 49,262, and each fill still produces
exactly two unicast memory requests. I-cache stall core-cycles nevertheless
fall from 7,215,454 to 4,034,138. Every jointly hot interval has zero unicast
requests and fills; among the 66 jointly hot input-active steps, four-way is
15.41% faster. The extra fills are therefore compulsory path coverage, not
capacity refills or runtime DRAM weight traffic.

The final summary is in
`copilot/tmp/mnist-runtime-horizontal-full4-opt145-benchmark-20260904` and has
SHA-256
`42ae5b367e7429b9327ef78fdb32c9b3bd210d79d09bbc28364beb41a8e97115`.
Its frozen generator, payload, and simulator hashes are respectively
`d479f46eddd91d541e7a7806a41b0231b5e43fcfcdfd23e4b8fa333efa89b206`,
`f611613dbc7ecf85b93c1c8d8a15d8b5d66bf896c39a875e19674b318747d3bc`,
and `895a1d437b9732b91e6e56d94b004002948b50d2d189dcae68b56b9e4e5ca529`.

### Dual-SMT input preparation and decay benchmark

The input path uses the already-installed phase-local worker and join handlers;
it does not allocate another event type or add an atomic work queue. For the
four-way layout, thread 0 processes the first five or six local records and
thread 1 processes the final five. Each writes to a fixed fired-vector segment.
The join receives both 16-bit counts in one payload word and copies only the
fired pointers in the second segment, at most five, behind the first compact
sublist. This preserves the old feedforward row order and makes the RNG and
weight update sequence directly comparable with the serial implementation.

Pre-trace decay previously ran on every input PU inside the phase-start handler
before that PU forwarded the start event down the tree. It is independent local
work, and serial placement therefore lengthened the tree's critical path. The
selected schedule runs each input PU's two static decay halves during the
neuron phase. The existing full-tree neuron reduction is unchanged and already
guarantees that both halves finish before any reverse trace can be sent.

A three-snapshot pilot uses ordinary presentation snapshot 000, fully warm rest
snapshot 007, and five-excitatory-spike snapshot 065. Every variant runs four
warm-up and twelve timed steps and produces the same three final checksums and
the same input, excitatory, and inhibitory activity sequence.

| Input schedule | Snapshot 000 | Snapshot 007 | Snapshot 065 | Three-case mean | Change from legacy |
| --- | ---: | ---: | ---: | ---: | ---: |
| Legacy serial prepare and phase-start decay | 9,190.50 | 6,427.00 | 9,259.67 | 8,292.39 | - |
| Parallel prepare only | 9,192.75 | 6,429.00 | 9,407.08 | 8,342.94 | +0.61% |
| Parallel neuron-phase decay only | 8,595.75 | 5,790.00 | 8,733.83 | 7,706.53 | -7.07% |
| Parallel prepare and separate neuron-phase decay | 8,362.50 | 5,794.00 | 8,623.92 | 7,593.47 | -8.43% |
| Parallel prepare with fused decay | 8,418.25 | 5,790.00 | 8,660.25 | 7,622.83 | -8.07% |

Parallel preparation by itself is neutral on a warm fixed path and loses on
independent cold starts because it adds worker/join code. Removing decay from
serialized tree fanout is the primary optimization. Once that bottleneck is
gone, splitting preparation improves the active snapshots enough to offset its
four-cycle rest overhead. Fusing decay into preparation saves a traversal but
extends that phase's critical path; it is 0.39% slower than the selected
separate schedule over the pilot. Mode 1 therefore remains the default.

The selected schedule was then run over the complete 100-snapshot corpus with
the same four warm-up and twelve timed steps, full-four layout, unbundled
three-trace rule, repeated-tree recurrence, images, and simulator as the prior
control. All 100 runs succeed. All 100 final checksums and every one of the
1,200 per-frame activity and message-count records match the control exactly.
The corpus still contains 2,543 input, 24 excitatory, and 66 inhibitory spikes,
569,632 feedforward messages, and 1,003,255 reverse trace messages.

| Interval subset | Steps | Previous four-way | Dual-SMT input schedule | Change |
| --- | ---: | ---: | ---: | ---: |
| Realistic 70/30 mix | 1,200 | 8,311.55 | 7,588.46 | -8.70% |
| Presentation | 840 | 9,119.21 | 8,357.51 | -8.35% |
| Rest | 360 | 6,427.01 | 5,794.01 | -9.85% |
| Input active | 783 | 9,313.61 | 8,544.12 | -8.26% |
| Excitatory active | 20 | 10,124.25 | 9,764.70 | -3.55% |
| Jointly strict-hot, all phases | 494 | 6,833.07 | 6,198.99 | -9.28% |
| Jointly strict-hot and input active | 99 | 8,453.15 | 7,814.83 | -7.55% |

Of the 1,200 paired frames, 1,198 are faster and two are slower. The new median,
P90, P95, P99, and maximum are 8,027, 8,681.1, 8,803.3, 13,754.11, and 14,567
cycles, compared with 8,751, 9,551, 9,868, 14,310.09, and 15,080 cycles for the
control. Thus the optimization improves the complete distribution as well as
the mean.

I-cache fills fall from 49,262 to 45,446 and still produce exactly two unicast
requests apiece. Aggregate I-cache stall core-cycles rise by 2.76%, from
4,034,138 to 4,145,329, because the larger binary first-touches more of the rare
firing-post path on individual input PUs. The only two slower frames are cold
excitatory-active intervals with 144 and 424 more fills than the control. Every
one of the 494 intervals that is strict-hot in both binaries has exactly zero
unicast requests, fills, and I-cache stall cycles. Their 9.28% improvement is
therefore steady-state execution, not a cache artifact. No excitatory-active
interval is jointly strict-hot, so the 3.55% excitatory-active result still
includes compulsory per-PU path coverage.

At that historical full-four checkpoint, the selected three-trace tree payload
had 6,568 bytes of text, used no stack-relative runtime memory access, and left
1,624 bytes of the 8 KiB I-cache unused. Rebuilding with both new switches set
to zero reproduced the control payload SHA-256
`f611613dbc7ecf85b93c1c8d8a15d8b5d66bf896c39a875e19674b318747d3bc`
exactly.

The complete result is in
`copilot/tmp/mnist-runtime-horizontal-full4-dual-input-benchmark-20260905`.
Its summary SHA-256 is
`c314cc4c485e2f27f8d6c691dd78b3f38c1e8cf19d39e1f1d73f5ab52f386973`.
Its generator, payload, and simulator SHA-256 values are respectively
`f74edb5f02c32554b81ce2a0a401fba07e5b3a1528b17cb5de43a6b5f0b94ce3`,
`ed3bc45826e41372d313d01a503baa855d52d10e7f6b0fc408209fdf56b3efc8`,
and `895a1d437b9732b91e6e56d94b004002948b50d2d189dcae68b56b9e4e5ca529`.

### Full-eight pilot before receiver simplification (2026-09-06)

The configuration at this pilot checkpoint is unbundled three-trace STDP, repeated-tree
recurrence, all-input `full-8` partitioning, `N=288`, `M=224`, replica stride 3,
feedforward stride 167, and reverse stride 83. Both SMT preparation switches,
fused neuron work, and next-step preparation pipelining are enabled. The binary
contains 6,672 bytes and has SHA-256
`d00020d3dc029181c1ab732c6f7411d3697119d7c4360751bf5b9a7db0c7f031`.
The generator, benchmark driver, and simulator hashes are respectively
`f39124af387bf56f71f90d00066ce9133a796ecffff3da1c1b4abacc7d4b5bdb`,
`43fd6c791762fd74b568c572d6f5c9708b3ee8629a97bc01215f70390d64ed24`,
and `735dc39b58586e2ce993fe320a010a6bc3ff8b6005a0e1b9706578e180544953`.

Three checkpoint pilots each use four warm-up and twelve timed steps, one
simulator at a time, and eight soft-backend host workers:

| Snapshot | Timed activity | Immediate control | Current | Change |
| --- | --- | ---: | ---: | ---: |
| 000 | 31 input, 0 E, 0 I | 3,861.833 | 3,845.167 | -16.667 (-0.43%) |
| 007 | rest, no spikes | 2,163.833 | 2,143.833 | -20.000 (-0.92%) |
| 065 | 31 input, 5 E, 10 I | 4,063.000 | 4,051.000 | -12.000 (-0.30%) |

The immediate control differs only in loading the immutable subtree size from
SPM in every reducer event. All 36 current frames complete with the same three
checksums (`0x0923895e`, `0x3f2bca49`, and `0x0288fdd9`), activity, message
counts, and I-cache fill counts as their controls. Snapshot 000 improves all 12
individual frames. Snapshot 007 has 12 strictly hot frames and exactly zero
I-cache fills, unicast requests, or I-cache stall cycles. Snapshot 065 has two
strictly hot frames; its other requests are exactly two per first-use I-cache
fill, not data or stack traffic.

Relative to the exact full-eight binaries at the start of this optimization
pass, snapshot 007 falls from 2,609.833 to 2,143.833 cycles (-17.86%) and
snapshot 065 falls from 4,467.417 to 4,051.000 cycles (-9.32%). The final
control-path tail progressed as follows:

| Change | Snapshot 007 | Snapshot 065 |
| --- | ---: | ---: |
| Before dispatcher cleanup | 2,299.833 | 4,192.250 |
| Recurring-phase dispatcher first | 2,283.833 | 4,176.250 |
| Separate terminal XOR reducer | 2,203.833 | 4,109.500 |
| One-based child destination in header | 2,185.833 | 4,091.500 |
| Entry-tested phase-start child loop | 2,183.833 | 4,079.250 |
| Remove impossible normal-tree overflow test | 2,163.833 | 4,063.000 |
| Cache subtree size in thread-0 `ra` | 2,143.833 | 4,051.000 |

The terminal checksum alone uses the separate handler-15 XOR reducer. Normal
per-step reductions are add-only, phase-scoped, and generated entirely by the
trusted tree, so they no longer test for a count greater than the immutable
subtree size. Scatter mode and the terminal reducer retain their overflow
guards. The root still advances only on exact equality.

A phase profile taken immediately before the final small reducer changes shows
where the remaining time goes. A four-input/no-post frame spent 1,749 of 4,157
cycles in feedforward/neuron work and 2,408 cycles in reverse/weight work. A
three-input/three-post frame spent 1,665 of 5,529 cycles in feedforward/neuron
work and 3,864 cycles in reverse/weight work. Feedforward had zero blocked link
presentations in both profiles. Reverse/weight recorded 35,300 and 41,537
blocked link presentations, but only 170 and 68 blocked source-injection
presentations. Blocking is broad: 104 and 72 links are needed to cover 90%, and
the busiest link contributes only 1.79% and 2.08%. Thus the active critical path
is the combination of reverse network traversal and endpoint FP32 STDP work,
not DRAM bandwidth or one pathological link.

Two experiments were rejected. Conditional zero-elision in the reducer saved
eight cycles on the empty snapshot but only 0.167 cycles on snapshot 065 and
made its excitatory-active subset slower. The 256/256 PU split raised snapshot
065 from 4,109.500 to 4,610.333 cycles in its matched earlier binary because of
extra feedforward traffic and same-PU fired-row collisions. Their artifacts,
along with the selected pilots, are under
`copilot/tmp/mnist-reducer-zero-elide-20260906`,
`copilot/tmp/mnist-split-256-256-20260906`, and
`copilot/tmp/mnist-reducer-register-20260906`.

The historical 100-snapshot tables above remain the last completed comprehensive
corpus comparison and use full-four binaries. These full-eight pilots are an
optimization A/B and functional checkpoint, not a replacement corpus result.

### Optimization mechanisms and evidence

The large improvement is a combination of shorter synchronization paths,
overlapping independent work, narrower weight ownership, and less repeated
bookkeeping. It is not a change to the STDP equation, timestep, neuron count,
or instruction-cache size. All numbers in the following progression use the
same diagnostic snapshots 000, 007, and 065, with four warm-up and twelve timed
steps each. They are means of 36 steps, not estimates from all 100 snapshots.

| Checkpoint | Partition | Pilot mean cycles/step |
| --- | --- | ---: |
| Dual-SMT preparation, separate global phases | Four-way | 7,593.47 |
| Neuron work joins feedforward completion | Four-way | 6,659.31 |
| Also prepare next-step inputs at weight-phase completion | Four-way | 5,291.00 |
| Initial eight-way partition pilot | Eight-way | 4,706.25 |
| Later local/control optimizations, specialized receiver | Eight-way | 3,346.67 |

The first two scheduling changes reduce the matched four-way pilot by 30.32%.
Eight-way partitioning then reduces that pilot by another 11.05%. These are
sequential comparisons; percentages cannot be added, and their contribution
can change when combined with subsequent optimizations. The last row is the
historical specialized-receiver binary, not the shared-receiver build now
being benchmarked.

#### Static dual-SMT work assignment

Input preparation and pre-trace decay use two fixed contiguous record ranges.
Each thread advances only its own RNG states and writes its own fired-vector
segment. The join compacts the short fired lists and combines their counts;
there is no AMO to claim each neuron and no dynamic load balancer. Moving
pre-trace decay out of the phase-start tree also removes local computation
that previously delayed starting descendant PUs.

Replicas of the same logical input begin with identical RNG and threshold
state and advance exactly once per logical step, irrespective of whether they
fire. Consequently they generate identical spikes without runtime RNG
synchronization messages. Only the designated primary contributes to the
logical spike count. This invariant does not rely on a measured firing rate.
The XOR-based final fingerprint helps detect replica divergence, but is not
a collision-free proof of all state equality.

#### Fewer global rounds, with the necessary barriers retained

The previous recurring schedule was:

```text
prepare inputs -> global completion
send/receive feedforward -> global completion
update E/I neurons and decay pre traces -> global completion
reverse messages, weight updates, trace commits -> global completion
```

The selected steady-state schedule is:

```text
send/receive feedforward + local E/I updates + pre-trace decay
  -> global completion
reverse messages + weight updates + local trace commits + next-input preparation
  -> global completion
```

A neuron PU starts neuron work only after receiving its complete expected
feedforward count. An input PU decays its traces after finishing its sends.
The first global completion still waits for every input PU and neuron PU;
no reverse message or weight write starts while feedforward delivery remains
outstanding. Thus eliminating the intermediate global round does not weaken
the old-weight delivery requirement.

During the second phase, each input PU starts next-input preparation only
after its own reverse updates and current trace commits finish. The phase
completion includes that preparation, and the next feedforward phase waits
for the entire tree. Local preparation can therefore overlap slower PUs'
current work without letting next-step spikes compete with unfinished reverse
traffic. Root phase advancement and the full-tree completion conditions remain.

The initial step still needs a standalone preparation round. With four
warm-up steps, it is outside the measured intervals. Each ordinary timed step
includes preparation for the following step; work was moved between steps,
not removed from the recurring timed region. The final step deliberately
does not prepare an unused future step or advance its RNG. This gives a small
finite-window boundary benefit: for the historical specialized-receiver
pilot, including all twelve intervals gives 3,346.67 cycles, while dropping
each snapshot's final interval gives 3,381.91. Both should be distinguished
from a long-run steady-state mean. One benchmark "step" is 0.5 ms of model
time, not an entire MNIST image presentation.

#### Eight-way rows and rate-independent placement

Four-way partitioning gives each copy 100 weights; eight-way gives it 50.
There are still 288 input PUs and 224 neuron PUs. The eight groups contain
36 input PUs each, so local input-state records grow from about 11 to 21/22
per input PU. Weight storage does not increase: each logical synapse still
has one physical owner. Input-state/RNG work does increase because there are
eight replicas instead of four.

This trades more replicated neuron preparation for less STDP work on a
critical fired-row owner and fewer universal reverse destinations per post.
The total 224 feedforward packets per logical input spike is unchanged.
The assignment is index-cyclic, independent of image intensity or observed
firing history. Replica rotation and prime cyclic send starts distribute
traffic without changing XY routing or MC-aligned neuron zones.

#### Less repeated work in the local paths

- Each fired-vector entry contains both the absolute weight-row pointer and
  the input-record pointer. Reverse depression accesses the row directly;
  trace commit visits only the fired records rather than scanning all inputs.
- A source PU marks one feedforward row per phase. A single-threaded receiver
  appends that source once, eliminating the observed-source bitset, its clear
  loop, and duplicate-detection AMOs while preserving selective reverse sends.
- All local rows in a full partition share their destination segment. The
  feedforward sender reuses that setup across fired rows, streams descriptors
  and weights, and wraps the cyclic traversal at most once per row.
- The generator encodes the nonzero-RNG Poisson threshold as a cutoff so the
  runtime needs one unsigned comparison; zero and forced-fire thresholds keep
  their special semantics. Replicas and the reference still use the same RNG
  sequence and firing decisions.
- Post-dependent depression/potentiation factors and the weight byte offset
  are computed at the sender and shared with all receiving input PUs. Required
  per-weight arithmetic remains; the receiver does not approximate away the
  triplet rule's FP32 operations.

#### Cheaper tree and dispatch work

The phase-start handler now sends to child subtrees before performing
role-specific local setup. The immutable one-based parent and subtree count
are generated into SPM; thread 0 retains the count in its unused `ra` register
for recurring reductions. This is precomputation and register reuse, not a
larger hardware cache or a reduction in DRAM weight traffic.

Common phase dispatch tests come first. Per-step reductions are add-only, and
the terminal checksum uses a separate XOR reducer. The normal trusted-tree
path no longer checks an impossible count overflow, while exact completion
equality, startup validation, scatter guards, and terminal-checksum guards
remain. The final subtree-register change alone saved only 12-20 cycles per
pilot step; it is not the explanation for the multi-thousand-cycle gain.

#### Removed receiver specialization

The experimental receiver selected a one-neuron or two-neuron entry point at
startup. Both performed the same required additions as the shared handler,
but avoided decoding the optional second value on every packet. Its matched
snapshot-065 comparison was:

| Interval subset | Shared receiver | Specialized receiver | Reduction |
| --- | ---: | ---: | ---: |
| Twelve timed steps | 4,991.33 | 4,904.50 | 1.74% |
| Two jointly instruction-cache-hot steps | 4,151.50 | 4,146.50 | 0.12% |

The full twelve-step result also had fewer first-use instruction fills
(1,921 versus 1,769), so it does not isolate steady-state handler savings.
Checksums and activity matched. The experiment is preserved in
`copilot/tmp/mnist-ff-receiver-specialization-20260906`.

At the user's request, both specialized entries and their startup selector
have been removed. All neuron PUs now use one `ff_spike_handler`. The small
measured hot benefit did not justify retaining this extra mapping-dependent
code. This removal leaves the existing packet format and one/two-pair mapping
limit unchanged; supporting larger local populations is a separate extension.

### Shared-receiver full-corpus run (2026-09-06)

The shared-receiver payload has 6,608 bytes of linked text, 64 fewer than the
preceding selected binary, and SHA-256
`4bbe9f731cd8ffb57b212024915340938b928bc79352db2e69485fd94b2e1a85`.
The simulator and generator are unchanged. No RTL, queue depths, priorities,
handler thread masks, or STDP arithmetic changed in this removal.

All 30 generator and benchmark-parser tests pass. The three-case simulation
regression also passes, with the same final checksums and all 36 per-step
input/E/I activity and feedforward counts as the historical four-way control:

| Snapshot | Before removal | Shared receiver | Final checksum |
| --- | ---: | ---: | --- |
| 000, digit 7 presentation | 3,845.167 | 3,938.333 | `0x0923895e` |
| 007, rest | 2,143.833 | 2,143.833 | `0x3f2bca49` |
| 065, digit 9 presentation | 4,051.000 | 4,135.917 | `0x0288fdd9` |
| All 36 timed steps | 3,346.667 | 3,406.028 | All three match |

The shared receiver adds six cycles to snapshot 000's one jointly hot active
step and an average five cycles to snapshot 065's two jointly hot steps. All
twelve rest steps are unchanged and have zero unicast memory requests or
I-cache fills. The larger all-step increases also include additional first-use
instruction fills: 1,088 versus 952 for snapshot 000, and 1,921 versus 1,769
for snapshot 065. The matched pilot reduction against the earlier dual-SMT
four-way schedule is now 55.15%; this remains a pilot-only result.

The run directory is
`copilot/tmp/mnist-full8-shared-receiver-20260906`. It contains frozen copies
of the payload, generator, benchmark driver, simulator, configuration JSON,
IDX labels, and all 100 original snapshots, together with hash provenance and
the historical four-way control results. A separate three-case pilot gates
the full run. The original selection SHA-256 remains
`3d9061e3e0e11b75edc272458cda5d1af76abc0ea6a3233a0771f8527126e4d5`.
The full run started at 08:36:16 UTC on 2026-09-06 and passed at 10:52:22 UTC.
All 100 final checksums and all 1,200 timed steps' input/E/I activity and
feedforward counts match the frozen four-way control. Frozen input hashes also
passed their final verification.

The full run uses all 70 presentation and 30 rest snapshots, four warm-up
steps and twelve timed steps per snapshot, for 1,200 timed steps. The runtime
is unbundled three-trace, repeated-tree recurrence, eight-way partitioning,
288 input PUs, and 224 E/I PUs. Each simulator has eight host workers, and
only one simulator runs at a time. BLAS/OpenMP helper threads are capped at
one; no waveform or per-cycle NoC profile is collected. Each case is generated
into a new directory, preventing reuse of old payload results.

The driver reports per-step cycles, activity, traffic, I-cache fills and stalls,
mean/median/tail distributions, digit and presentation/rest groups, and
same-PU input firing collisions. After completion, `run.py` compares all final
checksums, every step's logical input/E/I activity, and feedforward counts
against the frozen four-way control. Reverse packet counts are not required
to match because changing the ownership groups changes that traffic.

Run and inspect through the Nix development shell:

```sh
nix develop --command bash -c \
  'PYTHONDONTWRITEBYTECODE=1 python -u \
   copilot/tmp/mnist-full8-shared-receiver-20260906/run.py full --detach'

tail -n 8 copilot/tmp/mnist-full8-shared-receiver-20260906/full.log
```

`full_status.json` records the exact command and passed state. The completed
results are in `full_results/summary.json` and `full_results/comparison.json`:

| Interval group | Steps | Mean cycles |
| --- | ---: | ---: |
| Complete 70/30 mix | 1,200 | 3,403.521 |
| Presentation | 840 | 3,943.387 |
| Rest | 360 | 2,143.833 |
| Input active | 783 | 4,074.073 |
| Excitatory active | 20 | 5,031.900 |

The complete mean is 55.15% below the previous 7,588.458-cycle full-corpus
result, or 2.230 times its throughput. This compares the combined schedule and
full-eight changes, not the receiver-specialization removal in isolation. The
three-snapshot estimate of 3,406.028 differs from the full result by only 0.074%.
Overall p95/p99/maximum are 6,081.25/6,823.03/7,262 cycles. Excluding every
snapshot's terminal step, which omits next-step preparation, gives 3,438.477
cycles over 1,100 steps.

The corpus contains 2,543 input, 24 excitatory, and 66 inhibitory spikes. There
are 604 strictly hot intervals, all with zero unicast memory requests and
I-cache stalls. Across all intervals, every unicast memory request is accounted
for by the two requests per instruction fill. On the 95 input-active intervals
that are strictly hot in both versions, the mean falls from 7,809.232 to
3,261.474 cycles (58.24%). This matched subset avoids comparing differently
selected hot populations.

There are 86 input-active steps with two locally fired rows on at least one
input PU: these average 6,298.012 cycles, versus 3,799.670 for the other 697
input-active steps. All 86 collision steps and all 20 excitatory-active steps
still include first-use instruction fills. Their cost therefore combines extra
work, handler overhead, cache fills, and communication; the difference is not
a measurement of removable instruction overhead alone.

### General fired-vector loop optimization (2026-09-07)

The subsequent change optimizes the ordinary variable-length fired-list loop,
not an entry specialized for the observed two-row case. It adds the two cached
header bounds described above, moves count/address validation to phase setup,
and replaces comparison/branch clipping with `fmax.s`/`fmin.s`. Both bounds
remain enforced. In the three-trace build the common loop drops from thirteen
to nine instructions per weight; setup now loads the two cached pointers and
the maximum weight. The two-trace build keeps its existing fifth-root
approximation and FP32 arithmetic order. One-trace has no presynaptic-only
depression loop, so that rule does not receive this loop optimization.

The min/max clipping uses canonical positive zero, as the existing unbundled
single-fired three-trace path already does. The trained corpus uses finite
nonnegative weights and traces; NaN propagation and preserving a negative-zero
weight bit pattern are not objectives of this clipping operation.

The updated payload sizes are 6,536/6,956/6,584 bytes for one/two/three trace,
all below the unchanged 8 KiB instruction-cache limit. All 37 generator and
benchmark-driver unit tests pass. The benchmark driver now accepts `--rule`,
stores it in results and summaries, and rejects cached images/results belonging
to another rule. Legacy results without a rule field are interpreted only as
three-trace results.

The three-snapshot regression passed for all rules, with matching final
checksums and every timed step's activity and feedforward/reverse packet counts
against the same-rule controls:

| Rule | Pre-change pilot mean | Optimized pilot mean | Cycle reduction |
| --- | ---: | ---: | ---: |
| One trace | 3,141.917 | 3,141.917 | 0.00% |
| Two trace | 7,377.194 | 7,330.222 | 0.64% |
| Three trace | 3,710.361 | 3,572.278 | 3.72% |

These means cover only snapshots 031, 065, and 007, not the complete corpus.
Snapshot 031 improves by 7.64% for three trace and 1.71% for two trace; the
two-trace snapshot 065 instead regresses by 0.46%. Rest timing is unchanged.
The one-trace control and optimized binaries have identical timing in all
three cases. No claim is made that fewer traces necessarily means less
arithmetic: the one-/two-trace rules retain their fifth-root weight dependence.

A separate directed old/new comparison forces 781 logical input firings,
reaching 22 local fired entries, and inserts weights that exercise zero,
depression below zero, and clipping above the configured maximum. Three-trace
checksums both equal `0x01184634`; two-trace checksums both equal `0x0033536d`.
Input/E/I activity and message counts also match. This is a correctness stress
test, not part of the realistic 100-snapshot performance distribution.

Experiment artifacts and preserved pre-change controls are in
`copilot/tmp/mnist-fired-vector-rules-20260907`. The serial three-case regression
uses snapshots 031, 065, and 007 to cover multiple local firings, excitatory
firings, and rest. One- and two-trace regression controls run the corresponding
pre-change rule, not the three-trace checksum. `pilot_status.json` and
`directed/summary.json` both record passing status. The full three-trace run
reuses the frozen 100-snapshot selection, four warm-up/twelve timed steps, one
simulator, and eight host workers:

```sh
nix develop --command bash -c \
  'PYTHONDONTWRITEBYTECODE=1 OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 \
   python -u copilot/tmp/mnist-fired-vector-rules-20260907/run.py \
   full --rules three-trace --detach'
```

Its results are written to `results/three-trace/summary.json`, with launch and
progress recorded in `full_status.json` and `full.log`. The three passing pilot
cases were reused with identical frozen inputs. The completed 100-snapshot run
passed all checksum/activity comparisons. Across 1,200 timed steps, the mean
was **3,281.5825 cycles**, versus **3,403.5208 cycles** for the paired control:
a **3.5827% reduction**. The strict hot subset contains 604 steps and averages
2,499.7020 cycles; none of those steps contains excitatory firing, so this subset
does not measure the hot firing-postsynaptic path. The user selected separately trained
one-/two-trace corpora, described below. Reusing three-trace-trained starting
states would instead be a controlled cross-rule continuation; the old controller
requires an explicit `--shared-state-rules` option for that distinct experiment.

### Rule-native 30k training and snapshot export (2026-09-07)

Artifact root: `copilot/tmp/mnist-rule-native-30k-20260907`.
The approved new runs train each dense rule from its historical initial weight
matrix through 30,000 accepted MNIST images, in fixed dataset order. They do not
resume the old three-trace checkpoint. Each rule gets 100 random runtime starts,
70 presentation and 30 rest, with 16 steps available within each selected phase.
The selection includes retried presentation/rest attempts. The seed is
19,260,817 for both the GeNN model and selection; the ActiveN Poisson seed is
`0x19260817`.

This is the **0k-30k training window**, per the latest user request. The existing
three-trace corpus is a **30k-40k continuation**. Their aggregate performance
must not be presented as a paired rule-only comparison: training age, learned
weights, retry schedules, and activity distributions differ. The existing
three-trace corpus and completed benchmark are preserved unchanged.

The original serial controller attempted the following for each rule:

1. Train/discover through accepted sample 30,000, saving weights/theta at
   10k, 20k, and 30k, with finite-state, runaway-spike, and retry-intensity checks.
2. Replay the identical fresh initialization and seeded GeNN RNG stream,
   export the chosen runtime states, compare every attempt's excitatory count
   and retry decision, and require identical final FP32 weights/theta digests.
3. Evaluate the new 30k checkpoint with plasticity disabled on the first 1,000
   test images, using the same activity for neuron assignment and scoring.
4. Evaluate the corresponding historical 30k checkpoint with that same current
   FP32 GeNN evaluator and seed, then record the difference in percentage points.

Historical reference scores under the 1,000-image simple-demo protocol are
83.2% for dense one-trace (GeNN CUDA, historical FP64/default seed-zero run),
and 84.5% for dense two-trace (Brian 1). The two-trace reference is the
`full_nupost0005_30000` case: `nu_pre=0.0001`, `nu_post=0.0005`, pre/post
time constants 20 ms, target 0.4, and both weight exponents 0.2. These are
reference observations, not pass thresholds or expected bitwise matches.
The matched current-evaluator checks help distinguish training differences
from backend, precision, RNG, and evaluation differences. Agreement in final
accuracy is not a substitute for per-step numerical differential testing.

Validation before launch: 46 unit tests passed. Each additive rule passed a
200-tick GPU trace check against recurrence reconstructed from per-tick input
and excitatory spike counts, including repeated spikes and traces above one.
The maximum presynaptic trace was 38.01723, with zero observed presynaptic
reconstruction error; two-trace postsynaptic values passed `rtol=atol=2e-6`.
Both 10-image export pilots produced ten snapshots (seven presentation/three
rest) and matched discovery/replay final digests. These checks validate trace
capture/replay, not a full ActiveN-versus-GeNN state comparison: exported
ActiveN Poisson states still initialize a separate xorshift stream.

Source files, input weights, historical checkpoints, dataset, payloads, and
simulator are frozen under `frozen/`, with hashes in `provenance.json`. The
installed GeNN Python/shared libraries are hashed and checked before/after the
controller. Builds are limited to two compiler jobs; one GPU process runs at a
time, and BLAS/OpenMP/MKL thread counts are one. No second ActiveN simulator is
launched by this training controller.

```sh
nix develop --command bash -c \
  'PYTHONDONTWRITEBYTECODE=1 OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 \
   python -u copilot/tmp/mnist-rule-native-30k-20260907/run.py full --detach'
```

Launch was recorded at `2026-09-06T21:44:10.800069+00:00` (host UTC), controller
PID 64640. It stopped at `2026-09-06T22:37:55` during two-trace replay.
The completed one-trace run is preserved: 30,000 accepted samples, 30,214
attempts (214 retries), all 100 snapshots, and matching discovery/replay digest
`bc67dd16e3b98308498d5bd737379c9c074117d83572151f0a8a3696607c43aa`.
Its new checkpoint scored **80.3%**, versus **82.4%** for the historical
checkpoint using the same current FP32 evaluator: **-2.1 percentage points**.
The historical published-protocol score remains 83.2%; it is not that matched
FP32 reevaluation.

Two-trace training reached 30,000 accepted samples in 30,220 attempts, with
checkpoints at 10k/20k/30k. Replay failed at attempt 10,374 (accepted sample
10,166): expected 15 excitatory spikes, observed 14. Its 34 partial snapshots
are not a complete benchmark corpus. These artifacts remain unchanged.

#### CUDA replay diagnosis and single-pass replacement

Read-only tests of the frozen two-trace build reproduced divergent FP32 state
without exporting any snapshots. Two fresh networks had identical initial
parameters and input spike sets, but their first input spike lists were ordered
`[575,625,217]` and `[575,217,625]`. At tick 2, excitatory neuron 7's conductance
differed by one ULP (`0x3e0695f8` versus `0x3e0695f7`); membrane voltage differed
by tick 5. All input spike sets matched over 700 ticks, while their orders
differed on roughly 31-33 ticks. Input traces matched.

The generated neuron kernel appends spikes with CUDA atomics, and the
feedforward kernel accumulates weights in that resulting order. Summing the
three corresponding weights in the observed orders reproduces the conductance
difference exactly. This establishes a source of replay divergence despite a
fixed seed; the diagnostic did not rerun the entire historical prefix to
reproduce the later attempt-10,374 mismatch. It is not evidence of an ActiveN
SMT race or missing trace checkpoint state.

The approved replacement lives in
`copilot/tmp/mnist-two-trace-single-pass-20260907`. It reruns only two-trace from
its historical initial weights, collecting snapshots directly during 0k-30k
training. The completed one-trace corpus and the failed two-trace run remain
frozen. Network equations, timestep, intensity/retry policy, and live additive
trace capture are unchanged. Capture occurs at the selected pre-step boundary;
the count baseline retains the original treatment of prior-rest spikes.

The controller gates the 30k launch on a 100-image GPU pilot and validation of
all 100 retained snapshots: selection metadata, phase boundaries, model time,
live trace arrays, zero rest input, snapshot hashes, and final checkpoint
digest. All 50 exporter/generator/benchmark unit tests passed. Tests cover
equivalence to elementwise Algorithm R, exact
small-case subset probabilities, bounded replacement slots, and mocked
single-network capture with retries and rest activity. A ten-image evaluation
pilot also checks the compiler path before the long run. Resource limits are
one GPU process, one BLAS/OpenMP/MKL thread, two `make` jobs, and CPU affinity
restricted to two host CPUs, including GeNN's separate block-size optimization
compiler subprocesses. After full
capture/validation it evaluates new and historical two-trace 30k checkpoints
serially. Rule-native 100-snapshot ActiveN simulations remain a separate,
subsequent stage.

The artifact-owned `shell.nix` restores the pinned CUDA 13 compiler/toolkit and
the CUDA 12 runtime required by the installed PyGeNN backend, without changing
that backend or the reused training binary. Launch status records the resolved
environment, CPU affinity, and controller/shell hashes. Two environment-only
pilot failures are preserved under `pilot-startup-failed/` (missing runtime)
and `pilot-toolkit-failed/` (capture/validation passed; evaluation compilation
lacked the cuRAND header output).

The successful pilot completed 100 accepted images in 111 attempts, retaining
70 presentation and 30 rest snapshots after 553 reservoir writes. All snapshot
checks passed; maximum pre/post additive traces were 5.635151 and 1.280004.
The ten-image evaluator build/run passed as a tooling check, not an accuracy
comparison. `pilot-status.json` records completion at
`2026-09-07T12:59:42.807706+00:00`.

The full replacement launched at `2026-09-07T13:00:01.145836+00:00`, controller
PID 68287, with CPU affinity `[0,1]`. Reproducible launch command:

```sh
nix develop --impure --cores 2 --max-jobs 1 \
  --expr 'import ./copilot/tmp/mnist-two-trace-single-pass-20260907/shell.nix' \
  --command bash -c \
  'python -u copilot/tmp/mnist-two-trace-single-pass-20260907/run.py full --detach'
```

The command deliberately refuses an existing full-run directory. Progress is
in `full-status.json` and `full-capture.log`; final snapshots/checkpoints go to
`full/snapshots/`, validation to `full/validation.json`, and evaluations to
`full/evaluate/` and `full/evaluate-old/`. `accuracy-comparison.json` is emitted
only after both evaluations finish.

The single-pass replacement finished at `2026-09-07T13:16:25.786495+00:00`.
Training/export took 907.0 seconds for 30,000 accepted images and 30,219 total
attempts (219 retries), retaining all 100 snapshots. The complete corpus passed
metadata, phase, live-trace, and checkpoint-digest validation. Its selected
accepted samples range from 131 to 29,395, and the final FP32 weights/theta
digest is `3e356dcf1762bd19c583477820ad943ad7cd1ce5d88fa2dda5763d646ab2379d`.
The new checkpoint's 1,000-image evaluation scored **83.4%**.

The historical two-trace checkpoint reevaluation is **invalid**: the import in
the artifact controller passed Brian theta values stored in volts directly to
the `theta_mv` field. For example, a median theta of 0.03844548 V should become
38.44548 mV, not 0.03844548 mV. This produced the anomalous 8.5% score and only
four assigned neurons. The raw `accuracy-comparison.json` therefore must not be
used as a valid improvement claim. That import still needs correction and a
separate reevaluation; it does not affect the new GeNN training or the native
ActiveN snapshot corpora. The historical 84.5% Brian score is a different
backend/protocol observation, not a matched current-evaluator control.

### Native one- and two-trace ActiveN benchmarks (2026-09-08)

Both 100-snapshot benchmarks completed successfully at
`2026-09-08T12:05:11.657014+00:00`. Artifact root:
`copilot/tmp/mnist-native-activen-100-20260908`.

Each rule uses its own trained 0k-30k corpus: one-trace selected accepted samples
28-29,903, and two-trace selected 131-29,395. Each corpus contains 70 presentation
and 30 rest snapshots. They are separate selections and trajectories, not paired
states for isolating a rule-only change. The earlier three-trace result of
3,281.5825 cycles uses the older 30k-40k corpus and remains a separate result.

The hardware, mapping, payload, and measurement settings are unchanged:

| Setting | Value |
| --- | --- |
| Simulator | Frozen soft system backend, flat memory |
| Hardware | 512 PUs, 16 MCs, two SMT threads, 16 KiB SPM and 8 KiB I-cache per PU |
| Placement | 288 input PUs, 224 E/I PUs, network-balanced MC zones |
| Input partition | All inputs split into eight 50-column groups; index-cyclic assignment |
| Delivery | Unbundled assembly payloads, tree recurrent mode |
| Measurement | Four warm-up and twelve timed steps per snapshot, 0.5 ms per step |
| Host execution | One simulator at a time, eight workers; rules alternate by snapshot |

All 200 simulator cases returned success, and all 2,400 timed intervals have
complete logs. The summary generator independently reconstructs the input RNG
sequence and checks every timed input-spike count. The final audit checks
result/log agreement, recomputed aggregates, frozen input hashes, and preservation
of all 114 completed result files saved before the host interruption. The
interrupted one-trace snapshot 057 was rerun; its incomplete log is retained
under `restarts/20260908T100841988193Z/`. Completed cases are counted once.

Final checksums are recorded for every case. These native corpora have no paired
pre-change payload run or full GeNN state differential; simulator success and
checksum logging do not by themselves establish that stronger numerical claim.
The earlier same-rule pilot and directed old/new checks remain the functional
regression evidence for the payload optimization.

| Interval group | One-trace steps | One-trace mean cycles | Two-trace steps | Two-trace mean cycles |
| --- | ---: | ---: | ---: | ---: |
| Complete 70/30 mix | 1,200 | **2,649.2558** | 1,200 | **6,691.5375** |
| Presentation | 840 | 2,870.7417 | 840 | 8,640.8036 |
| Rest | 360 | 2,132.4556 | 360 | 2,143.2500 |
| Strictly hot | 601 | 2,263.5291 | 560 | 3,606.2911 |
| Input active | 808 | 2,893.9505 | 798 | 8,982.5890 |
| Excitatory active | 16 | 7,497.1875 | 14 | 11,364.6429 |

The headline means include every timed step after the four-step warm-up. The
warm-up does not exercise every rare handler on every PU. Both strictly hot
subsets have exactly zero unicast memory requests, instruction fills, and
I-cache stall cycles, but neither contains an excitatory firing. They therefore
do not measure the hot firing-postsynaptic path, and their different sample
composition must not be used as a paired speedup comparison.

| Complete-corpus statistic | One trace | Two trace |
| --- | ---: | ---: |
| Median cycles | 2,602.00 | 7,718.50 |
| p95 cycles | 3,135.25 | 12,766.20 |
| p99 cycles | 7,246.49 | 14,047.13 |
| Maximum cycles | 7,857 | 16,808 |
| Mean excluding each snapshot's terminal step | 2,668.8909 | 6,736.9673 |
| Input spikes | 2,926 | 2,847 |
| Excitatory spikes | 16 | 14 |
| Inhibitory spikes | 61 | 73 |
| Feedforward messages | 655,424 | 637,728 |
| Reverse trace messages | 576 | 1,088,055 |
| Instruction fills | 41,172 | 128,759 |
| Unicast memory requests | 82,344 | 257,518 |

Excluding terminal steps leaves 1,100 intervals per rule; those terminal steps
omit next-step input preparation. For every timed interval in both corpora,
unicast requests equal exactly twice the instruction-fill count. No additional
unicast memory traffic is observed. The sums of I-cache stalls are aggregate
core-cycles, not elapsed time on the system's critical path.

One-trace has no presynaptic-only depression update and sends reverse traces
only when excitatory neurons fire. Two-trace also updates weights following
presynaptic firing, with weight-dependent fifth-root arithmetic. The measured
input activity is similar, but the reverse work differs substantially: 576
versus 1,088,055 trace messages. The similar resting costs and much larger
two-trace presentation cost are consistent with that extra work. These
measurements do not isolate FPU latency from receiver scheduling and network
backpressure, so they are not a new proof of a purely computational bottleneck.

Multiple input records on the same input PU fired in 113/808 input-active
one-trace steps and 119/798 input-active two-trace steps. Those steps average
3,155.354 and 12,822.874 cycles, respectively, versus 2,851.449 and 8,309.549 for
the remaining input-active steps. At most three records fired locally in these
corpora. This is an association with extra work, not an isolated estimate of
removable handler overhead; cache state and other activity also vary.

Both mappings are constant within their respective corpora. The presentation
digit counts, in order 0 through 9, are `[8,6,8,9,5,5,11,9,4,5]` for one-trace
and `[6,7,10,7,9,3,13,6,5,4]` for two-trace. All digits are represented, and no
cases were removed from the averages. Useful simulator wall time totals
8,006.13 seconds for one-trace and 8,756.69 seconds for two-trace; these exclude
the outage and the interrupted attempt.

Results and reproducibility records:

- `results/one-trace/summary.json` and `results/two-trace/summary.json`: complete
  grouped timing, activity, input-load reconstruction, and per-case results.
- `verification.json`: final validation and hashes, including restart preservation.
- `report.json`: compact combined report with terminal-step and I-cache accounting.
- `provenance.json`: frozen payloads, simulator, source corpora, and configuration.
- `status.json`: terminal `passed` state with 100 completed snapshots per rule.

## Table-free two-trace fifth root (2026-09-09)

This section records the first implementation. Its full rerun was stopped at
the user's request after 56 sequential snapshots; those results and the pilot
remain archived. The cached-constant implementations below supersede it.

The two-trace helper now seeds a reciprocal fifth root directly from the
positive FP32 bit pattern `I`:

```text
seed_bits = 0x4c2bac89 - high32(I * 0x33333333)
r = reinterpret_float(seed_bits)
```

FINX needs no instruction to reinterpret the register. The integer product
approximates division by five; the float encoding supplies an inexpensive
approximation to the logarithm. The initial relative error is below 3.14%
over the sampled normalized interval. No lookup table or additional SPM
storage is used.

The expanded Newton expression is `r = (6/5)*r - (x/5)*r^6`; the second term
is subtracted, not added. The selected implementation uses one slightly biased
refinement, `r = 1.20148*r - (x/5)*r^6`, followed by a quadratic correction.
The bias balances the first refinement's error so the final correction can be
shorter. `x/5` is prepared once, and the refinement uses five multiplies and
one subtraction. Specifically, it computes `r2 = r*r`, `r4 = r2*r2`, then
`((x/5)*r2)*r4`; forming `r6` first could overflow on tiny inputs.

The final result is:

```text
q = x*r^4
d = 1 - q*r
root = q + q*d*(0.800028 + 0.72*d)
```

In exact arithmetic the desired result is `q*(1-d)^(-4/5)`. The slightly
adjusted linear coefficient compensates for the omitted cubic term over the
small residual range. This avoids amplifying the rounded reciprocal estimate's
error by simply returning `x*r4`. Inputs below `2^-123` are multiplied by
`2^100` before iteration so
`x/5` remains normal, including for the smallest positive subnormal. An integer
exponent subtraction rescales the answer by `2^-20` exactly; every positive
finite FP32 input has a normal fifth root. Nonpositive arguments retain the
previous zero result. All arithmetic remains separate FP32 operations, with
no division, FMA, stack, or shared mutable state.

The normal path has 38 instructions and 17 FP operations, versus 66–71
instructions and 18–19 FP operations in the old degree-seven polynomial.
The reduction comes from removing polynomial coefficient construction and
exponent/remainder dispatch. Both receive SMT threads remain enabled. Only
the two-trace build selects the new helper; reconstructed one- and three-trace
binaries are byte-identical to the frozen baseline.

Two alternatives were also tested: two ordinary expanded Newton steps plus a
linear root correction (40 instructions, 21 FP operations), and one ordinary
step plus a cubic correction (42 instructions, 19 FP operations). The selected
38-instruction version was faster on each presentation pilot. Experimental
switches were removed from the runtime source; the candidates remain archived
under the experiment directory.

All candidates used the same four preselected native snapshots: 010, 000, 008,
and 070, covering a cache-free single-input interval, multiple local firings,
an excitatory spike, and rest. These four cases are diagnostic selection,
not a replacement for the 100-snapshot corpus average. Each mean below includes
all twelve timed steps after four warm-up steps:

| Native snapshot | Original polynomial | Two Newton + linear | One Newton + cubic | Selected biased + quadratic | Selected reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| 010, presentation | 8,445.08 | 6,762.58 | 6,851.00 | 6,662.75 | 21.10% |
| 000, presentation | 6,710.92 | 5,469.25 | 5,541.33 | 5,393.33 | 19.63% |
| 008, presentation, E spike | 9,244.83 | 7,355.83 | 7,502.00 | 7,277.58 | 21.28% |
| 070, rest | 2,143.25 | 2,143.25 | 2,143.25 | 2,143.25 | 0.00% |

The four-snapshot pilot mean fell from 6,636.02 to 5,369.23 cycles. Snapshot
010's seventh step, with one input spike and no E/I spikes, fell from 7,344 to
5,716 cycles; both measurements had zero I-cache fills and unicast memory
requests. All pilot spike counts and message counts match the baseline.
Some final weight checksums change because root rounding changed; directed
tests compare against the updated operation oracle instead of requiring the
old polynomial's bit pattern.

The accuracy target is a maximum four-ULP distance from the correctly rounded
mathematical fifth root, motivated by CUDA's documented
[four-ULP `powf` accuracy](https://docs.nvidia.com/cuda/cuda-programming-guide/05-appendices/mathematical-functions.html).
This preserves the function approximated by the old helper. It does not claim
bit-identical results with CUDA `powf(x, 0.2f)`, whose exponent is itself
rounded, or with the previous polynomial.

Validation:

- 50,832,919 host cases per candidate: all normalized mantissas across five
  exponent residues, every positive subnormal, exponent boundaries, and
  500,000 random finite inputs. Every operation was rounded separately to
  FP32. The two initial candidates had maximum observed distance two ULP, and
  the selected shorter candidate had maximum distance three ULP, from an FP64
  reference rounded to FP32. This is numerical coverage, not a formal
  correct-rounding proof.
- A separate 100-decimal-digit reference checked 1,031 directed/random inputs
  for the selected helper, including its exhaustive sweep's worst case;
  maximum distance three ULP.
- The actual selected assembly passed 1,026 hardware vectors, including zero,
  negative values, extreme positive values, and preservation of caller registers.
- The forced-post system test matched the updated operation oracle's
  `0x04ac8ffb` checksum. A separate test with 781 input spikes, long fired lists,
  and simultaneous posts 0/224 matched `0x0731f06c` with both receive threads.
- All 30 `test_mnist_train.py` tests passed with the selected helper's oracle.
  The changed checksum values above reflect permitted rounding changes.
- The final source, after removing experimental switches, assembles to exactly
  the same bytes as the selected, tested candidate: SHA256
  `00dbb929471faf3aea2dd783a3ab19ae2afa33366d02a6b07de61b6dceca16f3`.

Artifacts are under `copilot/tmp/mnist-fifth-root-20260909/`, including the
numerical sweep, disassemblies, hardware vectors, directed checks, and paired
benchmark controller. The controller uses the original frozen image generator,
native two-trace snapshots, 512-PU soft simulator, and 4+12 timestep windows.
It runs one simulator at a time with eight host workers. The unchanged,
180-byte GCC initializer was reused while assembling the handwritten payload;
reconstructing the entire old binary first matched the frozen binary byte for
byte, so no initializer/toolchain change is mixed into the comparison.

The paired rerun was stopped while snapshot 057 was running, in favor of
the implementations below. Snapshots 000–056 completed, and the earlier rest
pilot 070 is also available: 58 completed snapshots in total. It has no
complete 100-snapshot average. The
interrupted controller's `full-status.json` records that termination, and all
completed results remain available for comparisons on the identical subset.

## Cached constants and LUT comparison (2026-09-09)

Two unbundled two-trace variants use the same completed-square correction:

```text
q = x*r^4
t = q*r
root = q * (base + 0.72 * (t - center)^2)
```

For the LUT seed, `center=14/9` and `base=7/9`; this is algebraically the
previous `q*(1 + .8*(1-t) + .72*(1-t)^2)`. The bit-seed version retains its
single biased Newton refinement and uses `center=1.555575` and
`base=0.7777622219500002` to preserve the previous adjusted polynomial.
Completing the square removes one FP operation. Constants are rounded once
to FP32; every arithmetic operation remains separately rounded, with no FMA.

The seven main bit-seed constants live in `sp`, `s11`, `a4-a7`, and `t0` on
each receiving SMT thread. The LUT variant needs three polynomial constants
and its SPM base. The initializer remains the last stacked function; both
stack regions stay reserved, while the two-trace runtime reuses the `sp`
register as a floating-point constant. Message arguments are first copied to
the caller's saved registers, freeing `a0-a3` for root scratch alongside
`t1-t3`. Weight pointers, the original weight, effective pre-trace, caller
return addresses, and the existing tree/completion registers remain live.

Thread 1 initializes its constants at boot and restores them after its input
preparation/decay worker. Thread 0 restores them before contributing its
input-decay completion to the global tree. Phase-start, reduction, dispatch,
and frame handlers preserve the cached registers. This matters because a
reverse event can arrive before the local weight-phase handler; there is no
new barrier or per-message constant reload. Other STDP rules retain their
original register allocation and produce byte-identical payloads.

| Implementation | Normal helper instructions | FP operations | Payload bytes | Maximum observed ULP |
| --- | ---: | ---: | ---: | ---: |
| Previous bit seed, uncached residual correction | 38 | 17 | 6,772 | 3 |
| Cached bit seed, Newton, completed square | 24 | 16 | 6,940 | 4 |
| Cached LUT seed, completed square | 24 | 10 | 6,924 | 3 |

Counts include the normal input guard and return, and exclude the rare tiny
input rescaling branch and phase-level constant initialization. The LUT trades
FP work for integer indexing and two SPM loads; instruction counts alone do
not determine its cycle advantage.

Both helpers begin on a 64-byte I-cache line boundary. The initial LUT pilot
placed its helper 40 bytes into a line, spanning three lines instead of two;
it was faster on fully hot intervals but slower overall, with more first-use
fills. Unreachable alignment padding limits the LUT helper to two lines;
alignment alone changed that pilot's mean by only 2.58 cycles.
The bit-seed binary was already aligned and stays byte-identical. Pre-alignment
pilot results are archived separately and are excluded from the final run.

The LUT contains 256 exponent factors followed by 128 midpoint mantissa
seeds, totaling 1,536 bytes per input PU. Datagen places it at SPM offset
`0x2000`; the resulting data end is `0x2600` (9,728), leaving 2,560 bytes
before the first stack. The bit variant's maximum data end remains 5,672.
The table is shared read-only by both SMT threads, initialized through the
existing SPM image, and causes no runtime DRAM lookup. Datagen rejects a
layout that would overlap the table; neuron PUs do not allocate it.

Select `MNIST_FIFTH_ROOT_SEED=bit` or `lut` when building the payload, and pass
the matching `--fifth-root-seed bit` or `lut` to `datagen/mnist_train.py`.
Both default to `bit`. A LUT payload checks the unity exponent-table entry
at startup to catch a missing table. `MNIST_TRACE_BUNDLE=0` is required for
the cached two-trace receiver, as in all current benchmarks.

Validation includes 50,832,917 inputs per variant, covering all normalized
mantissas over five exponent residues, every positive subnormal, exponent
boundaries, and 500,000 random finite values. Maximum observed distances
from an FP64 root rounded to FP32 are four ULP (bit) and three ULP (LUT).
A separate 100-decimal-digit reference confirmed the same maxima on 1,032
directed/random inputs. These are numerical checks, not formal rounding
proofs or claims of bit identity with CUDA `powf`.

Each actual helper passed 1,797 hardware vectors, including constant and
caller-register preservation. Directed network checks exercise simultaneous
pre/post firing and a 781-input long fired vector with both receiving SMT
threads. The updated operation oracle supplies each variant's checksum.
All 32 generator/oracle tests passed, including LUT overlap rejection and
byte-preservation checks for neuron data and both stack regions.

Artifacts and progress are under `copilot/tmp/mnist-root-cached-20260909/`.
The paired full comparison uses all 100 original native two-trace snapshots,
four warm-up steps and twelve timed 0.5-ms steps per snapshot, the frozen
512-PU/16-MC soft simulator, 288 input PUs, 224 neuron PUs, full-eight input
partitioning, unbundled traces, and tree recurrence. The preflight uses one
simulator with eight active host workers. The full run uses at most two such
processes on the verified 32-CPU host, leaving half the CPUs available. It
first repeats one pilot per variant concurrently and requires exact equality
with the serial runs' cycle counts, interval statistics, and checksums.
The complete original polynomial run is the primary baseline; comparisons
against the interrupted intermediate version use only identical snapshots.
Both variants completed all 100 snapshots successfully. Each has 1,200 timed
steps: 840 presentation steps from 70 snapshots and 360 rest steps from 30
snapshots. A step is a 0.5-ms network timestep, not a complete MNIST image
presentation. These are means over every timed step after the four-step
warm-up; no cases or slow intervals are excluded.

| Two-trace implementation | Overall mean cycles | Presentation mean | Rest mean | Overall cycle reduction vs original |
| --- | ---: | ---: | ---: | ---: |
| Original polynomial helper | 6,691.54 | 8,640.80 | 2,143.25 | — |
| Cached bit seed + Newton + completed square | **4,726.74** | **5,828.46** | 2,156.08 | **29.36%** |
| Cached LUT seed + completed square | 4,782.02 | 5,910.18 | 2,149.67 | 28.54% |

The cached bit version gives 1.416 times the original throughput and reduces
presentation cycles by 32.55%. Its overall p95/p99 are 8,322.05/9,028.29 cycles,
down from 12,766.20/14,047.13; the maximum falls from 16,808 to 10,705.
The LUT's overall p95/p99 are 8,227.05/9,114.10, with a maximum of 10,594.
The bit version remains the default because it has the lower overall mean
and needs no extra table storage. The LUT remains a compile-time option.

Rest steps do not perform weight-root arithmetic, so they receive no benefit
from the shorter helper. Phase-level constant initialization adds 12.83
cycles to the bit version's rest mean (0.60%) and 6.42 cycles to the LUT's
(0.30%). These costs are included in the overall means above.

The completed intermediate run provides an additional, narrower comparison:
on its identical 58 snapshots (57 presentation, one rest; 696 timed steps),
the uncached bit-seed helper averages 6,665.54 cycles, the cached bit helper
5,746.53, and the LUT 5,826.03. Caching constants and folding the correction
therefore provide an additional **13.79%** reduction for the bit version;
the LUT improves this subset by 12.59%. These subset means must not be
compared directly with the full 70/30 presentation/rest means.

#### Why the LUT does not win the full run

Both helpers execute 24 instructions on the normal path. The LUT removes
six FP operations but replaces them with indexing and two SPM loads. On
the same 158 input-active steps with zero I-cache fills, I-cache stalls,
and unicast memory requests in **all three** implementations, the means are:

| Implementation | Mean cycles on the identical 158 hot, input-active steps |
| --- | ---: |
| Original polynomial | 7,327.75 |
| Cached bit seed | 5,093.69 |
| Cached LUT seed | 5,080.83 |

The LUT saves only 12.86 cycles (0.25%) over the cached bit version in that
comparison. This is a useful steady-state arithmetic comparison but does
not replace the full benchmark. There are 560 common strictly hot steps
when inactive presentation and rest steps are also included; their means
are 3,606.29, 2,985.19, and 2,976.95 respectively. None of the 14 timed steps
with excitatory firing is strictly hot in this short continuation.

The remaining 640 steps touch instruction paths that were not warmed on
every PU during the four-step warm-up. The full-run counters are:

| Implementation | I-cache fills | Unicast memory requests | Summed I-cache stall core-cycles |
| --- | ---: | ---: | ---: |
| Original polynomial | 128,759 | 257,518 | 10,837,325 |
| Cached bit seed | 68,583 | 137,166 | 5,688,317 |
| Cached LUT seed | 84,964 | 169,928 | 6,955,761 |

The LUT has 16,381 more fills and 1,267,444 more summed stall core-cycles
than the cached bit version, despite the helper alignment. Together with
its very small advantage on the identical hot steps, this supports code
placement and first-use instruction fetches as the reason it loses the full
mean by 55.28 cycles (1.17%). Summed stall core-cycles overlap across PUs
and are not a direct additive decomposition of elapsed step cycles. This
comparison does not establish a new NoC congestion bottleneck. Both helpers
and the complete payload still fit in the unchanged 8-KiB I-cache, and all
560 common strictly hot intervals have exactly zero unicast memory requests.

The final audit independently rechecked all 200 successful system results,
all 1,200 per-step activity comparisons for each variant, and 211 recorded
file hashes. Both variants preserve the baseline's 2,847 input spikes,
14 excitatory spikes, 73 inhibitory spikes, 637,728 feedforward messages,
and 1,088,055 trace messages. This is activity equivalence over these
continuations, not bit identity of weights: the changed FP32 root operation
order can change weight checksums, which the directed tests validate against
each variant's operation oracle.

The concurrent reproduction checks passed exactly for cycles, interval
statistics, and checksums. The paired run finished without interruption in
about 179 minutes, using at most two simulators with eight active host workers
each. The final summaries are `results/{bit,lut}/full-summary.json`, paired
baseline comparisons are `results/{bit,lut}/full-paired.json`, the intermediate
comparisons are `results/{bit,lut}/full-vs-interrupted.json`, and the direct
variant comparison is `full-lut-vs-bit.json`, all under the experiment
directory above. `final-audit.json` records the independent checks and the
common hot-step comparisons.

### Remaining optimization opportunities

The current priorities exclude additional placement sweeps, firing-rate-based
assignment, and fixed-local-neuron-count handlers.

1. Profile instruction issue, SPM stalls, and caller overhead in the current
   receive handlers. Root constants are now cached and both normal helpers
   are 24 instructions; removing FP operations alone gave the LUT only a
   0.25% advantage on identical hot, input-active steps. Any further inlining
   or scheduling change should also measure instruction-fetch behavior on
   the full paired corpus. Cross-rule averages do not measure an isolated
   optimization benefit.
2. Profile longer diagnostic continuations after the multi-input and firing-post
   instruction paths have been touched. The four-step warm-up does not make
   every PU's rare paths hot, so the current tail statistics do not isolate
   steady-state arithmetic from first-use fills.
3. Cache immutable per-thread input-record range descriptors for preparation
   and decay. The handlers currently reconstruct these bounds from the local
   count each step. Generated descriptors can support any legal count without
   an atomic work allocator or fixed-count specialization; expected savings are
   smaller than optimizing the per-weight loop.
4. Streamline the general firing-post row loop by inlining its per-weight helper
   and hoisting invariant setup, preserving FP32 operation order. Excitatory
   firings occur in 20/1,200 older three-trace intervals, 16/1,200 native
   one-trace intervals, and 14/1,200 native two-trace intervals, so this primarily
   targets the slower cases rather than a large fraction of the overall mean.
5. Investigate remaining tree/dispatch overhead only with phase-specific timing.
   A local yielding send already supports immediate handoff when applicable, so
   replacing it with a direct jump is not automatically a meaningful saving.
   Any such change must preserve thread ownership, quota guarantees, and the
   global gate before next-step spike delivery.
