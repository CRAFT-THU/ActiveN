# Full Morrison workload

<a id="selected-configuration"></a>

The selected implementation is **ABI4 distant G32 with SMT1-only reverse
relay**, built as `sys/morrison_bulk_worker_scheduled_smt1.bin`. It shares the
Brunel entrypoint/runtime and Morrison bulk, ready-queue and tree includes.
Individual edges use placement seed `0x20261011` and reside in worker MCs.
Relay handler 4 runs on SMT1; shared updater 12 runs on both SMT pipes.
Proxy acceptance uses handler 11 and the read issuer uses handler 8.

The network has 90,000 E and 22,500 I neurons, fixed incoming degrees 9,000 E
and 2,250 I, 810 million plastic E→E synapses and 1,265,625,000 recurrent
synapses overall. Connections are sampled with replacement without recurrent
autapses. The timestep is 0.1 ms. Alpha-current LIF dynamics retain voltage,
combined E/I current and derivative, learning-scaled traces, pending E/I inputs
and refractory/fire state in a 32-byte neuron record. See
`datagen/morrison_model.py` for constants and model contracts.

The selected layout uses 512 PUs, sixteen 1 GiB MCs, 32-byte transactions,
two SMT pipes, 16 KiB SPM and 8 KiB I-cache. It retains 1 KiB copies, two 2 KiB
forward banks reused as 64 reverse descriptors, a ready queue with 16 read
credits, posted weight stores and the experimental forward transition without
receiver counting. Timestep marker/MC-idle drain remains part of the protocol.
`morrison_bulk_layout.h` and the shared runtime define its current SPM/ABI.

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN/sim/payloads && make sys/morrison_bulk_worker_scheduled_smt1.bin'
nix develop --command bash -c 'cd /root/workspace/ActiveN && mkdir -p work/tools && g++ -O3 -std=c++17 -shared -fPIC datagen/morrison_locality.cpp -o work/tools/libmorrison_locality.so'
```

Use a fresh output path for the helper when retaining a previous build's
identity. The helper is required even for random placement; the current
layout library loads it during construction.

`datagen/morrison_worker_images.py` writes the selected physical ABI4 images
from a canonical, balanced G32 ABI3 base. It accepts retained asset IDs:

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN && python3 -B datagen/morrison_worker_images.py --images morrison-abi3 --payload sim/payloads/sys/morrison_bulk_worker_scheduled_smt1.bin --helper work/tools/libmorrison_locality.so --placement-seed 0x20261011 --reduced morrison-reduced-later --output work/morrison-later-images'
```

Omit `--reduced` to retain the original base state. The writer rejects an
existing output, verifies base image identities and records payload/helper,
placement, state and output identities. It does not infer the new payload's
scheduling or certify runtime validation; select the payload with the build
profile and validate execution separately. It preserves the base's encoded
warmup/timed window: the retained base has the historical **0+100 pilot**.
This command generates images with the historical window; new benchmarks
follow [sim.md](sim.md) and write run data under `benchmark/rundir/`.
For another approved window, first produce a canonical ABI3 base with explicit
lengths using `morrison_bulk.py --reuse-graph ... --dispatch-width 32
--dispatch-policy balanced --forward-completion tree --warmup ... --timed ...`.

`datagen/morrison_checkpoint.py` extracts reduced state from a native checkpoint
using the retained two-pass, hash-checked transport. Supply `--state`, `--graph`,
`--output`, the matching `--spec` JSON (or file), and preferably
`--expected-sha256`. An explicit `--host` plus `--remote-graph` supports the
read-only SSH exporter; remote input requires the expected state-manifest hash.
The spec file may contain a top-level `spec` object. Native state and graph
must preserve their manifest binding and `after-synapses-before-neurons`
boundary. `morrison_checkpoint_layout.py` overlays the reduced weights/neuron
state on unchanged canonical geometry, resolving recorded old paths through
`workload_paths.py`. These tools import an already advanced physical state;
they do not synthesize a new neural fast-forward trajectory.

Retained assets are `morrison-native`, `morrison-abi3` and
`morrison-reduced-later`; small conversion/worker fixtures live under
`workloads/data/fixtures/`. The full later native checkpoint is still remote;
the local reduced representation does not replace every native array.
New full image generation needs substantial disk space beyond the immutable
inputs. Never write into the retained inputs or their hard-linked archive.

`morrison.py`/`morrison_native.py` retain original graph/checkpoint import.
ABI1 G1/G4 targets remain functional-test prerequisites. `morrison_bulk.py`
produces ABI3; `morrison_worker.py` supplies the ABI4 layout library.
`morrison_validate.py` validates ABI1–3 images. The separate
`sim/tests/morrison_worker_check.py` decodes small ABI4 physical layouts and
checks forward/reverse bijection, graph, weights and neuron state.
Existing numerical/runtime validators remain in `sim/tests/` and
`datagen/test_morrison*.py`; the full-network pending-input-sum oracle remains
an acknowledged gap.

[workloads.md](workloads.md) records the historical 171,237.41 and 148,950.85
cycles/timestep pilots at different starting states. They use soft-model
MemIf write merging, which has no equivalent RTL implementation. The selected
payload has not adopted the new async-copy or global-idle notifications;
soft-model alignment and regenerated RTL are still needed for that work.
Complete former ABI1/ABI2 notes, previous comparisons and run identities are
preserved in [morrison.md](../archive/workload/doc/morrison.md) and
[morrison-bulk.md](../archive/workload/doc/morrison-bulk.md) in the archive.
