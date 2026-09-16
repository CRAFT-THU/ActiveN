# Bounded trace-based Core power

The selected candidate reaches **1.8797 GHz**, with **0.03323 mm² per Core** and
**38.63 mW in its highest tested power window, including the current SRAM
models**. It has zero synthesis setup/hold violations and passes eight bounded
cycle replays. This meets the 50 mW target for the tested activity and provisional
models; representative SNN power and physical timing closure are not established.

## Final timing-clean candidates

Both use ASAP7 SLVT FF, 0.77 V, 0 °C, the original 0.7 V SRAM views, two SMT
threads, one ICache line, scratchpad SRAM and event-payload SRAM. Area and timing
use the audited normalized libraries without another ASAP7 scaling factor.

| Candidate | Area including SRAM (mm²) | One-thread arithmetic steady (mW) | Two-thread arithmetic steady (mW) | Two-thread SRAM/FPU steady (mW) |
| --- | ---: | ---: | ---: | ---: |
| 1.8 GHz, incremental retiming and hold repair | 0.032558 | 30.50 | 24.45 | 34.33 |
| 1.8797 GHz, fresh explicit retiming and hold repair | 0.033233 | 32.73 | 28.12 | 38.63 |

Each candidate passes the same eight tests, totaling 12,538 clock-phase
comparisons with zero defined-output mismatches. All 96 SRAM port-edge counts
match the original RTL, including 1,112 bank edges in the memory-active test
and three event SRAM edges in the local-message test. Each has its own mapped
waveforms and eleven-window power audit; activity is not borrowed between
retimed netlists.

The 1.8797 GHz memory-active window contains 18.32 mW of logic plus all
interconnect, 18.25 mW of SPM internal power and 2.06 mW of event SRAM leakage.
It completes the same 189 cycles in 100,548 ps. Relative to the repaired 1.8 GHz
candidate, frequency rises 4.43%, total cell area 2.07%, and this window's power
12.53%. Its energy increases from 3.605 to 3.885 nJ. The faster candidate is the
performance choice within the modeled budget, not the lowest-energy choice.

Both power audits annotate 100% of nets and ports: 79,016 and 80,875 nets
respectively, and 1,488 ports each. All four pins of all 375 ICGs and every SRAM
clock pin have simulated activity. Missing pins are disconnected outputs:
512 SPM bits, 2,048 event write-port bits and respectively 25 or 11 unused adder
carry outputs. SAIF durations and SRAM clock-rate integrals are checked against
the actual waveforms. Surplus waveform objects (644 and 779 respectively) do
not represent missing design activity.

Final evidence is under
`copilot/tmp/ppa/timing-refinement/hold-closed-18/` and `hold-closed-188/`:
`replay-summary.json`, `power-summary.json`, `power-audit.tar.gz`, model/library
hashes and the timing reports. Remote power audits are
`/home/meow/copilot/tmp/ActiveN-closed18-power-20260911` and
`/home/meow/copilot/tmp/ActiveN-closed188-power-20260911` on the designated server.
The timing flow and netlist hashes are in
[core-frequency-experiments.md](core-frequency-experiments.md).

The remaining sections retain the original-netlist measurements and explain
the workloads, counting method and model limitations.

## Original mapped-netlist measurements

Four short regression workloads estimate **7.26–11.38 mW per Core including the
current SRAM models**, at a 1.8 GHz measurement point. Sustained arithmetic windows
increase this to **30.15 mW with one active SMT thread and 24.17 mW with two**.
An additional two-thread SRAM/FPU loop estimates **34.10 mW** during its steady
window, including active SRAM reads and writes.
These are trace-informed
synthesis estimates, not representative SNN power, worst-case power, or silicon
measurements. Representative SNN compliance with the 50 mW budget and physical
timing closure remain unproven.

## Configuration and results

The existing mapped Core uses two SMT threads, one 64-byte ICache line, 16 KiB
useful scratchpad in 64 SRAM banks, 16 event SRAMs, full-Core retiming and clock
gating. Standard cells use ASAP7 SLVT FF, 0.77 V, 0 °C. SRAM tables remain the
original 0.7 V views; this is not a matched PVT corner. Results use the audited
1× area and ps/fF Liberty units, with no additional geometry scaling.

| Workload | Measured cycles, excluding reset | Logic + all interconnect (mW) | SPM internal (mW) | Event SRAM internal + leakage (mW) | Total Core (mW) |
| --- | ---: | ---: | ---: | ---: | ---: |
| FPU verify | 263 | 4.93 | 0.27 | 2.06 | 7.26 |
| Extended FPU | 567 | 5.59 | 0.61 | 2.06 | 8.27 |
| SPM macro | 331 | 6.33 | 2.98 | 2.06 | 11.38 |
| Memory copy | 455 | 6.82 | 2.31 | 2.06 | 11.19 |

The logic/interconnect column subtracts macro internal power and leakage from
Core total, retaining **all net switching exactly once**, including nets driven
by SRAM and ICG cells. Summed cell reports have rounding error. SPM input/address
activity can consume internal power even when no bank receives a clock edge;
clock gating is not operand isolation. Event SRAMs receive no clock pulses in
these tests, so their approximately 2.0624 mW contribution is placeholder leakage.
SPM leakage is absent from its source model and is therefore missing from totals.

Windows cover each short test after reset, including startup and ICache refills.
For example, execute is occupied for only 49/263 cycles in FPU verify and 71/331
in the SPM test. These are not sustained arithmetic-throughput measurements.
The eventual shared cluster ICache may change activity substantially.

## Sustained arithmetic measurements

Two additional programs execute changing finite FP32 operands in a 48-byte loop
that fits the single ICache line. Each thread runs 32 iterations of four multiplies,
four additions and two subtractions, then checks all three results against an
independent reference that rounds each operation to FP32. The two-thread program
uses an atomic completion counter and parks the first finisher in WFI; it does
not spin and starve the other thread under the existing scheduler.

| Workload/window | Cycles | Logic + all interconnect (mW) | SPM internal (mW) | Event SRAM internal + leakage (mW) | Total Core (mW) |
| --- | ---: | ---: | ---: | ---: | ---: |
| One active thread, whole test excluding reset | 978 | 19.03 | 3.37 | 2.06 | 24.46 |
| One active thread, steady loop | 690 | 23.76 | 4.33 | 2.06 | 30.15 |
| Two active threads, whole test excluding reset | 1,755 | 13.42 | 2.89 | 2.06 | 18.37 |
| Two active threads, overlapping steady loops | 637 | 17.54 | 4.57 | 2.06 | 24.17 |

The steady windows complete 360 instructions/300 FP operations and 592
instructions/454 FP operations respectively. This is 0.435 and 0.713 FP operations
per cycle. Execute-completion occupancy is 52.17% and 92.94%; it is not a count of
architectural writebacks. The windows exclude initial refills and final checks,
and contain no external requests/responses or SRAM clock edges. The full two-thread
test has 24 physical SPM bank edges for initialization and atomic completion;
the full one-thread test has none. Both include the modeled SRAM input activity
and leakage in the table, but neither steady window stresses memory accesses.

The lower two-thread result is an observation for these specific, identical
thread programs, not a general SMT power reduction. Bubble-related operand
updates and inter-thread data correlation may contribute; this has not been
isolated by a paired implementation experiment. Hierarchical power is also not
an architectural-unit breakdown after retiming moves logic across boundaries.

RTL execution succeeds at 989 and 1,766 cycles. Mapped replay matches all 1,978
and 3,532 clock phases with zero defined-output mismatches. All four power windows
pass the same 100% net/port coverage, disconnected-pin, 374-ICG and 96-SRAM-clock
audits as the short tests. The new local-message capture also passes 258 mapped
phase comparisons and exercises three event SRAM edges (two reads, one write);
its power has not been included in these arithmetic tables.

Evidence is in `copilot/tmp/ppa/activity-replay/stress-power-summary.json`,
`stress-power-audit.tar.gz`, `stress-windows.json`, and the `fpu_stress/`,
`fpu_stress_smt2/`, and `local_send/` directories. The synthesis audit is
`/home/meow/copilot/tmp/ActiveN-stress-power-20260911` on the designated server.
These use the original mapped-netlist hash below, not a subsequent timing-repair
candidate. The 50 mW budget still needs memory/event-intensive and representative
SNN activity, along with the SRAM model limitations described above.

## SRAM-active arithmetic measurement

The `memory_fpu_smt2` program runs two threads on separate scratchpad regions.
Each executes 16 iterations of four word loads, four FP32 multiplies and four
word stores in a 56-byte loop. Both check all four final memory values, then use
the atomic completion/WFI protocol. RTL succeeds in 1,725 cycles and mapped replay
matches all 3,450 phases. Independent counts agree on 1,112 physical SPM edges:
552 reads and 560 writes, including initialization and final checking/completion.

| Window | Sampled cycles | Logic + all interconnect (mW) | SPM internal (mW) | Event SRAM internal + leakage (mW) | Total Core (mW) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Whole test excluding reset | 1,714 | 10.00 | 6.10 | 2.06 | 18.15 |
| Overlapping steady loops | 189 | 14.56 | 17.48 | 2.06 | 34.10 |

The overlapping steady window completes 178 instructions, including 48 FP
operations, and clocks 376 SPM bank edges. Event SRAM clocks remain idle. This
adds a memory-active budget measurement, but does not combine sustained incoming
events, bulk writes and a representative SNN workload.

All net/port, ICG and SRAM activity checks pass. An initial window starting exactly
on a falling clock transition produced two excess edge-equivalents when SAIF
toggles were divided by two. Moving the boundaries 2 ps beyond the settled low
phase fixes this discrepancy: SAIF rate integrals now match 1,112 and 376 actual
rising edges. The converter clips the whole-test window at the end of the VCD;
the audit reads its actual 952,220 ps duration rather than the requested stop.
The steady window is 105,000 ps. These are power-window adjustments only, with
no change to stimulus, RTL or execution cycles.

Reproduction uses `memory_windows.py`, `bundle_memory_power.py`,
`summarize_memory_power.py`, `memory-saif-timebase.json`, and
`memory-power-summary.json` under the activity-replay directory. The accepted
`memory-power-audit.tar.gz` comes from
`/home/meow/copilot/tmp/ActiveN-memory-power-v2-20260911`; the earlier boundary
failure is retained separately. As with the arithmetic tests, this is power of
the original netlist, not a later timing-repair candidate.

## Validation and annotation

- Fresh RTL runs retain prior completion cycles: 274, 578, 342 and 466.
- RTL-control replay and mapped replay both match all defined external outputs
  on both clock phases, totaling 3,320 phase comparisons in each model.
- Independent RTL and mapped counts agree for all 96 SRAM port clocks: FPU
  tests have zero SPM edges; SPM macro has 44 byte-bank reads and 23 writes;
  memory copy has 40 reads and 40 writes. These counts match the programs.
- All four DC runs annotate **73,963/73,963 nets and 1,488/1,488 ports** directly
  from simulation. All four pins of each of the 374 ICGs, and all 96 SRAM clock
  pins, have simulated activity. DC clock-rate integrals match the independently
  counted SRAM edges.
- Pin coverage is 258,259/260,844, or 99.01%. The 2,585 remaining pins are
  disconnected: 512 unused high SPM output bits, 2,048 unused event write-port
  output bits, and 25 unused adder carry outputs. DC reports confirm macro
  connectivity; the mapped Verilog omits all 25 adder carry connections.

The initial replay released reset and raised the clock in one evaluation,
causing an extra refill-counter edge through an automatically inserted gate.
Settling inputs on the previous phase before applying the next edge resolves
this harness error without an RTL change or extra cycle. Functional models use
1 ps sequential scheduling delays for event ordering; they do not model actual
cell delay, repair hold violations, or establish physical glitch power.

The first SAIF audit missed 3,202 underscore-named nets because Verilator omits
those names by default. Rebuilding with `--trace-underscore` restored every one,
and all four replays and SRAM edge checks still passed. Only the corrected audit
supports this table. DC also reports 642 surplus waveform objects that do not
exist in the DDC; full net coverage demonstrates these are not missing design
activity. Saved relative library paths generated warnings, but all five exact
SLVT FF libraries were loaded by absolute path, linking succeeded, and the
power reports identify the correct libraries and units.

## Clock-gating implication

Auto-gating coverage does not imply that bubbles suppress clocks. In FPU verify,
the mapped gates named `exec/clk_gate_delayed_r_reg` and
`fetch/clk_gate_holding_0_smsel_reg` pass 263 and 253 edges respectively, despite
only 49 occupied execute cycles. In the SPM test they pass 329 and 321 edges out
of 331 cycles, with 71 occupied execute cycles. Retiming can give these gates
fanout beyond the RTL register in their names. These observations support
investigating narrower semantic enables; they do not prove that all those edges
can safely be removed or quantify additional power savings.

## Limits and next work

The original netlist has -2.18 ps setup slack and -58.87 ps hold slack at
1.8 GHz; the final candidates above repair these violations while preserving the
tested cycle behavior. There is no placement, synthesized physical clock tree, extracted
interconnect or timing-accurate glitch activity. SRAM leakage/energy uncertainty
and unmatched SRAM PVT remain significant. No supply power gating is implemented.

Next measurements need concurrent memory/event-queue traffic and representative
SNN windows. Frontend/datapath enable experiments
must preserve drain, response capture and same-cycle wakeup behavior, then
compare the same traces and timing constraints before and after each change.
The end system remains 512 PUs / 16 MCs; these single-Core numbers cannot be
multiplied by 512 to claim total system power without uncore and activity analysis.

## Reproducibility and resource use

Scripts and provenance live in `copilot/tmp/ppa/activity-replay/`:
`prepare.py`, `refresh_tracing.py`, `resume_build.py`, `run_replays.py`,
`count_ports.py`, `check_mapped_clocks.py`, `bundle_power.py`, `power.tcl`,
`summarize_power.py`, `power-summary.json`, and `full-power-audit.tar.gz`.
The final remote audit is
`/home/meow/copilot/tmp/ActiveN-activity-replay-full-trace-20260911` on the
TAPEOUT-designated synthesis server. Source netlist SHA256 is
`92f4c94ec2c32ebfa859728365c9a2fafac8ef16a651e3aa90622ef7b7bcf89e`.
Binary/stimulus hashes, exact windows in ps, and comparison logs are archived.

The memory-heavy GCC aggregate build was stopped. The successful build uses
already-installed offline Nix store paths, one Clang job and separate compilation
units. Unchanged build files are reused. No full development-shell reconstruction
or unrelated cleanup is performed. Each waveform is bounded to 1 GiB and
compressed immediately after a passing replay.
