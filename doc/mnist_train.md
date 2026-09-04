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
- `--horizontal-partition none|hot-partial|full|full-4` for retaining whole
  rows, splitting the 368 highest-score rows, splitting every row two ways, or
  splitting every row four ways into compact 100-column owners.

`M` must be a multiple of 32, and every neuron PU owns at most two matched
pairs. Every selected group of 32 consecutive PU IDs is therefore a complete
MC broadcast range. This preserves the DRAM-scatter comparison without sending
recurrent rows to MCs that own no excitatory target. PU IDs follow the system
Hilbert mapping; they are not row-major mesh indices. The generator also checks
the exact per-PU data end against the first stack, so the usable upper bound on
`M` depends on `N` rather than an obsolete fixed limit of 256.

The measured default uses `N=196`, `M=224`, `placement-policy=near`, the
`network-balanced` neuron-zone policy, and rate-balanced input assignment. Each
input PU owns exactly four inputs. Each neuron PU owns one or two matched E/I
pairs. The `network-balanced` zone policy retains whole MC ranges but spatially
interleaves the seven neuron zones to reduce deterministic-XY trace traffic.

Rate-balanced assignment sorts inputs by their generated Poisson threshold and
greedily equalizes the threshold sum subject to the same four-input capacity on
every input PU. It changes neither global input identity nor the weight matrix;
frame scatter records carry the resulting local index.

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
`full` control retains a 400-slot row for each 200-column half and zero-fills
the unowned half. The `full-4` layout instead stores four physical 100-slot
quarters, so it has no padding and the four copies together occupy exactly the
same weight bytes as one unpartitioned row. An input PU also stores one
4-byte descriptor per neuron PU. Its low 16 bits are the destination PU and its
upper 16 bits identify the receiver's first and optional second local neuron.
Descriptors consume the next one or two values from the compact row. Each
excitatory record stores its reverse-path slot, and a neuron PU stores the
16-bit destination PU IDs for all input PUs.

For the `N=196`, `M=224` layout, the fullest input PU uses:

```text
header                       356 bytes
4 input records              112 bytes
4 * 400 FP32 weights       6,400 bytes
224 target descriptors
  * 4 bytes                   896 bytes
4 direct fired-row pointers   16 bytes
total                       7,780 bytes
```

The first SMT stack begins at byte `0x3000` (12,288), leaving 4,508 bytes
between the fullest baseline layout and stack 0. Thread 0's 2 KiB stack occupies
`0x3000-0x37ff`. Thread 1 has a reserved downward-growing range at
`0x3800-0x3fff`, but no thread-1 handler or initializer uses the stack. The
generator checks every PU and rejects a layout that reaches byte `0x3000`.

The tested `N=160`, `M=352` layout places five inputs on its fullest input PU
and remains below byte `0x3000`. It is functionally
supported, although its collision-frame performance is poor.

A two-neuron baseline PU uses at most 1,572 bytes. That includes its 392-byte full input
destination table, a 512-bit observed-source mask, and a 392-byte compact
observed-source vector, leaving more than 8.5 KiB before stack 0.

Aggregate logical and physical weight storage is:

```text
784 * 400 * 4-byte logical weight = 1,254,400 bytes
784 * 400 * 4-byte physical weight = 1,254,400 bytes
512 * 16 KiB                         = 8,388,608 bytes
```

In `full-4`, the 288 input PUs form four 72-PU groups and every input has one
copy in each group. Each copy owns 100 columns and stores a 100-value row. The
224 neuron PUs form four 56-PU groups with 100 matched E/I pairs each. Input PUs
hold ten or eleven records; the fullest data image ends at byte 5,332, leaving
6,956 bytes before stack 0. Aggregate physical weight storage remains exactly
1,254,400 bytes.

Even an 8-byte weight-plus-column representation would require only 2,508,800
bytes in aggregate, but aggregate capacity is not sufficient evidence by
itself. The per-PU check above is the binding safety check.

DRAM contains the executable and one-time SPM initialization image. At runtime
it supplies frame scatter records. In `scatter` recurrent mode it also stores
fixed I-to-E CSR rows. No trainable weight is fetched from or written to DRAM.

## Step protocol

The workload uses phase-scoped tree synchronization and no per-spike or
per-trace acknowledgements:

1. Scatter the frame thresholds and wait for the MCs to become idle.
2. Every input PU advances its RNGs, records direct pointers to fired weight
   rows in a flat local vector, and contributes its logical fired count to a
   512-PU tree sum. With replicated input state, only the primary copy
   contributes to this sum; every physical copy advances the same local RNG
   once.
3. The summed count starts feedforward delivery. Each fired input streams 224
   descriptors and corresponding one- or two-weight groups from its local row,
   sending one physical packet to each owned neuron PU. An unpartitioned
   row reaches all 224 PUs; a horizontal half reaches one 112-PU group; a
   four-way quarter reaches one 56-PU group. The packet carries one
   or two old FP32 weights for that PU's local excitatory neurons. A neuron PU
   completes after receiving exactly `global_fired_inputs` packets and
   accumulates its one or two conductances in registers. The first row from an
   active input PU also carries its physical source ID. Each neuron PU uses
   `AMOOR` on a 512-bit mask to deduplicate that ID and `AMOADD` to reserve a
   slot in its compact source vector.
4. Excitatory and inhibitory neurons update. The E-to-I weight is fixed at
   10.4 and is added locally after the inhibitory update, preserving the
   reference model's one-step recurrent delay.
5. I-to-E inhibition is produced by the selected recurrent mode, then the
   weight phase starts.
6. In unbundled mode, a nonfiring excitatory neuron sends its reverse message
   only to the physical input PUs observed during feedforward delivery. A firing
   excitatory neuron sends to every owner of its 200- or 100-column group. The message
   contains its global weight byte offset, precomputed depression delta, fired
   bit, and the precomputed slow-post potentiation factor needed by a firing
   three-trace neuron. Active half owners count exactly 200 traces, active
   quarter owners count 100, and active full owners count 400; inactive owners
   use the phase reduction's group-specific firing counts. Two 32-bit reduction
   words hold the inhibitory count and up to four excitatory-group counts. An
   optional compile-time mode
   packs two colocated traces into one physical message and retains universal
   reverse delivery for comparison.
7. Input PUs decay their pre traces before the neuron-update reduction
   completes, which makes that reduction the barrier before reverse messages.
   After all required synapses are updated, firing pre and post traces consume
   the current spike, and each PU contributes one phase-completion tree message.
8. Scatter mode also waits for recurrent DRAM traffic to drain. The root then
   starts the next step or XOR-reduces the final weight checksum. Replicated
   `(global input, RNG, threshold, pre-trace)` fingerprints cancel pairwise in
   that checksum; replica divergence therefore changes the reported value.

All feedforward deliveries therefore use old weights, and plastic writes cannot
begin until every feedforward spike has been delivered. Decayed old traces are
ready before the reverse path opens, and all plastic writes finish before any
current spike is committed into a trace for the next step.

For an unpartitioned layout with `A` active input PUs, `P` firing posts, and `N`
input PUs, the default unbundled two-/three-trace path sends
`(400 - P) * A + P * N` physical messages. For a horizontal layout, group `g`
sends `(C_g - P_g) * A_g + P_g * D_g`, where `C_g` is its column count, `A_g`
is the number of observed physical owners, and `D_g` is the universal
destination count for a firing post. Two-way full partitioning has `C_g=200`
and `D_g=144`; four-way full partitioning has `C_g=100` and `D_g=72`; the
partial layout has `C_g=200` and `D_g=196`. A zero-spike frame sends none. One-trace sends
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
two conductance accumulators in registers. Reverse sending also runs on thread
0. Reverse receipt uses both SMT threads for zero-, one-, and multiple-local-fire
handlers. Each thread has private working registers, the fired vector is read-only
during the reverse phase, distinct trace messages address distinct columns, and
the shared completion count uses an AMO. Feedforward source discovery also uses
an atomic bitmask and vector allocator even though the current feedforward
receiver is pinned to thread 0. An earlier N=160 run appeared to show
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
reduction. Single-pair and non-neuron PUs stay on thread 0.

One plasticity rule, one recurrent mode, and bundled or unbundled reverse
messages are selected with compile-time definitions. Startup rejects a rule or
recurrent-mode mismatch; bundling uses the same SPM layout, and the generator's
`--trace-bundle` flag records the matching traffic count in the manifest. FP32
constants are materialized as integer immediates. The nine unbundled rule/mode
binaries contain no FLW, FSW, FCVT, floating divide, or floating square-root
instruction.

Current linked `.text` sizes in bytes are:

| Rule | Scatter | Tree | Tree aggregate |
| --- | ---: | ---: | ---: |
| One trace | 6,312 | 5,976 | 5,984 |
| Two trace | 6,756 | 6,412 | 6,420 |
| Three trace | 6,396 | 6,048 | 6,056 |

All fit the 8,192-byte I-cache. Because `.text` starts 16 bytes into a cache
line, the largest binary spans 106 of the cache's 128 64-byte lines. The
contiguous text therefore fits the 64-set, two-way organization without
capacity or conflict eviction. After a PU executes a fixed dynamic path, stack
and weight data remain in SPM; later unicast requests are compulsory
instruction fills for stochastic paths that PU has not executed before.

Static instruction inspection, excluding handler scheduling and NoC
backpressure, gives these hot-path costs:

- reverse sender continuation: 6 cycles;
- feedforward sender: 9 retired instructions for a two-weight destination and
  8 for a one-weight destination, including the send and loop branch;
- feedforward receiver: about 22 cycles for one local target or 28 for two;
- inactive reverse receiver: one full post update followed by its completion
  AMO; nonfiring posts are not delivered there;
- unbundled three-trace depression with exactly one local fired input: 13
  retired instructions on the common path, including completion handling.

Firing-post scans and the two-trace fifth-root polynomial are intentionally
longer because they perform the required per-synapse arithmetic.

## Validation

The independent FP32 host model and system simulator agree on these one-step
weight checksums:

| Case | Rule | Checksum |
| --- | --- | ---: |
| Natural deterministic input | One trace | `0x032674ca` |
| Natural deterministic input | Two trace | `0x02eb6e72` |
| Natural deterministic input | Three trace | `0x0239bec0` |
| Forced same-step post column 1 | One trace | `0x0561f569` |
| Forced same-step post column 1 | Two trace | `0x04ac8ffc` |
| Forced same-step post column 1 | Three trace | `0x0480e684` |
| Forced same-PU post columns 2 and 3 | One trace | `0x049c8764` |
| Forced same-PU post columns 2 and 3 | Two trace | `0x05506233` |
| Forced same-PU post columns 2 and 3 | Three trace | `0x0409fc8f` |
| Forced post columns 0 and 224 | One trace | `0x0400db44` |
| Forced post columns 0 and 224 | Two trace | `0x05cbe328` |
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

Measurements use the cycle-aligned soft system simulator, flat memory, 512 PUs,
16 MCs, the deterministic synthetic frame at intensity 2.0, and eight host
worker threads per simulator process. Host parallelism changes wall time, not
modeled cycles.

The three-trace tree progression for the specialized, unbundled default is:

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
     MNIST_TRACE_BUNDLE=0"

PYTHONDONTWRITEBYTECODE=1 python3 datagen/mnist_train.py \
  --rule three-trace --recurrent-mode tree \
  --input-pus 196 --neuron-pus 224 \
  --placement-policy near --neuron-zone-policy network-balanced \
  --input-assignment rate-balanced \
  --horizontal-partition full-4 \
  --trace-destination-stride 83 \
  --warmup-steps 4 --timed-steps 4 \
  --output copilot/tmp/mnist-train
```

Use `--frame` and `--frame-index` for an IDX MNIST image. The manifest
records the role PU lists, complete neuron MC zones, per-PU storage, estimated
hop counts, warm/timed step counts, and initial weight checksum.

## Runtime checkpoint corpus

`mnist_genn_snapshots.py` resumes the dense FP32 GeNN triplet network from a
portable checkpoint, discovers the actual retry timeline, selects independent
presentation and rest points, and replays the same timeline to export complete
runtime snapshots. The two passes must use a nonzero deterministic GeNN model
seed. The exporter checks both the seed compiled into `librunner.so` and the
final FP32 weights/theta digest before accepting the corpus.

The portable checkpoint contains weights and theta but no neuron or RNG state.
Consequently, this is a deterministic continuation initialized from the 30k
learned parameters, not restoration of the hidden state from the historical
training process. Every exported snapshot includes current E/I membrane and
conductance state, refractory counters, reconstructed pre/post traces, pending
GeNN synaptic input folded into conductance, frame/intensity metadata, and a
deterministic nonzero ActiveN xorshift state for each Poisson neuron.

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

### Remaining optimization opportunities

1. Split the ten/eleven-record four-way input-prepare loop across both SMT
   threads with a local join. Four-way doubles the physical records relative to
   two-way and raises every no-input step by about 874 cycles. Erasing that
   penalty would remove about 305 cycles from the complete 70/30 mean. Reverse
   arithmetic and two-pair neuron updates already use both threads.
2. Decouple Poisson ownership from weight ownership. A primary PU can advance
   the RNG and sparsely notify three weight-only owners when an input fires.
   Preserving exact pre-trace values for rare postsynaptic firing requires
   either lightweight replica decay or timestamped lazy decay, so this is a
   larger change than dual-SMT preparation but can remove duplicate RNG work
   entirely without a rate prior.
3. Sweep fixed, rate-independent input permutations. Four-way has 38 collision
   steps; they average 14,170.03 cycles versus 9,065.90 for noncolliding active
   steps. Removing every measured collision would bound the opportunity at
   roughly 162 cycles per overall frame, without online rebalancing.
4. Add phase-scoped cycle counters for input preparation, feedforward, neuron
   update, reverse STDP, and final reduction. The end-to-end and load metrics
   identify the likely critical rows but do not directly assign every cycle to
   a phase.
5. Revisit the horizontal group count after reducing input preparation cost.
   More groups shorten each plastic row and all-post fanout but add replicas;
   the current rest result shows why an eight-way layout should not be judged
   before the replica overhead is addressed.
6. Consider a compile-time four-way specialization after the layout settles.
   The generic binary retains one-, two-, and four-group setup branches for
   validation flexibility; specializing them is lower priority than the
   measured input-prepare and collision costs.
