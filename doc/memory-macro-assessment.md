# Memory macro assessment

Assessment date: 2026-09-10. Target: 512 PUs, 16 MCs, 32 clusters.
First synthesis measurement point: one Core, ASAP7 7.5-track RVT TT,
0.7 V, 25 °C, 1 GHz. Two SMT pipes, FPU enabled, 512-bit memory lines,
16 KiB scratchpad. The ICache is reduced to one 64-byte line and one way.

This is the initial storage inventory and conversion assessment. Scratchpad and
event payload SRAM experiments have since been implemented and synthesized; see
[the candidate comparison](core-ppa-experiments.md). Capacities
below count declared payload fields, before synthesis removes unused bits;
queue pointers, valid bits, priorities, and other control state are additional.

The [completed single-Core baseline](core-ppa-baseline.md) measures
**98,529.22 µm²** of standard cells: **72,171.95 µm² (73.25%)** is scratchpad
and **16,482.09 µm² (16.73%)** is event FIFOs. Together they account for **89.98%**
of Core area. The 1 GHz constraint is not met (worst setup slack −904.88 ps),
and the 209.620 mW power report is vectorless with incomplete sequential activity.
These measurements strengthen the replacement ranking; they do not include SRAMs.

## Recommended order

1. **Scratchpads:** the strongest candidate, 16 KiB per PU and 8 MiB total.
   Specify byte-write support and the synchronous read behavior explicitly.
2. **Event queue payloads:** the next largest replicated storage, 4 KiB per PU
   and 2 MiB total. A compact implementation is worth investigating, but the
   queues cannot be merged into an ordinary one-write-port SRAM without
   addressing concurrent ingress and scheduler timing.
3. **DRAMIf response data:** 16 KiB per MC, 256 KiB total. Large enough to justify
   a redesign, but its current multiple readers and writers make conversion
   substantially harder than the scratchpad.
4. **Send queues:** 624 bytes per PU, 312 KiB total. Worth measuring with a
   shallow SRAM/register-file compiler; not an automatic win with the available
   deep SRAM shapes.
5. **Keep the current small/deeply parallel controls and shallow buffers in
   standard cells initially:** router input queues, core ingress queues,
   register files, handler controls, and two-entry response queues. Revisit the
   eight-entry core broadcast FIFO only if a suitable shallow, wide macro exists.

## Inventory

| Storage | Logical shape | Replication | Total at 512 PU / 16 MC | Initial judgement |
| --- | ---: | ---: | ---: | --- |
| Scratchpad | 256 × 512 bits | 512 | 8 MiB | Highest priority |
| Event FIFO | 16 × 128 bits | 16 per PU = 8,192 | 2 MiB | High-value architectural study |
| BIU send priority queue | 32 × 156 bits | 512 | 312 KiB | Conditional |
| BIU broadcast FIFO | 8 × 588 bits | 512 | 294 KiB | Shallow/wide; conditional |
| Core ingress priority queue | 4 × 156 bits | 512 | 39 KiB | Keep standard cells |
| Router input priority queue | 8 × 172 bits | 2,481 | 416.73 KiB | Keep standard cells initially |
| Integer/FP shared register file | 31 × 32 bits | 2 per PU = 1,024 | 124 KiB | Keep standard cells |
| Handler CSR banks combined | 16 × 49 bits | 512 | 49 KiB | Keep parallel control state |
| DRAMIf data buffer | 256 × 512 bits | 16 | 256 KiB | Strong capacity, difficult ports |
| DRAMIf MSHRs | 256 × 133 bits | 16 | 66.5 KiB | Split searchable state from selected data |
| DRAMIf bulk descriptor FIFO | 32 × 161 bits | 16 | 10.0625 KiB | Secondary candidate |
| PeripheralIf data buffer | 16 × 512 bits | 1 | 1 KiB | Low overall priority |
| PeripheralIf MSHRs, declared | 16 × 133 bits | 1 | 266 bytes | Much can simplify in this subclass |
| Ring forwarding FIFO | 2 × 560 bits | 17 | 2.324 KiB | Keep standard cells |
| DRAMIf/distributor broadcast FIFO | 2 × 588 bits | 16 + 32 | 6.891 KiB | Keep standard cells |
| Distributor unicast pipeline | 1 × 560 bits | 32 | 2.188 KiB | Pipeline register, not a macro candidate |

Sources: [parameters](../ActiveN/src/main/scala/koneko/Parameters.scala),
[SPM](../ActiveN/src/main/scala/koneko/exec/SPM.scala),
[BIU](../ActiveN/src/main/scala/koneko/BIU.scala),
[Core](../ActiveN/src/main/scala/koneko/Core.scala),
[flit types](../ActiveN/src/main/scala/koneko/bus/Flit.scala),
[MemIf](../ActiveN/src/main/scala/koneko/bus/MemIf.scala),
[Distributor](../ActiveN/src/main/scala/koneko/bus/Distributor.scala),
[RegFile](../ActiveN/src/main/scala/koneko/exec/RegFile.scala), and
[Exec](../ActiveN/src/main/scala/koneko/exec/Exec.scala).

The mesh is 32 × 16. [System](../ActiveN/src/main/scala/koneko/System.scala)
instantiates a queue for each directed mesh ingress and each local injection:
`2 × (31 × 16 + 15 × 32) + 512 + 16 + 1 = 2,481` queues. MC injection and
peripheral injection therefore matter to the total. The routing truth tables
are elaboration-time constants synthesized into logic, not writable memories.

These totals follow current source rather than historical architecture notes:
DRAMIf has 256 shared inflight slots, PeripheralIf has 16, and the event FIFO
depth is literally `16` in BIU rather than a use of `evQueueDepth`. The default
parameter also happens to be 16. No depth change is proposed here.

## Scratchpad mapping requirements

The scratchpad is a `SyncReadMem` with one synchronous read and one byte-masked
write, both using the same line address. At 512 bits per line its shape is
256 rows × 64 bytes. It is not a 4096 × 32 memory at the interface: asynchronous
copy writes an entire 64-byte line in a cycle.

The [LSU](../ActiveN/src/main/scala/koneko/exec/LSU.scala) arbitrates scalar
accesses, AMO accesses, and incoming asynchronous-copy writes before SPM. A
masked single-read/write-port implementation is a plausible mapping because
there is only one line address, but read-during-write behavior must be checked
at the LSU boundary. The present Chisel memory does not promise a defined
same-address read value during a write. Preserve the prior read used by AMO,
the delayed load response, and copy preemption/retry behavior; do not add a
cycle to scalar stores as an incidental consequence of replacing the memory.

Candidate organizations are a byte-masked 256 × 512 macro, sixteen byte-masked
256 × 32 banks, or eight byte-masked 256 × 64 banks. They have equal useful
capacity but different peripheral, clock, wiring, and read-energy costs.
Scalar reads ultimately select one 32-bit word, whereas the current SPM reads
the full line every cycle, without an explicit read enable. Selective bank/read
enabling could save power, but it is a separate LSU-aware optimization requiring
an approved design and timing validation.

The installed unmasked macros cannot directly replace this interface. One
functional organization to evaluate is 64 independently enabled 256 × 16 banks,
using only eight bits of each bank to implement byte writes. That doubles the
physical bit capacity and has 64 sets of memory peripherals. A whole-word
read/modify/write wrapper instead changes access scheduling and latency. Neither
cost should disappear from the estimate.

## Event queues: size is attractive, ports are the obstacle

Each handler owns a 16-entry, 128-bit FIFO. Current Chisel `Queue` instances use
combinational head reads; the scheduler selects among the 16 heads in the same
cycle. A synchronous SRAM needs a head-prefetch/output-register design or a
new scheduler boundary, with careful treatment of empty-to-nonempty transitions.

There are three independent ingress sources, demultiplexed by handler *before*
the per-handler arbiters. If their handler IDs differ, local push, unicast,
and broadcast can all enqueue in one cycle. The priority order applies when
they contend for the same handler. Consequently a single 256 × 128, 1R1W
memory does not preserve the existing ingress throughput. Candidate designs
must support the three writes, bank them without silent conflict loss, or make
an explicitly approved throughput change.

Keep occupancy, FIFO pointers, scheduler enable/margin state, and any prefetched
heads in registers. Preserve the 16 independent queue capacities; replacing them
with a shared free list changes capacity guarantees and deadlock reasoning.
An aggregation study should compare these costs against sixteen individual
small memories, not assume a free 256 × 128 replacement.

## Priority queues: payload versus metadata

[FlitQueue](../ActiveN/src/main/scala/koneko/bus/FlitQueue.scala) already stores
valid and one-hot priority separately from its payload. Its difficulty is the
combinational priority-selected payload read, not a shift-register data layout.
It allocates the lowest free slot, selects the highest priority, then the lowest
slot within that priority. `preserveOrder` defaults to false. Replacing it with
four conventional FIFOs would also change equal-priority selection behavior.

There is at most one enqueue and one dequeue per queue per cycle, potentially
at different addresses. A synchronous implementation needs independent read
and write capability or an equivalent banked design, plus explicit handling
of read latency, newly arriving higher-priority entries, and downstream stalls.
Prefetched entries must remain part of capacity/reservation accounting until
consumed. For routers, destination/priority information must be available for
routing and arbitration at the intended cycle, so some flit header fields may
also need to stay outside the SRAM.

The separate valid/priority arrays contain only 160 bits for a 32-entry send
queue, 40 bits for an eight-entry router queue, and 20 bits for a four-entry
core ingress queue, excluding counts. They should remain registers. The 32-entry
send payload is a plausible small-memory candidate; the four/eight-entry cases
are much less compelling despite their aggregate replication.

The broadcast FIFO is 588 bits wide because it stores a 512-bit line, a 12-bit
tag, and two 32-bit carried values. A core consumes only tag bits 3:0, so unused
tag bits can disappear during synthesis. Similarly the core ingress `src` field
is not consumed by the current BIU. The declared widths in the table are not a
claim that all these bits survive optimization.

## Memory-interface buffers

The current DRAMIf `buffer` is a 256 × 512 register vector with allocation and
response writes. It has selected reads for request issue, unicast response
ejection, and broadcast response ejection. A direct replacement therefore has
up to two writes and three logical read paths. Responses are `Valid`, with no
ready/backpressure signal, so a wrapper cannot simply reject a conflicting
response write.

A useful redesign to study separates 32-bit scalar store data from 512-bit
response data. Issue can replicate the stored scalar word onto the bus, leaving
the wide memory with one response writer and unicast/broadcast readers. Those
two response classes can be consumed concurrently. Arbitration, replication,
or a suitable multiport macro still has a real cost; a generic one-port SRAM
does not preserve this behavior automatically.

MSHRs contain operation, source, ID, address, size, and carried data: 133 bits
per slot as declared. Keep allocated/issued/fulfilled and the information needed
to form every slot's broadcast/unicast eligibility in registers. Selected
address/return/carried fields may be candidates for separate arrays, but their
issue and response readers need a port/timing plan. Current scalar and bulk
requests share the slot allocator; do not infer independent banks from old
descriptions of an ID-bit partition.

The 32-entry bulk descriptor FIFO is a modest secondary candidate. The one
peripheral interface has a similarly complicated data buffer but only 1 KiB,
so it has little system-level leverage compared with the replicated core
memories. Two-entry ring and broadcast FIFOs and single-cycle distributor
registers should retain their explicit timing boundaries.

## Storage to retain as registers

Each SMT register file has 31 physical 32-bit registers; x0 is constant. Its
two arbitrary read ports, normal write port, bypasses, and four fixed a0–a3
read/write connections make a generic SRAM inappropriate. The direct writes
have priority over the ordinary write to those registers. A specialized
register-file macro or a split with a0–a3 in flops would be a separate pipeline
design exercise for only 248 bytes per PU.

Handler masks, margins, quotas, queue status, reservation addresses, PCs,
micro-op holding registers, and execution pipeline state either participate
in parallel decisions or have too few rows to justify ordinary SRAM. The
configuration ROM is a small elaborated constant table. The simulation boot
ROM is provided by the simulator peripheral, not a memory instantiated inside
the synthesized Core.

## ASAP7 library findings and estimation boundaries

The Synopsys enablement directory is `~/asap7/asap7_snps`, as specified in
TAPEOUT.md. The installed standard-cell libraries
include the five RVT TT NLDM groups from `asap7sc7p5t_28`.

Their Liberty units are **1 ps, 1 fF, 1 pW leakage**. The cell
`DFFHQNx1_ASAP7_75t_R` has Liberty area 0.2916 and its explicitly 1× LEF has
size 1.08 × 0.27 µm, also 0.2916 µm². Thus these standard-cell Liberty areas
already match 1× geometry: do not divide DC's corresponding area by 16 merely
because a separate physical-design flow uses 4× geometry. A 1 GHz clock is
1000 of this library's time units, not 1.

The installed `asap7_sram_0p0/generated` has 36 Verilog/Liberty/LEF model sets.
Their addressable depths are **256, 512, and 1024**, and widths are
16, 18, 20, 32, 34, 36, 40, 48, 64, 72, 74, and 80 bits. The `64x4` name means
256 addressable words; it is not a 64-deep memory. All inspected models expose
one common address, read/write enables, and no byte-write mask. All 36 Liberty
files have **zero cell area and zero cell leakage power**. They do contain
timing/internal-power tables, whose applicability and provenance still need
validation; zero leakage is not a credible physical prediction.

The 256 × 64 example has LEF dimensions 9.612 × 77.76. This records the raw
installed view, not a validated 7 nm area conversion. SRAM geometry scaling
must be established independently before adding it to standard-cell area.

There is an upstream improvement to investigate before macro integration:
the [SRAM repository changelog](https://github.com/The-OpenROAD-Project/asap7_sram_0p0)
records an October 2025 update copying LEF area into Liberty, alongside output
port unit and timing-constraint fixes. The current
[256 × 64 Liberty](https://raw.githubusercontent.com/The-OpenROAD-Project/asap7_sram_0p0/main/generated/LIB/srambank_64x4x64_6t122.lib)
reports area 747.42912, matching the installed raw LEF footprint, but still
reports zero leakage. These upstream changes are not installed or used in the
Core baseline. Updating a model's area alone does not validate its power tables,
physical scaling, or suitability for the required ports.

The minimum depth is a major practical constraint:

| Desired memory | Minimum installed depth / desired depth | Consequence |
| --- | ---: | --- |
| Scratchpad, depth 256 | 1× | Depth fits; byte masks are missing |
| Send/bulk FIFO, depth 32 | 8× | At most 12.5% depth utilization |
| Event FIFO, depth 16 | 16× | At most 6.25% depth utilization |
| Core broadcast/router FIFO, depth 8 | 32× | At most 3.125% depth utilization |
| Core ingress FIFO, depth 4 | 64× | At most 1.5625% depth utilization |
| Response FIFO, depth 2 | 128× | At most 0.78125% depth utilization |

Width padding, extra ports, output staging, and peripheral overhead make these
utilizations still less favorable. Therefore “replace every queue with an
available macro” is not a sensible starting point. A shallow SRAM/register-file
compiler could change the ranking; the current supplied shapes do not establish
that small queue replacements save area or power.

[Hammer's ASAP7 documentation](https://docs.hammer-eda.org/en/latest/Technology/ASAP7.html)
describes its separate dummy SRAMs as 4× physical views with blank obstructed
layouts that are not DRC/LVS-clean. Its documented Calibre requirement concerns
physical verification; it does not itself prevent DC synthesis. Those dummy
models should not be confused with a characterized memory compiler or silently
substituted for the installed `srambank` models.

## Shared ICache accounting

Keep ICache outside the replacement inventory. For the requested future shared
ICache estimate, add **one 16 KiB scratchpad-equivalent per 16-PU cluster**:
32 equivalents, or 512 KiB of proxy capacity for the whole target. Do not add
one per PU, and do not subtract it from the 512 actual scratchpads.

This is an explicit area/power proxy, not an implemented shared ICache. Report
the one-line ICache residual retained in the Core baseline so it is not hidden
when adding the proxy. A trace from this deliberately tiny ICache will have
different misses and memory traffic from the planned shared cache. Scratchpad
area may be used as requested, but power still needs stated read/write activity;
cache frequency, sharing, tags, and interconnect are not inferred from capacity.

## Reproduction and status

Detailed arithmetic and server SRAM metadata are recorded in
`copilot/tmp/ppa/storage-inventory.json` and
`copilot/tmp/ppa/asap7-memory-models.json`, with their generating scripts nearby.
They are intermediate evidence, not replacement source RTL.

Library compilation now succeeds with LC Y-2026.03-SP2 installed at
`/opt/synopsys/lc/Y-2026.03-SP2`. The installer post-step failed because
`/bin/csh` resolves to BSD csh and rejects the long shell variable
`read_lib_error_if_no_license_checkout`. An isolated copy of the vendor script
renames only that local variable; its command-line options, sentinel filenames,
and the installer's original No/No settings are preserved. Running the copy
completed the post-step. The installed vendor script was not modified.

Set `SYNOPSYS_LC_ROOT=/opt/synopsys/lc/Y-2026.03-SP2` in the synthesis process
environment. DC's installed setup explicitly recognizes this location. This
resolved the original LCSH-3 failure, and all five ASAP7 RVT TT standard-cell
libraries (AO, OA, SIMPLE, INVBUF, SEQ) were read and written to `.db` successfully.
The INVBUF and SEQ internal library names differ from their filenames; the
conversion script uses the Liberty declaration for `write_lib`.

The HDL license blocker is resolved: after the user added
`26586@synopsys11p.elic.intel.com`, the complete Core passed SystemVerilog
analysis, elaboration, and linking under DC Y-2026.03-SP2. Native SCL also
advertises and authorizes `HDL-Compiler` at version 2026.03 on that endpoint.
The entitlement investigation is preserved in
`copilot/tmp/ppa/license-entitlements.md` and its linked JSON/CSV/logs.
Technology mapping and power reporting completed successfully. Results, mapped
outputs, limitations, and provenance are documented in
[the Core baseline](core-ppa-baseline.md) and `copilot/tmp/ppa/core-baseline/`.

Inputs and scripts are staged at
`/home/meow/copilot/tmp/ActiveN-core-1line-20260910-9TNH8D` on the designated
server. Local evidence in `copilot/tmp/ppa` includes the original
`compile-libraries.log` and `dc-core.log`, the successful conversion archive
`compiled-asap7-rvt-tt.tar.gz`, and the scripts `repair_lc_postinstall.py`,
`compile_libs.tcl`, and `dc_core.tcl`. The completed mapped run uses a 1000 ps clock,
50 ps uncertainty, 100 ps input/output delays, 20 ps input/clock transitions,
and 2 fF output loads, with generic hartid inputs and a stated vectorless input
activity of 0.1 transitions per clock. These are baseline modeling assumptions,
not a workload-derived or post-route power result.

Local core and 64-PU/2-MC system elaboration passed. Both generated JSON files
confirm one ICache line, one way, and a 64-byte block. A focused Verilator test
passed 1,089 fetches, 773 refills, and 18,107 cycles, checking same-line hits,
single-line eviction, adjacent lines differing in address bit 6, high address
bits, refill backpressure/latency, and reset invalidation. The simulator smoke
test `asm/icache_test.bin` also passed on the rebuilt single-Core simulator:
result 0 in 2,210 cycles. The rebuilt 64-PU/2-MC hard-system simulator passed
`sys/barrier_test.bin` with result 0 in 4,953 cycles and an arrival count of
64 (printed as hexadecimal `40`). This system shape is a regression test only;
the inventory and eventual PPA target remain 512 PU / 16 MC.
