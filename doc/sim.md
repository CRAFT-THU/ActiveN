# Simulation guideline

This document gives the general guideline for performing a simulation.

Core level tests can be perform using a core-level simulator.

System-level tests should use 64PU + 2MC configuration. For these tests, disable icache init, enable cosim, which guarantees alignment of hardware & software models. This means that you need to build the simulator with both hard and soft backend. Always build with BOLT enabled.

The benchmark environment should use 512PU + 16MC. For these configurations, you should build a software-backend only variant, which only requires elaborating a single PU's RTL (no routers and memifs requirired). Also build with BOLT enabled, and although it will use a 64PU 2MC binary for BOLT PGO, a software-backend-only simulator can use any PU / MC configuration. When running benchmarks, 

Further configuration of the benchmark environment:
- Assume 1.5GHz frequency
- Always use ramulator. Select 2 x HBM2e (1.6GHz frequency)
- Always enable icache init
- Always use 1 timestep as warmup. If you think the testing contract does not allow a warmup period, notify the user.
- Remember to use the timer in the peripheral interface.

Sometimes, for experimental works (e.g. testing the feasibility of an ongoing RTL work), the software backend and hardware system implementation can partially diverges. However, during all these conditions, the user have to explicitly told you to allow this. Otherwise, flag the divergence of software and RTL to the user. And in all these cases, the core interface / ISA should be backward-compatible with the test being run.

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
