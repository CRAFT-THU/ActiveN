#!/usr/bin/env python3
"""Generate and benchmark ActiveN images from GeNN MNIST snapshots."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Iterable


TIMER_RE = re.compile(
    r"^\[Soft\] Timer interval (\d+) \(cycles (\d+)\.\.(\d+), span=(\d+)\):$"
)
INTEGER_STAT_RE = re.compile(r"^\s+([a-z_]+):\s+(\d+)")
ICACHE_STALL_RE = re.compile(r"^\s+icache_stalls:\s+(\d+) core-cycles")
SYSTEM_RESULT_RE = re.compile(r"^\[System\] Result: (\d+) ")
SYSTEM_CYCLES_RE = re.compile(r"^\[System\] Cycles: (\d+)$")
SYSTEM_TIMER_RE = re.compile(r"^\[System\] Timer: (\d+) cycles$")
SYSTEM_RUNTIME_RE = re.compile(r"^\[System\] Runtime: ([0-9.]+)s$")
CHECKSUM_RE = re.compile(r"^\[Periph\] Output: ([0-9a-fA-F]+)$")
INTERVAL_KEYS = {
    "unicast_mem_requests",
    "icache_accesses",
    "icache_fills",
    "mnist_feedforward_messages",
    "mnist_trace_messages",
    "mnist_post_fired_trace_messages",
    "mnist_inhibitory_spikes",
}


def atomic_json(path: Path, value: Any) -> None:
    temporary = path.with_name(
        f".{path.name}.{os.getpid()}.{threading.get_ident()}.tmp"
    )
    with temporary.open("x", encoding="ascii") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalize_result_activity(result: dict[str, Any]) -> bool:
    """Repair activity derived from the packed neuron-reduction value."""
    changed = False
    for interval in result.get("intervals", []):
        inhibitory_spikes = interval["mnist_inhibitory_spikes"] & 0x3FF
        if interval["mnist_inhibitory_spikes"] != inhibitory_spikes:
            interval["mnist_inhibitory_spikes"] = inhibitory_spikes
            changed = True
        if interval.get("inhibitory_spikes") != inhibitory_spikes:
            interval["inhibitory_spikes"] = inhibitory_spikes
            changed = True
    return changed


def read_idx_labels(path: Path) -> list[int]:
    raw = path.read_bytes()
    if len(raw) < 8 or raw[:4] != b"\x00\x00\x08\x01":
        raise ValueError(f"{path} is not an unsigned-byte one-dimensional IDX file")
    count = int.from_bytes(raw[4:8], "big")
    if len(raw) != count + 8:
        raise ValueError(f"{path} has an inconsistent IDX payload length")
    return list(raw[8:])


def run_logged(
    command: list[str],
    log_path: Path,
    *,
    cwd: Path,
    environment: dict[str, str] | None = None,
) -> tuple[int, float]:
    started = time.monotonic()
    with log_path.open("w", encoding="ascii", errors="backslashreplace") as stream:
        completed = subprocess.run(
            command,
            cwd=cwd,
            env=environment,
            stdout=stream,
            stderr=subprocess.STDOUT,
            check=False,
        )
    return completed.returncode, time.monotonic() - started


def parse_simulator_log(
    path: Path,
    *,
    expected_intervals: int,
    input_pus: int,
    neuron_pus: int,
) -> dict[str, Any]:
    intervals: list[dict[str, Any]] = []
    current_interval: dict[str, Any] | None = None
    system_result = None
    system_cycles = None
    system_timer_cycles = None
    system_runtime_seconds = None
    checksum = None
    for raw_line in path.read_text(encoding="ascii", errors="replace").splitlines():
        timer_match = TIMER_RE.match(raw_line)
        if timer_match:
            index, start, end, span = (int(value) for value in timer_match.groups())
            current_interval = {
                "index": index,
                "start_cycle": start,
                "end_cycle": end,
                "cycles": span,
            }
            intervals.append(current_interval)
            continue
        if current_interval is not None:
            stall_match = ICACHE_STALL_RE.match(raw_line)
            if stall_match:
                current_interval["icache_stall_core_cycles"] = int(
                    stall_match.group(1)
                )
                continue
            stat_match = INTEGER_STAT_RE.match(raw_line)
            if stat_match and stat_match.group(1) in INTERVAL_KEYS:
                current_interval[stat_match.group(1)] = int(stat_match.group(2))
                continue
            if raw_line.startswith("["):
                current_interval = None
        match = SYSTEM_RESULT_RE.match(raw_line)
        if match:
            system_result = int(match.group(1))
            continue
        match = SYSTEM_CYCLES_RE.match(raw_line)
        if match:
            system_cycles = int(match.group(1))
            continue
        match = SYSTEM_TIMER_RE.match(raw_line)
        if match:
            system_timer_cycles = int(match.group(1))
            continue
        match = SYSTEM_RUNTIME_RE.match(raw_line)
        if match:
            system_runtime_seconds = float(match.group(1))
            continue
        match = CHECKSUM_RE.match(raw_line)
        if match:
            checksum = int(match.group(1), 16)

    if len(intervals) != expected_intervals:
        raise ValueError(
            f"{path} has {len(intervals)} timer intervals, expected {expected_intervals}"
        )
    required = INTERVAL_KEYS | {"icache_stall_core_cycles"}
    for interval in intervals:
        missing = sorted(required - interval.keys())
        if missing:
            raise ValueError(
                f"{path} interval {interval['index']} is missing {missing}"
            )
        feedforward = interval["mnist_feedforward_messages"]
        post_fired = interval["mnist_post_fired_trace_messages"]
        if feedforward % neuron_pus:
            raise ValueError(
                f"interval {interval['index']} feedforward count {feedforward} "
                f"is not divisible by {neuron_pus}"
            )
        if post_fired % input_pus:
            raise ValueError(
                f"interval {interval['index']} fired-trace count {post_fired} "
                f"is not divisible by {input_pus}"
            )
        interval["input_spikes"] = feedforward // neuron_pus
        interval["excitatory_spikes"] = post_fired // input_pus
        interval["mnist_inhibitory_spikes"] &= 0x3FF
        interval["inhibitory_spikes"] = interval["mnist_inhibitory_spikes"]
        interval["strictly_hot"] = (
            interval["unicast_mem_requests"] == 0
            and interval["icache_fills"] == 0
        )
    if system_result is None:
        raise ValueError(f"{path} has no system result")
    if system_timer_cycles != sum(item["cycles"] for item in intervals):
        raise ValueError(f"{path} timer total does not equal its interval spans")
    return {
        "intervals": intervals,
        "system_result": system_result,
        "system_cycles": system_cycles,
        "system_timer_cycles": system_timer_cycles,
        "system_runtime_seconds": system_runtime_seconds,
        "checksum": checksum,
    }


def benchmark_one(
    snapshot: Path,
    *,
    repository: Path,
    generator: Path,
    output: Path,
    payload: Path,
    simulator: Path,
    warmup_steps: int,
    timed_steps: int,
    max_cycles: int,
    soft_threads: int,
    input_pus: int,
    neuron_pus: int,
    horizontal_partition: str,
    labels: list[int],
) -> dict[str, Any]:
    index_match = re.search(r"(\d+)$", snapshot.stem)
    if index_match is None:
        raise ValueError(f"snapshot filename has no numeric index: {snapshot}")
    index = int(index_match.group(1))
    case = output / f"snapshot_{index:03d}"
    case.mkdir(parents=True, exist_ok=True)
    result_path = case / "result.json"
    if result_path.is_file():
        with result_path.open(encoding="ascii") as stream:
            result = json.load(stream)
        if normalize_result_activity(result):
            atomic_json(result_path, result)
        return result

    image = case / "image"
    manifest_path = image / "manifest.json"
    images = [image / f"dram.{mc}" for mc in range(16)]
    if not manifest_path.is_file() or not all(path.is_file() for path in images):
        if image.exists() and any(image.iterdir()):
            raise RuntimeError(f"partial image directory requires inspection: {image}")
        image.mkdir(parents=True, exist_ok=True)
        generator_command = [
            sys.executable,
            str(generator),
            "--payload",
            str(payload),
            "--output",
            str(image),
            "--runtime-snapshot",
            str(snapshot),
            "--rule",
            "three-trace",
            "--input-pus",
            str(input_pus),
            "--neuron-pus",
            str(neuron_pus),
            "--placement-policy",
            "near",
            "--neuron-zone-policy",
            "network-balanced",
            "--input-assignment",
            "rate-balanced",
            "--horizontal-partition",
            horizontal_partition,
            "--recurrent-mode",
            "tree",
            "--warmup-steps",
            str(warmup_steps),
            "--timed-steps",
            str(timed_steps),
        ]
        return_code, _ = run_logged(
            generator_command, case / "generate.log", cwd=repository
        )
        if return_code:
            raise RuntimeError(f"image generation failed for {snapshot}")

    with manifest_path.open(encoding="ascii") as stream:
        manifest = json.load(stream)
    if manifest.get("horizontal_partition", "none") != horizontal_partition:
        raise ValueError(f"cached image has the wrong partition mode: {image}")
    reverse_group_sizes = {
        len(group) for group in manifest.get("input_owner_groups", [])
    }
    if len(reverse_group_sizes) != 1:
        raise ValueError("reverse destination groups must have equal sizes")
    reverse_destinations_per_post = reverse_group_sizes.pop()
    metadata = manifest["runtime_snapshot"]
    frame_index = int(metadata["frame_index"])
    if not 0 <= frame_index < len(labels):
        raise ValueError(f"snapshot frame index is out of range: {frame_index}")
    digit = labels[frame_index]
    simulation = [
        str(simulator),
        "--soft",
        "--soft-threads",
        str(soft_threads),
        "--pu",
        "512",
        "--mc",
        "16",
        "--mc-size",
        "0x100000000",
        "--max-cycles",
        str(max_cycles),
        *(str(path) for path in images),
    ]
    environment = os.environ.copy()
    environment["SOFT_TIMER_MNIST_ACTIVITY"] = "1"
    return_code, wall_seconds = run_logged(
        simulation,
        case / "simulate.log",
        cwd=repository,
        environment=environment,
    )
    parsed = parse_simulator_log(
        case / "simulate.log",
        expected_intervals=timed_steps,
        input_pus=reverse_destinations_per_post,
        neuron_pus=neuron_pus,
    )
    if return_code or parsed["system_result"] != 0:
        raise RuntimeError(
            f"simulation failed for {snapshot}: process={return_code}, "
            f"result={parsed['system_result']}"
        )
    for interval in parsed["intervals"]:
        interval["snapshot_index"] = index
        interval["phase"] = metadata["phase"]
        interval["accepted_sample"] = metadata["accepted_sample"]
        interval["attempt_index"] = metadata["attempt_index"]
        interval["phase_tick"] = metadata["phase_tick"]
        interval["digit"] = digit
    result = {
        "schema": "activen-mnist-snapshot-result-v1",
        "snapshot_index": index,
        "snapshot": str(snapshot),
        "phase": metadata["phase"],
        "accepted_sample": metadata["accepted_sample"],
        "attempt_index": metadata["attempt_index"],
        "phase_tick": metadata["phase_tick"],
        "frame_index": frame_index,
        "digit": digit,
        "horizontal_partition": horizontal_partition,
        "wall_seconds": wall_seconds,
        **parsed,
    }
    atomic_json(result_path, result)
    return result


def percentile(values: list[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = quantile * (len(ordered) - 1)
    lower = math.floor(rank)
    upper = math.ceil(rank)
    fraction = rank - lower
    return float(ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction)


def summarize_group(intervals: Iterable[dict[str, Any]]) -> dict[str, Any]:
    selected = list(intervals)
    cycles = [float(item["cycles"]) for item in selected]
    return {
        "steps": len(selected),
        "cycle_mean": sum(cycles) / len(cycles) if cycles else None,
        "cycle_median": percentile(cycles, 0.5),
        "cycle_p90": percentile(cycles, 0.9),
        "cycle_p95": percentile(cycles, 0.95),
        "cycle_p99": percentile(cycles, 0.99),
        "cycle_min": min(cycles) if cycles else None,
        "cycle_max": max(cycles) if cycles else None,
        "input_spikes": sum(item["input_spikes"] for item in selected),
        "excitatory_spikes": sum(item["excitatory_spikes"] for item in selected),
        "inhibitory_spikes": sum(item["inhibitory_spikes"] for item in selected),
        "feedforward_messages": sum(
            item["mnist_feedforward_messages"] for item in selected
        ),
        "trace_messages": sum(item["mnist_trace_messages"] for item in selected),
        "unicast_mem_requests": sum(
            item["unicast_mem_requests"] for item in selected
        ),
        "icache_fills": sum(item["icache_fills"] for item in selected),
        "icache_stall_core_cycles": sum(
            item["icache_stall_core_cycles"] for item in selected
        ),
    }


def summarize_input_load(
    results: list[dict[str, Any]], args: argparse.Namespace
) -> dict[str, Any]:
    import mnist_train as imagegen

    logical_spikes = 0
    physical_firings = 0
    split_spikes = 0
    active_pus: list[float] = []
    critical_rows: list[float] = []
    critical_synapses: list[float] = []
    collision_cycles: list[float] = []
    noncollision_cycles: list[float] = []
    cycles_by_critical_synapses: dict[int, list[float]] = {}
    collision_steps = 0
    physical_records: set[int] = set()
    maximum_post_scan = 0

    for result in results:
        snapshot = imagegen.load_runtime_snapshot(Path(result["snapshot"]))
        manifest_path = (
            args.output / f"snapshot_{result['snapshot_index']:03d}" /
            "image" / "manifest.json"
        )
        with manifest_path.open(encoding="ascii") as stream:
            manifest = json.load(stream)
        owners_by_input = manifest.get("input_owners")
        if not owners_by_input or len(owners_by_input) != imagegen.N_INPUT:
            raise ValueError(
                f"manifest has no complete input ownership: {manifest_path}"
            )
        physical_records.add(sum(len(owners) for owners in owners_by_input))
        partition_mode = manifest.get("horizontal_partition", "none")
        group_count = int(manifest.get(
            "horizontal_group_count", 1 if partition_mode == "none" else 2,
        ))

        def owner_post_count(owner: dict[str, Any]) -> int:
            if "post_count" in owner:
                return int(owner["post_count"])
            if partition_mode == "none":
                return imagegen.N_EXC
            return (
                int(owner["group_mask"]).bit_count()
                * (imagegen.N_EXC // group_count)
            )

        post_scan_by_pu: dict[int, int] = {}
        for owners in owners_by_input:
            if sum(bool(owner["primary"]) for owner in owners) != 1:
                raise ValueError(
                    f"input does not have exactly one primary: {manifest_path}"
                )
            for owner in owners:
                columns = owner_post_count(owner)
                post_scan_by_pu[owner["pu"]] = (
                    post_scan_by_pu.get(owner["pu"], 0) + columns
                )
        maximum_post_scan = max(maximum_post_scan, *post_scan_by_pu.values())

        thresholds = [
            imagegen.poisson_threshold(pixel, snapshot.input_intensity)
            for pixel in snapshot.pixels
        ]
        states = list(snapshot.input_rng_state)
        for step in range(args.warmup_steps + args.timed_steps):
            fired: list[int] = []
            for input_index, threshold in enumerate(thresholds):
                state = imagegen.xorshift32(states[input_index])
                states[input_index] = state
                if threshold == 0xFFFFFFFF or state < threshold:
                    fired.append(input_index)
            if step < args.warmup_steps:
                continue

            interval = result["intervals"][step - args.warmup_steps]
            if interval["input_spikes"] != len(fired):
                raise ValueError(
                    f"snapshot {result['snapshot_index']} step {step} input count "
                    f"is {interval['input_spikes']}, reconstructed {len(fired)}"
                )
            work_by_pu: dict[int, int] = {}
            rows_by_pu: dict[int, int] = {}
            for input_index in fired:
                owners = owners_by_input[input_index]
                split_spikes += len(owners) > 1
                physical_firings += len(owners)
                for owner in owners:
                    columns = owner_post_count(owner)
                    pu = owner["pu"]
                    work_by_pu[pu] = work_by_pu.get(pu, 0) + columns
                    rows_by_pu[pu] = rows_by_pu.get(pu, 0) + 1
            logical_spikes += len(fired)
            if sum(work_by_pu.values()) != len(fired) * imagegen.N_EXC:
                raise ValueError("horizontal placement changed logical synapse work")
            if fired:
                active_pus.append(float(len(work_by_pu)))
                maximum_rows = max(rows_by_pu.values())
                maximum_synapses = max(work_by_pu.values())
                critical_rows.append(float(maximum_rows))
                critical_synapses.append(float(maximum_synapses))
                cycles_by_critical_synapses.setdefault(maximum_synapses, []).append(
                    float(interval["cycles"])
                )
                if maximum_rows > 1:
                    collision_steps += 1
                    collision_cycles.append(float(interval["cycles"]))
                else:
                    noncollision_cycles.append(float(interval["cycles"]))

    if len(physical_records) != 1:
        raise ValueError("physical input record count changed across snapshots")

    def distribution(values: list[float]) -> dict[str, float | int | None]:
        return {
            "samples": len(values),
            "mean": sum(values) / len(values) if values else None,
            "p95": percentile(values, 0.95),
            "maximum": max(values) if values else None,
        }

    return {
        "logical_input_spikes": logical_spikes,
        "physical_input_firings": physical_firings,
        "split_logical_spikes": split_spikes,
        "split_logical_spike_fraction": (
            split_spikes / logical_spikes if logical_spikes else None
        ),
        "physical_input_records_per_step": physical_records.pop(),
        "steps_with_input_spikes": len(active_pus),
        "steps_with_local_firing_collision": collision_steps,
        "active_input_pus_per_active_step": distribution(active_pus),
        "critical_fired_rows_per_active_step": distribution(critical_rows),
        "critical_presynaptic_synapses_per_active_step": distribution(
            critical_synapses
        ),
        "collision_step_cycles": distribution(collision_cycles),
        "noncollision_step_cycles": distribution(noncollision_cycles),
        "cycles_by_critical_presynaptic_synapses": {
            str(synapses): distribution(cycles)
            for synapses, cycles in sorted(cycles_by_critical_synapses.items())
        },
        "maximum_local_firing_post_scan_synapses": maximum_post_scan,
    }


def make_summary(results: list[dict[str, Any]], args: argparse.Namespace) -> dict[str, Any]:
    intervals = [item for result in results for item in result["intervals"]]

    def group(predicate: Any) -> dict[str, Any]:
        return summarize_group(item for item in intervals if predicate(item))

    placement_signatures = set()
    for result in results:
        manifest_path = (
            args.output / f"snapshot_{result['snapshot_index']:03d}" /
            "image" / "manifest.json"
        )
        with manifest_path.open(encoding="ascii") as stream:
            manifest = json.load(stream)
        placement_signatures.add(
            (
                tuple(manifest["input_pus"]),
                tuple(manifest["neuron_pus"]),
                manifest["input_assignment"],
                manifest.get("horizontal_partition", "none"),
                tuple(
                    tuple(
                        (
                            owner["pu"], owner["local"],
                            owner["group_mask"], owner["primary"],
                        )
                        for owner in owners
                    )
                    for owners in manifest.get("input_owners", [])
                ),
            )
        )
    groups = {
        "all": summarize_group(intervals),
        "strictly_hot": group(lambda item: item["strictly_hot"]),
        "not_strictly_hot": group(lambda item: not item["strictly_hot"]),
        "presentation": group(lambda item: item["phase"] == "presentation"),
        "rest": group(lambda item: item["phase"] == "rest"),
        "strictly_hot_presentation": group(
            lambda item: item["strictly_hot"]
            and item["phase"] == "presentation"
        ),
        "strictly_hot_rest": group(
            lambda item: item["strictly_hot"] and item["phase"] == "rest"
        ),
        "input_active": group(lambda item: item["input_spikes"] > 0),
        "input_zero": group(lambda item: item["input_spikes"] == 0),
        "strictly_hot_input_active": group(
            lambda item: item["strictly_hot"] and item["input_spikes"] > 0
        ),
        "strictly_hot_presentation_input_active": group(
            lambda item: item["strictly_hot"]
            and item["phase"] == "presentation"
            and item["input_spikes"] > 0
        ),
        "excitatory_active": group(lambda item: item["excitatory_spikes"] > 0),
        "excitatory_zero": group(lambda item: item["excitatory_spikes"] == 0),
        "strictly_hot_excitatory_active": group(
            lambda item: item["strictly_hot"]
            and item["excitatory_spikes"] > 0
        ),
        "strictly_hot_excitatory_zero": group(
            lambda item: item["strictly_hot"]
            and item["excitatory_spikes"] == 0
        ),
        "presentation_excitatory_active": group(
            lambda item: item["phase"] == "presentation"
            and item["excitatory_spikes"] > 0
        ),
        "presentation_excitatory_zero": group(
            lambda item: item["phase"] == "presentation"
            and item["excitatory_spikes"] == 0
        ),
        "rest_excitatory_active": group(
            lambda item: item["phase"] == "rest"
            and item["excitatory_spikes"] > 0
        ),
        "rest_excitatory_zero": group(
            lambda item: item["phase"] == "rest"
            and item["excitatory_spikes"] == 0
        ),
    }
    presentation_by_digit = {
        str(digit): group(
            lambda item, digit=digit: item["phase"] == "presentation"
            and item["digit"] == digit
        )
        for digit in range(10)
    }
    expected_steps = len(results) * args.timed_steps
    if len(intervals) != expected_steps:
        raise ValueError(f"collected {len(intervals)} steps, expected {expected_steps}")
    provenance = {
        "snapshot_directory": str(args.snapshots),
        "generator": str(args.generator),
        "generator_sha256": sha256_file(args.generator),
        "payload": str(args.payload),
        "payload_sha256": sha256_file(args.payload),
        "simulator": str(args.simulator),
        "simulator_sha256": sha256_file(args.simulator),
        "labels": str(args.labels),
        "labels_sha256": sha256_file(args.labels),
    }
    selection = args.snapshots / "selection.json"
    if selection.is_file():
        provenance["selection"] = str(selection)
        provenance["selection_sha256"] = sha256_file(selection)
    return {
        "schema": "activen-mnist-snapshot-benchmark-v1",
        "provenance": provenance,
        "snapshot_count": len(results),
        "presentation_snapshot_count": sum(
            result["phase"] == "presentation" for result in results
        ),
        "rest_snapshot_count": sum(result["phase"] == "rest" for result in results),
        "warmup_steps_per_snapshot": args.warmup_steps,
        "timed_steps_per_snapshot": args.timed_steps,
        "soft_threads_per_simulator": args.soft_threads,
        "parallel_simulators": args.jobs,
        "horizontal_partition": args.horizontal_partition,
        "mapping_identical_across_snapshots": len(placement_signatures) == 1,
        "all_intervals_strictly_hot": all(
            item["strictly_hot"] for item in intervals
        ),
        "successful_system_results": sum(
            result["system_result"] == 0 for result in results
        ),
        "input_load": summarize_input_load(results, args),
        "total_simulator_wall_seconds": sum(
            result["wall_seconds"] for result in results
        ),
        "groups": groups,
        "presentation_by_digit": presentation_by_digit,
        "results": [
            {
                "snapshot_index": result["snapshot_index"],
                "phase": result["phase"],
                "accepted_sample": result["accepted_sample"],
                "attempt_index": result["attempt_index"],
                "phase_tick": result["phase_tick"],
                "frame_index": result["frame_index"],
                "digit": result["digit"],
                "checksum": result["checksum"],
                "system_cycles": result["system_cycles"],
                "system_timer_cycles": result["system_timer_cycles"],
                "wall_seconds": result["wall_seconds"],
            }
            for result in sorted(results, key=lambda item: item["snapshot_index"])
        ],
    }


def parse_args() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--snapshots", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--generator", type=Path,
        default=repository / "datagen" / "mnist_train.py",
    )
    parser.add_argument(
        "--payload", type=Path,
        default=repository / "sim" / "payloads" / "sys" / "mnist_train.bin",
    )
    parser.add_argument(
        "--simulator", type=Path, default=repository / "sim" / "build" / "sim_system"
    )
    parser.add_argument("--warmup-steps", type=int, default=4)
    parser.add_argument("--timed-steps", type=int, default=12)
    parser.add_argument("--max-cycles", type=int, default=3_000_000)
    parser.add_argument("--soft-threads", type=int, default=8)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--snapshot-limit", type=int)
    parser.add_argument("--input-pus", type=int, default=196)
    parser.add_argument("--neuron-pus", type=int, default=224)
    parser.add_argument(
        "--horizontal-partition",
        choices=("none", "hot-partial", "full", "full-4"),
        default="none",
    )
    parser.add_argument(
        "--labels", type=Path,
        default=repository.parent / "stdp-experiments" / "data" / "mnist" /
        "train-labels-idx1-ubyte",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    repository = Path(__file__).resolve().parents[1]
    args.snapshots = args.snapshots.resolve()
    args.output = args.output.resolve()
    args.generator = args.generator.resolve()
    args.payload = args.payload.resolve()
    args.simulator = args.simulator.resolve()
    args.labels = args.labels.resolve()
    if args.jobs <= 0 or args.soft_threads <= 0:
        raise SystemExit("--jobs and --soft-threads must be positive")
    if args.snapshot_limit is not None and args.snapshot_limit <= 0:
        raise SystemExit("--snapshot-limit must be positive")
    if args.jobs * args.soft_threads > (os.cpu_count() or 1):
        raise SystemExit("parallel simulators would oversubscribe host CPUs")
    if args.warmup_steps < 0 or args.timed_steps <= 0:
        raise SystemExit("invalid warmup or timed step count")
    if (not args.generator.is_file() or not args.payload.is_file()
            or not args.simulator.is_file()):
        raise SystemExit("generator, payload, or simulator does not exist")
    snapshots = sorted(args.snapshots.glob("snapshot_*.npz"))
    if not snapshots:
        raise SystemExit(f"no snapshots found in {args.snapshots}")
    if args.snapshot_limit is not None:
        snapshots = snapshots[:args.snapshot_limit]
    args.output.mkdir(parents=True, exist_ok=True)
    labels = read_idx_labels(args.labels)
    started = time.monotonic()
    results: list[dict[str, Any]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as executor:
        pending = {
            executor.submit(
                benchmark_one,
                snapshot,
                repository=repository,
                generator=args.generator,
                output=args.output,
                payload=args.payload,
                simulator=args.simulator,
                warmup_steps=args.warmup_steps,
                timed_steps=args.timed_steps,
                max_cycles=args.max_cycles,
                soft_threads=args.soft_threads,
                input_pus=args.input_pus,
                neuron_pus=args.neuron_pus,
                horizontal_partition=args.horizontal_partition,
                labels=labels,
            ): snapshot
            for snapshot in snapshots
        }
        for future in concurrent.futures.as_completed(pending):
            snapshot = pending[future]
            result = future.result()
            results.append(result)
            hot = sum(item["strictly_hot"] for item in result["intervals"])
            exc = sum(item["excitatory_spikes"] for item in result["intervals"])
            print(
                f"COMPLETE snapshot={result['snapshot_index']:03d} "
                f"phase={result['phase']} hot={hot}/{args.timed_steps} "
                f"exc_spikes={exc} completed={len(results)}/{len(snapshots)}",
                flush=True,
            )
    summary = make_summary(results, args)
    summary["driver_invocation_wall_seconds"] = time.monotonic() - started
    atomic_json(args.output / "summary.json", summary)
    print(
        f"SUMMARY snapshots={len(results)} steps={summary['groups']['all']['steps']} "
        f"hot_steps={summary['groups']['strictly_hot']['steps']} "
        f"mean_cycles={summary['groups']['all']['cycle_mean']:.3f}",
        flush=True,
    )


if __name__ == "__main__":
    main()
