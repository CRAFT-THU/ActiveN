"""Combine a complete declared case without dropping failed validation windows."""
from __future__ import annotations

import argparse
from pathlib import Path

from baseline_checkpoints import read_json, sha256, write_json
from baseline_windows import compare_counts


def summarize_case(spec: dict, references: list[dict], observed: list[dict], *, tolerance=None) -> dict:
    def index(records):
        result = {}
        for row in records:
            key = row["sample"]
            if type(key) is not int or key in result:
                raise ValueError("Duplicate or invalid sample identity")
            result[key] = row
        return result
    refs, observations = index(references), index(observed)
    selected = {r["index"]: r for r in spec["samples"]}
    k = spec["windows"]
    if len(selected) != k or len(spec["samples"]) != k or set(refs) != set(selected) or set(observations) != set(selected):
        raise ValueError("Report requires every declared sample exactly once")
    results = []
    phase_cycles = {}
    for key, sample in selected.items():
        ref, actual = refs[key], observations[key]
        for field, expected in (("case", spec["case"]), ("workload", spec["workload"]),
                                ("phase", sample["phase"]), ("timed_start_tick", sample["timed_start_tick"]),
                                ("timed_end_tick", sample["timed_end_tick"])):
            if ref.get(field) != expected or actual.get(field) != expected:
                raise ValueError(f"Sample differs from schedule: {field}")
        result = dict(sample=key, phase=sample["phase"], **compare_counts(ref, actual, tolerance=tolerance))
        results.append(result)
        phase_cycles.setdefault(sample["phase"], []).append(result["observed_timer_cycles"])
    passed = all(r["passed"] for r in results)
    total = sum(r["observed_timer_cycles"] for r in results)
    return dict(schema="activen-baseline-comparison-v1", workload=spec["workload"], case=spec["case"],
                passed=passed, windows=k, measured_timesteps=100 * k, samples=results,
                observed_timer_cycles=total, accepted_timer_cycles=total if passed else None,
                cycles_per_timestep=total / (100 * k) if passed else None,
                phase_windows={p: len(v) for p,v in phase_cycles.items()},
                tolerance=tolerance, scope=spec["scope"])


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--schedule", type=Path, required=True)
    p.add_argument("--case", required=True)
    p.add_argument("--references", type=Path, required=True)
    p.add_argument("--observations", type=Path, required=True)
    p.add_argument("--tolerance", type=Path)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args(argv)
    records = [c for c in read_json(args.schedule)["cases"] if c["case"] == args.case]
    if len(records) != 1:
        p.error("Case is missing or ambiguous")
    reference_paths = sorted(args.references.glob("sample-*/reference.json"))
    observed_paths = sorted(args.observations.glob("sample-*/observed.json"))
    result = summarize_case(records[0], [read_json(p) for p in reference_paths],
                            [read_json(p) for p in observed_paths],
                            tolerance=read_json(args.tolerance) if args.tolerance else None)
    result["schedule_sha256"] = sha256(args.schedule)
    result["input_sha256"] = {str(p.resolve()): sha256(p) for p in [*reference_paths, *observed_paths]}
    write_json(args.output, result)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
