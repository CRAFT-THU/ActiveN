#!/usr/bin/env python3
"""Working sets and temporal reuse from actual Brunel neuron snapshots.

An update is a nonzero LTD/LTP coefficient delivered to a plastic synapse,
including an operation whose final FP32 weight might remain unchanged. Parallel
edges are distinct synapses. Within-step NoC arrival ordering is not inferred.
"""

import argparse
import json
from pathlib import Path
import numpy as np
from brunel import F, NE, PU, constants
from brunel_validate import dump


def working_sets(steps, owner, population, pu_count=PU):
    """Distinct IDs per step; reuse excludes duplicate operations in one step."""
    count = len(steps)
    intervals = np.zeros((count, count, pu_count), np.int32)
    for start in range(count):
        seen = np.zeros(population, bool)
        per_pu = np.zeros(pu_count, np.int32)
        for end in range(start, count):
            ids = steps[end]
            new = ids[~seen[ids]]
            seen[new] = True
            per_pu += np.bincount(owner[new], minlength=pu_count).astype(np.int32)
            intervals[start, end] = per_pu
    windows = []
    for length in (10, 20, 30):
        if length > count:
            continue
        sets = np.array(
            [intervals[i, i + length - 1].sum() for i in range(count - length + 1)]
        )
        first = intervals[0, length - 1]
        windows.append(
            dict(
                timesteps=length,
                prefix_distinct=int(first.sum()),
                prefix_distinct_amortized_per_timestep=float(first.sum() / length),
                prefix_fraction_of_plastic=float(first.sum() / population),
                prefix_per_pu_mean=float(first.mean()),
                prefix_per_pu_p95=float(np.percentile(first, 95)),
                prefix_per_pu_max=int(first.max()),
                sliding_distinct_mean=float(sets.mean()),
                sliding_distinct_min=int(sets.min()),
                sliding_distinct_max=int(sets.max()),
            )
        )
    last = np.full(population, -1, np.int32)
    histogram = np.zeros(count, np.int64)
    lower_sum = upper_sum = reused = 0
    for step, ids in enumerate(steps):
        previous = last[ids]
        for prev in np.unique(previous[previous >= 0]):
            selected = ids[previous == prev]
            weights = np.bincount(owner[selected], minlength=pu_count)
            n = len(selected)
            histogram[step - prev] += n
            reused += n
            if step > prev + 1:
                lower_sum += int(weights @ intervals[prev + 1, step - 1])
            upper_sum += int(weights @ (intervals[prev, step] - 1))
        last[ids] = step
    total = sum(map(len, steps))
    return dict(
        windows=windows,
        distinct_touched_per_timestep=total / count,
        repeat_step_touches_per_timestep=reused / count,
        repeat_fraction_of_step_touches=reused / total if total else None,
        mean_repeat_gap_timesteps=float(np.arange(count) @ histogram / reused)
        if reused
        else None,
        repeat_gap_histogram={str(i): int(n) for i, n in enumerate(histogram) if n},
        mean_distinct_other_synapses_between_reuses_lower=lower_sum / reused
        if reused
        else None,
        mean_distinct_other_synapses_between_reuses_upper=upper_sum / reused
        if reused
        else None,
        scope="Per-PU reuse; only consecutive touches observed inside this window. "
        "Lower bound excludes endpoint timesteps; upper includes them. "
        "No within-step arrival order is assumed; never-reused touches are censored.",
    )


def analyze(args):
    manifest = json.loads((args.images / "manifest.json").read_text())
    initial = np.load(args.images / "initial.npz")
    pre, post = initial["pre"], initial["post"]
    owner = pre % PU
    degree = np.bincount(pre, minlength=NE)
    c = constants(manifest["rule"])
    start, count = args.start, args.steps
    if (
        start < 1
        or count < 1
        or start + count - 1 > manifest["warmup"] + manifest["timed"]
    ):
        raise ValueError("requested window is outside the recorded simulation")
    if start == 1:
        previous = initial["neurons"]
    else:
        _, previous = dump(args.dumps / f"step-{start - 1:04}.bin")
    updated, demanded, records = [], [], []
    for step in range(start, start + count):
        _, current = dump(args.dumps / f"step-{step:04}.bin")
        x = F(previous[:, 6].view(np.float32) * c["decay_pre"])
        y = F(previous[:, 7].view(np.float32) * c["decay_post"])
        forward = current[pre, 10] != 0
        ltd = forward & (F(c["dep"] * y[post]) != 0)
        ltp = (current[post, 9] != 0) & (F(c["pot"] * x[pre]) != 0)
        updated.append(np.flatnonzero(ltd | ltp))
        demanded.append(np.flatnonzero(forward | ltp))
        records.append(
            dict(
                step=step,
                E=int(current[:NE, 9].sum()),
                I=int(current[NE:, 9].sum()),
                EE_spike_deliveries=int(forward.sum()),
                forward_weight_segments=int(
                    ((degree + 7) // 8 * current[:NE, 10]).sum()
                ),
                LTD=int(ltd.sum()),
                LTP=int(ltp.sum()),
                same_step_dual_updates=int((ltd & ltp).sum()),
                distinct_updated=len(updated[-1]),
            )
        )
        previous = current
    result = dict(
        rule=manifest["rule"],
        first_step=start,
        timesteps=count,
        average_per_timestep={
            k: sum(r[k] for r in records) / count for k in records[0] if k != "step"
        },
        updates=working_sets(updated, owner, len(pre)),
        reads_and_updates=working_sets(demanded, owner, len(pre)),
        per_step=records,
    )
    # Consecutive operations in one timestep have zero temporal gap. The
    # positive-gap pairs are the consecutive distinct-step touches above.
    for name, operation_count in (
        ("updates", sum(r["LTD"] + r["LTP"] for r in records)),
        (
            "reads_and_updates",
            sum(r["EE_spike_deliveries"] + r["LTD"] + r["LTP"] for r in records),
        ),
    ):
        stats = result[name]
        histogram = stats["repeat_gap_histogram"]
        same_step = operation_count - round(
            stats["distinct_touched_per_timestep"] * count
        )
        repeat_count = sum(histogram.values()) + same_step
        weighted_gap = sum(int(gap) * n for gap, n in histogram.items())
        stats["operations_per_timestep"] = operation_count / count
        stats["same_step_repeat_operations_per_timestep"] = same_step / count
        stats["repeat_operation_fraction"] = (
            repeat_count / operation_count if operation_count else None
        )
        stats["mean_repeat_operation_gap_timesteps"] = (
            weighted_gap / repeat_count if repeat_count else None
        )
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(manifest["rule"], "reuse analysis complete", flush=True)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--images", type=Path, required=True)
    p.add_argument("--dumps", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--start", type=int, default=31)
    p.add_argument("--steps", type=int, default=30)
    analyze(p.parse_args())
