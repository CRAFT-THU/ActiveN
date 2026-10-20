"""Datagen views of raw baseline checkpoints and temporary validation bundles.

Input dimensions, structural masks, native state and units remain explicit.
This is a data import API, not an assertion that a legacy payload implements
the imported model. Image emitters must qualify their own state conversion.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from pathlib import Path

from baseline_checkpoints import (NATIVE_CLASSIFICATION, NATIVE_MORRISON, classification_arrays_view,
                                  inspect_checkpoint, read_json, write_json)
from baseline_windows import load_arrays, load_window


@contextmanager
def imported_state(path: Path, *, verify=True):
    path = Path(path).resolve(strict=True)
    if path.is_dir() and (path / "window.json").exists():
        meta, state, graph = load_window(path, verify=verify)
        physical = dict(state=state, graph=graph, metadata=meta, converted=False)
        if meta["workload"] in ("mnist", "fashion-mnist", "cifar10"):
            physical["classification"] = classification_arrays_view(meta["ancestor_metadata"], {**state, **graph})
        yield physical
        return
    cp = inspect_checkpoint(path)
    with cp.arrays(verify=verify) as source:
        physical = dict(state=source, graph=None, metadata=cp.metadata, converted=False)
        if cp.format == NATIVE_CLASSIFICATION:
            physical["classification"] = classification_arrays_view(cp.metadata, source)
        elif cp.format == NATIVE_MORRISON:
            graph_dir = (cp.path / cp.metadata["graph"]).resolve(strict=True)
            records = read_json(graph_dir / "manifest.json")["arrays"]
            physical["graph"] = load_arrays(graph_dir, {k:dict(file=k + ".npy", **v) for k,v in records.items()}, verify=verify)
        yield physical


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("checkpoint", type=Path)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args(argv)
    with imported_state(args.checkpoint) as data:
        arrays = data["state"]
        names = arrays.files if hasattr(arrays, "files") else arrays
        result = dict(source=str(args.checkpoint.resolve()), converted=False,
                      arrays={k:dict(dtype=arrays[k].dtype.str, shape=list(arrays[k].shape)) for k in names})
        if "classification" in data:
            c = data["classification"]
            result["classification"] = dict(inputs=c["weights"].shape[0], exc=c["weights"].shape[1],
                    structural_edges=int(c["structural_mask"].sum()),
                    present_zero_weights=int(((c["weights"] == 0) & (c["structural_mask"] != 0)).sum()))
        write_json(args.output, result)


if __name__ == "__main__":
    main()
