"""Read the collected baseline formats without changing their native state.

The readers retain RNG, pending events, timestamps and graph padding. Logical
classification weights are an additional view, never a replacement checkpoint.
"""
from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
import csv
import hashlib
import json
from pathlib import Path
from typing import Iterator

import numpy as np

CLASSIFICATION = {"mnist", "fashion-mnist", "cifar10"}
NATIVE_CLASSIFICATION = "mnist-genn-native-v1"
NATIVE_BRUNEL = "brunel-genn-phase-checkpoint-v1"
NATIVE_MORRISON = "genn-native-phase-checkpoint-v1"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_json(path: Path) -> dict:
    return json.loads(Path(path).read_text())


def write_json(path: Path, value: dict) -> None:
    with Path(path).open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)
        stream.write("\n")


@dataclass(frozen=True)
class Checkpoint:
    path: Path
    format: str
    metadata: dict
    identity: str

    @property
    def tick(self) -> int:
        return int(self.metadata["timestep"])

    @property
    def boundary(self) -> str:
        return self.metadata["boundary"]

    @contextmanager
    def arrays(self, *, verify: bool = True) -> Iterator[dict]:
        """Full Morrison uses mmap; NPZ files are opened only for this context."""
        if self.format == NATIVE_MORRISON:
            result = {}
            for name, item in self.metadata["arrays"].items():
                path = self.path / (name + ".npy")
                if verify and (path.stat().st_size != item["bytes"] or sha256(path) != item["sha256"]):
                    raise ValueError(f"Native array integrity failure: {path}")
                a = np.load(path, mmap_mode="r", allow_pickle=False)
                _layout(a, item, name)
                result[name] = a
            yield result
            return
        archive = self.path / "state.npz" if self.format == NATIVE_CLASSIFICATION else self.path
        expected = self.metadata.get("archive_sha256", self.identity)
        if verify and sha256(archive) != expected:
            raise ValueError(f"Native archive checksum differs: {archive}")
        with np.load(archive, allow_pickle=False) as data:
            if self.format == NATIVE_CLASSIFICATION:
                if set(data.files) != set(self.metadata["arrays"]):
                    raise ValueError("Native archive field set differs")
                # NPZ loading is lazy. Validate each field once, then expose the
                # archive for callers that need only a subset of its arrays.
                for name, item in self.metadata["arrays"].items():
                    a = data[name]
                    _layout(a, item, name)
                    if verify and hashlib.sha256(np.ascontiguousarray(a).view(np.uint8)).hexdigest() != item["sha256"]:
                        raise ValueError(f"Native array checksum differs: {name}")
            yield data


def _layout(a: np.ndarray, item: dict, name: str) -> None:
    if a.dtype.str != item["dtype"] or list(a.shape) != item["shape"] or a.dtype.hasobject:
        raise ValueError(f"Native array layout differs: {name}")


def inspect_checkpoint(path: Path, *, verify: bool = False) -> Checkpoint:
    path = Path(path).resolve(strict=True)
    if "forensic" in path.parts or path.name == "failed-state":
        raise ValueError("Forensic failed states are not restart checkpoints")
    if path.is_dir():
        manifest = path / "manifest.json"
        meta = read_json(manifest)
        fmt = meta.get("schema", meta.get("format"))
        identity = sha256(manifest)
        if fmt not in (NATIVE_CLASSIFICATION, NATIVE_MORRISON):
            raise ValueError("Expected a native restart checkpoint, not a comparison endpoint")
        if meta.get("continuation_eligible") is False:
            raise ValueError("Checkpoint explicitly forbids continuation")
        if fmt == NATIVE_MORRISON:
            if meta.get("kind") != "state":
                raise ValueError("Expected Morrison state, not graph")
            graph = (path / meta["graph"]).resolve(strict=True)
            if sha256(graph / "manifest.json") != meta["graph_manifest_sha256"]:
                raise ValueError("Morrison graph manifest binding differs")
    else:
        with np.load(path, allow_pickle=False) as data:
            if "metadata" not in data:
                raise ValueError("Weights/theta-only archives are not native restart states")
            meta = json.loads(str(data["metadata"]))
        fmt = meta.get("format")
        if fmt != NATIVE_BRUNEL:
            raise ValueError("Unknown native checkpoint format")
        identity = sha256(path)
    if type(meta.get("timestep")) is not int or meta["timestep"] < 0:
        raise ValueError("Invalid checkpoint clock")
    cp = Checkpoint(path, fmt, meta, identity)
    if verify:
        with cp.arrays():
            pass
    return cp


@dataclass(frozen=True)
class Case:
    baseline: Path
    workload: str
    name: str
    start: Checkpoint
    endpoint: Checkpoint | None

    @property
    def root(self) -> Path:
        return self.baseline / self.workload / self.name

    @property
    def classification(self) -> bool:
        return self.workload in CLASSIFICATION

    @property
    def region(self) -> tuple[int, int]:
        if self.classification:
            return self.start.tick, self.endpoint.tick
        first = self.start.tick + 60
        return first, first + 1_000_000

    @property
    def phases(self) -> tuple[int, int] | None:
        if not self.classification:
            return None
        c = self.start.metadata["constants"]
        lengths = tuple(round(c[k] / c["dt_ms"]) for k in ("stimulus_ms", "rest_ms"))
        if any(n <= 0 for n in lengths):
            raise ValueError("Invalid phase lengths")
        return lengths

    @property
    def k(self) -> int:
        return 100 if self.workload == "full-morrison" else 1000

    @property
    def boundary(self) -> str:
        return ("after-synapses-before-neurons" if self.workload == "full-morrison"
                else "after-neurons-before-synapses")

    def record(self) -> dict:
        return dict(workload=self.workload, case=self.name, region=list(self.region),
                    region_identity="retained-checkpoint-timeline",
                    checkpoint=str(self.start.path), checkpoint_sha256=self.start.identity,
                    endpoint=str(self.endpoint.path) if self.endpoint else None,
                    endpoint_sha256=self.endpoint.identity if self.endpoint else None,
                    boundary=self.boundary, phase_ticks=self.phases, windows=self.k,
                    dt_ms=self.start.metadata.get("dt_ms", self.start.metadata.get("constants", {}).get("dt_ms")),
                    baseline_warmup=0 if self.classification else 60)


def load_cases(baseline: Path) -> list[Case]:
    baseline = Path(baseline).resolve(strict=True)
    result, seen = [], set()
    with (baseline / "cases.csv").open(newline="") as stream:
        for row in csv.DictReader(stream):
            key = row["workload"], row["case"]
            if key in seen or row["status"] != "complete":
                raise ValueError(f"Duplicate or incomplete case: {key}")
            seen.add(key)
            for field in ("timing_start", "training_endpoint"):
                if row[field] and not (baseline / row[field]).resolve().is_relative_to(baseline):
                    raise ValueError("Collection alias escapes baseline")
            start = inspect_checkpoint(baseline / row["timing_start"])
            end = inspect_checkpoint(baseline / row["training_endpoint"]) if row["training_endpoint"] else None
            case = Case(baseline, *key, start, end)
            if case.classification:
                ages = (28000, 30000) if case.workload == "cifar10" else (160000, 180000)
                if end is None or (start.metadata["controller"]["accepted"], end.metadata["controller"]["accepted"]) != ages:
                    raise ValueError(f"Wrong accepted-image region: {key}")
                for cp in (start, end):
                    if cp.format != NATIVE_CLASSIFICATION or cp.metadata["variant"]["name"] != case.name:
                        raise ValueError(f"Checkpoint case differs: {key}")
                    if cp.metadata["controller"]["phase"] != "before-attempt" or cp.boundary != case.boundary:
                        raise ValueError("Classification region has an unsupported boundary")
                for field in ("constants", "variant", "structural_mask_sha256"):
                    if start.metadata[field] != end.metadata[field]:
                        raise ValueError(f"Region endpoints disagree: {field}")
            elif case.workload == "brunel":
                if start.format != NATIVE_BRUNEL or start.metadata["rule"] != case.name:
                    raise ValueError("Wrong Brunel checkpoint")
            elif case.workload == "full-morrison":
                if start.format != NATIVE_MORRISON or case.name != "default":
                    raise ValueError("Wrong Morrison checkpoint")
            else:
                raise ValueError(f"Unsupported workload: {case.workload}")
            if case.region[1] <= case.region[0]:
                raise ValueError("Empty sampling region")
            result.append(case)
    return result


def checkpoint_index(case: Case) -> list[Checkpoint]:
    """Eligible ancestors; use metadata, never a filename as a training age."""
    if not case.classification:
        return [case.start]
    checkpoints = {}
    for category in ("checkpoints", "phase-checkpoints", "retry-checkpoints", "skip-checkpoints"):
        for manifest in sorted((case.root / category).glob("r*/*/manifest.json")):
            cp = inspect_checkpoint(manifest.parent)
            m, wanted = cp.metadata, case.start.metadata
            if cp.tick > case.region[1] or m.get("variant") != wanted["variant"]:
                continue
            if any(m.get(k) != wanted.get(k) for k in ("constants", "structural_mask_sha256", "source_sha256", "boundary")):
                continue
            # Legacy rest states cannot reconstruct the controller's saved
            # presentation readout when replay crosses the rest boundary.
            ctl = m["controller"]
            if case.workload != "mnist" and ctl["phase"] == "rest" and ctl.get("stimulus_counts") is None:
                continue
            checkpoints[str(cp.path)] = cp
    checkpoints[str(case.start.path)] = case.start
    return sorted(checkpoints.values(), key=lambda cp: (cp.tick, str(cp.path)))


def latest_checkpoint(index: list[Checkpoint], tick: int) -> Checkpoint:
    candidates = [cp for cp in index if cp.tick <= tick]
    if not candidates:
        raise ValueError("No earlier checkpoint for selected timestep")
    return max(candidates, key=lambda cp: (cp.tick, str(cp.path)))


def classification_view(cp: Checkpoint, *, verify: bool = True) -> dict:
    """Lossless logical weights/mask plus ALL native arrays and metadata.

    No transient, RNG, trace or pending-event folding is performed. A payload
    converter must explicitly handle the native event boundary and state fields.
    """
    if cp.format != NATIVE_CLASSIFICATION:
        raise ValueError("Expected a native classification checkpoint")
    with cp.arrays(verify=verify) as source:
        arrays = {name: source[name].copy() for name in source.files}
    return classification_arrays_view(cp.metadata, arrays)


def classification_arrays_view(metadata: dict, arrays: dict) -> dict:
    """Interpret classification arrays without regenerating sparse masks."""
    c = metadata["constants"]
    ni, ne = int(c["n_input"]), int(c["n_exc"])
    rows = arrays["feedforward__rowLength"].reshape(-1)
    ind = arrays["feedforward__ind"].reshape(-1)
    g = arrays["feedforward__vars__g"].reshape(-1)
    if len(rows) != ni or len(ind) != len(g) or len(ind) % ni:
        raise ValueError("Invalid padded feedforward layout")
    stride = len(ind) // ni
    weights = np.zeros((ni, ne), dtype="<f4")
    mask = np.zeros((ni, ne), dtype=np.uint8)
    for pre, count in enumerate(rows):
        count = int(count)
        if count > stride:
            raise ValueError("Feedforward row exceeds its stride")
        sl = slice(pre * stride, pre * stride + count)
        posts, values = ind[sl], g[sl]
        if np.any(posts >= ne) or len(np.unique(posts)) != count:
            raise ValueError("Invalid or duplicate classification edge")
        if not np.all(np.isfinite(values)) or np.any(values < 0):
            raise ValueError("Invalid feedforward weights")
        weights[pre, posts] = values
        mask[pre, posts] = 1
    if hashlib.sha256(mask.tobytes()).hexdigest() != metadata["structural_mask_sha256"]:
        raise ValueError("Logical structural mask differs from native contract")
    return dict(metadata=metadata, arrays=arrays, weights=weights, structural_mask=mask)
