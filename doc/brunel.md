# Sparse Brunel training

`sim/payloads/sys/brunel.c` and the hand-written `brunel_runtime.S` implement
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
**not** reproduce GeNN's graph RNG stream. Per-neuron external input uses a
specified xorshift32/Poisson process shared by the payload and reference.

Each tick forwards the preceding tick's spikes using current logical weights,
integrates all neurons, processes LTD then LTP for each affected synapse, and
increments the pre/post traces. There is no requirement to reproduce GeNN's
within-tick scheduling. Incoming current sums can differ in their FP32 reduction
order. The independent reference applies updates eagerly and evaluates Morrison
with `numpy.power`, without the cache or polynomial implementation.

Additive updates subtract `0.01 × 1.05 × 2JE × post_trace`, add
`0.01 × 2JE × pre_trace`, and clamp to `[0, 2JE]`. Morrison subtracts
`0.1 × 0.1026 × post_trace × w`, adds `0.1 × pre_trace × w^0.4`, and floors at
zero. The payload uses a degree-eight range-reduced polynomial for `w^0.4`;
`datagen/test_brunel.py` checks relative error below 2e-6 over positive FP32
weights, including subnormals.

CPU presimulation establishes neural activity before generating initially empty
PU caches. It is distinct from the **simulated cache warmup**. The default CPU
presimulation is 1,000 steps (100 ms). `--seek-active` advances to a reproducibly
selected active window if the initial 30+30 window is quiet, and records the
actual presimulation duration. Short burst windows are not estimates of the
long-duration population mean firing rate.

## Ownership, addressing, and SPM budget

Neuron `g` belongs to PU `1 + g % 512`, at local index `g / 512`; its MC is
`(g % 512) / 32`. Plastic FP32 weights and forward destination arrays are
contiguous per presynaptic neuron in that neuron's MC. A cache key is the aligned
32-bit **MC-local byte address** of an individual FP32 weight. MC identity is implicit in PU
ownership. Zero is reserved for an empty tag.

Static weights use the existing target-owned CSR scatter. Reverse plastic
connectivity uses neuron scatter: entries carry the owning PU, local presynaptic
neuron index, and that synapse's weight address. Forward and reverse directories
are stored in DRAM and streamed, avoiding a 16-MC directory per neuron in SPM.

| SPM offset | Size | Contents |
| --- | ---: | --- |
| `0x0000` | 256 B | Frozen header, counters, constants; legacy bucket fields stay zero |
| `0x0100` | up to 2,000 B | Up to 25 × 80 B neuron records; actual maximum is 22 |
| `0x0900` | 6,400 B | 800 × 8 B cache entries |
| `0x2200` | 128 B | Four independent 32 B refill buffers |
| `0x2280` | 256 B | Reserved, formerly writeback descriptors |
| `0x2400`–`0x27ff` | 1,024 B | Replay counters, cursor, capacity and spill-region base |
| `0x2800`–`0x2fff` | 2,048 B | 256 arrival records, eight bytes each |
| `0x3000`–`0x37ff` | 2,048 B | SMT 0 stack, descending from `0x3800` |
| `0x3800`–`0x3fff` | 2,048 B | SMT 1 stack, descending from `0x4000` |

The mean is 21.97265625 neurons/PU, requiring 1,757.8125 B of neuron records.
There are 409,600 individual entries across the system: **10.11% of the
4.05 million plastic weights before collisions**. Every occupied entry holds
one useful weight or pending coefficient. The earlier line-entry design had
16.18% nominal capacity, but its sparse pending lanes reduced useful occupancy.

## Cache and pending-update state

Each eight-byte entry contains a word address plus two state bits and one FP32
value. The address's low bits encode clean weight (0), dirty weight (1), pending
LTD (2), or pending LTP (3). A zero tag marks empty; weight addresses are nonzero.
Probe distance is derived from the address and occupied position. The multiply
hash uses the word index and enough high bits to distribute over all 800 homes.

Lookup examines all four allowed positions; deletion needs no tombstone. A new
key takes its home position. Displacements consume the **insertion operation's**
remaining budget, with at most four insertion probes total. The held victim is
evicted when that budget is exhausted or it has no further legal position.
The budget does not restart when the identity of the held synapse changes.

A missing weight can hold one FP32 coefficient and an LTD/LTP kind. No two
updates are combined. A second update to that synapse first loads the weight,
applies the older coefficient, and then applies the new update. Reading a weight
for spike delivery also materializes it first. Pending-only eviction follows
the same rule before issuing dirty word stores. Thus Additive clipping and
Morrison weight dependence are preserved without representing an arbitrary
composition of updates in one scalar.

Presynaptic forwarding inspects up to eight individual entries, then issues
one async 32-byte weight read if any required weight is absent or pending.
Resident cached values override fetched data. Existing pending values are
materialized in place before admission can evict them, preventing double
application and stale-lane reuse. Only initially absent entries are admitted;
an entry evicted during this bounded operation is not immediately reinserted.
A postsynaptic miss admits one pending coefficient. A second update fetches a
line but materializes only the affected synapse. Clean evictions need no write;
dirty evictions issue one acknowledged FP32 store.

SMT 0 exclusively mutates the cache and issues credited memory operations.
SMT 1 consumes completions and incoming spikes. R(0) is eviction/materialization
scratch, R(1) holds forward destinations or spill records, R(2) caches a scatter
directory line, and R(3) holds forward weights. An eight-entry stack staging
array protects pending values during admissions. Refilling entries do not migrate
concurrently. Assembly resolves weights directly into R(3), avoiding the C
caller's extra copy after admission.

Forward transmission occurs in the same invocation when issued memory requests
plus outgoing spikes plus the continuation fit quota 24. Otherwise a continuation
preserves R(1)/R(3) and sends the prepared segment separately. The worst admission
still fits: target + weight + eight victim read/write pairs + continuation = 19.

## Recorded backward replay

Plastic forward messages carry the weight's MC-local address in a2 and
`prePU | preLocal << 16` in a3. Static scatter sets a2/a3 to zero. Each plastic
arrival records `{address, prePU | preLocal << 16 | postLocal << 24}` in eight
bytes. The recording is strictly one timestep; STDP history remains in traces.

After integration, fired excitatory postsynaptic neurons retain full global
reverse scatter, applying LTD to currently firing sources and LTP to sources
with nonzero pre-traces. Recorded arrivals for nonfiring postsynaptic neurons
instead send the existing reverse handler `{preLocal, address, dep*y, 0}`.
Records for fired posts are skipped, including each distinct parallel edge.
Nonfiring posts no longer scan reverse directories by active-MC bitmap.

Each PU reserves 64 KiB in its local MC for record overflow. Once the local
recording fills, the spike handler is moved from SMT1 to SMT0. It can issue two
acknowledged word stores per overflow record and wait for credits while SMT1
continues consuming completions. No local staging queue can fill behind a
completion handler blocked on its own thread. The handler is restored to SMT1
after the finish barrier. Spill replay drains outstanding writes, loads records
one 32-byte line at a time, and applies the same fired-post deduplication.
Generation checks the structural bound of 18 local excitatory neurons, hence
at most 8,100 plastic arrivals per PU per timestep (18 * 450 indegree).

## Transport and progress

The current workload **requires memory effects to follow request order** and
uses the MemIf FIFO issue queue. It does not wait for a matching-line writeback acknowledgment
before issuing a refill, nor serialize same-word stores by acknowledgment.

Neural forward/replay messages and scatter requests/responses use priority 3
(`0xc01`, `0xc04`, and `0xc10`), below priority-2 refills and priority-1 stores.
Bounded control/report traffic stays at priority 0. This ordering is necessary
for progress: the earlier priority-0 replay messages could obstruct the refills
needed to consume those messages, deadlocking the Additive burst at step 32.
Handler IDs, quotas, margins, and queue depths are unchanged by this correction.

Manual line loads use tag `0x800` and words
`{address, (5 << 16) | 0x8002, line_ident, refill_slot}`. Dirty word stores and
spill writes use higher-priority tag `0x401` and words
`{address, (2 << 16) | 0x8003, 0x8000, data}`. Completion handler 0 sets a refill
slot ready or consumes a store notification. No per-write descriptor is needed.

At most 12 manual requests per PU are issued but not yet consumed, reserving
space in the dedicated 16-entry completion queue. The credit bound includes
returned-but-unhandled notifications. SMT0 can wait; SMT1 must consume responses.

Handler quotas/margins are spike 2/2, control 3/5, report 2/4, reverse 10/17,
and work 24/31. Completion and diagnostic marker handlers send nothing. Spike
quota covers overflow stores even when the normal recording path sends nothing.
Phase 1 generates spikes but no learning; phase 3 generates learning but no
spikes. The report tree and cooperative continuations are bounded protocol
traffic, rather than unlimited new work.

Root phase markers use tag 10 addressed through the NoC to PU1's no-op handler.
`SOFT_MNIST_PHASE_LOG=1` exposes their injection cycles using the simulator's
existing marker hook: phases 2/3/4/5 denote forward/integrate/reverse/
finish, and `low` is the timestep. Marker intervals include barrier/control
traffic and differ slightly from local PU start times. The additional sampled
profiler described below observes the rebuilt simulator without target changes.

Global phases use a binary control/report tree. Send counts are accumulated and
compared with receiver counts before advancing past forward or reverse traffic.
A scatter request's acceptance or an MC idle indication is not a delivery fence.
Finish drains manual requests, preserves pending cache entries, and publishes a
completed timestep only after every PU reports completion.

PU 1 runs a 32-byte transport regression at startup: four dirty lines are written
as individual acknowledged words, reloaded into four refill slots, and compared
word for word. The reserved test region starts at MC0 offset `0x8000`.

## Build and run

The compiler ISA is `rv32ia_zmmul_zfinx_zicsr`: the core has multiplication,
not integer division. Recording-index division by 80 is explicitly implemented
with multiply-high arithmetic. This also prevents Os from introducing DIVU.
The C target uses size-controlled O3 for arithmetic/cache kernels and explicit
Os for dispatch, reporting, and recording. The hand-written assembly target
contains no compiled C. Both linkers require the entire payload plus the image
header to fit the preloaded 8 KiB ICache. The C implementation remains available
as a reference; `brunel_asm_layout.inc` names the frozen header/record ABI offsets.

The FIFO MemIf experiment uses the regenerated 512-PU configuration and copied
simulator under `copilot/tmp/brunel-ordered-20260914/design/`. The earlier frozen
simulator under `copilot/tmp/brunel-replay-20260914/frozen/` remains preserved as
historical evidence, including the priority deadlock diagnostics. The following
commands create the required 512-PU simulator configuration:

Run project commands inside the Nix development shell. The Python interpreter
used for generation and validation must provide NumPy. If the development
shell's Python lacks it, enter a nested
`nix-shell -p 'python3.withPackages (ps: [ps.numpy])'` for the Python commands.

```sh
JAVA_TOOL_OPTIONS=-Xmx12g mill ActiveN.run --system --pu=512 --mc=16 --mem-bus-width=256 --icache-lines=128
cmake -S sim -B sim/build -G Ninja -DENABLE_HARD_BACKEND=OFF -DUSE_BOLT=OFF
ninja -C sim/build -j8 sim_system
make -C sim/payloads sys/brunel.bin sys/brunel_asm.bin

python3 datagen/brunel.py --rule morrison --presim 1000 --warmup 30 --timed 30 \
  --output copilot/tmp/brunel-morrison
mkdir -p copilot/tmp/brunel-morrison-run
SOFT_BRUNEL_DUMP=copilot/tmp/brunel-morrison-run sim/build/sim_system \
  --soft --soft-threads 8 --pu 512 --mc 16 --mc-size 0x100000000 --init-icache \
  --ramulator-config sim/build/ramulator-hbm2-1ch.yaml \
  --ramulator-stats copilot/tmp/brunel-morrison-run/ramulator \
  --max-cycles 100000000 copilot/tmp/brunel-morrison/dram.{0..15} \
  > copilot/tmp/brunel-morrison-run/run.log 2>&1
python3 datagen/brunel_validate.py --images copilot/tmp/brunel-morrison \
  --dumps copilot/tmp/brunel-morrison-run \
  --log copilot/tmp/brunel-morrison-run/run.log \
  --output copilot/tmp/brunel-morrison-run/report.json
```

For Additive, change the rule/output names and add `--seek-active` when seeking
an active cache measurement. The `hbm2-1ch` configuration has 32-byte transactions;
the default `ramulator.yaml` has 64-byte transactions and is incompatible with
this elaboration. `--soft` simulates the freshly elaborated RTL cores with the
software NoC/MemIf and Ramulator memory timing; it is not full-system hard RTL.

The opt-in observer writes all PU runtime state at each completed timestep and
backing DRAM at termination. Validation reconstructs logical weights from DRAM,
resident cache lines, and pending coefficients, using the runtime polynomial
for pending Morrison LTP. That polynomial reconstruction is checked bit for bit
against the C helper; the independent expected weights use `numpy.power`.
Validation compares all neuron states at every completed timestep, cross-checks
saved reference checkpoints at steps 1, 30, and 60, and checks all 4.05 million
final plastic weights. Reports include
per-step resident hit rates, resident/pending occupancy, and weighted hit rates
for three consecutive ten-step timing blocks. The tag hit rate additionally
counts pending-only entry matches; new tag insertions are derived from occupancy
growth plus evictions. The resident hit rate requires an actual buffered weight,
so a pending-only tag match is still a resident miss. A demand is one forward
synapse lookup or one nonzero coefficient application to a synapse; these counters
are not DRAM transaction hit rates. Cache warmup should be judged
from these trends, not merely from the number of elapsed timesteps.

Host regression commands:

```sh
cc -std=c11 -D_GNU_SOURCE -O2 -fno-builtin -ffp-contract=off \
  sim/tests/brunel_cache.c -o copilot/tmp/brunel-cache-test
copilot/tmp/brunel-cache-test
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s datagen -p test_brunel.py
```

The cache stress test runs actual runtime code against eager uncached updates
with intentionally reordered memory completions, exercises eviction and pending
materialization, and checks all backing words after draining writebacks.

`make -C sim/payloads sys/brunel_asm_numeric.bin` builds an opt-in assembly
startup self-test. Generate its images with `--payload` as usual. It compares
16 polynomial and 12 update/clipping vectors against C-derived FP32 golden
words, then runs the ordinary network; the normal assembly binary omits it.
The C compiler stack audit reports a largest individual frame of 128 bytes;
the deepest cache/refill call chain remains below 400 bytes, within each 2 KiB
stack allocation.

`SOFT_BRUNEL_PROFILE=1`, together with `SOFT_BRUNEL_DUMP`, enables observation
without adding target instructions. `pu-profile.csv` samples activity, stalls,
queue pressure, and outstanding requests every 256 simulated cycles, grouped
by PU, timestep, and global phase. `pc-profile.csv` samples each active thread's
fetch PC over the timed interval; function attribution is approximate because
of pipeline lookahead. `mc-profile.csv` records per-cycle MSHR occupancy and
accepted/presented memory requests. A profile-on/off smoke comparison checks
identical simulated timing, SPM snapshots, and final DRAM.

`datagen/brunel_hotspots.py` reports work per PU and timestep. It independently
reconstructs nonzero LTD/LTP demands from spike/trace state, and checks each PU
against `hits + misses - forward weight reads`. It also counts arithmetic
applications using pending-entry conservation:
`applied updates = new update demands - change in pending coefficients`.
This includes materializing older coefficients during reads or evictions.
Distinct affected synapses, new demands, and applied arithmetic are separate
quantities; clipping or FP32 rounding can leave a weight numerically unchanged.

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
