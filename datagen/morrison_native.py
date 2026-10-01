"""Bounded import of GeNN's padded, manifest-bound Morrison checkpoint format."""
import json
from pathlib import Path
import numpy as np

from morrison_graph import (BLOCK, MAX_ROW_EDGES, Graph, canonical_traces,
                            checkpoint_clock, sha256)
from morrison_model import GROUPS, hot_state

FORMAT = "genn-native-phase-checkpoint-v1"


def manifest(path, kind):
    value = json.loads(path.read_text())
    if value.get("format") != FORMAT or value.get("kind") != kind:
        raise ValueError(f"invalid native {kind} manifest")
    return value


def arrays(directory, meta, expected):
    """Verify every native file, including state not used by ActiveN's ABI."""
    recorded = meta.get("arrays", {})
    if set(recorded) != set(expected):
        raise ValueError("native manifest array set does not match the checkpoint contract")
    result = {}
    for name, (dtype, shape) in expected.items():
        path, entry = directory / f"{name}.npy", recorded[name]
        if path.stat().st_size != entry.get("bytes") or sha256(path) != entry.get("sha256"):
            raise ValueError(f"native {name} size/hash mismatch")
        a = np.load(path, mmap_mode="r", allow_pickle=False)
        if (a.dtype.str != entry.get("dtype") or list(a.shape) != entry.get("shape")
                or a.dtype != np.dtype(dtype) or a.ndim != len(shape)
                or any(n is not None and n != actual for n, actual in zip(shape, a.shape))):
            raise ValueError(f"invalid native {name} shape/type")
        result[name] = a
        print(f"verified native {name}: {path.stat().st_size} bytes", flush=True)
    return result


def real_slots(rows, stride):
    """Yield bounded flat indices in source-major order, skipping only padding."""
    rows_per_block = max(1, BLOCK // stride)
    columns = np.arange(stride, dtype=np.int64)
    for start in range(0, len(rows), rows_per_block):
        lengths = rows[start:start+rows_per_block]
        sources = np.arange(start, start+len(lengths), dtype=np.uint32)
        present = columns[None, :] < lengths[:, None]
        slots = (sources.astype(np.int64)[:, None] * stride + columns)[present]
        yield np.repeat(sources, lengths.astype(np.int64)), slots


def native_checkpoint_graph(path, directory, spec):
    path, directory = Path(path), Path(directory)
    state_manifest = path / "manifest.json"
    meta = manifest(state_manifest, "state")
    compatibility = meta.get("compatibility", {})
    if (meta.get("boundary") != "after-synapses-before-neurons" or meta.get("dt_ms") != .1
            or compatibility.get("scalar_dtype") != "<f4" or compatibility.get("dt_ms") != .1
            or compatibility.get("axonal_delay_steps") != dict.fromkeys(GROUPS, 0)
            or compatibility.get("post_delay_steps") != 0
            or compatibility.get("stdp_timing") != "arrival"
            or compatibility.get("stdp_tie_order") != "nest_causal_boundary"):
        raise ValueError("incompatible native Morrison checkpoint contract")
    reference = meta.get("graph")
    if not isinstance(reference, str) or not reference:
        raise ValueError("native checkpoint must name its graph directory")
    graph_path = (path / reference).resolve()
    graph_manifest = graph_path / "manifest.json"
    graph_hash = sha256(graph_manifest)
    if graph_hash != meta.get("graph_manifest_sha256"):
        raise ValueError("native graph manifest hash mismatch")
    graph_meta = manifest(graph_manifest, "graph")
    graph_shapes = {}
    for name, (npre, _, _, _, _) in spec.projections().items():
        graph_shapes[f"{name}_rowLength"] = ("<u4", (npre,))
        graph_shapes[f"{name}_ind"] = ("<u4", (None,))
    graph_shapes.update(ee_colLength=("<u4", (spec.ne,)),
                        ee_remap=("<u4", (spec.ne * spec.ce,)))
    connectivity = arrays(graph_path, graph_meta, graph_shapes)
    strides = {}
    for name, (npre, npost, degree, _, _) in spec.projections().items():
        rows, ind = connectivity[f"{name}_rowLength"], connectivity[f"{name}_ind"]
        stride, remainder = divmod(len(ind), npre)
        if (remainder or not 0 < stride <= MAX_ROW_EDGES or np.any(rows > stride)
                or int(rows.sum(dtype=np.uint64)) != npost * degree):
            raise ValueError(f"invalid native {name} row geometry")
        strides[name] = stride
    if np.any(connectivity["ee_colLength"] != spec.ce):
        raise ValueError("invalid native ee column lengths")
    state_shapes = {}
    for pop, n in (("exc", spec.ne), ("inh", spec.ni)):
        for field in ("V", "Iex", "Iin", "dIex", "dIin"):
            state_shapes[f"{pop}_{field}"] = ("<f4", (n,))
        for field in ("refrac", "tick", "lastSpikeTick", "spikeCount"):
            state_shapes[f"{pop}_{field}"] = ("<u4", (n,))
        state_shapes[f"{pop}_rng"] = ("<u4", (n, 6))
    for field in ("g", "preTrace", "postTrace", "lastTraceTick", "lastPreUpdateTick", "lastPostUpdateTick"):
        state_shapes[f"ee_{field}"] = ("<u4" if field.startswith("last") else "<f4",
                                       connectivity["ee_ind"].shape)
    for name, (_, npost, _, _, _) in spec.projections().items():
        state_shapes[f"{name}_outPost"] = ("<f4", (1, npost))
    data = arrays(path, meta, state_shapes)
    tick = checkpoint_clock(meta, data, spec)
    directory.mkdir(parents=True, exist_ok=False)
    for name, (_, npost, degree, _, _) in spec.projections().items():
        count = npost * degree
        pre = np.lib.format.open_memmap(directory / f"{name}_pre.npy", mode="w+", dtype="<u4", shape=(count,))
        post = np.lib.format.open_memmap(directory / f"{name}_post.npy", mode="w+", dtype="<u4", shape=(count,))
        weights = (np.lib.format.open_memmap(directory / "ee_g.npy", mode="w+", dtype="<f4", shape=(count,))
                   if name == "ee" else None)
        cursor = 0
        for sources, slots in real_slots(connectivity[f"{name}_rowLength"], strides[name]):
            end = cursor + len(slots)
            pre[cursor:end] = sources
            post[cursor:end] = connectivity[f"{name}_ind"][slots]
            if weights is not None:
                weights[cursor:end] = data["ee_g"][slots]
            cursor = end
        if cursor != count:
            raise ValueError("native edge conservation failed")
        pre.flush()
        post.flush()
        if weights is not None:
            weights.flush()
        print(f"native graph {name}: {count} real edges", flush=True)
    # Validate connectivity before any target ID is used for trace accumulation.
    graph = Graph(directory, spec, None, {})

    def blocks():
        for pre, slots in real_slots(connectivity["ee_rowLength"], strides["ee"]):
            yield (pre, connectivity["ee_ind"][slots], data["ee_lastTraceTick"][slots],
                   data["ee_preTrace"][slots], data["ee_postTrace"][slots])

    traces, errors = canonical_traces(spec, tick, blocks)
    # outPost is already EPSC-scaled and is due at the next neuron integration.
    # Saved spike/tie/RNG state belongs to GeNN's event order, not ActiveN's ABI.
    for name in GROUPS:
        data[f"{name}_input"] = data[f"{name}_outPost"]
    graph.state = hot_state(data, spec, traces)
    graph.provenance = dict(kind="checkpoint", path=str(path.resolve()),
        sha256=sha256(state_manifest), identity="native state manifest SHA-256",
        metadata=meta, graph_path=str(graph_path), graph_manifest_sha256=graph_hash,
        graph_manifest=graph_meta, canonical_trace_error=errors,
        conversion="source-major real slots; individual edges retained; reverse references rebuilt",
        future_rng="ActiveN per-SMT xorshift32; independent of GeNN XORWOW")
    return graph
