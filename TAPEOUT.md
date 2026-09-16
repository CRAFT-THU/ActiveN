## Current single-Core synthesis result

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
