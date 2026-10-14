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
