#!/usr/bin/env python3
"""Sparse Brunel images and independent FP32 reference.

Graph generation follows the validated GeNN model (fixed indegree, replacement,
no recurrent autapses), with a recorded NumPy realization. The runtime integrates
first, delivers spikes and quiet LTD, then performs post-active learning. CPU
presimulation uses a per-neuron RNG; runtime workers use per-SMT RNGs. Validation
infers the observed external samples to compare deterministic state transitions.
"""

from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import numpy as np

NE, NI, PU, MC, LINE = 9000, 2250, 512, 16, 32
MAGIC = 0x42524E34
NODE_BASE, DATA_END = 256, 0x3F00
PLASTIC_CHUNK = 128
POISSON_BASE, STREAM_BASE = 0x2DA4, 0x2DD0
F = np.float32


def poisson_thresholds(mean):
    """Inverse-CDF cutoffs for one uniform U32; the tiny >=9 tail maps to 9."""
    probability = math.exp(-float(mean))
    cumulative = probability
    cutoffs = []
    for k in range(9):
        if k:
            probability *= float(mean) / k
            cumulative += probability
        cutoffs.append(min(2**32 - 1, int(cumulative * 2**32)))
    return np.array(cutoffs, dtype=np.uint32)


def constants(rule):
    dt, tau, tm, cm = 0.1, 0.32582722403722841, 10.0, 250.0
    beta = tau * tm / (tm - tau)
    decay = math.exp(-dt / tau)
    ex = math.expm1(dt * (tm - tau) / (tau * tm))
    je = 45.609600316540956
    eta, scale, ratio = (1.685, 0.47, 5.0) if rule == "additive" else (3.2, 0.32, 8.0)
    rate = 20 / (450 * tm / cm * je * math.e * tau) * eta * 450 * scale * 1000
    syn_scale = math.e / tau * math.sqrt(20)
    learn_scale = syn_scale if rule == "additive" else syn_scale ** 0.6
    return dict(
        p11=F(decay),
        p21=F(dt * decay),
        p31=F(beta / cm * decay * (beta * ex - dt)),
        p32=F(beta / cm * decay * ex),
        p33=F(math.exp(-dt / tm)),
        epsc=F(math.e / tau),
        delivery=F(math.sqrt(20)),
        external_mean=F(rate * dt / 1000),
        external_limit=F(math.exp(-rate * dt / 1000)),
        external_weight=F(je),
        # The scaled matrix is used directly during delivery.  Morrison LTD
        # is proportional to the current scaled weight, so its coefficient is
        # not scaled; only the nonlinear LTP coefficient carries S**0.6.
        dep=F(0.01 * 1.05 * 2 * je * learn_scale if rule == "additive" else 0.1 * 0.1026),
        pot=F((0.01 * 2 * je if rule == "additive" else 0.1) * learn_scale),
        wmax=F(2 * je * syn_scale),
        decay_pre=F(math.exp(-dt / 20)),
        decay_post=F(math.exp(-dt / 30)),
        inhibitory_weight=F(-ratio * je),
    )


def graph(seed):
    rng = np.random.default_rng(seed)

    def edges(npre, npost, k, prebase, postbase):
        post = np.repeat(np.arange(npost, dtype=np.int32), k)
        pre = rng.integers(npre - (prebase == postbase), size=len(post), dtype=np.int32)
        if prebase == postbase:
            pre += pre >= post
        return pre + prebase, post + postbase

    pre, post = edges(NE, NE, 450, 0, 0)
    order = np.argsort(pre, kind="stable")
    pre, post = pre[order], post[order]
    parts = [
        edges(NE, NI, 450, 0, NE),
        edges(NI, NE, 112, NE, 0),
        edges(NI, NI, 112, NE, NE),
    ]
    sp = np.concatenate([x[0] for x in parts])
    st = np.concatenate([x[1] for x in parts])
    order = np.argsort(sp, kind="stable")
    sp, st = sp[order], st[order]
    return pre, post, sp, st


def offsets(indices, n):
    return np.concatenate(([0], np.cumsum(np.bincount(indices, minlength=n)))).astype(
        np.int64
    )


def ranges(ids, off):
    return (
        np.concatenate([np.arange(off[i], off[i + 1]) for i in ids])
        if len(ids)
        else np.empty(0, dtype=np.int64)
    )


def rng_step(r):
    r ^= r << np.uint32(13)
    r ^= r >> np.uint32(17)
    r ^= r << np.uint32(5)
    return r


def poisson(r, limit):
    p = np.ones(len(r), dtype=np.float32)
    counts = np.zeros(len(r), dtype=np.uint32)
    active = np.arange(len(r))
    while len(active):
        rr = rng_step(r[active].copy())
        r[active] = rr
        u = F((rr >> 8).astype(np.float32) + F(1)) * F(2**-24)
        p[active] = F(p[active] * u)
        counts[active] += 1
        active = active[p[active] > limit]
    return counts - 1


class Reference:
    def __init__(self, edges, rule, state=None, seed=20260724):
        self.pre, self.post, self.sp, self.st = edges
        self.c = constants(rule)
        self.rule = rule
        self.po = offsets(self.pre, NE)
        self.so = offsets(self.sp, NE + NI)
        self.reverse = np.argsort(self.post, kind="stable")
        self.ro = offsets(self.post, NE)
        if state is None:
            rng = np.random.default_rng(seed)
            self.n = np.zeros((NE + NI, 20), dtype=np.uint32)
            self.n[:, 0] = (
                rng.normal(5.7, 7.2, NE + NI).astype(np.float32).view(np.uint32)
            )
            self.n[:, 8] = rng.integers(1, 2**32, size=NE + NI, dtype=np.uint32)
            self.n[:, 11] = np.arange(NE + NI)
            self.w = np.full(len(self.pre), F(self.c["external_weight"] * self.c["epsc"] * self.c["delivery"]), dtype=np.float32)
        else:
            self.n, self.w = state[0].copy(), state[1].copy()

    def tick(self, cache=None, external=None):
        n = self.n
        f = n.view(np.float32)
        c = self.c
        old = n[:, 9].copy()
        # The hardware integrates first, then forwards the spikes produced by
        # this integration.  Input fields are the one-step delivery buffer.
        ei = f[:, 17].copy()
        ii = f[:, 18].copy()
        f[:, 17] = F(0)
        f[:, 18] = F(0)
        v = F(
            F(F(F(c["p31"] * f[:, 2]) + F(c["p32"] * f[:, 1])) + F(c["p31"] * f[:, 4]))
            + F(c["p32"] * f[:, 3])
        )
        v = F(v + F(c["p33"] * f[:, 0]))
        free = n[:, 5] == 0
        f[free, 0] = v[free]
        n[~free, 5] -= 1
        f[:, 1] = F(F(c["p21"] * f[:, 2]) + F(c["p11"] * f[:, 1]))
        f[:, 3] = F(F(c["p21"] * f[:, 4]) + F(c["p11"] * f[:, 3]))
        f[:, 2] = F(F(c["p11"] * f[:, 2]) + ei)
        f[:, 4] = F(F(c["p11"] * f[:, 4]) + ii)
        ext = poisson(n[:, 8], c["external_limit"]) if external is None else external
        f[:, 2] = F(
            f[:, 2] + F(F(c["epsc"] * c["external_weight"]) * ext.astype(np.float32))
        )
        fired = (n[:, 5] == 0) & (f[:, 0] >= 20)
        n[:, 9] = fired
        n[:, 10] = fired
        f[fired, 0] = 0
        n[fired, 5] = 5
        f[:, 6] = F(f[:, 6] * c["decay_pre"])
        f[:, 7] = F(f[:, 7] * c["decay_post"])
        plastic = ranges(np.flatnonzero(fired[:NE]), self.po)
        if cache is not None:
            self.w[plastic] = cache.forward_spikes(self, fired, plastic)
        static = ranges(np.flatnonzero(fired), self.so)
        np.add.at(f[:, 17], self.post[plastic], self.w[plastic])
        exc = self.sp[static] < NE
        np.add.at(f[:, 17], self.st[static[exc]], F(c["external_weight"] * c["epsc"] * c["delivery"]))
        np.add.at(f[:, 18], self.st[static[~exc]], F(c["inhibitory_weight"] * c["epsc"] * c["delivery"]))
        potentiated = self.reverse[ranges(np.flatnonzero(fired[:NE]), self.ro)]
        if cache is not None:
            cache.learn(self, fired, plastic, potentiated)
            f[:, 6] = F(f[:, 6] + fired.astype(np.float32))
            f[:, 7] = F(f[:, 7] + fired.astype(np.float32))
            return [int(fired[:NE].sum()), int(fired[NE:].sum())]
        dep = F(c["dep"] * f[self.post[plastic], 7])
        w = self.w[plastic]
        if self.rule == "morrison":
            dep = F(dep * w)
        self.w[plastic] = np.maximum(F(0), F(w - dep))
        pot = F(c["pot"] * f[self.pre[potentiated], 6])
        w = self.w[potentiated]
        if self.rule == "morrison":
            # The numerical target is mathematical 2/5. A binary32 exponent
            # 0.4f is a different function at extreme FP32 weights.
            pot = F(pot * F(np.power(w.astype(np.float64), 2 / 5)))
        self.w[potentiated] = F(w + pot)
        if self.rule == "additive":
            np.minimum(self.w, c["wmax"], out=self.w)
        f[:, 6] = F(f[:, 6] + fired.astype(np.float32))
        f[:, 7] = F(f[:, 7] + fired.astype(np.float32))
        return [int(fired[:NE].sum()), int(fired[NE:].sum())]


def weight_layout(ref):
    """Stable MC-local addresses, independent of payload and cache policy."""
    images = [bytearray(65536) for _ in range(MC)]
    addresses = np.zeros(len(ref.pre), dtype=np.uint32)
    for src in range(NE):
        lo, hi = ref.po[src : src + 2]
        count = hi - lo
        mc = (src % PU) // 32
        image = images[mc]
        base = len(image)
        ref.n[src, 12] = base
        ref.n[src, 13] = count
        # A manual AC transfer is one 1 KiB half: 128 weight words followed
        # by 128 packed destination words.  The final chunk is padded so every
        # chunk has the same address arithmetic in the payload.
        for chunk in range(0, count, PLASTIC_CHUNK):
            n = min(PLASTIC_CHUNK, count - chunk)
            w = np.pad(
                ref.w[lo + chunk : lo + chunk + n],
                (0, PLASTIC_CHUNK - n),
            )
            destinations = (
                (NODE_BASE + (ref.post[lo + chunk : lo + chunk + n] // PU) * 80 + 68).astype(np.uint32)
                << 16
            ) | ((ref.post[lo + chunk : lo + chunk + n] % PU) + 1).astype(np.uint32)
            d = np.pad(destinations, (0, PLASTIC_CHUNK - n))
            image.extend(w.tobytes())
            image.extend(d.tobytes())
            addresses[lo + chunk : lo + chunk + n] = (
                base + (chunk // PLASTIC_CHUNK) * (2 * PLASTIC_CHUNK * 4)
                + np.arange(n, dtype=np.uint32) * 4
            )
        # N_TARGETS is retained in the neuron ABI as the row storage base;
        # the target half is selected by the chunk arithmetic in the payload.
        ref.n[src, 14] = base
    return images, addresses


def write_weights(images, ref):
    for src in range(NE):
        lo, hi = ref.po[src : src + 2]
        base = int(ref.n[src, 12])
        image = images[(src % PU) // 32]
        for chunk in range(0, hi - lo, PLASTIC_CHUNK):
            n = min(PLASTIC_CHUNK, hi - lo - chunk)
            offset = base + (chunk // PLASTIC_CHUNK) * (2 * PLASTIC_CHUNK * 4)
            image[offset : offset + n * 4] = ref.w[lo + chunk : lo + chunk + n].tobytes()


def generate(args):
    if args.warmup < 0 or args.timed < 1 or args.presim < 0:
        raise ValueError("timed must be positive; warmup/presim nonnegative")
    if args.payload is None:
        args.payload = (Path(__file__).resolve().parents[1] / "sim/payloads/sys"
                        / f"brunel_{args.rule}.bin")
    payload = args.payload.read_bytes()
    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    edges = graph(args.seed)
    ref = Reference(edges, args.rule, seed=args.seed)
    images, addresses = weight_layout(ref)
    history = []
    for t in range(args.presim):
        history.append(ref.tick())
        if (t + 1) % 100 == 0:
            print(
                "presim",
                t + 1,
                "spikes_per_timestep",
                np.mean(history[-100:], axis=0).tolist(),
                flush=True,
            )
    if args.seek_active:
        saved = (ref.n.copy(), ref.w.copy())
        # Preserve the historical 30-step activity-selection window even when
        # the hardware warmup is zero. Selection is independent of cache policy.
        selection_warmup = args.warmup or 30
        future = []
        chosen = None
        for extra in range(2000):
            future.append(ref.tick())
            if len(future) >= selection_warmup + args.timed:
                start = len(future) - selection_warmup - args.timed
                if (
                    sum(x[0] for x in future[start : start + selection_warmup]) >= 100
                    and sum(x[0] for x in future[start + selection_warmup :]) >= 100
                ):
                    chosen = start
                    break
        if chosen is None:
            raise RuntimeError("no active warmup+timing window within 200ms")
        ref = Reference(edges, args.rule, state=saved)
        for extra in range(chosen):
            history.append(ref.tick())
        args.presim += chosen
        print("selected active starting step", args.presim, flush=True)
    pre, post, sp, st = edges
    np.savez(
        out / "initial.npz",
        neurons=ref.n,
        weights=ref.w,
        pre=pre,
        post=post,
        static_pre=sp,
        static_post=st,
    )
    initial_n = ref.n.copy()
    initial_w = ref.w.copy()
    write_weights(images, ref)
    if len(payload) + 16 > 0x8000:
        raise ValueError("payload overlaps reserved transport test region")
    images[0][16 : 16 + len(payload)] = payload

    def place(mc, data):
        image = images[mc]
        image.extend(bytes((-len(image)) % LINE))
        offset = len(image)
        image.extend(data)
        return offset

    n = initial_n.copy()

    def csr(sources, targets, data, index, nsource):
        dstpu = (targets % PU) + 1
        dstmc = (dstpu - 1) // 32
        keys = sources.astype(np.int64) * MC + dstmc
        order = np.argsort(keys, kind="stable")
        off = offsets(keys, nsource * MC)
        entries = (
            dstpu.astype(np.uint64)
            | (index.astype(np.uint64) << 16)
            | (data.astype(np.uint64) << 32)
        )
        entries = entries[order]
        dirs = np.zeros((nsource, MC, 2), dtype=np.uint32)
        for src in range(nsource):
            for mc in range(MC):
                lo, hi = off[src * MC + mc : src * MC + mc + 2]
                count = hi - lo
                if count:
                    row = np.pad(entries[lo:hi], (0, (-count) % 4))
                    dirs[src, mc] = (place(mc, row.tobytes()), count)
        return dirs

    c = ref.c
    staticw = np.where(
        sp < NE, F(c["epsc"] * c["delivery"] * c["external_weight"]),
        F(c["epsc"] * c["delivery"] * c["inhibitory_weight"])
    ).astype(np.float32)
    statdirs = csr(
        sp,
        st,
        staticw.view(np.uint32),
        NODE_BASE + (st // PU) * 80 + np.where(sp < NE, 68, 72),
        NE + NI,
    )
    revdirs = csr(post, pre, addresses, NODE_BASE + (pre // PU) * 80, NE)
    for src in range(NE + NI):
        mc = (src % PU) // 32
        n[src, 15] = place(mc, statdirs[src].tobytes())
        if src < NE:
            n[src, 16] = place(mc, revdirs[src].tobytes())
    spms = []
    for pu in range(PU):
        rows = n[pu::PU]
        exc_count = int(np.count_nonzero(rows[:, 11] < NE))
        if np.any(rows[:exc_count, 11] >= NE) or np.any(rows[exc_count:, 11] < NE):
            raise ValueError("runtime requires a contiguous E-neuron prefix per PU")
        header = np.zeros(64, dtype=np.uint32)
        header[:10] = [
            MAGIC,
            pu + 1,
            PU,
            args.rule == "morrison",
            len(rows),
            0,
            0,
            0,
            args.warmup,
            args.warmup + args.timed,
        ]
        header[10] = pu // 32
        for i, key in enumerate(
            (
                "p11",
                "p21",
                "p31",
                "p32",
                "p33",
                "epsc",
                "delivery",
                "external_mean",
                "external_limit",
                "external_weight",
                "dep",
                "pot",
                "wmax",
                "decay_pre",
                "decay_post",
            )
        ):
            header[46 + i] = np.array(c[key], dtype=np.float32).view(np.uint32)
        if np.count_nonzero(rows[:, 11] < NE) > 18:
            raise ValueError("spill bound requires at most 18 E neurons per PU")
        spill_base = place(pu // 32, bytes(65536))
        blob = bytearray(DATA_END)
        struct.pack_into("<I", blob, 0x2D80 + 12, spill_base)
        struct.pack_into("<I", blob, 0x2D80 + 28, 256)
        blob[:256] = header.tobytes()
        blob[NODE_BASE : NODE_BASE + rows.nbytes] = rows.tobytes()
        blob[POISSON_BASE : POISSON_BASE + 36] = poisson_thresholds(c["external_mean"]).tobytes()
        spms.append(blob)
    images[0].extend(bytes((-len(images[0])) % LINE))
    section = len(images[0])
    images[0].extend(bytes(PU * 8))
    for pu, blob in enumerate(spms):
        offset = len(images[0]) - section
        struct.pack_into("<II", images[0], section + pu * 8, offset, len(blob))
        images[0].extend(blob)
    struct.pack_into("<IIII", images[0], 0, 0x0100006F, section, MC, PU)
    for mc, image in enumerate(images):
        (out / f"dram.{mc}").write_bytes(image)
    results = []
    for t in range(args.warmup + args.timed):
        results.append(ref.tick())
        if t in (0, args.warmup - 1, args.warmup + args.timed - 1):
            np.savez(out / f"reference-{t + 1:04}.npz", neurons=ref.n, weights=ref.w)
    manifest = dict(
        rule=args.rule,
        rule_selection="compile-time; boot validates image H_RULE",
        seed=args.seed,
        presim=args.presim,
        kernel="timestep-buffer-mshr",
        plastic_layout="chunk128-weights512-targets512",
        rng="per-SMT xorshift32 inverse-CDF Poisson, capped at 9",
        firing_flag="N_FIRE",
        power="inverse-fifth-root-newton-quadratic",
        forward_carried="source-mc-id",
        reverse_index="neuron-spm-byte-offset",
        fired_list="atomic-append-neuron-pointers",
        current_state="combined",
        trace_state="learning-scaled",
        trace_population="excitatory-only",
        inhibitory_trace_state="zero",
        warmup=args.warmup,
        timed=args.timed,
        neurons=NE + NI,
        plastic=len(pre),
        static=len(sp),
        reference_spikes=results,
        presim_spikes=history,
        constants={k: float(v) for k, v in c.items()},
        image_bytes=list(map(len, images)),
        spm=dict(
            data_end=DATA_END,
            stack_bytes=256,
            stack_bases=[0x3F00, 0x3F80],
            forward_buffer_bytes=0,
            fired_list_offset=0x900,
            fired_list_capacity=25,
            fired_count_offset=212,
            forward_weight_bytes=4,
            mshr_entries=16,
            mshr_entry_bytes=64,
            replay_offset=0x2D80,
            replay_record_bytes=8,
            replay_capacity=256,
            record_offset=0x2F00,
            ping_pong_offset=0x3700,
            ping_pong_bytes=2048,
            poisson_offset=POISSON_BASE,
            stream_offset=STREAM_BASE,
            spill_bytes_per_pu=65536,
        ),
        ordering="alpha-current integration -> forward/static delivery with post-side quiet LTD -> reverse LTP/LTD -> trace increments",
        graph_rng="numpy.default_rng; fixed indegree with replacement, recurrent autapses excluded",
        numpy_version=np.__version__,
        payload_sha256=hashlib.sha256(payload).hexdigest(),
        initial_state_sha256=hashlib.sha256(
            initial_n.tobytes() + initial_w.tobytes()
        ).hexdigest(),
        graph_sha256=hashlib.sha256(b"".join(a.tobytes() for a in edges)).hexdigest(),
        system=dict(
            pus=PU, mcs=MC, memory_line_bytes=LINE, spm_bytes=16384, smt_pipes=2
        ),
    )
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("generated", out, flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--rule", choices=("additive", "morrison"), default="additive")
    p.add_argument("--seed", type=int, default=20260724)
    p.add_argument("--presim", type=int, default=1000)
    p.add_argument("--warmup", type=int, default=30)
    p.add_argument("--timed", type=int, default=30)
    p.add_argument("--seek-active", action="store_true")
    p.add_argument("--payload", type=Path,
                   help="default: sim/payloads/sys/brunel_<rule>.bin")
    generate(p.parse_args())


if __name__ == "__main__":
    main()
