import ramulator


def make_controller():
    dram = ramulator.dram.HBM2(
        org_preset="HBM2_4Gb",
        timing_preset="HBM2_2000Mbps",
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
