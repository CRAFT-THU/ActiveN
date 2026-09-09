#!/usr/bin/env python3
"""Export realistic ActiveN MNIST runtime snapshots during GeNN training."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import re
import sys
import time
from dataclasses import asdict, dataclass, replace
from pathlib import Path
from typing import Any

import numpy as np


SNAPSHOT_SCHEMA = "activen-mnist-runtime-snapshot-v1"
SELECTION_SCHEMA = "activen-mnist-snapshot-selection-v1"
RULES = ("one-trace", "two-trace", "three-trace")


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


class PhaseReservoir:
    """Algorithm R over eligible phase starts, retaining fixed output slots."""

    def __init__(self, capacity: int, phase: str, starts: int,
                 rng: random.Random, index_offset: int = 0):
        if capacity < 0 or starts <= 0:
            raise ValueError("reservoir capacity must be nonnegative and starts positive")
        self.capacity = capacity
        self.phase = phase
        self.starts = starts
        self.rng = rng
        self.index_offset = index_offset
        self.seen = 0
        self.items: list[Selection] = []

    def plan(self, attempt_index: int) -> list[Selection]:
        if not self.capacity:
            self.seen += self.starts
            return []
        updates: dict[int, Selection] = {}
        for tick in range(self.starts):
            self.seen += 1
            slot = self.seen - 1 if self.seen <= self.capacity else self.rng.randrange(self.seen)
            if slot >= self.capacity:
                continue
            selection = Selection(self.index_offset + slot, attempt_index, self.phase, tick)
            if slot == len(self.items):
                self.items.append(selection)
            else:
                self.items[slot] = selection
            updates[slot] = selection
        # Earlier replacements of the same slot cannot survive this phase.
        return sorted(updates.values(), key=lambda item: item.phase_tick)


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


def rule_variant(rule: str, get_variant: Any) -> Any:
    if rule == "three-trace":
        return get_variant("triplet-dense")
    variant = get_variant("one-trace-dense")
    if rule == "two-trace":
        return replace(variant, name="two-trace-dense", learning_rule="two-trace-power")
    if rule != "one-trace":
        raise ValueError(f"unsupported rule: {rule}")
    return variant


def validate_checkpoint_rule(checkpoint: Any, variant: Any) -> None:
    source = checkpoint.manifest.get("variant", {})
    for key in ("learning_rule", "topology", "weight_max", "potentiation_rate"):
        if key in source and source[key] != getattr(variant, key):
            raise ValueError(f"checkpoint {key} does not match selected rule")


def initial_state(args: argparse.Namespace, dependencies: tuple[Any, ...]) -> Any:
    _, MODEL, load_checkpoint, _, _, get_variant = dependencies
    variant = rule_variant(args.rule, get_variant)
    if args.checkpoint is not None:
        checkpoint = load_checkpoint(args.checkpoint)
        validate_checkpoint_rule(checkpoint, variant)
        return checkpoint
    from zd3.io import PortableCheckpoint, load_reference_triplets
    from zd3.variants import prepare_initial_weights

    weights, _ = prepare_initial_weights(
        load_reference_triplets(args.initial_weights, MODEL.n_input, MODEL.n_exc),
        variant,
    )
    return PortableCheckpoint(
        weights=weights,
        theta_mv=np.full(MODEL.n_exc, MODEL.theta_initial_mv, dtype=np.float64),
        accepted_samples=0,
        manifest={"variant": variant.as_dict()},
    )


def save_training_checkpoint(args: argparse.Namespace, network: Any, accepted: int) -> None:
    from zd3.io import save_checkpoint

    save_checkpoint(
        args.output / "checkpoints" / f"checkpoint_{accepted:06d}.npz",
        weights=network.weights(), theta_mv=network.theta_mv(),
        accepted_samples=accepted,
        manifest={
            "backend": "genn-cuda", "precision": "float",
            "rng_seed": args.model_seed, "variant": network.variant.as_dict(),
            "source_run": str(args.output),
            "runtime_state_scope": "weights-and-theta-only",
        },
    )


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
    variant = rule_variant(args.rule, get_variant)
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
                network.validate_runtime(counts, args.runaway_spikes)
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
            if accepted % args.checkpoint_interval == 0 or accepted == args.end_sample:
                save_training_checkpoint(args, network, accepted)
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


def pull_traces(network: Any, MODEL: Any, rule: str) -> tuple[np.ndarray, ...]:
    if rule == "three-trace":
        trace_time = max(0.0, float(network.model.t) - MODEL.dt_ms)
        return (
            trace_from_spike_times(network.inputs, trace_time, MODEL.pre_tau_ms),
            trace_from_spike_times(network.exc, trace_time, MODEL.post1_tau_ms),
            trace_from_spike_times(network.exc, trace_time, MODEL.post2_tau_ms),
        )
    if rule not in RULES:
        raise ValueError(f"unsupported rule: {rule}")
    # Additive traces contain contributions from more than the latest spike.
    pre = network.feedforward.pre_vars["x"]
    pre.pull_from_device()
    x = np.asarray(pre.view, dtype=np.float32).reshape(-1).copy()
    y = np.zeros(MODEL.n_exc, dtype=np.float32)
    if rule == "two-trace":
        post = network.feedforward.post_vars["y"]
        post.pull_from_device()
        y = np.asarray(post.view, dtype=np.float32).reshape(-1).copy()
    if x.shape != (MODEL.n_input,) or y.shape != (MODEL.n_exc,):
        raise ValueError("GeNN additive trace shape does not match neuron populations")
    if not np.all(np.isfinite(x)) or not np.all(np.isfinite(y)):
        raise ValueError("GeNN additive traces contain non-finite state")
    if np.any(x < 0.0) or np.any(y < 0.0):
        raise ValueError("GeNN additive traces must be nonnegative")
    return x, y, np.zeros(MODEL.n_exc, dtype=np.float32)


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
    source_checkpoint: Path | None,
    source_checkpoint_sha256: str | None,
    poisson_seed: int,
    window_steps: int,
    rule: str = "three-trace",
    source_initial_weights: Path | None = None,
    collection_mode: str = "replay",
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

    input_pre_trace, exc_post1, exc_post2 = pull_traces(network, MODEL, rule)
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
        "source_checkpoint": str(source_checkpoint) if source_checkpoint else None,
        "source_checkpoint_sha256": source_checkpoint_sha256,
        "source_initial_weights": str(source_initial_weights) if source_initial_weights else None,
        "source_initial_weights_sha256": (
            sha256_file(source_initial_weights) if source_initial_weights else None
        ),
        "rule": rule,
        "collection_mode": collection_mode,
        "variant": network.variant.as_dict(),
        "trace_source": (
            "nearest-spike-times" if rule == "three-trace" else "live-additive-state"
        ),
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
    checkpoint_sha256 = sha256_file(args.checkpoint) if args.checkpoint else None
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
                        rule=args.rule,
                        source_initial_weights=args.initial_weights,
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
                        rule=args.rule,
                        source_initial_weights=args.initial_weights,
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


def train_and_capture(
    args: argparse.Namespace, checkpoint: Any, data: Any, dependencies: tuple[Any, ...],
) -> tuple[list[Attempt], list[Selection], str, dict[str, int]]:
    GeNNNetwork, MODEL, _, _, connectivity_mask, get_variant = dependencies
    rng = random.Random(args.selection_seed)
    reservoirs = {
        "presentation": PhaseReservoir(
            args.presentation_snapshots, "presentation",
            MODEL.stimulus_ticks - args.window_steps + 1, rng,
        ),
        "rest": PhaseReservoir(
            args.snapshots - args.presentation_snapshots, "rest",
            MODEL.rest_ticks - args.window_steps + 1, rng, args.presentation_snapshots,
        ),
    }
    placement_scores = data.images.mean(axis=0, dtype=np.float64)
    checkpoint_sha256 = sha256_file(args.checkpoint) if args.checkpoint else None
    network = make_network(
        args, checkpoint, GeNNNetwork, MODEL, connectivity_mask, get_variant,
        build_path=args.build_path, reuse_build=args.reuse_build,
    )
    snapshot_writes = 0

    def run_phase(phase: str, ticks: int, attempt: Attempt, pixels: np.ndarray) -> None:
        nonlocal snapshot_writes
        current_tick = 0
        for selection in reservoirs[phase].plan(attempt.index):
            network._step(selection.phase_tick - current_tick)
            save_snapshot(
                args.output / f"snapshot_{selection.index:03d}.npz",
                network=network, MODEL=MODEL, selection=selection, attempt=attempt,
                pixels=pixels, label=int(data.labels[attempt.frame_index]),
                placement_scores=placement_scores, source_checkpoint=args.checkpoint,
                source_checkpoint_sha256=checkpoint_sha256,
                poisson_seed=args.poisson_seed, window_steps=args.window_steps,
                rule=args.rule, source_initial_weights=args.initial_weights,
                collection_mode="single-pass",
            )
            snapshot_writes += 1
            current_tick = selection.phase_tick
        network._step(ticks - current_tick)

    attempts: list[Attempt] = []
    accepted = checkpoint.accepted_samples
    count_baseline = np.zeros(MODEL.n_exc, dtype=np.uint32)
    zero_pixels = np.zeros(MODEL.n_input, dtype=np.uint8)
    started = time.monotonic()
    try:
        while accepted < args.end_sample:
            intensity = MODEL.initial_intensity
            while True:
                frame_index = accepted % len(data.images)
                # Presentation exports use only the fields known at phase entry.
                attempt = Attempt(len(attempts), accepted, frame_index, float(intensity), 0, False)
                network.normalize()
                network.set_image(data.images[frame_index], intensity)
                run_phase("presentation", MODEL.stimulus_ticks, attempt, data.images[frame_index])
                current = pull_array(network.exc, "spikeCount", np.uint32)
                counts = current.astype(np.int64) - count_baseline.astype(np.int64)
                count_baseline = current
                network.validate_runtime(counts, args.runaway_spikes)
                attempt = replace(attempt, excitatory_spikes=int(counts.sum()),
                                  retry=int(counts.sum()) < MODEL.minimum_exc_spikes)
                network.set_zero_input()
                run_phase("rest", MODEL.rest_ticks, attempt, zero_pixels)
                network.exc.vars["spikeCount"].pull_from_device()
                attempts.append(attempt)
                if not attempt.retry:
                    accepted += 1
                    break
                intensity += MODEL.intensity_increment
                if intensity > args.max_intensity:
                    raise RuntimeError(f"sample {accepted} exceeded intensity {args.max_intensity}")
            if accepted % args.checkpoint_interval == 0 or accepted == args.end_sample:
                save_training_checkpoint(args, network, accepted)
            if (accepted - checkpoint.accepted_samples) % args.progress_interval == 0:
                print(f"TRAIN_CAPTURE accepted={accepted} attempts={len(attempts)} "
                      f"retained={sum(len(r.items) for r in reservoirs.values())}/{args.snapshots} "
                      f"snapshot_writes={snapshot_writes} elapsed_seconds={time.monotonic()-started:.1f}",
                      flush=True)
        final_digest = state_digest(network.weights(), network.theta_mv())
    finally:
        network.close()
    selections = sorted((s for r in reservoirs.values() for s in r.items), key=lambda s: s.index)
    if len(selections) != args.snapshots:
        raise RuntimeError("not enough eligible timestep starts to fill snapshot reservoirs")
    return attempts, selections, final_digest, {
        "presentation": reservoirs["presentation"].seen,
        "rest": reservoirs["rest"].seen, "snapshot_writes": snapshot_writes,
    }


def parse_args() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[1]
    genn_root = repository.parent / "stdp-experiments"
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--checkpoint", type=Path)
    source.add_argument("--initial-weights", type=Path)
    parser.add_argument("--rule", choices=RULES, default="three-trace")
    parser.add_argument("--collection-mode", choices=("single-pass", "replay"),
                        default="single-pass")
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
    parser.add_argument("--checkpoint-interval", type=int, default=10000)
    parser.add_argument("--runaway-spikes", type=int, default=5000)
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
        raise SystemExit("--model-seed must be nonzero for a seeded GeNN RNG stream")
    dependencies = load_dependencies(args.genn_root)
    _, MODEL, load_checkpoint, load_mnist, _, _ = dependencies
    checkpoint = initial_state(args, dependencies)
    if checkpoint.accepted_samples >= args.end_sample:
        raise SystemExit("--end-sample must be after the source checkpoint")
    if args.snapshots <= 0:
        raise SystemExit("--snapshots must be positive")
    if not 0 <= args.presentation_snapshots <= args.snapshots:
        raise SystemExit("--presentation-snapshots must be between zero and --snapshots")
    if args.progress_interval <= 0:
        raise SystemExit("--progress-interval must be positive")
    if args.checkpoint_interval <= 0 or args.runaway_spikes <= 0:
        raise SystemExit("checkpoint interval and runaway spike bound must be positive")
    if not 0 < args.window_steps <= min(MODEL.stimulus_ticks, MODEL.rest_ticks):
        raise SystemExit("--window-steps must be positive and fit both phases")

    data = load_mnist(args.data_path, "train")
    write_json(args.output / "manifest.json", {
        "rule": args.rule, "variant": checkpoint.manifest.get("variant"),
        "checkpoint": str(args.checkpoint) if args.checkpoint else None,
        "checkpoint_sha256": sha256_file(args.checkpoint) if args.checkpoint else None,
        "initial_weights": str(args.initial_weights) if args.initial_weights else None,
        "initial_weights_sha256": sha256_file(args.initial_weights) if args.initial_weights else None,
        "start_sample": checkpoint.accepted_samples, "end_sample": args.end_sample,
        "model": MODEL.as_dict(), "precision": "float", "model_seed": args.model_seed,
        "selection_seed": args.selection_seed, "poisson_seed": args.poisson_seed,
        "snapshots": args.snapshots, "presentation_snapshots": args.presentation_snapshots,
        "window_steps": args.window_steps, "command": sys.argv,
        "collection_mode": args.collection_mode,
        "cuda_trajectory_bitwise_reproducible": False,
    })
    sampling_stats = None
    if args.collection_mode == "single-pass":
        attempts, selections, final_digest, sampling_stats = train_and_capture(
            args, checkpoint, data, dependencies,
        )
    else:
        attempts, final_digest = discover_attempts(args, checkpoint, data, dependencies)
        selections = choose_selections(
            attempts, presentation_count=args.presentation_snapshots,
            rest_count=args.snapshots - args.presentation_snapshots,
            window_steps=args.window_steps, stimulus_ticks=MODEL.stimulus_ticks,
            rest_ticks=MODEL.rest_ticks, seed=args.selection_seed,
        )
    validate_build_seed(args.build_path, args.model_seed)
    schedule = {
        "schema": SELECTION_SCHEMA,
        "checkpoint": str(args.checkpoint) if args.checkpoint else None,
        "checkpoint_sha256": sha256_file(args.checkpoint) if args.checkpoint else None,
        "initial_weights": str(args.initial_weights) if args.initial_weights else None,
        "rule": args.rule,
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
        "collection_mode": args.collection_mode,
        "selection_algorithm": "phase-stratified-algorithm-r" if sampling_stats else "offline-uniform",
        "sampling_stats": sampling_stats,
        "training_final_state_sha256": final_digest,
        "attempts": [asdict(item) for item in attempts],
        "selections": [asdict(item) for item in selections],
    }
    if args.collection_mode == "replay":
        schedule["discovery_final_state_sha256"] = final_digest
    write_json(args.output / "selection.json", schedule)
    if args.collection_mode == "replay":
        final_digest = capture_snapshots(
            args, checkpoint, data, attempts, selections, final_digest, dependencies,
        )
    completion = {
        "schema": SNAPSHOT_SCHEMA, "rule": args.rule,
        "collection_mode": args.collection_mode,
        "snapshot_count": len(selections),
        "presentation_snapshot_count": args.presentation_snapshots,
        "rest_snapshot_count": args.snapshots - args.presentation_snapshots,
        "training_final_state_sha256": final_digest,
        "snapshot_sha256": {
            f"snapshot_{s.index:03d}.npz": sha256_file(args.output / f"snapshot_{s.index:03d}.npz")
            for s in selections
        },
    }
    if args.collection_mode == "replay":
        completion["replay_final_state_sha256"] = final_digest
    write_json(
        args.output / "complete.json", completion,
    )
    print(
        f"COMPLETE attempts={len(attempts)} snapshots={len(selections)} "
        f"final_state_sha256={final_digest}",
        flush=True,
    )


if __name__ == "__main__":
    main()
