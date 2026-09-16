# Clock gating and trace-based Core power estimation

## Status and scope

This is an assessment and implementation plan with an initial bounded activity
replay prototype; it is not yet a representative workload power profiler or a
new RTL clock domain. It uses the validated two-SMT Core with a one-line
ICache, SRAM scratchpad, SRAM event payloads, and the existing two-cycle FPU
interface. The performance target is 1.8 GHz and the budget is 50 mW per Core
**including SRAM**. The eventual system has 512 PUs and 16 MCs.

## Existing gating

The implementation gates clocks; it does not disconnect the supply or remove
leakage. The 1 GHz RVT TT mapped design contains 279 automatically inserted ICGs
and 96 explicit memory ICGs. DC reports 17,355 of 17,520 registers gated (99.06%).
This counts coverage, not the fraction of edges suppressed or power saved.

The scratchpad's 64 byte banks have individual clock enables. A scalar word read
clocks four banks, while writes clock the selected byte lanes; a full bulk write
can clock all 64. The 16 event queues have separate read/write port gates. Their
read enables include the collision-bypass recovery needed to keep the prefetched
head valid. Enables are held stable during the high clock phase. Existing unit
and integration comparisons preserve accepted transfers, result latency, and
completion cycles; see `core-ppa-experiments.md` for the validation record.

The approximately 26.36 mW logic figure forces all gate enables active. The
5.884 mW figure is a default vectorless **total**, including SRAM, with very low
inferred enable activity. They are different activity assumptions and scopes,
not a measured 30-to-5 mW improvement on the same workload.

## Feasibility of frontend/backend gating

Both are feasible without adding a wakeup cycle, provided the enable arrives
before the original active edge and every observable state transition is
preserved. A single `!fetchable` or `!exec.valid` test is insufficient.

| Candidate domain | Reasons it must receive a clock edge | Recommended first boundary |
| --- | --- | --- |
| Frontend | New fetch, branch/handler redirect, pending fetch/decode transfer, held instruction consumption or kill, ICache reset, request acceptance, response capture, refill serialization and completion | PC/decode/holding state first; retain a responsive ICache/refill controller |
| Execute datapath | New issue, current instruction progression, FPU/Mul/LSU result drain, delayed writeback, handler argument writes, WFI/yield and CSR state changes | Issue/execute/data registers and arithmetic units, with explicit valid/drain conditions |
| Entire `Exec` module | All of the above plus asynchronous copy responses, SPM writes, AMO replay, incoming unicast/broadcast, outgoing queue progress and BIU scheduling | Defer whole-module gating; keep BIU, response handling and wakeup logic independently responsive |

Specific source evidence:

- `Fetch.scala`: `step` follows `icache.input.ready` even with no fetchable thread;
  `sentSmsel` and `held` can still represent useful instructions. Holding data
  updates under `!held`, and decode registers under `step`, including idle cycles.
- `ICache.scala`: a `Valid` response cannot be backpressured. After capture,
  `s1refillWriting` serializes a beat across multiple cycles without another
  response. Reset and killed-refill bookkeeping also need edges.
- `Exec.scala`: `s0step = !valid || s1done` is true on an empty execution stage.
  Thus the existing operand/uop register enables are not semantic idle detectors.
  `delayedSent` may still write back when `valid` is false. WFI is consumed by
  idle/scheduler logic outside the normal instruction issue path.
- `Mul.scala`: the output is an unconditional `RegNext`, with no operation-valid
  enable. `Exec` also advances delayed uop metadata unconditionally. These are
  candidates for meaningful enables even though most *other* registers are gated.
- `BIU.scala`: scheduling can dequeue an event and write handler arguments on the
  same cycle as wakeup. Broadcast serialization and send queues progress separately
  from instruction execution. `ext.working` is not a backend-quiescent indication.
- `LSU.scala`: asynchronous copy responses update state and write SPM independently
  of the current execute instruction. Pending requests, AMO replay and delayed
  loads must be included in any drain predicate.

A held instruction blocked by execution need not keep all frontend data registers
clocked. Likewise a stable wait for memory need not clock the arithmetic datapath.
Those are finer opportunities than waiting until both SMT threads execute WFI.
Always-responsive combinational wake detection must reopen the relevant ICG before
the edge on which the original design would accept work. Do not insert a registered
wakeup delay, combinationally AND a clock, or gate the logic that must generate its
own wakeup. Reset/test bypass, enable setup/hold checks, and cross-domain clock
skew must be handled explicitly. Clock gating also does not by itself isolate
combinational logic from changing external operands.

## Size of the remaining opportunity

Expect substantial overlap with automatic gating, but 99.06% coverage does not
prove that useful idle opportunities are exhausted. Large FF queues dominate the
register count; a small ungated arithmetic/control subset can have disproportionate
power. Existing gates with broad enables can continue clocking during bubbles.
An upstream gate can additionally stop child ICG input activity and downstream
clock-tree branches, even if the child gates already suppress register clocks.
The current ideal-clock synthesis estimate has no synthesized physical clock tree.

A report-only audit of the same mapped DDC quantifies the ICG contribution at
1 GHz, 0.7 V, RVT TT:

| ICG enable annotation | All 375 ICGs: internal power (mW) | Nets driven by ICGs (mW) |
| --- | ---: | ---: |
| Baseline propagated activity | 0.13590 | 0.15202 |
| Every enable continuously high | 0.36121 | 4.60404 |
| Every enable continuously low | 0.12645 | 0 |

The 0.12645 mW is the residual child-gate internal activity that an upstream gate
could largely suppress while idle, before subtracting its own overhead. It is
only about 2.1% of the default 5.884 mW total, even if eliminated continuously.
This strongly limits the benefit of merely adding another level of gating in
the current synthesis model. Savings from correcting broad enables, ungated
register clocks, and a future physical clock tree are additional and require
measurement. The table's net power is already included in Core switching power;
do not add it again. Forced enable annotations are sensitivity experiments, not
functional operating modes or measured idle power. Reports and a 375-row checked
sum are in `copilot/tmp/ppa/coarse-gating-audit/`.

Under the *current default annotations only*, logic plus all net switching is
2.366 mW. This is a generous ceiling on additional logic-side savings in that same
report: real gating cannot remove all of it, and cannot remove SRAM leakage.
It is not a bound for an active workload or a post-CTS implementation. Do not
predict another multi-fold whole-Core reduction from the register coverage count.

The existing soft-backend random-message regression supplies one illustrative
observation: 16 PUs, two SMT threads, 265,433 sampled cycles, execution valid in
15.6136% of Core cycles, and at least one active thread in 91.6802%. Consequently
84.3864% are execution-empty and 8.3198% have both threads idle. These are **not
safe-to-gate fractions**: they do not subtract issue, drain, responses or wakeups,
nor measure clocks already stopped by DC. This test and its one-line ICache are
not representative of the eventual SNN workload/shared ICache.

Measure incremental savings by comparing the same trace, netlist scope, PVT,
frequency and memory model with and without each candidate enable. For a candidate
domain, an approximate opportunity is its *remaining* active-clock power times
the newly suppressible edge fraction, minus gate/control and wakeup overhead.
Do not multiply total Core power by execution-empty fraction.

## Planned measurement implementation

1. **Common observation contract.** Add an optional host-side collector shared by
   `sim_single`, hard-system and soft-system Core probes. Reuse the current profile
   windows and peripheral timer semantics. In system mode sample settled signals
   after `stage(K)` and before `step(K)`, once per Core per cycle, preserving the
   existing one-dump FST convention. The current soft profiler already runs at
   the start of `step`, before worker release and registered-state commit; reuse
   that point. In `single.cpp`, sample before the next posedge after the preceding
   negedge has settled, retaining the existing cycle numbering. Counters live in
   C++, with simulation-only RTL probes if signals
   are unavailable; avoid adding synthesized counters or preservation attributes
   that change mapping. Label the existing `valid && s0step` statistic as execute
   completion: delayed architectural writeback is a separate event.
2. **Counters and windows.** Record instruction issue/completion/writeback by
   class, FPU operation class, active SMT count, stalls, fetch/decode occupancy,
   cache requests/refill work, memory requests/responses, and handler wakeups.
   Count accepted FIFO enqueue/dequeue for every handler, allowing both in one
   cycle. Count actual SPM bank read/write edges and event SRAM port edges,
   including prefetch/bypass recovery; instruction counts alone miss these.
   Export per-PU totals and configurable cycle windows with run/configuration,
   RTL/netlist and memory-model hashes, frequency, PVT, seed and exact duration.
3. **Shadow gating statistics before RTL changes.** Compute conservative frontend
   and datapath quiescence predicates without gating anything. Record each reason
   preventing gating, newly suppressible edges relative to existing enables,
   joint frontend/backend idle fraction, idle-run lengths and wakeup causes.
   Validate drain and same-cycle wakeup cases using handshakes and waveforms.
   Report alternatives that retain a responsive ICache/BIU/LSU boundary separately
   from deep whole-domain idle. Do not infer functional safety from a histogram.
4. **SRAM energy ledger.** Integrate characterized read/write/idle clock energy
   per physical bank/port and leakage over elapsed seconds, using measured address,
   data and mask activity where the model supports it. Account for the physically
   present unused SPM bits. If the power tool already includes a macro's internal
   and leakage power, replace that contribution with the ledger, rather than add
   it twice; keep interconnect and ICG energy exactly once. Current SPM leakage is
   missing and FIFO energy/leakage is a placeholder, so retain an explicit model
   uncertainty range. A voltage-only projection is not a characterized SRAM view.
5. **Mapped logic activity.** Capture clock-phase-resolved Core input stimuli for
   bounded representative windows and replay on the mapped, retimed netlist,
   checking outputs against the original simulation. Include reset/warmup or a
   validated state-transfer scheme; arbitrary mid-trace replay cannot invent the
   retimed state. Generate power-tool-compatible VCD/SAIF and audit annotation
   coverage, unknowns, ICG outputs and macro activity. RTL traces alone do not
   describe DC's retimed registers or inserted gates. The system's once-per-cycle
   FST is a functional trace, not a waveform with both clock edges for power;
   generate the power waveform in the replay flow without changing that contract.
   Zero-delay activity omits physical glitches; use timing-annotated replay and
   extracted capacitance for a later physical estimate.
   Start with the installed DC activity/power flow and a Verilator-compatible
   functional replay using the supplied `asap7sc7p5t_28/Verilog` cell models,
   after checking module coverage and ICG/flip-flop behavior. `pt_shell` and `vcs`
   are not currently on the server's interactive PATH; timing-annotated power
   analysis is a later tool-flow dependency. Convert waveform time explicitly
   to the selected clock period in ps; do not apply a second ASAP7 geometry
   scaling factor to already normalized Liberty energy or timing.
6. **Paired experiments and validation.** First verify instrumentation on/off has
   identical external transfers, values and cycle counts. Check counter totals
   against small traces with simultaneous FIFO enqueue/dequeue, scalar/bulk SPM
   operations, FPU bubbles, delayed writeback, WFI plus incoming events, branches,
   refill completion and reset. Then try semantic register enables and coarse
   domain gating independently, with sequential equivalence where available and
   cycle comparisons otherwise. Preserve ready/valid behavior and reopen on the
   original acceptance edge. Run timing including ICG enable checks and compare
   retiming freedom: new hard clock boundaries may obstruct full-Core retiming.

The output should report energy per task, average and maximum-window power,
logic/SPM/event-SRAM breakdown, idle versus active energy, activity coverage and
model uncertainty. These are trace-informed estimates, not measured silicon
power. Use the SNN trace and final shared-cache behavior to assess the 50 mW budget;
scale to 512 PUs only after accounting for per-PU activity variation and uncore
energy separately.

## Evidence

- `copilot/tmp/ppa/core-validated/reports/clock_gating.rpt`
- `copilot/tmp/ppa/core-validated/reports/clock_gating.identified.rpt`
- `copilot/tmp/ppa/core-validated/reports/power.rpt`
- `copilot/tmp/ppa/core-validated/power-scenarios.json`
- `copilot/tmp/ppa/corrected-integration/noc-profile/summary.txt`
- `copilot/tmp/ppa/corrected-integration/cycle-comparison.json`
- `copilot/tmp/ppa/coarse-gating-audit.tcl` (report-only ICG power sensitivity)

## Bounded replay progress (2026-09-11)

The completed experiment now covers eight workloads on each of two timing-clean
candidates, with 12,538 defined-output phase comparisons per candidate and no
mismatches. Their own eleven-window SAIF audits cover 100% of nets and ports,
all 375 ICGs and all 96 SRAM clocks. The selected 1.8797 GHz candidate reaches
38.63 mW including current SRAM models in its highest tested window, a two-thread
SRAM/FPU loop; the repaired 1.8 GHz candidate reaches 34.33 mW. This is below the
modeled budget for the tested activity, but not representative SNN validation.
No coarse frontend/backend gate or production profiling counters were added.
The final comparisons and reproducibility records are in
[core-activity-power.md](core-activity-power.md) and
[core-frequency-experiments.md](core-frequency-experiments.md).

The following records the initial prototype and the activity-coverage issues
resolved before those final measurements.

The prototype under `copilot/tmp/ppa/activity-replay/` captures both clock phases
from the existing single-Core simulator and replays its 54 input/output ports on
the 1.8 GHz SLVT FF mapped netlist. Four independent RTL-control replays and four
mapped replays pass with zero defined-output mismatches: FPU verify (274 cycles),
extended FPU (578), SPM macro test (342), and memory copy (466). These comparisons
are bounded functional checks, not formal sequential equivalence.

An initial mapped mismatch was traced to the replay harness releasing reset and
raising the clock in one evaluation. The reset term held an automatically
inserted ICG open for that edge and incorrectly incremented the refill counter.
Settling inputs on the previous clock phase before applying the new edge resolves
the mismatch without changing RTL or adding a cycle. The functional cell models
retain a 1 ps sequential scheduling delay; this is not characterized timing or
a repair of the reported hold violations.

Independent counting of all 96 physical SRAM port clocks agrees between RTL and
mapped waveforms. After reset, the SPM test has 44 byte-bank reads and 23 writes;
memory copy has 40 reads and 40 writes. Neither FPU test clocks SPM, and none of
these four tests exercises event queues. Counts match the memory test programs.

The initial SAIF audit completes for all four traces, but covers only 95.67% of
hierarchical nets. All 3,202 missing nets begin with an underscore, which the
default Verilator tracing settings omit; they include register-file state. Rebuilding with `--trace-underscore` restores 100% net/port activity coverage;
all four replays and SRAM clock checks still pass. The remaining 2,585
unannotated pins are disconnected SRAM/adder outputs. The validated short-test
totals are 7.26–11.38 mW including the current SRAM models; see
[the detailed activity report](core-activity-power.md) for the breakdown,
coverage audit and limitations. Short boot-to-completion regression windows do
not establish SNN power, high-utilization power, or compliance with the 50 mW
budget. The original SRAM characterization limitations still apply.

To respect disk and memory limits, the replay build uses only existing offline
Nix store paths, one Clang compilation job, and separate compilation units; the
full development shell is not reconstructed. Waveforms are bounded, compressed
after successful replay, and subject to a 1 GiB per-file limit.

Actual gate-edge observations already show why coverage is not sufficient: in
the FPU-verify window, execute is occupied for 49/263 cycles, while the gates
named `exec/clk_gate_delayed_r_reg` and
`fetch/clk_gate_holding_0_smsel_reg` pass 263 and 253 edges respectively. In the
SPM test, occupancy is 71/331 cycles and those gates pass 329 and 321 edges.
These are mapped gate names, not a guarantee that their post-retiming fanout
is confined to the named RTL register. The evidence supports examining broad
enables; it does not establish how many of those edges are safely suppressible
or the incremental power benefit. Counts are in
`copilot/tmp/ppa/activity-replay/broad-enable-examples.json`.
