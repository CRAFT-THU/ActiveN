"""Native functional cache bridge shared by image generation and policy sweeps.

Each PU processes forward rows in neuron/row order. Reverse events are fired
posts in gid/CSR order (LTD then LTP per connection), followed by recorded
non-fired posts in forward delivery order. This is a legal deterministic event
order, not a prediction of the NoC's interleaving across sources.
"""

import ctypes as C
from pathlib import Path
import numpy as np

METRICS = (
    "hits",
    "misses",
    "tag_hits",
    "probes",
    "inserts",
    "evictions",
    "clean_evictions",
    "dirty_evictions",
    "pending_evictions",
    "write_words",
    "weight_line_loads",
    "pending_ops",
    "forced",
    "bypasses",
    "insert_probes",
    "insert_max",
    "forward_weights",
    "updates",
)
POLICIES = {
    1: "bounded",
    2: "shifted",
    3: "four_candidate_useful",
    4: "bounded_useful",
    5: "element_bounded_useful",
}
U32 = np.dtype("uint32")


def ptr(a):
    assert a.flags.c_contiguous
    return a.ctypes.data_as(C.c_void_p)


class CacheModel:
    def __init__(self, library, images, rule, wmax, policy=1, shift=2, pus=512):
        self.lib = C.CDLL(str(Path(library).resolve()))
        self.lib.bc_create.argtypes = [C.c_uint] * 4 + [C.c_void_p] * 2 + [C.c_uint]
        self.lib.bc_create.restype = C.c_void_p
        self.lib.bc_destroy.argtypes = [C.c_void_p]
        self.lib.bc_forward.argtypes = [C.c_void_p, C.c_void_p, C.c_uint, C.c_void_p]
        self.lib.bc_apply.argtypes = [C.c_void_p, C.c_void_p, C.c_uint]
        self.lib.bc_logical.argtypes = [C.c_void_p] * 3 + [C.c_uint, C.c_void_p]
        self.lib.bc_export.argtypes = [C.c_void_p] * 5
        self.drams = [np.frombuffer(image, dtype=U32).copy() for image in images]
        pointers = (C.c_void_p * len(self.drams))(*[a.ctypes.data for a in self.drams])
        sizes = np.array([a.size for a in self.drams], dtype=np.uint64)
        self.handle = self.lib.bc_create(
            policy,
            shift,
            rule == "morrison",
            int(np.float32(wmax).view(U32)),
            pointers,
            ptr(sizes),
            pus,
        )
        if not self.handle:
            raise ValueError("invalid cache model configuration")
        self.policy, self.shift, self.pus = policy, shift, pus

    def close(self):
        if self.handle:
            self.lib.bc_destroy(self.handle)
            self.handle = None

    def forward(self, commands, count):
        commands = np.ascontiguousarray(commands, dtype=U32).reshape(-1, 4)
        out = np.empty(count, dtype=U32)
        self.lib.bc_forward(self.handle, ptr(commands), len(commands), ptr(out))
        return out.view(np.float32)

    def apply(self, commands):
        commands = np.ascontiguousarray(commands, dtype=U32).reshape(-1, 4)
        self.lib.bc_apply(self.handle, ptr(commands), len(commands))

    def logical(self, pus, addresses):
        pus, addresses = [np.ascontiguousarray(x, dtype=U32) for x in (pus, addresses)]
        out = np.empty(len(addresses), dtype=U32)
        self.lib.bc_logical(self.handle, ptr(pus), ptr(addresses), len(out), ptr(out))
        return out.view(np.float32)

    def export(self):
        entries = np.empty((self.pus, 800, 2), dtype=U32)
        useful = np.empty((self.pus, 800), dtype=np.uint8)
        stats = np.empty((self.pus, len(METRICS)), dtype=np.uint64)
        chains = np.empty((self.pus, 3202), dtype=np.uint64)
        self.lib.bc_export(self.handle, *map(ptr, (entries, useful, stats, chains)))
        return dict(entries=entries, usefulness=useful, stats=stats, chains=chains)

    def bind(self, ref, addresses):
        self.addresses = addresses
        self.owners = (ref.pre % self.pus).astype(U32)

    def forward_spikes(self, ref, old, plastic):
        commands = []
        offset = 0
        for pre in np.flatnonzero(old[: len(ref.po) - 1]):
            lo, hi = ref.po[pre : pre + 2]
            for i in range(lo, hi, 8):
                count = min(8, hi - i)
                commands.append((pre % self.pus, self.addresses[i], count, offset))
                offset += count
        assert offset == len(plastic)
        return self.forward(commands, offset)

    def learn(self, ref, old, plastic, potentiated):
        f = ref.n.view(np.float32)
        # Fired-post scatter includes all connections, including presynaptic
        # neurons that did not fire this step. Zero updates do not touch cache.
        q = potentiated
        commands = np.zeros((len(q), 2, 4), dtype=U32)
        commands[:, :, 0] = self.owners[q, None]
        commands[:, :, 1] = self.addresses[q, None]
        commands[:, 0, 2] = np.where(
            old[ref.pre[q]], np.float32(ref.c["dep"] * f[ref.post[q], 7]), np.float32(0)
        ).view(U32)
        commands[:, 1, 2] = np.float32(ref.c["pot"] * f[ref.pre[q], 6]).view(U32)
        commands[:, 1, 3] = 1
        self.apply(commands)
        q = plastic[ref.n[ref.post[plastic], 9] == 0]
        commands = np.zeros((len(q), 4), dtype=U32)
        commands[:, 0] = self.owners[q]
        commands[:, 1] = self.addresses[q]
        commands[:, 2] = np.float32(ref.c["dep"] * f[ref.post[q], 7]).view(U32)
        self.apply(commands)
        touched = np.union1d(plastic, potentiated)
        ref.w[touched] = self.logical(self.owners[touched], self.addresses[touched])


def metrics_delta(before, after):
    """Per-PU counters; maxima come from the interval histogram, not subtraction."""
    delta = after["stats"] - before["stats"]
    hist = after["chains"] - before["chains"]
    delta[:, METRICS.index("insert_max")] = np.max(
        np.where(hist != 0, np.arange(hist.shape[1]), 0), axis=1
    )
    return delta


def summary(snapshot):
    tags = snapshot["entries"][:, :, 0]
    valid = tags != 0
    return dict(
        occupied=int(valid.sum()),
        resident_entries=int((valid & ((tags & 3) < 2)).sum()),
        pending_coefficients=int((valid & ((tags & 3) >= 2)).sum()),
        dirty_weights=int((valid & ((tags & 3) == 1)).sum()),
        occupancy_fraction=float(valid.mean()),
    )
