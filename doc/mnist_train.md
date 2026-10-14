# Dense MNIST workload

The selected implementation is the shared assembly entrypoint
`sim/payloads/sys/mnist_train.S`, with neuron and fifth-root includes. It models
784 Poisson inputs, 400 excitatory and 400 inhibitory neurons at 0.5 ms per
step. The one-, two- and three-trace rules use compile-time selection.
[workloads.md](workloads.md) records retained performance and its limits.

The selected placement has 512 PUs/16 MCs, 288 input PUs, 224 neuron PUs,
`full-8` horizontal partitioning, `network-balanced` neuron zones, tree
recurrence, unbundled traces and the bit-seeded fifth-root helper. The generic
Makefile and image-generator defaults do not select this entire configuration.

Build one rule, or use `make -f selected.mk selected-workloads` for the full selected matrix:

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN/sim/payloads && make -f selected.mk sys/mnist_dense_three-trace.bin'
```

Replace `three-trace` with `one-trace` or `two-trace` for the other rules.
The profile filenames keep all rule binaries separate. The rule, recurrence,
trace bundling, fifth-root policy and horizontal group count must match the
image. Shared code also supports legacy compile-time experiment switches;
the selected fixed recipes are in `sim/payloads/selected.mk`.

`datagen/mnist_train.py` generates images. For a selected-layout snapshot import,
pass the matching payload and rule, `--input-pus 288 --neuron-pus 224`,
`--horizontal-partition full-8 --neuron-zone-policy network-balanced`,
`--recurrent-mode tree --fifth-root-seed bit`, and `--runtime-snapshot PATH`.
Pass warmup/timed lengths explicitly and choose a fresh output. Use `work/`
for functional images or `benchmark/rundir/` for benchmark images, following
[sim.md](sim.md).
Snapshot continuation cannot exceed the recorded window. The preserved corpus
uses four warmup and twelve measured steps per snapshot; that historical flat
measurement does not define the new benchmark policy in [sim.md](sim.md).

Retained input IDs are `mnist-dense-one-trace`, `mnist-dense-two-trace` and
`mnist-dense-three-trace`, plus `mnist-labels`. Use
`datagen/workload_paths.py --list` or [the catalog](../workloads/retained.json)
to find them. The three rules use separately trained snapshot corpora. Preserve
the snapshot, dataset, checkpoint and model hashes when continuing a run.
`datagen/mnist_genn_snapshots.py` retains snapshot selection/export machinery;
its GeNN environment still depends on the sibling baseline repository.

The image generator and `sim/payloads/sys/snn_init.c` define the input ABI;
assembly includes define the SPM/runtime layout. `datagen/test_mnist_train.py`
and `test_mnist_genn_snapshots.py` cover image construction, numerical helpers
and snapshot contracts. Snapshot log parsing lives in
`datagen/mnist_snapshot_benchmark.py`; historical runner defaults must not be
used as implicit parameters for a new benchmark.

The payload has not adopted the newer async-copy or system-wide idle
notifications. See [shared integration work](workloads.md#integration-and-validation-still-outstanding).
Complete placement/ABI explanations and the superseded experiment history are
preserved in [the pre-cleanup document](../archive/workload/doc/mnist_train.md).
