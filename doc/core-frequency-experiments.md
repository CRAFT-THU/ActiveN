# Core frequency and faster-cell experiments

## Result

The selected single-Core candidate reaches **1.8797 GHz**, with **0.03323 mm²
area including SRAM** and **38.63 mW in its highest tested window**. It uses
ASAP7 SLVT FF at 0.77 V and 0 °C. It has zero synthesis setup/hold violations
and passes eight bounded cycle replays plus an eleven-window activity-power
audit. The SRAM models remain provisional and unmatched to the standard-cell PVT.

A repaired 1.8 GHz candidate also passes those checks, with 0.03256 mm² area
and a highest tested window of 34.33 mW. Both retain the original absolute
constraints and essentially zero timing margin. Placement, physical clock-tree
synthesis and extracted interconnect are not included; these are synthesis PPA
estimates, not physical timing closure or a representative SNN budget guarantee.

The initial five-run comparison below preceded these repairs. Its fastest run
missed setup by 2.18 ps at 1.8 GHz and retained hold violations. The later flow,
validation and final netlist hashes are detailed at the end of this report.

The higher-voltage library is also fast process and colder temperature. This is
not an isolated voltage experiment. No matched TT 0.77 V view was available in
the inspected installation; the TT comparison uses 0.7 V and 25 °C throughout.

| Standard cells | PVT | Area including SRAM (mm², 1×) | Setup slack at 1.8 GHz (ps) | Setup-only implied frequency (GHz) | Default vectorless total (mW) |
| --- | --- | ---: | ---: | ---: | ---: |
| RVT | TT, 0.7 V, 25 °C | 0.032349 | -481.01 | 0.965 | 8.911 |
| LVT | TT, 0.7 V, 25 °C | 0.032202 | -262.79 | 1.222 | 9.527 |
| SLVT | TT, 0.7 V, 25 °C | 0.032364 | -130.07 | 1.459 | 11.306 |
| LVT | FF, 0.77 V, 0 °C | 0.032194 | -77.30 | 1.580 | 13.866 |
| SLVT | FF, 0.77 V, 0 °C | 0.032191 | -2.18 | 1.793 | 14.673 |

Power in this table is reported at the **1.8 GHz target**, not at the implied
setup-only frequency. Default gate-enable activity is inferred and very low;
these totals are not measured workload power or validation of the 50 mW budget.
Areas come directly from the normalized Liberty views, without a second ASAP7
geometry scaling. The existing scaling audit is in `core-ppa-baseline.md`.

## What was exercised

All five use the exact validated RTL snapshot with two SMT threads, one-line
ICache, 16 KiB useful scratchpad in 64 SRAM banks, 16 event-payload SRAMs, access
clock gating and the internally staged FPU with unchanged external cycle latency.
The source archive SHA256 is
`3747cd9cdf8e58258360efc47be9201f0e9414087c9ea5cae6d41acaebfce8ee`.

Each run uses full-Core `compile_ultra -retime -gate_clock`, automatic ungrouping,
followed by incremental retiming/gating when setup fails. Constant/multiple-port
nets are buffered using `set_fix_multiple_port_nets -all -buffer_constants`.
Only the SRAM instances are protected. The existing absolute constraints remain:
50 ps clock uncertainty, 20 ps clock/input transition, 100 ps input/output delay
and 2 fF output load. Reset remains the only false-path source; no FPU multicycle
or false-path exceptions were introduced.

For LVT/SLVT, the 96 explicit memory ICG instances are rebound to the corresponding
cell flavor before mapping, including shared module instances. Mapped reference
reports confirm only the selected standard-cell flavor is instantiated. All 80
SRAM instances remain present, as checked by the per-cell power report parser.
The final Tcl, logs, netlists, DDC checkpoints and retiming SVF are archived for
every candidate. All five tool sessions exit successfully.

Twenty source Liberty files were compiled without modifying their contents:
five each for LVT TT, SLVT TT, LVT FF and SLVT FF. Their operating conditions and
source/content hashes are recorded in `copilot/tmp/ppa/fast-libraries-provenance/`.
RVT FF was excluded because its installed SIMPLE view declares a different PVT
from the other four RVT FF groups. An early duplicate-library/shared-ICG binding
attempt was discarded; only the completed `-v2` runs are results.

## Timing implications

For SLVT FF, the critical path starts at execute operation decode and ends at
the FMA internal sum register. Its arithmetic front/back slacks are -2.18/+0.07 ps;
conversion front/back are +0.12/+0.38 ps. Extending conversion alone would not
fix this arithmetic path. These margins are too small to imply physical closure.
Worst hold slack is -58.87 ps.

CVFPU is also not the only concern: the independent 1 GHz RVT audit finds
**-32.57 ps outside the FPU**, almost equal to the overall -32.68 ps. Integer
arithmetic/forwarding, execute-to-fetch, LSU write data and multiply all have
violating paths. Details are in `core-ppa-experiments.md`. Replacing CVFPU alone
cannot be assumed to solve frequency closure. The next implementation comparison
should include these paths and the timing cost of any new coarse clock boundary.

## Power budget and SRAM limitations

The budget is 50 mW **including SRAM**. The present memory timing/energy tables
remain the original 0.7 V models for every run. FF results therefore combine fast
standard cells with unmatched SRAM views; they are exploratory, not a matched
signoff PVT. No SRAM delay tables were scaled to invent a higher-voltage model.
Scratchpad leakage is absent from its source model and event-SRAM energy/leakage
is an uncalibrated placeholder.

For the fastest SLVT FF candidate, uniform ICG enable sensitivity gives:

| Enable probability for every gate | Logic + all nets (mW) | SPM internal (mW) | Event SRAM internal + leakage (mW) | Total (mW) |
| ---: | ---: | ---: | ---: | ---: |
| 0.01 | 6.831 | 5.193 | 5.914 | 17.939 |
| 0.10 | 13.936 | 49.673 | 19.697 | 83.306 |
| 1.00 | 74.691 | 494.394 | 159.240 | 728.325 |

These are artificial gate-enable annotations, not CPU utilization or a legal
traffic workload. Unlike the older 1% experiment, the new 1% case uses 0.01 enable
transitions per cycle; the 10–50% cases use 0.1 and the always-open case uses zero.
SRAM is already included in the total and must not be added a second time.

The scratchpad model's continuously clocked reference is about 4.29 pJ per bank
per cycle: four banks per scalar word imply about 17.2 pJ of clock-dominated
internal energy, or 30.9 mW if accessed every cycle at 1.8 GHz. All 64 banks on a
bulk-write cycle correspond to approximately 274.7 pJ. Actual read/write and
address/data activity must refine these references. This illustrates why counting
bank/port edges matters more than assigning one activity factor to the whole Core.

Paired trace-based power measurements now include arithmetic and SRAM-active
loops; the final timing-clean candidates remain below 50 mW in all tested
windows, subject to the SRAM model limitations. Detailed results are in
[core-activity-power.md](core-activity-power.md). Selective enable/coarse-gating
experiments remain a possible next optimization. The concrete
plan and gating feasibility assessment are in
[simulation-power-plan.md](simulation-power-plan.md). No 50 mW workload-power
claim is established by the current default report.

## Reproduction and evidence

- `copilot/tmp/ppa/frequency-summary.json`: checked summaries and all power cases.
- `copilot/tmp/ppa/core-18-{rvt-tt,lvt-tt,slvt-tt,lvt-ff,slvt-ff}-v2/`:
  exact run Tcl, source hash manifest, mapped reports and `results.tar.gz`.
- `copilot/tmp/ppa/summarize_frequency_experiments.py`: requires successful markers,
  no DC errors, correct cell flavor, matching RTL hash and 64+16 SRAM power rows.
- `copilot/tmp/ppa/fast-libraries-provenance/`: original Liberty hashes/PVTs and LC log.

The faster runs' early `libraries.rpt` diagnostic mistakenly selected the RVT
bridge library. Their actual mapped libraries/PVTs are correctly listed in
`area.rpt`, `power.rpt` and `references.mapped.rpt`; the preparation helper's
diagnostic selector is corrected for future experiments.

## Further retiming and hold repair (2026-09-11)

| Candidate | Clock period (ps) | Setup violations | Hold violations | Area including SRAM (mm²) | Validation status |
| --- | ---: | ---: | ---: | ---: | --- |
| Original SLVT FF | 555.555556 | 132 | 5,401 | 0.032191 | Eight bounded activity replays pass |
| Incremental retiming plus final hold repair | 555.555556 | 0 | 0 | 0.032558 | Eight bounded replays and eleven-window power audit pass |
| Fresh mapping plus explicit register retiming, 2 GHz target | 500 | 2,953 | 6,349 | 0.032829 | Exploration only; setup slack −30.67 ps |
| Same fresh candidate, hold repair at 532 ps | 532 | 0 | 0 | 0.033233 | Eight bounded replays and eleven-window power audit pass |

The 1.8 GHz flow resumes the mapped original with incremental full-Core retiming
and clock gating. A separate `compile -incremental_mapping -only_hold_time
-map_effort high` reduces hold violations to 52 without sacrificing setup.
Targeted non-inverting data buffers finish the remaining short paths. The first
buffer pass repaired the single-line ICache data branches; the last four repairs
select branches with sufficient setup margin, including FPU operand-register
inputs. No clock or timing exception changes are used for this repair.

An `optimize_registers` attempt on an already-retimed checkpoint reports
RTDC-142 and skips retiming. That attempt is discarded. The faster flow instead
starts from the same original RTL snapshot, maps with `compile_ultra -gate_clock`,
then runs `optimize_registers -minimum_period_only -clock core_clock
-delay_threshold 500 -justification_effort high` on the fresh mapped design.
Explicit retiming improves setup slack at 500 ps from −77.80 to −36.57 ps;
subsequent mapping reaches −30.67 ps. The installed tool documents
`compile_ultra -timing_high_effort_script` as ignored, so it is not counted as
an optimization.

The faster design is constrained to 532 ps (1.879699 GHz) and receives a separate
hold-only pass, followed by 78 targeted non-inverting buffers. Final reported
slacks are approximately +0.0014 ps setup and +0.0086 ps hold. The 1.8 GHz design's
corresponding slacks are +0.0020/+0.0007 ps. These tiny values should be read as
zero margin at synthesis precision, not useful physical guardband. The 532 ps
candidate retains all 80 SRAM instances; standard-cell area is approximately
10,338 µm² and macro area approximately 22,895 µm².

All logic optimization runs remove the saved report-only reset case analysis
first, preserving reset functionality. Reset is set to zero again only for
normal-operation reporting. Before/after reports agree on the material setup
and hold results. The buffer-only passes do not optimize Boolean logic and
preserve connectivity function. Full-Core retiming may move logic into a
hierarchy whose name no longer identifies its architectural function.

The explicit retimer issues RTDC-92 for negative computed delay arcs on some
small combinational cells and uses zero for those arcs in its retiming search.
The original libraries are unchanged and final STA uses the supplied views.
The default zero `max_leakage_power` optimization objective remains violated;
it is not the user's 50 mW total-power limit. Setup/hold and electrical constraints
are checked separately. Workload power requires new activity from each candidate.

Scripts and compact audit archives are in `copilot/tmp/ppa/timing-refinement/`.
The final 1.8 GHz netlist SHA256 is
`1b7836ef93983be8acdd164be772b66203582ca2346469b14f2360250b8a1c48`;
the 532 ps netlist SHA256 is
`7a4d5d005558d58735f51000e91d53c051aa5e5efe2659b00e31b15874691599`.
Remote final candidates are `ActiveN-hold-eco3-20260911` and
`ActiveN-hold-188-eco-20260911` below `/home/meow/copilot/tmp/` on the designated
synthesis server. No production RTL changes are made in these experiments.
