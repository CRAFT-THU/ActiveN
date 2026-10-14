# Sparse MNIST workload

Sparse MNIST shares `sim/payloads/sys/mnist_train.S` with dense MNIST and selects
`mnist_sparse.inc` at compile time. It uses the same three trace rules and
0.5 ms timestep. Selected images use eight horizontal groups, compact scheduling,
reverse setup extraction and the bit-seeded fifth-root helper. Relevant masks,
MC drain and active-prefix experiments are disabled in the selected recipes.

| Rule | Connectivity | Selected reverse mode |
| --- | --- | --- |
| One trace | 12.5%, 25%, 50% | Neuron scatter (3) |
| Two or three traces | 12.5% | Neuron scatter (3) |
| Two or three traces | 25%, 50% | Dense reverse map (4) |

Both mode builds remain available for matching comparisons. Mode 3 carries
weight locations with the reverse events; mode 4 uses the dense reverse map.
The bounded-tail binary-search switch only affects modes 1/2. Those older
modes remain supported in the shared source for functional checks/research.

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN/sim/payloads && make -f selected.mk sys/mnist_sparse_two-trace_mode4.bin'
```

Change the rule and mode in the filename as required. Build all selected
rule/mode combinations with `make -f selected.mk selected-workloads`. The full profile matrix is
in [workloads/profiles.json](../workloads/profiles.json).

`datagen/mnist_sparse.py` accepts the corresponding `--rule`, `--payload`,
`--reverse-mode neuron-scatter` (3) or `--reverse-mode dense` (4),
`--connection-rate`, and `--runtime-snapshot`. Give an explicit measured window
and a fresh output. Use `work/` for functional images or `benchmark/rundir/` for
benchmark images, following [sim.md](sim.md). Preserve the realized graph and snapshot;
specifying the same connectivity rate alone does not reproduce a graph.
The current weight cap is per-post `float32(784 / fan_in[j])` for nonempty
columns, with zero for an empty column. Keep `--weight-max-policy fan-in`.
Do not normalize weights when continuing a captured runtime snapshot.

Nine retained corpora have asset IDs `mnist-sparse-<rule>-<rate>`, using rates
`0.125`, `0.25` and `0.5`. Each has 100 snapshots: 70 presentation and 30 rest.
The historical comparison used four warmup plus twelve measured steps per
snapshot in flat memory. [workloads.md](workloads.md) gives the selected means;
[the archived manifest](../archive/workload/doc/mnist_sparse_baseline.json)
and `archive/tmp/mnist-sparse-baseline-full-20260913/` retain every case.

`datagen/mnist_sparse_state.py` decodes logical state, and
`mnist_sparse_reference.py` independently replays FP32 learning. Preserve
weight/input/trace checks and activity when comparing modes.
`test_mnist_sparse.py` and `test_mnist_weight_caps.py` cover generator contracts.
`mnist_sparse_benchmark.py` remains an explicit-parameter replay tool; its
legacy defaults do not establish new benchmark settings.

The completed relevant-mask/MC-drain pilot remains unselected, with its old
restart-era summary preserved. Nibble skipping and the pilot's disposition
are still open. New global idle and async-copy notification integration also
remains pending. Detailed ABI and all superseded experiments are preserved in
[the pre-cleanup document](../archive/workload/doc/mnist_sparse_train.md).
