# Full Morrison workload

<a id="selected-configuration"></a>

As of **2026-10-14**, this Morrison optimization round is complete. Retain **SMT1-only reverse relay** as the best measured configuration from this round. This is the ABI4 distant G32 variant with individual synapses randomly assigned to workers and stored in their worker MCs (placement seed `0x20261011`). “SMT1-only” applies to reverse relay handler 4; shared updater handler 12 remains enabled on both SMT threads. Proxy acceptance uses handler 11 and the read issuer uses handler 8. The selected payload is `sys/morrison_bulk_worker_scheduled_smt1.bin`.

The selected implementation retains 1 KiB bulk reads, two 2 KiB forward banks reused as 64 reverse software descriptors, a ready queue with 16 read credits, posted weight stores, and the experimental forward transition without receiver counting. The recorded performance includes the timestep marker/MC-idle drain. The recorded MemIf write merging is a soft-model experiment tied to the retained simulator and payload.

Both checkpoint comparisons used the full network and 512 PUs, sixteen 1 GiB MCs (two 8 GiB HBM2 stacks), 32-byte transactions, two SMT pipes, 1 GHz, Ramulator HBM2, soft backend with eight host threads, 256 MemIf slots, NoC reservation multiplier 1, 8 KiB instruction-cache preload and external drive. Each used the approved **0 warmup + 100 measured timesteps** pilot window, or 10 ms biological time. These pilots do not replace the standard benchmark windows below. The primary metric is the complete peripheral timer divided by 100, including all measured synchronization and drain overhead.

| Starting checkpoint | Policy | Timer cycles / 100 steps | Cycles/timestep |
| --- | --- | ---: | ---: |
| Settled, absolute tick 12,000,000 | **SMT1-only relay** | 17,123,741 | **171,237.41** |
| Settled, absolute tick 12,000,000 | Updater11 / relay12 | 17,280,278 | 172,802.78 |
| Diagnostic step 750,000, absolute tick 12,750,060 | **SMT1-only relay** | 14,895,085 | **148,950.85** |
| Diagnostic step 750,000, absolute tick 12,750,060 | Updater11 / relay12 | 14,989,157 | 149,891.57 |

Updater11/relay12 costs **0.9142%** more cycles in the first pair and **0.6316%** more in the second. The preference repeats across these two starting states, although the advantage is small. Compare policies within each checkpoint; different activity across checkpoints is not an implementation speedup or evidence of a universal optimum.

For the step-750,000 comparison, the phase breakdown is:

| Policy | Integration | Forward | Reverse | Finish/drain | Other timer overhead |
| --- | ---: | ---: | ---: | ---: | ---: |
| SMT1-only relay | 12,732.61 | 41,766.12 | 84,968.58 | 9,460.46 | 23.08 |
| Updater11 / relay12 | 12,732.89 | 41,604.25 | 86,064.47 | 9,466.88 | 23.08 |

All entries are cycles/timestep. Updater11/relay12 adds 1,095.89 reverse cycles and saves 161.87 forward cycles, for a net cost of 940.72 cycles/timestep.

The matched activity and final weight statistics for each 100-step window are:

| Starting checkpoint | E / I spikes | E / I rate (Hz) | LTD / LTP opportunities | Final weight mean / stddev (pA) |
| --- | ---: | ---: | ---: | ---: |
| Absolute tick 12,000,000 | 2,276 / 778 | 2.528889 / 3.457778 | 20,482,463 / 20,484,000 | 44.497198122 / 3.960250737 |
| Diagnostic step 750,000 | 1,927 / 696 | 2.141111 / 3.093333 | 17,334,233 / 17,343,000 | 44.482373707 / 3.984935992 |

In the second pair, the initial physical state and graph match apart from policy code. Both produce identical spike sets in every timestep, 29,498,313 forward deliveries and 17,343,000 reverse visits; all **810 million final weights match bit for bit**. Neuron states have small floating-point differences. Each run passes its own integration/trace/RNG and per-PU delivery/copy/cleanup checks; the full-network checker does not independently sum every pending input or establish a tolerance between the two trajectories. The [reviewed comparison](../copilot/tmp/morrison-checkpoint-750000-20261013/results-report.md) records the numerical differences and validation limits.

The step-750,000 updater11/relay12 capture was interrupted during final archival. Its completed rerun used bounded host archive buffers and a 26 GiB process-group limit, reproducing the original timer, all 400 phase markers and all 100 SPM snapshots exactly. The interrupted capture is retained as incomplete evidence; only the completed rerun supplies the final comparison.

The [selected run recipe](../copilot/tmp/morrison-checkpoint-750000-20261013/relay_smt1/layout-recipe.json) pins the ABI4 layout, handler policy and payload SHA-256 `c0df21626f61674a82d0b6bb5aa4f5a012056535652a4cc237b699e0c95c15b4`. [Checkpoint selection](../copilot/tmp/morrison-checkpoint-750000-20261013/selection.json), [complete metrics](../copilot/tmp/morrison-checkpoint-750000-20261013/results-updater11_relay12_recovered2.json), [pair validation](../copilot/tmp/morrison-checkpoint-750000-20261013/comparison.json) and the [independent results review](../copilot/tmp/morrison-checkpoint-750000-20261013/results-review.md) retain source identities and raw-evidence paths. The [earlier checkpoint review](../copilot/tmp/morrison-update-before-relay-20261013/results-review.md) covers the first pair. The measured optimization round is closed. Reproduction with newer RTL and
backports to Brunel remain separate work; these historical pilot figures do not
measure those changes.

## Selected source and integration status

The selected build target is `sys/morrison_bulk_worker_scheduled_smt1.bin` in
`sim/payloads/Makefile`. It uses `brunel.S`/`brunel_runtime.S` with the bulk,
ready-queue and tree includes. `BR_BULK_WORKER=1` selects ABI4;
`BR_BULK_SCHEDULED=1` and `BR_BULK_RELAY_SMT1=1` select the shared updater and
SMT1 relay policy. The worker layout comes from `datagen/morrison_worker.py`;
its `WorkerLayout` is a library, not a standalone image-generation CLI.
The selected case's recorded launcher and `layout-recipe.json` supply the
canonical ABI3 base, placement seed, helper library and checkpoint overlay.
The later checkpoint also requires `checkpoint_stream.py`,
`checkpoint_layout.py` and the reduced state under
`copilot/tmp/morrison-checkpoint-750000-20261013/`.

Write merging is enabled by `SOFT_MEMIF_WRITE_MERGE=1` in these retained runs.
It remains an opt-in software MemIf experiment; the RTL has no equivalent
merge path. The selected payload still uses its copy checker and marker/MC-idle
completion protocol. The new async-copy and system-wide idle interfaces are
present in current RTL but have not been integrated into this workload. The
soft backend's global-idle alignment and refreshed generated targets remain
necessary for that later work. See [the workload overview](workloads.md) for
the shared integration status and archive dependencies.

## Historical ABI1 baseline

The remaining layout details and commands describe the original ABI1 baseline
and its historical results. The [bulk variant document](morrison-bulk.md)
distinguishes current ABI3/ABI4 targets from the historical ABI2 design.
The ABI1 build/image commands below do not recreate the selected ABI4 result.

The [bulk coroutine variant](morrison-bulk.md) adds whole-row range delegation and prefetch across jobs. The G1/G4 layout below remains the ABI1 baseline. Its home-update stores are posted, but its finish path waits only for read completions and software slots; it does not implement an explicit NoC/MemIf drain. ABI2 now supplies that timestep boundary with route markers followed by MC idle notifications. Historic passing runs do not by themselves prove the missing ABI1 ordering guarantee.

The full Morrison port targets the model in `../stdp-experiments/workloads/morrison.py`: 90,000 excitatory and 22,500 inhibitory neurons, fixed incoming degrees of 9,000 E and 2,250 I, and 0.1 ms timesteps. Connectivity is sampled with replacement without recurrent autapses. Of the 1,265,625,000 recurrent synapses, 810,000,000 E→E synapses are plastic. External Poisson input is generated locally and has no stored adjacency matrix.

The target has 512 PUs, sixteen 1 GiB memory controllers (two 8 GiB HBM stacks), 32-byte memory lines, two SMT pipes, 16 KiB scratchpad and 8 KiB instruction cache per PU. The ordinary `generated/system` configuration is a separate regression target. The Morrison build below uses its own generated directory.

The port preserves the approved Brunel arithmetic and event order: integrate neurons, deliver the old weight and apply quiet-post LTD, process post-active LTD followed by LTP, then increment traces. Zero configured delay means delivery is consumed on the next tick. This differs from the reference's exact update/delivery order. Future validation against the baseline must compare macroscopic dynamics over matching intervals and starting physical states.

The reference still uses an alpha-current LIF neuron. Its synaptic time constant is 0.33 ms; replacing the alpha filter with a voltage-only model would change the network. The port therefore retains voltage, combined E/I alpha current and its derivative. Equal E/I synaptic time constants allow those filters to share two state variables. The 80-byte Brunel record also contained metadata and redundant fields that do not need to remain in scratchpad.

Each new neuron record occupies 32 bytes:

| Byte offset | Field | Reason |
| --- | --- | --- |
| 0 | FP32 voltage | Membrane state |
| 4, 8 | FP32 current and derivative | Alpha filter state |
| 12, 16 | FP32 X/Y coefficients | Two learning-scaled traces for E neurons |
| 20, 24 | FP32 E/I pending input | Independent accumulators used by delivery handlers |
| 28, 29 | Byte refractory count / fired flag | Separate byte accesses preserve both fields |
| 30–31 | Padding | Keeps the stride and reverse-address encoding aligned |

Inhibitory neurons retain the same stride but do not update learning traces. GIDs derive from the PU and local slot. Outgoing row descriptors live in HBM and are fetched only for fired sources. RNG state is per SMT worker. The default placement puts 219 or 220 neurons on each PU, requiring at most 7,040 bytes of neuron state.

The complete scratchpad allocation is below. Ends are exclusive; offsets are relative to `0x20000000`. Hardware event queues, registers and instruction cache are separate resources.

| Offsets | Bytes | Allocation |
| --- | ---: | --- |
| 0000–0100 | 256 | Header, constants, counters, worker RNG states |
| 0100–1c80 | 7,040 | 220 neuron records |
| 1c80–1ff0 | 880 | 220 fired-neuron pointers |
| 1ff0–2000 | 16 | Alignment |
| 2000–2040 | 64 | Proxy group configuration |
| 2040–2720 | 1,760 | 880 two-byte G4 job IDs |
| 2720–2790 | 112 | Duplicate-detection bitmap |
| 2790–27a0 | 16 | Alignment |
| 27a0–27c0 | 32 | One streamed row descriptor |
| 27c0–2900 | 320 | Unallocated |
| 2900–2d00 | 1,024 | Sixteen 64-byte MSHR entries |
| 2d00–2d80 | 128 | Four directory/refill lines |
| 2d80–2e00 | 128 | Existing runtime scratch reservation |
| 2e00–2e20 | 32 | Model/topology/ABI parameters |
| 2e20–2e40 | 32 | Alignment |
| 2e40–2e90 | 80 | Twenty Poisson cutoffs |
| 2e90–3700 | 2,160 | Unallocated |
| 3700–3f00 | 2,048 | Two 1 KiB streaming halves |
| 3f00–4000 | 256 | Two guarded 128-byte SMT stacks |

`sim/payloads/sys/morrison_layout.h` defines ABI version 1 (`MR_MAGIC=0x4d4f5231`). G1 and G4 use the same compact layout. G4 assigns equal outgoing-row quotas to four consecutive PUs with a locality preference. Job IDs encode slot+1; the maximum is 880. The row's 32-byte HBM descriptor contains GID, plastic base/count and static/reverse directory pointers. Home PUs retain trace ownership.

Descriptor scalar refills and forward asynchronous copies share the live `ac.sBase`. The runtime waits for scalar ID-2 completion before changing that base; waiting only for the AC counter is insufficient. Forward data uses bounded ping-pong staging, and post-active updates use the existing MSHRs. Handler priorities, margins, quotas, queues and the twelve-request credit limit are unchanged. Four receipt/expected counters reset during integration after the preceding step's global completion, preventing cumulative overflow. Per-step full-network edge counts fit uint32.

Plastic rows retain 128-edge full chunks. In the final partial chunk, weight and target arrays are independently padded to 32 bytes, and the copy length and target offset follow that actual size. The old unused 64 KiB-per-PU spill reservation is absent. Static scatter and reverse entries remain eight bytes each; there is no new hardware scatter format.

For the generated G4 graph with seed 92713 and graph SHA-256 `fbbd03d8de0fb56afa3162300508cba0394196a56a51847b01d1bbcb67833183`, all images together occupy 16,794,867,616 bytes (15.641439 GiB). They leave 367.166107 MiB in aggregate. The largest image is MC0 at 1,058,062,656 bytes, leaving 15,679,168 bytes (14.952820 MiB) in that controller. These figures include padding, row metadata, directories, code and SPM initialization. Each controller must fit independently; the aggregate margin cannot be reassigned between controllers. The generator rejects an overflowing image, and the simulator enforces physical request bounds in optimized builds. This establishes capacity for the recorded graph/layout, not every possible graph or measured throughput.

The imported settled native checkpoint at tick 12,000,000 (1,200 s) uses state-manifest SHA-256 `7598a8351bc690fb099db5f36fe9891c3a6159dcc6a6ab034a6bf3f689608b80` and canonical graph SHA-256 `c672d31e75e5637cc59fb289d3f8d66f58da1a8a62f8f494c8cd52dea179b7a4`. Its G4 images occupy 16,794,934,720 bytes (15.641502 GiB). MC0 remains the tightest controller: 1,057,995,648 bytes used, leaving 15,746,176 bytes (15.016724 MiB). This is a separate graph from the earlier fresh seed-92713 capacity case. Input provenance, conversion errors and per-controller sizes are retained in the [imported manifest](../copilot/tmp/morrison-native-20260929-v1/settled-g4/manifest.json), with complete generation and edge-validation records in that task directory.

Model constants are separate from sparse Brunel: initial E weight 45.61 pA, inhibitory ratio 5, external rate 20,880 Hz/neuron, both trace constants 20 ms, depression ratio 0.1057, learning rate 0.1, exponent 0.4 and no upper weight cap. Stored plastic weights are delivery coefficients `W=(e/0.33)*w`; LTP uses the correspondingly scaled coefficient. The sampler uses an inclusive inverse CDF over nonzero xorshift32 draws with twenty cutoffs, including counts above nine, at 32-bit uniform resolution.

Build the payloads and target through the development shell:

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN/sim/payloads && make sys/morrison_g1.bin sys/morrison_g4.bin'

nix develop --command bash -c 'cd /root/workspace/ActiveN && JAVA_TOOL_OPTIONS=-Xmx10g mill ActiveN.run --system --pu=512 --mc=16 --mc-size=0x40000000 --mem-bus-width=256 --icache-lines=128 --output=generated/morrison-512'

nix develop --command bash -c 'cd /root/workspace/ActiveN && cmake -S sim -B sim/build/morrison-target -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DENABLE_HARD_BACKEND=OFF -DUSE_BOLT=OFF -DUSE_LTO=OFF -DGENERATED_SYSTEM_DIR=/root/workspace/ActiveN/generated/morrison-512'

nix develop --command bash -c 'cd /root/workspace/ActiveN && ninja -C sim/build/morrison-target -j 4 sim_system'
```

`--mc-size` accepts a power-of-two byte size, in decimal or hexadecimal, up to 4 GiB. Its default remains 4 GiB for existing targets. Verify the generated JSON before running. The G1/G4 payload code sizes in the recorded build are 4,992/5,016 bytes, within the 8 KiB code/cache contract. Payload diagnostics are disabled.

To generate normal time-zero input images with the standard quick window:

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN && python3 datagen/morrison.py --fresh --output copilot/tmp/morrison-images --group 4 --seed 20260724 --warmup 60 --timed 1000'

nix develop --command bash -c 'cd /root/workspace/ActiveN && python3 datagen/morrison_validate.py copilot/tmp/morrison-images'
```

The output directory must not exist. Graph generation, sorting and reverse-index construction use bounded host-memory blocks and disk-backed arrays; allow substantial temporary disk space beyond the approximately 16 GiB final images. The manifest records the complete model, graph and image hashes, payload identity, state provenance, topology, measurement window and component/per-controller bytes. The validator decodes every edge, checks fixed indegrees and parallel-edge weights, and verifies a unique reverse reference to every plastic edge. Weight statistics from this image validator are in stored delivery-coefficient units; `morrison_results.py` converts final moments to pA.

`datagen/morrison_cold.py SOURCE OUTPUT` can reuse a full fresh graph/image with normal time-zero voltages and zero currents, traces, inputs and refractory state. It copies MC0 and shares immutable remaining images through symlinks. It rejects checkpoint-weight sources and disabled external drive. It does not generate a checkpoint or rewind settled weights. Its defaults are also 60 warmup and 1,000 measured steps. Graph and state seeds are recorded separately when they differ.

Checkpoint generation belongs to the baseline run. `datagen/morrison.py --checkpoint PATH --output OUTPUT --group 4 --warmup 60 --timed 1000` accepts either its native state directory or a complete portable NPZ. The input checkpoint and graph are read without modification.

Native directories use `genn-native-phase-checkpoint-v1`. The state manifest names the graph directory and its manifest hash; relative paths such as `../graph` resolve from the state directory. Keep both directories when copying a checkpoint. The importer verifies the manifest contract and all forty array files against their recorded SHA-256, byte size, dtype and shape. It requires the matching model, FP32 scalars, exact uint32 neuron clocks, zero axonal/back-propagation delays and the `after-synapses-before-neurons` boundary.

The native graph stores padded source rows. The importer removes padding using `rowLength`, preserving the order and individual weight of every real edge, including parallel edges. It rebuilds ActiveN's reverse references from that forward graph; the native reverse remap is hash-checked but is not used to construct ActiveN's reverse index. Native lazy traces are traversed directly in bounded blocks, avoiding three extra full-size unpadded trace arrays on disk.

Native `outPost` arrays contain pending EPSC-scaled delivery coefficients. They enter ActiveN's pending input fields directly, without another scaling or delivery pass. Voltage, refractory state and the combined E/I alpha current/derivative are retained. Native spike counters, tie-order bookkeeping and CUDA RNG state are checked as input files but do not replace ActiveN's approved event order or worker RNG.

The portable NPZ path remains supported and requires:

- Complete source-major `ee/ei/ie/ii_pre` and `_post` arrays, individual FP32 `ee_g`, and all four pending `_input` arrays.
- E/I `V`, `Iex`, `Iin`, `dIex`, `dIin`, refractory state and one-dimensional uint32 per-neuron tick arrays.
- FP32 `ee_preTrace` and `ee_postTrace`, and uint32 `ee_lastTraceTick` arrays for every plastic edge.
- JSON metadata with format `brunel-genn-phase-checkpoint-v1`, boundary `after-synapses-before-neurons`, Morrison rule, FP32 precision, 0.1 ms timestep, zero configured delay, checkpoint timestep and a complete matching `model` dictionary from `Spec.model()`.

Both paths normalize lazy trace copies to the checkpoint boundary using 20 ms for each trace, then check every real copy against the reconstructed FP32 per-neuron trace. The allowed absolute error is `2e-5 * max(1, abs(trace))`; the manifest records the observed maximum and RMS errors. Future ActiveN random draws use independent per-SMT xorshift32 streams. `benchmark_ready` identifies a complete checkpoint import, not verified macroscopic agreement with GeNN. Synthetic small test checkpoints are functional fixtures and are not baseline checkpoints.

Host validation includes a padded native fixture with nonzero currents and pending inputs, varied lazy timestamps, clocks beyond FP32's exact integer range, distinct weights on parallel edges and invalid padding. It checks analytic state expectations, native/portable image equivalence, and rejection of corrupt or incompatible inputs. Run both test modules with new artifact directories:

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN && MR_TEST_DIR=copilot/tmp/morrison-unit MR_NATIVE_TEST_DIR=copilot/tmp/morrison-native-unit python3 -B -m unittest discover -s datagen -p "test_morrison*.py" -v'
```

For a standard target measurement, use a new output directory and the full declared interval:

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN && mkdir -p copilot/tmp/morrison-run'

nix develop --command bash -c 'cd /root/workspace/ActiveN && SOFT_BRUNEL_DUMP=copilot/tmp/morrison-run SOFT_BRUNEL_SUMMARY_ONLY=1 SOFT_MNIST_PHASE_LOG=1 sim/build/morrison-target/sim_system --soft --soft-threads 8 --pu 512 --mc 16 --mc-size 0x40000000 --freq-ghz 1 --ramulator-config sim/build/morrison-target/ramulator-hbm2-1ch.yaml --ramulator-stats copilot/tmp/morrison-run/ramulator --ramulator-stats-timer-only --init-icache=0x80000000 --max-cycles 1000000000000 copilot/tmp/morrison-images/dram.{0..15} > copilot/tmp/morrison-run/simulator.log 2>&1'

nix develop --command bash -c 'cd /root/workspace/ActiveN && python3 datagen/morrison_results.py copilot/tmp/morrison-images copilot/tmp/morrison-run --log copilot/tmp/morrison-run/simulator.log --system-config generated/morrison-512/System.config.json --freq-ghz 1'
```

Retain exact commands, exit statuses, environment overrides, raw logs, configuration and source/binary hashes. Require the startup `Ramulator enabled` line with 32-byte transactions and all sixteen MC statistics. The example preloads `[0x80000000,0x80002000)`; cache preload does not replace warmup. Passive observations produce per-step E/I counts, spike IDs and phase markers without adding payload instructions. `SUMMARY_ONLY` omits per-step SPM snapshots but retains the final initialized DRAM images for full weight moments. Omit it for functional state-oracle runs.

Add `SOFT_BRUNEL_PROFILE=1` to collect passive PC, PU, MC and memory-event profiles. These files can be large; reserve space for them and the complete final DRAM dump. Filter per-step profiles to the measured steps. PC observations sample active-thread fetch PCs every 256 simulated cycles and give approximate code attribution. Profile phase/completion boundaries supplement the exact peripheral timer; their totals must not replace that timer or be treated as identical measurement windows.

The performance metric is total peripheral timer cycles divided by all completed measured timesteps. The timer excludes warmup and includes the measured phases, barriers, drain and boundary overhead; boot before the timer is excluded. Report the raw total and denominator, frequency, E/I rates, LTD/LTP opportunities, final weight mean/stddev and per-MC statistics over that same interval. Phase differences are supplementary. Standard windows remain 60+1,000 or 60+10,000. Any shorter startup window requires separate approval under `AGENTS.md`; zero initial learning traces also make the first step skip effective weight updates. No cold-start result should be presented as settled throughput or a matched baseline comparison.

Functional evidence retained under `copilot/tmp/morrison-port-20260928-v1/` includes five host tests, SPM initialization in hard/cosim, seven-step mixed G1/G4 cosim, a two-step all-neuron/all-job occupancy cosim, and a two-step full-network HBM2 functional run. The mixed cases also checked G1 hard execution, parallel weights and partial tails; four pre-existing Brunel binaries remained byte-identical. The occupancy case exercises all 220 neuron records and all 880 G4 job slots per PU. The full controlled case uses native external drive and nonzero learning traces; its oracle checks neuron fields, jobs, receipts, drain, guards, 18,049 touched weights, and unexpected writes within retained initialized backing images. Dumps do not cover unused physical memory. These checks do not establish macroscopic agreement with the rerunning baseline.

The initial full fixture manifests retain a generic cold-state description despite their diagnostic overrides. For those archived runs, the fixture script, neuron-state hash and review records establish the actual controlled state. New fixtures record their override explicitly.

The recorded G4 run starts from the normal time-zero state and graph seed 92713 described above. It completes 60 warmup plus 1,000 measured steps: 6 ms of warmup followed by 100 ms of biological time. The target is 512 PUs, sixteen 1 GiB MCs, 32-byte Ramulator HBM2 transactions, two SMT pipes and a simulated 1 GHz core/NoC clock. It uses eight host threads, the documented instruction-cache preload and passive observations. The simulator and frozen result analyzer both exit successfully.

| Measured quantity | Result |
| --- | ---: |
| Peripheral timer cycles | 297,286,009 |
| Completed measured timesteps | 1,000 |
| End-to-end cycles per timestep | 297,286.009 |
| Simulated hardware time at 1 GHz | 297.286009 ms |
| Biological time / simulated hardware time | 0.336376 |
| E / I spikes in the measured interval | 71,974 / 17,860 |
| E / I mean firing rate | 7.997111 / 7.937778 Hz |
| Presynaptic LTD / postsynaptic LTP opportunities | 647,757,628 / 647,766,000 |
| Forward deliveries | 1,010,642,447 |
| Final plastic-weight population | 810,000,000 |
| Final weight mean / population standard deviation | 45.608418636 / 0.156668809 pA |

The timer starts at cycle 7,539,713 and stops at 304,825,722. It excludes warmup and includes all measured steps and their drain/barrier overhead. Phase-marker spans average 12,912.777 integration, 157,090.007 forward, 126,158.580 reverse and 1,096.633 finish cycles per timestep. Another 28.012 cycles per timestep lie outside those spans. Forward and reverse together account for 95.28% of the timer interval; these phase intervals include processing and communication and do not isolate a particular hardware bottleneck.

All sixteen MC statistics cover the same timer interval. They total 648,843,998 accepted reads, 457,881,324 accepted writes and 1,106,725,322 completions, with zero timed outstanding requests at stop. Counting each accepted 32-byte transaction gives 119.128412 GB/s in decimal units over the measured interval. This is accepted transaction traffic, not useful synaptic payload bandwidth.

An additional scan compares every final plastic weight with its input value. Across all 1,060 evolved steps, including warmup, 287,879,287 distinct weights end with different FP32 values: 144,392,290 lower and 143,486,997 higher. All final weights are finite and nonnegative. This confirms effective learning during the run; it counts net differences from initialization, not update events or differences confined to the measured interval.

The simulator loop takes 45,076.798 host seconds, about 12 h 31 min 17 s. Host execution time is distinct from simulated hardware time; final dumping and analysis follow the loop. Exact commands, environment overrides, exit statuses and frozen sources are in the [run record](../copilot/tmp/morrison-port-20260928-v1/cold-standard-v2/run.json). The [complete simulator log](../copilot/tmp/morrison-port-20260928-v1/cold-standard-v2-data/simulator.log), [result analysis](../copilot/tmp/morrison-port-20260928-v1/cold-standard-v2-data/analysis.log), [per-MC statistics](../copilot/tmp/morrison-port-20260928-v1/cold-standard-v2-data/ramulator/) and [weight-change scan](../copilot/tmp/morrison-port-20260928-v1/weight-changes-v1/raw.log) retain the raw evidence.

The full network fits the memory target and completes the measured run. At the simulated 1 GHz clock, this cold-start measurement runs at 0.336 times biological real time. It does not establish settled throughput, a G4/G1 speed comparison or agreement with the reference. The baseline checkpoint and equivalent-window reference results remain necessary for the settled comparison.
