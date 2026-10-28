# PPA optimization handoff

Updated 2026-10-28, Asia/Shanghai. This document records completed work and the
next experiments requested by the user. The next experiments below have **not
been implemented or launched** as part of this handoff. Status is based on
saved reports and the prior checkpoint audit, not a fresh server process poll.

## Starting point

- Current checkout: **546f654a72e02e25a4485466179bccd661dcc17a**,
  `Parallelize BIU candidate selection`.
- The selected routed Core result used base
  `ce187fe8d0e4681d9915255b07404dbde96e6645` plus the scheduler/BIU/Exec source
  snapshot identified below. Do not relabel that physical run as a synthesis
  of the later commit.
- Current `EventScheduler` generates `2^pipeCnt` decisions for all thread
  counts. The measured two-thread version used the same four-decision scheme;
  the subsequent generalization removed the direct-selection fallback for
  counts above two. Its 1/2/3-thread formal checks passed.
- At handoff, the only pre-existing untracked source is
  `ActiveN/src/main/scala/koneko/EventSchedulerCheck.scala`. It is an optional
  standalone proof harness, deliberately not required in the production patch.
- [TAPEOUT.md](TAPEOUT.md) contains the historical results. Old dated sections
  and experiment READMEs sometimes still say "running" or "pending"; use the
  completed results below and the actual reports instead.

## Fixed comparison assumptions

| Item | Current baseline |
|---|---|
| Process/corner | ASAP7 SLVT TT, 0.7 V, 25 C |
| Core frequency | 1.5 GHz, 666.666667 ps period |
| Core FPU | Full Berkeley HardFloat, explicitly selected at elaboration |
| Core shape | Two SMT threads, one ICache line, 16 KiB scratchpad |
| Memories | Scratchpad and event SRAM models enabled; 80 Core macro instances |
| Clock gating | SRAM port/bank gating, response payload enables, DC automatic clock gating |
| FPU latency | Existing two-cycle issue-to-writeback contract retained |
| Response stages | Two stages at the previously agreed interfaces |
| Core synthesis | Whole-design retiming, 80 ps setup uncertainty, 50 ps hold, 30 ps critical range |
| Final physical constraints | 50 ps setup and hold uncertainty; 100 ps input/output delays, both min and max |
| Other electrical constraints | 20 ps input/clock transition, 2 fF output load |
| Physical tools | Synopsys DC followed by ICC2; high-effort placement, CTS, routing and post-route optimization |
| Power activity | Vectorless, input probability 0.5 and toggle rate 0.1 relative to the clock; reset held inactive |

The latest DRAMIf run used **50 ps** synthesis setup uncertainty and did not
include the later Core-only 30 ps synthesis reserve. Preserve this distinction
when comparing experiments. The extra Core setup uncertainty applies to
internal clocked paths too; it is not an external-interface-only margin.

The long-term system target remains 512 PUs / 16 MCs. The 64-PU / 2-MC
elaboration is the current system experiment shape, not that final target.
The requested power target is approximately 50 mW per Core including SRAM;
the reported vectorless totals do not establish a workload power ceiling.

## Completed results

All entries are **routed** results at the baseline TT conditions, not synthesis
estimates. Failure counts are reported checks across timing groups, not the
number of all possible paths. Area is cell plus macro footprint, not die area.

| Configuration | Setup WNS / failures | Hold WNS / failures | Area (um2) | Vectorless power (mW) | Route DRCs |
|---|---:|---:|---:|---:|---:|
| Full HardFloat, before scheduler rewrite, 30 ps synthesis reserve | -5.642474 ps / 19 | -39.709747 ps / 6, non-SRAM | 33448.740 | 15.15296 | 5829 |
| Full HardFloat, scheduler rewrite, before final repair | -5.522609 ps / 14 | -0.085056 ps / 5, non-SRAM | 34218.021 | 17.75801 | 10674 |
| **Full HardFloat, scheduler rewrite + repair01** | **+4.950821 ps / 0** | **+0.401616 ps / 0, raw** | **34311.625** | **17.82331** | **10869** |
| DRAMIf, ordered issue + gated response payloads | -354.007843 ps / 9301 | -237.260223 ps / 2301, raw | 129744.430 | 85.30067 | 18407 |
| Router_32, October 17 high-effort baseline | -0.777364 ps / 2 | +0.180990 ps / 0 | 3287.032 | 2.191790 | See original reports |

**The selected Core meets setup and raw hold timing, including SRAM checks.**
An independent ICC2 session reopened `Core/repair_1` and reproduced zero
violations in all four path groups under the nominal constraints. The remaining
route DRCs and predictive RC/PG setup mean this is **not physical signoff**.
DRAMIf has no SRAM macros, so none of its hold failures can be excluded using
the known ASAP7 SRAM-model exception.

Selected Core dynamic power is 14.84189 mW and modeled leakage is 2.981422 mW;
floorplan core-region area is 67044.745 um2. These figures are Core-only,
excluding its router and any eventual shared cluster ICache.

The successful final repair copied the original routed checkpoint, disabled
power recovery, optimized with temporary setup/hold uncertainty of 60/55 ps,
and restored **50/50 ps before reporting and saving the selected result**.
It closed timing after the first `route_opt` pass. No RTL, clock period,
additional false path, or multicycle exception was introduced in this repair.

The frontend Fudian comparison was stopped after full HardFloat closure was
independently confirmed; there is no completed Fudian result for that frontend
iteration. Simplified HardFloat is not the selected baseline because it excludes
subnormal inputs/results from its correctness scope.

The historical approximately 1.88 GHz result used **SLVT FF, 0.77 V, 0 C** and
was synthesis-only. It is not evidence of routed TT performance. Setting
`set_voltage 0.77` with single-voltage TT libraries does not create a valid
TT/0.77 V characterization; retain TT/0.7 V for the new frequency experiments.

## What has changed in RTL

### Core

`EventScheduler.scala`, `BIU.scala`, and `exec/Exec.scala` now compute a scheduling
decision for every possible accepting-thread mask and select the completed
decision using the actual mask. Each decision contains dequeue-ready, handler
ID, 128-bit payload, handler PC, and wakeup. The scheduler computes PC directly
from the one-hot winner; Exec's local-send fast path retains its priority.

The late accepting-thread signal therefore runs in parallel with most event
selection logic. No register boundary, latency, scheduling priority, queue
depth, quota rule, or WFI behavior changed. Lower handler indices retain higher
priority, and the lowest permitted accepting thread receives the event.

FPU experiments are selectable with `--fpu-impl=hardfloat`,
`--fpu-impl=hardfloat-normal`, or `--fpu-impl=fudian`; the source default remains
CVFPU. IEEE FP32 remains in the shared register file. Use full HardFloat for the
next Core experiments unless making an explicitly labeled backend comparison.

### DRAMIf / common MemIf

The allocation-order issue-index queue was selectively ported from mnist
`0a5d9b1df54c31c70d1d5dad49b4d0aec9f6f871`. The new packet format and revised
idle semantics were **not** ported. Current idle still means internal processing
complete, not necessarily the new response pipelines drained.

Two stages were added at Core unicast ingress, MemIf unicast egress, ring ingress,
and external-memory-response ingress. Ring ingress advertises ready=true under
the established nonblocking-ring invariant, with assertions checking internal
acceptance. `Common.delayValid` advances valid every cycle and enables payload
registers only when the preceding input/stage is valid. This enables clock
gating; it is not supply power gating. The response-stage experiment deliberately
changes interface latency; the later scheduler rewrite does not.

MSHR payload storage remains `Reg(Vec(inflight, UInt(memBusWidth.W)))`:
**256 x 512 bits = 16 KiB of flop-based storage** in the measured DRAMIf.
The ordered queue stores indices and does not pipeline this payload readout.
Common MemIf changes also affect PeripheralIf. Simulator alignment remains
explicitly out of scope; full regression for these uncore changes is deferred.

## DRAMIf: where closure is blocked

The read-only October 25 audit reproduced the final slacks exactly:
`copilot/tmp/ppa/dramif-path-audit-20261025/README.md`, `endpoints.tsv`, and
the input/internal/output setup and hold reports.

| Path group | Setup WNS / failures | Hold WNS / failures |
|---|---:|---:|
| Input to register/gate | -89.050354 ps / 4724 | -1.188755 ps / 39 |
| Internal register/gate | -260.261292 ps / 3571 | -237.260223 ps / 2262 |
| Register to output | -354.007843 ps / 1006 | Met / 0 |

1. **Request readout:** `core_clock_r_REG229_S2 -> mem_req_bits_wdata[274]`.
   The issue-queue index selects MSHR metadata and the 256-entry payload buffer,
   then drives the external request directly. Clock-to-output delay is about
   818 ps, including about 225 ps of wire delay and 593 ps of cell arcs; 21
   buffer/inverter stages contribute substantially. This indicates a costly
   selection/distribution network, not proof that congestion alone causes it.
   Of the worst 200 setup paths, 199 end at request write-data and one at request
   address. All 512 request data bits and 32 address bits fail.
2. **NoC ingress/allocation:** all 4724 listed worst input setup checks start at
   `nocIn_0_valid`, through priority arbitration, class decode, allocation and
   distribution to MSHR state or clock enables. Worst adverse clock skew is
   about 105 ps. Response staging did not address this request-side cone.
3. **Internal control/gating:** worst path ends at
   `clk_gate_allocated_0_reg/latch/ENA`. It has about 609 ps of cell delay,
   50 ps of interconnect, and 185 ps of adverse skew. Other failures reach
   unicast payload/round-robin and ring-forwarding queue clock gates. These are
   real clock-gating setup checks, not paths to mark false.
4. **Hold/clock balance:** the worst bulk/broadcast metadata path has about
   286 ps adverse launch/capture skew and -237 ps hold slack. Lower frequency
   will not repair it. There are also 39 small input hold failures involving
   ring data, external response data, and NoC source IDs.

Only one unicast output check remains failing (-0.691 ps), whereas request,
broadcast, and ring outputs still fail. It would be incorrect to focus further
work solely on unicast response slices or solely on the external request port.

## Next experiments requested by the user

### 1. Continue DRAMIf closure on the server

Start from the ordered-issue/gated-response snapshot and its routed audit at
1.5 GHz TT. Preserve it as a comparison point. Candidate experiments:

- Buffer accepted NoC requests before the allocation/distribution network,
  preserving priority, ordering, throughput, and backpressure.
- Separate issue-index selection from metadata/payload readout, then consider
  an elastic external request stage. A register only after the complete 818 ps
  cone may merely move the violation to an internal path.
- Investigate banked SRAM or separating request write data from response data.
  The current buffer has allocation and response writes and request, unicast,
  and broadcast reads. Inventory simultaneous accesses, arbitration, and
  read-during-write semantics before selecting macro ports/latency; a 2RW macro
  is not automatically sufficient for this structure.
- Shorten allocation/free and response-selection enables, and address CTS/ICG
  clock balance and minimum delay. Report raw hold, because DRAMIf has no SRAM
  exceptions.

These are proposed investigations, not completed RTL changes. Establish the
precise ready/valid and cycle-boundary implications before inserting new stages;
do not silently treat an extra request cycle as cycle-equivalent.

### 2. Factor EventScheduler's wide output muxes

Current: `2^pipeCnt` complete decisions each select payload and PC, followed by
a wide final selection. Requested experiment:

```text
for each constant accepting mask:
    calculate narrow arbitration result
actual accepting mask -> select narrow result
selected winner -> one shared payload Mux1H and one shared handler-PC Mux1H
```

Retain the general `2^pipeCnt` array. A safe narrow decision representation can
carry `winner`, `hasCandidate`, and `ready`; wakeup can either remain precomputed
as a small per-decision field or be derived after selection and compared for
timing. Calculate the encoded handler ID from the selected winner.

Two equivalence traps matter:

- When no candidate is valid, the original arbiter selects handler **15**, and
  exposes that handler's payload/PC, but wakeup must be zero. Preserve the
  fallback winner plus a separate validity indication.
- `ready = grant & enabled` is **not** simply the one-hot winner. Empty,
  enabled higher-priority queues can have ready asserted. Preserve exact ready
  behavior independently of the payload winner.

This should reduce RTL duplication of the 16-to-1, 128-bit payload mux and PC
mux. The actual area gain is unmeasured because synthesis may already share
logic. It moves a payload/PC mux **after** the late accepting-mask selection, so
it can increase delay on the path that was just closed. Compare at unchanged
1.5 GHz first, using identical constraints and physical effort; retain the
existing implementation as the control. Do not claim a fourfold physical saving
from the four RTL instances.

### 3. Raise Core frequency

Use full HardFloat at SLVT TT/0.7 V/25 C. Suggested first point: **1.6 GHz
(625 ps)**, followed by a bracket/refinement search if it closes. This is a
proposed search point, not an already measured result. Run the current scheduler
as the baseline and the shared-mux candidate separately so an area/timing
tradeoff is not confused with a frequency change.

Update every period-dependent field together: `create_clock`,
`optimize_registers -delay_threshold`, launch manifests, completion messages,
and exported SDC. The archived scripts hardcode 666.666667 in several places.
Retain 80/50 ps DC and 50/50 ps final uncertainty and 100 ps I/O delays. Use the
same physical repair strategy, restoring nominal constraints before reports.
Require actual routed setup and hold checks at each candidate frequency;
do not infer a new closed frequency from hold slack or a single setup WNS.
Report area, power, all timing groups, failure counts, and route DRCs together.

## Validation available for reuse

- `EventSchedulerCheck.scala` instantiates the old Chisel Arbiter and the new
  scheduler with shared arbitrary inputs. Yosys SAT proves `mismatch == 0` for
  ready, ID, payload, PC, and wakeup, including invalid cycles. There are no
  assumptions on input masks/data/eligibility; the reference output-ready is
  tied true, matching the actual BIU. Quota arithmetic and queue state are
  outside this combinational boundary.
- The latest generalized implementation passed concrete 1/2/3-thread proofs;
  logs: `copilot/tmp/ppa/scheduler-polymorphic-20261028/equivalence-{1,2,3}.log`.
  Three threads exercises eight decisions. This is not proof for every parameter.
- Initial frontend integration passed seven dual-Core differential workloads
  (AUIPC, ICache, memcopy, scratchpad, two FPU tests, local send) with identical
  top-level outputs at every evaluated cycle outside reset.
- Rebuilt hard 16-PU/1-MC integration passed local-send, ping-pong, and randomized
  NoC tests. The 64-PU simulator build exceeded local RAM.
- Full HardFloat/Fudian arithmetic tests against SoftFloat each covered 282,764
  operations, including rounding modes, special values, conversions, bubbles,
  and latency. See `copilot/ppa/fpu-experiments-20261023/README.md`.
- **No whole-Core post-retiming or post-PnR LEC was run.** The optional checker
  can remain outside the source patch, but copy it separately for the next
  scheduler experiment. Production compilation/elaboration does not depend on it.

## Server, artifacts, and migration requirements

Existing host: `meow@radl-spr-01.bj.intel.com`. License variables are initialized
by the user's shell startup files; invoke launch commands through `zsh -lic`
or an equivalent initialized interactive shell. Do not assume a plain
noninteractive SSH command inherits the license setup.

Tools used by the saved supervisors:

```text
/opt/synopsys/syn/Y-2026.03-SP2/bin/dc_shell
/opt/synopsys/syn/Y-2026.03-SP2/fusioncompiler/bin/icc2_shell
/opt/synopsys/lc/Y-2026.03-SP2
/opt/synopsys/scl/2025.03-SP2
```

LC was repaired and compiled the five SLVT TT timing databases. Reuse the
verified databases rather than interpreting an absent upstream `.db` as an
absent TT Liberty corner. The PDK is under `/home/meow/asap7` on that host.

| Artifact | Location |
|---|---|
| Core synthesis/PnR scripts and launchers | `copilot/ppa/frontend-20261025/` |
| Core original run on server | `/home/meow/copilot/tmp/ActiveN-frontend-i01-hardfloat-20261025/Core/` |
| Selected Core repair on server | `/home/meow/copilot/tmp/ActiveN-frontend-i01-hardfloat-repair01-20261028/` |
| Selected physical checkpoint | Repair root: `design.ndm`, block `Core/repair_1` |
| Selected reports / DEF / SPEF locally | `copilot/tmp/ppa/frontend-20261025/i01-hardfloat/repair01/` (`pass1/`, `audit/`) |
| DRAMIf scripts | `copilot/ppa/ordered-gated-20261023/` |
| DRAMIf original run on server | `/home/meow/copilot/tmp/ActiveN-ordered-gated-20261023/DRAMIf/` |
| DRAMIf physical checkpoint | Above directory: `design.ndm`, block `DRAMIf/routed` |
| DRAMIf final reports locally | `copilot/tmp/ppa/fpu-margin-20261025/status-20261025/DRAMIf/` |
| DRAMIf detailed audit locally | `copilot/tmp/ppa/dramif-path-audit-20261025/` |
| Shared standard-cell NDMs, native tech adapter, RC inputs on server | `/home/meow/copilot/tmp/ActiveN-pnr-synopsys-20261015/` |
| Reused macro NDMs and library/bridge inputs on server | `/home/meow/copilot/tmp/ActiveN-high-effort-20261017/` |

Core original source inventory SHA256:
`13d6f1fe53eb24f88ebcd60467089e85195e8167da33a384c31489d6b8d2f2c8`.
DRAMIf original RTL patch SHA256:
`441a21da5d3ec951a064e81d150f5e07bfa407d662407d2405b846d628ee49a7`.
Use the associated source manifests, not just Git HEAD, to identify old runs.

**A Git clone alone does not contain the experiment infrastructure:** `/copilot`
and `/generated` are ignored. Transfer the relevant script directories, manifests,
reports, optional checker, and any needed immutable input bundles separately.
To resume an existing physical result on another server, also transfer the design
NDM and all referenced standard-cell/macro libraries, technology and RC inputs,
or retain access to their existing server paths. Local DEF/SPEF reports alone
are not a replacement for the saved ICC2 checkpoint and reference libraries.

The scripts are useful templates, not portable turnkey entrypoints: they contain
absolute `/home/meow` paths and dependencies on prior experiment directories.
`launch.py` reuses library/bridge inputs and macro NDMs; `package.py` checks old
proof-log locations; `launch_repair.py` names a particular original checkpoint.
Update those in a new experiment directory, bind proof evidence to the new source
snapshot, and use fresh run names. Never overwrite a completed reference run.
Regenerate DRAMIf's dependency closure from the current elaborated modules;
Chisel numbered module names can change.

The detached Python supervisor uses `start_new_session=True`, redirected stdin
and logs, and starts ICC2 after successful DC completion. Verify the detached
PID/session, `status.json`, `progress.jsonl`, `synthesis.log`, and `pnr.log` after
launch. Completion markers indicate execution only; inspect nominal timing,
PVT, macro counts, unconstrained paths, exceptions, and routing reports before
declaring closure. Previous PIDs are historical and must not be used blindly.

## Re-elaboration and proof commands

Run project commands inside the Nix development shell. The current local
container requires `--extra-experimental-features 'nix-command flakes'` before
`develop`; omit that flag only where those features are already enabled.

```sh
nix --extra-experimental-features 'nix-command flakes' develop

# Inside the development shell, from the repository root:
mill --no-server ActiveN.run --core \
  --spm-sram --event-sram --spm-clock-gating --event-clock-gating \
  --fpu-pipeline --insert-stages=2 --fpu-impl=hardfloat
mill --no-server ActiveN.run --system --pu=64 --mc=2 \
  --spm-sram --event-sram --spm-clock-gating --event-clock-gating \
  --fpu-pipeline --insert-stages=2 --fpu-impl=hardfloat

# Requires the optional checker and Yosys, copied separately if omitted from Git:
mill --no-server ActiveN.runMain koneko.EventSchedulerCheckMain
YOSYS=/path/to/yosys bash copilot/ppa/frontend-20261025/prove.sh
```

The proof script writes historical log paths: copy/adapt it to a new evidence
directory for new experiments. Confirm generated JSON and source hashes before
packaging. Disk saturation occurred earlier; check capacity before Nix builds
or large snapshot transfers. No new synthesis/PnR is needed merely to consume
this handoff document.

## Interpretation limits to preserve

- Power includes available macro power views, but scratchpad leakage is missing
  and event SRAM uses a budgetary model with placeholder leakage. No manual
  leakage allowance is included in the table. These are neither measured trace
  power nor guaranteed upper bounds.
- Scratchpad activity must respect banking/access limits; do not sum all bank
  write powers as though the workload could activate every bank simultaneously.
  The user requested a maximum of 32 B written per cycle for the SRAM estimate.
- The physical flow uses native 1x geometry and an experimental technology
  adapter plus calibrated predictive RC. Do not divide the reported area by
  16 or timing/power by four again. Read report units: current ICC2 power is pW
  (divide by 1e9 for mW), while DC total power reports use mW.
- Preserve `predictive_rc.tcl`: its metal-resistance and layer-capacitance
  corrections are part of this comparison flow, not arbitrary output scaling.
  The available stack is not a qualified extraction/signoff deck.
- Do not remove FPU/control/ICG paths to obtain timing closure. Keep the original
  reset exception and audit all other exceptions. The selected Core needed no
  SRAM hold exclusion; any future filtered hold result must accompany raw hold.
- Use Synopsys physical tools, as requested. The ASAP5 source checkout alone
  did not provide a released standard-cell timing-library replacement; stay on
  ASAP7 for these experiments.
