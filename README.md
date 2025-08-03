# ActiveN

*ActiveN* is a RISC-V-based many-core neuromorphic processor. This repository contains the RTL of the core, as well as data generator and simulator used in our paper [_ActiveN: A Scalable and Flexibly-Programmable Event-Driven Neuromorphic Processor_](https://doi.org/10.1109/MICRO61859.2024.00085).

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

## Cite us!

Please cite our work with:

```bibtex
@inproceedings{ActiveN,
  author={Liu, Xiaoyi and Pu, Zhongzhu and Qu, Peng and Zheng, Weimin and Zhang, Youhui},
  booktitle={2024 57th IEEE/ACM International Symposium on Microarchitecture (MICRO)},
  title={ActiveN: A Scalable and Flexibly-Programmable Event-Driven Neuromorphic Processor},
  year={2024},
  pages={1122-1137},
  keywords={Neuromorphics;Scalability;Computational modeling;Random access memory;Prototypes;Spiking neural networks;Programming;System-on-chip;Synapses;Testing;spiking neural networks;many-core architecture;active message},
  doi={10.1109/MICRO61859.2024.00085}
}
```
