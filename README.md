# ActiveN

*ActiveN* is a RISC-V-based many-core neuromorphic processor. This repository contains the RTL of the core, as well as data generator and simulator used in our paper.

To elaborate the RTL code:

```bash
# Edit parameters
vim src/main/scala/koneko/Main.scala

# Install dependencies
nix develop -i

# Generate Core.sv
mill Koneko.run
```

## Usage

The elaborated RTL code is synthesizable. To use the built-in simulation framework (inclucded in `./sim`), run `src/build.sh`. This will generate a full-system simulator that's ready to run.

The base ISA is the standard RV32I ISA, with optional floating point support (F with Zfinx). The common practice for building free-standing RISC-V binaries is used for compile software for *ActiveN*.

Custom instructions is included for:

- Active message support
- Fixed-point arithmetic support

Check `sim/payloads` for examples of softwares.
