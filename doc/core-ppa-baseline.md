# Single-Core PPA baseline

Measured 2026-09-10 with Design Compiler Y-2026.03-SP2. Synthesis completed,
including power reporting and mapped DDC/Verilog/SDC output. **The baseline does
not meet 1 GHz.** All memories are implemented in standard cells; SRAM macros
are not included in this original run. Later SRAM and optimization experiments
are documented in [core-ppa-experiments.md](core-ppa-experiments.md).

**Follow-up:** the user confirmed that the earlier 28 nm result used full
retiming and excluded SPM implementation, with an estimated SRAM timing
boundary. Its approximately 50 mW result was for Core minus SPM. The comparable
arithmetic subtraction here is **209.620 − 153.878 = 55.742 mW**, but this is
not a fresh synthesis with SRAM boundary constraints. The isolated retiming run
completed at 0.097023 mm², 209.575 mW and -717.76 ps setup slack. Subsequent
runs replace the SPM and event payload arrays with macro timing views; see the
[experiment comparison](core-ppa-experiments.md). The tables below retain the
original unretimed baseline.

## Results

| Quantity | Result |
| --- | ---: |
| Standard-cell area, 1× geometry | **98,529.22 µm² = 0.098529 mm²** |
| Leaf cells / sequential cells | 880,932 / 179,090 |
| Latches / macros / black boxes | 0 / 0 / 0 |
| Clock constraint | 1,000 ps = 1 GHz |
| Worst setup slack | **−904.88 ps** |
| Setup violating paths reported by DC | 145,298 |
| Worst hold slack / hold violations | −12.53 ps / 5,119 |
| Nets with transition / capacitance violations | 15 / 1 |
| Reported vectorless total power | **209.620 mW** |
| Cell internal / net switching power | 208.966 / 0.579 mW |
| Leakage power | approximately 0.0744 mW = 74.4 µW |

Sources: `copilot/tmp/ppa/core-baseline/reports/{area,qor,timing,power}.rpt`.
Power-table rounding accounts for the small component-sum discrepancy. The
power report uses **mW for dynamic/total power and pW for its leakage column**;
its `7.44e+07` leakage entry is 74.4 µW.

These are synthesis estimates, not placed-and-routed or signoff results. DC
reports no wire load and an undefined physical total area. There is no clock
tree or extracted interconnect. The clock connects to 179,090 loads; DC emits
TIM-134 for the high-fanout clock net, whose optimized hierarchical alias is
`exec/biu/evq_1/ram_ext/W0_clk`. Timing uses an ideal clock.

## Where the area goes

These rows are disjoint; parent/child hierarchy areas are not added together.
Queue rows include their local FIFO or priority-queue controls.

| Block | Cell area (µm²) | Core area | Reported vectorless power (mW) |
| --- | ---: | ---: | ---: |
| 16 KiB scratchpad | **72,171.95** | **73.25%** | 153.878 |
| All 16 event FIFOs, 4 KiB combined | **16,482.09** | **16.73%** | 38.158 |
| Send priority queue | 2,637.74 | 2.68% | 6.046 |
| Core broadcast FIFO | 2,034.54 | 2.06% | 5.468 |
| Both SMT register files | 1,188.21 | 1.21% | 2.340 |
| FPU | 860.50 | 0.87% | 0.0409 |
| One-line ICache, including control | 510.29 | 0.52% | 1.287 |
| Core ingress priority queue | 247.25 | 0.25% | 0.633 |
| Remaining logic | 2,396.66 | 2.43% | approximately 1.77 |

Scratchpad and event FIFOs account for **89.98% of Core area**. Event payload
arrays alone occupy **16,390.03 µm²**; the remaining 92.06 µm² is local FIFO
control. Ingress arbitration and the scheduler sit outside these FIFO rows.

The complete scratchpad replacement, including macros and wrapper logic, must
occupy less than approximately **72,172 µm² per PU** to beat this baseline.
The event-payload budget is **16,390 µm² per PU**, before accounting for new
prefetch state or arbitration.

For a scratchpad example, 64 independently enabled 256 × 16 unmasked banks,
each using only eight data bits, have a combined installed raw LEF footprint
of **15,945.15 area units**. If those dimensions are treated as 1× µm geometry,
that is 15,945.15 µm² before the wrapper, versus 72,171.95 µm² for the current
scratchpad. This supports its priority even with substantial byte-bank padding.
It is a conditional footprint comparison, not validated macro PPA: physical
scaling, model quality, and LSU read/write timing still need validation.

The [memory assessment](memory-macro-assessment.md) gives port requirements,
queue concurrency, system capacities, and SRAM-model limits. It recommends
shallow SRAM/register-file macros for further queue studies; the installed
minimum depth of 256 is poorly matched to the 4–32-entry queues.

## Timing and power interpretation

The worst path starts at `exec/uop_funct7_reg[1]` and ends at
`exec/fpu/r_held_reg[8]`. It passes through FP operand/operation selection and
`fpnew_cast_multi` conversion logic, including normalization and rounding.
DC reports 71 logic levels and a path length of 1,846.43 ps; the setup-required
arrival time is 941.55 ps after uncertainty and setup time.

[FPU.sv](../FPU.sv) configures CVFPU with `PipeRegs: '{default: 0}` and captures
the result in `r_held`. Its `retiming_backward` attribute is described in the
source as a Vivado hint. This baseline uses `compile_ultra -no_autoungroup`,
without `-retime`; it does not establish the best timing obtainable with DC
retiming or a different FP pipeline. **Scratchpad replacement alone does not
remove this reported worst path.** No FP, queue, or LSU timing boundary was
changed. Hold and electrical violations remain; the checkpoint is not closed.

### Follow-up FPU path audit

Read-only queries on the mapped checkpoint show that the slow path is **not
unique to conversion**. The arithmetic query traverses the implemented
`fpnew_fma` unit used for FADD/FSUB/FMUL; this does not imply the ISA exposes FMA.

| Query | Launch → capture | Arrival (ps) | Slack (ps) |
| --- | --- | ---: | ---: |
| Original unrestricted worst path, conversion | `uop_funct7[1]` → `r_held[8]` | 1846.43 | −904.88 |
| Through arithmetic result pins | `uop_funct7[4]` → `r_held[23]` | 1846.28 | −904.65 |
| Through conversion result pins | `uop_funct7[1]` → `r_held[24]` | 1840.19 | −898.60 |
| Through non-computational result pins | `uop_funct7[1]` → `r_held[8]` | 964.78 | −23.23 |
| Through registered FPU output | `r_held[11]` → `directval_3[11]` | 286.43 | +646.35 |

The last path uses register-file write-data forwarding into the next consumer;
it is not merely the path to the register-file storage. These are independently
selected worst paths and must not be added as though they were a single path.
Reports and the completed query log are in `copilot/tmp/ppa/fpu-audit/` and
`copilot/tmp/ppa/fpu-path-audit.log`. A through-pin query is a constrained path
selection and need not reproduce the unrestricted report's exact worst path.

The RTL's two-cycle FP latency consists of an execution-input register, a
free-running `r_held` register after all FP logic, and a subsequent writeback
cycle. `PipeRegs: '{default: 0}` adds no internal CVFPU pipeline. Thus the
original synthesis required the full calculation to finish in the first
cycle, leaving substantial slack after `r_held`. It did not test balanced
two-cycle operation. Retiming is the next experiment; a blanket two-cycle
multicycle constraint would not be justified by this RTL because `r_held`
captures every edge and another SMT instruction may change the inputs.

On the arithmetic path, operation decode reaches the FMA input at 151.89 ps,
the normalization leading-zero counter input at 1036.13 ps, its count output at
1155.73 ps, the rounding input at 1644.48 ps, and the rounded value at 1761.21 ps.
The remaining result selection reaches `r_held` at 1846.28 ps. On the original
conversion path, operand selection ends at 226.68 ps, leading-zero count at
541.66 ps, sticky/round preparation at 1388.81 ps, rounding at 1563.29 ps, and
final selection/sign handling at 1846.43 ps. Retiming must balance normalization,
shifting, rounding, and their control paths, not just move the final mux.

Grouping the existing constraint report also shows substantial memory timing
violations. These counts sum to the 145,298 setup violations in `qor.rpt`:

| Endpoint group | Setup violations | Worst slack (ps) |
| --- | ---: | ---: |
| Scratchpad data registers | 131,072 | −862.72 |
| Event FIFOs, including local control | 10,567 | −278.17 |
| FPU | 32 | −904.88 |
| Other | 3,627 | −758.95 |

The event group's worst endpoint is `evq_15/deq_ptr_value_reg[3]`, so payload
replacement must also account for the surrounding queue-control timing.
`endpoint-timing-summary.json` is generated by
`copilot/tmp/ppa/summarize_endpoint_timing.py`; setup and hold counts are checked
against QoR, including a hold violation rounded to zero in the printed report.

Primary signal inputs were assigned static probability 0.5 and 0.1 transitions
per clock. Reset was held inactive for reporting. DC used low-effort,
zero-delay activity propagation and emitted **PWR-415: unannotated sequential
cell outputs**. No SAIF or workload trace was used. In particular, the low
reported combinational FP activity is not the energy of executing FP operations.

Most reported power is internal power in clocked storage. These figures
illustrate the register-based memories under this model; they are not validated
workload power or a prediction of a working 1 GHz chip. Do not multiply
209.620 mW by 512 and call it system power. Macro comparisons need read/write/
idle activity, credible memory power models, and consistent clock/interconnect
accounting. The installed SRAM models report zero leakage.

## Configuration and reproduction

| Setting | Value |
| --- | --- |
| Elaboration | `--core`, default standalone configuration |
| SMT pipes / FPU | 2 / enabled |
| Scratchpad / memory line | 16 KiB / 512 bits |
| ICache | One 64-byte line, one way |
| Standalone controller-size list | One 4 GiB entry |
| PU identity | Generic `hartid` input, not bound to a constant |
| Standard cells / PVT | ASAP7 7.5-track RVT TT NLDM / 0.7 V, 25 °C |
| Clock uncertainty / transition | 50 ps / 20 ps |
| Input delay / transition | 100 ps / 20 ps |
| Output delay / load | 100 ps / 2 fF |
| Reset | False path from reset; case analysis 0 after mapping |
| Hierarchy / retiming | `-no_autoungroup`; no `-retime` |
| DC / Library Compiler | Both Y-2026.03-SP2 |

Standard-cell Liberty area matches the explicitly 1× LEF: for example,
`DFFHQNx1_ASAP7_75t_R` has area 0.2916 µm² in both views. **Do not divide these
DC areas by 16.** Both initial and reloaded-checkpoint unit reports confirm ps
and fF. Power units come from `power.rpt`; generic `report_units` says N/A.

The follow-up scaling audit reads Hammer's actual
[ASAP7 plugin code](https://github.com/ucb-bar/hammer/blob/master/hammer/technology/asap7/__init__.py),
[defaults](https://github.com/ucb-bar/hammer/blob/master/hammer/technology/asap7/defaults.yml),
and [technology library configuration](https://github.com/ucb-bar/hammer/blob/master/hammer/technology/asap7/asap7.tech.json).
The defaults explicitly describe 4× **place-and-route LEF/GDS geometry** and the
plugin installs an Innovus `write_design` hook, `asap7_scale_final_gds`, to
downscale the output GDS. Its configuration pairs scaled LEFs with the ordinary
NLDM Liberty files. This is not a blanket scaling of Liberty delays or power.
We used DC directly, not Hammer's physical flow or its GDS hook.

For the actual installed `DFFHQNx1_ASAP7_75t_R`, Liberty area is 0.2916; the
1× LEF is 1.08 × 0.27 µm = 0.2916 µm², while the 4× LEF is 4.32 × 1.08 µm =
4.6656 µm². Therefore the DC cell area is already on the 1× scale. A physical
area measured from that 4× LEF would need division by 16. Timing is already
in ps, and the power report derives mW from V² × fF / ps; leakage uses the
separate 1 pW Liberty unit. Neither timing nor this power report gets a geometric
4×/16× correction.

All five staged NLDM Liberty files were also compared byte-for-byte with the
decompressed original `.lib.7z` archives under
`/home/meow/asap7/asap7sc7p5t_28/LIB/NLDM`; all five match exactly. No hidden
electrical rescaling was introduced during staging. As a magnitude check,
the cited DFF has 0.474736 fF clock-pin capacitance and 0.558822 fF data-pin
capacitance. The library-directory suffix `_28` is part of the ASAP7 standard
cell release name, not evidence that a TSMC 28 nm electrical model was loaded.

The run is staged at `/home/meow/copilot/tmp/ActiveN-core-1line-20260910-9TNH8D`
on `radl-spr-01.bj.intel.com`. Launch DC through an interactive shell to load
licenses, using the Bash launcher and
`SYNOPSYS_LC_ROOT=/opt/synopsys/lc/Y-2026.03-SP2`.

Local evidence is under `copilot/tmp/ppa/core-baseline/`:

- `dc_core.tcl` and `dc_core.licensed.log`: exact script and synthesis log.
- `core-input-manifest.json`: 45 source/config/filelist/header hashes, checked
  against the staged inputs before synthesis.
- `baseline-metadata.json`: revision and exact Core configuration; the
  uncommitted ICache edits mean the revision alone does not identify the inputs.
- `reports/Core.mapped.ddc`, `Core.mapped.v`, and `Core.sdc`: mapped outputs.
- `summary.json`: hierarchy totals extracted by
  `copilot/tmp/ppa/summarize_core_baseline.py`, with hierarchy consistency checks.

Compiled libraries and successful LC conversion logs are in
`copilot/tmp/ppa/compiled-asap7-rvt-tt.tar.gz`. The added license endpoint resolves
the HDL-Compiler blocker, and actual power reporting now works. No DC downgrade
was needed; cross-version DC/LC compatibility was not tested.

## Validation and system accounting

The mapped checkpoint reloads and links without messages. Timing-coverage checks
report no loops, missing input delays, or unconstrained endpoints; register
queries confirm zero latches. `check_design` reports generated hierarchy
connectivity warnings: unused ports, feedthroughs, shorted output aliases, and
unused hierarchical pins. The synthesis log also contains helper shell-redirection
warnings; optimization completed and all five libraries were mapped successfully.
These checks are not formal equivalence or physical signoff.

The follow-up inspection completed the clock, unit, register, link, and timing
coverage checks. A subsequent redundant query explicitly enumerating every
register as both startpoint and endpoint was stopped because of excessive
runtime and memory use. The completed baseline reports supply the endpoint
summary above. The exact inspection script/log are archived with the results;
the reusable `copilot/tmp/ppa/inspect_core_baseline.tcl` omits that broad query.

The one-line ICache passed a focused Verilator test (1,089 fetches, 773 refills),
the rebuilt single-Core cache smoke test (2,210 cycles), and the rebuilt
64-PU/2-MC hard-system barrier test (4,953 cycles). This regression shape is
separate from the final **512-PU/16-MC** target.

Replicating this generic Core gives a **50.447 mm² Core cell subtotal**, excluding
uncore and physical overhead. Removing its register-based scratchpads leaves
**13.495 mm²**, to which 512 complete macro-based scratchpads must be added.
The target controller configuration, bound PU IDs, routers, MCs, distributors,
response ring, and peripheral still need accounting. This is not chip area.

The requested shared-ICache proxy is one scratchpad-equivalent per cluster:
**32 equivalents, or 512 KiB of capacity**. The measured one-line cache residual
is 510.29 µm² and 1.287 vectorless mW per Core; it remains in the subtotal above.
Removing those stubs subtracts 0.2613 mm² across 512 PUs, before adding the new
shared-cache interface/control. Keep the residual explicit when applying the
proxy, and determine activity separately from capacity.
