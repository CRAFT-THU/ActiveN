# Sparse MNIST trace-STDP

The sparse payload implements four interchangeable reverse mappings for
the one-, two-, and three-trace rules. It shares the dense payload's FP32 neuron
integration and fifth-root helper. Trainable weights reside in compact forward
CSR rows in input-PU scratchpads; structural zero-weight synapses remain present.

The implementation and experiment files are:

- `datagen/mnist_sparse.py`: forward/reverse CSR construction and DRAM images.
- `sim/payloads/sys/mnist_sparse.inc`: sparse runtime selected by
  `MNIST_SPARSE_MODE` in `mnist_train.S`.
- `datagen/mnist_genn_snapshots.py`: rule-native sparse training and snapshots.
- `datagen/mnist_sparse_benchmark.py`: paired delivery-mode benchmark driver.
- `datagen/mnist_sparse_state.py`: portable state-dump reader and logical hashes.
- `datagen/mnist_sparse_reference.py`: independent FP32 weight/trace replay.
- `copilot/tmp/mnist-sparse-20260910/`: pilot logs, reference sources and runs,
  experimental payloads, commands, and comparison artifacts.
- `copilot/tmp/mnist-sparse-genn-fix-20260910/`: recorded-profile alignment,
  regenerated snapshots, renewed correctness checks, and prepared restart.
- `copilot/tmp/mnist-fanin-20260910/`: realized-fan-in caps, new correctness
  checks, and preserved strict-bound reference attempts.
- `copilot/tmp/mnist-fanin-no-bound-check-20260910/`: fresh full training with
  the diagnostic normalization bound assertion disabled and automatic benchmark handoff.

## Caps from realized fan-in

The current sparse model uses `wmax[j] = float32(784 / fan_in[j])` for every
synapse into postsynaptic neuron `j`. The degree counts structural connections
across all input PUs, including present connections whose weight is zero.
Normalization still scales each incoming column to 78. Learning-rate scaling
continues to use the nominal connection rate.
For each nonempty column, the cap is therefore `784 / 78` (about 10.05) times
its normalized mean incoming weight, independently of realized degree.

| Rate | Realized fan-in range | Actual cap range |
| --- | ---: | ---: |
| 50% | 353–431 | 1.8190–2.2210 |
| 25% | 160–224 | 3.5000–4.9000 |
| 12.5% | 74–123 | 6.3740–10.5946 |

The GeNN backend accepts an optional `weight_max_by_post` array, represented as
a read-only postsynaptic variable in each of the three existing learning
kernels. Its normalization guard compares each column against its own cap
with the existing 2% tolerance. This cap change does not make column
normalization preserve individual upper bounds.

For the current full-training experiment, the exporter is launched with
`--skip-normalization-bound-check`. This disables only the diagnostic
post-normalization upper-bound assertion. Incoming columns still normalize to
78; zero/non-finite column sums are still rejected, and the existing STDP
clamps still use the per-post cap. The default remains to check the bound.
Manifests, snapshots, checkpoints, selection records and completion records
store `normalization_bound_check` so the run policy is explicit.
The strict-bound one-trace runs stopped after last reporting 9,000, 12,000 and
14,000 accepted examples at 50%, 25% and 12.5% connectivity, respectively.
Their artifacts are preserved; the replacement runs start at zero because
weights/theta checkpoints do not preserve the complete runtime/RNG state.

The sparse exporter defaults to `--weight-max-policy fan-in`; dense runs retain
their existing scalar cap and checkpoint compatibility. The explicit
`connection-rate` policy retains historical scalar-cap reference generation,
including `--baseline-inputs`. Sparse MSP4 images require fan-in snapshots.
Checkpoint and build metadata include the policy and SHA-256 of the FP32 cap
array; snapshots additionally store that array. Old scalar-cap checkpoints
cannot resume fan-in training. The scalar `variant.weight_max` field remains
the nominal legacy value; the explicit per-post array is authoritative under
the fan-in policy.

Each input PU stores a static cap table: 50 floats (200 bytes) in local/PU
scatter mode, or 56 descriptor-indexed floats (224 bytes) in neuron scatter
mode. The latter reads the existing descriptor immediately before the forward
weight. No extra carried argument or reverse edge list is needed. Empty
structural columns have a finite zero cap for directed routing tests; training
normalization rejects them because they cannot have incoming sum 78.

Fresh references use the original random masks and initial weights, seed
19260817, FP32, presynaptic sparse scheduling with 32 threads per spike, and
30,000 accepted examples. Each completed rule/rate corpus supplies 100 windows
(70 presentation, 30 rest) to all three modes, with four warm-up and twelve
timed steps. `copilot/tmp/mnist-fanin-no-bound-check-20260910/full-status.json` records progress;
incomplete reference corpora are never submitted to the benchmark.

Validation of this cap change passed 71 Python checks, nine 100-example GPU
pilots, and GPU saturation checks for all three rules over all 39,001 synapses
at 12.5%. Nine directed native tests and 54 sampled presentation/rest simulations
passed independent FP32 weight/trace checks. All paired modes also produced
identical complete logical states. Detailed evidence and pilot timings are in
`copilot/tmp/mnist-fanin-20260910/validation.md`; the long-run report is
`copilot/tmp/mnist-fanin-no-bound-check-20260910/summary.md`.

## Reverse message data

Two carried scalar values suffice: `d_minus` and `d_plus`. The current post's
firing status is represented by the handler tag. CSR supplies the other two
handler arguments from its 16-bit index and 32-bit data fields.

| Mode | CSR location and entry | Receiver arguments `a0`, `a1`, `a2`, `a3` |
| --- | --- | --- |
| 1: `local` | Local reverse row indexed by post; entries identify input state and forward weight | unused, post slot, `d_minus`, `d_plus` |
| 2: `pu-scatter` | Global entry per connected input PU; local reverse row retained | unused index, post slot, `d_minus`, `d_plus` |
| 3: `neuron-scatter` | Global entry per structural synapse; no local reverse edge list | input-state SPM offset, forward-weight SPM offset, `d_minus`, `d_plus` |
| 4: `dense` | Local dense reverse row indexed by post and local input; synapse offset plus skip | unused, post slot, `d_minus`, `d_plus` |

Offsets are relative to the destination PU's SPM base. Thus mode 3 identifies
both the presynaptic trace and the exact forward synapse without a reverse lookup.
Each 8-byte global CSR entry is `(u16 destination PU, u16 index, u32 data)`.
Mode 2 leaves the index zero and uses data for the post's slot within its group.

One-trace uses the firing-post handler only. Its power-rule update uses the local
pre-trace and weight. Two-trace needs `d_minus`; three-trace uses both coefficients.
The same four-argument receiver ABI serves all three rules.

## Placement and static MC selection

The maintained configuration is 512 PUs, 16 MCs, 32-byte memory lines, two SMT
pipes, 16 KiB SPM, and an 8 KiB instruction cache. Input PUs occupy nine complete
MC zones; neuron PUs occupy seven. The existing eight horizontal groups each
contain 36 input PUs and 28 neuron PUs and own 50 postsynaptic neurons.

Every logical input has one state copy in each horizontal group. Its outgoing
weights are split by the group's postsynaptic neuron ownership. Input records
retain the dense index-cyclic placement, independently of realized synapse counts.
No balancing or reassignment by sparse row length occurs.

The generator partitions each global reverse row by destination MC and emits a
descriptor only for a nonempty partition. The runtime iterates these descriptors;
it issues no scatter to an MC with no structural destination. This restriction
is known ahead of time and requires no runtime connectivity search.

For independent Bernoulli masks generated from seed 20260723:

| Nominal connection rate | Structural synapses | Mean connected input PUs per post | Connected input MCs per post |
| --- | ---: | ---: | ---: |
| 50% | 156,873 | 36.00 | 9 |
| 25% | 77,989 | 35.92 | 9 |
| 12.5% | 39,001 | 34.08 | 9 |

All 400 posts reach all nine input MCs at these rates. Static selection skips
the seven neuron-only MCs but cannot skip additional input MCs in these random
graphs. The generator also supports rows reaching fewer MCs, including empty
rows; the directed tests exercise this separately from the random benchmark.

For nonfiring posts in two-/three-trace mode, only input spikes can cause a
weight update. Mode 1 sends to active input PUs within the post's horizontal
group. Modes 2 and 3 skip an MC unless one of that group's input PUs in that MC
is active. A selected scatter still reaches every structural entry in its MC
partition. Receivers skip irrelevant updates while counting the delivery for
completion. This dynamic MC selection does not imply per-entry dynamic pruning.

## Storage

Forward rows contain 8-byte records: destination PU, compact destination neuron
descriptor, FP32 weight. Local reverse rows use 51 halfword pointers and one
4-byte `(input-state offset, forward-weight offset)` pair per structural edge.
Mode 3 omits these reverse pointers and pairs. All modes retain 50 halfword
degree counts for completion; those counts cannot identify or update a synapse.

Global CSR rows are padded to 32 bytes; padding has destination zero and produces
no delivery. The measured graph storage is:

| Rate | Local reverse bytes, modes 1/2 | Global reverse bytes, mode 2 | Global reverse bytes, mode 3 | Maximum initialized SPM, modes 1/2 | Maximum initialized SPM, mode 3 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 50% | 656,868 | 115,200 | 1,297,920 | 8,608 | 6,152 |
| 25% | 341,332 | 115,200 | 666,720 | 5,258 | 3,918 |
| 12.5% | 185,380 | 115,200 | 355,776 | 3,592 | 2,808 |

Local reverse totals include row pointers. Global totals include row padding and
exclude the small per-neuron SPM descriptor lists. SPM totals include headers,
states, forward/reverse data, descriptors, and control metadata. All fit below
the reserved stack boundary at `0x3000`.

Each PU's startup descriptor copies only its initialized data rounded to a
memory line. It does not copy the unused 16 KiB SPM tail or reserved stacks.
The current sparse SPM magic is `MSP4` (`0x4d535034`); header offset 156 is now
`H_WMAX_BASE`, a pointer to the cap table. Regenerate images after
changing the ABI or selecting a different sparse payload mode.

## Completion and arithmetic

The frozen 2026-09-10 full-benchmark build used three globally separated phases:

1. Advance each input RNG, decay its pre-trace, record spikes, and count outgoing
   edges per destination neuron PU, then deliver its feedforward weights without
   waiting for other input PUs to finish preparation.
   The last packet from a source PU to a destination
   requests an acknowledgment after its accumulation. Global completion waits
   for both all senders and all expected acknowledgments.
2. Integrate local E/I pairs. A binomial tree within each horizontal group
   collects and distributes the post-firing and active-input-PU masks. Global
   completion proves that every receiver has its expected reverse delivery count.
3. Apply recurrent inhibition, decay post traces, send reverse coefficients,
   update weights, and commit trace increments. The global barrier waits for all
   sends and all reverse receiver completions.

`MNIST_SPARSE_FUSE_FORWARD=1` is the default. With compact scheduling disabled,
setting it to zero retains a
separate preparation barrier before feedforward delivery for comparison. Neuron
preparation does not clear accumulated excitatory conductance, so early packets
from an input PU cannot be erased by a slower neuron PU's preparation handler.

That build's feedforward acknowledgment relies on same-source, same-priority FIFO packet
order in the maintained XY network. Reverse updates for different post rows
touch disjoint weights, allowing both SMT receivers to work concurrently. An
atomic count and a one-time completion claim protect the final input-trace commit.

### Compact scheduling and ordered reverse lookup (2026-09-11)

The current default enables `MNIST_SPARSE_COMPACT_SCHEDULE=1` for all rules and
`MNIST_SPARSE_FUSE_NEURON=1` for two-/three-trace. With fusion enabled, after the
first preparation each timestep has two
global phases: feedforward delivery with local neuron integration and group-mask
exchange, followed by reverse delivery with preparation of the next input step.
Input preparation uses both SMT threads. They append to the existing fired
vector atomically; their join insertion-sorts this small vector by input-state
offset. No input is advanced beyond the final requested timestep.
One-trace retains the separate neuron phase by default: its presentation pilots
favored avoiding count-announcement traffic. It still uses parallel, pipelined
input preparation; fusion remains selectable explicitly.

During preparation, a source counts actual outgoing packets and announces a
count only to neuron PUs that will receive data. Each announcement is acknowledged
before the preparation/reverse barrier completes. The next phase can therefore
integrate each destination as soon as its announced number of packets arrives,
without a global feedforward-completion barrier. Early feedforward arrivals are
accumulated before local phase start, but integration waits for local setup.
The receive counter is cleared after integration, never at phase start. Group
accumulators reset on the first new report, so a report arriving before local
phase start is retained. Existing message priorities, quotas and margins remain
unchanged.

The existing input-PU count table packs a destination PU ID in its upper half
and the packet count in its lower half. Sending drains it back to zero, avoiding
unconditional clear/count scans. The optional `MNIST_SPARSE_ACTIVE_PREFIX=1`
partitions the existing mode-1 destination list in place into active and inactive
prefixes once per group-mask update; quiet posts then visit only the active
prefix. It is disabled by default because its group-barrier overhead outweighed
the sender savings in the initial paired pilot. Immutable fifth-root constants
are retained in each input thread's registers.

`MNIST_SPARSE_BINARY_REVERSE=1` searches the sorted reverse CSR row once per
fired input for quiet posts. The sorted fired vector permits advancing the
search's lower bound after each hit or miss. The default value `2` uses the same
binary search with a bounded sequential tail of at most eight entries; `0`
retains the original full-row fired-flag scan. Firing posts traverse the complete
reverse row, and neuron-scatter mode still receives exact synapse offsets.
For a reverse row of degree `d` and `k` fired inputs, lookup uses
`O(k log(d + 1))` work, with a constant-sized tail, and adds no connection index.
Local connection storage remains `O(E + Npre + Npost)`, including existing row pointers
and fired vectors, rather than a directory proportional to `Npre * Npost`.

These changes occupy the previously unused header word at byte 252
(`H_PREPARED`) and add no initialized SPM bytes. Input PUs retain completed-step
activity totals in otherwise unused reverse-sender slots, so timer observations
remain correct while next-step preparation is in progress. The simulator's
observer recognizes the new header field and remains compatible with the frozen
MSP4 images. To reproduce the former scheduling/lookup paths, set
`MNIST_SPARSE_COMPACT_SCHEDULE=0`, `MNIST_SPARSE_FUSE_NEURON=0`, and
`MNIST_SPARSE_BINARY_REVERSE=0`; frozen binaries remain the exact historical
benchmark baseline.

The short 2026-09-11 pilots reused snapshot 055 for every rule/rate, with four
warm-up and twelve timed steps per case. Selected-default local-mode cycle
reductions versus the original paired runs were:

| Rule | 50% | 25% | 12.5% |
| --- | ---: | ---: | ---: |
| One-trace | 5.6% | 6.5% | 7.6% |
| Two-trace | 29.8% | 17.3% | 12.0% |
| Three-trace | 27.9% | 13.4% | 7.0% |

An exact waveform replay of the previously profiled one-input-spike
three-trace/50% step fell from 13,663 to 8,938 cycles. PU412's reverse key probes
fell from 566 to 171 for the same 28 updates, and receiver instructions fell
from 6,818 to 3,780. The waveform confirms the fused two-phase schedule.

All 71 checkpoint simulations and twelve directed simulations completed
successfully and passed their FP32 plasticity/state checks. Weights, input state
and observed post-spike histories match the original checkpoint runs exactly;
some excitatory conductance/voltage fields differ by up to three FP32 ULPs with
the changed arrival order. The quiet rest pilots are completely bitwise
identical. Fusion helped the rest cases more than the default one-trace
unfused schedule, so the latter is not a universal optimum. These are short
pilots, not a rerun of the full reservoir benchmark. Detailed per-mode results,
ablation measurements, validation limits and artifacts are in
`copilot/tmp/mnist-sparse-compact-20260911/summary.md` and `summary.json`.

For two-/three-trace updates, depression is applied and clamped before
potentiation. Present zero weights are processed normally. Bounds and the FP32
operation order match the dense reference-validated kernels. The sparse build
currently uses the bit-seeded fifth-root helper; LUT mode is rejected because
the sparse SPM layout does not reserve its table.

The group-mask tree forwards data before scanning local degree counts. Otherwise
the degree work accumulates along the tree's critical path. Common completion
counts use precomputed totals: 50 group posts, the connected-post count, or the
local structural edge count, depending on the mode. Empty post masks bypass
the scan entirely.

An input PU with no spikes reports completion immediately. In the separate-phase
comparison build, the root skips feedforward when preparation reports no spikes.
Reverse senders also skip a nonfiring post when its whole
input group is inactive, and skip inactive destinations without scheduling an
extra continuation for each skipped entry.

## Initial optimization

The selected payloads are archived under
`copilot/tmp/mnist-sparse-20260910/payloads-opt5/`. Pilots guided these changes:
copy only initialized SPM data; collect and distribute group masks using a tree;
forward masks before local degree work; use precomputed completion totals;
bypass empty work; and combine input preparation with acknowledged feedforward.

The following synthetic 12.5% pilot uses four warm-up and twelve timed steps.
These windows contain input spikes but no E/I spikes, so they measure the quiet
post path and are not a substitute for the trained snapshot benchmark.

| Rule | Local, cycles/step | PU scatter, cycles/step | Neuron scatter, cycles/step |
| --- | ---: | ---: | ---: |
| One-trace | 4,986.92 | 4,986.92 | 4,986.92 |
| Two-trace | 9,484.67 | 9,675.58 | 8,347.25 |
| Three-trace | 8,346.33 | 8,511.92 | 7,600.75 |

The original preflight also compares all three modes for presentation and rest
snapshots with the full four-plus-twelve-step window: one-trace at 50%, two-trace
at 25%, and three-trace at 12.5%. It verifies every weight and trace transition.
The first two corpora are ten-example reference pilots; the third is the full
30,000-example corpus. Earlier native snapshot pilots cover all nine rule/rate
pairs. All these paired runs have matching weights, input state, and neuron state.

## Reference generation

The generator supports fresh training and the recorded GeNN profiling workload.
Fresh training starts each rule/rate pair from the original untrained weight
matrix and applies its structural mask. Rates use the same seeded uniform
random values, giving nested masks. Sparse reference parameters are:

- Normalization target: column sum 78.
- STDP weight cap: `1 / connection_rate`.
- One-/two-trace learning rates: dense rates multiplied by `rate ** -0.8`.
- Three-trace learning rates: dense rates multiplied by `rate ** -1`.

Snapshots include the structural mask, all live neuron/trace state, the rule and
connectivity metadata, and deterministic ActiveN input RNG states. Image
generation rejects mismatched rules, rates, seeds, masks, or compensation
parameters and rejects nonzero weights outside the structural mask.

The original corpus plan was 100 snapshots per rule/rate, reservoir-sampled over
30,000 accepted training examples: 70 presentation windows and 30 rest windows.
The comparison uses identical snapshots across delivery modes, with four
warm-up and twelve timed 0.5-ms steps. Default timing uses the same flat-memory
soft backend as the previous dense benchmarks; Ramulator is an explicit option.

The completed GeNN profiling workload in the sibling repository's `BASELINE.md`
uses a different protocol: a 100-image branched continuation from each recorded
10,000-image checkpoint, with seed 20260724. Except for independently trained
12.5% one-trace, sparse starts are derived by masking a dense checkpoint and
applying bounded column rescaling during input preparation. This preparation
does not replace the ordinary normalization performed before each attempt.

`--baseline-inputs <prepared-input-directory>` now loads those exact immutable
checkpoints and verifies their hashes, variant metadata, and training dataset
hashes. It defaults to seed 20260724 and ending sample 10100. Sparse execution
uses presynaptic parallelism with 32 threads per spike, matching the profile;
the earlier exporter incorrectly retained dense postsynaptic scheduling with
one thread per spike. Build reuse now checks a source/graph/seed/scheduling
contract as well as the generated seed. The GeNN model and normalization code
are unchanged.

The new comparison artifacts are in
`copilot/tmp/mnist-sparse-genn-fix-20260910/`. Each case runs both the unchanged
GeNN baseline harness and the snapshot collector, compares their retry/stimulus
sequences and final state arrays, and checks population totals and all 400
excitatory spike counts against the recorded GPU profile. The Python suite
passes all 65 tests. The full benchmark was stopped and its results preserved
at the user's request; renewed ActiveN correctness checks use one presentation
and one rest window for every rule/rate/mode combination before restart.

The renewed matrix passes all 54 simulations: nine rule/rate pairs, two windows
per pair, and all three reverse modes. Each simulation independently validates
all weight and trace updates across four warm-up and twelve timed steps. All
paired final weights, replicated input states, and neuron states match exactly.
An independent observer audit also passes all 648 timed steps. Nine additional
directed simulations force all 784 inputs and 400 posts to spike, covering a
concentrated graph, single-MC rows, an empty post row, zero/subnormal weights,
and above-cap weights. Their independent first-step checks and all paired final
state comparisons pass. Results are summarized in that directory's
`validation.md` and `validation.json`.

The 50% two-trace collector initially differed from its control in one final
membrane voltage: neuron 219 was `-69.95746612548828` instead of
`-69.95745086669922` mV, a two-ULP FP32 difference. Repeating the unchanged
control reproduced the collector's entire final state bit-for-bit. Both control
executions also matched the recorded event counts, all 400 excitatory spike
counts, and the complete accepted-stimulus sequence. One intermediate maximum
excitatory conductance differed by one ULP between controls. These observations
demonstrate reference-run variability; the collector check uses no numerical
tolerance and retains both control results and their differences.
The 25% three-trace case likewise needed one repeated control: its first control
differed in two final voltages by one ULP, and its repeated control matched every
final state array exactly.
For 50% three-trace, the first and repeated controls differ from the collector
at different neurons (3 and 257 respectively), each by one ULP. Every captured
voltage is therefore exactly reproduced at that neuron by an unchanged control,
and every other final state array matches both controls. This last case passes
the measured repeatability check but has no single control execution with an
identical full state. All nine cases match the recorded spike diagnostics and
the control's accepted-stimulus sequence. No floating-point tolerance is used.

The connection comparison intentionally retains the existing dense arithmetic.
An audit found two inherited differences from the frozen GeNN model: the ActiveN
trace factors `0x3f7cd1e6` and `0x3f7e67ad` correspond to approximately 40/80 ms
at a 0.5-ms step, whereas GeNN uses 20/40 ms; ActiveN uses the dense Euler membrane
update with midpoint conductances, whereas this GeNN source uses an exponential
membrane update. GeNN therefore supplies realistic starting states. The explicit
FP32 oracle validates ActiveN's update sequence and addressing, and the three
delivery modes are compared against each other. These tests do not establish
trajectory equivalence with GeNN or learning accuracy for the ActiveN kernel.

All nine ten-example reference pilots completed and passed mask/hash audits.
Long training exposed the existing 2% post-normalization cap tolerance: the
12.5% one-trace run stopped at maximum weight 8.221274210 and two-trace stopped
at 8.459968692, against limit 8.16. At 25%, one-trace stopped at 4.195071601 and
two-trace at 4.239576533, against limit 4.08. At 50%, one-trace stopped at
2.049446017, against limit 2.04. Their incomplete runs are preserved and are not
accepted by the full benchmark driver. These failures occur in the GeNN
checkpoint generator, before the ActiveN comparison can use those corpora.

The original successful Brian 1 sparse runs use the same column scaling and
2% guard. The sibling repository's `sparse-networks.md`, under "Bernoulli 12.5%
GPU portability check", already records that the sparse GeNN port does not
reproduce Brian dynamics: an older 30k GeNN run completed before enforcement of
the guard, with transient weights reaching 33.37 and a final maximum of 25.02
for `wmax=8`. That completion is not evidence of a valid guarded trajectory.

A source audit finds a concrete one-trace discrepancy: Brian's compiled STDP
adds a weight clamp after both pre- and postsynaptic updates, including a
presynaptic event whose user rule only increments a trace. The frozen GeNN
one-trace presynaptic synapse handler only calls `addToPost(g)`; it omits that
clamp. This changes treatment of normalization overshoots. It has not yet been
isolated as the sole cause of the long-run failures, and it does not by itself
explain the two-trace failures, whose presynaptic handler already clamps.

The earlier request to choose a new normalization policy was premature and is
withdrawn. The original normalization definition is preserved. The successful
short GeNN profile and the failed fresh 30k runs are different workloads;
matching the profile does not establish successful fresh sparse training
through 30k.

A fresh 12.5% one-trace recheck with the corrected presynaptic/32-thread
scheduler confirms that scheduling alone does not resolve the original failure.
Using the original untrained weights and seed 19260817, it again fails the
unchanged guard with `max=8.221274210`, `limit=8.160000000`, after the 12,000-image
progress report. Its artifacts are in
`copilot/tmp/mnist-sparse-genn-fix-20260910/fresh-recheck-one0125/`.
The 900 snapshots for the recorded profiling workload pass restart preflight,
but that workload has not been substituted for the original fresh-training
corpus without resolving the workload choice with the user.

The frozen GeNN power-rule kernels evaluate `(weightMax - g) ** 0.2` before
clamping the updated weight. Above-cap values therefore give the fractional
power a negative base. The inherited ActiveN fifth-root helper instead returns
zero for a nonpositive argument. Retaining above-cap normalization is a workload
decision as well as a guard setting; this is another reason not to claim GeNN
trajectory equivalence from these connection-path tests.

An audit of the saved reservoirs confirms that above-cap states are actually
present: one snapshot for 12.5% one-trace, six for 12.5% two-trace, and two for
25% two-trace. The corresponding counts of above-cap weight observations are
1, 11, and 2. All saved weights are finite and nonnegative. No above-cap weight
appears in the completed 12.5%/25% three-trace snapshots. This snapshot audit
does not bound transient overshoots between captures; the larger maxima reported
by the normalization guard are measured immediately after column scaling.

The 12.5% three-trace run completed 30,000 accepted examples in 30,000 attempts,
retained all 100 snapshots, and passed its structural-mask and snapshot-hash
audit. Its final reference state digest is
`26cae1687eaf385806e1f101e2ba0af82cac1495b8258d5419441f1517496edd`.
The 25% three-trace corpus also completed and passed the same audits, with digest
`5ed36ef4e4825841b04fbd06535bcfb19ed65c3e4d507fbcee279c6639d75d30`.

## Reproduction and validation

Build one variant with the repository development shell:

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN/sim/payloads && \
  make sys/mnist_sparse.bin MNIST_RULE=three-trace MNIST_SPARSE_MODE=2'
```

Generate its images and run the maintained 512-PU simulator:

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN && \
  PYTHONDONTWRITEBYTECODE=1 python datagen/mnist_sparse.py \
    --payload sim/payloads/sys/mnist_sparse.bin \
    --output copilot/tmp/sparse-example --reverse-mode pu-scatter \
    --rule three-trace --connection-rate 0.125 && \
  SOFT_MNIST_SPARSE_STATS=1 sim/build/sim_system --soft --soft-threads 4 \
    --pu 512 --mc 16 --mc-size 0x100000000 --max-cycles 3000000 \
    copilot/tmp/sparse-example/dram.{0..15}'
```

`SOFT_MNIST_SPARSE_STATS=1` reads completed-step activity and delivery counts at
timer boundaries. `SOFT_MNIST_SPARSE_DUMP=<path>` saves initialized SPM state at
simulation exit. `SOFT_MNIST_SPARSE_STEPS=1` records the eight post-firing masks
after each completed step, including warm-up steps. These observers add no
target instructions. The original
three-trace smoke reproduces checksum `0x96beb6ea`, 601,151 total cycles, and
58,229 timed cycles with the observers enabled.

For a completed corpus, the benchmark driver expects a directory containing
`one-trace-local.bin`, `one-trace-pu-scatter.bin`, and
`one-trace-neuron-scatter.bin`, substituting the selected rule name:

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN && \
  PYTHONDONTWRITEBYTECODE=1 python datagen/mnist_sparse_benchmark.py \
    --snapshots <completed-snapshot-directory> --payload-dir <payload-directory> \
    --output copilot/tmp/sparse-benchmark --rule one-trace --connection-rate 0.125'
```

The driver freezes the generator, payloads, simulator, and configuration hashes;
retains images, logs, commands, and final state for every case; rejects changed
provenance on resume; and requires identical final logical weights and input
state across the three modes. Every case must pass an independent FP32 replay
from its initial forward graph and observed post spikes. The replay derives
presynaptic spikes from the initial RNGs and validates all structural weights,
all eight input-state copies, and post traces. It does not read reverse edge
lists or infer correctness from agreement among delivery modes.
Neuron-state differences are listed separately
because FP32 feedforward accumulation order can depend on packet timing.

Structural tests cover mask nesting, exact forward/reverse correspondence,
scatter partitioning and padding, SPM bounds, and group-tree coverage. The
directed FP32 oracle checks every weight and input state for all nine rule/mode
combinations with simultaneous posts, colocated posts, and high mask bits. The
dense kernel extraction produces byte-identical dense binaries for all rules.
The benchmark parser and state reader were checked against 27 completed sparse
simulations before being used for native snapshot pilots.
The earlier sparse implementation passed all 65 Python tests. A saturated 50% random case with
784 input spikes and 400 post spikes checks all 156,873 weight updates in each
mode. A separate directed graph checks single-MC reverse rows, an empty post row,
structurally present zero/subnormal weights, and values above the STDP cap.

## Dense reverse-map baseline (2026-09-12)

Select `MNIST_SPARSE_MODE=4` when building and `--reverse-mode dense` when
creating images. Forward weights remain sparse CSR; this is a dense **reverse
map**, not a fully connected network. Delivery, horizontal ownership, caps,
compact scheduling, and the sorted fired-vector/forward-send order match mode 1.
The sort is retained to keep forward accumulation order comparable; reverse
lookup itself needs no sorting or search.

Each post row contains one four-byte entry per local input:
`uint16_t weight_offset; uint16_t skip`. A zero offset denotes no structural
connection. A present zero-valued weight still has a nonzero offset. Offsets are
relative to the receiving PU's SPM base, as in the other sparse ABIs.
`skip[i]` is the positive distance to the next non-null entry strictly after
slot `i`, or to the row end if none remains. A fired-post scan starts at slot
zero and follows these skips, visiting at most one leading null slot and every
present connection. Entirely empty rows jump straight to the end.

Quiet-post processing uses each fired input's state pointer to recover its local
index: `((state - input_base) >> 2) * 0xb6db6db7`, taking the low 32 bits.
The displacement is an exact multiple of 28, and the constant is the modular
inverse of 7, so this gives the exact index using the existing integer multiplier.
The indexed halfword then supplies the weight offset or NULL. Quiet processing
is O(number of fired local inputs); fired-post traversal is O(local degree + 1).
The table costs O(local inputs × local posts), deliberately providing a dense
baseline for the more scalable CSR modes.

For the existing Bernoulli masks (seed 20260723), the maximum initialized data
footprints, including skip fields and all other state, are:

| Connection rate | Maximum SPM data bytes | Bytes below the 12,288-byte stack boundary |
| --- | ---: | ---: |
| 50% | 10,632 | 1,656 |
| 25% | 8,398 | 3,890 |
| 12.5% | 7,288 | 5,000 |

Startup copies round these images up to 32-byte lines: 10,656, 8,416, and
7,296 bytes respectively. Even including that padding, the smallest remaining
gap before the reserved stack region is 1,632 bytes.

A 22-input PU uses 4,400 bytes for dense entries, plus 102 bytes for row pointers.
The generator checks every PU against the data limit; these measurements do not
assert fit for arbitrary larger layers or connection rates.

`mnist_sparse_benchmark.py --modes dense` runs only the new baseline. Omitting
`--modes` runs all four modes; any explicit subset is supported. Per-case FP32
replay remains mandatory, and multi-mode runs compare weight/input hashes across
the selected modes. Experiments and build provenance for the added mode are in
`copilot/tmp/mnist-sparse-dense-20260912/`.

Validation completed with eight structural unit tests, twelve payload builds,
six directed simulations, twelve checkpoint pilots, and one dense-only native
benchmark-driver smoke test. The original nine binaries remain byte-for-byte
identical. All nineteen simulator runs passed FP32 replay; all twelve checkpoint
pilots matched compact-local weights, input state, neuron state, spike histories
and measured activity exactly. A four-mode summary check used actual validated
results from all four implementations.

Snapshot-055 presentation pilot cycle reductions relative to compact-local
(four warm-up, twelve timed steps) were:

| Rule | 50% | 25% | 12.5% |
| --- | ---: | ---: | ---: |
| One-trace | -0.28% | 0.00% | -0.19% |
| Two-trace | 14.09% | 11.94% | 7.81% |
| Three-trace | 16.46% | 12.82% | 6.77% |

Negative reductions mean a small slowdown. All three snapshot-070 rest pilots
had identical cycle counts. These short windows do not establish full-corpus
averages. The validated implementation report is
`copilot/tmp/mnist-sparse-dense-20260912/summary.md`.

The 900-case full benchmark is queued in
`copilot/tmp/mnist-sparse-dense-full-20260912/`, reusing twelve validated pilots.
It waits for the existing compact controller to exit successfully, then runs
only mode 4 on the same reference windows and audits against compact-local.
Frozen provenance, process identities, status, and automatic final reporting
are retained in that directory; no manual batch handoff is needed.

## Reverse setup pilot

`MNIST_SPARSE_CACHE_REVERSE=1` (the compact-schedule default) prepares each
input PU's reverse receiver registers before contributing to the existing
neuron/group barrier. Thread zero installs its cache and sends one local
setup event through the existing thread-one setup queue. Thread one installs
the same cache and yields the PU's barrier contribution to the existing
thread-zero reducer. No new global phase, handler queue, priority, or quota is
introduced. Reverse messages cannot be issued until every input PU has
completed both setups.

Both threads retain the reverse-directory and per-post weight-cap base
addresses. One-/two-trace handlers also retain the fired-vector end and the
potentiation rate/pre-trace target; their six persistent fifth-root constants
keep their existing registers. Three-trace has
more free registers and additionally caches fired-vector bounds, input-state
base, and received-count address/expected value. Per-post cap selection and
all required FP32 operations remain in the receiver.

In mode 4, the fired scratch vector now reserves eight bytes per input:
the existing absolute input-state pointer followed by its byte index in a
dense reverse row. Each firing computes that index once during preparation;
insertion sort moves the two words together. The per-trace vector loop loads
the cached index directly. When exactly one input fired, setup selects a
quiet handler that performs one direct lookup and no vector loop. The
ordinary post-fired handler remains available on both threads. Other modes
retain their four-byte fired vectors; no synapse directory is added.

The generator's larger mode-4 scratch allocation also accommodates legacy
four-byte-vector binaries, allowing paired control/optimized images to have
the same static layout. `MNIST_SPARSE_CACHE_REVERSE=0` retains the old receiver
setup and vector format for comparisons. Destination activity masks and
message-count semantics are unchanged in this pilot.

The benchmark driver accepts `--init-icache`, records it in provenance, and
passes it to the simulator. Both pilot variants use prefill with the existing
four warmup and twelve timed steps. The frozen baseline, optimized payloads,
directed checks, and background pilot are under
`copilot/tmp/mnist-sparse-setup-20260913/`. Performance conclusions must use
that directory's paired results; earlier cold-cache results are not its
control. This pilot does not implement per-post activity masks or nibble
skipping.
