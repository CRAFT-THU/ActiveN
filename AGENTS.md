# ActiveN Agent Guide

This file applies to the entire repository unless a more specific `AGENTS.md`
exists below a directory.

## Mandatory interaction and Git rules

These rules take priority over convenience, speed, and historical instructions
under `copilot/`.

1. **Never modify Git history.** Do not create, amend, delete, squash, rebase,
   reset, or otherwise rewrite commits. Do not create, delete, or move branches
   or tags, and do not force-push. Read-only Git operations such as `status`,
   `diff`, `show`, `log`, `blame`, and inspecting old commits are allowed. Leave
   repository changes for the user to review and commit.
2. **Never begin implementation without explicit user consent.** First perform
   read-only investigation, form a concrete plan, discuss it with the user, and
   wait for the user to explicitly approve carrying out that plan. An initial
   request to fix or optimize something is not approval of a plan that has not
   yet been presented. Do not edit files, regenerate committed artifacts, or
   run commands with intended source-tree side effects before approval. If the
   approved approach must materially change, pause and obtain approval for the
   revised plan. The temporary review artifacts described below are the sole
   exception to the pre-approval file-write restriction.
3. **Ask questions whenever intent is unclear.** The user knows the architecture
   and implementation details. Do not guess about protocol semantics,
   microarchitectural intent, timing boundaries, topology, workload behavior,
   or whether suspicious RTL is intentional. Discuss doubts early.
4. **Obtain an independent review before plans, results, and documentation.**
   Follow the review procedure below before presenting any plan or revised
   plan, after every test or benchmark run and before reporting its results,
   before presenting a final summary, and before modifying any document.
   Review supplements explicit user approval; it never replaces it.
5. **Use realistic benchmark defaults and separately approve exceptions.**
   Follow the benchmark contract under "Building simulators". Approval of an
   implementation plan does not approve deviations from benchmark defaults.

### Independent review procedure

- Write the exact pending plan, summary, or proposed document update to a
  temporary file under `copilot/tmp/` before its review. For documentation,
  include the proposed diff and sufficient surrounding text. Include the
  task requirements, scope, assumptions, relevant source/configuration paths,
  and unresolved questions so the review is self-contained.
- After every test or benchmark run, include the actual commands, working
  directory, relevant environment overrides, exit status, complete raw logs,
  configuration, input/checkpoint identities, measurement boundaries, raw
  counts, units, and calculations. Link the execution and analysis scripts,
  their relevant dependencies, and the configuration files used for the run.
  Identify the versions actually used, retaining hashes or frozen copies when
  files may change; an edited current script is not evidence of a past run's
  settings. Include failures and incomplete runs; do not select only favorable
  evidence or omit parameters merely because the summary does not mention them.
- Launch a fresh subagent with the main agent's model and reasoning effort and
  **without conversation history**. With the current collaboration tool, use
  `fork_turns="none"` and inherit the parent's model/effort without overrides.
  Supply the review packet and primary evidence paths, not the conversation
  transcript or a demand to confirm the main agent's conclusion.
- Give the reviewer a bounded, independent task: inspect the pending content
  and its evidence for logical inconsistencies, obvious implementation errors,
  infeasibility, unrealistic or unrepresentative parameters, unmatched workload
  functionality or starting states, and mismatched units, timing windows,
  denominators, frequencies, or comparison standards. Require concrete findings
  with evidence and corrections, or an explicit statement that no blocking
  issue was found, plus any uncertainty or unverified claim.
- The reviewer must open and inspect **every linked execution/analysis script,
  configuration file, and raw log**, following relevant dependencies needed to
  establish the run's behavior. Review the artifacts themselves, not just the
  main agent's summary or selected excerpts. For large logs, read-only parsing
  and searches may be used, but coverage must include the complete relevant
  records and error/completion paths rather than a favorable sample.
- Independently reconstruct the effective parameters from script defaults and
  conditional paths, actual command-line arguments, environment overrides,
  generated configuration/manifests, and startup logs. Compare them with the
  approved/intended settings, including settings omitted from the summary.
  Distinguish requested settings from evidence that they actually took effect.
- Independently verify output interpretation and **recompute reported results
  from raw evidence**. Check analysis/parser logic, field meanings, units,
  warmup exclusion, timer boundaries, timestep counts, denominators, averaging,
  and comparison calculations. Reusing the same analysis script alone is not
  an independent check of that script's interpretation or arithmetic.
- Record which artifacts were inspected, the effective settings established,
  and which results were independently reproduced. Identify missing,
  inaccessible, stale, truncated, or unread evidence as explicit review gaps.
  A discrepancy or missing evidence needed to establish a claim blocks that
  claim; do not clear it merely because the summary is internally consistent.
  The main agent may report the limitation without presenting the affected
  claim as verified.
- The reviewer inspects source and evidence without modifying project files,
  running tests/benchmarks, or spawning further reviewers. Read-only analysis
  of existing artifacts is permitted, with temporary analysis scripts and
  outputs kept under `copilot/tmp/`; this does not authorize rerunning the
  workload or test. It may also write its review report there. These temporary
  analysis artifacts, review packets, and reviewer reports are
  temporary process artifacts: creating or revising them is permitted before
  plan approval and does not itself trigger another documentation review.
  Delivering the reviewer's own report or verdict is also exempt from this
  gate; the main agent's plans, summaries, and document edits remain gated.
- While review runs, continue independent read-only investigation, but wait for
  the review before presenting the gated content or applying a document edit.
  Resolve blocking findings, revise the pending content, and obtain a fresh
  review of material changes. Retain the packet and review together; disclose
  unresolved limitations rather than describing the review as proof of
  correctness. Routine progress updates must not assert unreviewed conclusions.
- One review may cover a plan, document diff, and summary together only if it
  explicitly checks each current version. New test/benchmark evidence or a
  material revision requires another review before the affected content is
  presented or written. User approval is still required for implementation
  and destination-document edits under rule 2.
- If a fresh reviewer with matching model/effort is unavailable, stop the gated
  action, explain the limitation, and ask the user how to proceed; do not
  silently skip review or substitute a weaker reviewer.

## Working scope and repository hygiene

- `README.md` is for first-time human readers. Keep agent-specific operational
  guidance in this guide and workload status/evidence in `doc/workloads.md`.
  Change the README only when the user explicitly requests a README change.
- The primary implementation areas are `ActiveN/`, `datagen/`, and `sim/`;
  `fpga/` contains the target-board documentation, schematic, and archived
  Vivado reference projects used for hardware deployment.
- Read `fpga/board.md` before FPGA integration or board bring-up work. It
  summarizes the FACE-VU13P-C resources, clocks, DDR channels, interfaces,
  reference projects, test flow, and known documentation inconsistencies.
  Verify exact electrical and pin details against the schematic and relevant
  reference-project XDC.
- `dep/cvfpu/` is vendored FPU RTL. Avoid changing it unless the task explicitly
  requires a vendor modification.
- `generated/` contains elaborated SystemVerilog and configuration JSON. Never
  hand-edit generated files; change Chisel and regenerate them.
- `sim/build/`, `datagen/target/`, `work/`, and most of `copilot/tmp/` are build
  or experiment artifacts, not source of truth.
- The worktree may already be dirty. Start with `git status --short`, inspect
  relevant diffs, preserve user changes, and never revert unrelated work.
- Put traces, logs, temporary scripts, generated debug data, and other
  intermediate artifacts in `copilot/tmp/`. Artifacts with established
  destinations stay there: generated RTL in `generated/`, payload binaries next
  to payload sources, simulator binaries in `sim/build/`, and Rust outputs in
  `datagen/target/`.
- Historical notes under `copilot/` are useful research material, but many files
  describe obsolete executables, environment variables, build targets, or old
  task instructions. Cross-check every historical claim against current source.

## Project overview

ActiveN is a parameterized many-core neuromorphic processor implemented in
Chisel. Each processing unit is an RV32I-derived core with optional floating
point support, shared integer/floating-point register semantics, two SMT pipes
by default, custom active-message instructions, and local scratchpad memory.

The repository has three main maintained layers:

1. **Core RTL** - fetch, decode, execution, register files, LSU, FPU, scratchpad,
   and the active-message/event machinery.
2. **System RTL** - a mesh NoC, one router per PU, memory-controller interfaces,
   response distributors, a response ring, and a peripheral endpoint.
3. **Software tooling** - the single-core and system simulators, the cycle-aligned
   software NoC backend, bare-metal payloads, and the SNN DRAM-image generator.

The local `generated/system/System.config.json` currently describes a
64-PU, 2-MC, 4-GiB-per-MC system with two SMT pipes. This is a useful regression
configuration, not a universal architectural constant. Always inspect the
generated JSON before building or running a system simulator.

## Repository map

| Path | Purpose |
| --- | --- |
| `ActiveN/src/main/scala/koneko/Main.scala` | Elaboration CLI and config JSON emission |
| `ActiveN/src/main/scala/koneko/Parameters.scala` | Core parameters and elaboration-time invariants |
| `ActiveN/src/main/scala/koneko/Core.scala` | Single-core top-level wiring |
| `ActiveN/src/main/scala/koneko/System.scala` | Topology generation and full-system wiring |
| `ActiveN/src/main/scala/koneko/Data.scala` | Shared core-side bundles and micro-op fields |
| `ActiveN/src/main/scala/koneko/BIU.scala` | Active-message queues, broadcast mapping, quotas, and scheduling |
| `ActiveN/src/main/scala/koneko/fetch/` | Fetch, I-cache, and decode |
| `ActiveN/src/main/scala/koneko/exec/` | Execute, LSU, SPM, register file, multiply, AMO, and FPU |
| `ActiveN/src/main/scala/koneko/bus/` | Router, flits, arbiters, encoder, crossbar, MemIf, and distributor |
| `doc/` | Intended memory, message, deadlock, and SNN formats |
| `doc/workloads.md` | Selected workloads, retained results, integration status, and artifact dependencies before archive migration |
| `sim/src/single.cpp` | Single-core simulator |
| `sim/src/system.cpp`, `system.h` | Shared system frontend, memory model, CLI, and backend contract |
| `sim/src/soft_backend.*` | Software NoC backend driver and topology |
| `sim/src/soft_components.h` | Software queues, arbiters, flits, and routers |
| `sim/src/soft_mem.h` | Software MemIf, scatter, distributor, and ring behavior |
| `sim/src/devices.h` | Simulation peripheral and MMIO behavior |
| `sim/gen_system_header.py` | Generates the hard backend adapter and config header |
| `sim/CMakeLists.txt` | Verilator, simulator, PGO/LTO, and BOLT build |
| `sim/payloads/` | Bare-metal single-core and system workloads |
| `datagen/src/main.rs` | SNN generation and per-MC DRAM image emission |
| `fpga/board.md` | FACE-VU13P-C board capabilities, interfaces, examples, and bring-up notes |

## Sources of truth

- Use `doc/` and direct user clarification for architectural intent. Some RTL
  may be buggy and therefore may not express the intended design.
- Use current Scala/C++/Rust source for current implementation behavior.
- Use `generated/*/*.config.json` for the exact elaborated simulator
  configuration.
- Use `fpga/board.md` as the board overview, then consult the board schematic
  and reference-project XDC files for authoritative electrical and pin details.
- Use waveforms and ready-valid handshakes as the ground truth for cycle timing.
- Never copy an old `copilot/` command or expected result without verifying it
  against the current CLI and payload.

## RTL architecture and invariants

### Core data path

- `Core.scala` wires `Fetch` to `Exec`.
- The I-cache and LSU share the outgoing memory encoder through `Crossbar`.
- The encoder's memory flits and BIU active-message flits arbitrate onto
  `ext.out`; memory traffic has static priority.
- External unicast messages enter a priority-aware `FlitQueue` before BIU
  ingestion.
- Ordinary memory responses return through `mem.unicast`; scatter/broadcast
  responses use the dedicated decoupled `mem.broadcast` path.
- `CoreParameters.pipeCnt` defaults to 2. Do not assume one thread because old
  scripts and notes frequently used `AN_PIPE_CNT=1`.

### Active messages

- A flit is one packet containing 16-bit `src`, 16-bit `dst`, a 12-bit tag, and
  four 32-bit payload words.
- Sending behaves like an RPC with up to four arguments carried in `a0`-`a3`.
- Handler metadata lives in custom CSRs; see `doc/msg.md` before modifying ABI,
  scheduling, handler masks, margins, or quotas.
- Destination 0 means a local send. PU IDs are otherwise 1-based.
- There are 16 handler/event queues. Local push, unicast ingress, and broadcast
  ingress arbitrate into them in that priority order.
- The top two tag bits define four NoC priority levels; numerically smaller is
  higher priority.
- Queue-space reservation and handler margin/quota logic are part of deadlock
  avoidance, not merely performance policy. Read `doc/deadlock.md` and ask the
  user before changing queue depths, priorities, margins, or scheduling.

### System topology

- `SystemParameters` requires power-of-two PU and MC counts, at least 16 PUs per
  MC, and one memory-size entry per MC.
- PU count must form either a square mesh or a 2:1 rectangular mesh.
- PU IDs are 1-based and assigned along a Hilbert curve.
- Every 16 consecutive IDs form a cluster.
- Clusters are divided into contiguous MC zones. Each cluster contributes one
  MC request connection at the PU closest to the mesh center.
- Routing is deterministic XY: column first, then row. The mesh has no wraparound.
- Each PU router has local core injection/ejection. Some routers also have a
  local MC eject; PU1 has the peripheral eject.
- Peripheral destination ID is `0x8000`; MC destination IDs begin at `0x8001`.
- Memory responses do not return through the normal NoC. They travel through
  per-MC response logic, the inter-MemIf ring when needed, and one distributor
  per 16-PU cluster.
- The response ring order is MC0, MC1, ..., last MC, peripheral, then back to
  MC0.

### Memory protocol

- Address map:
  - `0x20000000-0x3fffffff`: scratchpad
  - `0x40000000-0x7fffffff`: peripheral/MMIO
  - `0x80000000-0xffffffff`: global memory
- Memory flit classes are selected by the low tag byte:
  - `0x00`: scalar load
  - `0x01`: scalar store
  - `0x10`: CSR/scatter load with broadcast response
  - `0x11`: bulk load with unicast response
- Higher tag bits are available for priority and are ignored by the MemIf class
  decoder.
- Global memory requests use 512-bit lines in the current configuration and
  8-bit external request IDs.
  Scalar IDs have bit 7 clear; bulk/scatter IDs have bit 7 set and encode an
  inflight beat slot.
- Only one bulk request is active in each `DRAMIf`; its issue, response, and
  retirement counters are timing-sensitive.
- `CoreParameters` enforces queue and width constraints. Preserve those checks,
  and do not bypass them with casts or generated-RTL edits.

### Ready-valid discipline

- Presented `valid` and data must not depend on downstream `ready`.
- State changes only when the corresponding handshake fires.
- Do not assume a decoupled producer is irrevocable unless the interface
  explicitly guarantees it.
- Router local-port ordering, forwarding-table indices, distributor backpressure,
  and ring arbitration are positional and timing-sensitive.
- Use `@instantiable`, `Definition`, and `Instance` patterns consistently for
  the replicated core. This is important for generated-RTL deduplication.

## Simulation peripheral

The peripheral base is `0x40000000`.

| Offset | Access | Meaning |
| --- | --- | --- |
| `0x00` | write | End simulation; 0 is success, nonzero is failure |
| `0x04` | write | 1 starts the timer, 0 stops it |
| `0x08` | write | Output low byte as ASCII |
| `0x0c` | write | Print an auxiliary 32-bit result |
| `0x10` | read/write | Read RNG value or reseed `std::mt19937` |

The configuration ROM is reached through peripheral space:

- `0x68000000`: number of PUs
- `0x68000008`: number of MCs
- `0x68000010`: PUs per MC
- `0x68000018`: log2 of the memory-line size in bytes
- `0x68000020`: per-MC size as a little-endian 64-bit value

The default RNG seed is `0x19260817`; both simulators accept `--rng-seed`.

## Development environment

Sessions run as root inside an isolated NixOS container. Commands that do not
risk deleting project files are permitted, and `nix-env` may be used to install
missing utilities as needed. This permission does not override the mandatory
plan-approval rule for source-tree side effects or the prohibition on modifying
Git history.

All supported project build tools are provided by the Nix development shell.
Run build, test, simulator, RISC-V toolchain, Verilator, Mill, Cargo, and FST
commands through:

```sh
nix develop --command bash -c "cd /root/workspace/ActiveN && <command>"
```

Prefer the development shell for project toolchains. Install an additional
package with `nix-env` only when the needed utility is not already available.

## Elaboration and generated RTL

Current entrypoint:

```sh
# Single-core RTL and generated/core/Core.config.json
nix develop --command bash -c \
  "cd /root/workspace/ActiveN && mill ActiveN.run --core"

# Example full system and generated/system/System.config.json
nix develop --command bash -c \
  "cd /root/workspace/ActiveN && mill ActiveN.run --system --pu=64 --mc=2"
```

`Main.scala` also accepts `--output=<path>` and passes arguments after `--` to
the Chisel stage.

After any Scala RTL change, regenerate every affected target:

- Core-only simulator behavior: regenerate `--core`.
- System or shared core behavior: regenerate `--system`.
- A core RTL change normally affects both simulators, so regenerate both.

Do not use the old `mill Koneko.run`, `AN_SYSTEM`, `AN_NUM_PU`, `AN_NUM_MC`, or
`AN_PIPE_CNT` flow found in `README.md`, `scripts/build.sh`, or old
`copilot/quickstart.md`; it is stale. Use this guide and the current CLI source
for agent workflows.

## Building simulators

For the existing configured build tree:

```sh
nix develop --command bash -c \
  "cd /root/workspace/ActiveN && ninja -C sim/build -j\$(nproc)"
```

For a fresh build tree:

```sh
nix develop --command bash -c \
  "cd /root/workspace/ActiveN && cmake -S sim -B sim/build -G Ninja && ninja -C sim/build -j\$(nproc)"
```

Important build details:

- `sim_single` Verilates `generated/core/`.
- The hard system backend Verilates `generated/system/`.
- Those directories are CMake defaults. Use
  `-DGENERATED_SYSTEM_DIR=<absolute-path>` to select a separate system RTL and
  `System.config.json` directory for both the hard backend and soft-core
  extraction. `-DGENERATED_CORE_DIR=<absolute-path>` selects the single-core
  RTL and `Core.config.json` directory. Match these paths to the elaborator's
  `--output=<path>` and inspect the selected configuration before building.
- The soft backend also extracts `Core` from `generated/system/`, not
  `generated/core/`, so hard and soft cores use the same parameters.
- The system-level simulator can be built with the full hard backend enabled
  or disabled using CMake's `-DENABLE_HARD_BACKEND=ON|OFF` switch. Disabling
  it skips elaboration and linking of the Verilated full `System` model; the
  resulting `sim_system` binary supports `--soft` and rejects `--hard`.
- CMake reads `generated/system/System.config.json`. Re-run CMake when PU/MC
  counts or other configuration values change; an incremental Ninja build is
  normally enough for RTL changes at the same configuration.
- BOLT is enabled by default. Its system profiling command expects generated
  SNN images under `copilot/tmp/snn_shuffle/`. For an ordinary fresh debug build
  where post-link optimization is irrelevant, configure with
  `-DUSE_BOLT=OFF`.
- Build outputs are `sim/build/sim_single` and `sim/build/sim_system`.
- The old standalone `sim_soft` and `sim_cosim` executables no longer exist.
  `sim_system --soft`, `--hard`, or both selects the backend mode.

System-level regression tests generally use the 64-PU/2-MC configuration
represented by the Scala system-test defaults and should run in `--soft --hard`
cosimulation mode so that soft/hard request and timing alignment is checked.
Workload benchmarks such as MNIST and Brunel target the 2x-HBM2-scale
configuration with 512 PUs, 16 MCs, and a 256-bit external memory bus (32-byte
memory lines). These workload simulators may be built with
`-DENABLE_HARD_BACKEND=OFF`. Benchmark runs must use `--soft --soft-threads 8`
without `--hard`; `--init-icache` is optional when a preloaded instruction cache
represents the intended measurement. Record the preload choice and region;
preloading does not replace workload warmup. Eight host threads are simulator
parallelism, not the core's SMT pipe count. Hard/cosim validation remains
required where specified below and is distinct from performance benchmarking.

**Every workload benchmark must enable Ramulator HBM2 timing**, including quick
benchmarks. Pass `--ramulator-config sim/build/ramulator-hbm2-1ch.yaml` (or a
hash-recorded copy of that configuration), specify the core frequency, verify
the startup `Ramulator enabled` message and 32-byte transaction width, and
retain per-MC memory statistics. `--soft` and `ENABLE_HARD_BACKEND=OFF` do not
disable or replace Ramulator. Flat memory is only for explicitly labelled
functional tests or diagnostic controls; never present it as target-memory
benchmark performance.

Brunel benchmarks start from the recorded settled GeNN checkpoint for each
rule, retaining the exact graph and physical state. A **quick benchmark** has
60 warmup plus **1,000 measured timesteps**; a **full benchmark** has 60 warmup
plus **10,000 measured timesteps**. At 0.1 ms/timestep, these measure 100 ms and
1,000 ms, respectively. Always collect macroscopic dynamics for the same
measured interval: separate E/I firing rates, synapse-update opportunities and
final weight mean/stddev, alongside phase and total cycles/timestep. Keep payload
diagnostics disabled and use passive observations to avoid changing the timed
instruction stream. Compare equal windows and starting states, especially when
instruction scheduling or FP reordering changes the activity.

### Benchmark contract

- Choose the most realistic parameters for the documented workload and target
  hardware by default, including DRAM timing, PU/MC counts, memory capacity and
  bus width, core frequency, workload size, connectivity, and physical state.
  The workload requirements above apply even to quick benchmarks. Do not use
  the smaller regression configuration or legacy script defaults as implicit
  benchmark defaults; pass the required parameters explicitly and verify the
  effective configuration in generated JSON, manifests, and startup logs.
- Use the soft backend only, eight host threads, and the target DRAM timing
  model as specified above. Retain per-MC memory statistics and identify their
  measurement interval; use `--ramulator-stats-timer-only` when it matches the
  verified workload interval. Keep required hard/cosim correctness checks as
  functional validation rather than benchmark performance claims.
- Include workload-appropriate warmup timesteps before the measured interval,
  even when instruction-cache preload is used. Declare warmup and measured
  lengths and exact start/end conditions in advance. Use the Brunel lengths
  and checkpoints above for Brunel; clarify unspecified workload requirements
  with the user rather than guessing. Measure the complete declared interval,
  including quiet and busy timesteps.
- Always use **average end-to-end simulated cycles per timestep** for benchmark
  performance comparisons. Here, **end-to-end means the workload's measured
  interval as delimited by the simulator timers, excluding warmup**. Compute
  `C_timer / N_measured`, where `C_timer` is the total simulator timer cycles
  accumulated for that complete measured interval and `N_measured` is the
  number of completed measured timesteps in the same interval. Include all
  phases, communication, synchronization, DRAM stalls, drain, and boundary
  overhead counted within that timer interval. Retain the raw timer total and
  timestep count alongside the average.
- Verify the timer start/stop boundaries against the current payload and
  simulator, and confirm the timer count and timestep denominator cover the
  same declared measured interval, excluding warmup. If the workload stops and
  restarts its timer per timestep, sum the recorded measured timer intervals;
  do not add untimed gaps or initialization outside those intervals. Do not
  substitute whole-run simulation cycles, phase sums, selected-step averages,
  sums of per-PU cycles, host wall time, or simulator cycles/second for the
  workload timer metric. Compare matching timer conventions; clarify ambiguous
  boundaries with the user rather than silently changing them. Phase averages
  and derived percentages may supplement the end-to-end average; they do not
  replace it. Label frequency, timestep duration, and any unit conversions
  explicitly; do not invent comparable cycle counts for an external result
  whose timing basis is unknown.
- Compare equivalent functionality, inputs/graph, starting physical state,
  seeds, numerical rules, timestep duration, and measurement boundaries except
  for the explicitly investigated variable. Check output and activity metrics
  over that same interval so a reduction in work is not misreported as an
  implementation speedup. If windows, workload semantics, or units cannot be
  reconciled, report the limitation and withhold the performance comparison.
- Any temporary departure from these benchmark defaults requires its own
  explicit user approval **separate from plan approval**, before the run.
  State the reason, exact old/new values, affected cases, expected effect on
  realism/comparability, and how the departure ends. Use invocation-local
  overrides or isolated temporary configurations under `copilot/tmp/`; do not
  change persistent defaults to make an experiment convenient. Record the
  approval and overrides with the results, label diagnostic controls, and
  return to the documented defaults for subsequent runs. A failed or expensive
  run is not permission to reduce fidelity or shorten the measurement window.

## Simulator architecture and timing contract

### Single-core simulator

`sim_single` owns one Verilated core, 16 MiB of flat memory, the peripheral, and
a simple event loop.

```sh
sim/build/sim_single --max-cycles 500000 sim/payloads/asm/<test>.bin
sim/build/sim_single --max-cycles 500000 sim/payloads/c/<test>.bin
```

Useful options are `--trace`, `--log`, `--max-cycles`, and `--rng-seed`.
Tracing writes `trace.fst` in the current directory.

### Unified system simulator

`sim_system` has one frontend and one or more backends:

- **Hard backend**: the complete Verilated `System` RTL.
- **Soft backend**: one Verilated `Core` per PU plus C++ models of routers,
  MemIf, distributors, response ring, and peripheral path.
- **Cosim**: both backends in lockstep, sharing the same frontend memory input.

The CLI requires the elaborated shape explicitly:

```sh
sim/build/sim_system \
  --hard \
  --pu 64 --mc 2 --mc-size 0x100000000 \
  --max-cycles 100000 \
  <mc0-image> <mc1-image>
```

Rules:

- Specify at least one of `--hard` or `--soft`.
- Supply exactly one positional image per MC.
- With `--hard`, `--pu`, `--mc`, and `--mc-size` must exactly match the
  compiled `System.config.json`.
- Flat memory is the default, so a benchmark must explicitly select HBM2.
  `--ramulator-config` enables timing in the shared frontend. CMake exports
  `sim/build/ramulator-hbm2-1ch.yaml` from `sim/ramulator-config/hbm2-1ch.py`
  for the 32-byte-line workload target; the separate `ramulator.yaml` comes
  from `sim/ramulator-config/hbm1-8ch.py` and is not its substitute.
- `--ramulator-stats <dir>` writes one YAML statistics file per MC and requires
  `--ramulator-config`.
- `--freq-ghz`/`-f` sets the core and NoC frequency in GHz and defaults to 1.
  The frontend uses this value and Ramulator's `tCK` to schedule memory ticks.
- `--trace --trace-start <cycle>` writes `trace.fst`.
- `--log` enables periodic soft-backend statistics.

The frontend cycle order is:

```text
++cycle
peek all backends and compare
tick frontend memory
build bus inputs and serve accepted requests
stage all backends
dump trace once
step all backends
```

`SystemBackend` semantics:

- `peek(K)` reads state after the previous posedge and presents memory requests.
  It must not consume frontend input or advance state.
- With Ramulator enabled, request readiness is the direct result of Ramulator's
  request submission API. The frontend must not queue rejected requests.
- `stage(K)` supplies request readiness and responses, drives core inputs, and
  settles combinational/negedge behavior without committing registered state.
- `step(K)` commits the posedge and all transfers resolved for cycle K.

The FST tracer is registered once at the top level. System waveform time `t=K`
corresponds to cycle K, with exactly one dump between `stage(K)` and `step(K)`.
Do not add per-backend top-level registrations or extra dumps.

### Cosimulation

When both backends are enabled, the hard backend is constructed first and its
request is the frontend ground truth after comparison.

Cosim compares:

- request presence
- address
- write flag
- size
- write byte enable and write data for stores

It intentionally ignores request ID because hard and soft backends use different
internal ID formats.

Debug controls:

- `COSIM_VERBOSE=1`: print observed per-port requests.
- `COSIM_KEEP_GOING=1`: continue after mismatches.

There are `numMC + 1` frontend ports: port 0 is peripheral, and port `1+i` is
MC `i`.

### Soft NoC alignment invariants

As of 2026-10-14, the system-wide idle notifier is implemented in the system
RTL, while the soft peripheral MemIf still bases idleness on its own allocated
requests. Alignment of that behavior remains pending; a successful build alone
does not establish cosimulation correctness. `doc/workloads.md` records the
retained configurations and other outstanding workload integration work.

- Treat the hard RTL waveform as timing ground truth during an alignment task.
- Do not modify RTL merely to make the soft model agree. If RTL looks wrong,
  stop and ask the user.
- Only undertake alignment as an explicitly approved task. Uncore RTL changes
  commonly require later soft-model realignment.
- The soft topology must mirror `Topology.build`: Hilbert numbering, active
  mesh directions, router port ordering, nearest MC connection, XY routing,
  cluster boundaries, and ring order.
- `IndexVector` and most PU-facing soft structures use 1-based indexing.
- Collect presented core/router values before the posedge. Never read a
  post-posedge core output and treat it as the value that entered a queue on
  that same edge.
- Resolve all ready-valid fires before `step`; keep each component's step atomic.
- Preserve explicit register boundaries even if collapsing them is functionally
  equivalent. A one-cycle response-visibility change breaks alignment.
- Keep soft cores sourced from the system elaboration so configuration cannot
  silently diverge from the hard backend.

## Building payloads

The payload Makefile uses the bare-metal `riscv64-unknown-none-elf-` toolchain.

```sh
# All discovered asm and C single-core tests
nix develop --command bash -c \
  "cd /root/workspace/ActiveN/sim/payloads && make all-tests"

# Common system tests
nix develop --command bash -c \
  "cd /root/workspace/ActiveN/sim/payloads && make sys-tests"

# Targets not included in sys-tests
nix develop --command bash -c \
  "cd /root/workspace/ActiveN/sim/payloads && \
   make sys/csr_scatter_test.bin sys/csr_scatter_dram1.bin sys/snn_main.bin NUM_PU=64"
```

Important:

- `NUM_PU` defaults to 16 and is used by `snn_main`; pass the elaborated PU count.
- `CORE_CNT` defaults to 512 for legacy `test.S`; do not confuse it with current
  system payload configuration.
- Single-core assembly tests use RV32IMF; C tests use RV32I and `c/crt0.S`.
- System tests are linked at `0x80000000` and use custom active-message
  instructions.
- System success is a zero write to `0x40000000`. Auxiliary computed values are
  normally printed through `0x4000000c`.

## System test images

For the current 64-PU/2-MC configuration:

| Test | MC0 image | MC1 image | Suggested limit |
| --- | --- | --- | ---: |
| `barrier_test` | `sys/barrier_test.bin` | same | 100,000 |
| `local_send_test` | `sys/local_send_test.bin` | same | 100,000 |
| `no_receipt_store_test` | `sys/no_receipt_store_test.bin` | same | 100,000 |
| `csr_scatter_test` | `sys/csr_scatter_test.bin` | `sys/csr_scatter_dram1.bin` | 100,000 |
| `pingpong_test` | `sys/pingpong_test.bin` | same | 500,000 |
| `random_noc_test` | `sys/random_noc_test.bin` | same | 500,000 |
| `sha256_concurrent` | `sys/sha256_concurrent.bin` | same | 500,000 |
| `spm_init` | generated `dram.0` | generated `dram.1` | 1,000,000 |
| `snn_main` | generated `dram.0` | generated `dram.1` | 20,000,000 |

Example cosim:

```sh
nix develop --command bash -c \
  "cd /root/workspace/ActiveN && \
   sim/build/sim_system --hard --soft \
     --pu 64 --mc 2 --mc-size 0x100000000 \
     --max-cycles 100000 \
     sim/payloads/sys/csr_scatter_test.bin \
     sim/payloads/sys/csr_scatter_dram1.bin"
```

Do not pass raw `spm_init.bin` or `snn_main.bin` directly for their full system
tests. Datagen embeds the executable and data into the per-MC DRAM images.

Prior sessions recorded approximate flat-memory hard/cosim completion cycles for
the 64-PU/2-MC configuration: barrier 1.2K, local send 0.2K, CSR scatter 1.8K,
ping-pong 86K, random NoC 51K, SHA-256 48K, SPM init 371K, and SNN 3.81M. Treat
these as regression hints only; verify current payloads, parameters, and
simulator source before declaring a mismatch.

## Datagen

Build:

```sh
nix develop --command bash -c \
  "cd /root/workspace/ActiveN && cargo build --release --manifest-path datagen/Cargo.toml"
```

Important arguments:

- `--core-cnt` is required.
- `--num-mc` defaults to 1.
- `--spm-size` defaults to 16384.
- `--mem-line-width` is specified in bits and defaults to 512.
- The main generation seed defaults to decimal `19260817`.
- `--dump-shuffle-seed` independently controls CSR-entry shuffling and requires
  `--dump`.
- `--text` embeds a payload in `dram.0`.
- `--load-nest-nodes`, `--load-nest-conns`, `--sudoku`, `--mnist`, and
  `--dump-genn` select alternate input/output modes.

Reproducible 64-PU/2-MC SPM-init image:

```sh
nix develop --command bash -c \
  "cd /root/workspace/ActiveN && \
   datagen/target/release/datagen \
     --core-cnt 64 --num-mc 2 --spm-size 16384 \
     --pre-simulate 1 --tot-neuron 65344 \
     --text sim/payloads/sys/spm_init.bin \
     --dump copilot/tmp/spm_init_dram"
```

Reproducible shuffled SNN image:

```sh
nix develop --command bash -c \
  "cd /root/workspace/ActiveN && \
   datagen/target/release/datagen \
     --core-cnt 64 --num-mc 2 --spm-size 16384 \
     --pre-simulate 1 --tot-neuron 65344 \
     --text sim/payloads/sys/snn_main.bin \
     --dump-shuffle-seed 0 \
     --dump copilot/tmp/snn_shuffle"
```

DRAM image layout:

- `dram.0` begins with a 16-byte header: jump instruction, SPM-init offset,
  number of MCs, and number of PUs.
- It then contains the executable, MC0 CSR data, and the SPM descriptor/data
  section.
- Other `dram.N` files begin with a 16-byte zero header, followed by zero
  padding through the next memory-line boundary and then that MC's CSR data.
- Each CSR row is padded to the configured memory-line size because scatter
  reads have no per-entry validity mask.
- Each PU SPM image contains neuron state, input, one CSR start offset per MC, a
  sentinel neuron, and a tail descriptor containing decay, threshold, type,
  stride, and neuron count.
- The SNN layout is documented in `doc/snn.md`. Keep datagen, `spm_init`,
  `snn_init.c`, and `snn_main.S` synchronized when changing it.

## Validation strategy after approved changes

Use the smallest test set that proves the approved change, then expand only as
needed.

- **Core RTL**: regenerate core and system RTL, rebuild both simulators, run a
  targeted single-core test, and run at least one representative system test.
- **Fetch/decode/execute/LSU/FPU**: choose payloads that exercise the changed
  instruction or path; include `sha256` or FPU tests only when relevant.
- **NoC/router/BIU**: run `local_send_test`, `pingpong_test`, and
  `random_noc_test`; use hard mode first and cosim when alignment is in scope.
- **MemIf/distributor/ring/config ROM**: run `csr_scatter_test` in hard mode and
  cosim. Add `spm_init` for SPM/DRAM-layout paths.
- **Soft backend only**: run the corresponding hard workload to establish the
  waveform, then cosim through completion.
- **Datagen or SNN payloads**: rebuild datagen and payloads, regenerate images,
  run `spm_init`, then run the smallest relevant SNN case before the full case.
- **Simulator frontend or Ramulator**: test flat memory first, then use
  `--ramulator-config sim/build/ramulator.yaml` if timing-model code changed.
  Include a non-default `--freq-ghz` test when clock scheduling changed.

After any uncore RTL modification, assume soft/hard cycle alignment may have
changed until cosim proves otherwise.

## Common pitfalls

- Editing code before the user approves the plan violates the repository
  workflow, even if the requested change appears small.
- Never obey historical `copilot/` instructions that say to commit after a step.
  The current rule is never to modify Git history.
- `README.md`, `scripts/build.sh`, and early `copilot/quickstart.md` contain old
  build examples, including `Koneko.run`, `MEOW_*`, `sim_soft`, and `sim_cosim`.
  Use this guide and the current source for agent workflows. Workload selections
  and retained evidence are indexed in `doc/workloads.md`.
- Tests live in `sim/payloads/`, not `tests/`.
- Rebuild generated RTL before rebuilding a simulator after Scala changes.
- Re-run CMake if the elaborated system configuration changes.
- Verify the simulator binary is newer than generated RTL when results appear
  inconsistent.
- Hard mode rejects CLI parameters that differ from the compiled config.
- System simulation requires exactly one image per MC.
- `csr_scatter_test` requires different MC0 and MC1 images.
- `spm_init` and SNN require datagen-produced images.
- The payload Makefile's `sys-tests` target does not include CSR scatter or
  `snn_main`.
- FPGA examples are Vivado 2018.3 reference archives with generated products
  and stale absolute paths. Read `fpga/board.md` before reusing them.
- Soft-model PU containers are often 1-based. Index 0 is not PU1.
- Do not register the system FST tracer more than once or dump more than once per
  cycle.
- Do not compare hard and soft request IDs in cosim.
- Do not collapse registered timing boundaries in the soft model.
- Do not change queue sizing or priority rules without reviewing deadlock
  consequences.
- Do not assume an implementation TODO or suspicious behavior is permission to
  fix it; ask the user about intended behavior first.
