#!/usr/bin/env python3
"""Compare five caches on identical Brunel events with exact logical weights.

Traffic is counted per timestep; this functional experiment does not estimate
cycles. All policies see the same deterministic, legal event order.
"""

import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from brunel import Reference, graph, weight_layout
from brunel_cache import CacheModel, METRICS, POLICIES, metrics_delta, summary


class Comparison:
    def __init__(self, models):
        self.models = models

    def forward_spikes(self, ref, old, plastic):
        want = self.models[0].forward_spikes(ref, old, plastic)
        for model in self.models[1:]:
            got = model.forward_spikes(ref, old, plastic)
            np.testing.assert_array_equal(got, want)
        return want

    def learn(self, ref, old, plastic, potentiated):
        touched = np.union1d(plastic, potentiated)
        self.models[0].learn(ref, old, plastic, potentiated)
        want = ref.w[touched].copy()
        for model in self.models[1:]:
            model.learn(ref, old, plastic, potentiated)
            np.testing.assert_array_equal(ref.w[touched], want)


def window(rows, chains, start, end):
    a = rows[start:end]
    totals = a.sum(axis=(0, 1), dtype=np.uint64)
    metrics = {
        name: float(totals[i]) / len(a)
        for i, name in enumerate(METRICS)
        if name != "insert_max"
    }
    demands = metrics["hits"] + metrics["misses"]
    distribution = {}
    for name in ("probes", "weight_line_loads", "write_words", "evictions", "updates"):
        v = a[:, :, METRICS.index(name)]
        distribution[name] = dict(
            mean_per_pu_per_timestep=float(v.mean()),
            p50=float(np.percentile(v, 50)),
            p95=float(np.percentile(v, 95)),
            p99=float(np.percentile(v, 99)),
            maximum=int(v.max()),
        )
    hist = chains[end] - chains[start]
    nonzero = np.flatnonzero(hist)

    def quantile(q):
        return (
            int(np.searchsorted(np.cumsum(hist), q * hist.sum())) if hist.sum() else 0
        )

    return dict(
        steps=[start + 1, end],
        average_per_timestep=metrics,
        resident_hit_rate=metrics["hits"] / demands if demands else None,
        tag_hit_rate=metrics["tag_hits"] / demands if demands else None,
        per_pu_timestep_distribution=distribution,
        insertion_probes=dict(
            mean=metrics["insert_probes"] / (metrics["inserts"] + metrics["bypasses"])
            if metrics["inserts"] + metrics["bypasses"]
            else 0,
            p50=quantile(0.5),
            p99=quantile(0.99),
            maximum=int(nonzero[-1]) if len(nonzero) else 0,
            histogram_average_per_timestep={
                str(i): float(hist[i]) / len(a) for i in nonzero
            },
        ),
    )


def run(args):
    if args.warmup < 0 or args.steps < 1 or not 0 <= args.shift <= 9:
        raise ValueError("invalid window or shift")
    args.output.mkdir(parents=True, exist_ok=False)
    ref = Reference(graph(args.seed), args.rule, seed=args.seed)
    images, addresses = weight_layout(ref)
    models = [
        CacheModel(
            args.library, images, args.rule, ref.c["wmax"], policy=p, shift=args.shift
        )
        for p in range(1, 6)
    ]
    for model in models:
        model.bind(ref, addresses)
    compare = Comparison(models)
    previous = [m.export() for m in models]
    rows = [[] for _ in models]
    states = [[] for _ in models]
    chains = [[p["chains"].sum(axis=0)] for p in previous]
    spikes = []
    for step in range(args.warmup + args.steps):
        spikes.append(ref.tick(compare))
        for i, model in enumerate(models):
            state = model.export()
            rows[i].append(metrics_delta(previous[i], state))
            states[i].append(summary(state))
            chains[i].append(state["chains"].sum(axis=0))
            previous[i] = state
            if step + 1 == args.warmup:
                # A complete, quiescent checkpoint for each offline policy.
                np.savez_compressed(
                    args.output / f"policy-{i + 1}-hot.npz",
                    **state,
                    neurons=ref.n,
                    weights=ref.w,
                    addresses=addresses,
                    policy=i + 1,
                    shift=args.shift,
                    **{f"dram_{mc}": d for mc, d in enumerate(model.drams)},
                )
        if (step + 1) % 30 == 0:
            print(
                "step",
                step + 1,
                "E/I_per_timestep",
                np.mean(spikes[-30:], axis=0).tolist(),
                "occupancy",
                [round(s[-1]["occupancy_fraction"], 3) for s in states],
                flush=True,
            )
    result = dict(
        cache_model_sha256=hashlib.sha256(args.library.read_bytes()).hexdigest(),
        rule=args.rule,
        seed=args.seed,
        warmup=args.warmup,
        steps=args.steps,
        slots_per_pu=800,
        useful_sidecar_bytes_per_pu=200,
        shifted_hash_shift=args.shift,
        event_order="forward pre/row order; fired-post gid/CSR order LTD then LTP; manual replay forward order",
        verification="Every forward weight and every updated logical weight is bit-identical across all five policies at every timestep",
        graph_sha256=hashlib.sha256(
            b"".join(x.tobytes() for x in (ref.pre, ref.post, ref.sp, ref.st))
        ).hexdigest(),
        spikes=spikes,
        policies={},
    )
    for i, model in enumerate(models):
        r, h = np.array(rows[i]), np.array(chains[i])
        np.savez_compressed(
            args.output / f"policy-{i + 1}-measurements.npz",
            per_pu_per_step=r,
            insertion_histogram_prefix=h,
            metrics=np.array(METRICS),
        )
        result["policies"][str(i + 1)] = dict(
            name=POLICIES[i + 1],
            measured=window(r, h, args.warmup, len(r)),
            blocks=[
                window(r, h, start, min(start + 30, len(r)))
                for start in range(0, len(r), 30)
            ],
            state_per_step=states[i],
        )
        model.close()
    (args.output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print("wrote", args.output / "report.json", flush=True)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--rule", choices=("additive", "morrison"), required=True)
    p.add_argument("--seed", type=int, default=20260724)
    p.add_argument("--warmup", type=int, default=1000)
    p.add_argument("--steps", type=int, default=300)
    p.add_argument("--shift", type=int, default=2)
    run(p.parse_args())
