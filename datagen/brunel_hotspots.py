#!/usr/bin/env python3
"""Per-PU Brunel work, checked against both runtime counters and STDP traces."""

import argparse
import csv
import json
from pathlib import Path
import numpy as np
from brunel import F, NE, PU, constants
from brunel_validate import dump


def analyze(images, dumps, start, steps, output):
    manifest = json.loads((images / "manifest.json").read_text())
    buffered = manifest.get("kernel") == "timestep-buffer-mshr"
    initial = np.load(images / "initial.npz")
    pre, post = initial["pre"], initial["post"]
    owner = pre % PU
    c = constants(manifest["rule"])
    if (
        start < 1
        or steps < 1
        or start + steps - 1 > manifest["warmup"] + manifest["timed"]
    ):
        raise ValueError("window outside generated workload")
    if start > 1:
        previous_spm, previous = dump(dumps / f"step-{start - 1:04}.bin")
        previous_counts = np.array([s[30:38] for s in previous_spm], dtype=np.int64)
        previous_pending = np.array(
            [
                [np.count_nonzero((s[576:2176:2] & 3) == state) for state in (2, 3)]
                for s in previous_spm
            ]
        )
    else:
        previous = initial["neurons"]
        previous_counts = np.zeros((PU, 8), dtype=np.int64)
        previous_pending = np.zeros((PU, 2), dtype=np.int64)
        if manifest.get("cache_initial"):
            tags = np.load(images / "cache-initial.npz")["entries"][:, :, 0]
            previous_pending = np.column_stack(
                [np.count_nonzero((tags & 3) == state, axis=1) for state in (2, 3)]
            )
    if buffered:
        previous_pending[:] = 0
    rows, per_step = [], []

    def count(mask):
        return np.bincount(owner[mask], minlength=PU)

    for step in range(start, start + steps):
        spms, n = dump(dumps / f"step-{step:04}.bin")
        counts = np.array([s[30:38] for s in spms], dtype=np.int64)
        delta = counts - previous_counts
        x = F(previous[:, 6].view(np.float32) * c["decay_pre"])
        y = F(previous[:, 7].view(np.float32) * c["decay_post"])
        fire_field = 9 if manifest.get("firing_flag") == "N_FIRE" else 10
        forward = n[pre, fire_field] != 0
        ltd = forward & (F(c["dep"] * y[post]) != 0)
        ltp = (n[post, 9] != 0) & (F(c["pot"] * x[pre]) != 0)
        reads, down, up, unique = map(count, (forward, ltd, ltp, ltd | ltp))
        updates = down + up
        pending = np.array(
            [
                [np.count_nonzero((s[576:2176:2] & 3) == state) for state in (2, 3)]
                for s in spms
            ]
        )
        if buffered:
            pending[:] = 0
        # Each demand applies once or adds one pending coefficient; materializing
        # an older coefficient removes it. Conservation counts actual arithmetic.
        applied = np.column_stack((down, up)) - (pending - previous_pending)
        if np.any(applied < 0):
            raise ValueError("invalid pending-update conservation")
        # Runtime cache demand = scalar forward reads + nonzero update operations.
        if buffered:
            in_buffer = n[pre, 19] != 0
            np.testing.assert_array_equal(delta[:, 6], reads)
            np.testing.assert_array_equal(
                delta[:, 0], count(ltd & in_buffer) + count(ltp & in_buffer)
            )
            np.testing.assert_array_equal(delta[:, 1], count((ltd | ltp) & ~in_buffer))
            np.testing.assert_array_equal(delta[:, 1], delta[:, 2])
        else:
            np.testing.assert_array_equal(delta[:, 0] + delta[:, 1] - reads, updates)
        pre_fired = np.bincount(
            np.arange(NE) % PU, weights=n[:NE, fire_field], minlength=PU
        ).astype(int)
        arrivals = np.bincount(post[forward] % PU, minlength=PU)
        for pu in range(PU):
            rows.append(
                dict(
                    step=step,
                    pu=pu + 1,
                    LTD=int(down[pu]),
                    LTP=int(up[pu]),
                    updates=int(updates[pu]),
                    applied_updates=int(applied[pu].sum()),
                    applied_LTD=int(applied[pu, 0]),
                    applied_LTP=int(applied[pu, 1]),
                    distinct_updated=int(unique[pu]),
                    forward_weights=int(reads[pu]),
                    pre_fired=int(pre_fired[pu]),
                    received_plastic_spikes=int(arrivals[pu]),
                    reverse_deliveries=int(delta[pu, 7]),
                    probes=0 if buffered else int(delta[pu, 6]),
                    evictions=0 if buffered else int(delta[pu, 3]),
                    **(
                        dict(
                            buffer_updates=int(delta[pu, 0]),
                            mshr_loads=int(delta[pu, 1]),
                            overflow_rows=int(delta[pu, 3]),
                            pool_full=int(delta[pu, 5]),
                            buffer_bytes=int(spms[pu][38]),
                        )
                        if buffered
                        else {}
                    ),
                    stores=int(delta[pu, 4]),
                )
            )
        top = int(updates.argmax())
        per_step.append(
            dict(
                step=step,
                mean_updates_per_pu=float(updates.mean()),
                p50_updates_per_pu=float(np.percentile(updates, 50)),
                p95_updates_per_pu=float(np.percentile(updates, 95)),
                max_updates=int(updates[top]),
                busiest_pu=top + 1,
                busiest_pre_fired=int(pre_fired[top]),
                busiest_forward_weights=int(reads[top]),
                busiest_LTD=int(down[top]),
                busiest_LTP=int(up[top]),
            )
        )
        previous, previous_counts, previous_pending = n, counts, pending
    fields = [k for k in rows[0] if k not in ("step", "pu")]
    matrices = {k: np.array([r[k] for r in rows]).reshape(steps, PU) for k in fields}
    updates = matrices["updates"]
    active = updates != 0
    worst = max(rows, key=lambda r: r["updates"])
    mean_pu = updates.mean(axis=0)
    result = dict(
        rule=manifest["rule"],
        steps=[start, start + steps - 1],
        definition="Nonzero LTD/LTP operations; rounding/clipping may leave a weight unchanged. Parallel edges are distinct.",
        verification="Every PU/timestep agrees with forward, buffered-update, and MSHR counters."
        if buffered
        else "Every PU/timestep agrees with runtime (hits + misses - forward weights).",
        average_per_pu_per_timestep={k: float(v.mean()) for k, v in matrices.items()},
        average_system_per_timestep={
            k: float(v.sum(axis=1).mean()) for k, v in matrices.items()
        },
        update_distribution=dict(
            p50=float(np.percentile(updates, 50)),
            p95=float(np.percentile(updates, 95)),
            p99=float(np.percentile(updates, 99)),
            maximum=int(updates.max()),
            active_pu_timestep_fraction=float(active.mean()),
            mean_per_active_pu_timestep=float(updates[active].mean())
            if active.any()
            else 0,
            mean_of_per_step_maximum=float(updates.max(axis=1).mean()),
        ),
        worst_pu_timestep=worst,
        worst_forward_pu_timestep=max(rows, key=lambda r: r["forward_weights"]),
        mean_of_per_step_maximum_forward_weights=float(
            matrices["forward_weights"].max(axis=1).mean()
        ),
        worst_applied_pu_timestep=max(rows, key=lambda r: r["applied_updates"]),
        mean_of_per_step_maximum_applied_updates=float(
            matrices["applied_updates"].max(axis=1).mean()
        ),
        busiest_average_pu=int(mean_pu.argmax() + 1),
        busiest_pu_average_updates=float(mean_pu.max()),
        per_step=per_step,
    )
    output.write_text(json.dumps(result, indent=2) + "\n")
    with output.with_suffix(".csv").open("w") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps({k: v for k, v in result.items() if k != "per_step"}, indent=2))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--images", type=Path, required=True)
    p.add_argument("--dumps", type=Path, required=True)
    p.add_argument("--start", type=int, default=31)
    p.add_argument("--steps", type=int, default=30)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    analyze(a.images, a.dumps, a.start, a.steps, a.output)
