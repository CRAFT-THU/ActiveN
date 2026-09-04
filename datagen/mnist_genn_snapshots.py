#!/usr/bin/env python3
"""Export realistic ActiveN MNIST runtime snapshots from a GeNN replay."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import re
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import numpy as np


SNAPSHOT_SCHEMA = "activen-mnist-runtime-snapshot-v1"
SELECTION_SCHEMA = "activen-mnist-snapshot-selection-v1"


@dataclass(frozen=True)
class Attempt:
    index: int
    accepted_sample: int
    frame_index: int
    intensity: float
    excitatory_spikes: int
    retry: bool


@dataclass(frozen=True)
class Selection:
    index: int
    attempt_index: int
    phase: str
    phase_tick: int


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def state_digest(weights: np.ndarray, theta: np.ndarray) -> str:
    digest = hashlib.sha256()
    digest.update(np.asarray(weights, dtype="<f4", order="C").tobytes())
    digest.update(np.asarray(theta, dtype="<f4", order="C").tobytes())
    return digest.hexdigest()


def validate_build_seed(build_path: Path, expected_seed: int) -> None:
    init_source = build_path / "zd3_genn_CODE" / "init.cc"
    runner = build_path / "zd3_genn_CODE" / "librunner.so"
    if not init_source.is_file() or not runner.is_file():
        raise ValueError(f"incomplete GeNN build: {build_path}")
    assignments = re.findall(
        r"deviceRNGSeed\s*=\s*([0-9]+)\s*;",
        init_source.read_text(encoding="ascii"),
    )
    if not assignments or int(assignments[-1]) != expected_seed:
        actual = assignments[-1] if assignments else "unknown"
        raise ValueError(
            f"GeNN build seed {actual} does not match --model-seed {expected_seed}"
        )


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("x", encoding="ascii") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def load_dependencies(genn_root: Path) -> tuple[Any, ...]:
    reimpl = genn_root / "reimpl"
    vendored = genn_root / "3rdparty" / "genn"
    for path in (str(vendored), str(reimpl)):
        if path not in sys.path:
            sys.path.insert(0, path)
    from backends.genn_backend import GeNNNetwork
    from zd3.constants import MODEL
    from zd3.io import load_checkpoint, load_mnist
    from zd3.variants import connectivity_mask, get_variant

    return GeNNNetwork, MODEL, load_checkpoint, load_mnist, connectivity_mask, get_variant


def make_network(
    args: argparse.Namespace,
    checkpoint: Any,
    GeNNNetwork: Any,
    MODEL: Any,
    connectivity_mask: Any,
    get_variant: Any,
    *,
    build_path: Path,
    reuse_build: Path | None,
) -> Any:
    variant = get_variant("triplet-dense")
    return GeNNNetwork(
        weights=checkpoint.weights.copy(),
        theta_mv=checkpoint.theta_mv.copy(),
        plasticity=True,
        inhibition=MODEL.train_inhibition,
        seed=args.model_seed,
        backend="cuda",
        build_path=build_path,
        variant=variant,
        structural_mask=connectivity_mask(variant),
        precision="float",
        parallelism="postsynaptic",
        num_threads_per_spike=1,
        timing_enabled=False,
        reuse_build=reuse_build,
    )


def discover_attempts(
    args: argparse.Namespace,
    checkpoint: Any,
    data: Any,
    dependencies: tuple[Any, ...],
) -> tuple[list[Attempt], str]:
    GeNNNetwork, MODEL, _, _, connectivity_mask, get_variant = dependencies
    network = make_network(
        args,
        checkpoint,
        GeNNNetwork,
        MODEL,
        connectivity_mask,
        get_variant,
        build_path=args.build_path,
        reuse_build=args.reuse_build,
    )
    attempts: list[Attempt] = []
    accepted = checkpoint.accepted_samples
    started = time.monotonic()
    try:
        while accepted < args.end_sample:
            intensity = MODEL.initial_intensity
            while True:
                frame_index = accepted % len(data.images)
                network.normalize()
                network.set_image(data.images[frame_index], intensity)
                counts = network.run_stimulus()
                retry = int(counts.sum()) < MODEL.minimum_exc_spikes
                network.run_rest()
                attempts.append(
                    Attempt(
                        index=len(attempts),
                        accepted_sample=accepted,
                        frame_index=frame_index,
                        intensity=float(intensity),
                        excitatory_spikes=int(counts.sum()),
                        retry=retry,
                    )
                )
                if not retry:
                    accepted += 1
                    break
                intensity += MODEL.intensity_increment
                if intensity > args.max_intensity:
                    raise RuntimeError(
                        f"sample {accepted} exceeded intensity {args.max_intensity}"
                    )
            completed = accepted - checkpoint.accepted_samples
            if completed % args.progress_interval == 0:
                elapsed = time.monotonic() - started
                print(
                    f"DISCOVER accepted={accepted} attempts={len(attempts)} "
                    f"elapsed_seconds={elapsed:.1f}",
                    flush=True,
                )
        final_digest = state_digest(network.weights(), network.theta_mv())
    finally:
        network.close()
    return attempts, final_digest


def choose_selections(
    attempts: list[Attempt],
    *,
    presentation_count: int,
    rest_count: int,
    window_steps: int,
    stimulus_ticks: int,
    rest_ticks: int,
    seed: int,
) -> list[Selection]:
    presentation_starts = stimulus_ticks - window_steps + 1
    rest_starts = rest_ticks - window_steps + 1
    if presentation_starts <= 0 or rest_starts <= 0:
        raise ValueError("snapshot window is longer than a presentation phase")
    rng = random.Random(seed)

    def select(count: int, starts: int, phase: str) -> list[Selection]:
        ranks = rng.sample(range(len(attempts) * starts), count)
        return [
            Selection(
                index=-1,
                attempt_index=rank // starts,
                phase=phase,
                phase_tick=rank % starts,
            )
            for rank in ranks
        ]

    unordered = select(presentation_count, presentation_starts, "presentation")
    unordered.extend(select(rest_count, rest_starts, "rest"))
    unordered.sort(key=lambda item: (item.attempt_index, item.phase, item.phase_tick))
    return [
        Selection(
            index=index,
            attempt_index=item.attempt_index,
            phase=item.phase,
            phase_tick=item.phase_tick,
        )
        for index, item in enumerate(unordered)
    ]


def pull_array(population: Any, name: str, dtype: Any) -> np.ndarray:
    variable = population.vars[name]
    variable.pull_from_device()
    return np.asarray(variable.view, dtype=dtype).copy()


def pull_output(synapses: Any) -> np.ndarray:
    if synapses.out_post is None:
        raise RuntimeError(f"GeNN fused the output buffer for {synapses.name}")
    synapses.out_post.pull_from_device()
    return np.asarray(synapses.out_post.view, dtype=np.float32).reshape(-1).copy()


def trace_from_spike_times(population: Any, now_ms: float, tau_ms: float) -> np.ndarray:
    if population.spike_times is None:
        raise RuntimeError(f"GeNN did not expose spike times for {population.name}")
    population.spike_times.pull_from_device()
    spike_times = np.asarray(population.spike_times.view, dtype=np.float64).reshape(-1)
    age = np.maximum(0.0, now_ms - spike_times)
    return np.where(spike_times > -1.0e20, np.exp(-age / tau_ms), 0.0).astype(
        np.float32
    )


def snapshot_rng(seed: int, selection: Selection, count: int) -> np.ndarray:
    phase = 0 if selection.phase == "presentation" else 1
    generator = np.random.default_rng(
        np.random.SeedSequence(
            [seed, selection.attempt_index, phase, selection.phase_tick]
        )
    )
    return generator.integers(1, 1 << 32, size=count, dtype=np.uint32)


def save_snapshot(
    path: Path,
    *,
    network: Any,
    MODEL: Any,
    selection: Selection,
    attempt: Attempt,
    pixels: np.ndarray,
    label: int,
    placement_scores: np.ndarray,
    source_checkpoint: Path,
    source_checkpoint_sha256: str,
    poisson_seed: int,
    window_steps: int,
) -> None:
    # GeNN consumes these one-step synaptic buffers in its next neuron update.
    # Folding them into conductance avoids dropping already-delivered activity
    # when the state is transferred to ActiveN's phase-ordered timestep.
    exc_ge = pull_array(network.exc, "ge", np.float32)
    exc_ge += pull_output(network.feedforward)
    exc_gi = pull_array(network.exc, "gi", np.float32)
    exc_gi += pull_output(network.inh_to_exc)
    inh_ge = pull_array(network.inh, "ge", np.float32)
    inh_ge += pull_output(network.exc_to_inh)

    trace_time = max(0.0, float(network.model.t) - MODEL.dt_ms)
    input_pre_trace = trace_from_spike_times(
        network.inputs, trace_time, MODEL.pre_tau_ms
    )
    exc_post1 = trace_from_spike_times(network.exc, trace_time, MODEL.post1_tau_ms)
    exc_post2 = trace_from_spike_times(network.exc, trace_time, MODEL.post2_tau_ms)
    input_intensity = attempt.intensity if selection.phase == "presentation" else 0.0
    metadata = {
        "schema": SNAPSHOT_SCHEMA,
        "selection_index": selection.index,
        "attempt_index": selection.attempt_index,
        "accepted_sample": attempt.accepted_sample,
        "frame_index": attempt.frame_index,
        "label": label,
        "attempt_intensity": attempt.intensity,
        "input_intensity": input_intensity,
        "phase": selection.phase,
        "phase_tick": selection.phase_tick,
        "window_steps": window_steps,
        "source_model_time_ms": float(network.model.t),
        "source_checkpoint": str(source_checkpoint),
        "source_checkpoint_sha256": source_checkpoint_sha256,
        "source_precision": "float",
        "pending_synaptic_inputs_folded_into_conductance": True,
        "poisson_state_source": "deterministic-activeN-xorshift-seed",
    }
    metadata_json = json.dumps(
        metadata, sort_keys=True, separators=(",", ":")
    ).encode("ascii")
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("xb") as stream:
        np.savez_compressed(
            stream,
            weights=np.asarray(network.weights(), dtype=np.float32, order="C"),
            theta_mv=pull_array(network.exc, "theta", np.float32),
            input_pre_trace=input_pre_trace,
            input_rng_state=snapshot_rng(poisson_seed, selection, MODEL.n_input),
            exc_v=pull_array(network.exc, "V", np.float32),
            exc_ge=exc_ge,
            exc_gi=exc_gi,
            exc_post1=exc_post1,
            exc_post2=exc_post2,
            exc_refractory=pull_array(network.exc, "refrac", np.uint32),
            inh_v=pull_array(network.inh, "V", np.float32),
            inh_ge=inh_ge,
            inh_gi=pull_array(network.inh, "gi", np.float32),
            inh_refractory=pull_array(network.inh, "refrac", np.uint32),
            pixels=np.asarray(pixels, dtype=np.uint8),
            placement_scores=np.asarray(placement_scores, dtype=np.float32),
            metadata_json=np.frombuffer(metadata_json, dtype=np.uint8),
        )
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def capture_snapshots(
    args: argparse.Namespace,
    checkpoint: Any,
    data: Any,
    attempts: list[Attempt],
    selections: list[Selection],
    discovery_digest: str,
    dependencies: tuple[Any, ...],
) -> str:
    GeNNNetwork, MODEL, _, _, connectivity_mask, get_variant = dependencies
    by_point: dict[tuple[int, str, int], Selection] = {
        (item.attempt_index, item.phase, item.phase_tick): item
        for item in selections
    }
    if len(by_point) != len(selections):
        raise ValueError("duplicate snapshot selections")

    placement_scores = data.images.mean(axis=0, dtype=np.float64)
    checkpoint_sha256 = sha256_file(args.checkpoint)
    network = make_network(
        args,
        checkpoint,
        GeNNNetwork,
        MODEL,
        connectivity_mask,
        get_variant,
        build_path=args.build_path,
        reuse_build=args.build_path,
    )
    accepted = checkpoint.accepted_samples
    attempt_index = 0
    count_baseline = np.zeros(MODEL.n_exc, dtype=np.uint32)
    captured = 0
    started = time.monotonic()
    try:
        while accepted < args.end_sample:
            expected = attempts[attempt_index]
            if expected.accepted_sample != accepted:
                raise RuntimeError("capture replay diverged before an attempt")
            network.normalize()
            image = data.images[expected.frame_index]
            network.set_image(image, expected.intensity)
            for tick in range(MODEL.stimulus_ticks):
                selection = by_point.get((attempt_index, "presentation", tick))
                if selection is not None:
                    save_snapshot(
                        args.output / f"snapshot_{selection.index:03d}.npz",
                        network=network,
                        MODEL=MODEL,
                        selection=selection,
                        attempt=expected,
                        pixels=image,
                        label=int(data.labels[expected.frame_index]),
                        placement_scores=placement_scores,
                        source_checkpoint=args.checkpoint,
                        source_checkpoint_sha256=checkpoint_sha256,
                        poisson_seed=args.poisson_seed,
                        window_steps=args.window_steps,
                    )
                    captured += 1
                network.model.step_time()

            spike_count = network.exc.vars["spikeCount"]
            spike_count.pull_from_device()
            current = np.asarray(spike_count.view, dtype=np.uint32).copy()
            counts = current.astype(np.int64) - count_baseline.astype(np.int64)
            count_baseline = current
            retry = int(counts.sum()) < MODEL.minimum_exc_spikes
            if int(counts.sum()) != expected.excitatory_spikes or retry != expected.retry:
                raise RuntimeError(
                    f"capture replay diverged at attempt {attempt_index}: "
                    f"expected spikes/retry {expected.excitatory_spikes}/{expected.retry}, "
                    f"observed {int(counts.sum())}/{retry}"
                )

            network.set_zero_input()
            zero_pixels = np.zeros(MODEL.n_input, dtype=np.uint8)
            for tick in range(MODEL.rest_ticks):
                selection = by_point.get((attempt_index, "rest", tick))
                if selection is not None:
                    save_snapshot(
                        args.output / f"snapshot_{selection.index:03d}.npz",
                        network=network,
                        MODEL=MODEL,
                        selection=selection,
                        attempt=expected,
                        pixels=zero_pixels,
                        label=int(data.labels[expected.frame_index]),
                        placement_scores=placement_scores,
                        source_checkpoint=args.checkpoint,
                        source_checkpoint_sha256=checkpoint_sha256,
                        poisson_seed=args.poisson_seed,
                        window_steps=args.window_steps,
                    )
                    captured += 1
                network.model.step_time()

            if not expected.retry:
                accepted += 1
            attempt_index += 1
            if attempt_index % args.progress_interval == 0:
                elapsed = time.monotonic() - started
                print(
                    f"CAPTURE attempts={attempt_index}/{len(attempts)} "
                    f"snapshots={captured}/{len(selections)} elapsed_seconds={elapsed:.1f}",
                    flush=True,
                )

        final_digest = state_digest(network.weights(), network.theta_mv())
    finally:
        network.close()
    if attempt_index != len(attempts) or captured != len(selections):
        raise RuntimeError("capture replay did not consume its complete schedule")
    if final_digest != discovery_digest:
        raise RuntimeError(
            f"replay final digest differs: {final_digest} != {discovery_digest}"
        )
    return final_digest


def parse_args() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[1]
    genn_root = repository.parent / "stdp-experiments"
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--genn-root", type=Path, default=genn_root)
    parser.add_argument("--data-path", type=Path, default=genn_root / "data" / "mnist")
    build = parser.add_mutually_exclusive_group(required=True)
    build.add_argument("--build-path", type=Path)
    build.add_argument("--reuse-build", type=Path)
    parser.add_argument("--end-sample", type=int, default=40000)
    parser.add_argument("--snapshots", type=int, default=100)
    parser.add_argument("--presentation-snapshots", type=int, default=70)
    parser.add_argument("--window-steps", type=int, default=16)
    parser.add_argument("--selection-seed", type=int, default=19260817)
    parser.add_argument("--poisson-seed", type=int, default=0x19260817)
    parser.add_argument("--model-seed", type=int, default=19260817)
    parser.add_argument("--max-intensity", type=float, default=20.0)
    parser.add_argument("--progress-interval", type=int, default=250)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f"output directory is not empty: {args.output}")
    args.output.mkdir(parents=True, exist_ok=True)
    if args.reuse_build is not None:
        args.build_path = args.reuse_build.resolve()
        validate_build_seed(args.build_path, args.model_seed)
    elif args.build_path.exists():
        raise SystemExit(f"fresh GeNN build path already exists: {args.build_path}")
    else:
        args.build_path = args.build_path.resolve()
    if args.model_seed == 0:
        raise SystemExit("--model-seed must be nonzero for reproducible replay")
    dependencies = load_dependencies(args.genn_root)
    _, MODEL, load_checkpoint, load_mnist, _, _ = dependencies
    checkpoint = load_checkpoint(args.checkpoint)
    if checkpoint.accepted_samples >= args.end_sample:
        raise SystemExit("--end-sample must be after the source checkpoint")
    if args.snapshots <= 0:
        raise SystemExit("--snapshots must be positive")
    if not 0 <= args.presentation_snapshots <= args.snapshots:
        raise SystemExit("--presentation-snapshots must be between zero and --snapshots")
    if args.progress_interval <= 0:
        raise SystemExit("--progress-interval must be positive")

    data = load_mnist(args.data_path, "train")
    attempts, discovery_digest = discover_attempts(
        args, checkpoint, data, dependencies
    )
    validate_build_seed(args.build_path, args.model_seed)
    selections = choose_selections(
        attempts,
        presentation_count=args.presentation_snapshots,
        rest_count=args.snapshots - args.presentation_snapshots,
        window_steps=args.window_steps,
        stimulus_ticks=MODEL.stimulus_ticks,
        rest_ticks=MODEL.rest_ticks,
        seed=args.selection_seed,
    )
    schedule = {
        "schema": SELECTION_SCHEMA,
        "checkpoint": str(args.checkpoint),
        "checkpoint_sha256": sha256_file(args.checkpoint),
        "checkpoint_accepted_sample": checkpoint.accepted_samples,
        "end_sample": args.end_sample,
        "model_seed": args.model_seed,
        "genn_build": str(args.build_path),
        "genn_runner_sha256": sha256_file(
            args.build_path / "zd3_genn_CODE" / "librunner.so"
        ),
        "selection_seed": args.selection_seed,
        "poisson_seed": args.poisson_seed,
        "window_steps": args.window_steps,
        "attempt_count": len(attempts),
        "retry_count": sum(item.retry for item in attempts),
        "discovery_final_state_sha256": discovery_digest,
        "attempts": [asdict(item) for item in attempts],
        "selections": [asdict(item) for item in selections],
    }
    write_json(args.output / "selection.json", schedule)
    final_digest = capture_snapshots(
        args,
        checkpoint,
        data,
        attempts,
        selections,
        discovery_digest,
        dependencies,
    )
    write_json(
        args.output / "complete.json",
        {
            "schema": SNAPSHOT_SCHEMA,
            "snapshot_count": len(selections),
            "presentation_snapshot_count": args.presentation_snapshots,
            "rest_snapshot_count": args.snapshots - args.presentation_snapshots,
            "replay_final_state_sha256": final_digest,
        },
    )
    print(
        f"COMPLETE attempts={len(attempts)} snapshots={len(selections)} "
        f"final_state_sha256={final_digest}",
        flush=True,
    )


if __name__ == "__main__":
    main()
