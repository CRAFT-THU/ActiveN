#!/usr/bin/env python3
"""Replay the latest baseline checkpoint and consume temporary 101-step windows.

Run one case per process. A consumer receives the window directory as its final
argument, after reference.json has been written. Successful temporary checkpoints
are removed; reference records, identities, logs and failures remain in output.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

import numpy as np

from baseline_checkpoints import checkpoint_index, latest_checkpoint, load_cases, read_json, sha256, write_json
from baseline_sampling import SCHEMA as SCHEDULE_SCHEMA, validate_sample
from baseline_windows import reference_record


def selected_case(schedule: dict, name: str):
    if schedule.get("schema") != SCHEDULE_SCHEMA:
        raise ValueError("Unknown schedule schema")
    records = [c for c in schedule["cases"] if c["case"] == name]
    if len(records) != 1:
        raise ValueError("Case name is missing or ambiguous in schedule")
    spec = records[0]
    cases = [c for c in load_cases(Path(schedule["baseline"]))
             if c.name == name and c.workload == spec["workload"]]
    if len(cases) != 1:
        raise ValueError("Selected case no longer exists")
    case = cases[0]
    if case.record() != {k: spec[k] for k in case.record()}:
        # JSON serializes phase tuples as lists.
        current = json.loads(json.dumps(case.record()))
        if current != {k: spec[k] for k in current}:
            raise ValueError("Schedule's checkpoint/region identity changed")
    return case, spec


def run(args):
    schedule = read_json(args.schedule)
    case, spec = selected_case(schedule, args.case)
    selected = spec["samples"]
    if args.sample is not None:
        wanted = set(args.sample)
        selected = [s for s in selected if s["index"] in wanted]
        if len(selected) != len(wanted):
            raise ValueError("Unknown or duplicate selected sample")
    if not selected:
        raise ValueError("No selected windows")
    for sample in selected:
        validate_sample(case, sample)
    if not args.consumer and not (args.retain_window and len(selected) == 1):
        raise ValueError("Provide a consumer, or retain exactly one window for inspection")
    output = args.output.resolve()
    if output.is_relative_to(case.baseline):
        raise ValueError("Outputs cannot be written into the baseline collection")
    output.mkdir(parents=True, exist_ok=False)
    environment = {k: os.environ.get(k) for k in ("CUDA_VISIBLE_DEVICES", "CUDA_PATH", "CUDAHOSTCXX",
                   "STDP_CUDART", "NVCC_PREPEND_FLAGS", "PYTHONPATH", "LD_LIBRARY_PATH", "PATH")}
    sources = {p.name: sha256(p) for p in Path(__file__).parent.glob("baseline_*.py")}
    write_json(output / "run.json", dict(argv=sys.argv, cwd=str(Path.cwd()), environment=environment,
               schedule=str(args.schedule.resolve()), schedule_sha256=sha256(args.schedule),
               case=case.record(), selected_samples=[s["index"] for s in selected],
               source_sha256=sources, consumer=args.consumer,
               validation_scope="reference generation; no ActiveN timing or comparison",
               temporary_checkpoint_policy="delete after successful consumer; preserve one failed window"))
    index = checkpoint_index(case)
    from baseline_genn import Replay
    replay = Replay(case, output / "runtime", build_jobs=args.build_jobs)
    temporary = output / "temporary"
    temporary.mkdir()
    try:
        for sample in selected:
            record_dir = output / f"sample-{sample['index']:04d}"
            record_dir.mkdir()
            parent = latest_checkpoint(index, sample["warmup_start_tick"])
            if parent.identity != replay.parent.identity:
                replay.restore_parent(parent)
            replay.prepare(sample)
            owned = Path(tempfile.mkdtemp(prefix="window-", dir=temporary))
            window_path = owned / "window"
            try:
                window = replay.save(window_path, sample, rng=dict(mode="native-genn", branch=False,
                                    trajectory="native continuation of selected retained ancestor"))
                window["window_manifest_sha256"] = sha256(window_path / "window.json")
                counts = replay.record_window()
                if replay.tick != sample["timed_end_tick"]:
                    raise RuntimeError("Reference endpoint tick differs")
                reference = reference_record(window, counts, replay.populations)
                # Check warmup and measured counts separately: comparing only
                # their sum could hide a one-step shift at the timing boundary.
                replay.restore_window(window_path, window)
                before = replay.engine.counts()
                if case.classification:
                    replay.engine.steps(1)
                else:
                    replay.engine.steps(1, after_synapses=replay.engine.full)
                after_warmup = replay.engine.counts()
                if case.classification:
                    replay.engine.steps(100)
                    replay.engine.ctl.phase_tick += 101
                else:
                    replay.engine.steps(100, after_synapses=replay.engine.full)
                after = replay.engine.counts()
                warmup = [int((b.astype(np.int64) - a.astype(np.int64)).sum()) for a, b in zip(before, after_warmup)]
                clean = [int((b.astype(np.int64) - a.astype(np.int64)).sum()) for a, b in zip(after_warmup, after)]
                agreement = clean == reference["totals"] and warmup == reference["warmup_counts"]
                reference["restore_replay_check"] = dict(passed=agreement,
                    counted_steps=100, uninterrupted_totals=clean, warmup_totals=warmup,
                    recorded_totals=reference["totals"],
                    scope="separate warmup and 100-step aggregate counts; not full-state equivalence")
                write_json(record_dir / "reference.json", reference)
                write_json(record_dir / "window-manifest.json", window)
                write_json(record_dir / "graph-manifest.json", read_json(Path(window["graph"]["directory"]) / "graph.json"))
                write_json(window_path / "reference.json", reference)
                if not agreement:
                    raise RuntimeError("Uninterrupted and restored reference counts differ")
                if args.consumer:
                    command = [*args.consumer, str(window_path)]
                    with (record_dir / "consumer.log").open("x") as log:
                        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
                    write_json(record_dir / "consumer.json", dict(command=command, cwd=str(Path.cwd()),
                                exit_status=result.returncode))
                    if result.returncode:
                        raise RuntimeError("Window consumer failed; temporary state retained")
                # Continue from the original native state, never a validation
                # branch endpoint. The next seek can reuse this same ancestor.
                replay.restore_window(window_path, window)
                if not args.retain_window:
                    shutil.rmtree(owned)
                write_json(record_dir / "complete.json", dict(reference_sha256=sha256(record_dir / "reference.json"),
                           temporary_checkpoint_deleted=not args.retain_window,
                           retained_window=str(window_path) if args.retain_window else None))
                print(f"REFERENCE {case.name} sample={sample['index']} phase={sample['phase']} measured={reference['totals']}", flush=True)
            except BaseException as exc:
                write_json(record_dir / "failed.json", dict(error=repr(exc), retained_window=str(window_path)))
                raise
        if not args.retain_window:
            replay.cleanup_graphs()
        write_json(output / "complete.json", dict(case=case.name, windows=len(selected),
                   full_schedule=len(selected) == case.k, measured_timesteps=100 * len(selected),
                   temporary_graphs_deleted=not args.retain_window))
    finally:
        replay.close()


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--schedule", type=Path, required=True)
    p.add_argument("--case", required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--sample", type=int, action="append", help="Explicit functional subset; never labelled a complete sweep")
    p.add_argument("--build-jobs", type=int, default=4)
    p.add_argument("--retain-window", action="store_true")
    p.add_argument("--consumer", nargs=argparse.REMAINDER, help="Executable and args; window directory appended")
    args = p.parse_args(argv)
    if args.build_jobs <= 0:
        p.error("Build job count must be positive")
    run(args)


if __name__ == "__main__":
    main()
