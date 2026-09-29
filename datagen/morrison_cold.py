#!/usr/bin/env python3
"""Reuse a fresh Morrison graph/image with normal time-zero neuron state.

Only MC0 contains SPM initialization. Copy it and share the other immutable
input images; simulator output must go to a separate directory. This does not
produce a checkpoint or convert settled weights back into initial weights.
"""
import argparse
import copy
import json
from pathlib import Path
import shutil
import struct
import numpy as np

from morrison_graph import sha256
from morrison_model import NODE_BASE, cold_state
from morrison_validate import Image


def prepare(source, output, seed, warmup=60, timed=1000):
    image = Image(source)
    manifest = copy.deepcopy(image.manifest)
    image.spec.validate(False)
    if manifest["source"]["kind"] != "fresh" or not manifest["external_drive"]:
        raise ValueError("cold initialization requires fresh weights and native external drive")
    if warmup < 0 or timed < 1 or seed < 1:
        raise ValueError("invalid timestep window or seed")
    source, output = Path(source).resolve(), Path(output)
    output.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(source/"dram.0", output/"dram.0")
    for mc in range(1, image.spec.mc):
        (output/f"dram.{mc}").symlink_to(source/f"dram.{mc}")
    (output/"graph").symlink_to(source/"graph", target_is_directory=True)
    shutil.copyfile(source/"plastic-outdegree.npy", output/"plastic-outdegree.npy")
    state = cold_state(image.spec, seed)
    rng = np.random.default_rng(seed)
    workers = rng.integers(1, 2**32, size=(image.spec.pu, 2), dtype=np.uint32)
    with (output/"dram.0").open("r+b") as stream:
        section = int(image.read(0, 4, 1)[0])
        for pu in range(image.spec.pu):
            offset = int(image.read(0, section+pu*8, 2)[0])
            base = section+offset
            stream.seek(base+32)
            stream.write(struct.pack("<2I", warmup, warmup+timed))
            for field in (64, 68, 104, 108):
                stream.seek(base+field)
                stream.write(bytes(4))
            stream.seek(base+43*4)
            stream.write(workers[pu].tobytes())
            stream.seek(base+NODE_BASE)
            stream.write(state[pu::image.spec.pu].tobytes())
    np.save(output/"neurons.npy", state)
    manifest.pop("fixture", None)
    manifest["source"].pop("fixture", None)
    manifest["source"].update(physical_state="time zero: normal voltage (5.7, 7.2) mV, zero currents, inputs and learning traces",
                              state_seed=seed, reused_image_manifest_sha256=sha256(source/"manifest.json"))
    manifest.update(functional=False, warmup=warmup, timed=timed, seed=seed,
                    benchmark_ready=False, neuron_sha256=sha256(output/"neurons.npy"))
    manifest["image_sha256"][0] = sha256(output/"dram.0")
    (output/"manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--seed", type=int, default=20260724)
    parser.add_argument("--warmup", type=int, default=60)
    parser.add_argument("--timed", type=int, default=1000)
    args = parser.parse_args()
    manifest = prepare(args.source, args.output, args.seed, args.warmup, args.timed)
    print(json.dumps({key: manifest[key] for key in ("spec", "source", "warmup", "timed", "image_bytes")}, indent=2))


if __name__ == "__main__":
    main()
