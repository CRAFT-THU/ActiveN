"""One 1-GiB HBM3 channel for the preserved 32-byte ActiveN interface.

Upstream: CMU-SAFARI/ramulator2 at
99a0e1e87a9321587492fef5b0bd6197928f8d68,
python/ramulator/dram/hbm3.py and controller/hbm34.py.

Sixteen instances give the existing 16-MC/16-GiB capacity. The 32-bit
pseudo-channel and eight-beat burst transfer 32 bytes, matching ActiveN's
transaction size. HBM3_6400Mbps denotes 3.2-GHz data strobes and a 1.6-GHz
command clock. The pinned exporter uses half-command-clock ticks and rounds
625/2 ps down to 312 ps (about 0.16% faster than ideal 312.5-ps ticks).

Use the native HBM34 controller and timing preset together; changing only
the clock or channel_width would change the modeled protocol or bandwidth.
The archived frontend applies byte-enabled writes in its data image but
does not model the read-modify-write cost required for partial HBM3 bursts
unless a controller can supply the unchanged bytes from a valid cached line.
This configuration alone does not add that controller behavior.
"""

import ramulator


def make_controller():
    dram = ramulator.dram.HBM3(
        org_preset="HBM3_16Gb_8hi",
        timing_preset="HBM3_6400Mbps",
    )
    return ramulator.controller.HBM34(
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
