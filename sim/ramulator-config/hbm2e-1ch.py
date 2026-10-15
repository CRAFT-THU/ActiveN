"""One 1-GiB channel of the ActiveN 1.6-GHz HBM2-derived extension model.

Sixteen independent channels represent two eight-channel, 8-GiB stacks. The
model retains HBM2_4Gb geometry and the physical latency assumptions of the
pinned HBM2_2000Mbps model; it is not a calibrated manufacturer speed bin.
Primary physical delays are rounded up at 625 ps. nRC also covers the sum of
the separately rounded nRAS and nRP. Burst, CAS-spacing and command lengths
retain the pinned model's fixed cycle counts, including its +2 CK turnaround.

Every timing is explicit: the pinned Ramulator resolves secondary timings
BEFORE applying keyword overrides, so overriding tCK alone would leave stale
activation/refresh constraints. All times below are memory CK except rate
(Mbps/pin) and tCK_ps (picoseconds). The exported constraint table is regenerated
by Ramulator from these values, including its multi-cycle command offsets.
"""

import ramulator


TIMINGS = {
    "rate": 3200,
    "tCK_ps": 625,
    "nBL": 2,
    "nCL": 23,       # 14 ns
    "nRCDRD": 23,    # 14 ns
    "nRCDWR": 20,    # 12 ns
    "nRP": 23,       # 14 ns
    "nRAS": 55,      # 34 ns
    "nRC": 78,       # max(ceil(48 ns / tCK), nRAS + nRP)
    "nWR": 26,       # 16 ns
    "nRTPL": 8,      # 5 ns
    "nCWL": 8,       # 5 ns
    "nCCDS": 2,
    "nCCDL": 4,
    "nRRDS": 7,      # max(4 CK, 4 ns)
    "nRRDL": 7,      # max(4 CK, 4 ns)
    "nWTRS": 10,     # 6 ns
    "nWTRL": 13,     # 8 ns
    "nFAW": 24,      # max(8 CK, 15 ns); inherited model assumption
    "nRFC": 416,     # 260 ns at the retained density
    "nRFCpb": 256,   # 160 ns
    "nRREFD": 13,    # max(4 CK, 8 ns)
    "nREFI": 6240,   # maximum interval 3900 ns, exactly representable
    "nREFIpb": 195,  # 3900 ns / 32 banks, exactly representable
}


def make_controller():
    dram = ramulator.dram.HBM2(
        org_preset="HBM2_4Gb",
        timing_preset="HBM2_2000Mbps",
        **TIMINGS,
    )
    return ramulator.controller.HBM12(
        dram=dram,
        scheduler=ramulator.scheduler.FRFCFS(),
        refresh_manager=ramulator.refresh_manager.AllBank(),
        row_policy=ramulator.row_policy.Open(),
        addr_mapper=ramulator.addr_mapper.RoBaRaCoCh(),
    )


frontend = ramulator.frontend.External(clock_ratio=1)
memory = ramulator.memory_system.GenericDRAM(
    clock_ratio=1,
    controllers=[make_controller()],
    channel_mapper=ramulator.channel_mapper.CacheLineInterleave(),
)
simulation = ramulator.Simulation(frontend, memory)
