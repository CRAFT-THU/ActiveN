## 2026-10-25: 30 ps synthesis reserve — two Core reruns complete, Fudian running

All three approved Core reruns use **1.5 GHz, ASAP7 SLVT TT 0.7 V / 25 C**,
with the exact October 23 RTL snapshots (base `da8c51d` plus their recorded
uncommitted changes). DC now uses **30 ps critical range** and **80 ps setup
uncertainty**; hold uncertainty stays 50 ps. Before exporting to PnR, setup
uncertainty returns to **50 ps**, with no recompilation. Thus the extra 30 ps
is a synthesis reserve target; all final comparisons use the original timing
constraints. No RTL, pipeline latency, timing exception or SRAM model changed.

As of approximately 02:52 +08 on October 25:

| Core backend | State | Nominal synthesis setup WNS (ps) | Final routed setup WNS / failures | Non-SRAM hold WNS / failures | Cell + macro area (um2) | Vectorless power (mW) |
|---|---|---:|---:|---:|---:|---:|
| HardFloat | Complete | +30.0007 | -5.642474 / 19 | -39.709747 / 6 | 33448.74 | 15.15296 |
| Simplified HardFloat | Complete | +17.4467 | -1.801312 / 19 | -0.067800 / 3 | 33102.64 | 15.33645 |
| Fudian | Synthesis active | pending | pending | pending | pending | pending |

Full HardFloat improved routed setup from -13.382 ps / 89 failures to
-5.642 ps / 19. Simplified HardFloat setup changed from -1.089 ps / 7 to
-1.801 ps / 19, while non-SRAM hold improved from -49.736 to -0.068 ps.
Neither completed run is timing/physical clean (5829 / 5750 route DRCs).
Simplified HardFloat did not achieve the full synthesis reserve: its tightened
DC report has about -12.55 ps WNS, despite passing nominal synthesis setup.
Existing predictive RC/PG and SRAM power/leakage limitations still apply.

The interrupted session launched HardFloat PID/SID 2881782 and simplified
HardFloat 2882077 at 00:48; both completed by 01:58. Fudian had only uploaded.
After resumption its archive hash was verified and supervisor **2928828** was
launched at **02:48:35**, PPID 1, in
`/home/meow/copilot/tmp/ActiveN-fpu-margin-fudian-20261025/Core`.
It passed HDL/link, verified 80 SRAM macros and logged the intended 80/50/30 ps
optimization settings; ICC2 starts automatically after synthesis. The other
backend directories use the same prefix with `hardfloat` / `hardfloat-normal`.

Original Fudian's -17.249 ps path is instruction bit 12 -> decode/event-scheduling
control -> next-PC selection -> retimed ICache PC/tag comparison. It suffers
**71.793 ps adverse clock skew**, with 555.763 ps clock-to-D delay. Frontend RTL
is byte-identical across variants, but whole-Core retiming and physical/clock
optimization produce different register boundaries, loads and clock arrivals.
Frontend/control plus clock balance is the next timing focus; no frontend RTL
change is included in these reruns.

Scripts, provenance, detailed path analysis and launch record:
[`fpu-margin-20261025/README.md`](copilot/ppa/fpu-margin-20261025/README.md).

### Ordered-issue DRAMIf completed independently

The October 23 ordered-issue/gated-response DRAMIf run completed October 24
06:43 +08. Final routed: setup **-354.007843 ps / 9301 failing checks**, hold
**-237.260223 ps / 2301**, area **129744.43 um2**, vectorless power
**85.30067 mW**, **18407 route DRCs**. Its worst setup path still ends at
`mem_req_bits_wdata[274]`. It improves on the ungated response-stage predecessor
(-479.969 ps, 135.2067 mW, 855325 DRCs), but remains far from timing closure.
It has no SRAM macros. This is a separate experiment, not one of the three
Core margin reruns. Reports are saved under
`copilot/tmp/ppa/fpu-margin-20261025/status-20261025/DRAMIf/`.

## 2026-10-23: ordered DRAM issue and three alternative FPU runs launched

Source base **da8c51d41a3b30134d3357aea6faf128819139c1**, with uncommitted
experiment changes; no new commit was created. All runs use **1.5 GHz
(666.666667 ps), ASAP7 SLVT TT, 0.7 V / 25 C**, 50 ps clock uncertainty,
100 ps min/max external I/O delays, whole-design retiming, automatic clock
gating, synthesis hold repair, high-effort physical optimization and three
post-route optimization passes. No new FPU false/multicycle paths were added.

Detached DC -> ICC2 pipelines on `meow@radl-spr-01.bj.intel.com`:

| Experiment | Launch time (+08:00) | Supervisor PID/SID | Server directory under `/home/meow/copilot/tmp/` |
|---|---|---:|---|
| DRAMIf: ordered issue + gated response payloads | October 23 14:24:49 | 2354489 | `ActiveN-ordered-gated-20261023/DRAMIf` |
| Core: standard HardFloat | October 23 14:41:59 | 2359908 | `ActiveN-fpu-hardfloat-20261023/Core` |
| Core: simplified-recoding HardFloat | October 23 14:42:02 | 2360225 | `ActiveN-fpu-hardfloat-normal-20261023/Core` |
| Core: Fudian | October 23 14:42:05 | 2360537 | `ActiveN-fpu-fudian-20261023/Core` |

All supervisors have parent PID 1 and separate sessions; the pipelines survive
SSH disconnect and automatically start ICC2 after successful DC completion.
At the post-launch check, all three Core jobs passed HDL compilation/linking
and reported `INPUT_MACRO_COUNT 80`, with no substantive DC errors; DRAMIf
was mapping. Initial Core startup spent several minutes traversing the
configured license servers, then proceeded without environment changes.
These are running experiments, **not new timing/area/power results**.

DRAMIf selectively ports the allocation-order issue-index queue from mnist
`0a5d9b1df54c31c70d1d5dad49b4d0aec9f6f871`. Packet format and idle definition
remain unchanged. Its immutable RTL patch SHA256 is
`441a21da5d3ec951a064e81d150f5e07bfa407d662407d2405b846d628ee49a7`.
The previous two response stages remain at the same interfaces and latency;
payload registers now enable only on incoming valid, while valid bits advance
unconditionally. This permits automatic clock gating; it is not supply power
gating. DRAMIf MSHR data remains flop-based. The common MemIf issue queue also
applies to PeripheralIf, without importing mnist's packet/idle protocol changes.

All Core experiments keep two SMT threads, one ICache line, 16 KiB scratchpad,
existing event queues, **80 SRAM macros**, SRAM clock gating, two response
stages with gated payloads and the existing **two-cycle FPU issue-to-writeback
budget**. IEEE FP32 remains in the shared register file. Each FPU has one
internal register boundary and can accept an operation every cycle.
Standard HardFloat and Fudian support the existing instruction/rounding set;
simplified HardFloat preserves normal numbers, zeros, infinities and NaNs but
excludes subnormal inputs/results from correctness scope. Its normal recoding
still adjusts the exponent by 129; it is not pure wiring.

Pinned libraries: HardFloat `c1105e6ac6a0dd90fc80893efc4830ab609005d3`, Fudian
`e1bd4695ca7beb36a5ce7357e9527ad9e95b9ec1`; upstream sources are unmodified.
Immutable source-inventory SHA256 values (include new untracked source):

| Core backend | Source snapshot SHA256 |
|---|---|
| HardFloat | `434ada93c09e57914a8819f4b0c24f0d5ee91a98a75fdd65f6cea38f140553ee` |
| Simplified HardFloat | `0f204a8e2d12ddc4812e29375939f9fd836352fcfb6b04f229cf0eb5a866c447` |
| Fudian | `6fe1a4189f0968a28d74e90a7fdef70cfbfcf2ae608983e9d7507c6add47fb4e` |

Focused Verilator checks against Berkeley SoftFloat's RISC-V specialization
passed **282,764 operations each** for HardFloat and Fudian, and **259,229
in-scope comparisons** for simplified HardFloat, including all rounding modes,
conversions, special values, back-to-back operations, internal register latency
and 16,633 idle bubbles. Core and 64-PU/2-MC RTL were regenerated for every
backend; standard generated/core and generated/system retain default CVFPU.
Full-system regression, simulator alignment and post-retiming equivalence are
not claimed; the first two remain user-deferred.

Reproduction scripts and details:
[`ordered-gated-20261023/README.md`](copilot/ppa/ordered-gated-20261023/README.md),
[`fpu-experiments-20261023/README.md`](copilot/ppa/fpu-experiments-20261023/README.md).
Existing predictive RC/PG and SRAM power-model limitations remain, including
missing scratchpad leakage. Vectorless power is not workload-measured power.

### Completed predecessor: ungated two-response-stage experiment

DRAMIf's previous October 21 run finished routing on October 23 at 00:35;
this supersedes the October 22 "still routing" status below.

| Block / final routed | Setup WNS (ps) / failing checks | Non-SRAM hold WNS (ps) / failing checks | Cell + macro area (um2) | Vectorless power (mW) |
|---|---:|---:|---:|---:|
| Core | -31.699419 / 216 | -11.194229 / 11 | 33534.671 | 16.82355 |
| DRAMIf | -479.969025 / 13306 | -250.515 / 1918 | 135121.81 | 135.2067 |

Neither block met timing. DRAMIf's setup count is path-group checks across
12,793 unique failing endpoints. It has 855,325 signal-route DRCs (743,403
shorts), so these are diagnostic estimates from an unclosed physical design.
Its worst path runs from retimed issue state through request selection to
`mem_req_bits_wdata[213]`; other major failures involve NoC allocation/clock
gating and internal MSHR state. Clock-related power grew from 7.789 to
40.531 mW, contributing to the previous power increase. The new ordered issue
queue and response payload enables target these problems; improvement is not
yet measured. Detailed predecessor analysis is saved in
`copilot/tmp/ppa/response-stages-20261021/status-20261023/DRAMIf/analysis/`.

## 2026-10-22: two-stage experiment — Core complete, DRAMIf still routing

At approximately 10:10–10:23 +08:00, Core's detached synthesis/PnR pipeline
has completed successfully (October 21, 13:27:48), but **timing is not met**.
DRAMIf synthesis completed; ICC2 is still active in `route_auto`, before the
three post-route optimization passes. Both use 1.5 GHz, SLVT TT, 0.7 V / 25 C.

| Block / completed stage | Setup WNS (ps) / failing checks | Non-SRAM hold WNS (ps) / failing checks | Cell + macro area (um2) | Vectorless power (mW) |
|---|---:|---:|---:|---:|
| Core / final routed | -31.699419 / 216 | -11.194229 / 11 | 33534.671 | 16.82355 |
| DRAMIf / synthesis only | +0.00146484 / 0 | -32.3152 / 8310 | 104176.430 | 49.276 |

Core's I/O setup groups now pass (input-to-register +0.19 ps,
register-to-output +0.59 ps); remaining setup failures are internal. The worst
path is FMA sum register to `exec/rs2val_reg[24]`. The worst logic hold path
is flop-based ICache data storage to its read-data register, not an SRAM-model
exception. Raw hold count is 16, including five excluded SRAM checks.
Setup failures decreased from 652 to 216 versus the October 17 baseline;
worst setup slack did not improve. Core has 5715 remaining signal-route DRCs.
DRAMIf final routed PPA is pending; its large intermediate routing DRC counts
must not be interpreted as final timing or physical results.

Core area/power include the existing SRAM views; missing scratchpad leakage
and predictive physical-model limitations remain. Local assertion validation
is deferred by explicit user instruction; simulator alignment is out of scope.
Detailed status and source provenance are in
[`response-stages-20261021/README.md`](copilot/ppa/response-stages-20261021/README.md).

## 2026-10-21: two response stages — new Core/DRAMIf runs launched

New experiment: `--insert-stages=2` / `responsePipelineStages=2` adds two
unconditional register stages at Core unicast ingress, DRAMIf unicast egress,
MemIf ring ingress and external memory-response ingress. Ring input ready is
constant true; assertions check actual internal acceptance, forwarding-queue
occupancy and ring egress. PeripheralIf uses the delayed external response for
ROM arbitration. Idle detection is unchanged. Software-NoC alignment is excluded
by user instruction, so this configuration must use hard RTL simulation.

Source base: `da8c51d`, plus the uncommitted RTL patch SHA256
`b0bd7ccc51c44d7b2c83a2062f7a30cfce1c29cd5f02db13caf16c7c405c20a2`.
The additional MemIf redesign mentioned in discussion is not yet implemented
and is not included. Both Core and 64-PU/2-MC RTL were regenerated with existing
SRAM, SRAM clock gating and FPU pipeline options enabled.

Detached DC → ICC2 pipelines launched at 12:17:35 +08:00 on
`meow@radl-spr-01.bj.intel.com`, under
`/home/meow/copilot/tmp/ActiveN-response-stages-20261021`:
Core supervisor PID/SID **2141846**, DRAMIf **2141880**; both parent PID 1.
Same 1.5 GHz, SLVT TT 0.7 V / 25 C, I/O delays, uncertainty, high effort,
whole-design retiming, hold repair and three post-route optimization passes as
the preceding experiment. Existing SRAM/RC/PG limitations still apply.
These are running experiments, not new PPA results.

Scripts, configuration, provenance and validation status:
[`copilot/ppa/response-stages-20261021/README.md`](copilot/ppa/response-stages-20261021/README.md).

## High-effort synthesis and PnR results — verified 2026-10-21

All three detached pipelines **completed on October 17**, including synthesis,
placement, CTS, detailed route and three post-route optimization passes. Exit
codes, completion markers and original reports were checked against the server.
**None fully meets timing at 1.5 GHz.** Router_32 meets hold and is close on
setup; Core and DRAMIf still violate both. This is execution completion, not
physical signoff.

Configuration: **ASAP7 SLVT TT, 0.7 V / 25 C, 666.666667 ps**, 100 ps input/output
delays (min and max), 50 ps uncertainty, 2 fF output load. All three ICC2 PVT
reports explicitly say there are no mismatches. Source snapshot:
`f7dc714f52ac65613b5ff4ceda4b14d3e23c5700`; existing 1-line ICache, two SMT threads,
16 KiB scratchpad, SRAM/event gating and FPU pipeline. No RTL changes.

### Final routed results

Hold columns exclude SRAM data paths, as requested. Area is the sum of cell and
macro footprints. Power includes the implemented clock network and available
macro power models; it is vectorless at the requested 1.5 GHz, not measured
workload power or a power ceiling.

| Block | Timing met? | Setup slack (ps) | Setup failures | Non-SRAM hold slack (ps) | Non-SRAM hold failures | Cell + macro area (um2) | Vectorless power (mW) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Core | No | -30.527651 | 652 | -0.253648 | 6 | 33,341.617 | 14.466190 |
| Router_32 | No | -0.777364 | 2 | +0.180990 | 0 | 3,287.032 | 2.191790 |
| DRAMIf | No | -591.066345 | 21,667 | -248.612640 | 1,528 | 132,607.622 | 66.298810 |

The failure counts are the tool's QoR counts summed across path groups, not a
count of every possible combinational path or necessarily distinct endpoints
across all groups. Core's six non-SRAM hold violations were also enumerated in
the complete filtered timing report. Its raw hold result is **-1.133740 ps / 8
failures**; the other two endpoints are event SRAM banks 8 and 7.

| Block | Dynamic (mW) | Modeled leakage (mW) | Floorplan core region (um2) | Setup TNS (ps) | Non-SRAM hold TNS (ps) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Core | 11.546190 | 2.920004 | 65,101.523 | -7,882.32 | -0.831395 |
| Router_32 | 1.927056 | 0.264735 | 6,395.838 | -0.83 | 0.000000 |
| DRAMIf | 53.086270 | 13.212540 | 210,507.527 | -1,374,402.97 | -97,036.360000 |

Core area includes 80 SRAM macros: 23,061.187 um2 macro footprint and
10,280.431 um2 standard cells, excluding the router. Its memory power group is
4.351594 mW (including the event model's placeholder leakage); **scratchpad
leakage remains missing**, with no manual allowance added. Event macro LEF area
is larger than its Liberty area, explaining 166.254 um2 of the synthesis-to-PnR
macro-area difference. Router and DRAMIf contain no SRAM macros; their existing
storage is mapped to standard cells.

ICC2 power uses pW: divide by 1e9 for mW. Synthesis total power is already
reported in mW. No additional ASAP7 geometry multiplier is applied. Activity
uses 0.5 input static probability and 0.1 input toggle rate relative to the
clock, then vectorless propagation; DC and ICC2 use different propagation
engines/efforts, so their power difference is not a pure interconnect-power delta.

### Completed synthesis, before physical implementation

| Block | Setup slack (ps) | Setup failures | Non-SRAM hold slack (ps) | Non-SRAM hold failures | Cell + macro area (um2) | Vectorless power (mW) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Core | -0.095459 | 1 | -25.684600 | 1,909 | 32,395.785 | 13.705 |
| Router_32 | +0.113464 | 0 | -17.439700 | 148 | 3,204.188 | 1.562 |
| DRAMIf | +0.000854 | 0 | -22.579900 | 552 | 105,303.552 | 29.452 |

Router and DRAMIf met setup after synthesis, but neither met hold. Core missed
setup by 0.095459 ps. Core's raw synthesis hold count was 2,663 at -47.077 ps;
the 1,909 non-SRAM count was verified by reopening the saved DDC and by matching
against the original complete constraint report (754 macro endpoints excluded).
The audit reproduces the original raw count and raw/filtered worst slacks.

### Timing failures and progress through physical optimization

- **Router:** only two setup failures remain: REG104_S2 -> REG9_S1 at
  -0.777364 ps, and the same launch -> `ejects_1_bits_data_0[1]` at -0.054955 ps.
  Hold is met, with +0.180990 ps worst slack. This is a near miss, not a pass.
- **Core:** 309 input-to-register and 343 clock-group setup failures. Worst
  setup is REG7526_S9 -> REG338_S7 at -30.527651 ps. Worst non-SRAM hold is
  REG7840_S7 -> REG540_S11 at -0.253648 ps. Output setup paths pass.
- **DRAMIf:** 15,686 input-to-register, 2,242 register-to-output, 1,070
  input-to-output and 2,669 clock-group setup failures. Worst setup ends at
  `unicast_1_bits_data[457]` from REG166129_S7. Worst internal clock-group setup
  is -262.74 ps, so this is not only an external-output budget issue. Hold has
  1 input-to-register and 1,527 clock-group failures, worst -248.612640 ps.

| Block | Route-opt pass 1 setup WNS / failures | Pass 2 | Pass 3 |
| --- | --- | --- | --- |
| Core | -41.19 ps / 1,042 | -34.03 ps / 752 | -30.53 ps / 652 |
| Router_32 | -3.41 ps / 31 | -0.99 ps / 6 | -0.78 ps / 2 |
| DRAMIf | -602.11 ps / 32,480 | -591.35 ps / 24,382 | -591.07 ps / 21,667 |

### Physical validity

The extracted results use the documented predictive RC approximation and Core
SRAM physical proxy. **None is physically closed.** Zero open signal nets does
not remove the remaining DRC and power-grid connectivity problems.

| Block | Signal-route DRC count | Open signal nets | PG DRC count |
| --- | ---: | ---: | ---: |
| Core | 8,122 | 0 | 9,492 |
| Router_32 | 0 | 0 | 833 |
| DRAMIf | 9,197 | 0 | 62,204 |

DRAMIf's signal-route total includes 9,192 hard and 5 soft violations. Core still
has 64 floating scratchpad macros on each supply and 55 floating VSS standard
cells, plus floating supply wires/vias. Router and DRAMIf PG connectivity reports
have zero floating objects, but their M1 PG spacing errors remain. Antenna rules
are unavailable. These results cannot be presented as tapeout-ready PPA or a
validated maximum clock frequency.

### Completion times and archived evidence

| Block | Completed (CST, October 17) | Full pipeline elapsed |
| --- | --- | ---: |
| Core | 04:32:20 | 1.765 h |
| Router_32 | 03:14:23 | 0.465 h |
| DRAMIf | 19:45:41 | 16.987 h |

Remote root: `/home/meow/copilot/tmp/ActiveN-high-effort-20261017`.
Original reports and status files copied to
`copilot/tmp/ppa/progress-20261021/{Core,Router_32,DRAMIf}/`.
Machine-readable summary: `copilot/tmp/ppa/progress-20261021/summary.json`.
Scripts: `copilot/ppa/high-effort-20261017/`; reporting audit and parser:
`audit-20261021/`. The reported PnR values are final pass 3, not cherry-picked
intermediate checkpoints. No new optimization run was started for this check.

---

## High-effort standalone synthesis + PnR launched — 2026-10-17

Three independent detached pipelines started at **02:46:28 +08:00**, using
**ASAP7 SLVT TT / 0.7 V / 25 C / 1.5 GHz (666.666667 ps)**. Source snapshot:
`f7dc714f52ac65613b5ff4ceda4b14d3e23c5700`, with the existing 1-line ICache,
two SMT threads, 16 KiB SRAM scratchpad, event SRAM, SRAM clock gating and FPU
pipeline. No RTL changes. External delays remain 100 ps, uncertainty 50 ps,
output load 2 fF. This is not a 0.77 V / FF substitution.

| Pipeline | Detached supervisor | DC PID at launch | Initial verified state |
| --- | ---: | ---: | --- |
| Core | 2019942 | 2019946 | Synthesis elaboration/mapping active |
| Router_32 | 2019973 | 2019980 | Synthesis elaboration/mapping active |
| DRAMIf | 2020007 | 2020014 | Synthesis elaboration/mapping active |

Each pipeline automatically continues into ICC2 placement, CTS, detailed routing
and **three post-route optimization passes**. Physical timing, signal DRC, PG
DRC/connectivity and vectorless power are reported separately; tool completion
is not a claim of timing/physical closure. There are no new PPA results yet.

DC uses adaptive retiming during Ultra compile, whole-design register
optimization, two incremental passes and high-effort hold mapping. Correction
to the earlier effort discussion: this DC release documents `compile_ultra` as
already high effort and `-timing_high_effort_script` as **ignored**. ICC2's active
high timing/placement/congestion/routing/CCD settings were validated in a tool
preflight, and automatic hold-effort reduction is disabled.

Core macro NDMs now build using an explicitly predictive functional-port SRAM
abstract: unmatched `sdel` metal is retained as routing obstruction, VSS is
classified GROUND, and the existing timing model is unchanged. No electrical
sdel value is guessed. This is a PPA proxy, not tapeout-ready SRAM collateral.
Core has 80 macros; Router and DRAMIf still use standard-cell storage. Scratchpad
leakage remains absent from the model. Prior predictive RC limitations remain.
Core physical preflight completed import/placement/PG generation, but its PG
still has 4,940 M4/M5 spacing errors and disconnected macro supply shapes;
this is not physically closed. Those issues are recorded independently of the
queued signal-timing optimization and must not be concealed by a completion flag.

Remote run root: `/home/meow/copilot/tmp/ActiveN-high-effort-20261017`.
Scripts, input provenance, effort settings and caveats:
[high-effort run record](copilot/ppa/high-effort-20261017/README.md).

### Previous routed Router: failing endpoints

The October 15 Router_32 result has **30 setup violations**: 22 mesh-output
ports, 5 local-output ports, 2 internal registers and 1 ICG-enable endpoint.
**21 share `core_clock_r_REG49_S2`**, in input queue 3's retimed control domain.
Despite their names, these `core_clock_r_*` cells are Router registers.

| Failing path / endpoint group | Count | Worst slack (ps) |
| --- | ---: | ---: |
| Mesh output ports (egress_0..3) | 22 | -5.605 |
| Local Core ejection (ejects_0) | 4 | -1.66 |
| Memory-interface ejection (ejects_1) | 1 | -0.42 |
| Internal register setup | 2 | -2.384 |
| inputQueues_1 priority ICG enable setup | 1 | -0.14 |
| Internal register self-feedback hold | 1 | -0.0912 |

Worst setup: `core_clock_r_REG49_S2/QN` through 27 combinational stages to
`egress_2_bits_data_0[26]`, arrival 626.41 ps versus required 620.81 ps. The
standalone output constraint contributes 100 ps; no neighboring block is part
of this path. Router_32 maps to PU33's router in the saved System; egress_2 is
South to router_34, ejects_0 goes to PU33, and ejects_1 to memif_2.nocIn_0.
The worst internal path from the same launch register to REG13_S1 also suffers
about 30.94 ps unfavorable clock skew. The hold failure is REG237_S1's feedback
through a hold buffer and A2O1A1I gate. Higher-effort clock/data optimization,
buffering and sizing address these without changing the RTL protocol.

The old layout additionally has **819 M1 PG spacing violations**, despite zero
signal-route DRCs/opens; timing effort alone does not establish physical closure.
[Complete endpoint list and path analysis](copilot/ppa/high-effort-20261017/router_previous_endpoints.md).

## Router 0.77 V audit and standalone status — 2026-10-15 17:57 +08:00

Tried `set_voltage 0.77 -min 0.77` on the completed Router layout while retaining
TT / 25 °C, with a 0.70 V control in the same ICC2 session. The detached audit
completed at 17:56:56, without saving changes to the baseline layout.
**This does not produce a valid TT / 0.77 V result:** `report_pvt` reports all
21,844 cells at effective **0.70 V**, despite requested 0.77 V. All five TT
libraries have a single 0.7 V / 25 °C characterization. The shipped 0.77 V
Liberty is **FF / 0 °C**, so substituting it also changes process and temperature.

The unsupported voltage setting superficially improves setup from −5.604982 to
−2.591372 ps, but the worst path's data-cell increments remain identical; its
launch-clock latency changes by 3.013611 ps. This is not evidence of a physically
faster TT / 0.77 V implementation. The mismatched power result is likewise not
accepted. A voltage-only TT / 25 °C comparison needs a corresponding characterized
library or validated voltage-scaling model. No full reroute was launched with
these mismatched conditions. Script: `router_voltage_probe.tcl` in the saved PnR
script directory; reports: `copilot/tmp/ppa/pnr-synopsys-20261015/voltage-probe/`.

Rechecked original server completion markers, summaries, area and power reports:

| Standalone synthesis | Status | Area (µm²) | Setup (ps) | Non-SRAM hold (ps) | Vectorless power (mW) |
| --- | --- | ---: | ---: | ---: | ---: |
| Core, repaired ~1.5 GHz | Complete; setup/non-SRAM hold met | 33,358.269 | +0.01843 | +1.00401 | 16.193 |
| Core, newer 1.6 GHz trial | Complete; timing violated | 33,107.202 | −38.0876 | −29.3715 | 29.465 |
| DRAMIf, 1.5 GHz | Complete; setup met, hold violated | 105,903.344 | +0.003784 | −22.4056 | 29.733 |

These are **pre-layout**, SLVT TT / 0.7 V / 25 °C. Core includes 64 scratchpad
and 16 event SRAM models, excludes router, and retains the documented missing
scratchpad leakage; no manual leakage allowance is added above. Raw Core SRAM
hold violations remain excluded only from the non-SRAM hold column. DRAMIf has
no SRAM macros and 551 hold violations. Neither Core nor DRAMIf has a PnR result.

## Router post-route result — checked 2026-10-15 17:45 +08:00

The detached ICC2 Router_32 run **finished at 13:38:18**, in **11 min 26 s**
supervisor wall time. Placement optimization, CTS, detailed routing, route
optimization and extraction completed; exit 0 and completion marker verified.
Configuration: **667 ps (~1.5 GHz), ASAP7 SLVT TT, 0.7 V / 25 °C**; inherited
50 ps setup/hold uncertainty, 100 ps I/O delays and 2 fF output loads.
Uses the documented calibrated predictive RC approximation; no SRAM or RTL changes.

| Metric | Router_32 post-route |
| --- | ---: |
| Standard-cell area | 3,300.416 µm² |
| Core placement region | 6,478.156 µm² |
| Block footprint including margins | 7,192.227 µm² |
| Worst setup slack | −5.605 ps |
| Worst register-to-register setup slack | −2.384 ps |
| Setup violations / TNS | 30 / −48.730 ps |
| Worst hold slack / violations | −0.0912 ps / 1 |
| Vectorless total power | ~4.04 mW |
| Dynamic / leakage | ~3.77 / 0.269 mW |
| Clock-network power, included above | ~1.91 mW |

This is **not yet timing/physical closure**. `check_routes` reports zero signal
routing DRCs and zero open nets, and PG connectivity reports zero floating
wires/vias/cells. However, the separate final PG DRC check reports **819 M1
spacing violations** after optimization/routing; the earlier pre-route PG check
was clean. No antenna rules are present. Worst setup is register-to-egress port
`egress_2_bits_data_0[26]`; the single hold violation is a register self-feedback
path. All 22,762 signal nets have extracted RC networks.

Cell area is approximately 1.8% above the prior 3,242.504 µm² DC result.
Vectorless power is higher than the earlier 1.560 mW synthesis report; the
implemented clock-network category alone accounts for 47.3% of post-route power.
This is not workload-measured power, and DC/ICC2 activity propagation is not
proven identical. Power figures are read in pW and converted to mW, with no
additional geometry scaling.

Local reports: `copilot/tmp/ppa/pnr-synopsys-20261015/completed-router/`.
Remote routed checkpoint: `ActiveN-pnr-synopsys-20261015/router_place.ndm:Router_32/routed`.
DEF, netlist, SDC and SPEF are retained in the remote `router_reports/` directory.
Core PnR has not started; its SRAM physical/timing-view mismatch remains unresolved.

The **64-PU / 2-MC hierarchical synthesis is still running**, about 7 h 41 min
after its resumed integration launch. At 17:45 it was actively characterizing
router_58 / Router_57 and writing contextual checkpoints. All 72 seed blocks
are complete; context refinement and final integration remain pending.

## Synopsys PnR bring-up history — 2026-10-15

The bundled **ICC2 / Library Manager X-2025.06-SP3-T-20260410** now execute on
the server, including confirmed `ICCompilerII-4` license checkout. Five SLVT TT
standard-cell NDMs were built and passed workspace checks. The first physical
trial uses the previously repaired **Router_32**, **667 ps / approximately
1.5 GHz, SLVT TT, 0.7 V / 25 °C**, at 50% floorplan utilization; no RTL changes.
The imported 21,540-cell netlist has **3,242.505 µm²** cell area, agreeing with DC.
This checks native geometric scaling; it is not a new routed PPA result.

Import, linking, floorplanning, placement and legalization **completed** at
13:08:23 +08:00 (supervisor 1997523). ICC2 reports **zero placement legality
violations**. M2 uses six horizontal tracks per 0.270 µm row; ICC2's `-dir Y`
selects the stepping axis, not the wire direction. A corrected continuation
for power routing, placement optimization, CTS and detailed routing started
detached at 13:10:09, but stopped at timing-driven placement because its RC
model returned **zero resistance**. Reading a TLU+ file successfully did not
establish that it was electrically usable.

Per the user's requested scope, this is a **predictive PPA experiment**.
The current fallback calibrates a released YZU ASAP7 research RC table:
front-side M1–M9 resistance follows its published metal resistivity, corrected
for the 4× stack thickness; capacitance is normalized per layer against the
native IEEE DATC table at minimum wire width. This gives approximately
67.90 / 32.12 / 15.625 / 9.063 Ω/µm for M1–3 / M4–5 / M6–7 / M8–9.
Timing-driven initial placement completed using the resistance-bearing model
at 13:25:44. The full continuation restarted detached at **13:26:52** under
supervisor **2000943**, with explicit single-cut PG vias, extended outer straps
and separate virtual-route RC scale controls. At 13:28 it is executing
timing-driven placement optimization. The new PG checks show **zero DRC errors
and zero floating wires, vias or cells** for both VDD and VSS. This is an
explicit RC approximation, not a foundry extraction deck or a BPR implementation.

The initial timing-coverage warnings were audited: 139 constant ICG scan-enable
pins, one reset-fed D pin without an arrival path, and reset without an input
delay, consistent with the inherited reset false path. No register clocks
were missing. Functional timing paths retain the synthesis constraints.

**At the 13:28 check, no CTS/detailed-route/extracted PPA result was available.** Core physical import also
found a scratchpad view mismatch: five LEF `sdel` pins are absent from its
timing/Verilog model, and LEF VSS has the wrong POWER classification. These
were not silently removed or tied to assumed values. Existing synthesis PPA
remains pre-layout. OpenROAD is not used.

Scripts, provenance, limitations and logs:
[Synopsys PnR run record](copilot/ppa/pnr-synopsys-20261015/README.md).
Remote directory: `/home/meow/copilot/tmp/ActiveN-pnr-synopsys-20261015`.

## Synthesis progress — checked 2026-10-15

Standalone DRAMIf completed on October 13 at 19:04:13 +08:00, in **2 h 51 min
40 s**, exit 0 with completion marker. At 666.666667 ps, SLVT TT / 0.7 V /
25 °C: **105,903.344 µm²**, setup **+0.003784 ps**, hold **−22.4056 ps**,
vectorless power **29.733 mW** (6.852 switching, 14.527 internal, approximately
8.354 leakage). Zero setup violations, 551 hold violations; not timing closed.
This controller has no SRAM macros: buffer and queue storage remain standard cells.

All **72 hierarchical seed block jobs completed successfully**, but initial
System integration failed October 13 at 22:40:36 before mapping: the script
treated `read_ddc`'s returned list of design names as a Boolean. Both checkpoint
load sites were corrected to check for the expected loaded design. The pipeline
resumed detached October 15 at **10:04:20 +08:00**, supervisor PID **1990589**,
reusing all completed blocks. Failed integration logs and the original manifest
are retained under `failed-integration-20261013/`. No integrated PPA result yet.

| Hierarchical seed block (before context refinement) | Area (µm²) | Setup (ps) | Non-SRAM hold (ps) | Vectorless total (mW) |
| --- | ---: | ---: | ---: | ---: |
| Core | 32,969.290 | +0.009583 | −31.3916 | 12.010 |
| Router_32 | 3,216.844 | +0.073914 | −18.2335 | 1.560 |
| DRAMIf | 96,224.732 | −16.6750 | −22.3509 | 25.547 |
| DRAMIf_1 | 99,930.224 | +0.001709 | −22.4056 | 27.332 |

These are fresh, native-hold-repair seed results, not replacements for the older
custom-ECO timing-closed Core/Router figures. Core includes 80 SRAMs and retains
raw SRAM hold slack −48.9848 ps; prior SRAM-model limitations apply. The proposed
1 mW scratchpad leakage allowance is not added to these raw tool reports.
Reports and execution records: `copilot/tmp/ppa/progress-20261015/`.

## Hierarchical 64-PU / 2-MC experiment launched — 2026-10-13

The approved hierarchical experiment started detached at **16:52:06 +08:00**
at **1.5 GHz / 666.666667 ps, SLVT TT, 0.7 V / 25 °C**, alongside the standalone
DRAMIf run. Supervisor PID `1980103`; remote directory
`/home/meow/copilot/tmp/ActiveN-system64x2-hier15-20261013`.
Four workers map the 72 distinct block definitions, assemble the System using
64 copies of one mapped Core, extract actual block timing environments, run
one context-refinement pass, and automatically perform the final integration.
Each distinct routing table is retained. Mapped blocks are preserved during
integration; all boundary paths remain timed. No RTL was changed.

Initial block I/O budgets are seeds only; final full-System STA determines
closure. SRAM instance counts and shared-Core references are checked during
integration. Area and vectorless power include the existing SRAM models with
their documented limitations. Results are pending. Scripts, assumptions,
provenance, and monitoring paths: [hierarchical run record](copilot/ppa/hier15-20261013/README.md).

## DRAMIf restarted at 1.5 GHz — 2026-10-13

At user request, the unfinished 1.6 GHz DRAMIf run was terminated and its
process group verified stopped. A fresh detached run started at
**16:12:33 +08:00**, using **666.666667 ps / 1.5 GHz**, ASAP7 SLVT TT,
0.7 V / 25 °C. Only the clock period and retiming delay threshold changed;
RTL, libraries, I/O constraints, uncertainty, gating and native hold repair
are retained. No new Core or Router run was launched. Their earlier repaired
667 ps results remain the available approximately 1.5 GHz reference.

Remote directory: `/home/meow/copilot/tmp/ActiveN-dramif-tt15-20261013`;
runner PID `1978836`, DC PID `1978848`. The runner was verified reparented to
PID 1 after SSH disconnect, in its own session. Results are pending; process
startup is not a synthesis completion or timing-closure claim. DRAMIf storage
remains standard-cell logic, with no SRAM macro instances. The cancelled run's
inputs and logs remain preserved. Scripts and provenance:
[DRAMIf 1.5 GHz restart](copilot/ppa/tt15-dramif-20261013/README.md).

## 1.6 GHz standalone jobs — progress checked 2026-10-13

Core and Router_32 finished with exit status zero and completion markers;
DRAMIf (MC1 of the 64-PU / 2-MC elaboration) remains running detached on
`radl-spr-01.bj.intel.com` at the 16:04 +08:00 progress check.
Target: **625 ps / 1.6 GHz, ASAP7 SLVT TT / 0.7 V / 25 °C**; 50 ps uncertainty,
100 ps I/O delays, 20 ps slew, 2 fF output load. Full-top register optimization,
automatic clock gating, native hold repair, and actual-load reporting enabled.

Core retains **64 SPM + 16 event SRAM macros** with existing .db area/power views.
Router and DRAMIf contain **no SRAM macro instances**; their queues and the
controller response buffer remain standard-cell logic. No RTL or cycle-level
behavior changes were made. SRAM power retains the prior model limitations.

Scripts and run paths: [1.6 GHz run record](copilot/ppa/tt16/README.md).
Source/config hashes: `copilot/tmp/ppa/tt16/input-manifest.json`; source commit
`f7dc714f52ac65613b5ff4ceda4b14d3e23c5700`. Input hashes identify the exact generated snapshot.
| Block | Status / elapsed | Cell + macro area (µm²) | Setup slack (ps) | Non-SRAM hold slack (ps) | Raw hold slack (ps) | Vectorless total power (mW) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Core | Finished, 22 min 17 s | 33,107.202 | −38.0876 | −29.3715 | −48.3880 | 29.465 |
| Router_32 | Finished, 10 min 19 s | 3,226.408 | +0.08853 | −16.7120 | −16.7120 | 1.643 |
| DRAMIf | Cancelled at 16:12; restarted at 1.5 GHz above | — | — | — | — | — |

Core has 1,638 setup-violating endpoints; Router_32 has zero setup violations
but 141 hold violations. Neither completed result is fully timing-closed.
This run includes native hold repair, not the custom ECO used for the older
1.5 GHz table. Core area includes 22,894.933 µm² of SRAM macros. Core power is
1.058 mW switching, 25.527 mW internal, and approximately 2.880 mW leakage;
Router power is 0.189 mW switching, 1.198 mW internal, and approximately
0.255 mW leakage (rounded report components). These are vectorless estimates,
not measured workload power or maximum-power bounds.

Downloaded reports and execution provenance are in
`copilot/tmp/ppa/tt16/results/`. DRAMIf is actively mapping with no DC errors
reported so far. Shell `Bad fd number` warnings occur in all three logs,
including both completed jobs; they are recorded for follow-up and have not
stopped this run.
The later hierarchical reuse experiment follows these standalone measurements.

## Latest nominal-corner results — checked 2026-09-22

The **64-PU / 2-MC System and Core hold-repair runs both finished successfully**
as tool jobs. The repaired standalone Core and Router_32 meet the **667 ps
(1.49925 GHz)** constraint under the hold-reporting policy below; the integrated
System **does not meet setup timing**. All three use **ASAP7 SLVT TT, 0.7 V,
25 °C**, DC Y-2026.03-SP2, and the existing SRAM timing/power models.

| Configuration / final run | Cell + macro area (µm²) | Setup slack (ps) | Hold slack excluding SRAM paths (ps) | Raw hold slack (ps) | Vectorless total power | Workload power at TT |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Core, logic ECO v4 | 33,358.269 | +0.01843 | +1.00401 | −48.9848 | 16.193 mW | Not measured |
| Router_32, logic ECO v3 | 3,242.504 | +0.62433 | +0.93410 | +0.93410 | 1.560 mW | Not measured |
| 64-PU / 2-MC System, detached v3 + actual-load audit | 2,500,760.620 (2.50076 mm²) | −75.5161 | −33.9194 | −48.9848 | **755.542 mW** | Not measured |

**Integration diagnosis, September 22:** the worst path is Core execution-to-Fetch
branch feedback (−75.5161 ps), and an internal FPU path misses by −75.2063 ps.
Other targeted failures include Router11-to-MC1 (−73.2952 ps), MC2-clock-origin
endpoints (−72.7030 ps), internal router arbitration (−65.9290 ps), and
MC1-to-response-distributor (−63.7982 ps). A path connecting Router10 and Router9
clock-origin registers misses by −59.48 ps. The System freshly synthesized RTL;
it did not reuse the repaired standalone Core/Router implementations. Its
standalone 100 ps input/output budgets also do not establish timing closure of
the actual connected interfaces. Recomputing timing with actual pin loads
instead of DC's high-fanout approximation leaves the global setup result and
600,775 violating endpoint count unchanged.
The additional full Core↔Router directional queries remain running detached;
the paths above are confirmed examples, not a claim that every other boundary
passes.

The area excess over **64 × (repaired Core + heaviest Router_32)** is only
**158,311.094 µm² / 6.76%**. A fully retained DRAM controller measures
**98,004.600 µm²**; each controller has a 256 × 512-bit **register-based**
response/write-data buffer, metadata and queues. This is substantial real
area beyond Core + Router. Actual routers have 291 input queues across the
mesh, versus 384 when multiplying the heaviest router by 64. MC2 is mostly
flattened, so its remaining queue hierarchy is not its full area.

The original power anomaly has been explained: **76 nets accounted for 42.7625 W of
switching while each is assigned approximately 1,000,000 fF (1 nF)** by the
high-fanout load model. The same named Core clock nets had loads around
144 fF before retiming. MC1 reports only 28.389 mW in the original report;
it does not explain the tens-of-watts discrepancy. The report-only audit with
`high_fanout_net_threshold = 0` recomputed power using actual pin loads on the
unchanged checkpoint: **755.542 mW total**, comprising **75.926 mW switching,
468.722 mW internal and approximately 210.894 mW leakage**. MC1's corrected
total is **17.217 mW**. For example, `pu_54/exec/net857810` actually loads
1,553.595 fF and contributes 1.1410 mW, versus the artificial 1 nF and
734.4532 mW. Area and setup/hold results are unchanged. See the detailed
[System diagnosis](copilot/ppa/tt15-detached/system-analysis-20260922.md) and
the preserved scripts/reports referenced there.

**Hold reporting:** by user instruction, only hold paths touching either SRAM
model are excluded from the logic-only metric, using timing-report/query
filters. No SRAM false paths were added and **all setup paths remain checked**.
The repaired Core has zero setup and zero logic-only hold violations; its raw
report retains 782 SRAM-related hold violations. Router_32 contains no SRAM.
The System checkpoint audit confirms a logic hold violation of −33.9194 ps:
its worst reported path is directly between two retimed registers inside
`pu_52/exec`, with 31.39 ps arrival versus 65.31 ps required (including the
unchanged 50 ps hold uncertainty). This is not an SRAM-interface path.
Positive slacks this small provide no practical implementation guardband.

**Power scope:** Core/System area and power include the SRAM views, but the SPM
library lacks leakage and the event SRAM uses a budgetary model, including
placeholder leakage. The model is not a calibrated total-silicon estimate or
an upper bound. Vectorless power uses input probability 0.5 and toggle rate
0.1 per clock, with inferred internal activity; no workload trace was used for
these TT results. Core power comprises approximately 13.261 mW dynamic and
2.932 mW leakage; Router power is approximately 1.303 mW dynamic and 0.257 mW
leakage. The Core repair raised total vectorless power from 12.866 to 16.193 mW
and area from 32,983.563 to 33,358.269 µm². It accepted three equivalent cell
sizings and 5,139 non-inverting buffers, with no RTL or pipeline change; this
ECO netlist has not received a new waveform/equivalence replay.

**System caveat:** the original approximately 43.4 W total is invalid as a
physical power estimate because of the artificial high-fanout loads described
above; retain it only as diagnostic evidence. The corrected 755.542 mW is still
vectorless, with unannotated sequential/black-box outputs, no clock-tree or
extracted wire power, and the SRAM limitations above. It is not a measured
workload value or maximum-power bound. Standalone Core/Router and full-System
input activity propagate differently, so their totals are not an equal-workload
comparison. The 600,775 setup violations mean successful completion is not
timing closure. The run took **34 h
18 min 32 s**, from September 16 22:14:18 to September 18 08:32:50 (+08:00),
with reported peak memory about 97,298 MB. The final Core ECO took 11 min 11 s,
finishing September 17 11:06:32 (+08:00). Both exit statuses are zero and both
completion markers are present.

**Configuration and provenance:** two SMT pipes/Core; one 64-byte ICache line;
16 KiB useful SPM in 64 banks/Core; 16 event SRAMs/Core; `spmSram`, `eventSram`,
both SRAM clock-gating options and `fpuPipeline` enabled. System has 5,120 SRAM
instances, 512-bit memory lines and two 4-GiB memory-controller address spaces;
DRAM devices/PHYs are not included. Router_32 has six 8-entry flit queues,
implemented in standard cells. System/Router source manifest records commit
`f7dc714f52ac65613b5ff4ceda4b14d3e23c5700`, exact configuration and file SHA-256
hashes in `copilot/tmp/ppa/tt15-results/system-input-manifest.json`. Core uses the
archived PPA RTL snapshot based on `ba03b4775711695925a364e899a939994bed6331`
plus the PPA edits recorded in `copilot/tmp/ppa/core-18-slvt-ff-v2/` metadata
and source manifest; a commit alone does not reproduce that dirty snapshot.

Clock uncertainty remains 50 ps, clock/input transition 20 ps, I/O delay
100 ps, output load 2 fF; reset is the only false-path exception. The flow
includes full-design register optimization and automatic clock gating, then
hold repair. No placement, clock-tree synthesis or extracted interconnect is
included. Area is Liberty cell/macro area, not floorplan area. Report units are
ps, fF, mW dynamic and pW leakage; no arbitrary 4×/16× division is applied to
these Liberty results to account for scaled layout geometry.

Scripts are preserved in `copilot/ppa/tt15-detached/`. Retrieved reports are
under `copilot/tmp/ppa/tt15-results/{core-logic-eco-v4,router32-logic-eco,system64x2}/`.
The System's filtered hold result is from the separate report-only checkpoint
audit in `system64x2-audit/`; that audit completed successfully in 17 min 15 s
and reproduced the power anomaly (approximately 43.5 W, including 42.8 W net
switching). The table now uses the corrected power report in
`system-exact-load/reports/power.rpt`, produced by
`system_exact_load_audit.tcl`; the original synthesis report is preserved.
On `meow@radl-spr-01.bj.intel.com`, final run directories under
`/home/meow/copilot/tmp/` are:

- `ActiveN-core-tt15-logic-eco-v4`
- `ActiveN-router32-tt15-logic-eco-v3`
- `ActiveN-system64x2-tt15-detached-v3`

## ASAP5 experiment status — 2026-09-22

Requested initial point: **Core and Router_32, TT / 25 °C, 1.75 GHz
(571.428571 ps)**, retaining the architectural configuration above. The intended
voltage is 0.7 V, subject to an actually characterized ASAP5 library corner;
changing a nominal-voltage label is not voltage characterization.

The server checkout `/home/meow/asap5`, revision
`97962f34ae9754fabd119cfbb88137f9779acbfb`, contains ASAP5 PDK r0p4 transistor
models and sample OpenAccess cell layouts. No standard-cell Liberty timing/power
files or synthesis `.db` libraries were found there or in the additional server
library/archive search. `cds.lib` is a Cadence library mapping and `tech.db`
is an OpenAccess technology database, not a Synopsys timing database. The
upstream [directory description](https://github.com/The-OpenROAD-Project/asap5/blob/main/asap5PDK_r0p4/docs/pdk_directory_structure.txt)
also describes the TT `.pm` file as a transistor compact model.

**ASAP5 synthesis and frequency search have not started:** they require a
characterized standard-cell `.lib` or `.db` package, including sequential cells
and a compatible clock-gating implementation. Library Compiler can translate
Liberty to `.db`; it cannot derive timing/power tables directly from the PDK.
The existing SRAMs are ASAP7/budgetary models and must remain explicitly
identified as such if reused for an ASAP5 logic experiment.

Once the library is available, start independent detached Core and Router runs
at 571.428571 ps, map and retime fresh RTL at each candidate period, apply hold
repair, and retain every run's constraints/reports/checkpoint. Bracket a passing
and failing frequency, then bisect to a 1% bracket and rerun the best passing
point. Require nonnegative setup and logic-only hold slack at unchanged I/O
constraints and uncertainty; report raw SRAM hold separately. This is a
measured synthesis convergence criterion, not a claim of physical signoff or
global optimality. Track area and power at every point against the 50 mW/Core
target, retaining the vectorless/SRAM-model caveats.

## Historical fast-corner single-Core synthesis result

The best validated single-Core result is **1.8797 GHz** at a **532 ps clock
constraint**, with **33,233.14 µm² (0.03323 mm²) area including 64 scratchpad
SRAM banks and 16 event SRAMs**.  The mapped design reports zero setup and hold
violations at that constraint.  Its highest measured bounded activity window is
**38.634 mW including the current SRAM models**, below the 50 mW per-Core target.
This is a synthesis estimate, not physical signoff or representative 512-PU/16-MC
system power.

The result uses the worktree RTL at commit **`ba03b4775711695925a364e899a939994bed6331`**
plus the authorized uncommitted RTL changes present in this worktree, with two
SMT threads, one 64-byte ICache line, 16 KiB useful scratchpad implemented as 64
SRAM banks, 16 event queues implemented as 2RW SRAMs, access clock gating, and
the existing two-cycle external FPU interface.  Standard cells are ASAP7 **SLVT
FF, 0.77 V, 0 °C**; SRAM views remain the original **0.7 V** budgetary models.
The 80 SRAM instances are preserved as macros.

The timing constraints are: `core_clock` period **532 ps** (1.879699 GHz), 50 ps
clock uncertainty, 20 ps clock and input transition, 100 ps input/output delay,
2 fF output load, and reset as the only false path.  No FPU multicycle exception,
false path, or relaxed timing exception was used.  Full-Core retiming was applied
on a fresh mapped design, followed by hold-only mapping and 78 targeted
non-inverting data-path buffers.  The candidate passes eight bounded workloads,
12,538 clock-phase comparisons with zero defined-output mismatches, and all SRAM
clock-edge checks.

The **1.8797 GHz number was not obtained by a frequency sweep**.  We first mapped
the fresh design with a 500 ps (2 GHz) target; it missed setup by 30.67 ps after
retiming.  We then selected a fixed 532 ps target (approximately 1.88 GHz), ran
hold repair, and verified zero setup/hold violations.  We also validated a
separate 555.556 ps (1.8 GHz) candidate.  Thus 1.8797 GHz is a successful fixed
constraint point, not a binary-searched maximum; because its final slack is only
about 0.001 ps setup and 0.009 ps hold, it should be treated as a practical
synthesis point with no guardband.

This is not universally better than the previous normal-voltage result.  The
older comparison used RVT typical at 0.7 V and 25 °C, while this result uses
SLVT fast-fast at 0.77 V and 0 °C.  The faster cells, higher voltage and fast
corner explain most of the improvement, together with fresh full-Core retiming,
SRAM macro replacement, clock gating and targeted hold repair.  A fair voltage
comparison requires the same cell flavor, process/temperature corner, SRAM views,
constraints and optimization flow.  The SRAM models are not voltage-matched, and
there is no placement, extracted interconnect or physical clock-tree analysis.

Exact scripts are copied under
`copilot/ppa/timing-clean-188/`, including the fresh 2 GHz mapping,
532 ps constraint and hold-repair flows.  Reports, netlists, replay logs and
power audits remain under `copilot/tmp/ppa/timing-refinement/hold-closed-188/`.

You should elaborate RTL locally, and run synthesis on the server `ssh meow@radl-spr-01.bj.intel.com`.
The server has synopsys dc installed at /opt/synopsys/syn. You can start the `dc_shell` with `bash $(which dc_shell)`, and wait for a little bit. Please use an interactive shell because environment variables set in .zshrc is important (e.g. license files).

The PDK you should use is ASAP7, which is a predictive PDK. It's is installed at ~/asap7, with community-contributed Synopsys enablement files at `~/asap7/asap7_snps`. ASAP7 has some well-known quirks: such as it's a "scaled" PDK. Some tool will give result in 4x. You need to perform various conversion to get the predictive 7nm result. You can refer to UCB's Hammer project's ASAP7 setup to see how the conversions are done, or even use it directly. But IIRC, they only supports ASAP7 on Mentor Calibre, not Synopsys toolchains.

We currently do not have a memory compiler installed. So you need to manually rewrite some of the memory using SRAM macro to get a reliable result. Also, there are several identical structures reused across the system:
- Cores in different PU (with the pu id bounded with different constants)
- Distributors in different distributors (also, with different constants)
- Memory controller (different constants)
- Routers, but this is harder, because the computed routing table is different. Chisel will generate them as different modules.
You may be able to "harden" individual design to save up on synthesis work.
