# Sparse Brunel training

The hand-written `sim/payloads/sys/brunel_runtime.S` implements
Additive and Morrison training on
512 PUs, 16 MCs, two SMT pipes, 16 KiB SPM per PU, and 256-bit memory lines.
`datagen/brunel.py` generates the images and an independent NumPy FP32 reference;
`datagen/brunel_validate.py` compares simulator snapshots against that reference.
This is a separate payload; it does not change the existing MNIST formats.

## Model and comparison contract

The model follows the selected sparse, zero-delay alpha-current Brunel cases in
`../stdp-experiments/BASELINE.md` and `genn-sweep/README.md`:

| Quantity | Value |
| --- | ---: |
| Excitatory / inhibitory neurons | 9,000 / 2,250 |
| Excitatory / inhibitory fixed indegree | 450 / 112 |
| Plastic E→E weights | 4,050,000 |
| Static E→I, I→E, I→I weights | 2,272,500 |
| Integration timestep | 0.1 ms |
| Membrane / alpha-current time constant | 10 / 0.32582722403722841 ms |
| Capacitance | 250 pF |
| Rest, reset / threshold voltage | 0 / 20 mV |
| Refractory duration | 5 steps |
| Base excitatory weight | 45.609600316540956 pA |
| Recurrent delivery multiplier | √20 |
| Pre / post trace decay time | 20 / 30 ms |
| External-rate scale, Additive / Morrison | 0.47 / 0.32 |

Connections are sampled with replacement, excluding recurrent autapses.
The NumPy RNG realization is recorded in the manifest: using the same seed does
**not** reproduce GeNN's graph RNG stream. External input is Poisson-distributed.
The payload keeps one xorshift32 state per SMT thread in a register during
integration and uses one draw per neuron with a generated inverse-CDF table.
The nine thresholds represent counts 0–8; the very small remaining tail is
mapped to 9. The CPU presimulation retains its independent per-neuron generator.

Each tick integrates neurons using the preceding tick's input accumulators,
delivers this tick's spikes, performs post-active reverse learning, then drains
and increments traces. A post-quiet recipient applies LTD immediately; a
post-active recipient defers both LTD and LTP to the reverse phase. Incoming
current sums can differ in their FP32 reduction order. The independent reference
applies the same equations eagerly and evaluates Morrison with FP64 NumPy
`power(w, 2/5)` rounded to FP32, independently of the assembly approximation.

Additive updates subtract `0.01 × 1.05 × 2JE × post_trace`, add
`0.01 × 2JE × pre_trace`, and clamp to `[0, 2JE]`. Morrison subtracts
`0.1 × 0.1026 × post_trace × w`, adds `0.1 × pre_trace × w^0.4`, and floors at
zero. Both rules use the decayed trace snapshots from integration. The
instruction-level optimizations retain the existing stored learning
coefficients and phase order. Weight units are scaled as described below.

The payload evaluates `w^(2/5)` with a bit-derived inverse-fifth-root seed,
one modified Newton step, and a quadratic residual correction. Its normal path
uses 39 instructions, including 17 FP operations and one integer multiply-high;
it needs no table, divide, or stack. Inputs below `0x02000000` are first scaled
by `2^100`; reducing the output exponent by 40 reverses that scaling exactly.
Positive finite FP32 inputs, including subnormals, and exact +0 are supported.
Negative values, infinities, and NaNs are outside the weight-domain contract.

The test in `sim/tests/brunel_pow04.cpp`, run with `--exhaustive`, checks every positive finite FP32
pattern against an independent exact integer rounding oracle. The worst error
is two representable FP32 steps; an additional binade-boundary check establishes
the requested four-ULP bound relative to the mathematical result. The assembly
startup self-test covers boundary and difficult inputs, and
`datagen/test_brunel.py` checks the FP32 operation ordering against the host helper.

CPU presimulation establishes neural activity before generating initially empty
timestep buffers. The simulated warmup is retained for comparable activity windows;
there is no persistent cache to populate. The default CPU
presimulation is 1,000 steps (100 ms). `--seek-active` advances to a reproducibly
selected active window if the initial 30+30 window is quiet, and records the
actual presimulation duration. Short burst windows are not estimates of the
long-duration population mean firing rate.

## Current assembly runtime: post-quiet delivery and post-active MSHRs

The current payload is **assembly only**, in `brunel.S` and `brunel_runtime.S`.
`make -C sim/payloads sys/brunel.bin` builds that runtime; `brunel_asm.bin` is
also available. The C kernel has been removed. The host numerical helper and
historical cache-policy evaluator remain independent testing/research tools.
The image ABI is `0x42524e34`; images with older plastic-row layouts must be
regenerated.

There is no persistent cache or deferred learning across timesteps. Plastic
weights are presynaptic-MC-owned, and static connections retain target-owned
CSR scatter. Neuron ownership is `PU = 1 + gid % 512`, local index `gid / 512`.

| SPM offsets | Bytes | Contents |
| --- | ---: | --- |
| `0x0000–0x0100` | 256 | Header, counters, constants |
| `0x0100–0x0900` | 2,048 | Up to 25 neurons, 80 bytes each |
| `0x0900–0x2900` | 8,192 | Reserved; the forward weight buffer has been removed |
| `0x2900–0x2d00` | 1,024 | 16 MSHRs, 64 bytes each |
| `0x2d00–0x2d80` | 128 | Four directory/transport scratch lines |
| `0x2d80–0x2e00` | 128 | Legacy descriptor, Poisson thresholds, stream/tree state |
| `0x2e00–0x2f00` | 256 | Reserved; the dirty bitmap has been removed |
| `0x2f00–0x3700` | 2,048 | Reserved; incoming-spike recording has been removed |
| `0x3700–0x3f00` | 2,048 | Two 1 KiB async-copy staging halves |
| `0x3f00–0x3f80` | 128 | SMT0 stack |
| `0x3f80–0x4000` | 128 | SMT1 stack |

The generator initializes through `0x3f00`; the observer dumps the complete
16 KiB, including both stack guards. Normal steady-state handlers have no stack
frames; the full-MSHR fallback spills three words in a 16-byte frame. The
arithmetic startup test uses at most 64 bytes including its callers. Both stack
base guards are checked at every timestep boundary. The work/wait/drain event
continuations have no stack frame across a yield. Helper calls preserve the
registers holding the forward stream's state.
Header offset 244 stores the immutable MC destination ID, computed at boot.
The neuron firing flag at offset 36 (`N_FIRE`) remains authoritative through
reverse learning. Offset 40 is reserved and retains its input-image value;
there is no per-timestep duplicate flag store. New manifests identify this
contract with `firing_flag: N_FIRE`.

### Integration

Handler 9 runs on both SMT threads. An AMO claims the next 80-byte neuron
record, so static SMT0 priority does not leave a fixed partition stranded on
SMT1. The final worker reports completion after an AMO join. Each worker loads
shared coefficients and its RNG state once, then writes its RNG state once on
completion. Neuron fields remain in registers until their final stores.

The membrane expression factors the equal excitatory/inhibitory coefficients
as `p31 * (dIex + dIin) + p32 * (Iex + Iin) + p33 * V`. The usual
non-refractory, non-firing path uses 19 FP instructions when external input is
zero, or 20 for one or two external events. Zero skips conversion and external
arithmetic entirely. One uses the precomputed external contribution in `s7`;
two uses `a7 = s7+s7`, computed once per worker. Counts of three or more retain
conversion and multiplication. The first three CDF thresholds stay in registers;
rarer cases read the remaining small table. Both threads retain dynamic AMO
work allocation and independent register-held RNG states.

### Forward and reverse processing

On a presynaptic spike, stream its outgoing row without retaining a forward
weight buffer. Each DRAM row consists of 1 KiB chunks: 128 FP32 weights in the
first 512 bytes and their 128 packed destinations in the next 512 bytes.
The last chunk is padded; only the row's actual entries are sent.

The first async copy loads one chunk into the first staging half. A local
yielding wait polls completion. Once ready, it starts the next copy into the
other half before draining this half. Each drain iteration loads the target
and weight from SPM, sends
the spike, and yields to a local drain continuation. The steady loop executes
ten instructions through the yield, including its exit branch, remote blocking send, and local
yield. The local yield replenishes quota and admits higher-priority handlers.
After the half is consumed, the continuation waits for the next copy and
exchanges halves. The MSHR region remains separate.

Boot fixes the async-copy byte length at 1 KiB. `LoadManual` changes only the
SPM destination and writes operation value 1 to CSR `0x730` for each chunk.
Software then sends bulk tag `0x811` to the source MC with
return ID `0x8001`: the high bit selects the LSU at the core crossbar, and the
LSU sees ID 1. There is one active bulk copy per PU. Before returning to
directory or MSHR scalar reads, software restores the shared AC SPM base to
`0x20000000`. Future overlap between forward and reverse phases must account
for that shared base; ID-2 scalar landing addresses use it too.

The matrix stores the delivered value `W = S*w`, where
`S = EPSC*delivery`. Sending therefore requires no FP multiply. Additive
increments and the upper bound scale by `S`. Morrison's proportional LTD
coefficient stays unchanged; its LTP coefficient becomes `eta*S^0.6`, since
`S*eta*(W/S)^0.4 = eta*S^0.6*W^0.4`.

Plastic deliveries carry `(inputOffset, weight, weightDramAddress, sourceMC)`.
The direct spike handler runs on both SMT threads and accumulates the input with
LR/SC. If the post did not fire and its decayed post trace is nonzero, it computes
LTD and sends the changed FP32 weight directly to the source MC with a
no-receipt store. It skips this learning arithmetic/store when the trace is zero,
while still accumulating and counting the spike. Static deliveries have address
zero and perform only accumulation. The essential receipt counter is atomic.

Every fired neuron has a 128-byte directory of sixteen `(rowAddress,count)`
descriptors. Software issues all four directory-line reads into the existing
scratch region before waiting for their individual completions. A yielding
worker retains its directory pointer, coefficients, and tag in registers.

Fired-post reverse CSR events carry `(preLocal, weightDramAddress, LTD, LTP)`.
The receiver suppresses LTD unless the pre also fired, and multiplies LTP by the
decayed local pre trace. A zero trace and quiet pre reject the event before the
FP multiply. Every nonzero update uses an MSHR. Post-quiet and post-active
updates are disjoint, including parallel edges with distinct addresses; multiple
outstanding MSHRs cannot update the same weight. Different words sharing a DDR
line remain independent because writeback changes only one FP32 word.

### MSHR progress and writeback

Each descriptor has a 32-byte aligned landing line, address, LTD coefficient,
LTP coefficient, state, diagnostic sequence number, and three reserved words.
A post-active update has LTP and may also have LTD if the pre fired this step.
Retirement applies LTD before LTP.

SMT0 owns a 16-entry FIFO allocation/retirement ring. State 1 means loading;
state 2 means its completion notification has been consumed. SMT1 is a short
leaf completion handler: it marks ready and retires one notification credit.
It never performs update arithmetic, waits for memory, or sends messages.

One coalesced local handler-6 event on SMT0 retires the ready ring head, applies
the update, emits one acknowledged FP32 store, and frees its landing line. It
reschedules itself while entries remain. Reverse handlers have higher dispatch
priority. If the pool is full, allocation services the head inline; this avoids
waiting for a lower-priority handler on the same SMT thread. FIFO retirement
can wait behind the oldest outstanding read, which is measured separately from
merely finding the pool full of ready entries.

There are still only **12 issued-but-unconsumed memory notifications** across
loads and stores, with a dedicated 16-entry completion queue. Sixteen descriptors
do not imply sixteen concurrent reads. Reusing a descriptor after issuing its
store is safe: the outgoing scalar request already contains its value. Store
acknowledgements still occupy notification credits. The finish barrier waits
for all MSHR events and all store acknowledgements before starting another tick.

Handler 6 uses SMT0, zero arguments, quota 2 and margin 3: at most one store and
one local continuation. Its local event queue contains at most one notification.
Existing reverse/work quotas remain sufficient (at most one inline retirement,
one read, and one service event per allocating reverse handler). NoC priorities
remain writeback 1, refill 2, neural/scatter 3; control and local messages use 0.
Forward wait/drain handlers 11/12 use SMT0, zero arguments, quota 2 and margin
19, leaving room for the existing higher-priority handlers.

Boot selects the rule-specific spike handler and LTD/LTP leaf addresses once.
Disabled event-address CSRs `0x70d/0x70e` hold the leaf pointers; their event
enable masks remain zero. No handler quotas, priorities, or queue sizes change.
Directional clamps retain only the applicable bound for nonnegative input
weights/coefficient values. A generic rule-dispatch wrapper exists only in the
numeric self-test build. Local direct calls use `jal`, and leaf/private-register
contracts avoid call frames. MSHR retirement returns its remaining count to its
caller, removing repeated count loads.

The final phase drains acknowledgements, checks descriptor and stack guards,
increments the traces of fired neurons, and completes the reduction barrier.
It has no weight-buffer flush or recorded-spike replay.

### Counters, profiling, and validation

The optional `BR_DIAGNOSTIC` counters at offsets 120–148 retain their historical
layout; active counters include MSHR loads/commits, weight stores, full-pool
encounters, forward weights, and reverse events. Offset 252 counts full-pool
encounters where the head was not ready. Offsets 152–180 hold buffer bytes,
MSHR live count, head, tail, coalesced-event flag, two SMT RNG states, and peak
MSHR occupancy. Counters are cumulative; reports express increments per timestep.
The stream descriptor at `0x2dd0` records its state, copies, overlap checks,
copies already complete when the preceding drain ends, and busy wait polls.
Production benchmarks leave diagnostics disabled. Transport/barrier receipt
counts remain enabled because they are required for correctness.

`SOFT_BRUNEL_DUMP=<directory>` writes complete timestep snapshots.
`SOFT_BRUNEL_PROFILE=1` additionally writes:

- `pu-profile.csv`: sampled activity, stalls, queue pressure, MSHR occupancy,
  and notification-credit saturation; exact issued-instruction and neuron
  counts per SMT, AC wait cycles, BIU rejection cycles, and last plastic-send
  injection cycle per PU and phase.
- `mc-profile.csv`: request admission and inflight/fulfilled occupancy.
- `pc-profile.csv`: sampled PCs in the timed interval.
- `mshr-memory.csv`: exact accepted source-router reads and scalar responses at
  their source PU, including timestep, PU, descriptor token and line address.
- `ac-memory.csv`: accepted forward bulk requests and every returning beat,
  allowing copy-latency and complete-response accounting checks.

`datagen/brunel_buffer_profile.py` supports the older buffered/replay runtime;
its counter expectations and some preserved-simulator AC/last-injection hooks
assume the older phase numbering. Do not apply those fields uncritically to
the current post-quiet runtime. Current phase timings use `mc-profile.csv`;
instruction counts, BIU rejection cycles, sampled PCs, and `mshr-memory.csv`
remain usable. Memory response latency excludes software issue/notification/service time.
The four phase costs come from simulator-observed phase-marker stores and the
completed-timestep store; the small gap between timesteps is reported separately.
`phase-cycles.csv` contains each timed step, and `profile.json` includes the
fraction of prefetched copies ready before the preceding half finishes draining.
The last plastic-injecting PU is identified explicitly; it is not assumed to
be the last PU to complete the whole forward phase, which also includes static
scatter and barriers.
`datagen/brunel_hotspots.py` also retains buffered-runtime counter checks;
current production builds require trace-derived work counts rather than its
disabled diagnostic counters.

With the shared RNG, the validator reconstructs recurrent input from reference
weights and recorded spikes, then infers each integer external Poisson draw
from the observed current increment. A residual check rejects non-integer
increments. It verifies the complete deterministic neuron update, carries
observed neuron states into the next reference step, and keeps reference weights
independent for the entire run. Final DRAM weights are compared against those
reference weights. Free-running reference spike counts are reported separately
to assess the broader dynamics.

Build and run through the Nix development shell, using an already-built 512-PU,
16-MC, 256-bit, two-SMT, 8 KiB ICache simulator. Workload-only changes need no
simulator rebuild or re-elaboration. The following example assumes `sim_system`
is that matching binary; use a preserved binary path if another session has
changed the current elaboration. Its Ramulator flag selects HBM timing and must
be omitted when reproducing the flat-memory comparisons recorded below.

```sh
make -C sim/payloads sys/brunel.bin sys/brunel_asm_numeric.bin
python3 datagen/brunel.py --rule morrison --presim 1000 --warmup 60 --timed 60 \
  --output copilot/tmp/brunel-morrison
python3 sim/tests/brunel_buffer_image.py copilot/tmp/brunel-morrison
mkdir -p copilot/tmp/brunel-morrison-run
SOFT_BRUNEL_DUMP=copilot/tmp/brunel-morrison-run SOFT_BRUNEL_PROFILE=1 \
SOFT_MNIST_PHASE_LOG=1 sim/build/sim_system --soft --soft-threads 8 \
  --pu 512 --mc 16 --mc-size 0x100000000 --init-icache --max-cycles 30000000 \
  --ramulator-config sim/build/ramulator-hbm2-1ch.yaml \
  copilot/tmp/brunel-morrison/dram.{0..15} > copilot/tmp/brunel-morrison-run/run.log 2>&1
python3 datagen/brunel_validate.py --images copilot/tmp/brunel-morrison \
  --dumps copilot/tmp/brunel-morrison-run --log copilot/tmp/brunel-morrison-run/run.log \
  --output copilot/tmp/brunel-morrison-run/report.json
c++ -std=c++20 -O3 -ffp-contract=off -fopenmp sim/tests/brunel_pow04.cpp \
  -o copilot/tmp/brunel-pow04-test
copilot/tmp/brunel-pow04-test --exhaustive
```

## Iterative power and assembly optimization benchmark (2026-09-17)

The preserved 512-PU/16-MC simulator ran 60 warmup plus 60 timed steps with
eight host threads, an initialized I-cache, flat memory, and `BR_DIAGNOSTIC=0`.
Its binary and the graph/initial neuron/weight images match the preceding
2026-09-16 benchmark. Only embedded payload code changed. Clean and passive
observation runs both reported 13,528.67 cycles per timed timestep.

| Phase | Previous cycles/timestep | Optimized cycles/timestep | Reduction |
| --- | ---: | ---: | ---: |
| Integration | 2,347.77 | 2,282.07 | 2.80% |
| Forward + post-quiet LTD | 8,675.78 | 7,492.62 | 13.64% |
| Post-active LTD/LTP | 3,639.03 | 2,715.40 | 25.38% |
| Drain + trace increments | 1,039.60 | 1,038.53 | 0.10% |
| Workload timer total | 15,702.23 | 13,528.67 | 13.84% |

The full-window gain includes a stochastic workload difference: the changed
integration instruction sequence changes per-SMT RNG-to-neuron assignment.
Timed E/I spikes average 1.97/0.53 per timestep versus 3.25/0.93 previously.
Steps 1–4 have identical fired-neuron identities. Steps 2–4, excluding the
initial cache-refill transient, take 16,318.33 versus 17,836.67 cycles/timestep
(8.51% reduction); forward improves 6.01% and reverse 18.23% on those steps.
This short matched window is a check on the implementation gain, not a
long-duration firing-rate estimate.

All 120 steps passed deterministic replay of observed Poisson draws: current
and trace fields matched exactly, and maximum voltage error was 3.82e-6 mV.
All 4,050,000 weights were within one FP32 step of the independent reference
(maximum absolute error 1.220703125e-4 in scaled weight units). Additive and
Morrison numerical pilots passed. A one-entry-MSHR diagnostic stress test passed
with 1,042 full-pool encounters per timestep. Both stack guards and transport
retirement checks passed. The exhaustive power test covered 2,139,095,039
positive finite FP32 inputs, with a worst error of two representable steps.

The production payload is 4,368 bytes. Forward remains 55.38% of timed cycles;
the busiest PU issues 13.83 times the average PU's forward instructions. No BIU
enqueue rejection cycles or sampled credit/MSHR saturation occurred. MSHR loads
average 459.67 per timestep, including 10.58 repeated loads of a PU-local line.
These flat-memory measurements do not establish real-HBM latency tolerance.

Reproduction commands, source/binary snapshots, logs, phase/PC/PU observations,
and validation reports are in `copilot/tmp/brunel-iterative-20260917/`.
`results.json` and `report.md` include the workload and comparison limits.

## Validated 8 KiB-ICache benchmark (2026-09-15)

The final pilot and benchmark use the regenerated 512-PU/16-MC/256-bit/two-SMT
configuration with 128 64-byte ICache lines (8 KiB), HBM2 timing, eight soft
simulator workers, 30 warm-up steps, and 30 timed steps. The simulator binary,
payload, and each embedded `dram.0` payload were hash-checked before the run.
Both runs returned result 0. The complete snapshots pass stochastic replay,
integer neuron-state checks, voltage/current tolerances, final weight checks,
SPM guard checks, and memory-transport accounting. The six generator tests and
both initial-image checks also pass.

All traffic values below are averages per timed timestep. Phase values are
cycles per timestep. The old column is the previous assembly implementation
from the matching 30-step baseline; its images and initial-state hashes are
recorded in the experiment directory.

| Rule | Total cycles | Old total | Change | Forward | Integration | Reverse | Flush |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Additive | 153,596.2 | 205,039.6 | −25.1% | 24,638.7 (−63.2%) | 2,522.0 (−43.7%) | 94,559.4 (−3.3%) | 31,840.7 (−11.0%) |
| Morrison | 89,444.5 | 114,104.8 | −21.6% | 12,478.8 (−64.8%) | 2,536.1 (−45.2%) | 52,285.6 (+1.1%) | 22,108.6 (−0.7%) |

The phase-marker/timer boundary contributes 35.4 cycles per timestep. Additive
averages 76.7 excitatory and 17.5 inhibitory spikes; Morrison averages 8.0 and
2.17. The free-running reference realization averages 63.4/15.6 and 9.73/2.17,
respectively. Because integration uses per-SMT xorshift states rather than the
CPU reference generator, those free-running counts are a dynamics comparison,
not a deterministic target. Replaying the external integer draws inferred from
the observed current makes every integer neuron state agree and keeps the
maximum voltage error at 1.91e-6 mV; final Additive and Morrison weights match
the independent reference bit-for-bit.

| Rule | Forward weights | Buffered updates | MSHR loads/commits | Scalar stores | Reverse events | Scatter/manual reverse |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Additive | 33,375.0 | 14,887.1 | 14,691.5 / 14,691.5 | 29,515.6 | 67,383.4 | 34,500.0 / 32,883.4 |
| Morrison | 3,699.3 | 1,886.3 | 1,843.2 / 1,843.2 | 3,728.9 | 7,296.3 | 3,600.0 / 3,696.3 |

The 2 KiB ping-pong staging halves hide nearly all forward copy latency:
Additive has 296.8 copies and 222.6 overlap checks per timestep, with 99.76%
ready before the preceding drain ends; Morrison has 32.9 copies and 24.7
checks, all ready before drain. Additive's exceptional late-copy tail produces
0.53 late copies per timestep and a 29,752-cycle maximum wait on one PU during
the busiest burst, while its critical plastic-sending PU has zero BIU rejection
cycles. Morrison has no late copies. The source-MC request backpressure fraction
is 39.3% for Additive and 15.5% for Morrison, so the forward phase is still
memory-controller and workload-imbalance limited even though the drain loop
itself is short.

The dominant phase remains reverse processing: 94,559 cycles for Additive and
52,286 for Morrison. The sampled Additive reverse path is concentrated in the
work/reverse/record-replay and MSHR-retire handlers, while the Morrison path has
the same structure at lower traffic. Flush is the next largest phase and is
dominated by the dirty-word scan and scalar write acknowledgements. Integration
now takes about 2.5K cycles; both SMT threads claim work dynamically, averaging
14.95 and 7.02 neuron records per PU per timestep, with about 70 issued
instructions per neuron and no BIU blocking.

The timed Additive distribution has 57.8 weight updates per PU per timestep on
average, p50 26, p95 271, and a maximum of 729. Its worst PU/timestep updated
729 weights after three presynaptic neurons fired and forwarded 1,310 weights.
Morrison averages 7.28 updates per PU per timestep, p50 3, p95 9, and a maximum
of 460; that hotspot had two presynaptic neurons and 909 forward weights. The
largest forward row observed was 1,394 weights on Additive and 909 on Morrison,
well below the 8 KiB buffer. The 16-entry MSHR peaked at 16 descriptors for
Additive and 4 for Morrison; no allocation wait or overflow occurred in the
production runs.

Raw reports, phase CSVs, snapshots, and exact commands are under
`copilot/tmp/brunel-loadmanual-20260915/{additive,morrison}-resumed-run`.
The forced stress variant (zero forward buffer, one MSHR, two record slots)
also passes its one-step simulator and full final-weight validation, exercising
the spill and allocation fallback paths.

For transport/numeric validation, embed `brunel_asm_numeric.bin` in a short
image and run first with flat memory. Compile-time `BR_BUFFER_LIMIT` (bytes,
0–8192, multiple of 32) and `BR_MSHR_LIMIT` (1–16) allow forced-overflow and
allocation-pressure tests via `BRUNEL_ASM_FLAGS`; rebuild with `make -B` when
changing flags, and restore the normal payload afterwards. These limits affect
capacity, not the SPM ABI. No smaller queue or alternate memory response format
is required by this implementation.

## Historical cache experiments

The remaining sections record the superseded persistent-cache implementations.
Their C-kernel and hot-cache build commands are historical, not current workflows.

## Recorded replay and ordered MemIf results (2026-09-14)

The simulator was re-elaborated and rebuilt for the configuration above, including
the FIFO MemIf implementation. Each run uses eight simulator workers, the software
NoC/MemIf with RTL cores, HBM2 timing, 30 warmup steps and 30 timed steps. The C
payload is 8,000 bytes and the hand-written assembly payload is 6,276 bytes; both
fit the preloaded 8 KiB ICache, including the 16-byte image header.

The earlier individual-entry run was stopped early. Compare it with the new
implementations on **the same steps 31–40**, not against a different part of the
burst or against the earlier line-entry cache:

| Common steps 31–40 | Additive | Morrison |
| --- | ---: | ---: |
| Previous individual-entry cycles/timestep | 1,020,411.9 | 556,779.9 |
| Recorded replay, C cycles/timestep | 785,651.6 | 353,442.6 |
| Recorded replay, assembly cycles/timestep | 678,721.1 | 301,245.6 |
| Previous reverse deliveries/timestep | 3,803,239.7 | 1,953,795.8 |
| Assembly reverse deliveries/timestep | 99,742.9 | 10,121.4 |
| Previous table probes/timestep | 483,740.9 | 47,047.4 |
| Assembly table probes/timestep | 484,807.5 | 47,065.0 |

Reverse traffic falls by 97.38% / 99.48%, while assembly cycles fall by
33.49% / 45.90%. The removed deliveries mostly took the cheap early-return path.
Useful forwarding and learning work remains: table-probe counts barely change.
Traffic is distributed across the system, whereas a phase barrier waits for its
slowest PU. Removing most messages therefore does not remove most cycles.

### Full assembly timing and work

All averages in this table cover timed steps 31–60. A new update is a nonzero
LTD/LTP operation, even if rounding or clipping leaves the weight unchanged.
Applied arithmetic additionally accounts for materializing older pending values.

| Metric | Additive | Morrison |
| --- | ---: | ---: |
| C cycles/timestep | 667,488.3 | 351,923.0 |
| Assembly cycles/timestep | 570,721.0 | 300,412.1 |
| Assembly cycle reduction relative to C | 14.50% | 14.64% |
| Forward cycles/timestep | 391,316.8 | 207,703.0 |
| Reverse cycles/timestep | 173,395.1 | 86,548.9 |
| New LTD updates/PU/timestep | 24.24 | 4.27 |
| New LTP updates/PU/timestep | 23.81 | 4.38 |
| New updates/PU/timestep | 48.04 | 8.65 |
| Applied arithmetic updates/PU/timestep | 38.53 | 4.84 |
| Distinct updated synapses/PU/timestep | 47.96 | 8.65 |
| Forward weight reads/PU/timestep | 55.86 | 8.44 |
| Median new updates in a PU/timestep | 17 | 4 |
| 99th percentile new updates in a PU/timestep | 304.41 | 230 |
| Maximum new updates in one PU/timestep | 807 | 470 |
| Reverse deliveries/timestep | 56,793.97 | 8,695.80 |
| Table probes/timestep | 298,039.70 | 42,743.50 |
| FP32 word stores/timestep | 14,993.20 | 852.27 |

Additive's maximum update demand occurs on PU297 at step 41: four firing source
neurons cause 1,749 outgoing weight reads and 807 new updates. Its largest forward
row workload occurs on PU210 at step 39: four firing sources cause 1,807 weight
reads and 786 new updates, while that PU receives only 146 plastic spikes.
Morrison's maximum is PU190 at step 35: two firing sources, 915 outgoing weight
reads, 470 new updates, and 16 received plastic spikes. Fan-out averages 450;
these large source workloads do not require unusually large incoming fan-in.

### Measured bottleneck

Forward processing consumes 68.57% / 69.14% of assembly cycles. Its mean busiest
PU activity is 389,350 / 206,012 cycles per timestep, almost the complete forward
phase; mean activity across all PUs is only 23,713 / 3,360 cycles per timestep.
The busiest PU is selected separately each step. Source-owned rows concentrate
work on the few PUs whose neurons fired, leaving other PUs waiting at the barrier.

Sampled forward SMT0 fetch PCs attribute 35.56% / 28.16% to `br_insert`,
16.11% / 17.11% to `br_lookup`, and 15.81% / 16.80% to `br_forward_weights`.
The `br_load_wait` routine accounts for 5.77% / 3.26%. These are approximate
active-thread function shares, not retired-instruction or whole-phase shares.
The next optimization target is the insertion/probing and scalar admission path;
these measurements do not isolate the multiply hash as the cause.

Reverse processing remains a secondary bottleneck, especially when useful replay
converges on those source PUs. Additive's reverse-phase MC occupancy averages
36.73 entries per MC, of which 36.40 are already fulfilled, 0.021 are unissued,
and 0.308 await memory. The high occupancy is mainly completed scatter data
waiting for downstream delivery, consistent with slow receiver consumption.
Forward MC occupancy averages only 0.207 / 0.028 entries per MC. DRAM service
capacity is not the dominant measured limit; local cache work and the resulting
response/replay backpressure are more important.

There is also a remaining sender-side optimization: recorded replay currently
sends even when its LTD coefficient is zero. Filtering these would remove
15,984.6 / 2,129.8 deliveries per timestep, or 28.14% / 24.49% of the remaining
reverse traffic. These counts are independently reconstructed from the recorded
spikes and postsynaptic traces. This filter is not part of the measured payload;
forward cache processing remains the larger target.

### Cache contents, warmup and correctness

| Assembly cache metric | Additive | Morrison |
| --- | ---: | ---: |
| Timed resident-weight hit rate | 18.87% | 23.09% |
| Timed tag hit rate, including pending entries | 20.34% | 23.88% |
| Resident hit, steps 31–40 | 18.35% | 23.55% |
| Resident hit, steps 41–50 | 19.21% | 23.53% |
| Resident hit, steps 51–60 | 20.31% | 22.10% |
| Occupancy at step 30 / 60 | 14.19% / 99.37% | 27.23% / 62.03% |
| Final resident weights / all plastic synapses | 6.01% | 3.84% |
| Final resident weights plus pending coefficients / all plastic synapses | 10.05% | 6.27% |
| Maximum recording length in one PU/timestep | 221 | 25 |
| Overflow records/timestep | 0 | 0 |

The Additive table is still being populated in the first timed block; it reaches
93.03% occupancy at step 40. Morrison is fairly stable by the requested hit-rate
criterion, but continues accumulating pending entries. Its word stores rise from
582.9 to 801.9 to 1,172.0 per timestep across the three blocks. Stable hit rates
alone therefore do not establish settled writeback/materialization costs.

All four C/assembly runs match every reference firing decision across all 60 steps.
All 4.05 million final logical Additive weights match exactly; Morrison differs
on 75 weights by one FP32 ULP at most (3.8147e-6 pA), matching the earlier numerical
error bound. Maximum voltage errors are 1.3351e-5 / 3.8147e-6 mV. The full network
runs do not spill; a separate one-record-buffer assembly test exercises the
overflow fallback and numerical startup checks. Profile-on/off versions of that
test have identical target cycles, SPM snapshots, and final DRAM.

Artifacts are under `copilot/tmp/brunel-ordered-20260914/`: copied design and
source hashes, all four C/assembly runs, validation reports, `work-per-pu.csv`,
`hotspot-profile.json`, and the performance/cache and bottleneck figures. The
per-PU CSV independently agrees with runtime demand counters for every PU and
timestep, rather than estimating work by dividing message volume by PU count.

## Reference dynamics check (2026-09-14)

A separate 10,000-step CPU reference run from each benchmark's starting state
checks that the chosen graph realization and within-tick order retain the
baseline population dynamics. This is a model check, not simulated hardware
timing or a claim of bitwise agreement with GeNN.

| Rule | Reference E / I Hz | Baseline E / I Hz |
| --- | ---: | ---: |
| Additive | 4.3556 / 4.4836 | 4.417 / 4.507 |
| Morrison | 6.0173 / 6.0387 | 5.955 / 5.996 |

All four rates are within 1.5% of the recorded baseline. Final plastic weights
were finite and interior: Additive 38.30–51.23 pA, Morrison 41.29–48.48 pA.
Detailed counts, 100-ms chunks, and means are in
`copilot/tmp/brunel-20260914/{additive,morrison}-reference-long.json`.

## Initial line-entry baseline (2026-09-14)

This section records the earlier 160 × 40 B line-entry implementation, preserved
under `copilot/tmp/brunel-20260914/source/`. It is not the current cache layout.

Both runs completed successfully with the freshly elaborated 512-PU RTL cores,
the software NoC/MemIf backend, one HBM2 timing channel per MC, 32-byte
transactions, and a 1 GHz core/NoC clock. Both used 30 sequential warmup steps
and 30 sequential timed steps, without resetting neural or cache state between
the two periods. The 8 KiB instruction caches were preloaded.

| Metric | Additive | Morrison |
| --- | ---: | ---: |
| CPU presimulation | 115.9 ms, selected active window | 100 ms |
| Timed biological interval | 118.9–121.9 ms | 103–106 ms |
| Cycles per timestep | 676,657.1 | 422,110.8 |
| E / I spikes per timestep | 63.37 / 15.60 | 9.73 / 2.17 |
| Largest per-step spike-count difference | 0 | 0 |
| Largest voltage error, all neurons and all 60 steps | 0.000012398 mV | 0.000003815 mV |
| Largest final plastic-weight error | 0 pA | 0.000003815 pA |
| Final weights differing from reference | 0 / 4,050,000 | 75 / 4,050,000 |
| Timed resident-weight hit rate | 34.30% | 38.12% |
| Timed tag hit rate, including pending entries | 54.28% | 53.59% |
| Evictions per timestep | 10,880.07 | 1,255.23 |
| FP32 word stores per timestep | 18,957.57 | 2,121.90 |
| Word stores per eviction | 1.74 | 1.69 |
| Reverse-scatter deliveries per timestep | 3,504,446.60 | 1,866,736.77 |

All firing, previous-spike, refractory, RNG, and identity fields matched exactly
at every timestep. Pre/post traces also matched exactly. The small current and
voltage differences are consistent with FP32 accumulation order; Morrison's
75 differing weights each differ by one ULP. Final checks reconstruct every
plastic weight, verify drained request accounting, and reject invalid cache
tags or leftover spike accumulators. The transport startup regression and host
cache/numerical tests also passed.

### Warmup assessment

| Ten-step timing block | Additive resident / tag hit | Morrison resident / tag hit |
| --- | ---: | ---: |
| 31–40 | 32.34% / 52.13% | 39.13% / 52.05% |
| 41–50 | 36.27% / 56.79% | 38.88% / 54.83% |
| 51–60 | 37.70% / 57.07% | 36.13% / 54.28% |

Additive is still populating its table during the first timed block: occupancy
grows from 26.13% at step 30 to 99.01% at step 40. Its resident hit rate rises by
5.36 percentage points over the three blocks; the last two differ by 1.43 points.
Thirty warmup steps are therefore too short for a measurement that assumes a
settled Additive cache. A subsequent run should place the timing boundary after
this population phase, and account for the changing activity of the burst.

Morrison's resident hit rate spans 3.00 percentage points, and its tag hit rate
spans 2.78 points. This is approximately stable by the hit-rate criterion, though
occupancy continues growing from 52.78% at step 30 to 91.33% at step 60. These
short windows do not establish long-run stationarity. The Additive timing window
intentionally includes a burst; its short-window population rate must not be
compared directly with the one-second mean above.

### Actual buffered contents

| Final cache contents | Additive | Morrison |
| --- | ---: | ---: |
| Resident lines | 21,096 | 15,131 |
| Pending-only lines | 60,765 | 59,688 |
| Resident valid synapse weights | 167,471 (4.14%) | 120,125 (2.97%) |
| Pending synapse coefficients | 94,278 | 76,869 |
| Resident weights + pending coefficients / plastic synapses | 6.46% | 4.86% |
| Mean pending coefficients per pending-only line | 1.55 / 8 | 1.29 / 8 |

The nominal 16.18% capacity assumes all eight lanes in every entry hold useful
synapse data. Pending-only entries are much sparser in this workload, and occupy
most entries. This is a concrete reason to revisit pending-entry storage when
tuning the cache layout. Reverse-scatter delivery volume is also much larger
than word-writeback volume; the counters alone do not isolate their cycle costs.
This first runtime serializes cache mutations and refill consumption on SMT 0,
while SMT 1 handles completions/spikes and acknowledged writebacks can overlap.

The complete reports, per-step states, DRAM dumps, reference data, source hashes,
build logs, and plot scripts are under `copilot/tmp/brunel-20260914/`. In particular:

- `additive-final/report.json` and `morrison-final/report.json`: full metrics and
  checkpoint comparisons.
- `*-final/all-neuron-states.json`: every-timestep neuron comparisons.
- `*-final/ramulator/`: memory statistics for the complete simulation, including
  startup and warmup; the workload timer and cache counters above cover timing only.
- `brunel-results.{png,pdf,svg}`: firing counts, hit rates, and occupancy trends.
- `source-hashes.json`, `source/`, and `run-metadata.json`: build and input provenance.

## Individual-entry working sets and reuse

The next run uses 800 eight-byte synapse entries per PU while retaining async
32-byte fills. The graph, initial neural state, and 30 warmup + 30 timed steps
are identical to the line-entry baseline. `datagen/brunel_reuse.py` reconstructs
nonzero LTD/LTP operations from observed spikes and decayed traces; its counts
match the baseline runtime's update-demand counters exactly. Parallel edges
are counted as separate synapses, even when their neuron endpoints match.
An update means a nonzero coefficient, including a clipped or FP32-rounded
operation that might not change the stored weight numerically.

All traffic counts below are averages per timestep. Working-set cardinalities
are explicitly labeled as distinct counts over the requested window; they are
not cumulative traffic counts. Windows start at timestep 31, immediately after
the 30-step warmup. Amortized distinct counts divide that union by window length,
not the sum of each timestep's separately deduplicated updates.

| Timesteps in window | Additive distinct updated synapses | Additive amortized distinct / step | Morrison distinct updated synapses | Morrison amortized distinct / step |
| --- | ---: | ---: | ---: | ---: |
| 10 | 382,082 | 38,208.20 | 50,574 | 5,057.40 |
| 20 | 589,975 | 29,498.75 | 90,118 | 4,505.90 |
| 30 | 672,152 | 22,405.07 | 130,976 | 4,365.87 |

| Mean per timestep, steps 31–60 | Additive | Morrison |
| --- | ---: | ---: |
| E→E synaptic spike deliveries | 28,601.63 | 4,320.93 |
| LTD operations | 12,409.73 | 2,188.47 |
| LTP operations | 12,188.87 | 2,240.73 |
| Distinct updated synapses within a timestep | 24,555.63 | 4,428.00 |
| Synapses receiving both LTD and LTP in a timestep | 42.97 | 1.20 |

The 30-step update-only working set averages 1,312.80 synapses per PU for
Additive and 255.81 for Morrison. The maximum PU holds 2,490 / 865 distinct
updated synapses, respectively; averaging across PUs hides the source-ownership
imbalance. Including presynaptic reads raises the means to 2,150.46 / 377.92 and
the maxima to 4,518 / 1,529. Capacity is 800 entries per PU, with additional
losses from bounded probing and competition with entries from earlier steps.

Among consecutive updates observed inside the timed window, mean temporal reuse
gaps are 7.50 / 9.64 timesteps, including same-step LTD/LTP pairs as zero gaps.
Only 8.92% / 1.43% of update operations repeat a synapse already updated inside
this window. Excluding same-step pairs, repeated-step gaps average 7.65 / 9.82.
These are conditional means: first touches and updates with no recurrence inside
this short window do not establish a complete long-run reuse distribution.

The cache also benefits from reading a weight during forwarding and updating
it later in the same timestep. Including reads, 31.01% / 26.29% of operations
repeat a synapse touched inside the window; mean temporal gaps between those
repeats are 1.83 / 0.49 timesteps. These are workload reuse statistics, not
predicted cache hit rates. They exclude hits on warmup history.

Per-PU distinct-synapse reuse-distance bounds are also recorded. Between
successive *different-step* update touches, the mean number of other updated
synapses is bounded by 411.78–720.88 for Additive and 67.46–307.84 for Morrison.
The lower bound excludes both endpoint timesteps; the upper includes them.
Including intervening reads changes those bounds to 645.56–1,282.36 and
98.61–563.36. Within-step arrival order is deliberately not invented. These
bounds describe distinct working sets, not the displacement policy's retention.

`*-reuse.json` records all per-step counts, prefix and sliding-window working
sets, PU distributions, gap histograms, and reuse-distance bounds. Additive's
10-step sliding update sets range from 96,181 to 447,861 distinct synapses,
showing the strong dependence on position within its burst. Morrison's range is
37,428–51,245. Thus the selected prefix footprints are not stationary averages.

Reproduce the working-set analysis with:

```sh
python3 datagen/brunel_reuse.py --images <images> --dumps <snapshots> \
  --start 31 --steps 30 --output <reuse.json>
```

The original individual-entry implementation also compacted each scalar writeback
descriptor from 64 to 16 bytes, retaining the 256-byte region and increasing its
capacity from four descriptors to sixteen. This avoids reducing scalar-store
concurrency when an eviction changes from a whole dirty line to one dirty word.
The issued-but-unconsumed notification limit remains twelve; hardware queues,
handler quotas, and margins were unchanged in that version. The current ordered
memory experiment removes these descriptors and uses the transport contract
described above.

Weight-only baseline fills are recovered as the increase in resident-line count
plus evictions: a fill either adds/promotes a resident table line or materializes
a pending eviction. A host test checks this identity against actual requests
after every operation in the original cache's randomized stress workload. The
individual-entry version counts weight fills directly in header word 62. Total
manual loads minus weight fills gives destination/directory loads, which are
checked for equality between cache variants on the identical spike history.

## Generator cache model and hot checkpoints

`datagen/brunel_cache_model.cpp` implements a native functional model, shared by
`datagen/brunel_cache.py`, the image generator, and
`datagen/brunel_cache_sweep.py`. It uses the payload's FP32 arithmetic and Morrison
polynomial. The ordinary independent reference still uses NumPy's power function.
The model reproduces eight-weight forward staging, one shared 32-byte refill,
individual admission, pending materialization before displacement, and ordered
backing-memory effects. It does not predict completion timing or NoC arrival order.

The baseline is checked against the actual C payload through
`sim/tests/brunel_cache_oracle.c`: identical ordered operations must give identical
slots, backing memory, hit/miss counts, pending counts, evictions, stores, forced
materializations, probes, and weight-line requests. The existing cache test also
checks twelve outstanding requests with newest-first completion notifications.
`sim/tests/brunel_cache_model.py` additionally checks every policy against eager
logical weights, including clipping, zero coefficients, partial-line forwarding,
and pending updates, for both learning rules.

Image generation accepts `--cache-warmup N --cache-library <libcache.so>` to model
the final N timesteps of CPU presimulation, including any subsequent active-window
search advancement. N must not exceed `--presim`. Weight addresses are assigned
before this simulation and are independent of the executable and cache policy.
The checkpoint contains neurons/traces/RNG, physical backing weights, resident
clean/dirty weights, and pending coefficients. `initial.npz` contains **logical**
weights; dirty/pending data must not also be eagerly applied to image DRAM.
`cache-initial.npz` records the cache and diagnostic counters. Runtime counters,
inflight bookkeeping, replay records, and local timestep start at zero.

Only policy 1 is exported into executable images in this stage; alternative
policy checkpoints from the sweep are offline artifacts. Their different lookup
rules require a matching payload before they can run on hardware. The 800-slot,
eight-byte table ABI remains unchanged. Policies 3–5 require 200 bytes of packed
two-bit usefulness counters, fitting in the unused writeback-descriptor region
at SPM 0x2280. This preserves the 800-entry capacity and replay/stack allocations.

Hardware `--warmup 0` is supported by both C and assembly payloads. PU1 starts the
timer after the boot barrier, immediately before broadcasting the first forward
phase. The transport self-test drains its notifications before resetting its
request counters, so its traffic is excluded. Validators use the exported initial
occupancy and pending state when computing insertions and applied updates.
`sim/tests/brunel_hot_image.py` verifies the serialized neurons, tags/data,
sidecar, quiescent header, and all 4.05 million reconstructed logical weights.

The deterministic offline event order is source-neuron/row order for forwarding,
then fired-post gid/CSR order (LTD followed by LTP for each connection), then
manual reverse events in forward-delivery order. Different NoC interleavings can
change placement and traffic. Exact parity is claimed for the **same ordered
operations**, not for the offline table and an arbitrary network arrival order.

### Five policies

All policies have 800 slots per PU and full-address keys. Lookup examines all
four legal positions when necessary, without stopping at an empty entry.

1. **Current:** multiply hash, at most four probes across the entire insertion,
   Robin-Hood-like movements; each entry also remains within distances 0–3.
2. **Shifted:** the same policy with `h=((address>>2)<<shift)&1023`, followed by
   `home=h>=800?h-800:h`. The measured shift is 2, making neighboring words'
   four-slot candidate groups disjoint. All 800 slots remain reachable, although
   only 200 are home positions. Folding doubles the address population of 56 of
   the 200 groups relative to the other 144; this is deliberately not described
   as a uniform hash.
3. **Four candidates with usefulness:** one mixed-hash candidate in each of four
   disjoint 200-slot banks, with no relocation chain. A miss decrements all four
   occupied candidates, prefers the first empty candidate, then the first zero,
   and otherwise bypasses admission. Updates are resolved immediately; a bypass
   still loads/applies/writes the modified weight. No learning is dropped.
4. **Current with usefulness:** decrement each occupied slot encountered during
   insertion; immediately evict a zero-usefulness entry as a tombstone. Otherwise
   retain the original Robin Hood movements and total-four-probe insertion cap.
5. **Element bound with usefulness:** policy 4 without the total-probe cap; each
   held element is still limited to distances 0–3. A swap changes the held
   element and its distance. Usefulness travels with moved entries. Each occupied
   encounter decreases the usefulness potential, bounding termination by
   3×800+1 encounters; assertions and insertion histograms check this bound.

Counters initialize to 3 and saturating-increment on each logical tag hit,
including pending-coefficient hits. Internal materialization lookups do not
increment usefulness a second time. Policies 1, 2, 4, and 5 retain one pending
coefficient per absent key; policy 3 resolves new updates immediately.

### Offline comparison

Artifacts are in `copilot/tmp/brunel-cache-20260914/`. Both rules use seed
20260724, 1,000 CPU warmup timesteps and 300 measured timesteps (1001–1300).
Every forward value and updated logical weight is checked bit-for-bit across
all five policies at every timestep. Counts below are system averages per
timestep; refills are 32-byte weight requests and stores are FP32 write requests.
These are functional traffic measurements, not cycle estimates.

| Policy | Additive resident hits | Additive refills/step | Additive stores/step | Morrison resident hits | Morrison refills/step | Morrison stores/step |
|---|---:|---:|---:|---:|---:|---:|
| 1 Current | 20.17% | 1,934.50 | 2,575.07 | 23.01% | 1,858.93 | 2,563.47 |
| 2 Shifted | 26.59% | 1,562.27 | 2,602.72 | 30.74% | 1,452.13 | 2,579.49 |
| 3 Four candidates | 25.03% | 2,025.33 | 2,574.77 | 29.28% | 1,831.80 | 2,565.97 |
| 4 Current + usefulness | 24.39% | 1,702.76 | 2,589.07 | 27.73% | 1,626.50 | 2,568.40 |
| 5 Element bound | 26.86% | 1,565.30 | 2,599.71 | 30.67% | 1,477.80 | 2,577.46 |

Policy 2 reduces refills by 19.24% / 21.88%, with 11.70% / 10.14% more slot probes
and a cheaper hash. Stores change by only +1.07% / +0.62%. Policy 4 reduces probes
by 6.41% / 6.94%, but adds usefulness bookkeeping. Policy 5's longest measured
insertions are 88 / 55 probes; its tail matters even though average traffic is
competitive. Policy 3 bypasses 296.54 / 265.53 admissions per timestep; immediate
update resolution offsets much of its hit-rate improvement in refill traffic.

Resident-hit rates exclude pending matches; tag-hit rates include them. For
policies 1–5, Additive tag hits are 22.26%, 28.15%, 25.03%, 26.10%, 28.30%; Morrison
tag hits are 26.08%, 33.11%, 29.28%, 30.28%, 32.83%. Policy 3 has no pending entries.
The reports separate clean/dirty/pending evictions, bypasses, and write requests,
and include per-PU/timestep distributions and full insertion-length histograms.

The cache is essentially full before measurement, but hit rates still follow
the biological firing pattern. In particular, Additive's measured window spans
quiet intervals and a burst. `warmup.*` shows occupancy and hit rates in 30-step
blocks; `policy-comparison.*` shows the full-window comparison.
`common-active-windows.json` also reports biological steps 1190–1219 for Additive
and 1031–1060 for Morrison, matching the earlier experiment's timed windows.

Build and run the native tools through the Nix shell (Python needs NumPy):

```sh
g++ -std=c++17 -O3 -fPIC -shared -ffp-contract=off \
  datagen/brunel_cache_model.cpp -o copilot/tmp/libcache.so
gcc -O3 -fPIC -shared -fno-builtin -ffp-contract=off \
  sim/tests/brunel_cache_oracle.c -o copilot/tmp/liboracle.so
python3 sim/tests/brunel_cache_model.py \
  --library copilot/tmp/libcache.so --oracle copilot/tmp/liboracle.so
python3 datagen/brunel_cache_sweep.py --rule additive \
  --library copilot/tmp/libcache.so --output copilot/tmp/cache-additive
python3 datagen/brunel.py --rule additive --presim 1189 --cache-warmup 1189 \
  --warmup 0 --timed 30 --payload sim/payloads/sys/brunel_asm.bin \
  --cache-library copilot/tmp/libcache.so --output copilot/tmp/hot-additive
python3 sim/tests/brunel_hot_image.py copilot/tmp/hot-additive
```

### Hot-start simulator pilots

The copied ordered-MemIf 512-PU simulator uses eight host threads, 16 MCs,
32-byte memory lines, 16 KiB SPM and 8 KiB ICache per PU. No RTL or simulator
changes were needed for this stage. Rebuilt C/assembly payloads are 8,040 / 6,316
bytes, both fitting the ICache including the 16-byte image header.

Assembly pilots execute three timed steps with HBM2 timing and no hardware
warmup. Additive starts after CPU step 1189; Morrison starts after step 1030.
Cold controls have exactly the same logical weights and neuron states, with
those weights materialized in backing memory and empty tables.

| Rule | Hot cycles/timestep | Cold cycles/timestep | Hot increase |
|---|---:|---:|---:|
| Additive | 557,570.33 | 408,032.67 | 36.65% |
| Morrison | 333,515.67 | 192,413.00 | 73.33% |

The empty table postpones almost all writebacks and pending-eviction refills
during this short window. Thus hot initialization improves benchmark realism;
it is not itself a cycle-reduction optimization. These three-step pilots
validate startup and illustrate cold-start bias, not long-window performance.

Both rules pass full neuron-state and logical-weight validation: all firing
decisions, refractory counters, RNG states and traces match the reference.
Hot Additive has all 4.05 million logical weights exact and at most
1.90735e-6 mV voltage error. Hot Morrison differs on five weights by one FP32 ULP
at most (3.81470e-6 pA), with exact voltages. One-step C hot-start pilots also
pass, using flat memory (577,837 / 374,549 cycles per timestep); these validate
the C boot path and are not directly comparable to the HBM2 three-step averages.

`pilot-summary.json` contains per-timestep traffic, cache hits and numerical
errors for every run. The per-PU work analyzer also passes pending-conservation
checks starting from the hot checkpoint, including coefficients created before
the hardware run. `validation-status.json` records the complete test status.
