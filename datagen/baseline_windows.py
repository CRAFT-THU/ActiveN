"""Versioned native window bundles and population-count validation.

These bundles preserve full GeNN state, including pending events. They are not
the reduced legacy ActiveN image ABI. Readers can mmap recurrent state arrays.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import math
import shutil
from pathlib import Path

import numpy as np

from baseline_checkpoints import read_json, sha256, write_json

SCHEMA = "activen-baseline-window-v1"
BLOCK = 8 << 20


class Transfer:
    """Bounded CUDA transfer; no second host-sized copy of large arrays."""
    def __init__(self):
        import os
        self.cuda = ctypes.CDLL(os.environ.get("STDP_CUDART", "libcudart.so.12"))
        self.cuda.cudaMemcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
        self.cuda.cudaMemcpy.restype = ctypes.c_int
        self.cuda.cudaDeviceSynchronize.restype = ctypes.c_int
        self.sync()

    def sync(self):
        code = self.cuda.cudaDeviceSynchronize()
        if code:
            raise RuntimeError(f"CUDA synchronize failed: {code}")

    def copy(self, device, host: np.ndarray, *, to_device=False):
        raw = host.view(np.uint8).reshape(-1)
        pointer = int(device._device_pointer)
        for offset in range(0, raw.size, BLOCK):
            count = min(BLOCK, raw.size - offset)
            src, dst = raw.ctypes.data + offset, pointer + offset
            if not to_device:
                src, dst = dst, src
            code = self.cuda.cudaMemcpy(dst, src, count, 1 if to_device else 2)
            if code:
                raise RuntimeError(f"CUDA state transfer failed: {code}")


def save_arrays(directory: Path, registry: dict, transfer: Transfer) -> dict:
    directory.mkdir(parents=True, exist_ok=False)
    required = sum(np.dtype(dtype).itemsize * math.prod(shape) for _, dtype, shape in registry.values())
    if shutil.disk_usage(directory).free < required + (2 << 30):
        raise RuntimeError(f"Insufficient checkpoint space: need {required} array bytes plus 2 GiB reserve")
    result = {}
    transfer.sync()
    for name, (device, dtype, shape) in registry.items():
        if Path(name).name != name or not name:
            raise ValueError("Unsafe array name")
        path = directory / (name + ".npy")
        array = np.lib.format.open_memmap(path, mode="w+", dtype=dtype, shape=tuple(shape))
        transfer.copy(device, array)
        array.flush()
        del array
        result[name] = dict(file=str(path.name), dtype=np.dtype(dtype).str, shape=list(shape),
                            bytes=path.stat().st_size, sha256=sha256(path))
    return result


def _array_path(directory: Path, record: dict) -> Path:
    path = (directory / record["file"]).resolve(strict=True)
    if not path.is_relative_to(directory.resolve()):
        raise ValueError("Bundle array path escapes its directory")
    return path


def load_arrays(directory: Path, records: dict, *, verify=True) -> dict:
    result = {}
    for name, record in records.items():
        path = _array_path(directory, record)
        if path.stat().st_size != record["bytes"] or (verify and sha256(path) != record["sha256"]):
            raise ValueError(f"Window array integrity failure: {name}")
        a = np.load(path, mmap_mode="r", allow_pickle=False)
        if a.dtype.str != record["dtype"] or list(a.shape) != record["shape"]:
            raise ValueError(f"Window array layout differs: {name}")
        result[name] = a
    return result


def restore_arrays(directory: Path, records: dict, registry: dict, transfer: Transfer):
    if set(registry) != set(records):
        raise ValueError("Window and runtime state field sets differ")
    arrays = load_arrays(directory, records)
    for name, (device, dtype, shape) in registry.items():
        a = arrays[name]
        if a.dtype != np.dtype(dtype) or a.shape != tuple(shape):
            raise ValueError(f"Window and runtime state layout differs: {name}")
        transfer.copy(device, a, to_device=True)
    transfer.sync()


def load_window(path: Path, *, verify=True) -> tuple[dict, dict, dict]:
    """Return manifest, native state and immutable graph without conversions."""
    path = Path(path).resolve(strict=True)
    m = read_json(path / "window.json")
    if m.get("schema") != SCHEMA or m.get("warmup_steps") != 1 or m.get("measured_steps") != 100:
        raise ValueError("Unsupported window contract")
    if m["timed_start_tick"] != m["warmup_start_tick"] + 1 or m["timed_end_tick"] != m["timed_start_tick"] + 100:
        raise ValueError("Window timing boundaries disagree")
    arrays = load_arrays(path / "state", m["arrays"], verify=verify)
    graph_path = Path(m["graph"]["directory"]).resolve(strict=True)
    if sha256(graph_path / "graph.json") != m["graph"]["manifest_sha256"]:
        raise ValueError("Window graph identity differs")
    graph = load_arrays(graph_path, read_json(graph_path / "graph.json")["arrays"], verify=verify)
    return m, arrays, graph


def reference_record(window: dict, counts: np.ndarray, populations: list[str]) -> dict:
    counts = np.asarray(counts)
    if counts.shape != (101, len(populations)) or counts.dtype.kind not in "ui" or np.any(counts < 0) or np.any(counts > 0xffffffff):
        raise ValueError("Expected all 101 integer population-count rows")
    if len(set(populations)) != len(populations):
        raise ValueError("Duplicate population name")
    if populations not in (["input", "exc", "inh"], ["exc", "inh"]):
        raise ValueError("Expected the declared emitted-spike populations")
    _identity(window.get("window_manifest_sha256"))
    _identity(window.get("checkpoint_sha256"))
    totals = counts[1:].sum(axis=0, dtype=np.uint64)
    return dict(schema="activen-baseline-reference-v1", workload=window["workload"], case=window["case"],
                sample=window["index"], boundary=window["boundary"],
                warmup_start_tick=window["warmup_start_tick"],
                timed_start_tick=window["timed_start_tick"], timed_end_tick=window["timed_end_tick"],
                warmup_steps=1, measured_steps=100, phase=window["phase"],
                populations=populations, warmup_counts=[int(x) for x in counts[0]],
                totals=[int(x) for x in totals], total=int(totals.sum(dtype=np.uint64)),
                counts_per_tick=counts.astype(np.uint64).tolist(), rng=window["rng"],
                window_manifest_sha256=window["window_manifest_sha256"],
                checkpoint_sha256=window["checkpoint_sha256"])


def _identity(value):
    if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
        raise ValueError("Expected a concrete SHA-256 window/checkpoint identity")


def compare_counts(reference: dict, observed: dict, *, tolerance: dict | None = None) -> dict:
    """Exact by default. Approximate tolerances must be supplied explicitly.

    A tolerance is a caller-provided acceptance policy, not an inferred
    confidence interval. Preserve its approval/reference in the comparison.
    """
    fields = ("workload", "case", "sample", "boundary", "phase", "warmup_start_tick", "warmup_steps",
              "timed_start_tick", "timed_end_tick", "measured_steps", "populations", "window_manifest_sha256")
    _identity(reference.get("window_manifest_sha256"))
    _identity(observed.get("window_manifest_sha256"))
    for field in fields:
        if field not in reference or field not in observed or reference[field] != observed[field]:
            raise ValueError(f"Different comparison interval/identity: {field}")
    if (reference["measured_steps"] != 100 or type(reference["timed_start_tick"]) is not int
            or reference["timed_start_tick"] < 0 or type(reference["timed_end_tick"]) is not int
            or reference["timed_end_tick"] - reference["timed_start_tick"] != 100
            or reference["warmup_steps"] != 1 or type(reference["warmup_start_tick"]) is not int
            or reference["warmup_start_tick"] < 0
            or reference["timed_start_tick"] != reference["warmup_start_tick"] + 1):
        raise ValueError("Incomplete measured window")
    a, b = reference["totals"], observed["totals"]
    if reference["populations"] not in (["input", "exc", "inh"], ["exc", "inh"]):
        raise ValueError("Expected the declared emitted-spike populations")
    if len(a) != len(reference["populations"]) or len(a) != len(b) or any(type(x) is not int or x < 0 for x in [*a, *b]):
        raise ValueError("Invalid population totals")
    if tolerance is not None:
        if not tolerance.get("approval") or set(tolerance["populations"]) != set(reference["populations"]):
            raise ValueError("Approximate acceptance requires an identified policy for every population")
    errors, passed = {}, True
    for population, expected, actual in zip(reference["populations"], a, b):
        absolute, relative = 0, 0.0
        if tolerance:
            policy = tolerance["populations"][population]
            absolute, relative = policy["absolute"], policy["relative"]
            if not isinstance(absolute, (int, float)) or not math.isfinite(absolute) or absolute < 0 or not math.isfinite(relative) or not 0 <= relative <= 1:
                raise ValueError("Invalid explicit count tolerance")
        limit = max(absolute, relative * expected)
        delta = actual - expected
        ok = abs(delta) <= limit
        errors[population] = dict(reference=expected, observed=actual, difference=delta, allowed=limit, passed=ok)
        passed &= ok
    cycles = observed.get("timer_cycles")
    if type(cycles) is not int or cycles <= 0:
        raise ValueError("Expected a positive peripheral timer cycle total")
    return dict(passed=passed, population_counts=errors, tolerance=tolerance,
                accepted_timer_cycles=cycles if passed else None,
                observed_timer_cycles=cycles, measured_steps=100,
                cycles_per_step=cycles / 100 if passed else None)


def main(argv=None):
    p = argparse.ArgumentParser(description="Compare the same 100-step window's population totals")
    p.add_argument("reference", type=Path)
    p.add_argument("observed", type=Path)
    p.add_argument("--tolerance", type=Path)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args(argv)
    result = compare_counts(read_json(args.reference), read_json(args.observed),
                            tolerance=read_json(args.tolerance) if args.tolerance else None)
    write_json(args.output, result)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
