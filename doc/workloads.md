# Selected workloads and retained evidence

Status as of **2026-10-14**. This is the entry point for the selected MNIST,
Brunel and full-Morrison implementations. The detailed documents retain dated
experiments pending archive migration; their historical commands and rankings
do not override the selections below.

The results are measurements of frozen payloads, inputs and simulators. A
current-source build has not been rebenchmarked against them. Reproduction
with newer RTL and a revised test protocol remains separate work. Until that
protocol is agreed, the existing benchmark requirements in [AGENTS.md](../AGENTS.md)
apply to new runs; historical flat-memory runs and short pilots do not establish
new defaults.

## Selected implementations

All assembly paths below are relative to `sim/payloads/`. Rules and variants
already share entrypoints and includes; the selected build settings are part
of the implementation identity.

| Workload | Current source and selected build |
| --- | --- |
| [Dense MNIST](mnist_train.md) | `sys/mnist_train.S`, shared neuron/fifth-root includes; `sys/mnist_train.bin` with the selected `MNIST_RULE`, tree recurrence, unbundled traces, bit fifth-root seed and `MNIST_HORIZONTAL_GROUP_COUNT=8` |
| [Sparse MNIST](mnist_sparse_train.md) | Same entrypoint plus `sys/mnist_sparse.inc`/`mnist_sparse_layout.inc`; `sys/mnist_sparse.bin` with setup extraction and explicit reverse mode 3 or 4 from that document's table |
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
[sparse baseline manifest](mnist_sparse_baseline.json) pin its separate payloads.
These identities identify retained binaries, not proof of a fresh byte-identical
rebuild from the current source/toolchain.

## Retained performance

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
is a standard 60+1,000 quick or 60+10,000 full benchmark.

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

## Evidence and extraction inventory

Paths in this section are relative to `copilot/tmp/` unless stated otherwise.
They are existing dependencies to retain through migration, not proposed
permanent destinations. The [reviewed audit](../copilot/tmp/workspace-audit-20261014/audit.md)
and its independent reviews provide the detailed identity/coverage record.
Historical `copilot/tmp` evidence is ignored by Git; a source commit does not
preserve it.

| Workload | Primary retained run evidence | Additional roots needed |
| --- | --- | --- |
| Dense MNIST | `mnist-dense-consolidated-20260909/`: payloads, frozen source, commands/provenance, per-snapshot images/logs/results | `mnist-rule-native-30k-20260907/` frozen simulator/config/labels and one-trace snapshots; `mnist-two-trace-single-pass-20260907/full/snapshots`; `mnist-fired-vector-rules-20260907/frozen/snapshots`; `mnist-fifth-root-20260909/build.py` and retained initializer object |
| Sparse MNIST | `mnist-sparse-baseline-full-20260913/`: all 1,800 case records, state dumps, frozen binaries/source | `mnist-sparse-setup-20260913/` build recipes/logs; `mnist-fanin-no-bound-check-20260910/reference/` original 900 snapshots and upstream training/input provenance; mask/drain pilot records for disposition |
| Brunel | `brunel-hbm-full-20260928-v1/review-20260929-v1/full/`: full logs, manifests, dynamics, MC statistics; quick/group campaigns remain comparisons | `brunel-relay-20260925/source/` payloads/source; `brunel-morrison-seeds-20260920/` simulator/source/runtime; `brunel-settled-20260927/` checkpoint, generator and continuation records |
| Full Morrison | `morrison-scheduling-controls-20261012/smt1/` and `morrison-checkpoint-750000-20261013/relay_smt1/`, with parent launch/source/review records | Canonical images, native graph/state, checkpoint overlay/reduced data and the XOR chains below; retain completed alternative and interrupted captures with their status |

The maintained generators/importers already include `datagen/mnist_train.py`,
`mnist_sparse.py`, `mnist_genn_snapshots.py`, `brunel.py`, `brunel_checkpoint.py`,
`morrison.py`, `morrison_native.py`, `morrison_bulk.py` and
`morrison_worker.py`. Selected execution/import/continuation glue remains outside
those maintained entrypoints and must be inventoried before extraction:

- Dense/sparse benchmark `run.py` scripts and their frozen imports/build helpers.
  The dense runner also consumes prior controls under
  `mnist-native-activen-100-20260908/results/one-trace`,
  `mnist-root-cached-20260909/results/bit`, and
  `mnist-fired-vector-rules-20260907/results/three-trace`; retain these with its
  snapshots and payload identities when consolidating.
- Brunel's `brunel-settled-20260927/` continuation and checkpoint records,
  including the frozen `gpu-source-v1/brunel/settled.py`; baseline-side scripts
  also live in the sibling `../stdp-experiments` repository.
- Morrison's `morrison-checkpoint-750000-20261013/checkpoint_stream.py` and
  `checkpoint_layout.py`, reduced `ee_g.npy`/`neurons.npy`, import/selection
  manifests, and the selected launcher/stream/archive dependencies. These
  import and overlay the selected physical state; do not replace them with
  time-zero generation or assume a generic generator fast-forwards to it.

Morrison's input `dram.*` entries under `input-fifos` are FIFOs, not stored
images. Final-memory XOR archives require reconstruction of the exact physical
starting image from the recorded layout, payload and canonical inputs.
`morrison-ready-fast-20261006/target-images` links into
`morrison-bulk-20261002-v1/settled-images` and
`morrison-native-20260929-v1/settled-g4/graph`. Selected SPM snapshots depend on
`morrison-uncounted-20261009/distant/output`, whose XOR bases depend on
`morrison-pilot100-20261008/distant/output/step-*.bin.zst`. Keep these manifests,
base files, decoders and helper libraries together when relocating artifacts.

Large Brunel input/final memories and 10,060 snapshot archives per rule remain
on CRAFT-A100 under `/data/meow/ActiveN-stdp/brunel-hbm-full-20260928-v1/`, with
settled checkpoints under `/data/meow/ActiveN-stdp/brunel-settled-20260927/`.
The original later Morrison checkpoint remains under
`/data/meow/baseline-rerun-20260928-v1/a100-full-morrison-r2/full-morrison/diagnostic/checkpoint-step-750000`.
The audit confirmed remote metadata accessibility and selected manifest hashes;
it did not decode or rehash every large blob. The `stdp` symlink alone does not
preserve the sibling repository.

The completed MNIST runs retain frozen binaries/configurations, but their exact
historical simulator source-to-binary rebuild chain was not established. One
failed dense attempt also has a stale runner hash at its recorded path; the
failure log/status and correctly pinned resumed runner remain. Preserve these
limitations and the original failed records along with successful runs.

Archive migration must retain historical comparisons and reviews, remap path
and symlink dependencies, and keep useful generators/checkpoint tools usable
outside the old temporary tree. The current documentation update has not moved
artifacts, consolidated assembly or defined the next benchmark protocol.
