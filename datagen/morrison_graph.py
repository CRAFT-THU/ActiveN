"""Disk-backed Morrison graphs and strict, chunked GeNN checkpoint import."""
import hashlib
import json
from pathlib import Path
import shutil
import zipfile
import numpy as np

from morrison_model import F, GROUPS, Spec, constants, cold_state, hot_state

BLOCK = 1 << 20
SOURCE_BUCKET = 1024
MAX_BUCKET_BYTES = 512 << 20
MAX_ROW_EDGES = 1 << 20
PAIR = np.dtype([("pre", "<u4"), ("post", "<u4")])


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def slices(n, block=BLOCK):
    for lo in range(0, n, block):
        yield slice(lo, min(n, lo + block))


def partition(records, keys, streams):
    """Vectorized stable partition of a bounded block; no per-edge Python loop."""
    order = np.argsort(keys, kind="stable")
    sorted_keys = keys[order]
    boundaries = np.r_[0, np.flatnonzero(sorted_keys[1:] != sorted_keys[:-1]) + 1, len(keys)]
    for lo, hi in zip(boundaries[:-1], boundaries[1:]):
        if hi > lo:
            records[order[lo:hi]].tofile(streams[int(sorted_keys[lo])])


class Graph:
    """Projection arrays are canonical source-major mmap files, in local IDs."""
    def __init__(self, directory, spec, state, provenance):
        self.directory, self.spec, self.state = Path(directory), spec, state
        self.provenance = provenance
        self.arrays, self.offsets = {}, {}
        self.graph_hash = hashlib.sha256()
        for name, (npre, npost, degree, srcbase, dstbase) in spec.projections().items():
            pre = np.load(self.directory / f"{name}_pre.npy", mmap_mode="r")
            post = np.load(self.directory / f"{name}_post.npy", mmap_mode="r")
            if (pre.ndim != 1 or pre.shape != post.shape or len(pre) != npost * degree
                    or pre.dtype not in (np.dtype("<i4"), np.dtype("<u4"))
                    or post.dtype not in (np.dtype("<i4"), np.dtype("<u4"))):
                raise ValueError(f"invalid {name} graph shape/type")
            incoming, outgoing = np.zeros(npost, np.int64), np.zeros(npre, np.int64)
            last = -1
            for sl in slices(len(pre)):
                p, q = pre[sl], post[sl]
                if (np.any(p < 0) or np.any(p >= npre) or np.any(q < 0) or np.any(q >= npost)
                        or np.any(p[1:] < p[:-1]) or p[0] < last
                        or (srcbase == dstbase and np.any(p == q))):
                    raise ValueError(f"invalid {name} IDs/order/autapse")
                last = int(p[-1])
                incoming += np.bincount(q, minlength=npost)
                outgoing += np.bincount(p, minlength=npre)
            if np.any(incoming != degree):
                raise ValueError(f"{name} does not have the specified fixed indegree")
            if np.max(outgoing) > MAX_ROW_EDGES:
                raise ValueError(f"{name} source row exceeds bounded host workspace")
            for a in (pre, post):
                for sl in slices(len(a)):
                    self.graph_hash.update(a[sl].astype("<u4", copy=False).tobytes())
            self.arrays[name] = (pre, post)
            self.offsets[name] = np.r_[0, np.cumsum(outgoing)]
        weight_path = self.directory / "ee_g.npy"
        self.weights = np.load(weight_path, mmap_mode="r") if weight_path.exists() else None
        if self.weights is not None:
            if self.weights.shape != self.arrays["ee"][0].shape or self.weights.dtype != np.dtype("<f4"):
                raise ValueError("invalid plastic weight array")
            for sl in slices(len(self.weights)):
                w = self.weights[sl]
                if np.any(~np.isfinite(w)) or np.any(w < 0):
                    raise ValueError("invalid plastic weight")

    def row(self, name, source):
        lo, hi = self.offsets[name][source:source+2]
        post = self.arrays[name][1][lo:hi]
        w = None
        if name == "ee":
            # Preserve imported parallel-edge identity and use one rounding for
            # the reference-pA to delivered-current conversion.
            scale = float(constants()["epsc"])
            raw = self.weights[lo:hi] if self.weights is not None else np.full(len(post), F(45.61))
            w = (raw.astype(np.float64) * scale).astype(F)
            if np.any(~np.isfinite(w)):
                raise ValueError("plastic weight overflows delivered-current representation")
        return post, w


def fresh_graph(directory, spec, seed):
    """Fixed indegree with replacement, no autapses; bounded external buckets.

    Only a bounded target block and one source bucket are sorted at once. A
    pathological bucket is rejected before allocation instead of exhausting RAM.
    Normal full-size buckets occupy about 74 MiB (512 MiB enforced ceiling).
    """
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=False)
    rng = np.random.default_rng(seed)
    for name, (npre, npost, degree, srcbase, dstbase) in spec.projections().items():
        work = directory / f"{name}-buckets"
        work.mkdir()
        paths = [work / str(i) for i in range((npre + SOURCE_BUCKET - 1) // SOURCE_BUCKET)]
        streams = [p.open("wb") for p in paths]
        try:
            targets_per_block = max(1, BLOCK // degree)
            for start in range(0, npost, targets_per_block):
                post = np.repeat(np.arange(start, min(npost, start + targets_per_block), dtype="<u4"), degree)
                pre = rng.integers(npre - (srcbase == dstbase), size=len(post), dtype=np.uint32)
                if srcbase == dstbase:
                    pre += pre >= post
                records = np.empty(len(post), PAIR)
                records["pre"], records["post"] = pre, post
                partition(records, pre // SOURCE_BUCKET, streams)
        finally:
            for stream in streams:
                stream.close()
        pre_out = np.lib.format.open_memmap(directory / f"{name}_pre.npy", mode="w+", dtype="<u4", shape=(npost * degree,))
        post_out = np.lib.format.open_memmap(directory / f"{name}_post.npy", mode="w+", dtype="<u4", shape=(npost * degree,))
        cursor = 0
        for path in paths:
            if path.stat().st_size > MAX_BUCKET_BYTES:
                raise ValueError("fresh graph source bucket exceeds bounded workspace; use canonical source-major input")
            records = np.fromfile(path, dtype=PAIR)
            order = np.argsort(records["pre"], kind="stable")
            end = cursor + len(records)
            pre_out[cursor:end], post_out[cursor:end] = records["pre"][order], records["post"][order]
            cursor = end
            path.unlink()
        pre_out.flush()
        post_out.flush()
        work.rmdir()
        print(f"graph {name}: {cursor} edges", flush=True)
    return Graph(directory, spec, cold_state(spec, seed), dict(
        kind="fresh", seed=seed, numpy_version=np.__version__,
        graph_rng="NumPy default_rng; target blocks; replacement; no recurrent autapses",
        physical_state="cold random voltage, zero currents and traces; not a settled checkpoint"))


def checkpoint_graph(path, directory, spec):
    """Import a native directory or extract portable NPZ members, then validate.

    The producer must bind the complete model dictionary to checkpoint metadata.
    An old sparse checkpoint or a histogram cannot establish the model identity.
    Canonical traces are recomputed from lazy edge states at 20/20 ms.
    """
    path, directory = Path(path), Path(directory)
    if path.is_dir():
        from morrison_native import native_checkpoint_graph
        return native_checkpoint_graph(path, directory, spec)
    directory.mkdir(parents=True, exist_ok=False)
    needed = {"metadata"}
    needed.update(f"{g}_{v}" for g in GROUPS for v in ("pre", "post", "input"))
    needed.update(f"{p}_{v}" for p in ("exc", "inh") for v in ("V", "Iex", "Iin", "dIex", "dIin", "refrac", "tick"))
    needed.update(("ee_g", "ee_preTrace", "ee_postTrace", "ee_lastTraceTick"))
    with zipfile.ZipFile(path) as archive:
        for key in sorted(needed):
            with archive.open(f"{key}.npy") as source, (directory / f"{key}.npy").open("wb") as target:
                shutil.copyfileobj(source, target, length=8 << 20)
    data = {key: np.load(directory / f"{key}.npy", mmap_mode="r", allow_pickle=False) for key in needed}
    meta = json.loads(str(data["metadata"]))
    if (meta.get("format") != "brunel-genn-phase-checkpoint-v1"
            or meta.get("boundary") != "after-synapses-before-neurons"
            or meta.get("rule") != "morrison" or meta.get("dt_ms") != .1
            or meta.get("delay_steps") != 0 or meta.get("precision") != "float32"):
        raise ValueError("incompatible full Morrison checkpoint contract")
    tick = checkpoint_clock(meta, data, spec)
    # Validate the graph before IDs are used for trace accumulation.
    graph = Graph(directory, spec, None, {})
    last = data["ee_lastTraceTick"]
    for name in ("pre", "post"):
        lazy = data[f"ee_{name}Trace"]
        if last.shape != lazy.shape or last.shape != graph.arrays["ee"][0].shape:
            raise ValueError("invalid lazy trace shape")
        if last.dtype != np.dtype("<u4") or lazy.dtype != np.dtype("<f4"):
            raise ValueError("invalid lazy trace/timestamp type")

    def blocks():
        for sl in slices(len(last)):
            yield (data["ee_pre"][sl], data["ee_post"][sl], last[sl],
                   data["ee_preTrace"][sl], data["ee_postTrace"][sl])

    traces, errors = canonical_traces(spec, tick, blocks)
    graph.state = hot_state(data, spec, traces)
    graph.provenance = dict(kind="checkpoint", path=str(path.resolve()), sha256=sha256(path),
                            metadata=meta, canonical_trace_error=errors,
                            future_rng="ActiveN per-SMT xorshift32; independent of GeNN XORWOW")
    return graph


def checkpoint_clock(meta, data, spec):
    """Bind the model and exact integer neuron clocks to the saved boundary."""
    actual_model = meta.get("model", {})
    for key, value in spec.model().items():
        if actual_model.get(key) != value:
            raise ValueError(f"checkpoint must record matching model.{key}: {value!r}")
    tick = meta.get("timestep")
    if type(tick) is not int or not 0 <= tick < 2**32:
        raise ValueError("invalid checkpoint timestep")
    for pop, n in (("exc", spec.ne), ("inh", spec.ni)):
        clock = data[f"{pop}_tick"]
        if clock.shape != (n,) or clock.dtype != np.dtype("<u4"):
            raise ValueError(f"invalid {pop}_tick shape/type")
        if np.any(clock != np.uint32(tick)):
            raise ValueError("neuron clock differs from checkpoint boundary")
    return tick


def canonical_traces(spec, tick, blocks):
    """Two bounded passes over real edges; blocks is a repeatable iterator.

    The caller validates shapes, types and IDs. Native padding never enters
    this iterator. Float64 elapsed-time decay avoids rounding uint32 clocks.
    """
    sums = {name: np.zeros(spec.ne, np.float64) for name in ("pre", "post")}
    counts = {name: np.zeros(spec.ne, np.int64) for name in sums}
    for pre, post, last, pre_lazy, post_lazy in blocks():
        if np.any(last > tick):
            raise ValueError("invalid lazy trace/timestamp")
        decay = np.exp(-(tick - last.astype(np.int64)) * .1 / 20.)
        for name, ids, lazy in (("pre", pre, pre_lazy), ("post", post, post_lazy)):
            if np.any(lazy < 0) or np.any(~np.isfinite(lazy)):
                raise ValueError("invalid lazy trace/timestamp")
            values = lazy.astype(np.float64) * decay
            sums[name] += np.bincount(ids, weights=values, minlength=spec.ne)
            counts[name] += np.bincount(ids, minlength=spec.ne)
    traces, errors, squared = {}, {}, {}
    for name in sums:
        if np.any(counts[name] == 0):
            raise ValueError("cannot reconstruct trace for an unrepresented neuron")
        traces[name] = (sums[name] / counts[name]).astype(F)
        errors[name], squared[name] = dict(max_absolute=0.), 0.
    for pre, post, last, pre_lazy, post_lazy in blocks():
        decay = np.exp(-(tick - last.astype(np.int64)) * .1 / 20.)
        for name, ids, lazy in (("pre", pre, pre_lazy), ("post", post, post_lazy)):
            values = lazy.astype(np.float64) * decay
            expected = traces[name][ids]
            error = np.abs(values - expected)
            if np.any(error > 2e-5 * np.maximum(1., np.abs(expected))):
                raise ValueError("edge trace copies disagree beyond conversion tolerance")
            errors[name]["max_absolute"] = max(errors[name]["max_absolute"], float(error.max(initial=0.)))
            squared[name] += float(np.dot(error, error))
    for name in errors:
        errors[name]["rms"] = float(np.sqrt(squared[name] / counts[name].sum()))
    return traces, errors
