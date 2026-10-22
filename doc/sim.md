# Simulation guideline

This document gives the general guideline for performing a simulation.

Core level tests can be perform using a core-level simulator.

System-level tests should use 64PU + 2MC configuration. For these tests, disable icache init, enable cosim, which guarantees alignment of hardware & software models. This means that you need to build the simulator with both hard and soft backend. Always build with BOLT enabled.

The benchmark environment should use 512PU + 16MC. For these configurations, you should build a software-backend only variant, which only requires elaborating a single PU's RTL (no routers and memifs requirired). Also build with BOLT enabled, and although it will use a 64PU 2MC binary for BOLT PGO, a software-backend-only simulator can use any PU / MC configuration. When running benchmarks, 

Further configuration of the benchmark environment:
- Assume 1.5GHz frequency
- Always use Ramulator with the [HBM3 configuration](#hbm3-memory-configuration): one 16-GiB stack with sixteen channels (1 GiB per MC).
- Always enable icache init
- Always use 1 timestep as warmup. If you think the testing contract does not allow a warmup period, notify the user.
- Remember to use the timer in the peripheral interface.

Sometimes, for experimental works (e.g. testing the feasibility of an ongoing RTL work), the software backend and hardware system implementation can partially diverges. However, during all these conditions, the user have to explicitly told you to allow this. Otherwise, flag the divergence of software and RTL to the user. And in all these cases, the core interface / ISA should be backward-compatible with the test being run.

## HBM3 memory configuration

Use [sim/ramulator-config/hbm3-1ch.py](../sim/ramulator-config/hbm3-1ch.py).
It selects the pinned Ramulator `HBM3_16Gb_8hi` organization,
`HBM3_6400Mbps` timing preset and native `HBM34` controller. Each MC instantiates
one channel; 16 MCs provide 16 GiB total. The 32-bit pseudochannels and eight-beat
bursts give 32-byte transactions, matching the benchmark interface without a
bus-width converter.

`HBM3_6400Mbps` means 6.4 Gbit/s per data pin, with 3.2-GHz data strobes and a
1.6-GHz command clock. The preset supplies the memory clock and timing
constraints. `--freq-ghz` continues to set the core/NoC frequency.

Since the command clock is exactly twice the speed of the data strobes, we used twice the interface width in our simulator frontend so that the bandwidth matches. In the eyes of the backend, it's operating on a 8B-wide bus with 1.6GHz frequency, just like a HBM2e physical channel. This is intentional, and should guarantee relatively accurate HBM3 benchmark result.

## Build directories and benchmarking simulator binary consistency

By default, the Scala is built into /out, verilog generated in /generated, and simulators built inside sim/build. ALL THESE DIRECTORIES ARE VOLATILE and may be overwritten by later works.

You are allowed to use these artifacts (especially the ones in sim/build) ONLY FOR TESTING uncommited changes inside the RTL and for doing software-backend cosim alignment works (basically, works that can resulting in new committed code in both RTL and simulaltor behavior), or explicitly approved by the user.

For benchmarking, if there is RTL / simulator source update after the latest saved simulator binary, you should:
- First, make sure that the RTL and simulator source directories is clean: No uncommitted changes.
- Then, regenerate Core.sv, build the simulator in a temporary directory (i.e. from a clean state).
- Finally, copy the built simulator into benchmark/simulator/, named sim_{commit_hash}. commit_hash is the last commit that changed the simulator source and/or the RTL. You can now delete the temporary build directory.
- Use that simulator in your scripts. Those simulators will never be deleted until the project is completed. In all of the reports, add the sha256 hash of the simulator binary.

We recommend doing similar archiving for workloads inside benchmark/workload. The commit hash for these archive workloads should be the last commit that moddified the source code of the workloads.

Benchmark runtime directories should be in benchmark/rundir, reports should be in benchmark/report.
