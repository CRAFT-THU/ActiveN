# Selected workloads and retained evidence

Selections audited **2026-10-14**; reproduction accepted **2026-10-20**.
This is the entry point for the selected MNIST,
Brunel and full-Morrison implementations. Historical experiments and their original documentation are indexed in
[archive/README.md](../archive/README.md); their rankings and commands do not
override the selections below.

Reproduction of the retained ActiveN cases is complete and accepted. The
[reproduction report](../benchmark/report/reproduction-133a679-20261015.md),
[timing controls](../benchmark/report/morrison-timing-diagnosis-133a679-20261017.md)
and [MemIf pilot](../benchmark/report/morrison-memif-pilot-20261020.md) retain
results, exact simulator identities, raw evidence and validation limits. The
later [stall diagnosis](../benchmark/report/morrison-memif-stall-20261020.md)
supersedes the pilot report's preliminary, inconclusive stall discussion.

The historical table below remains a record of the previous target.
[sim.md](sim.md) defines new-run procedures and artifact retention. Its
1.5 GHz/HBM2e/one-step-warmup policy changes both the conversion to time and the
core/memory relationship; do not obtain new timings by scaling historical
cycles by 1.5. Acceptance of reproduction does not establish equivalence to
every upstream neural model or complete the later full sweep.

## Selected implementations

All assembly paths below are relative to `sim/payloads/`. Rules and variants
already share entrypoints and includes; the selected build settings are part
of the implementation identity.

| Workload | Current source and selected build |
| --- | --- |
| [Dense MNIST](mnist_train.md) | `sys/mnist_train.S`, shared neuron/fifth-root includes; `sys/mnist_dense_<rule>.bin` with the selected trace rule, tree recurrence, unbundled traces, bit fifth-root seed and `MNIST_HORIZONTAL_GROUP_COUNT=8` |
| [Sparse MNIST](mnist_sparse_train.md) | Same entrypoint plus `sys/mnist_sparse.inc`/`mnist_sparse_layout.inc`; `sys/mnist_sparse_<rule>_mode<3|4>.bin` with setup extraction and explicit reverse mode 3 or 4 from that document's table |
| [Brunel](brunel.md) | `sys/brunel.S` and `sys/brunel_runtime.S` with common rule/layout/proxy includes; `sys/brunel_additive_proxy_home_g4.bin` and `sys/brunel_morrison_proxy_home_g4.bin`, diagnostics disabled |
| [Full Morrison](morrison.md#selected-configuration) | Same Brunel entry/runtime plus bulk/ready/tree includes; `sys/morrison_bulk_worker_scheduled_smt1.bin`, ABI4 distant G32, individual-edge placement seed `0x20261011`, SMT1-only reverse relay and shared updater on both SMT pipes |

MNIST's selected placement is 288 input PUs and 224 neuron PUs with `full-8`
partitioning. The generic dense Makefile default is four groups, and the sparse
mode default is 1: neither default alone selects these retained configurations.
Brunel uses `--proxy-groups consecutive --proxy-size 4 --proxy-updates home` and
imports the exact settled checkpoint with `--load-genn-checkpoint` and
`--presim 0`. Full Morrison needs the recorded ABI4 worker layout and checkpoint
overlay; a fresh ABI1 `morrison.py` image is not its input.

Current selected Brunel payload hashes are
`101154d6eb73b056994b61601dcb3d54ea4e33b24b2d653bb97e4f39e524a665`
(Additive) and
`450c86e6a633cc40b1914e4e7a665763e8773eb04f3f7c8db52f7e1046ed1bc3`
(Morrison rule). The selected full-Morrison payload hash is
`c0df21626f61674a82d0b6bb5aa4f5a012056535652a4cc237b699e0c95c15b4`.
Dense MNIST's retained `build.json` and the
[historical sparse baseline manifest](../archive/workload/doc/mnist_sparse_baseline.json) pin its separate payloads.
These identities identify retained binaries, not proof of a fresh byte-identical
rebuild from the current source/toolchain.

## Findings to carry into the full sweep

The reproduction covered three dense MNIST corpora, nine sparse corpora in
both retained delivery modes, both Brunel rules and both retained full-Morrison
checkpoint ages. It did not include the sibling baseline's newer fixed-fan-in
MNIST, Fashion-MNIST or CIFAR-10 cases. Pin the next sweep's case matrix and
canonical checkpoint identities independently. The sibling
`../stdp-experiments/BASELINE.md` can change; preserve the exact version used
with the selected inputs rather than referring to its current contents.

The accepted numerical comparisons are in the linked reports. Carry forward
these interpretations and limits:

- MNIST and Brunel reproduce the retained ActiveN implementations under the
  approved new settings. Dense MNIST's historical lack of preload is one
  changed condition, not an isolated explanation of its reduction.
- Full Morrison closely reproduces historical cycles when historical
  core/memory timing is restored. For the later checkpoint with matched one-warmup/100-measured-step windows
  and fixed HBM2 timing, reverse elapsed time barely improves with the higher
  core clock, so its core-cycle count grows; forward scales more strongly. This is compatible with a bank-service
  bottleneck and does not establish insufficient MSHRs to hide individual-read
  latency. Archived bank-probe CSVs provide context, but an original
  checksum binding was not found in the retained ledgers; the current clock controls did not collect that detailed
  bank breakdown. The [retained mechanism assessment](../benchmark/rundir/reproduction-closeout-20261020/clock-mechanism/pending-answer-v4.md)
  and its [review](../benchmark/rundir/reproduction-closeout-20261020/clock-mechanism/scope-review.md)
  record those limits. That assessment predates the later MSHR pilot.
- The later first-free MSHR pilot changes allocation, admission and reservation
  semantics relative to the original reproduction binary. Compare its pool
  sizes against its own control. It is a short pilot, not a general scaling
  result or a replacement for the full matrix.
- Strict sequential allocation is excluded from the intended sweep. The
  reviewed deadlocks have fulfilled broadcasts pinning the cursor, unusable
  free holes, and scalar-store/NoC/send-queue backpressure preventing the
  broadcast handler from meeting its scheduling margin. No pending I-cache
  miss is required to sustain that cycle. Available-first (`first-free`)
  allocation remains the selected policy for the current design.

### Measurement and configuration

Use the complete peripheral-timer total divided by all completed measured
steps, including quiet steps. For MNIST, sum its separate timed intervals
without adding untimed gaps. At 1.5 GHz, `us/timestep = cycles/timestep / 1500`.
Phase spans are supplemental; retain the timer residual. Accepted PU1 markers
use 2 for integration, 3 for forward, 4 for reverse and 5 for finish. Older
Morrison probes use different header conventions; decode each format against
the exact executed source.

The reproduction used soft mode, eight workers, 512 PUs/16 MCs, one GiB per MC,
32-byte lines, two SMT pipes, 16-KiB SPM, 8-KiB I-cache preload, 1.5-GHz core/NoC,
one warmup step and merging enabled. Its memory model is a complete 625-ps
HBM2-derived extension representing two eight-channel stacks, not a
manufacturer-calibrated HBM2e part. Preserve its YAML hash and effective startup
configuration. A frequency override alone does not rescale cycle-based DRAM
timings. Retained measured lengths were 12 steps per MNIST snapshot, 10,000 per
Brunel rule and 100 per Morrison age; these do not silently select the broader
sweep's lengths.

Preload and one warmup step do not imply zero measured I-cache refills. Normal
replacement remains active, boot-ROM fetches can displace preloaded lines, and
one event-driven step need not exercise every handler on every PU. Retained
reproduction evidence includes refill counts; cumulative refills are not
concurrent MSHR occupancy. Cleaned simulator builds no longer report these
refill counters.

Identify each memory-statistics interval. Reproduction MNIST statistics included
initialization and warmup; recurrent timer-only statistics cover their measured
interval with the recorded boundary phases. Memory ticks, request
presentations, accepted transactions, logical accesses and changed weights are
different quantities. Merging can change physical write counts while spikes
and final initialized DRAM agree. Heartbeat request counters count
presentations; the printed pending-response field samples MC 0. Neither is an
all-controller-empty or accepted-traffic oracle.

Use explicit soft-MemIf controls: `--soft-memif-mshrs` (1..65536, default 256),
`--soft-memif-allocation first-free`, `--soft-memif-unicast-reserve` (0..N,
default 32), and `--soft-memif-write-merge on|off|true|false` (default off).
Counts below 32 need a smaller reservation; `R=N` is valid but leaves no
bulk allocation headroom. Reservation covers both scatter and unicast bulk
allocations. Scalar acceptance requires a free allocatable
slot even when a write could merge. External IDs are direct 16-bit MSHR indices;
the former 256-ID mapping limit is gone. `SOFT_MEMIF_WRITE_MERGE` is removed.
These controls affect the software backend and do not certify matching RTL.

The proposed hardware `canMerge` bit is a future direction, not an implemented
result. Mark at most one unissued eligible posted write per line; any later
same-line access clears older eligibility, including reads and bulk read beats.
A merge keeps its FIFO position and updates byte masks/data. Clear eligibility
on issue/free without reviving older entries, and exclude a request accepted
by memory with its old payload. Freezing at first presentation is a conservative
alternative that narrows today's software merge window. Receipt-bearing stores
need additional completion bookkeeping. This supports first-free allocation
without deriving age from indices; no RTL timing benefit has been measured.

### Validation and retained identity

Keep performance acceptance separate from upstream model agreement. The
[original report](../benchmark/report/reproduction-133a679-20261015.md) records
these unresolved inherited differences:

- MNIST trace decays differ from available corpus time-constant manifests.
  Reproducing the retained FP32 arithmetic does not resolve that mismatch.
  Dense final-state/membrane coverage is incomplete. Sparse modes retain a
  one-ULP neuron-state difference under the historical allowance, not universal
  bit equality.
- Brunel's Morrison-rule payload differs from inspected retained GeNN source
  in old/new weight delivery and tied LTD/LTP ordering. The checkpoint creator's
  exact execution and a matching autonomous stochastic trajectory were not
  established.
- Full Morrison replay is conditioned on observed spikes. Its weight, trace
  and transport checks do not certify autonomous membrane trajectories, all
  pending-current sums or every upstream conversion. Matching final DRAM and
  spikes does not make every intermediate neuron word identical.
- The reproduction records an unresolved hard/soft SNN mismatch and a
  single-core SHA assertion. Later CSR checks do not resolve those separate failures.
  Software-only merging and documented idle differences were explicitly
  authorized; this is not universal RTL/software equivalence.

The original reproduction and timing controls use preserved simulator SHA-256
`54a3a0677256f5bae81b313c2c00655da021aa060670f6dccac73ba5fbc15f77`.
The MemIf pilot and stall captures use
`1d784885a1f1d8e64f2bf8fd18984a12581533353142569718c9d2a29843e73f`,
from an explicitly authorized frozen uncommitted snapshot. Commit labels alone
do not identify these different source/configurations. The reports link their
source manifests, payloads, configs, execution scripts and raw logs. Preserve
those artifacts unchanged when cleaning active source.

After cleanup and user commit, a new benchmark needs a fresh build and preserved
identity under [sim.md](sim.md). Select the intended Core export; default
generated directories may be stale. The scalar MC address map, line size and
SMT/cache/SPM geometry are compiled properties. Runtime PU/MC arguments do not
rewrite that map. Soft-only BOLT training uses 64/2 with compatible two-image
inputs; benchmark topology is selected at runtime. Keep successful BOLT
training evidence, runtime libraries and configuration hashes.

The simulator cleanup removes workload-specific SPM/DRAM dumps, hardcoded
completion-PC and receipt observers, phase/NoC/core profiling, the event-queue-1
spike assumption, and I-cache refill counters. This includes profiling inherited
from `next`. The system `--log` and `--noc-profile*` options are removed; the
single-core packet log remains available. Functional MemIf changes, sequential
allocation and router reservation controls, I-cache preload (`--init-icache`),
peripheral timers/results, FST/cosim and Ramulator statistics are retained.

Older benchmark scripts that consume those observers or dumps need replacement
integration before they can validate runs with the cleaned simulator. The
agreed follow-up is to reduce firing counts across all populations in the
workload, print their sum through PU1's peripheral output, and compare that sum
and final state. Generic SPM export through VPI/DPI and generic DRAM output
for mutable weights are deferred; this cleanup does not implement them or add
replacement architectural completion checks. Historical binaries and evidence
retain their original behavior and output formats.

The user owns old build-directory removal. Source cleanup does not delete
`sim/build*`, immutable workload inputs, preserved binaries or raw evidence.
The broader sweep has not been launched by this closeout.

## Historical performance (previous target)

The metric is the complete peripheral timer total divided by the measured
step count in that same interval, excluding warmup. MNIST stops/restarts its
timer per step; sum those intervals without adding the untimed gaps.

| Workload/configuration | Timer cycles / measured steps | Cycles/timestep | Memory and window |
| --- | ---: | ---: | --- |
| Dense MNIST, one trace | 3,102,966 / 1,200 | 2,585.8050 | Flat; 100 snapshots, each 4+12 steps |
| Dense MNIST, two traces | 5,495,399 / 1,200 | 4,579.4992 | Flat; same snapshot count/window, separate trained corpus |
| Dense MNIST, three traces | 3,817,586 / 1,200 | 3,181.3217 | Flat; separate trained corpus |
| Sparse MNIST, selected modes across rules/rates | Each rule/rate/mode has 1,200 measured steps | 3,965.0383–6,396.2517 | Flat; 100 snapshots, each 4+12 steps |
| Brunel Additive, consecutive home G4 | 57,632,097 / 10,000 | 5,763.2097 | HBM2; 60 warmup + 10,000 measured |
| Brunel Morrison rule, consecutive home G4 | 76,523,023 / 10,000 | 7,652.3023 | HBM2; 60 warmup + 10,000 measured |
| Full Morrison, settled tick 12,000,000 | 17,123,741 / 100 | 171,237.41 | HBM2; historical 0+100 pilot |
| Full Morrison, tick 12,750,060 | 14,895,085 / 100 | 148,950.85 | HBM2; historical 0+100 pilot |

MNIST timesteps are 0.5 ms; Brunel/Morrison timesteps are 0.1 ms. All use
512 PUs/16 MCs, 32-byte memory lines and two SMT pipes. Dense MNIST used eight
host threads without preload; sparse MNIST used one host thread per simulator
with I-cache preload. Both HBM campaigns used eight host threads, 1 GHz and
8 KiB preload. Historical MNIST/Brunel frontend capacity was 4 GiB per MC;
full Morrison used 1 GiB per MC. The Brunel HBM2 model's nominal geometry is
1 GiB per MC despite its larger frontend allocation.

The sparse range selects the lower mean of the two modes in the completed
comparison. It is not a bound on every configuration: two-trace 50% neuron
scatter took 12,120.1433 cycles/step. Comparing separate dense/sparse or
cross-rule corpora does not establish an implementation speedup.

G4 is Brunel's selected common configuration. In the matched 1,000-step HBM
quick comparison, Additive G2 took 5,766.419 versus G4's 5,792.763 cycles/step;
only G4 was subsequently measured over the full 10,000-step window. Full
Morrison's two values reflect different starting states. The selected policy
won the tested policy comparison within each state, but neither 100-step pilot
uses the then-standard 60+1,000 quick or 60+10,000 full benchmark window.

## Integration and validation still outstanding

- Morrison's newer proxy/range-dispatch and shared-updater mechanisms remain
  candidates for backporting to Brunel. They are not included in the retained
  home-G4 results, and no backport speedup is measured.
- Historical MNIST/Brunel merging settings were not authenticated. Historical
  full Morrison explicitly enabled the former environment option. Current
  runs select merging through the CLI; it remains off by default and has no
  equivalent RTL implementation.
- Async-copy notification (`acnotify`, CSR `0x734`) and system-wide idle
  notification are present in RTL, but these payloads still use their earlier
  completion/drain protocols. The soft backend lacks the new global idle
  reduction and ring-empty condition. Reproduction used a fresh Core export
  and explicit authorization for these differences; software alignment and
  adoption of newer completion protocols remain separate work.
- Async completion is [documented as volatile](mem.md#async-copy). Event-queue
  pressure can lose notifications; adoption must preserve the workload's
  completion-accounting requirements.
- Sparse MNIST's 72-case relevant-mask/MC-drain pilot has completed according
  to retained records, but remains an experiment with a stale restart-era
  summary. It has not replaced the selected full-corpus baseline. Nibble
  skipping and the experiment's disposition remain open.

The audit independently reconstructed timer arithmetic, configuration and
artifact identities. It did not run new workloads, rebuild simulators or
replay all numerical state. Historical validation reports retain their original
limits, including Morrison's missing full-network pending-input-sum oracle.

## Maintained tools and retained inputs

`make -C sim/payloads -f selected.mk selected-workloads` builds the selected rule/mode matrix
with distinct filenames. Run it through the Nix development shell. The exact
profile/placement selections are in [workloads/profiles.json](../workloads/profiles.json).
Here a build profile is a named firmware Make target with fixed compile-time
options and a distinct output file; it is separate from simulator builds.
[sim-retained.md](sim-retained.md) identifies existing simulator binaries that
must remain in place through reproduction.
The workload entrypoint includes the shared Makefile, whose existing integration
test sources and recipes have been restored unchanged. That earlier workload
reorganization was limited to workload additions outside `next`, apart from
requested documentation/path updates. The later simulator cleanup also removes
profiling inherited from `next`, as described above.
Generic targets retain compatibility switches for functional tests and research;
their defaults are not the selected benchmark configuration.

[workloads/retained.json](../workloads/retained.json) maps immutable local inputs
to their original archive paths. Inputs live under `workloads/data/`; generated
functional outputs belong in fresh `work/` directories; benchmark runs belong
in `benchmark/rundir/` with reports under `benchmark/report/`.
The local inputs are hard-linked to archive files until the archive is compressed;
never overwrite them. Large input/output data are ignored by Git.

| Workload | Maintained entrypoints | Retained input location |
| --- | --- | --- |
| Dense/sparse MNIST | `datagen/mnist_train.py`, `mnist_sparse.py`, `mnist_genn_snapshots.py`; snapshot readers and FP32 validators alongside them | `workloads/data/mnist/`: three dense and nine sparse snapshot corpora, frozen dataset |
| Brunel | `datagen/brunel.py`, `brunel_checkpoint.py`; GeNN settle/resume in `tools/checkpoints/brunel/` | `workloads/data/brunel/`: one settled checkpoint per rule |
| Full Morrison | `datagen/morrison_checkpoint.py`, `morrison_worker_images.py`, and shared model/graph/layout libraries | `workloads/data/morrison/`: original native graph/state, canonical ABI3 base/graph, later reduced state |
| Functional/BOLT fixtures | Existing system payloads and small native/worker-layout fixtures | `workloads/data/fixtures/`, including the two `snn_shuffle` BOLT images |

The old dense/sparse campaign runners, layout launchers, frozen simulators,
source snapshots, analysis scripts and raw logs remain in `archive/tmp/`.
They preserve old invocation paths as provenance. Resolve a retained asset or
old input path with `python3 datagen/workload_paths.py INPUT`; use
`--archive-root /mount/tmp` when the archived tree is mounted elsewhere.
This resolves data paths; it does not make every historical script runnable
without adapting its hardcoded paths. Current tools do not import archive code.

Primary evidence roots (relative to `archive/tmp/`):

| Workload | Evidence |
| --- | --- |
| Dense MNIST | `mnist-dense-consolidated-20260909/`, with the three upstream snapshot/training roots in the input catalog |
| Sparse MNIST | `mnist-sparse-baseline-full-20260913/`, `mnist-sparse-setup-20260913/`, `mnist-fanin-no-bound-check-20260910/reference/` |
| Brunel | `brunel-hbm-full-20260928-v1/review-20260929-v1/full/`, `brunel-settled-20260927/`, frozen `brunel-relay-20260925/source/` |
| Full Morrison | `morrison-scheduling-controls-20261012/smt1/`, `morrison-checkpoint-750000-20261013/relay_smt1/`, with their complete parent records and XOR bases |

The [reviewed audit](../archive/tmp/workspace-audit-20261014/audit.md) retains
measurement and identity coverage. Morrison input FIFOs are not saved images:
rebuild the exact initial physical image before decoding final-memory XOR data.
Its SPM chain includes `morrison-uncounted-20261009/distant/output` and
`morrison-pilot100-20261008/distant/output/step-*.bin.zst`; all remain archived.

Large Brunel input/final memories and 10,060 snapshots per rule remain on
CRAFT-A100 under `/data/meow/ActiveN-stdp/brunel-hbm-full-20260928-v1/`.
The later full native Morrison checkpoint remains under
`/data/meow/baseline-rerun-20260928-v1/a100-full-morrison-r2/full-morrison/diagnostic/checkpoint-step-750000`.
The earlier audit checked remote metadata and selected manifest hashes, not
every large blob. The sibling `stdp-experiments` repository and `stdp` link
remain separate dependencies.

The historical MNIST simulator source-to-binary rebuild chain is not fully
established. One failed dense attempt also retains a stale runner hash; its
failure status and correctly pinned resumed runner remain. Source commits do
not preserve ignored local data or remote artifacts. The completed reproduction
retains its own build/runtime/input evidence; future cases still need pinned
checkpoints and simulator identities under [sim.md](sim.md).
