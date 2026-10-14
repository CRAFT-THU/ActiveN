# Optional simulator ICache initialization

Both simulators accept `--init-icache[=ADDR]`. With no address, the start is
`0x80000000`. The simulator inserts exactly one contiguous region of
`iCacheLines * iCacheBlockSize` bytes into every PU's instruction cache. The
geometry comes from the model's elaborated configuration. The workload targets
used for the retained results have 128 lines of 64 bytes and preload 8 KiB;
the default 32-line elaboration preloads 2 KiB. Inspect the selected generated
configuration instead of assuming either size. Preload does not replace
workload warmup; record the address and region in each measurement.

```sh
nix develop --command bash -c 'cd /root/workspace/ActiveN && \
  sim/build/sim_single --init-icache sim/payloads/asm/icache_test.bin'

# Address override, also accepting --init-icache 0x60000000
nix develop --command bash -c 'cd /root/workspace/ActiveN && \
  sim/build/sim_single --init-icache=0x60000000 sim/payloads/asm/icache_test.bin'
```

The address must be cache-line aligned and the region must not wrap the
32-bit address space. Supported sources are mapped global memory and the boot
ROM window, including `--bootrom` overrides. Bytes beyond an image's end are
zero-filled within its mapped memory. MMIO and scratchpad ranges are rejected.
No external conversion step, initialization file, or simulated memory request
is required. Without the flag, caches start cold as before.

The default region excludes the boot ROM at INITVEC (`0x60000000`). Boot-ROM
fetches therefore still miss and may replace preloaded lines. Initialization
visits later addresses first so the beginning of the region is most recently
used. Normal replacement and refill behavior remains active: code outside the
region, and any subsequently evicted lines, still cause misses.

## DPI implementation

`sim/icache_dpi.sv` binds a simulation-only `ICacheDpi` helper into each ICache.
It directly accesses the CIRCT-generated `data_ext.Memory`,
`metadata_ext.Memory`, and `plruState_ext.Memory` arrays. It is included by
CMake's simulator targets, not by the synthesis filelist, and adds no hardware
memory port or reset state machine. Changes to CIRCT memory lowering may
require updating these hierarchical references and packed layouts.

The exported `insertLine(address, way, lineData)` task accepts one packed
cache line, with the lowest-address word in bits 31:0. It writes all data
words, publishes the tag and valid bit, and updates tree-PLRU. Invalid alignment,
out-of-range ways, and duplicate tags in another way are rejected.
`icacheInitReady()` permits calls only after the metadata reset sweep finishes
and before any fetch has been accepted since reset.

The driver selects each cache's DPI scope, inserts lines after `step()` with
all workers quiescent, and calls `eval()` once per affected model to settle
combinational logic. This consumes no additional target clock edges. In cosim,
both backends receive the same backing-image bytes at this boundary. Reasserting
reset invalidates inserted lines normally; the CLI preload is a one-time
startup operation.

At exit, `[ICache ...]` reports accepted refill beat requests, the subset
targeting global memory (`global_refill_requests`), and completed line fills.
These counters exclude DPI insertion and include startup. They do not count
ordinary cache hits as memory traffic. System counters are summed over PUs,
with one report per backend. The existing soft timer-interval statistics can
separately measure instruction-cache fills during benchmark execution.
