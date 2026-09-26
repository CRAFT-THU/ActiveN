# Sparse Brunel training

## Final performance and macroscopic check (2026-09-26)

**Selected configuration: consecutive home G4 for both rules; seed 20260724.**
The final benchmarks use 512 PUs, 16 MCs, two SMT threads, 32-byte memory
lines, hot I-cache, eight simulator host threads, flat memory, and disabled
payload diagnostics. Each has **60 warmup + 1,000 timed steps** at 0.1 ms/step.
Additive starts after 1,120 presimulation steps and measures physical time
**118–218 ms**; Morrison starts after 1,000 and measures **106–206 ms**.

| Final rule | Integration | Forward + quiet LTD | Post-active LTD/LTP | Drain | Total cycles/timestep |
| --- | ---: | ---: | ---: | ---: | ---: |
| Additive | 1,693.114 | 2,380.411 | 1,702.191 | 638.692 | **6,414.411** |
| Morrison | 1,742.243 | 4,262.263 | 2,705.465 | 644.491 | **9,354.465** |

The total includes three timer bookend cycles across the entire timed window.

**Exact-window GeNN comparison.** The final check reused the preserved CUDA
baseline binaries on CRAFT-A100, with the same model parameters and seed,
and collected the exact intervals above. Both reruns reproduce the saved
baseline’s complete 10,000-step firing and structural traversal counters.
Weight moments below cover **all 4,050,000 plastic weights** on both sides,
in original pA units; ActiveN stored weights are divided by
`epsc * delivery = 37.30973184964796`. Standard deviations are population
standard deviations. Percentage differences use GeNN as denominator.

| Metric | Additive ActiveN | Additive GeNN | Difference | Morrison ActiveN | Morrison GeNN | Difference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| E firings/timestep | 3.243 | 5.059 | -35.90% | 5.108 | 5.271 | -3.09% |
| I firings/timestep | 0.814 | 1.256 | -35.19% | 1.322 | 1.332 | -0.75% |
| Mean E neuron rate, Hz | 3.6033 | 5.6211 | -35.90% | 5.6756 | 5.8567 | -3.09% |
| Mean I neuron rate, Hz | 3.6178 | 5.5822 | -35.19% | 5.8756 | 5.9200 | -0.75% |
| Plastic forward deliveries/timestep | 1,458.773 | 2,278.252 | -35.97% | 2,298.146 | 2,371.702 | -3.10% |
| Plastic reverse traversals/timestep | 1,459.350 | 2,276.550 | -35.90% | 2,298.600 | 2,371.950 | -3.09% |
| Quiet nonzero-trace updates/timestep | 828.258 | 1,437.163 | -42.37% | 1,458.718 | 1,455.162 | +0.24% |
| Post-active nonzero-trace updates/timestep | 846.212 | 1,467.252 | -42.33% | 1,468.926 | 1,466.025 | +0.20% |
| Total nonzero-trace updates/timestep | 1,674.470 | 2,904.415 | -42.35% | 2,927.644 | 2,921.187 | +0.22% |
| Final weight mean, pA | 45.592928 | 45.586325 | +0.01% | 45.584923 | 45.587583 | -0.01% |
| Final weight standard deviation, pA | 0.429724 | 0.503607 | -14.67% | 0.197111 | 0.190241 | +3.61% |

An update here counts an edge once in a timestep if an LTD or LTP term has
a nonzero prior partner trace; quiet and post-active sets are disjoint.
GeNN opportunities are reconstructed from its recorded spikes and actual
graph using the same definition, including parallel edges and excluding
zero-lag trace increments. The same reconstruction exactly reproduces both
ActiveN runs’ saved update counts. This measures learning work, not GPU
kernel invocations, two separate counts for LTD and LTP on one edge, or
weights whose FP32 bits changed. Counts are associated with firings in the
stated interval; GeNN executes the corresponding synapse kernels on the
following step. Weight moments are measured at each stated endpoint.

**Assessment: Morrison agrees closely over the final window; the final
Additive window does not by itself establish close macroscopic agreement.**
Morrison differs by −3.09% in E firing, −0.75% in I firing, +0.22% in update
work, −0.002659 pA in mean weight, and +3.61% in weight spread. Additive has
−35.90% E firing, −35.19% I firing, −42.35% update work and −14.67% weight
spread, despite its close mean weight (+0.006603 pA). Its lower benchmark
cost therefore must not be presented as a measurement with GeNN-matched
activity, or as a one-second steady-state result.

Additive is strongly bursty: the ten nonoverlapping 100-ms windows of this
GeNN baseline range from **2.060 to 7.404 E firings/timestep**, encompassing
the final run’s 3.243. This supports short-window variability as a plausible
explanation; it is not an equivalence test or proof that the final trajectory
will converge. The implementations use different graph/noise realizations
even with the same numeric seed.

**Longer validation remains supporting evidence, not a new G4 measurement.**
The previously validated, ungrouped V3 runs cover physical 100–1,100 ms
(10,000 steps) for both rules. Comparing those saved ActiveN trajectories
with the reproduced GeNN baseline gives:

| Earlier 10,000-step validation | Additive ActiveN | Additive GeNN | Difference | Morrison ActiveN | Morrison GeNN | Difference |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| E firings/timestep | 4.1404 | 3.9755 | +4.15% | 5.1454 | 5.3597 | -4.00% |
| I firings/timestep | 1.0404 | 1.0141 | +2.59% | 1.3221 | 1.3492 | -2.01% |
| Nonzero-trace updates/timestep | 3,256.718 | 3,229.265 | +0.85% | 4,241.668 | 4,403.083 | -3.67% |
| Final weight mean, pA | 45.477458 | 45.476285 | +0.00% | 45.460167 | 45.453724 | +0.01% |
| Final weight standard deviation, pA | 1.036851 | 1.013320 | +2.32% | 0.438639 | 0.449642 | -2.45% |

The new GeNN weight statistics use the full population from the preserved
A100 binaries; older tables below used the original 100,000-weight samples.
The earlier five-seed Morrison comparison also found only +0.068% E and
+0.144% I activity in the ActiveN ensemble, with closely matching weight
moments. These results support broad macroscopic agreement of the learning
implementation. Grouped Morrison preserves all firing identities over the
1,060-step overlap with V3; all tested final grouped layouts preserve their
matched trajectories and final weights. Additive’s final checkpoint differs
from the old long run, so its old statistics cannot be relabeled as G4 results.

The optimization and configuration selection are recorded as complete, with
the final numerical checks passing. A blanket claim that **both final
1,000-step benchmarks** match GeNN’s macroscopic activity is not supported.
A direct long-duration check of the final Additive G4 configuration would be
needed to remove that qualification; the final cycle figures remain valid
for their explicitly measured activity and flat-memory conditions.

Reproducibility: the final payload SHA-256 values are
`101154d6eb73b056994b61601dcb3d54ea4e33b24b2d653bb97e4f39e524a665`
(Additive) and
`450c86e6a633cc40b1914e4e7a665763e8773eb04f3f7c8db52f7e1046ed1bc3`
(Morrison). Final performance sources are the dated configuration section
below. This check’s scripts, baseline/build identities, complete comparison
JSON and independent count checks are in
[`copilot/tmp/brunel-final-macros-20260926/`](../copilot/tmp/brunel-final-macros-20260926/);
GPU spike/graph diagnostics are preserved at the matching directory under
`/data/meow/ActiveN-stdp/` on CRAFT-A100. Earlier long and seed validation
sources are `copilot/tmp/brunel-v3-long-20260919/both-comparison.json` and
`copilot/tmp/brunel-morrison-seeds-20260920/ensemble-comparison.json`.

The hand-written `sim/payloads/sys/brunel_runtime.S` implements
Additive and Morrison training on
512 PUs, 16 MCs, two SMT pipes, 16 KiB SPM per PU, and 256-bit memory lines.
`datagen/brunel.py` generates the images and an independent NumPy FP32 reference;
`datagen/brunel_validate.py` compares simulator snapshots against that reference.
This is a separate payload; it does not change the existing MNIST formats.

## Final configuration and completed optimization (2026-09-25)

Use **consecutive groups of four PUs with home-owned post-active learning**
for both Additive and Morrison:
`--proxy-groups consecutive --proxy-size 4 --proxy-updates home`.
This concludes the current Brunel optimization campaign. It is the best common
configuration supported by the tested layouts and windows, and has the lowest
cost among the completed long-run configurations for each rule. Morrison's
short pilot was only 0.19% behind distant home G8; the missing longer comparison
now favors consecutive home G4 by **3.64%**. A separate Morrison G8 recommendation
is therefore unnecessary for these workloads.

Both final benchmarks use **60 warmup and 1,000 timed steps**, the same frozen
512-PU/16-MC simulator, 32-byte memory lines, two SMT pipes, hot I-cache and flat
memory. Payload diagnostics are **off**; passive simulator profiles are on.
Seed 20260724 and each rule's starting checkpoint match the earlier comparisons:
Morrison presim 1000, Additive presim 1120. The new Morrison benchmark ran as
one eight-host-thread job, without rebuilding the simulator or changing the
payload. The Additive row reuses its completed matching long run.

Average **cycles per timestep**:

| Rule, consecutive home G4 | Integration | Forward + quiet LTD | Post-active LTD/LTP | Drain + trace increments | Total |
| --- | ---: | ---: | ---: | ---: | ---: |
| Morrison | 1,742.243 | 4,262.263 | 2,705.465 | 644.491 | **9,354.465** |
| Additive | 1,693.114 | 2,380.411 | 1,702.191 | 638.692 | **6,414.411** |

Totals include three timer bookend cycles over the complete timed interval.
Morrison uses 353.111 fewer cycles/timestep than distant home G8's 9,707.576:
forward costs 290.494 more, but post-active processing saves 643.569, with
integration and draining nearly unchanged. It also uses 6.68% fewer cycles than
distant relay G4's 10,024.368. These compare complete configurations, including
both group size and placement; they are not an isolated placement measurement.

Additive uses 5.60% fewer cycles than distant home G4, 3.75% fewer than distant
relay G4, and 8.60% fewer than distant replicated G4. All percentages use the
alternative's cycle count as denominator. Its quieter long window explains the
lower absolute cost than the active pilot; the compared layouts have identical
activity within each window. The full report includes 100-step block comparisons,
so a favorable whole-window mean is not a claim of winning every short interval.

Average **events per timestep** for the final configurations:

| Rule | E firings | I firings | Plastic deliveries | Static deliveries | Quiet LTD updates | Post-active updates | Remote post-active loads |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Morrison | 5.108 | 1.322 | 2,298.146 | 1,313.413 | 1,458.718 | 1,468.926 | 0 |
| Additive | 3.243 | 0.814 | 1,458.773 | 820.471 | 828.258 | 846.212 | 0 |

The new Morrison run has identical firing identities, refractory state,
learning-trace words and worker RNG state to distant home G8 throughout all
1,060 steps. All 4,050,000 final logical weights match bitwise, including the
saved relay control. Maximum voltage difference is 5.7220459e-6 mV. Independent
neuron, recurrent-delivery and canonical-learning checks pass; the maximum
weight discrepancy against the independent oracle is 0.00036621 in scaled
units. Every measured MSHR read matches the independently reconstructed step,
PU, aligned address and multiplicity. The first 119 steps also reproduce all
476 comparable phase timings of the consecutive home G4 pilot exactly.
The preceding Additive long comparisons likewise preserve firing, traces, RNG,
refractory state and all final weights. These gains do not come from reduced
spiking or omitted learning work.

The common design keeps neuron states and authoritative traces at their home
PU, divides each outgoing row into four balanced proxy shares, and uses the
proxies for delivery. Quiet postsynaptic receivers update weights immediately;
post-active learning runs at the presynaptic home. Consecutive G4 groups stay
inside one MC zone, so their home-owned MSHR refills are local. Group control,
jobs and descriptors occupy 3,232 bytes/PU rather than G8's 6,400 bytes/PU.
In the matched Morrison window, G4 also reduces notifications from 4,096.360
to 2,048.180 per timestep and plastic copy traffic from 41,844.736 to 20,961.280
bytes per timestep.

Mean refill request-to-response latency falls from 379.136 to 8.606
cycles/request in this Morrison comparison. All presented external memory
requests are accepted by the flat frontend, so this measures the benefit of
local response transport and queueing; it excludes completion-handler scheduling.

The preceding experiments establish the other final choices:

- Persistent hash caching was replaced by streaming delivery and immediate
  learning; the runtime has no persistent weight cache or forward weight buffer.
- The four phases are integration, forward delivery plus quiet LTD, post-active
  LTD/LTP, and draining plus trace increments. Assembly, shared constants,
  dynamic SMT integration, two-SMT spike receipt, 2-KiB ping-pong copies and the
  validated iterative Morrison power approximation remain in use.
- Home learning avoids replicated trace maintenance (about 508 integration
  cycles/timestep at G4) and the relay variant's extra handler/message dependency.
  The 16 MSHRs and 12-notification credit bound remain independent of the
  ping-pong area. G8's extra notification, directory and padded-copy traffic did
  not justify retaining a separate recommended configuration.

Select the configuration explicitly: compatibility CLI defaults and experimental
variants remain available. Matching binaries are
`sys/brunel_additive_proxy_home_g4.bin` and
`sys/brunel_morrison_proxy_home_g4.bin`. For example, from the repository root:

```sh
nix develop --command bash -c '
  make -B -C sim/payloads \
    sys/brunel_additive_proxy_home_g4.bin \
    sys/brunel_morrison_proxy_home_g4.bin BRUNEL_ASM_FLAGS=-DBR_DIAGNOSTIC=0
  python3 datagen/brunel.py --rule morrison --seed 20260724 --presim 1000 \
    --warmup 60 --timed 1000 --proxy-groups consecutive --proxy-size 4 \
    --proxy-updates home --output copilot/tmp/brunel-morrison-home-g4
  python3 datagen/brunel.py --rule additive --seed 20260724 --presim 1120 \
    --warmup 60 --timed 1000 --proxy-groups consecutive --proxy-size 4 \
    --proxy-updates home --output copilot/tmp/brunel-additive-home-g4
'
```

Run the resulting sixteen images with the preserved 512-PU simulator using
`--soft --pu 512 --mc 16 --mc-size 0x100000000 --soft-threads 8 --init-icache
--max-cycles 100000000`.
The exact measured invocation, environment, hashes, validation and comparisons
are in `copilot/tmp/brunel-final-20260925/results.md` and its `collected/` directory.
Raw snapshots remain under `/data/meow/ActiveN-stdp/brunel-final-20260925/` on
CRAFT-A100. The simulator host run took about 49.8 minutes; this is separate
from the reported simulated cycles per timestep.

This configuration conclusion applies to the tested checkpoints and flat-memory
frontend; it does not establish an optimum for every seed or a detailed HBM
timing model. The prior multi-seed GeNN comparison remains the macroscopic
dynamics evidence, while the grouping experiments establish matched workload
and numerical preservation over the benchmark windows. Earlier dated sections
below retain the measurements and interpretations at each stage of the campaign.

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

Runtime traces now include their learning coefficients: `X = pot * pre_trace`
and `Y = dep * post_trace`. They decay by the same factors; firing adds `pot`
or `dep` instead of one. Boot converts excitatory canonical traces once and
zeros inhibitory traces, which are unused by the E-to-E plastic graph.
Quiet LTD consumes Y directly, and reverse receivers use X directly, removing
per-update coefficient multiplications. The image and `initial.npz` retain
unscaled traces; manifests identify the runtime with `trace_state: learning-scaled`.
Reverse scatter's fourth argument is unused and need not be initialized.
Manifests declare `trace_population: excitatory-only` and
`inhibitory_trace_state: zero`; initial images retain canonical traces for all
neurons, while completed runtime snapshots must have zero inhibitory traces.

This representation changes FP32 rounding history. Validation keeps canonical
traces and weights independently throughout the run, converting only the
comparison copy to runtime units. It never rescales observed traces into its
learning oracle. A 20,000-step synthetic stress test, including refractory-limit
firing, found at most 5.75e-6 normalized coefficient error; its largest weight
difference was 0.03834 in scaled units (1.13e-5 of the weight range).
Replay of 949 recorded timesteps across all 4,050,000 weights found a maximum
difference of 0.00036622 for either learning rule. These are measured error
bounds for those cases, not a universal few-ULP equivalence claim. The existing
simulator validator's 0.02 absolute weight limit is unchanged.

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

## Proxy synapse ownership (updated 2026-09-25)

The optional `--proxy-groups consecutive|distant --proxy-size 2|4|8` layout
separates neuron ownership from outgoing-synapse ownership. Home PU IDs
remain `1 + gid % 512`; membrane/current state, RNG, integration and
postsynaptic traces stay in their original 80-byte home records.

Consecutive groups contain adjacent PU IDs. Distant groups contain
`i + k * (512 / G)`, for `i=1..512/G` and `k=0..G-1`. Each neuron's plastic
and static rows are independently divided into shares differing by at most
one synapse. The deterministic placement heuristic prefers nearby targets
under these quotas, using Hilbert coordinates and Manhattan distance.
Plastic weights reside in the selected proxy's MC, in the existing
128-weight/128-target, 1-KiB chunk format. Static weights retain target-MC
CSR scatter, with a directory per proxy shard. Quiet postsynaptic recipients
perform LTD and write directly to the originating MC with no receipt.

`--proxy-updates` selects the post-active learning owner at compile time:

- `replicated`: reverse CSR entries name the synapse proxy, its descriptor
  offset and the full MC-local weight address. Each proxy maintains a copy
  of its group neurons' learning-scaled pre trace. Both integration workers
  decay these copies with the home's exact FP32 recurrence, and firing adds
  the same increment at the end of the timestep. The proxy uses its local
  MC and acknowledged scalar writeback, as in the original four-PU pilot.
- `home`: reverse CSR entries name the presynaptic neuron owner. Their
  16-bit index is `neuronSpmOffset | weightMC`, using the four spare low bits
  of the 16-byte-aligned neuron record offset. The full 32-bit MC-local
  weight address is unchanged. The handler reads X and the firing flag from
  its home neuron, issues the manual scalar refill to the encoded remote MC,
  and stores that MC destination at MSHR offset 52. Retirement applies the
  same LTD/LTP arithmetic and sends a no-receipt FP32 write directly to that
  MC. Proxy X words remain zero; proxy trace initialization, decay and
  increment code are absent. Thus this comparison also removes post-active
  store acknowledgements, in addition to trace replication and concentrated
  reverse ownership.
- `relay`: reverse CSR entries also name the presynaptic neuron owner, but
  the spare low nibble stores the weight owner's group rank instead of its
  MC. The home handler reads its authoritative X/firing state, retains the
  carried postsynaptic Y coefficient only when pre fired, and skips the
  event if both coefficients are zero. Otherwise it sends one targeted
  message to the weight owner: `a0=full MC-local weight address`, `a1=LTD
  coefficient`, `a2=LTP coefficient`; the fourth word is unused. The proxy
  performs the ordinary local MSHR refill and identical arithmetic, followed
  by a local no-receipt write. Proxy trace words remain zero. No postsynaptic
  ID, presynaptic ID or runtime CSR search is needed at the proxy because
  the event carries the exact address and both learning coefficients.

Relay mode acknowledges a useful reverse event only after proxy MSHR
allocation; zero-effect events are acknowledged at home. Thus each original
reverse CSR entry contributes exactly one count, and the existing reverse
barrier includes relay packets in transit. Final draining still waits for
MSHR retirement before incrementing home traces. The receiver uses handler 4
on SMT0 with its existing quota/margin (10/17). Home trace requests use handler
7 on SMT1 (quota 1, margin 18), overlapping proxy work while leaving room for
higher-priority handlers. Both threads increment the reverse-event counter
atomically; the relay reads immutable home state throughout the reverse phase.
Notifications retain handler 5 on SMT1 and completion retains handler 0.
Relay-mode tags form the dependency order `0xc07` scatter -> `0x804` relay ->
`0x400` scalar read -> `0x002` no-receipt write. Memory completions use the
unchanged dedicated queue and 12-request credit bound. These priority changes
are specific to relay payloads; home and replicated binaries are unchanged.

The home of a fired postsynaptic neuron initiates reverse scatter in all three
variants. A quiet presynaptic neuron can still receive LTP: home and relay
variants read its authoritative trace, and the replicated variant decays all
group E traces every timestep, not just those that recently fired.

After integration, each home sends firing-job offsets to all group members;
the replicated variant also carries the decayed pre trace. The last job
carries the sender's total job count, and a silent home sends an empty end
marker. SMT1 receives jobs without sending messages. SMT0 waits for all G
markers and their aggregate job count, without assuming arrival order.
Eight-member notifications use two invocations of at most four sends plus
one continuation, preserving the existing quota of five and margin of eight.
The job flag is forward-work bookkeeping in home and relay variants, not learning
trace state. The same global barriers and four measured phases remain.

Group control uses 64 bytes at `0x980`; up to `22*G` job pointers start at
`0xa00`, and 32-byte descriptors start at `0xd00`. The allocations total
1,648 / 3,232 / 6,400 bytes for G=2/4/8, excluding alignment gaps. Even G=8
ends at `0x2300`, below the unchanged MSHR region at `0x2900`. The 16 MSHRs,
2-KiB ping-pong area and two 128-byte stacks remain separate. At G=16 the
current descriptor format does not fit alongside these required regions.

Build `sys/brunel_<rule>_proxy_<replicated|home|relay>_g<2|4|8>.bin` with
`BRUNEL_ASM_FLAGS=-DBR_DIAGNOSTIC=0`. Datagen chooses this filename from the
three proxy arguments. Legacy `brunel_<rule>_proxy.bin` targets alias the
replicated G=4 binary. Ordinary targets retain ungrouped operation. Boot
checks a magic encoding both the group size and learning owner, so mismatched
images fail rather than silently using another variant.

`initial.npz` records physical weight addresses/owners, static owners, group
members and descriptor slots. Image checks cover every plastic target,
static entry, reverse route/MC/address, row balance and unique weight storage.
Runtime validation checks notification completion, exact proxy job flags,
replicated trace equality or absent home/relay proxy traces, and the independent
neuron/delivery/learning oracle. No RTL or simulator rebuild is required.

### Trace-relay comparison (2026-09-25)

Eight relay configurations and the previously missing consecutive Morrison
home G4 control passed four-step smoke tests and matched 60-warmup/60-timed
pilots. These use the preceding experiment's simulator, checkpoints and build
settings: diagnostics off, passive profiles on, hot I-cache and flat memory.
All twelve existing home/replicated binaries are byte-identical to the previous
experiment, so the earlier measurements remain valid controls.

Average cycles per timestep for the relay configurations:

| Placement / group | Morrison | Additive |
| --- | ---: | ---: |
| Distant G2 | 11,153.417 | 13,430.483 |
| Distant G4 | **9,999.617** | **12,015.300** |
| Distant G8 | 10,226.883 | 12,204.333 |
| Consecutive G4 | 10,402.383 | 12,235.483 |

Distant G4 wins the relay search for both rules. Morrison relay improves on
the best tested replicated pilot by 2.63%, but costs 5.63% more than distant
home G8. Additive relay costs 1.34% more than distant replicated G4 and 10.08%
more than consecutive home G4. At the same distant G4 placement, Additive relay
does beat home updates by 0.74%; that is not a win over the best home placement.

The new consecutive Morrison home G4 control costs 9,484.117 cycles/timestep,
with phases 1,742.417 / 4,419.250 / 2,680.767 / 641.633. It is only 0.19% slower
than distant home G8, so the prior measurements do not establish a strong
Morrison preference for distant placement under home-owned learning.

All nine pilots have identical firing identities, refractory states, worker
RNG states and all 4,050,000 final logical weights to their matched controls.
Maximum voltage difference is 1.1444092e-5 mV. Independent image routing,
reverse-event accounting, neuron, delivery and learning checks pass. Every
measured MSHR read's step, PU, aligned address and multiplicity also matches an
independent graph/state reconstruction. Proxy trace words remain zero.

Relay removes the G4 replicated-trace maintenance cost of about 508 integration
cycles/timestep, but adds a handler and message dependency for each useful
post-active update. It retains the proxy layout's concentration of learning:
the distant G4 pilots average 11.867 / 17.983 updates at the busiest PU for
Morrison / Additive, versus 8.767 / 15.450 with home-owned learning. There are
1,218.050 / 2,922.950 relays per timestep, of which 912.050 / 2,185.250 cross
between PUs. Local refills alone therefore do not establish an overall benefit.

Both selected distant G4 relays completed their 60-warmup/1,000-timed follow-ups.
Average cycles per timestep:

| Rule | Integration | Forward + quiet LTD | Post-active | Drain | Total |
| --- | ---: | ---: | ---: | ---: | ---: |
| Morrison | 1,742.319 | 4,130.603 | 3,511.221 | 640.222 | 10,024.368 |
| Additive | 1,693.589 | 2,317.548 | 2,013.287 | 639.807 | 6,664.234 |

Additive relay costs 5.04% less than distant replicated G4 and 1.92% less than
distant home G4, but 3.89% more than consecutive home G4. Compared with
replication, it saves 508.084 integration cycles and adds 167.116 post-active
cycles per timestep. The quieter
long window makes the fixed trace-maintenance saving more valuable than in the
active pilot; the gain does not come from fewer spikes or learning updates.
For example, in the quiet timed steps 201–300, relay takes 5,027.190 versus
5,538.330 cycles/timestep with replication. In the busy timed steps 601–700,
relay takes 14,997.260 versus 14,436.660; the performance ordering depends on
activity even though the two firing trajectories are identical.

Morrison relay costs 3.26% more than the prior distant home G8 long run. The
increase is 158.834 forward and 162.187 post-active cycles/timestep, offset by
small changes in integration/draining. These are different selected group
sizes, so this is a comparison against the best previous long-run configuration,
not an isolated measurement of the relay handler's cost. The matched G4 pilots
above establish the same-placement comparison.

Across all 1,060 steps, both rules' firing identities, refractory state, trace
words, worker RNG state and all 4,050,000 final logical weights match their saved
controls exactly. Maximum voltage differences are 4.7683716e-6 mV for Morrison
and 8.5830688e-6 mV for Additive. Starting numerical state also matches exactly,
including pending excitatory/inhibitory input accumulators. Every measured
MSHR read route/count passes the independent check throughout both long runs.

Long-run averages per timestep:

| Rule | E firings | I firings | Plastic deliveries | Static deliveries | Quiet LTD updates | Post-active updates / relays | Cross-PU relays |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Morrison | 5.108 | 1.322 | 2,298.146 | 1,313.413 | 1,458.718 | 1,468.926 | 1,102.487 |
| Additive | 3.243 | 0.814 | 1,458.773 | 820.471 | 828.258 | 846.212 | 634.035 |

All relay weight refills use the receiving proxy's local MC. Mean source-router
acceptance to response-arrival latency is 8.058 / 8.027 cycles per request for
Morrison / Additive, with a 95th percentile of 11 for both. All presented external
memory requests are accepted by the flat frontend. These measurements do not
include completion-handler scheduling latency or establish HBM bandwidth usage.

The relay is useful for Additive with distant G4 placement in the longer window,
but neither rule's relay beats the best previously measured home configuration.
It remains an explicit experimental selection; defaults are unchanged. Full
phase tables, profile limits, provenance and raw-result locations are in
`copilot/tmp/brunel-relay-20260925/results.md`.

### Group-size and learning-owner sweep (2026-09-24)

All twelve distant-group cases passed four-step smoke tests and matched
60-warmup/60-timed pilots. Diagnostics were disabled, using the preserved
512-PU simulator, hot I-cache, flat memory, and at most two eight-thread jobs.
Seed 20260724, graph, initial weights and numerical neuron state match within
each rule; Morrison uses presim1000 and Additive the prior presim1120 checkpoint.

Average cycles per timestep:

| Group size | Morrison replicated | Morrison home | Additive replicated | Additive home |
| ---: | ---: | ---: | ---: | ---: |
| 2 | 11,019.37 | 10,724.87 | 12,745.75 | 12,940.38 |
| 4 | 10,269.88 | 9,524.82 | **11,856.70** | 12,104.80 |
| 8 | 11,312.73 | **9,466.30** | 12,714.02 | 12,262.70 |

The selected cases reduce cost by 28.23% for Morrison and 17.86% for Additive
relative to the matched ungrouped pilots. Morrison home G8 only beats G4 by
0.61%, while doubling much of the per-shard traffic and increasing per-PU
group storage from 3,232 to 6,400 bytes. These are measured choices among
G=2/4/8, rather than a claim of a universal optimum. Both selected configurations
passed follow-up 60-warmup/1,000-timed benchmarks. An additional consecutive
Additive home G4 pilot and two matched Additive home-owner long runs also
passed, bringing the total to seventeen benchmarks.

All twelve pilots have identical firing identities, refractory states, worker
RNG states and all final logical weights to the corresponding ungrouped run.
The largest voltage difference is 1.1444e-5 mV. Independent neuron, recurrent
delivery, canonical learning and transport checks pass. The full phase table
and traffic statistics are in `copilot/tmp/brunel-groups-20260924/results.md`.

Trace replication costs about 508 integration cycles/timestep at G4 and 1,001
at G8. Home ownership removes this cost and spreads post-active arithmetic
back across the original presynaptic owners. It also requires remote line
responses: G4 mean refill request-to-response latency is 158 cycles for
Morrison and 760 for Additive, versus about 8.2 for proxy-local reads. All
presented flat-memory frontend requests are accepted; fulfilled MemIf slots
accumulate, locating the observed waiting after external memory completion.
This does not establish HBM2 bandwidth utilization. The home variant also
removes store receipts, so these are comparisons of the complete requested
designs, not an isolation of trace maintenance alone.

G8 halves the average outgoing share again, but fixed 1-KiB chunks contain more
padding and notification/directory work grows. Morrison home G4 to G8 reduces
the average per-step busiest outgoing count from 168.58 to 93.70, yet forward
cost only falls from 4,229.72 to 4,034.03 cycles/timestep. Plastic copy bytes
double from 20,889.60 to 41,779.20 per timestep. The incoming spike workload
is unchanged. These costs limit further benefit from larger groups.

Completed long runs use 60 warmup and 1,000 timed steps. All numbers below are
average cycles per timestep; totals include three timer bookend cycles over
the full timed interval, in addition to the phase sums.

| Rule | Placement / learning owner / group | Integration | Forward + quiet LTD | Post-active | Drain | Total |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Morrison | distant / home / 8 | 1,742.596 | 3,971.769 | 3,349.034 | 644.174 | 9,707.576 |
| Additive | distant / replicated / 4 | 2,201.673 | 2,320.286 | 1,846.171 | 649.692 | 7,017.825 |
| Additive | distant / home / 4 | 1,693.115 | 2,318.604 | 2,144.050 | 638.865 | 6,794.637 |
| Additive | consecutive / home / 4 | 1,693.114 | 2,380.411 | 1,702.191 | 638.692 | **6,414.411** |

Morrison averages 5.108 E and 1.322 I firings/timestep and matches the archived
ungrouped first 1,000 timed steps' firing identities, refractory and worker RNG
states through all 1,060 steps. Maximum voltage difference is 5.72205e-6 mV;
cycle cost is 24.99% below the archived 12,941.963. That archive differs in
175 initial weights by one FP32 step, despite identical graph/neuron state.
Independent delivery and canonical learning checks pass for all four new long
runs. Measured post-active refill counts equal independent graph/state-derived
update counts in all seventeen benchmarks, with no unmatched MSHR requests.

Additive averages only 3.243 E / 0.814 I firings per timestep over the long
window, versus 12.700 / 3.250 in the active pilot. Its smaller long cycle count
is therefore not an additional implementation gain. This activity difference
also makes fixed trace-maintenance overhead more important: distant home G4
beats distant replicated G4 by 3.18% over the long window, saving 508.558
integration cycles while adding 297.879 post-active cycles per timestep.

Home ownership also reopens the placement tradeoff. A consecutive G4 group
stays within one MC zone, eliminating remote scalar refills. An additional
Additive home G4 consecutive pilot passed at 10,915.30 cycles/timestep, with
phases 1,718.32 / 5,172.70 / 3,384.15 / 640.08, identical firing and final weights,
and 8.0% lower total cost than the distant replicated G4 pilot winner. Its
completed long run beats distant replicated G4 by 8.60% and distant home G4
by 5.60%. Compared with distant home G4, consecutive placement costs 61.807
extra forward cycles but saves 441.859 post-active cycles per timestep. Mean
refill response latency falls from 1,182.198 to 8.309 cycles/request; remote
post-active reads fall from 634.035 to zero per timestep. All presented
flat-memory requests were accepted, so this measures the remote response
transport/queueing cost, not HBM2 bandwidth saturation.

Across all 1,060 steps, the three Additive long runs have identical firing,
refractory, RNG and trace words; maximum voltage difference is 1.1444092e-5 mV.
All 4,050,000 final logical weights are bitwise identical, as are the starting
graphs, weights and neuron states. Each averages 1,458.773 plastic deliveries,
820.471 static deliveries, 828.258 quiet LTD updates and 846.212 post-active
updates per timestep. These savings therefore do not come from less activity.

At this stage, the measured choices favored home-owned learning: distant G8
for Morrison's pilot, with distant G4 only 0.61% slower and using much less group
storage/traffic; consecutive G4 for Additive in both windows. This sweep had
extended only Morrison G8 and had tested only G4 among consecutive group sizes.
The later consecutive Morrison G4 pilot and long run above supersede the
separate Morrison recommendation. This is still not a complete search of all
consecutive sizes, and the original replicated-trace distant-placement preference
cannot be generalized to home-owned learning. Defaults and legacy aliases remain
unchanged.

### Original four-PU pilot results (2026-09-23)

These measurements used the original replicated-trace layout with descriptors
at `0xc00`; current images use the parameterized layout described above.

All six runs used 60 warmup and 60 timed steps, disabled payload diagnostics,
the preserved 512-PU simulator with eight host threads, hot I-cache and flat
memory. At most two simulators ran concurrently. Morrison uses the 1,000-step
presimulation checkpoint; Additive uses the same activity-selected 1,120-step
checkpoint for all placements.

All timing values below are average cycles per timestep:

| Rule | Placement | Integration | Forward + quiet LTD | Post-active | Drain | Total |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Morrison | Ungrouped | 1,730.33 | 8,120.03 | 2,699.20 | 639.52 | 13,189.13 |
| Morrison | Consecutive | 2,241.43 | 4,432.42 | 3,268.13 | 653.17 | 10,595.20 |
| Morrison | Distant | 2,249.82 | 4,242.47 | 3,116.47 | 667.93 | 10,276.73 |
| Additive | Ungrouped | 1,705.35 | 8,468.83 | 3,622.05 | 638.00 | 14,434.28 |
| Additive | Consecutive | 2,218.85 | 5,182.40 | 4,068.28 | 652.60 | 12,122.18 |
| Additive | Distant | 2,226.17 | 5,000.47 | 3,981.23 | 652.53 | 11,860.45 |

Distant grouping reduces total cost by **22.08% for Morrison and 17.83% for
Additive**. Consecutive grouping reduces it by 19.67% and 16.02%. Within each
rule, all placements have identical firing decisions through all 120 steps and
identical final values for all 4,050,000 weights. The gains do not come from
reduced activity. Morrison averages 5.100 E / 1.283 I firings and 2,300.73 plastic
deliveries per timestep; Additive averages 12.700 E / 3.250 I firings and
5,713.75 plastic deliveries per timestep.

Average per-step maximum outgoing synapse counts fall from 616.75 to 181.85 /
168.58 for Morrison and from 684.52 to 219.82 / 210.93 for Additive
(consecutive / distant). Mean plastic-route distance falls from 15.942 hops to
15.443 / 7.050. Static deliveries retain their MC scatter transport, so these
hop figures describe plastic delivery, not all NoC traffic.

The tradeoffs are visible: trace replication adds about 511–521 integration
cycles/timestep, and post-active work becomes more concentrated near firing
targets. The busiest PU's post-active updates rise from 8.77 to 12.08 / 11.87
per timestep for Morrison and from 15.45 to 17.73 / 17.98 for Additive. Group
completion sends about 2,048 / 2,050 notifications per timestep, mostly empty
end markers. Static directory reads grow fourfold, while plastic AC bytes are
essentially unchanged. These are remaining optimization opportunities.

All grouped proxy traces/flags equal their home values bit for bit. Worker RNGs,
home traces and refractory states also match the baseline across all steps.
The largest voltage difference is 1.1444e-5 mV, from changed FP32 summation order.
Independent delivery, deterministic neuron replay, canonical trace/weight
validation and the FP64 derivative bound all pass. The maximum final-weight
error against the independent oracle is 0.00024414 in scaled units.

Full results and provenance are in `copilot/tmp/brunel-proxy-20260923/results.md`.
Raw runs are on CRAFT-A100 under `/data/meow/ActiveN-stdp/brunel-proxy-20260923/`.
These matched short pilots establish the performance change; they do not replace
the longer GeNN dynamics comparison below.

## Morrison seed investigation (completed 2026-09-23)

Five seeds, each with 60 warmup and 10,000 measured timesteps, resolve the
earlier single-seed activity concern. The five-seed means are:

| Implementation | E firings/timestep | I firings/timestep | Cycles/timestep |
| --- | ---: | ---: | ---: |
| Older V1 | 5.33390 | 1.34802 | 13,863.74 |
| Latest ungrouped V3 | 5.27658 | 1.33982 | 13,163.27 |
| GeNN | 5.27302 | 1.33790 | — |

V3 differs from GeNN by +0.068% E and +0.144% I activity in the ensemble. Its
mean cycle cost is 5.05% lower than V1, and is lower in every seed, including
the three seeds where V3 does more spike work. The paired V3-minus-V1 E-activity
change is -1.044%, with a 95% confidence interval of [-4.746%, +2.658%].
The original low-activity V3 seed was not representative of the ensemble.
V3/GeNN mean final weights are 45.45534/45.45726, standard deviations
0.44613/0.44525, ISI CVs 0.71207/0.70599, and Fano factors 43.1684/43.1890.
GeNN weight statistics use its 100,000-weight sample.

V3's four phase costs average 1,730.06 integration, 7,735.98 forward,
3,043.01 post-active, and 654.21 drain cycles/timestep. These runs disabled
payload diagnostics for both V1 and V3. New short pilots still need their own
matched baseline because their activity window differs from the long ensemble.

Deterministic replay, canonical trace/weight checks, and all final weights pass.
The old derivative-relative-error check was replaced by the existing FP64
gamma(3) cancellation bound; all 50,300 V1 steps pass, with maximum bound
fraction 0.7221. This historical replay conditions on observed recurrent input
accumulators and does not independently establish their delivery correctness.
The current validator additionally compares newly delivered E/I inputs against
the independently advanced reference before importing observed state.

The detailed report is `copilot/tmp/brunel-morrison-seeds-20260920/results.md`;
raw server runs are under `/data/meow/ActiveN-stdp/brunel-morrison-seeds-20260920`
on CRAFT-A100. Future jobs are limited to two concurrent eight-thread simulators.

## Assembly cleanup and combined-current measurements (2026-09-17)

The two stages were benchmarked separately with 60 warmup and 60 timed steps,
using the same preserved 512-PU/16-MC/two-SMT simulator, eight host threads,
initialized I-cache, and flat memory. Payload diagnostics were disabled;
passive simulator observation collected phase timings and full state snapshots.
No simulator rebuild or RTL re-elaboration was performed.

All timing entries are average cycles per timestep:

| Phase | Previous | Lists/report/local cleanup | Also combine currents |
| --- | ---: | ---: | ---: |
| Integration | 2,282.07 | 2,075.05 | 1,845.35 |
| Forward + post-quiet LTD | 7,492.62 | 7,790.37 | 6,944.08 |
| Post-active LTD/LTP | 2,715.40 | 2,635.52 | 2,342.33 |
| Drain + trace increments | 1,038.53 | 735.68 | 735.50 |
| Workload timer total | 13,528.67 | 13,236.67 | 11,867.32 |

The total reduction is 12.28%, but stochastic work differs: neuron firings
average 2.50 / 3.78 / 2.83 per timestep. In steps 2–3, with identical fired
neuron identities and the startup cache transient excluded, the first stage
reduces total cycles 7.83%; combining currents then reduces integration 10.77%
and total cycles a further 1.52%. This matched comparison is only two steps.

Both 120-step runs pass independent deterministic replay and checks of all
4,050,000 weights. Both learning rules pass short assembly self-test pilots.
An all-22-neurons-on-one-PU stress case validates list population/clearing; a
500-step filter test compares combined FP32 state against separate FP64 channels
without resynchronization. Full results, input/binary hashes, tested source
snapshots, and commands are under `copilot/tmp/brunel-more-20260917/`.
The combined-current version is the current source implementation.

## Current assembly runtime: post-quiet delivery and post-active MSHRs

The current payload is **assembly only**, in `brunel.S` and `brunel_runtime.S`.
The shared `brunel_rule.inc` selects the spike handler and learning rule
at compile time. Build `sys/brunel_additive.bin` and/or `sys/brunel_morrison.bin`;
datagen automatically selects the matching binary for `--rule`. Boot checks
that the image's `H_RULE` matches the compiled rule. Production retirement
inlines the selected arithmetic and the variant's scalar writeback; it does not read function
pointers from handler CSRs or call separate learning helpers.
The numeric variants (`brunel_<rule>_numeric.bin`) additionally include both
rules' numerical test routines; their workload still uses the compiled rule.

Compatibility targets `sys/brunel.bin`, `sys/brunel_asm.bin`, and
`sys/brunel_asm_numeric.bin` select `BRUNEL_RULE=additive` by default; pass
`BRUNEL_RULE=morrison` for Morrison aliases. Changing that variable always
refreshes the aliases, even if the selected binary was built earlier. When
changing `BRUNEL_ASM_FLAGS` for instrumentation/stress variants, use `make -B`
and preserve the resulting binary separately from production benchmarks.
The C kernel has been removed. The host numerical helper and
historical cache-policy evaluator remain independent testing/research tools.
The image ABI is `0x42524e34`; images with older plastic-row layouts must be
regenerated.

There is no persistent cache or deferred learning across timesteps. Plastic
weights reside in the synapse owner's MC: the presynaptic home in ungrouped
images, or the assigned proxy in grouped images. Static connections retain
target-owned CSR scatter. Neuron ownership is `PU = 1 + gid % 512`, local index
`gid / 512`.

| SPM offsets | Bytes | Contents |
| --- | ---: | --- |
| `0x0000–0x0100` | 256 | Header, counters, constants |
| `0x0100–0x0900` | 2,048 | Up to 25 neurons, 80 bytes each |
| `0x0900–0x0964` | 100 | Up to 25 fired-neuron SPM pointers |
| `0x0964–0x2900` | 8,092 | Reserved in ungrouped images; proxy control/jobs/descriptors use 1,648–6,400 bytes |
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
Header offset 112, formerly the unused report-MC word, stores the immutable
neuron-end pointer produced by the boot conversion loop. Header offset 44,
formerly the unused active-MC mask, holds the end of the contiguous excitatory
prefix. Boot checks this ordering. Integration retains dynamic AMO assignment,
first using the excitatory end and then the total end. Leaf PUs use
a boot-selected control entry that skips child and reduction setup.
Boot repurposes the image constants at offsets 208/212/216 as the external
current contribution (`FP32(EPSC * external_weight)`), fired-list count, and
self-plus-children report count. The initial-image constants remain unchanged.
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
Each worker transitions from an E loop to an I loop, processing the first I
record it already claimed before claiming another. The shared assembly macro
emits identical membrane/current/RNG equations for both loops. Only the E loop
emits trace decay: this saves two loads, two FP multiplies, and two stores per
I neuron, without a per-neuron type check in either loop. Trace finalization
also skips fired I neurons; their firing flags and outgoing static traffic
remain fully active. The validator checks zero I trace words and each PU's
E-end pointer while preserving the independent canonical trace oracle.

Both current channels have the same alpha-filter coefficients. Boot converts
the image's separate E/I states to `I_total=FP32(Iex+Iin)` and
`dI_total=FP32(dIex+dIin)` in offsets 4/8. Offsets 12/16 become zero reserved
words. Incoming E/I accumulators remain at offsets 68/72, so delivery and the
80-byte neuron stride are unchanged. Manifests declare `current_state: combined`;
the independent presimulation and initial image retain separate channels.

The membrane expression is `p31*dI_total + p32*I_total + p33*V`.
The current recurrence is `I_total'=p11*I_total+p21*dI_total`, followed by
`dI_total'=p11*dI_total+(input_E+input_I)+external`. This removes six FP
instructions, two loads, and two stores per non-refractory neuron, at the cost
of different FP32 grouping. The usual
non-refractory, non-firing E path uses 11 FP instructions when recurrent and
external inputs are zero, or 12 for one or two external events. Nonzero recurrent
inputs add two FP instructions. Zero external input skips conversion and external
arithmetic entirely. One uses the precomputed external contribution in `s7`;
two uses `a7 = s7+s7`, computed once per worker. Counts of three or more retain
conversion and multiplication. The first three CDF thresholds stay in registers;
rarer cases read the remaining small table. I paths use two fewer FP
instructions. Both threads retain dynamic AMO
work allocation and independent register-held RNG states.

Workers retain the AMO claim address in a register. Zero recurrent inputs skip
both input additions and their clearing stores; the current decays and external
Poisson contribution still execute. Refractory counters are stored only when
decremented or set by firing. Only firing neurons store `N_FIRE=1` and atomically
append their SPM pointers. Before the next integration starts, control clears
the preceding list's firing flags and resets its count. The final integration
join publishes all pointers before forward/reverse work can read the list.
Boot seeds the list from the presimulated flags so the first clearing is exact.
Forward, reverse, and trace increments iterate this bounded list instead of
scanning every local neuron; snapshots still retain the completed step's flags.
The validator infers the external Poisson integer from the combined derivative,
advances the original separate-channel reference equations, and compares their
sums with the observed state. Only then does it carry the observed state into
the next step; synaptic weights and canonical learning traces remain independently
computed for the whole run when `trace_state: learning-scaled` is selected.
A separate 500-step filter test checks accumulated rounding against two FP64
channels without any intermediate state resynchronization.
For the combined derivative, validation also evaluates the recurrence in FP64
and bounds rounding by `gamma(3) * sum(abs(terms))`, with an underflow allowance.
This handles nearly cancelling excitation/inhibition without normalizing error
only by the small final result. The 1,060-step baseline contained one such case;
its derivative was 0.0000537 from the FP64 value and inside the rounding bound.
Firing flags, refractory state, weight limits and the other state checks retain
their existing requirements.

The reduction tree retains its original messages, priorities, quotas, and
completion conditions. Boot selects a direct forwarding report handler for
leaf PUs. Internal nodes store only incomplete aggregates and load their own
identity only when the aggregate is complete. Report arguments `a2/a3` are
unused. The former active-MC mask and its reduction have been removed: reverse
work visits its directory independently of that mask. Their header words now
hold the E and total-neuron end pointers without changing the image ABI size.
Integration alone
resets the join counter; other phases initialize only their work iterators.
Terminal report/control/work sends yield directly while retaining the existing
handler ownership, priority, quota, and margin settings.
The MSHR service continuation deliberately uses an ordinary send before idling:
yielding during its enqueue can schedule an already-pending drain worker before
the requeued service becomes visible. An extended pilot exposed that ordering
hazard with two MSHRs still live; service priority must remain effective through
retirement and clearing its scheduled flag.

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
nine instructions through the yield, including its exit branch, remote blocking send, and local
yield. The local yield replenishes quota and admits higher-priority handlers.
Each chunk keeps `DRAM_start - SPM_start` in a saved register, allowing the
message's weight address to be derived from the advancing SPM pointer without
maintaining a second per-synapse iterator.
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
It also retains the sent-event accumulator in `s9`, publishing it once the
directory is complete. Plastic delivery adds its sent count once per completed
row. Directory/MSHR refills pass the SPM line index directly to the transport.

Fired-post reverse CSR events carry `(preSpmOffset, weightDramAddress, LTD, unused)`.
In ungrouped images the first field is `256 + 80 * preLocal`, precomputed in the generator and
identified by `reverse_index: neuron-spm-byte-offset` in the manifest. The
receiver adds the SPM base directly. Old reverse-index images require migration
or regeneration with the current generator, even though the outer row-layout
magic remains `0x42524e34` for the existing simulator observer.
The receiver suppresses LTD unless the pre also fired, and takes its LTP
coefficient from the decayed, learning-scaled local pre trace. A zero trace and
quiet pre reject the event before allocation. Every nonzero update uses an
MSHR. Post-quiet and post-active
updates are disjoint, including parallel edges with distinct addresses; multiple
outstanding MSHRs cannot update the same weight. Different words sharing a DDR
line remain independent because writeback changes only one FP32 word.

### MSHR progress and writeback

Each descriptor has a 32-byte aligned landing line, address, LTD coefficient,
LTP coefficient, state, diagnostic sequence number, optional remote MC
destination at offset 52, and two reserved words.
A post-active update has LTP and may also have LTD if the pre fired this step.
Retirement applies LTD before LTP.

SMT0 owns a 16-entry FIFO allocation/retirement ring. State 1 means loading;
state 2 means its completion notification has been consumed. A short leaf
completion handler on SMT1 marks ready and retires one notification credit.
This handler never performs update arithmetic, waits for memory, or sends
messages. In relay builds SMT1 also runs the separate home trace forwarder.

One coalesced local handler-6 event on SMT0 retires the ready ring head, applies
the update, emits one FP32 store, and frees its landing line. Ungrouped and
replicated variants request a store receipt; home-update proxy variants use
a no-receipt store to the descriptor's MC, and relay variants use a local
no-receipt store. It
reschedules itself while entries remain. Allocating reverse/proxy-update
handlers have higher dispatch priority. If the pool is full, allocation services
the head inline; this avoids waiting for a lower-priority handler on the same
SMT thread. FIFO retirement
can wait behind the oldest outstanding read, which is measured separately from
merely finding the pool full of ready entries.

There are still only **12 issued-but-unconsumed memory notifications** across
loads and stores, with a dedicated 16-entry completion queue. Sixteen descriptors
do not imply sixteen concurrent reads. Reusing a descriptor after issuing its
store is safe: the outgoing scalar request already contains its value. Store
acknowledgements occupy notification credits in the acknowledged variants;
home and relay variants consume credits only for reads. The finish barrier waits
for all MSHR events and issued notifications. No-receipt writes use the existing
writeback-priority transport; their completion is not counted by software.

Handler 6 uses SMT0, zero arguments, quota 2 and margin 3: at most one store and
one local continuation. Its local event queue contains at most one notification.
Existing reverse/work quotas remain sufficient (at most one inline retirement,
one read, and one service event per allocating reverse handler). NoC priorities
are writeback 1, refill 2, neural/scatter 3; control and local messages use 0.
Relay builds instead use writeback 0, refill 1, relayed update 2 and scatter 3,
as described in the proxy ownership section.
Forward wait/drain handlers 11/12 use SMT0, zero arguments, quota 2 and margin
19, leaving room for the existing higher-priority handlers.

The shared compile-time rule selection supplies the spike-handler symbol and
inline LTD/LTP arithmetic. Boot validates the image's rule and proxy variant.
Event slots 13/14 are proxy notifier/wait handlers in grouped builds, and
remain disabled in ungrouped builds; neither stores a function pointer.
Directional clamps retain only the applicable bound for nonnegative input
weights/coefficient values. A generic rule-dispatch wrapper exists only in the
numeric self-test build. MSHR retirement keeps the weight in outgoing store-data
register `a3`, preserves both return links, and returns the remaining count.
Allocation receives address/LTD/LTP directly in `a4`/`a5`/`a6`; only its full-pool
fallback spills them. Reverse work reserves `s3`/`s4`/`s10` for Morrison's seed,
division multiplier, and Newton constant, or `s3` for Additive's upper clamp.
Each PU initializes these before reporting forward completion, so reverse
events arriving ahead of local phase control already see valid constants.
Incoming handlers preserve the saved registers. The spike receiver also computes
directly into `a3`; unused no-receipt-store and bulk/scatter carried words do not
need initialization because their receivers do not inspect them.

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
make -C sim/payloads sys/brunel_morrison.bin sys/brunel_morrison_numeric.bin
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

For transport/numeric validation, embed the matching `brunel_<rule>_numeric.bin` in a short
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
