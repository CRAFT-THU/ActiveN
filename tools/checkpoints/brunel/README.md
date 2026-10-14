# Brunel GeNN checkpoint tools

`settled.py` and `ports/` were extracted from the frozen source used for the
retained settled Brunel checkpoints. The original bytes remain under
`archive/tmp/brunel-settled-20260927/gpu-source-v1/brunel/`.
They implement probe, settle and resume operations without depending on an
archive import or a hardcoded ActiveN workspace path.

Run `python3 tools/checkpoints/brunel/settled.py --help` in the reference GPU
environment. It requires NumPy, SciPy, psutil, PyGeNN and CUDA (the checkpoint
adapter loads `libcudart.so.12` and GeNN's generated runner). The project Nix
shell alone does not supply this complete GPU environment. The frozen port's
missing-PyGeNN error mentions its old environment wrapper; that wrapper is
historical and is not a current repository command.

The tool takes a required fresh `--output` and explicit `--rule`; resume also
uses `--checkpoint`. Preserve graph/state hashes and phase boundaries when
continuing. Use new output paths under `work/`. Model equations, seeds and
historical defaults were preserved; this extraction does not establish GPU
compatibility or a new testing standard. No GPU execution was performed during
cleanup. See [the Brunel guide](../../../doc/brunel.md) for image import.
