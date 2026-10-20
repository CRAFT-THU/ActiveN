"""Plan reproducible, phase-contained baseline validation windows."""
from __future__ import annotations

import argparse
import bisect
import hashlib
import random
from pathlib import Path

from baseline_checkpoints import Case, load_cases, write_json

SCHEMA = "activen-baseline-schedule-v1"
WARMUP = 1
MEASURED = 100


def eligible_intervals(first: int, end: int, phase: str, lengths: tuple[int, int] | None) -> list[tuple[int, int]]:
    """Half-open intervals of eligible warmup-start ticks (101 steps total)."""
    duration = WARMUP + MEASURED
    if end - first < duration:
        raise ValueError("Sampling region is shorter than the window")
    if lengths is None:
        if phase != "continuous":
            raise ValueError("Recurrent windows have no presentation/rest phases")
        return [(first, end - duration + 1)]
    presentation, rest = lengths
    if phase not in ("presentation", "rest"):
        raise ValueError("Unknown classification phase")
    offset, length = (0, presentation) if phase == "presentation" else (presentation, rest)
    if length < duration:
        raise ValueError("A complete 101-step window does not fit this phase")
    attempt = presentation + rest
    result = []
    for base in range((first // attempt) * attempt, end, attempt):
        lo = max(first, base + offset)
        hi = min(end, base + offset + length) - duration + 1
        if lo < hi:
            result.append((lo, hi))
    return result


def spread_starts(intervals: list[tuple[int, int]], count: int, seed: int) -> list[int]:
    """One seeded selection from each equal-sized bin of eligible tick ranks."""
    cumulative = [0]
    for lo, hi in intervals:
        if hi <= lo:
            raise ValueError("Empty eligibility interval")
        cumulative.append(cumulative[-1] + hi - lo)
    total = cumulative[-1]
    if count < 1 or total < count:
        raise ValueError("Too few eligible ticks for the declared sample count")
    rng = random.Random(seed)
    result = []
    for i in range(count):
        rank = rng.randrange(i * total // count, (i + 1) * total // count)
        index = bisect.bisect_right(cumulative, rank) - 1
        result.append(intervals[index][0] + rank - cumulative[index])
    return result


def sample_case(case: Case, seed: int) -> dict:
    first, end = case.region
    if case.phases:
        p, r = case.phases
        # Half-up rounding is deterministic; current K=1000 ratios are exact.
        kp = (2 * case.k * p + p + r) // (2 * (p + r))
        quotas = {"presentation": kp, "rest": case.k - kp}
    else:
        quotas = {"continuous": case.k}
    samples = []
    for phase, count in quotas.items():
        stream = hashlib.sha256(f"{seed}:{case.workload}:{case.name}:{phase}".encode()).digest()
        intervals = eligible_intervals(first, end, phase, case.phases)
        for tick in spread_starts(intervals, count, int.from_bytes(stream, "little")):
            samples.append(dict(warmup_start_tick=tick, timed_start_tick=tick + WARMUP,
                                timed_end_tick=tick + WARMUP + MEASURED, phase=phase,
                                phase_offset=(tick % sum(case.phases) - (case.phases[0] if phase == "rest" else 0)) if case.phases else None))
    samples.sort(key=lambda row: row["warmup_start_tick"])
    for index, row in enumerate(samples):
        row["index"] = index
    return dict(**case.record(), seed=seed, quotas=quotas, warmup_steps=WARMUP,
                measured_steps=MEASURED, samples=samples,
                sampling_law="equal bins of eligible simulated-tick ranks within duration-proportional phase strata; seeded jitter",
                scope="phase-contained windows; excludes transitions/normalization; not a full-loop mean")


def validate_sample(case: Case, sample: dict) -> None:
    """Validate caller-supplied schedules before advancing any reference state."""
    fields = ("index", "warmup_start_tick", "timed_start_tick", "timed_end_tick")
    if any(type(sample.get(k)) is not int or sample[k] < 0 for k in fields):
        raise ValueError("Window identifiers and clocks must be nonnegative integers")
    u, s, e = (sample[k] for k in fields[1:])
    if s != u + 1 or e != s + 100 or not case.region[0] <= u or e > case.region[1]:
        raise ValueError("Window lies outside the declared region or 1+100 timing contract")
    if case.phases:
        p, r = case.phases
        position = u % (p + r)
        phase, offset, limit = ("presentation", position, p) if position < p else ("rest", position - p, r)
        if sample.get("phase") != phase or sample.get("phase_offset") != offset or offset + 101 > limit:
            raise ValueError("Window crosses or mislabels a classification phase")
    elif sample.get("phase") != "continuous" or sample.get("phase_offset") is not None:
        raise ValueError("Recurrent windows have no presentation/rest phase")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--baseline", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--workload", action="append")
    p.add_argument("--case", action="append")
    p.add_argument("--seed", type=int, default=20261021)
    args = p.parse_args(argv)
    cases = [c for c in load_cases(args.baseline)
             if (not args.workload or c.workload in args.workload)
             and (not args.case or c.name in args.case)]
    if not cases:
        p.error("No matching cases")
    if args.output.resolve().is_relative_to(args.baseline.resolve()):
        p.error("Write schedules outside the immutable baseline")
    records = [sample_case(c, args.seed) for c in cases]
    write_json(args.output, dict(schema=SCHEMA, baseline=str(args.baseline.resolve()),
                                cases=records, total_windows=sum(c.k for c in cases),
                                measured_timesteps=sum(c.k for c in cases) * MEASURED))


if __name__ == "__main__":
    main()
