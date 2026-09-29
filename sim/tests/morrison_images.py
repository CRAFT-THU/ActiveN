#!/usr/bin/env python3
"""Deterministic compact-runtime functional cases, explicitly not benchmarks."""
import argparse
import json
from pathlib import Path
import struct
import sys
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "datagen"))
from morrison import write_images
from morrison_graph import fresh_graph, sha256
from morrison_model import Spec, F, constants
from morrison_validate import check_images


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", choices=("mixed", "occupancy", "full"), required=True)
    parser.add_argument("--group", type=int, choices=(1, 4), required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    # Occupancy puts 176 E + 44 I on every PU and fills all 880 G4 job slots.
    # Mixed includes long, partial rows and a mix of quiet and simultaneous pairs.
    spec = (Spec() if args.case == "full" else
            Spec(ne=11264, ni=2816, ce=1, ci=1, pu=64, mc=2) if args.case == "occupancy"
            else Spec(ne=256, ni=64, ce=129, ci=8, pu=64, mc=2))
    graph = fresh_graph(args.output / "graph", spec, 92713)
    f = graph.state.view(F)
    c = constants()
    f[:, 0] = F(21.)
    if args.case == "mixed":
        f[1::2, 0] = F(0.)
        # Refractory byte and fire byte must not alias; these neurons are quiet.
        graph.state[3::7, 7] = 3
    elif args.case == "full":
        # Exercise full fan-in/out, all MCs and native Poisson drive with two
        # known initial spikes. This cold diagnostic is never a benchmark.
        f[:, 0] = F(0.)
        f[[0, spec.ne], 0] = F(21.)
    f[:spec.ne, 3] = F(c["pot"] * F(.5))
    f[:spec.ne, 4] = F(c["dep"] * F(.75))
    graph.provenance["fixture"] = args.case
    graph.provenance["physical_state"] = (
        "controlled functional fixture: zero currents and pending inputs; "
        "E trace coefficients X=pot*0.5, Y=dep*0.75; "
        + {"mixed": "alternating V=21/0 mV, refractory=3 for GIDs 3::7",
           "occupancy": "all V=21 mV, refractory=0",
           "full": "V=0 except E0/I0 at 21 mV, refractory=0"}[args.case])
    manifest = write_images(graph, args.output, ROOT / f"sim/payloads/sys/morrison_g{args.group}.bin",
                            group=args.group, functional=True, warmup=0,
                            timed=7 if args.case == "mixed" else 2,
                            seed=19260817, external=args.case == "full")
    # Seed cumulative-style counters near wrap. Phase 1 must reset all four
    # before any new delivery, on tree leaves as well as the root.
    image = args.output / "dram.0"
    with image.open("r+b") as stream:
        header = struct.unpack("<4I", stream.read(16))
        for pu in range(spec.pu):
            stream.seek(header[1] + 8*pu)
            offset, size = struct.unpack("<2I", stream.read(8))
            base = header[1] + offset
            for field in (64, 68, 104, 108):
                stream.seek(base+field)
                stream.write(struct.pack("<I", 0xFFFFFFF0))
    manifest["image_sha256"][0] = sha256(image)
    manifest["fixture"] = dict(name=args.case, external_drive=args.case == "full", counter_seed=0xFFFFFFF0,
                                measured_performance=False)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    print(json.dumps(check_images(args.output), indent=2), flush=True)


if __name__ == "__main__":
    main()
