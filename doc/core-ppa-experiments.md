# Core SRAM and optimization experiments

These experiments target one two-SMT Core at 1 GHz, ASAP7 7.5-track RVT TT,
0.7 V and 25 °C. The eventual system is 512 PUs and 16 MCs; the standalone
Core retains its original one-MC address configuration. ICache remains one
64-byte line. Routers, distributors, MCs, a shared ICache interface, placement,
clock-tree synthesis and extracted wires are not included.

The subsequent 1.8 GHz sweep across RVT/LVT/SLVT and the available higher-voltage
fast corner is documented in [core-frequency-experiments.md](core-frequency-experiments.md).

## Completed comparisons

The final candidate uses explicit packed replication for CVFPU's pipeline stage
counts. It passes all nine baseline regressions with identical completion cycles,
results, timers and peripheral outputs, plus the soft-backend FIFO profiler check.
Cycle-by-cycle unit comparisons cover FIFO handshakes/data and FPU result latency.
This is simulation evidence, not a formal equivalence claim.

| Candidate | Cell + macro area (mm², 1×) | Worst setup slack (ps) | Reported total power (mW) | Logic + all net switching (mW), excluding instantiated SRAM internal/leakage |
| --- | ---: | ---: | ---: | ---: |
| Original FF memories, no retiming | 0.098529 | -904.88 | 209.620 | SRAM not instantiated |
| FF memories, retiming + automatic ungrouping | 0.097023 | -717.76 | 209.575 | SRAM not instantiated |
| SRAM scratchpad, FF queues, retiming | 0.042095 | -747.60 | 330.642 | 55.903 |
| SRAM scratchpad + SRAM event payloads, retiming | 0.033863 | -772.65 | 384.506* | 20.369 |
| Validated combined SRAM, access/logic gating, internal FPU stage, incremental retiming | 0.032162 | -32.68 | 5.884*† | 2.366† |

The two ungated SRAM total-power numbers include continually clocked macro models.
**The FIFO model's power is an uncalibrated placeholder, not a characterized
prediction.** Its 89.404 mW contribution explains why the second SRAM total
increases despite a large reduction in standard-cell power. Do not use that
384.506 mW total as a chip power estimate.

The scratchpad model contributes 274.740 mW internal power with 64 clocks
running every cycle. Its zero leakage is missing characterization, not a
prediction of zero physical leakage. The final candidate adds access-based
SPM/FIFO clock gating and automatic logic gating. DC identifies 375 gates:
279 tool-inserted gates and 96 explicit memory-port gates. It reports 17,355
of 17,520 registers gated (99.06%).

†The default 5.884 mW total and 2.366 mW logic estimate depend on DC's very low
inferred gate-enable activity. They are not representative active-power claims.
Use the explicit activity sensitivity cases below and the memory-model caveats.

For the old 28 nm comparison, the first two runs' arithmetic Core-minus-SPM
power is about 55.7 mW. The SRAM-boundary run gives 55.903 mW for the remaining
logic and all nets under these vectorless assumptions. These are estimates
using primary-input toggle rate 0.1 per clock, static probability 0.5, and DC's
low-effort activity propagation, which warns about unannotated sequential and
macro outputs. STA alone does not supply switching activity. The historical
50 mW number cannot establish a process-scaling ratio without matching activity
and implementation assumptions.

## Final power sensitivity

The following supplemental reports annotate **every ICG enable pin** with the
listed static probability, using 0.1 transitions per clock (zero transitions
for the always-enabled case). Other signal activity retains the baseline
assumptions. These are reproducible vectorless sensitivity cases, not a
measured workload or a calibrated fraction of CPU memory operations.

| Gate-enable probability | Logic + all nets (mW) | SPM internal (mW) | FIFO internal + placeholder leakage (mW) | Total model report (mW) |
| ---: | ---: | ---: | ---: | ---: |
| 0.01 | 2.145 | 6.957 | 5.455 | 14.557 |
| 0.10 | 4.122 | 27.727 | 11.981 | 43.830 |
| 0.25 | 7.870 | 69.057 | 24.819 | 101.746 |
| 0.50 | 14.128 | 137.519 | 46.519 | 198.166 |
| 1.00 | 26.355 | 274.675 | 89.353 | 390.383 |

The always-enabled case provides a useful clocking reference: **26.36 mW for
logic and all nets, excluding SRAM internal power/leakage**. It should not be
confused with the 5.884 mW default total. The FIFO power column remains
uncalibrated at every activity setting; its default leakage alone is
2.0624 mW. Actual macro characterization is required for a reliable total.

Final area is **32,161.776 µm²**, of which 22,894.933 µm² is macro area and
9,266.844 µm² is standard-cell area. This is 67.36% below the original FF Core.
The final run has 843 setup-violating endpoints, worst hold slack -19.22 ps,
and one transition-violating net. It is not timing-closed or a physical signoff
result. No multicycle timing exceptions were used to conceal the FPU path.

## Remaining timing paths

| Path segment | Worst setup slack (ps) |
| --- | ---: |
| Operation decode → FMA internal sum register | -32.68 |
| FMA internal sum → execute result register | -30.76 |
| Conversion control → internal exponent register | -10.10 |
| Conversion internal exponent → execute result register | -20.34 |

Both sides of the arithmetic pipeline remain critical. Increasing conversion
latency alone would not close timing. The internal stage replaces the former
wrapper output register, leaving the execute pipeline's cycle latency unchanged.
The setup-only period implied by the worst slack is about 1.033 ns (0.968 GHz)
under these same ideal-clock constraints; this is not a routed frequency guarantee.
Stage reports are in `copilot/tmp/ppa/core-validated/reports/*_{front,back}.rpt`.

### Paths outside CVFPU

A separate report-only audit excludes paths from/through/to sequential cells in
the FPU hierarchy; it does not alter constraints or add false paths. The worst
remaining path is **-32.57 ps**, from `exec/uop_alu2imm_reg` through integer
arithmetic, register-file write forwarding and message-argument readback to
`exec/directval_0_reg[30]`. Its listed path contains no FPU cells. CVFPU therefore
is not the sole 1 GHz bottleneck in this implementation.

| Endpoint group, excluding FPU register paths | Worst setup slack (ps) |
| --- | ---: |
| All remaining paths | -32.57 |
| Fetch/ICache | -30.16 |
| LSU (SPM write data) | -25.62 |
| Integer multiply | -16.25 |
| Core output ports | -7.53 |
| Register file | -4.54 |
| BIU | +0.53 |

These are endpoint groups, not isolated block intrinsic delays. For example,
fetch paths start in execute/redirect logic. SPM address setup has +266.25 ps,
but that does not establish all SPM input paths meet timing: write data is critical.
Source reports are in `copilot/tmp/ppa/nonfpu-audit-v2/reports/` and the archived
audit log confirms completion. Hierarchical exclusions can miss logic renamed by
retiming; the explicit non-FPU critical path above is the decisive evidence.

The assessment of additional frontend/backend clock gating and the proposed
trace-power measurement flow are in [simulation-power-plan.md](simulation-power-plan.md).

### Clock-gating and false-path audit

An independent audit reloads the mapped DDC without resynthesis, records timing,
then forces all 375 ICG `ENA` pins high with case analysis and repeats the checks.
Overall slack stays -32.68 ps. The four FPU segments above retain exactly the same
slacks with gates forced open. Their reports show launch at 0 ps and capture at
1000 ps on `core_clock`; there is no additional cycle allowance.

The saved SDC has one clock, reset deasserted, and a false path only from reset.
It has no FPU false paths, multicycle exceptions, clock groups or explicit timing
disables. Both audits report no unconstrained endpoints. Of the 189 arithmetic/
conversion register D pins inspected, 186 have timed paths in both cases. Three
have no data path because their D inputs connect directly to TIEHI cells: the
FMA mask, conversion `is_boxed`, and one fixed integer-format bit. Their absence
is unchanged when gates are opened. Audit scripts, coverage and path reports are
in `copilot/tmp/ppa/fpu-clock-audit/`.

The combined experiment does not isolate the numerical contribution of each
optimization. The substantial structural change is moving the FPU output register
boundary inside the arithmetic, distributing work over the already existing
execute/writeback cycles. Clock gating alone does not explain that partition.
Retiming runs at `current_design Core` scope, with automatic ungrouping and no
per-module retiming commands. Only the SRAM instances are explicitly protected
with `dont_touch`. The earlier mapped design nevertheless retained `r_held` at
the end of the long FPU path; whole-Core retiming did not automatically obtain
the internal partition used by this experiment.

## RTL changes and temporal contract

The memory and FPU changes are experimental, opt-in implementation substitutions.
They must not silently alter the microarchitecture used for performance evaluation.
The user-requested one-line ICache is the explicit exception: it changes cache
capacity and therefore fetch misses/stalls, so these runs do not represent the
original ICache's performance.

| Change | Why RTL is needed | Behavior that must be preserved |
| --- | --- | --- |
| SRAM scratchpad | Link actual memory timing/area instead of synthesizing its storage as flip-flops | Synchronous read latency, byte masks, bulk-write priority, AMO replay and completion |
| SRAM event payloads | An SRAM has synchronous reads, unlike the existing queue head mux | Ready/valid and visible-head timing, independent handler capacities, concurrent ingress, simultaneous enqueue/dequeue, full-state bubble |
| Access clock gating | The available memory power model charges clocks even on idle cycles | All required read/write edges and accepted-transfer timing; idle unconsumed data may hold instead of toggle |
| Internal FPU register | Split arithmetic across the existing execute/writeback boundaries | Same result at the consuming edge, same instruction latency and acceptance throughput; no additional pipeline cycle |
| Simulator queue probes | Generated hierarchy differs between queue implementations | Observation only; no simulation scheduling or protocol change |

No queue depth, priority, quota, margin, instruction latency, or uncore pipeline
boundary is intentionally changed. Default flags retain the prior FF memories
and FPU wrapper. Moving the FPU register changes intra-cycle signal arrival,
which is the purpose of the timing experiment; it must preserve values at every
architecturally relevant sampling edge. Standalone comparisons alone are not
sufficient proof of whole-Core temporal equivalence. Combined integration must
also pass and its cycle counts must be compared with the recorded baseline.

## Scratchpad implementation details

`--spm-sram` substitutes 64 `srambank_64x4x16_6t122` 256×16 1RW banks.
Each bank stores one byte lane; its upper eight bits are unused. This retains
all 64 independent write enables, the 256-row organization, and synchronous
read latency without read-modify-write overhead. The available macro lacks
byte masks, so useful capacity is 16 KiB and physical capacity is 32 KiB.
Its total macro area is 15,945.154 µm², versus the retimed FF array's
70,808.425 µm². Macro output timing participates in STA; this is not subtraction
of an FF memory after synthesis.

Models are pinned to [asap7_sram_0p0 revision 9f5af09](https://github.com/The-OpenROAD-Project/asap7_sram_0p0/tree/9f5af0939e8dd3cc1a9693a50b23441691dd7d25).
Three compatibility corrections define a missing output-voltage group, fix an
output capacitance limit from 368.64 to 0.36864 pF, and make the clock-to-data
arc non-unate so LC preserves both rising and falling output tables. Timing
and energy table values are unchanged. Raw files, SHA256 provenance, corrected
files and compiled DBs are archived under `copilot/tmp/ppa/sram-models/`.

## Event FIFO implementation

`--event-sram` substitutes one 16×128 2RW SRAM per handler, retaining sixteen
independent queues and all scheduler, margin, quota and ingress arbitration
logic. Port A reads the next head on dequeue; port B writes on enqueue. A
registered collision bypass handles insertion into an empty queue and
simultaneous replacement of its only entry. The default Queue's non-flow,
non-pipe behavior, including the full-state enqueue bubble, is preserved.
Three different handlers can still receive local/unicast/broadcast messages
concurrently. One common 2RW bank would not preserve that concurrency.

No characterized dual-port view was available. The experiment uses pinned
[ORFS FakeRAM2.0 source](https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts/tree/aa9330a3b652d1889d43c63659dfa04219d352d1/tools/FakeRAM2.0)
with 218.3 ps assumed clock-to-output and 50 ps setup/hold. Its Liberty area is
434.361 µm² per bank; snapped LEF area is 444.752 µm². The legacy geometry
formula is not calibrated to a true dual-port bitcell; a 21 µm height allowance
was needed for its pin-placement scheme. The generated behavioral model was
not used because its latency and second-port clock handling differ from this
interface. An explicit one-edge behavioral model is used for simulation.

The measured standard-cell area reduction between the two SRAM runs is
15,181.395 µm². Thus the sixteen event SRAMs break even at approximately
948.84 µm² each, before physical integration overhead. A real macro below that
area would improve this comparison; the budgetary 434.361 µm² assumption gives
8,231.617 µm² net savings. Moving payloads removes 30,688 sequential cells net,
including the cost of the new bypass state.

The corresponding logic-power reduction is approximately 35.534 mW. Real FIFO
memory internal power plus leakage must fit below that budget to improve total
power at the same activity assumptions. The generated placeholder exceeds it,
so this experiment establishes the logic savings and a macro break-even target,
not a validated FIFO power saving.

The send queue (32×156 payload bits) and broadcast FIFO (8×588 bits) remain
in standard cells, as do the much smaller ingress/control buffers. The
available SRAM has a minimum 256-row shape, which is a poor fit for these
shallow stores. Send-queue replacement additionally needs priority/valid
metadata outside the array and a synchronous head/bypass design. This experiment
prioritizes the 4 KiB of event payload per Core, whose measured FF area dominates
those smaller queues; the full inventory and port requirements are in
[memory-macro-assessment.md](memory-macro-assessment.md).

## Scaling and limits

The actual Liberty DFF area is 0.2916 µm², matching the 1× LEF geometry
1.08×0.27 µm; the 4× LEF counterpart is 4.32×1.08 µm = 4.6656 µm².
These DC areas already use 1× geometry. The active design time and capacitance
units remain ps and fF even when the SRAM ns/pF libraries are linked.
The clock constraint is 1000 ps. No division of these areas by 16, or blanket
division of timing or power by four, is appropriate. See the
[Hammer audit in the baseline report](core-ppa-baseline.md#configuration-and-reproduction)
for the actual plugin code and source-library checks.

DC labels these interface-only library cells as black boxes, so its QoR
"Macro Count: 0" is not a count of instantiated SRAMs. `macros.rpt` verifies
64 scratchpad references, and 80 total references in the FIFO candidate;
macro area is charged. The behavioral macro files are excluded from DC analysis
and linked against their DB views, not re-synthesized into flip-flops.

## Reproduction and validation

All experiments have isolated source archives and SHA256 input manifests,
DC scripts, logs, mapped DDC/Verilog/SDC, timing, area, power and macro reports,
and retiming SVFs under `copilot/tmp/ppa/core-{retimed,spm,spm-event,validated}/`.
The synthesis host and invocation follow `TAPEOUT.md`. No commits or branches
were created. Default RTL parameters retain FF memories; CLI flags select the
experiments explicitly. The final Core elaboration is:

```sh
mill ActiveN.run --core --spm-sram --event-sram \
  --spm-clock-gating --event-clock-gating --fpu-pipeline
```

Run it through the Nix development shell. The corresponding regression System
uses the same flags with `--system --pu=16 --mc=1`. Simulator validation uses
BOLT disabled and two compiler jobs; the final host C++ build uses `-O1` to
reduce build time. Those host flags do not alter the DC constraints or RTL.

DC uses `compile_ultra -retime -gate_clock` with automatic ungrouping, followed
by `compile_ultra -incremental -retime -gate_clock` when setup slack remains
negative. The installed Y-2026.03-SP2 manual states that the historical
`-timing_high_effort_script` flag is ignored, so adding it is not an extra
optimization. Retiming SVFs are preserved, but no formal equivalence run is
claimed. The archived script's attempt to select an unsupported gating style
was rejected with PWR-191/PWR-132; DC retained its default scan-enabled precontrol
style with minimum bank width three. Gating still ran, as verified by the 279
inserted gates. The preparation helper now omits that ineffective style request.

Earlier `core-optimized` and `core-final` snapshots used an ambiguous packed-array
pipeline default: DC inserted stages, but Verilator did not insert the FP32 stage.
A post-edge-only unit comparison missed this; Core integration caught it. Those
RTL snapshots are superseded. Nested defaults fixed simulation but DC rejected
the syntax; explicit packed replication is accepted by both tools. The corrected
`core-validated` run reproduces the previous mapped PPA numbers with RTL that now
passes the strengthened unit and integration checks.

Completed targeted checks:

- Scratchpad unit: 20,898 cycles, all 256 rows and 64 byte lanes, 12,371 masked
  writes, randomized reads and full-line writes.
- FIFO: 200,000 cycles against the original Queue and a software scoreboard;
  103,117 simultaneous transfers, 4,230 empty enqueues and 1,237 full-state
  simultaneous attempts, plus reset, wraparound and combinational ready changes.
- Internal FPU pipeline: 500,000 cycles, 400,095 valid operations compared
  against the original wrapper, including rounding, exceptional operands and
  back-to-back operation-group changes and result stability before the next edge;
  no added latency or input backpressure.
- Gated scratchpad: 20,513 cycles checking partial reads/writes, idle data hold,
  every row, and high-phase enable/address changes.
- Gated FIFO: the same 200,000-cycle comparison passed with read/write clocks
  enabled only for head refresh or enqueue.
- LSU collision: a bulk return preempted an AMO write to the same word; the
  AMO replay read the newly copied data, completed once, and preserved the other
  fifteen words in the line (44 directed cycles).

The 64-PU simulator build exceeded this container's 8 GB RAM. Integration
regressions use 16 PUs and one MC, preserving a complete cluster and the system
interfaces. Scratchpad-only integration passed all nine tests: `spm_macro_test` (342
cycles), `memcopy_test` (466), `fpu_verify` (274), `fpu_extended` (578),
`barrier_test` (1,426), `local_send_test` (168), `pingpong_test` (56,619),
`random_noc_test` (265,455), and the generated 1,024-neuron `spm_init` image
(339,691). The first four use the single-Core simulator; the remaining tests
use the 16-PU hard system. The final combined candidate matches all nine exactly:

| Test | Baseline cycles | Combined candidate cycles |
| --- | ---: | ---: |
| Scratchpad masks/AMOs | 342 | 342 |
| Memory copy | 466 | 466 |
| FPU verify | 274 | 274 |
| FPU extended | 578 | 578 |
| Barrier | 1,426 | 1,426 |
| Local send | 168 | 168 |
| Ping-pong | 56,619 | 56,619 |
| Random NoC | 265,455 | 265,455 |
| Scratchpad initialization | 339,691 | 339,691 |

The baseline in this table is the SRAM-SPM-only implementation with original
FF queues and original FPU wrapper, at the same one-line ICache configuration.
SPM substitution itself is covered by the directed/randomized memory and LSU
checks above. Matching endpoint cycles alone does not prove every internal
signal identical; the FIFO and FPU units additionally compare behavior each cycle.
Results, timers, and peripheral outputs also match, recorded in
`copilot/tmp/ppa/corrected-integration/cycle-comparison.json`.
The separate soft-backend profiler regression passed and observed all 256
enqueues and 256 dequeues for handler 1 across the 16 PUs. It validates the
changed probe bindings; it is not a soft/hard cycle-alignment claim.

## Provisional 512-PU area scaling

Replicating the measured Core gives 16.467 mm² of cell-plus-macro area before
uncore and physical overhead. The requested shared-ICache proxy is 32 clusters
× one scratchpad: 0.510 mm² of additional macro area using this padded SRAM
organization. This is a capacity/area proxy, not an implemented shared ICache;
its power must use its own access activity. The one-line cache stubs remain in
the Core subtotal. The standalone Core's one-MC configuration also needs to be
re-elaborated for the final 16-MC system before treating replication as an exact
system estimate. No complete-chip power or routed area is claimed.
