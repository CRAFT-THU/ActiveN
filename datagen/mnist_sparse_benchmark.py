#!/usr/bin/env python3
"""Paired sparse reverse-delivery benchmarks over a completed snapshot corpus.

Run inside the repository's Nix development shell. Each case retains its input
images, command, log, final state and hashes. Resume rejects changed provenance.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import sys
import time

import mnist_sparse as sparse
from mnist_sparse_state import state_hashes
from mnist_sparse_reference import validate as validate_fp32
from mnist_snapshot_benchmark import atomic_json, run_logged, sha256_file

REPO = Path(__file__).resolve().parents[1]
METRICS = ("sparse_input_spikes", "sparse_excitatory_spikes", "sparse_inhibitory_spikes",
           "sparse_feedforward_messages", "sparse_reverse_messages", "icache_fills")


def parse_log(path: Path, steps: int) -> dict:
    log = path.read_text()
    if "[System] Result: 0 (0x0)" not in log:
        raise ValueError(f"simulation did not complete successfully: {path}")
    intervals = []
    pattern = r"\[Soft\] Timer interval (\d+) \(cycles (\d+)\.\.(\d+), span=(\d+)\):([^\[]*)"
    for match in re.finditer(pattern, log):
        index, start, end, cycles = map(int, match.group(1, 2, 3, 4))
        if index != len(intervals) + 1 or end - start + 1 != cycles:
            raise ValueError("invalid timer interval sequence")
        interval = dict(index=index, start=start, end=end, cycles=cycles)
        for metric in METRICS:
            value = re.search(rf"^  {metric}:\s*(\d+)", match[5], re.MULTILINE)
            if value is None:
                raise ValueError(f"missing {metric}: {path}")
            interval[metric] = int(value[1])
        intervals.append(interval)
    if len(intervals) != steps:
        raise ValueError(f"expected {steps} timer intervals, got {len(intervals)}: {path}")
    checksums = re.findall(r"\[Periph\] Output: ([0-9a-fA-F]+)", log)
    if len(checksums) != 1:
        raise ValueError(f"expected one terminal checksum: {path}")
    return dict(intervals=intervals, checksum=checksums[0],
                total_cycles=int(re.search(r"\[System\] Cycles: (\d+)", log)[1]))


def run_case(snapshot: Path, mode: str, args: argparse.Namespace) -> dict:
    case = args.output / snapshot.stem / mode
    result_path = case / "result.json"
    if result_path.exists():
        result = json.loads(result_path.read_text())
        if result["snapshot_sha256"] != sha256_file(snapshot):
            raise ValueError(f"cached snapshot changed: {case}")
        if result["log_sha256"] != sha256_file(case / "simulate.log"):
            raise ValueError(f"cached simulation log changed: {case}")
        if result["dump_sha256"] != sha256_file(case / "state.bin"):
            raise ValueError(f"cached state dump changed: {case}")
        return result
    if case.exists():
        raise ValueError(f"incomplete case requires inspection: {case}")
    case.mkdir(parents=True)
    image_dir = case / "image"
    frozen = args.output / "frozen"
    generation = [sys.executable, str(frozen / "mnist_sparse.py"), "--payload",
        str(frozen / f"{args.rule}-{mode}.bin"), "--output", str(image_dir),
        "--reverse-mode", mode, "--rule", args.rule, "--connection-rate", str(args.connection_rate),
        "--connectivity-seed", str(args.connectivity_seed), "--runtime-snapshot", str(snapshot),
        "--warmup-steps", str(args.warmup_steps), "--timed-steps", str(args.timed_steps)]
    if getattr(args, "relevant_masks", False):
        generation.append("--relevant-masks")
    code, generation_seconds = run_logged(generation, case / "generate.log", cwd=REPO)
    if code:
        raise RuntimeError(f"image generation failed: {case}")
    manifest = json.loads((image_dir / "manifest.json").read_text())
    if args.warmup_steps + args.timed_steps > manifest["runtime_snapshot"]["window_steps"]:
        raise ValueError("requested simulation exceeds the exported snapshot window")
    simulation = [str(frozen / "sim_system"), "--soft", "--soft-threads", str(args.soft_threads),
        "--pu", "512", "--mc", "16", "--mc-size", "0x100000000", "--max-cycles", str(args.max_cycles)]
    if getattr(args, "init_icache", False):
        simulation.append("--init-icache")
    if args.ramulator_config:
        simulation += ["--ramulator-config", str(frozen / "ramulator.yaml")]
    simulation += [str(image_dir / f"dram.{mc}") for mc in range(16)]
    atomic_json(case / "commands.json", {"generate": generation, "simulate": simulation,
        "environment": {"SOFT_MNIST_SPARSE_STATS": "1", "SOFT_MNIST_SPARSE_STEPS": "1",
                        "SOFT_MNIST_SPARSE_DUMP": str(case / "state.bin")}})
    env = {**os.environ, "SOFT_MNIST_SPARSE_STATS": "1", "SOFT_MNIST_SPARSE_STEPS": "1",
           "SOFT_MNIST_SPARSE_DUMP": str(case / "state.bin")}
    code, wall = run_logged(simulation, case / "simulate.log", cwd=REPO, environment=env)
    if code:
        raise RuntimeError(f"simulation failed: {case}")
    oracle = validate_fp32(image_dir / "dram.0", case / "simulate.log", case / "state.bin",
                          args.warmup_steps + args.timed_steps)
    atomic_json(case / "reference.json", oracle)
    result = {**parse_log(case / "simulate.log", args.timed_steps), "snapshot": snapshot.name,
        "snapshot_sha256": sha256_file(snapshot), "mode": mode, "rule": args.rule,
        "connection_rate": args.connection_rate, "metadata": manifest["runtime_snapshot"],
        "state_sha256": state_hashes(case / "state.bin"), "dump_sha256": sha256_file(case / "state.bin"),
        "log_sha256": sha256_file(case / "simulate.log"), "simulation_seconds": wall,
        "generation_seconds": generation_seconds, "structural_synapses": manifest["structural_synapses"],
        "fp32_reference": oracle}
    atomic_json(result_path, result)
    return result


def summarize(results: list[dict], modes=None) -> dict:
    modes = tuple(sparse.MODES if modes is None else modes)
    paired = {}
    for result in results:
        paired.setdefault(result["snapshot"], []).append(result)
    neuron_differences = []
    for snapshot, group in paired.items():
        if {item["mode"] for item in group} != set(modes):
            raise ValueError(f"incomplete delivery-mode comparison: {snapshot}")
        for state in ("weights", "inputs"):
            if len({item["state_sha256"][state] for item in group}) != 1:
                raise ValueError(f"unexplained final {state} mismatch: {snapshot}")
        if len({item["state_sha256"]["neurons"] for item in group}) != 1:
            neuron_differences.append(snapshot)
    summary = dict(snapshots=len(paired), cases=len(results), neuron_state_differences=neuron_differences, modes={})
    for mode in modes:
        rows = [interval for result in results if result["mode"] == mode for interval in result["intervals"]]
        cycles = sorted(row["cycles"] for row in rows)
        summary["modes"][mode] = dict(steps=len(cycles), mean_cycles=statistics.mean(cycles),
            median_cycles=statistics.median(cycles), p95_cycles=(statistics.quantiles(cycles, n=100, method="inclusive")[94]
                                                               if len(cycles) > 1 else cycles[0]),
            max_cycles=max(cycles), mean_reverse_messages=statistics.mean(row["sparse_reverse_messages"] for row in rows),
            mean_icache_fills=statistics.mean(row["icache_fills"] for row in rows))
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshots", type=Path, required=True)
    parser.add_argument("--payload-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rule", choices=("one-trace", "two-trace", "three-trace"), required=True)
    parser.add_argument("--connection-rate", type=float, required=True)
    parser.add_argument("--connectivity-seed", type=int, default=20260723)
    parser.add_argument("--simulator", type=Path, default=REPO / "sim/build/sim_system")
    parser.add_argument("--init-icache", action="store_true",
                        help="preload the simulator's configured instruction-cache region")
    parser.add_argument("--relevant-masks", action="store_true",
                        help="generate descriptors for a relevant-mask payload")
    parser.add_argument("--ramulator-config", type=Path)
    parser.add_argument("--warmup-steps", type=int, default=4)
    parser.add_argument("--timed-steps", type=int, default=12)
    parser.add_argument("--max-cycles", type=int, default=3000000)
    parser.add_argument("--soft-threads", type=int, default=4)
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--snapshot-limit", type=int)
    parser.add_argument("--modes", nargs="+", choices=sparse.MODES, default=list(sparse.MODES),
                        help="reverse modes to run (default: all four)")
    args = parser.parse_args()
    if len(set(args.modes)) != len(args.modes):
        parser.error("reverse modes must be unique")
    if min(args.jobs, args.soft_threads, args.timed_steps, args.max_cycles) <= 0 or args.warmup_steps < 0:
        parser.error("invalid worker or step counts")
    if args.jobs * args.soft_threads > (os.cpu_count() or 1):
        parser.error("worker count exceeds host CPUs")
    for key in ("snapshots", "payload_dir", "output", "simulator", "ramulator_config"):
        if getattr(args, key) is not None: setattr(args, key, getattr(args, key).resolve())
    config = json.loads((REPO / "generated/system/System.config.json").read_text())
    core = config["core"]
    if (config["numPU"], config["numMC"], core["memBusWidth"], core["scratchpadSize"], core["pipeCnt"]) != (512, 16, 256, 16384, 2):
        raise ValueError("sparse benchmark requires the maintained 512-PU/16-MC configuration")
    if len(core["memCtrlSizes"]) != 16 or any(int(size, 0) != 0x100000000 for size in core["memCtrlSizes"]):
        raise ValueError("sparse benchmark requires 4 GiB per MC")
    complete = json.loads((args.snapshots / "complete.json").read_text())
    snapshots = sorted(args.snapshots.glob("snapshot_*.npz"))
    if complete["snapshot_count"] != len(snapshots) or complete["rule"] != args.rule:
        raise ValueError("snapshot completion manifest disagrees with corpus")
    for path in snapshots:
        if complete["snapshot_sha256"][path.name] != sha256_file(path):
            raise ValueError(f"snapshot hash mismatch: {path}")
    if args.snapshot_limit is not None:
        if args.snapshot_limit <= 0: parser.error("snapshot limit must be positive")
        snapshots = snapshots[:args.snapshot_limit]
    sources = [Path(__file__), REPO / "datagen/mnist_train.py", REPO / "datagen/mnist_sparse.py",
        REPO / "datagen/mnist_weight_caps.py",
        REPO / "datagen/mnist_sparse_state.py", REPO / "datagen/mnist_snapshot_benchmark.py",
        REPO / "datagen/mnist_sparse_reference.py",
        REPO / "generated/system/System.config.json", args.simulator,
        *(args.payload_dir / f"{args.rule}-{mode}.bin" for mode in args.modes)]
    if args.ramulator_config: sources.append(args.ramulator_config)
    provenance = dict(arguments={key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
        sha256={str(path): sha256_file(path) for path in sources},
        snapshots={path.name: sha256_file(path) for path in snapshots}, memory_model="ramulator" if args.ramulator_config else "flat")
    args.output.mkdir(parents=True, exist_ok=True)
    provenance_path = args.output / "provenance.json"
    if provenance_path.exists() and json.loads(provenance_path.read_text()) != provenance:
        raise ValueError("benchmark provenance changed; use a new output directory")
    if not provenance_path.exists(): atomic_json(provenance_path, provenance)
    frozen = args.output / "frozen"
    frozen.mkdir(exist_ok=True)
    for source in sources:
        name = ("sim_system" if source == args.simulator else
                "ramulator.yaml" if source == args.ramulator_config else source.name)
        destination = frozen / name
        if destination.exists():
            if sha256_file(destination) != provenance["sha256"][str(source)]:
                raise ValueError(f"frozen source changed: {destination}")
        else:
            shutil.copy2(source, destination)
    started, results, triads = time.monotonic(), [], {}
    atomic_json(args.output / "status.json", {"state": "running", "pid": os.getpid(), "completed": 0, "total": len(snapshots) * len(args.modes)})
    try:
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            pending = [pool.submit(run_case, snapshot, mode, args) for snapshot in snapshots for mode in args.modes]
            try:
                for future in as_completed(pending):
                    results.append(future.result())
                    group = triads.setdefault(results[-1]["snapshot"], [])
                    group.append(results[-1])
                    if len(group) == len(args.modes):
                        summarize(group, args.modes)
                    atomic_json(args.output / "status.json", {"state": "running", "pid": os.getpid(),
                        "completed": len(results), "total": len(pending), "elapsed_seconds": time.monotonic() - started})
                    print(f"PASS {results[-1]['snapshot']} {results[-1]['mode']} ({len(results)}/{len(pending)})", flush=True)
            except BaseException:
                for future in pending: future.cancel()
                raise
        summary = summarize(results, args.modes)
        atomic_json(args.output / "summary.json", summary)
        atomic_json(args.output / "status.json", {"state": "complete", "completed": len(results), "elapsed_seconds": time.monotonic() - started})
        print(json.dumps(summary, indent=2))
    except BaseException as exc:
        atomic_json(args.output / "status.json", {"state": "failed", "error": repr(exc), "completed": len(results)})
        raise


if __name__ == "__main__":
    main()
