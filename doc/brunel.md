# Brunel workload

The selected sparse Brunel network has 9,000 excitatory and 2,250 inhibitory
neurons, fixed E/I indegrees 450/112, 4,050,000 plastic E→E synapses and a
0.1 ms timestep. It uses alpha-current LIF dynamics. The Additive and Morrison
learning rules share `sim/payloads/sys/brunel.S`, `brunel_runtime.S` and common
rule/layout includes. This small Brunel network with Morrison learning is
separate from [the full Morrison workload](morrison.md).

The selected proxy configuration is **consecutive home G4**. Weights live in
the assigned proxy's MC; learning traces remain authoritative at neuron homes.
The reverse event names its weight MC/address, and the home performs the
scalar read/update/posted store. Forward processing delivers the old weight
and applies quiet-post LTD; reverse processing applies post-active LTD then
LTP. The existing descriptor/notification drain completes before fired traces
are incremented. This is the retained protocol, before Morrison's newer
range-dispatch/shared-updater mechanisms.

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN/sim/payloads && make sys/brunel_additive_proxy_home_g4.bin sys/brunel_morrison_proxy_home_g4.bin'
```

Production diagnostics default to disabled. If overriding `BRUNEL_ASM_FLAGS`,
use a fresh build or `make -B` and preserve that binary separately. Generic
`brunel.bin` aliases and `brunel_<rule>_proxy.bin` do not select home G4: the
latter aliases the replicated G4 variant. All variants share the assembly and
are selected by compile-time switches; numerical self-tests remain separate.

Image construction uses `datagen/brunel.py`. For example, the new full
benchmark window (one warmup plus 10,000 measured steps) is encoded with:

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN && python3 -B datagen/brunel.py --rule additive --load-genn-checkpoint workloads/data/brunel/additive-settled-checkpoint.npz --presim 0 --proxy-groups consecutive --proxy-size 4 --proxy-updates home --warmup 1 --timed 10000 --payload sim/payloads/sys/brunel_additive_proxy_home_g4.bin --output benchmark/rundir/brunel-additive/images'
```

Use the Morrison-rule checkpoint/payload for that rule. The output must be
fresh. Follow [sim.md](sim.md) for simulator/memory settings and preserved
binaries; generating these images alone does not establish a compliant run.
Checkpoint import preserves the exact multigraph and physical state;
regenerating a graph from a seed is not equivalent. Do not use `--seek-active`
or CPU presimulation with a settled checkpoint. The manifest records the
input hash and the independent future ActiveN RNG stream.

The two checkpoints live under `workloads/data/brunel/` and are indexed as
`brunel-additive` and `brunel-morrison` in [the input catalog](../workloads/retained.json).
The GeNN settling and resume implementation is maintained under
[tools/checkpoints/brunel](../tools/checkpoints/brunel/README.md). It requires
its PyGeNN/CUDA scientific-Python environment; extraction did not rerun GPU
settling or change its model defaults.

Image checks live in `datagen/brunel_validate.py`; checkpoint validation is
in `brunel_checkpoint.py`. The FP numerical helper, assembly self-tests,
`test_brunel.py`, and `test_brunel_checkpoint.py` remain supported.
`brunel_reuse.py` remains a dependency of the tests. Superseded cache-policy
experiments have moved to `archive/workload/datagen/`.

[workloads.md](workloads.md) records 5,763.2097 and 7,652.3023 cycles/timestep
for the retained full HBM2 windows, together with timing/configuration limits.
Those results precede soft MemIf write merging and the new notification
interfaces. Backports and reproduction with current RTL remain pending.
The complete model constants, ABI tables, implementation notes and historical
measurements are retained in [the pre-cleanup document](../archive/workload/doc/brunel.md).
