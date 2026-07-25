import ramulator


class ActiveNHBM1(ramulator.dram.HBM1):
    """HBM1 geometry adjusted for ActiveN's 64-byte memory interface."""

    data_payload_bytes = 64
    command_cycles = {
        **ramulator.dram.HBM1.command_cycles,
        "RD": 2,
        "WR": 2,
        "RDA": 2,
        "WRA": 2,
    }


def make_controller():
    dram = ActiveNHBM1(
        org_preset="HBM1_4Gb",
        timing_preset="HBM1_2Gbps",
        bank=4,
        row=32768,
        column=64,
        nBL=2,
        nRCDWR=14,
        nRTPL=6,
        nFAW=30,
        nRFC=260,
        nREFIpb=128,
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
    controllers=[make_controller() for _ in range(8)],
    channel_mapper=ramulator.channel_mapper.CacheLineInterleave(),
)
simulation = ramulator.Simulation(frontend, memory)
