This is a many-core neuromorphic system implemented with the Chisel HDL, based on a RV32I base ISA. It has some modification, including the sharing of FP and Int registers, and a custom rf-to-rf inter-core communication system / instruction. Some documentation is located in `doc/`.

Your task involves reviewing changes to the RTL, implementing the simulator frontend, porting and updating test cases, debugging and identifying issues, and occasionally make minor modifications to the RTL.

The project can be roughly divided into two levels: the implementation of individual cores, and the implementation of the system-level interconnect.

Correspondingly, there are two types of simulators: a core-level simulator and a system-level simulator. A core-level simulator simulates a single core, while a system-level simulator simulates the entire system. There are two backends for the system-level simulator: a hardware NoC backend that's fully elaborated from the system-level RTL, and a software NoC backend that uses RTL for simulating individual cores, but uses a software NoC implementation for inter-core communication. Right now, the software mode is unaligned and contains bug.

There are also two types of test cases: single-core tests, which focus on verifying computation and memory access capabilities of individual cores, and system-level tests, which verifys the inter-core communication and parallel execution scheduling. You can find the tests inside the `tests/` directory. Some of the system-level tests requires running the data generator located in `datagen/`, which generates the DRAM image for SNN-like applications.

Previous sessions' transcripts are located inside `copilot/`. `copilot/chat.json` is the full transcript. The most distant instructions are located in `copilot/instruction.md`. Other files are the outputs of individual tasks.

You are inside a NixOS container, with root privileges. You're free to install any packages with nix-env, and run any commands you like. Please place your temporary files at `copilot/tmp`. This includes traces, logs, generated scripts and other intermediate artifacts, but do not include artifacts with well-defined destinations (SystemVerilog should be in generated/, built workloads in the same folder of the workloads, simulators in sim/build, etc.).

Don't commit any code. User will commit the code after you finish.

Note that you're encourged to ask user questions if you're unclear about anything. Be really careful about changing logic in the codebase. Ask for explicit permission before making significant changes. Crucially, don't make assumptions about the codebase without getting clarifications. Refer to the documentation, since some code might be buggy and does not reflect the design intention.
