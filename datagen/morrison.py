#!/usr/bin/env python3
"""Build capacity-checked full Morrison images with bounded host workspace."""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import struct
import numpy as np

from morrison_graph import Graph, fresh_graph, checkpoint_graph, partition, sha256, BLOCK
from morrison_model import (Spec, F, MAGIC, ABI_VERSION, NODE_BASE, NODE_BYTES,
    MAX_NEURONS, CONFIG, PARAMETERS, POISSON, INIT_END, MC_BYTES, LINE, CHUNK,
    constants, poisson_thresholds)

REVERSE = np.dtype([("post", "<u4"), ("entry", "<u8")])
REVERSE_BUCKET = 1024


class Images:
    def __init__(self, directory, mc):
        self.paths = [Path(directory) / f"dram.{i}" for i in range(mc)]
        self.files = [p.open("w+b") for p in self.paths]
        self.components = [dict(boot=0x10000) for _ in self.paths]
        for f in self.files:
            f.write(bytes(0x10000))

    def place(self, mc, data, component):
        f = self.files[mc]
        padding = (-f.tell()) % LINE
        size = memoryview(data).nbytes
        if f.tell() + padding + size > MC_BYTES:
            raise ValueError(f"MC {mc} exceeds its physical 1 GiB budget while placing {component}")
        f.write(bytes(padding))
        start = f.tell()
        f.write(data)
        counts = self.components[mc]
        counts[component] = counts.get(component, 0) + padding + size
        return start

    def patch(self, mc, offset, data):
        f = self.files[mc]
        end = f.tell()
        if offset < 0 or offset + len(data) > end:
            raise ValueError("patch outside initialized image")
        f.seek(offset)
        f.write(data)
        f.seek(end)

    def close(self):
        for f in self.files:
            f.close()


def topology(pu):
    side = 1 << ((pu.bit_length() - 1 + 1) // 2)
    result = []
    for distance in range(pu):
        x = y = 0
        t, scale = distance, 1
        while scale < side:
            rx, ry = (t >> 1) & 1, (t ^ (t >> 1)) & 1
            if not ry:
                if rx:
                    x, y = scale - 1 - x, scale - 1 - y
                x, y = y, x
            x, y = x + scale * rx, y + scale * ry
            t, scale = t >> 2, scale << 1
        result.append((x, y))
    return np.asarray(result, dtype=np.int16)


class Placement:
    def __init__(self, spec, group):
        self.spec, self.group = spec, group
        xy = topology(spec.pu)
        self.distances = np.abs(xy[:, None] - xy[None, :]).sum(axis=2)
        self.home_hops = self.proxy_hops = self.edges = 0

    def assign(self, source, targets):
        group, spec = self.group, self.spec
        peers = source % spec.pu // group * group + np.arange(group)
        n = len(targets)
        if group == 1:
            chosen = np.zeros(n, np.uint8)
        else:
            rotation = (np.arange(group) + source) % group
            cost = self.distances[targets % spec.pu][:, peers[rotation]]
            preference = np.argsort(cost, axis=1, kind="stable")
            sorted_cost = np.take_along_axis(cost, preference, axis=1)
            order = np.argsort(-(sorted_cost[:, 1] - sorted_cost[:, 0]), kind="stable")
            quota = np.full(group, n // group)
            quota[:n % group] += 1
            assigned = np.full(n, -1, np.int8)
            # Batch candidates rather than making a Python decision per edge.
            for level in range(group):
                for rank in range(group):
                    candidates = order[(assigned[order] < 0) & (preference[order, level] == rank)]
                    selected = candidates[:quota[rank]]
                    assigned[selected] = rank
                    quota[rank] -= len(selected)
            if np.any(assigned < 0) or np.any(quota):
                raise RuntimeError("proxy quota assignment incomplete")
            chosen = rotation[assigned].astype(np.uint8)
        self.home_hops += int(self.distances[source % spec.pu, targets % spec.pu].sum())
        self.proxy_hops += int(self.distances[peers[chosen], targets % spec.pu].sum())
        self.edges += n
        return peers, chosen


def plastic_row(images, mc, weights, destinations):
    """128-edge blocks; the final weight and target arrays each round to a line."""
    n = len(weights)
    full, tail = divmod(n, CHUNK)
    data = np.empty((full, 2 * CHUNK), dtype="<u4")
    data[:, :CHUNK] = weights[:full * CHUNK].view(np.uint32).reshape(full, CHUNK)
    data[:, CHUNK:] = destinations[:full * CHUNK].reshape(full, CHUNK)
    base = images.place(mc, data, "plastic")
    if tail:
        padded = (tail + 7) // 8 * 8
        last = np.zeros(2 * padded, dtype="<u4")
        last[:tail] = weights[-tail:].view(np.uint32)
        last[padded:padded+tail] = destinations[-tail:]
        images.place(mc, last, "plastic")
    indices = np.arange(n, dtype=np.uint32)
    addresses = base + (indices // CHUNK) * 1024 + (indices % CHUNK) * 4
    return base, addresses.astype("<u4")


def scatter_row(images, mc, entries, component):
    count = len(entries)
    if count > 65535 * 4:
        raise ValueError("scatter row exceeds the 16-bit line-count transport")
    if not count:
        return (0, 0)
    padded = np.zeros((count + 3) // 4 * 4, dtype="<u8")
    padded[:count] = entries
    return images.place(mc, padded, component), count


def write_images(graph, output, payload, *, group=4, warmup=60, timed=1000,
                 seed=20260724, functional=False, external=True):
    spec, output = graph.spec, Path(output)
    spec.validate(functional)
    if group not in (1, 4) or spec.pu_per_mc % group or warmup < 0 or timed < 1:
        raise ValueError("invalid proxy group or timestep interval")
    if not external and not functional:
        raise ValueError("disabling external drive is only a functional test")
    code = Path(payload).read_bytes()
    if len(code) + 16 > 8192:
        raise ValueError("payload exceeds the 8 KiB instruction-cache contract")
    if graph.state.shape != (spec.n, 8) or graph.state.dtype != np.dtype("<u4"):
        raise ValueError("invalid compact neuron state")
    if np.any(~np.isfinite(graph.state[:, :7].view(F))):
        raise ValueError("nonfinite compact neuron state")
    if np.any((graph.state[:, 7] & 255) > 5) or np.any(graph.state[:, 7] >> 9):
        raise ValueError("invalid compact status/reserved bytes")
    images = Images(output, spec.mc)
    reverse_paths = [output / f"reverse-work.{mc}" for mc in range(spec.mc)]
    reverse_files = [p.open("wb") for p in reverse_paths]
    capacity = MAX_NEURONS * group
    rows = np.zeros((spec.pu, capacity, 8), dtype="<u4")
    row_counts = np.zeros(spec.pu, np.uint32)
    statdirs = np.zeros((spec.n * group, spec.mc, 2), dtype="<u4")
    revdirs = np.zeros((spec.ne, spec.mc, 2), dtype="<u4")
    placement = Placement(spec, group)
    c = constants()
    plastic_count = static_count = 0
    try:
        for src in range(spec.n):
            slot = src // spec.pu * group + src % group
            peers = src % spec.pu // group * group + np.arange(group)
            rows[peers, slot, 2] = src
            row_counts[peers] = slot + 1
            if src < spec.ne:
                targets, weights = graph.row("ee", src)
                _, chosen = placement.assign(src, targets)
                for rank, owner in enumerate(peers):
                    ids = np.flatnonzero(chosen == rank)
                    post, w = targets[ids], weights[ids]
                    mc = int(owner // spec.pu_per_mc)
                    destination = ((NODE_BASE + post // spec.pu * NODE_BYTES + 20).astype(np.uint32) << 16) | (post % spec.pu + 1).astype(np.uint32)
                    base, addresses = plastic_row(images, mc, w, destination)
                    rows[owner, slot, 3:5] = base, len(ids)
                    records = np.empty(len(ids), REVERSE)
                    records["post"] = post
                    index = (NODE_BASE + src // spec.pu * NODE_BYTES) | mc
                    records["entry"] = ((src % spec.pu + 1) | (index << 16)) | (addresses.astype(np.uint64) << 32)
                    records.tofile(reverse_files[(src % spec.pu) // spec.pu_per_mc])
                    plastic_count += len(ids)
                post, _ = graph.row("ei", src)
                targets = post + spec.ne
                static_weight = F(c["epsc"] * c["external_weight"])
            else:
                post_e, _ = graph.row("ie", src - spec.ne)
                post_i, _ = graph.row("ii", src - spec.ne)
                targets = np.r_[post_e, post_i + spec.ne]
                static_weight = F(c["epsc"] * c["inhibitory_weight"])
            _, chosen = placement.assign(src, targets)
            for rank, owner in enumerate(peers):
                selected = targets[chosen == rank]
                mcs = (selected % spec.pu) // spec.pu_per_mc
                order = np.argsort(mcs, kind="stable")
                ordered = selected[order]
                offsets = np.r_[0, np.cumsum(np.bincount(mcs, minlength=spec.mc))]
                index = NODE_BASE + ordered // spec.pu * NODE_BYTES + (20 if src < spec.ne else 24)
                entries = (ordered % spec.pu + 1).astype(np.uint64) | (index.astype(np.uint64) << 16) | (np.uint64(static_weight.view(np.uint32)) << np.uint64(32))
                for mc in range(spec.mc):
                    lo, hi = offsets[mc:mc+2]
                    statdirs[src * group + rank, mc] = scatter_row(images, mc, entries[lo:hi], "static")
                static_count += len(selected)
            if (src + 1) % 10000 == 0:
                print(f"placed {src+1}/{spec.n} source rows", flush=True)
        for f in reverse_files:
            f.close()
        # Reverse entries are first spooled by destination MC. Partition each
        # file by postsynaptic source bucket in bounded blocks; fixed indegree
        # bounds each second-stage bucket at 1024*ce*12 bytes (110.6 MB target).
        for mc, path in enumerate(reverse_paths):
            work = output / f"reverse-buckets.{mc}"
            work.mkdir()
            paths = [work / str(i) for i in range((spec.ne + REVERSE_BUCKET - 1) // REVERSE_BUCKET)]
            streams = [p.open("wb") for p in paths]
            try:
                with path.open("rb") as source:
                    while True:
                        records = np.fromfile(source, dtype=REVERSE, count=BLOCK)
                        if not len(records):
                            break
                        partition(records, records["post"] // REVERSE_BUCKET, streams)
            finally:
                for stream in streams:
                    stream.close()
            for bucket, bucket_path in enumerate(paths):
                if bucket_path.stat().st_size > REVERSE_BUCKET * spec.ce * REVERSE.itemsize:
                    raise ValueError("reverse bucket violates fixed indegree bound")
                records = np.fromfile(bucket_path, dtype=REVERSE)
                order = np.argsort(records["post"], kind="stable")
                entries = records["entry"][order]
                start, end = bucket * REVERSE_BUCKET, min(spec.ne, (bucket + 1) * REVERSE_BUCKET)
                counts = np.bincount(records["post"] - start, minlength=end-start)
                offsets = np.r_[0, np.cumsum(counts)]
                for local, post in enumerate(range(start, end)):
                    lo, hi = offsets[local:local+2]
                    revdirs[post, mc] = scatter_row(images, mc, entries[lo:hi], "reverse")
                bucket_path.unlink()
            work.rmdir()
            path.unlink()
            print(f"reverse MC {mc} complete", flush=True)
        if plastic_count != spec.ne * spec.ce or static_count != spec.n * (spec.ce + spec.ci) - plastic_count:
            raise RuntimeError("edge conservation failed")
        for src in range(spec.n):
            slot = src // spec.pu * group + src % group
            peers = src % spec.pu // group * group + np.arange(group)
            for rank, owner in enumerate(peers):
                directory = statdirs[src * group + rank]
                padding = bytes((-directory.nbytes) % LINE)
                rows[owner, slot, 5] = images.place(int(owner // spec.pu_per_mc), directory.tobytes() + padding, "directories")
            if src < spec.ne:
                owner = src % spec.pu
                directory = revdirs[src]
                rows[owner, slot, 6] = images.place(owner // spec.pu_per_mc, directory.tobytes() + bytes((-directory.nbytes) % LINE), "directories")
        row_bases = []
        for pu in range(spec.pu):
            row_bases.append(images.place(pu // spec.pu_per_mc, rows[pu, :row_counts[pu]], "metadata"))
        images.patch(0, 16, code)
        section = images.place(0, bytes(spec.pu * 8), "spm-descriptors")
        rng = np.random.default_rng(seed)
        worker_rng = rng.integers(1, 2**32, size=(spec.pu, 2), dtype=np.uint32)
        for pu in range(spec.pu):
            state = graph.state[pu::spec.pu]
            header = np.zeros(64, dtype="<u4")
            header[:11] = [MAGIC, pu+1, spec.pu, 1, len(state), 0, 0, 0, warmup, warmup+timed, pu // spec.pu_per_mc]
            header[43:45] = worker_rng[pu]
            for i, key in enumerate(("p11", "p21", "p31", "p32", "p33", "epsc", "delivery", "external_mean", "external_limit", "external_weight", "dep", "pot", "wmax", "decay_pre", "decay_post")):
                header[46+i] = c[key].view(np.uint32)
            if not external:
                header[55] = 0
            blob = bytearray(INIT_END)
            blob[:256] = header.tobytes()
            blob[NODE_BASE:NODE_BASE+state.nbytes] = state.tobytes()
            peers = pu // group * group + np.arange(group) + 1
            config = np.zeros(16, dtype="<u4")
            config[:group] = peers
            config[8] = row_counts[pu]
            config[14] = 0x50580000 | group
            blob[CONFIG:CONFIG+64] = config.tobytes()
            struct.pack_into("<8I", blob, PARAMETERS, spec.ne, spec.ni, spec.mc, row_bases[pu], ABI_VERSION, group, 0, 0)
            table = poisson_thresholds(2.088 if external else 0.)
            blob[POISSON:POISSON+table.nbytes] = table.tobytes()
            offset = images.place(0, blob, "spm-initialization")
            images.patch(0, section+pu*8, struct.pack("<II", offset-section, len(blob)))
        images.patch(0, 0, struct.pack("<IIII", 0x0100006F, section, spec.mc, spec.pu))
    finally:
        for f in reverse_files:
            f.close()
        images.close()
    np.save(output / "neurons.npy", graph.state)
    np.save(output / "plastic-outdegree.npy", np.diff(graph.offsets["ee"]).astype("<u4"))
    sizes = [p.stat().st_size for p in images.paths]
    manifest = dict(format="activen-morrison-image-v1", model=spec.model(), spec=asdict(spec),
        system=spec.system(), functional=functional, source=graph.provenance,
        group_size=group, placement="consecutive groups, equal row quotas, batched locality preference",
        mean_home_hops=placement.home_hops / max(1, placement.edges),
        mean_proxy_hops=placement.proxy_hops / max(1, placement.edges),
        warmup=warmup, timed=timed, seed=seed, external_drive=external,
        constants={k: float(v) for k, v in c.items()},
        plastic_edges=plastic_count, static_edges=static_count,
        graph_sha256=graph.graph_hash.hexdigest(),
        neuron_sha256=sha256(output / "neurons.npy"), payload_sha256=sha256(payload),
        image_bytes=sizes, image_sha256=[sha256(p) for p in images.paths],
        component_bytes=images.components, per_mc_remaining_bytes=[MC_BYTES-n for n in sizes],
        plastic_layout="chunk128; final weights and targets independently padded to 32 bytes",
        trace_state="two learning-scaled FP32 coefficients; excitatory only",
        neuron_state="32 bytes; voltage, total alpha current/derivative, X/Y, E/I inputs, byte refractory/fire",
        ordering="integrate; old-weight delivery and quiet LTD; post-active LTD then LTP; trace increments",
        rng="per-SMT xorshift32, inclusive inverse-CDF at 32-bit uniform resolution",
        benchmark_ready=(not functional and graph.provenance["kind"] == "checkpoint"))
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--fresh", action="store_true", help="cold state; capacity/functional work, not a settled benchmark")
    source.add_argument("--checkpoint", type=Path, help="complete GeNN phase checkpoint with bound full-model metadata")
    parser.add_argument("--group", type=int, choices=(1, 4), default=4)
    parser.add_argument("--seed", type=int, default=20260724)
    parser.add_argument("--warmup", type=int, default=60)
    parser.add_argument("--timed", type=int, default=1000)
    parser.add_argument("--payload", type=Path)
    parser.add_argument("--functional", action="store_true")
    for key, value in asdict(Spec()).items():
        parser.add_argument(f"--{key}", type=int, default=value)
    args = parser.parse_args()
    spec = Spec(**{key: getattr(args, key) for key in asdict(Spec())})
    spec.validate(args.functional)
    args.output.mkdir(parents=True, exist_ok=False)
    graph_dir = args.output / "graph"
    graph = (fresh_graph(graph_dir, spec, args.seed) if args.fresh else
             checkpoint_graph(args.checkpoint, graph_dir, spec))
    payload = args.payload or Path(__file__).resolve().parents[1] / f"sim/payloads/sys/morrison_g{args.group}.bin"
    manifest = write_images(graph, args.output, payload, group=args.group,
                            warmup=args.warmup, timed=args.timed, seed=args.seed, functional=args.functional)
    print(json.dumps(dict(image_bytes=manifest["image_bytes"],
                          remaining_bytes=manifest["per_mc_remaining_bytes"])), flush=True)


if __name__ == "__main__":
    main()
