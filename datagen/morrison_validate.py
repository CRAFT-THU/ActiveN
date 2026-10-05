#!/usr/bin/env python3
"""Independently decode compact images, edge identities, and functional dumps."""
import argparse
import json
from pathlib import Path
import struct
import numpy as np

from morrison_graph import sha256
from morrison_model import (Spec, MAGIC, NODE_BASE, NODE_BYTES, CONFIG, PARAMETERS,
    INIT_END, SPM_BYTES, MC_BYTES, F, constants)


class WeightMoments:
    """Population moments without subtracting two nearly equal squared sums."""
    def __init__(self):
        self.count, self.mean, self.m2 = 0, 0., 0.

    def add(self, values):
        values = np.asarray(values, dtype=np.float64)
        n = values.size
        if not n:
            return
        mean = float(values.mean())
        centered = values - mean
        delta = mean - self.mean
        total = self.count + n
        self.m2 += float(np.dot(centered, centered)) + delta * delta * self.count * n / total
        self.mean += delta * n / total
        self.count = total

    @property
    def std(self):
        return float(np.sqrt(self.m2 / self.count)) if self.count else 0.


class Image:
    def __init__(self, directory, backing=None, hashes=True):
        self.directory = Path(directory)
        self.manifest = json.loads((self.directory / "manifest.json").read_text())
        if self.manifest["format"] not in ("activen-morrison-image-v1", "activen-morrison-image-v2"):
            raise ValueError("unknown image ABI")
        self.bulk = self.manifest["format"] == "activen-morrison-image-v2"
        self.spec = Spec(**self.manifest["spec"])
        self.spec.validate(self.manifest["functional"])
        self.group = self.manifest["group_size"]
        if self.group not in (1, 4):
            raise ValueError("invalid group")
        if self.bulk:
            width = self.manifest['dispatch_width']
            policy = self.manifest.get('dispatch_policy', 'legacy')
            completion = self.manifest.get('forward_completion','poll')
            if (completion not in ('poll','tree') or policy not in ('legacy', 'balanced') or width < 1 or width & (width-1)
                    or width > self.spec.pu_per_mc or (policy == 'legacy' and width > 4)):
                raise ValueError('invalid bulk dispatch policy/width')
        base = self.directory if backing is None else Path(backing)
        self.memory = []
        for mc in range(self.spec.mc):
            path = base / f"dram.{mc}"
            size = path.stat().st_size
            if size != self.manifest["image_bytes"][mc] or size > MC_BYTES or size % 32:
                raise ValueError(f"MC {mc} image size mismatch/overflow")
            if backing is None and hashes and sha256(path) != self.manifest["image_sha256"][mc]:
                raise ValueError(f"MC {mc} hash mismatch")
            self.memory.append(np.memmap(path, mode="r", dtype="<u4"))
        boot = self.read(0, 0, 4)
        if tuple(boot[[0, 2, 3]]) != (0x0100006F, self.spec.mc, self.spec.pu):
            raise ValueError("bad boot header")
        self.spms, self.rows = [], []
        degrees = np.load(self.directory / 'plastic-outdegree.npy') if self.bulk else None
        if self.bulk and (degrees.shape != (self.spec.ne,) or degrees.dtype != np.dtype('<u4')):
            raise ValueError('invalid ABI2 outdegree array')
        for pu in range(self.spec.pu):
            offset, size = self.read(0, int(boot[1])+pu*8, 2)
            if size != INIT_END or offset % 32:
                raise ValueError("bad SPM initialization descriptor")
            spm = self.read(0, int(boot[1])+int(offset), int(size)//4)
            local = len(range(pu, self.spec.n, self.spec.pu))
            if tuple(spm[:5]) != (MAGIC, pu+1, self.spec.pu, 1, local):
                raise ValueError("bad PU header")
            params = spm[PARAMETERS//4:PARAMETERS//4+8]
            if self.bulk:
                stride = max(32, self.spec.mc*8)
                if tuple(params[[0,1,2,4,5,7]]) != (self.spec.ne,self.spec.ni,self.spec.mc,2,self.manifest['dispatch_width'],stride):
                    raise ValueError('bad ABI2 parameters')
                if int(spm[PARAMETERS//4+8]) != (1 if self.manifest.get('dispatch_policy', 'legacy') == 'balanced' else 0):
                    raise ValueError('bulk dispatch policy disagrees with manifest')
                if int(spm[PARAMETERS//4+9]) != int(self.manifest.get('forward_completion','poll') == 'tree'):
                    raise ValueError('forward completion policy disagrees with manifest')
                gids = np.arange(pu, self.spec.n, self.spec.pu, dtype=np.uint32)
                exc = int((gids < self.spec.ne).sum())
                pointers = spm[0x2ea0//4:0x2ea0//4+exc+1].astype(np.uint64)
                counts = degrees[gids[:exc]].astype(np.uint64)
                if np.any(pointers%32) or np.any(np.diff(pointers) != ((counts+8)//8)*64):
                    raise ValueError('bad consecutive CSR boundaries')
                rows = np.zeros((len(gids),8), '<u4')
                rows[:,2] = gids
                rows[:exc,3] = pointers[:-1]
                rows[:exc,4] = counts
                rows[:,5] = int(params[3]) + np.arange(len(gids))*stride
                rows[:exc,6] = int(params[6]) + np.arange(exc)*stride
                mc = pu//self.spec.pu_per_mc
                for base,n in zip(pointers[:-1],counts):
                    base,n = int(base),int(n)
                    slots = (n%128+8)//8*8
                    tail = base+n//128*1024
                    w = self.read(mc,tail,slots)
                    d = self.read(mc,tail+slots*4,slots)
                    if np.any(w[n%128:]) or np.any(d[n%128:] != 0xffffffff):
                        raise ValueError('missing sentinel or invalid tail padding')
                self.spms.append(spm); self.rows.append(rows)
                continue
            if tuple(params[[0, 1, 2, 4, 5, 6, 7]]) != (self.spec.ne, self.spec.ni, self.spec.mc, 1, self.group, 0, 0):
                raise ValueError("bad compact parameters")
            config = spm[CONFIG//4:CONFIG//4+16]
            count = int(config[8])
            peers = pu // self.group * self.group + np.arange(self.group)
            expected_ids = (np.arange(220)[:, None] * self.spec.pu + peers).ravel()
            expected_ids = expected_ids[expected_ids < self.spec.n]
            if count != len(expected_ids) or config[14] != (0x50580000 | self.group):
                raise ValueError("bad proxy source count/magic")
            if not np.array_equal(config[:self.group], peers+1):
                raise ValueError("bad proxy peer IDs")
            rows = self.read(pu // self.spec.pu_per_mc, int(params[3]), count * 8).reshape(-1, 8)
            if not np.array_equal(rows[:, 2], expected_ids) or np.any(rows[:, [0, 1, 7]]):
                raise ValueError("bad metadata source IDs/reserved words")
            self.spms.append(spm)
            self.rows.append(rows)

    def read(self, mc, offset, words):
        if (not 0 <= mc < self.spec.mc or offset < 0 or offset % 4 or words < 0
                or offset // 4 + words > len(self.memory[mc])):
            raise ValueError(f"out-of-image access MC={mc} offset={offset} words={words}")
        return self.memory[mc][offset//4:offset//4+words]

    def forward(self, pu, row, backing=None):
        mc, n, base = pu // self.spec.pu_per_mc, int(row[4]), int(row[3])
        if not n:
            return np.empty(0, np.uint32), np.empty(0, F), np.empty(0, np.uint32)
        if int(row[2]) >= self.spec.ne or base % 32:
            raise ValueError("invalid plastic row")
        index = np.arange(n, dtype=np.uint32)
        address = base + index//128*1024 + index%128*4
        # Independent decode of the compact tail geometry.
        stored = n + int(self.bulk)
        last = stored//128*128
        delta = np.where(index < last, 512, ((stored % 128 + 7)//8)*32)
        target_address = address.astype(np.int64) + delta
        if int(target_address.max())//4 >= len(self.memory[mc]):
            raise ValueError("plastic row outside image")
        memory = self.memory if backing is None else backing
        target = memory[mc][target_address//4]
        dst, offset = target & 65535, target >> 16
        if np.any(dst < 1) or np.any(dst > self.spec.pu) or np.any(offset < NODE_BASE+20) or np.any((offset-NODE_BASE-20) % NODE_BYTES):
            raise ValueError("bad plastic destination")
        post = (offset-NODE_BASE-20)//NODE_BYTES*self.spec.pu+dst-1
        if np.any(post >= self.spec.ne) or np.any(post == row[2]):
            raise ValueError("plastic destination population/autapse")
        return post, memory[mc][address//4].view(F), address

    def directory_entries(self, owner_mc, address):
        if address % 32:
            raise ValueError("unaligned directory")
        return self.read(owner_mc, int(address), self.spec.mc*2).reshape(-1, 2)


def check_images(directory, compare_graph=True):
    image = Image(directory)
    spec = image.spec
    initial = np.load(image.directory / "neurons.npy", mmap_mode="r")
    if initial.shape != (spec.n, 8) or sha256(image.directory / "neurons.npy") != image.manifest["neuron_sha256"]:
        raise ValueError("initial neuron identity mismatch")
    incoming = np.zeros((2, spec.n), np.uint32)
    forward_rows = [[] for _ in range(spec.mc)]
    reverse_rows = [[] for _ in range(spec.mc)]
    plastic = static = 0
    c = constants()
    moments = WeightMoments()
    # Optional independent multiset comparison to the retained canonical graph.
    graphs, cuts = {}, {}
    if compare_graph:
        for name in ("ee", "ei", "ie", "ii"):
            pre = np.load(image.directory / "graph" / f"{name}_pre.npy", mmap_mode="r")
            post = np.load(image.directory / "graph" / f"{name}_post.npy", mmap_mode="r")
            graphs[name] = (pre, post)
            npre = spec.ne if name[0] == "e" else spec.ni
            cuts[name] = np.searchsorted(pre, np.arange(npre+1))
        path = image.directory / "graph/ee_g.npy"
        raw_weights = np.load(path, mmap_mode="r") if path.exists() else None
    plastic_by_source, static_by_source = {}, {}
    for pu, rows in enumerate(image.rows):
        mc = pu // spec.pu_per_mc
        state = image.spms[pu][NODE_BASE//4:NODE_BASE//4+len(initial[pu::spec.pu])*8].reshape(-1, 8)
        if not np.array_equal(state, initial[pu::spec.pu]):
            raise ValueError("SPM neuron initialization differs from canonical state")
        for row in rows:
            src = int(row[2])
            post, weight, address = image.forward(pu, row)
            if np.any(~np.isfinite(weight)) or np.any(weight < 0):
                raise ValueError("invalid plastic weight")
            np.add.at(incoming[0], post, 1)
            if len(post):
                forward_rows[mc].append((int(row[3]), len(post), src))
                plastic += len(post)
                moments.add(weight)
            if compare_graph and src < spec.ne:
                plastic_by_source.setdefault(src, []).append((post.copy(), weight.copy()))
            directory = image.directory_entries(mc, int(row[5]))
            static_posts = []
            for target_mc, (base, count) in enumerate(directory):
                entries = image.read(target_mc, int(base), int(count)*2).reshape(-1, 2)
                packed, w = entries[:, 0], entries[:, 1]
                dst, offset = packed & 65535, packed >> 16
                field = 20 if src < spec.ne else 24
                if np.any(dst < 1) or np.any(dst > spec.pu) or np.any((dst-1)//spec.pu_per_mc != target_mc) or np.any(offset < NODE_BASE+field) or np.any((offset-NODE_BASE-field) % NODE_BYTES):
                    raise ValueError("bad static scatter target")
                target = (offset-NODE_BASE-field)//NODE_BYTES*spec.pu+dst-1
                if np.any(target >= spec.n) or np.any(target == src) or (src < spec.ne and np.any(target < spec.ne)):
                    raise ValueError("bad static population/autapse")
                expected = F(c["epsc"] * c["external_weight" if src < spec.ne else "inhibitory_weight"]).view(np.uint32)
                if np.any(w != expected):
                    raise ValueError("bad static weight")
                np.add.at(incoming[0 if src < spec.ne else 1], target, 1)
                static += int(count)
                if compare_graph:
                    static_posts.append(target.copy())
            if compare_graph:
                static_by_source.setdefault(src, []).extend(static_posts)
            if src % spec.pu == pu and src < spec.ne:
                for target_mc, (base, count) in enumerate(image.directory_entries(mc, int(row[6]))):
                    reverse_rows[target_mc].append((src, int(base), int(count)))
        # All proxies for a group have now been visited; bound comparison RAM
        # to one group's outgoing rows rather than the complete network.
        if compare_graph and pu % image.group == image.group-1:
            for src, parts in plastic_by_source.items():
                observed_post = np.concatenate([p for p, _ in parts])
                observed_weight = np.concatenate([w for _, w in parts])
                lo, hi = cuts["ee"][src:src+2]
                expected_post = graphs["ee"][1][lo:hi]
                raw = raw_weights[lo:hi] if raw_weights is not None else np.full(hi-lo, F(45.61))
                expected_weight = (raw.astype(np.float64) * float(c["epsc"])).astype(F)
                a = np.lexsort((observed_weight.view(np.uint32), observed_post))
                b = np.lexsort((expected_weight.view(np.uint32), expected_post))
                if not np.array_equal(observed_post[a], expected_post[b]) or not np.array_equal(observed_weight[a], expected_weight[b]):
                    raise ValueError("plastic graph/parallel-edge weights differ from source")
            for src, parts in static_by_source.items():
                observed = np.concatenate(parts) if parts else np.empty(0, np.uint32)
                names = ("ei",) if src < spec.ne else ("ie", "ii")
                expected_parts = []
                for name in names:
                    local = src if src < spec.ne else src-spec.ne
                    lo, hi = cuts[name][local:local+2]
                    expected_parts.append(graphs[name][1][lo:hi] + (spec.ne if name[1] == "i" else 0))
                if not np.array_equal(np.sort(observed), np.sort(np.concatenate(expected_parts))):
                    raise ValueError("static graph differs from source")
            plastic_by_source.clear()
            static_by_source.clear()
    if np.any(incoming[0] != spec.ce) or np.any(incoming[1] != spec.ci):
        raise ValueError("decoded image indegrees differ from the model")
    reverse = 0
    for mc, row_list in enumerate(forward_rows):
        table = np.asarray(sorted(row_list), dtype=np.uint64).reshape(-1, 3)
        counts = table[:, 1]
        offsets = np.concatenate((np.zeros(1, dtype=np.uint64), np.cumsum(counts)))
        seen = np.zeros(int(offsets[-1]), dtype=bool)
        for post, base, count in reverse_rows[mc]:
            entries = image.read(mc, base, count*2).reshape(-1, 2)
            packed, address = entries[:, 0], entries[:, 1]
            dst, index = packed & 65535, packed >> 16
            if not count:
                continue
            if np.any(address % 4) or np.any(dst < 1) or np.any(dst > spec.pu) or np.any((dst-1)//spec.pu_per_mc != mc) or np.any((index & 15) != mc):
                raise ValueError("bad reverse destination/weight MC")
            neuron_offset = index & 0xFFF0
            if np.any(neuron_offset < NODE_BASE) or np.any((neuron_offset-NODE_BASE) % NODE_BYTES):
                raise ValueError("bad reverse neuron offset")
            src = (neuron_offset-NODE_BASE)//NODE_BYTES*spec.pu+dst-1
            row_index = np.searchsorted(table[:, 0], address, side="right")-1
            if np.any(row_index < 0):
                raise ValueError("reverse address precedes plastic storage")
            relative = address.astype(np.uint64)-table[row_index, 0]
            within = relative//1024*128+(relative%1024)//4
            if np.any(relative%1024 >= 512) or np.any(within >= counts[row_index]) or np.any(table[row_index, 2] != src):
                raise ValueError("reverse does not address the named source's weight")
            logical = (offsets[row_index]+within).astype(np.int64)
            if len(np.unique(logical)) != len(logical) or np.any(seen[logical]):
                raise ValueError("duplicate reverse reference to a plastic edge")
            seen[logical] = True
            stored = counts[row_index] + int(image.bulk)
            delta = np.where(within < stored//128*128, 512, (stored%128+7)//8*32)
            target = image.memory[mc][(address.astype(np.uint64)+delta)//4]
            target_gid = ((target>>16)-NODE_BASE-20)//NODE_BYTES*spec.pu+(target&65535)-1
            if np.any(target_gid != post):
                raise ValueError("reverse-to-forward postsynaptic identity mismatch")
            reverse += count
        if not np.all(seen):
            raise ValueError("missing reverse reference")
    if plastic != spec.ne*spec.ce or reverse != plastic or static != spec.n*(spec.ce+spec.ci)-plastic:
        raise ValueError("edge count mismatch")
    total = sum(image.manifest["image_bytes"])
    return dict(plastic=plastic, static=static, reverse=reverse, bytes=total,
                image_bytes=image.manifest["image_bytes"],
                per_mc_remaining_bytes=[MC_BYTES-size for size in image.manifest["image_bytes"]],
                weight_mean=moments.mean, weight_std=moments.std)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", type=Path)
    parser.add_argument("--skip-graph-comparison", action="store_true")
    args = parser.parse_args()
    print(json.dumps(check_images(args.images, not args.skip_graph_comparison), indent=2))


if __name__ == "__main__":
    main()
