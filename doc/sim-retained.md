# Retained simulator builds

Inventory checked on 2026-10-15. Preserve all existing `sim/build/` contents
until retained results have been reproduced or superseded. This records binary
identity and known roles; it does not certify each build directory as a complete
rebuild recipe or as the simulator used for every selected result.

| Existing path | Known relationship |
| --- | --- |
| `sim/build/morrison-phase-probe/sim_system` | Exact SHA-256 match for the later selected full-Morrison pilot at tick 12,750,060: `90c5d6321b004350ecb98a3d954057472deee0a1bb837e25a58bbce5486540f7` |
| `sim/build/morrison-functional/sim_system` | Exact match for the later Morrison functional/cosim evidence: `2b21a4bc4a7799841585bf7cb39492d775ced6b6c01a031f2dc9286cbf96e451` |
| `sim/build/sim_system` | Matches the archived local Brunel-era build (`37bef129c9a79a2c8f628d4d681e45d39938cc1eb146df35fd15daacd8bf7790`); the selected full HBM runs used the separate Zen2 build below |
| `sim/build/morrison-target/` | Retained 512/16 soft-only target build; its current binary is not an exact match to either selected pilot binary |
| `sim/build/memif-cosim-64/` | Retained 64/2 hard+soft build with 64-byte lines for MemIf validation; not the 32-byte-line workload target |
| `sim/build/sim_system_brunel_loadmanual_512`, `sim_single`, and other build products | Retained supporting/older artifacts; no selected-performance identity is asserted here |

The observed five CMake caches all have `USE_BOLT=OFF`; they do not satisfy the
new build policy merely because they remain useful historical evidence. Cache
files and generated headers can change independently of an executable; the
hash match is the evidence for the exact matches above.

The authoritative frozen executables for selected results remain under
`archive/tmp/`:

| Result | Frozen executable | SHA-256 |
| --- | --- | --- |
| Dense MNIST | `mnist-rule-native-30k-20260907/frozen/sim/build/sim_system` | `735dc39b58586e2ce993fe320a010a6bc3ff8b6005a0e1b9706578e180544953` |
| Sparse MNIST | `mnist-sparse-baseline-full-20260913/frozen/sim_system` | `b52178f4213fbde1b1b175177392e49ca8c78f02f11e1879219455f15c1c5295` |
| Brunel full HBM | `brunel-morrison-seeds-20260920/sim_system-zen2` | `93d5825c4c4a86cfb9e7a36208d9c86b2ec73517152789dc0d5d68cc9aadebd9` |
| Morrison earlier selected pilot | `morrison-scheduling-controls-20261012/smt1/sim_system` | `8892989b5234ebd694d56d2a4b812fac85a8014b27ed3bc87fb318f878536afc` |
| Morrison later selected pilot | `morrison-checkpoint-750000-20261013/relay_smt1/sim_system` | `90c5d6321b004350ecb98a3d954057472deee0a1bb837e25a58bbce5486540f7` |

Those hashes were checked against both the existing frozen files and the
retained command/provenance records. The dense/sparse MNIST and full Brunel
executables above do not match any inspected current `sim/build` simulator.
Full timing/context remains in [workloads.md](workloads.md) and the
[archive manifest](../archive/README.md).

The read-only inventory and matching records are retained under
`work/agent/simulator-policy-20261015/`. No simulator was rebuilt or run for this
inventory. Use [sim.md](sim.md) for future builds, measurements and permanent
`benchmark/` artifact locations.
