# Selected workloads and retained evidence

Selections audited **2026-10-14**; procedure updated **2026-10-15**.
This is the entry point for the selected MNIST,
Brunel and full-Morrison implementations. Historical experiments and their original documentation are indexed in
[archive/README.md](../archive/README.md); their rankings and commands do not
override the selections below.

The results are measurements of frozen payloads, inputs and simulators. A
current-source build has not been rebenchmarked against them. Reproduction
with newer RTL remains separate work. [sim.md](sim.md) now defines new-run
procedures and artifact retention. Its 1.5 GHz/HBM2e/one-step-warmup policy
makes the timings below outdated for the current target. They remain
implementation references; no new target result is established by scaling them
by 1.5. Historical flat-memory runs and short pilots do not establish defaults.

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
- The retained MNIST/Brunel runs precede MemIf write merging. Full Morrison
  explicitly enables `SOFT_MEMIF_WRITE_MERGE=1`; merging is off by default in
  the soft model and has no equivalent RTL implementation.
- Async-copy notification (`acnotify`, CSR `0x734`) and system-wide idle
  notification are present in current RTL, but these payloads still use their
  earlier completion/drain protocols. The soft backend lacks the new global
  idle reduction and ring-empty condition. The inspected `generated/system`
  and `generated/morrison-512` targets also predate both additions. Regeneration,
  soft-model alignment/cosimulation and workload integration are pending.
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
test sources and recipes have been restored unchanged. Cleanup is limited to
workload additions outside `next`, apart from requested documentation/path updates.
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
not preserve ignored local data or remote artifacts. Archive compression and
reproduction under [sim.md](sim.md) remain separate steps.
