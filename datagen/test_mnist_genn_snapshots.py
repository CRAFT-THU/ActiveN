from __future__ import annotations

from dataclasses import dataclass
from collections import Counter
from itertools import product
from pathlib import Path
import random
from types import SimpleNamespace
import unittest
from unittest import mock

import numpy as np

import mnist_genn_snapshots as snapshots


class Variable:
    def __init__(self, values):
        self.view = np.asarray(values, dtype=np.float32)
        self.pulls = 0

    def pull_from_device(self):
        self.pulls += 1


class SnapshotExportTest(unittest.TestCase):
    def setUp(self):
        self.model = SimpleNamespace(
            n_input=3, n_exc=2, dt_ms=0.5,
            pre_tau_ms=20.0, post1_tau_ms=20.0, post2_tau_ms=40.0,
        )
        self.x = Variable([2.0, 0.25, 0.0])
        self.y = Variable([1.5, 0.0])
        self.network = SimpleNamespace(
            feedforward=SimpleNamespace(pre_vars={"x": self.x}, post_vars={"y": self.y}),
            model=SimpleNamespace(t=20.5), inputs=object(), exc=object(),
        )

    def test_one_trace_preserves_accumulated_values_and_zeros_unused(self):
        x, y, z = snapshots.pull_traces(self.network, self.model, "one-trace")
        np.testing.assert_array_equal(x, [2.0, 0.25, 0.0])
        np.testing.assert_array_equal(y, [0.0, 0.0])
        np.testing.assert_array_equal(z, [0.0, 0.0])
        self.assertEqual(self.x.pulls, 1)
        self.assertEqual(self.y.pulls, 0)
        self.x.view[0] = 9.0
        self.assertEqual(x[0], 2.0)

    def test_two_trace_exports_both_accumulators(self):
        x, y, z = snapshots.pull_traces(self.network, self.model, "two-trace")
        np.testing.assert_array_equal(x, [2.0, 0.25, 0.0])
        np.testing.assert_array_equal(y, [1.5, 0.0])
        np.testing.assert_array_equal(z, [0.0, 0.0])
        self.assertEqual(self.y.pulls, 1)

    def test_three_trace_retains_previous_tick_boundary(self):
        with mock.patch.object(snapshots, "trace_from_spike_times", return_value=np.zeros(2)) as pull:
            snapshots.pull_traces(self.network, self.model, "three-trace")
        self.assertEqual(pull.call_args_list, [
            mock.call(self.network.inputs, 20.0, 20.0),
            mock.call(self.network.exc, 20.0, 20.0),
            mock.call(self.network.exc, 20.0, 40.0),
        ])

    def test_additive_trace_validation(self):
        for values in ([1.0], [float("nan"), 0.0, 0.0], [-1.0, 0.0, 0.0]):
            with self.subTest(values=values):
                self.x.view = np.asarray(values)
                with self.assertRaises(ValueError):
                    snapshots.pull_traces(self.network, self.model, "one-trace")

    def test_latest_spike_is_not_an_additive_trace(self):
        population = SimpleNamespace(spike_times=Variable([10.0]), name="test")
        nearest = snapshots.trace_from_spike_times(population, 20.0, 20.0)
        accumulated = np.exp(-20.0 / 20.0) + np.exp(-10.0 / 20.0)
        self.assertGreater(accumulated, float(nearest[0]))

    def test_two_trace_variant_retains_power_parameters(self):
        @dataclass
        class Variant:
            name: str = "one-trace-dense"
            learning_rule: str = "one-trace-power"
            potentiation_rate: float = 0.0005
            depression_rate: float = 0.0001

        variant = snapshots.rule_variant("two-trace", lambda name: Variant())
        self.assertEqual(variant.name, "two-trace-dense")
        self.assertEqual(variant.learning_rule, "two-trace-power")
        self.assertEqual(variant.potentiation_rate, 0.0005)
        self.assertEqual(variant.depression_rate, 0.0001)

    def test_wrong_rule_checkpoint_rejected(self):
        checkpoint = SimpleNamespace(manifest={"variant": {"learning_rule": "three-trace"}})
        with self.assertRaisesRegex(ValueError, "learning_rule"):
            snapshots.validate_checkpoint_rule(
                checkpoint, SimpleNamespace(learning_rule="one-trace-power")
            )

    def test_fresh_training_cli(self):
        with mock.patch("sys.argv", ["export", "--rule", "two-trace", "--initial-weights",
                                     "initial.npy", "--output", "out", "--build-path", "build",
                                     "--end-sample", "30000"]):
            args = snapshots.parse_args()
        self.assertIsNone(args.checkpoint)
        self.assertEqual(args.rule, "two-trace")
        self.assertEqual(args.end_sample, 30000)
        self.assertEqual(args.collection_mode, "single-pass")

    def test_selection_is_deterministic_and_within_phases(self):
        args = dict(
            attempts=[snapshots.Attempt(i, i, i, 2.0, 10, False) for i in range(30)],
            presentation_count=70, rest_count=30, window_steps=16,
            stimulus_ticks=700, rest_ticks=300, seed=19260817,
        )
        first = snapshots.choose_selections(**args)
        self.assertEqual(first, snapshots.choose_selections(**args))
        self.assertEqual(sum(s.phase == "presentation" for s in first), 70)
        self.assertEqual(len(set((s.attempt_index, s.phase, s.phase_tick) for s in first)), 100)
        for point in first:
            self.assertLess(point.attempt_index, 30)
            self.assertLessEqual(point.phase_tick + 16, 700 if point.phase == "presentation" else 300)


class PhaseReservoirTest(unittest.TestCase):
    def test_batched_selection_matches_elementwise_algorithm_r(self):
        for capacity in (0, 1, 4, 15):
            rng = random.Random(17)
            reservoir = snapshots.PhaseReservoir(capacity, "rest", 7, random.Random(17), 70)
            expected = []
            saved = {}
            seen = 0
            for attempt in range(10):
                for tick in range(7):
                    seen += 1
                    slot = seen - 1 if seen <= capacity else rng.randrange(seen)
                    if slot < capacity:
                        value = snapshots.Selection(70 + slot, attempt, "rest", tick)
                        if slot == len(expected):
                            expected.append(value)
                        else:
                            expected[slot] = value
                updates = reservoir.plan(attempt)
                self.assertEqual(updates, sorted(updates, key=lambda s: s.phase_tick))
                self.assertEqual(len({s.index for s in updates}), len(updates))
                saved.update((s.index, s) for s in updates)
                self.assertEqual(reservoir.items, expected)
                self.assertEqual(list(sorted(saved.values(), key=lambda s: s.index)), expected)
                self.assertEqual(reservoir.seen, seen)

    def test_small_population_has_exactly_uniform_subsets(self):
        frequencies = Counter()
        for choices in product(range(3), range(4)):
            rng = mock.Mock()
            rng.randrange.side_effect = choices
            reservoir = snapshots.PhaseReservoir(2, "presentation", 2, rng)
            reservoir.plan(0)
            reservoir.plan(1)
            selected = tuple(sorted(s.attempt_index * 2 + s.phase_tick for s in reservoir.items))
            frequencies[selected] += 1
        self.assertEqual(len(frequencies), 6)
        self.assertEqual(set(frequencies.values()), {2})

    def test_zero_capacity_and_invalid_dimensions(self):
        reservoir = snapshots.PhaseReservoir(0, "rest", 4, mock.Mock())
        self.assertEqual(reservoir.plan(0), [])
        self.assertEqual(reservoir.seen, 4)
        reservoir.rng.randrange.assert_not_called()
        for capacity, starts in ((-1, 4), (2, 0), (2, -1)):
            with self.assertRaises(ValueError):
                snapshots.PhaseReservoir(capacity, "rest", starts, random.Random())

    def test_capture_uses_one_network_and_preserves_retry_and_tick_boundaries(self):
        model = SimpleNamespace(n_exc=1, n_input=3, stimulus_ticks=4, rest_ticks=3,
                                initial_intensity=2.0, minimum_exc_spikes=2,
                                intensity_increment=1.0)
        counter = Variable([0])
        network = SimpleNamespace(model=SimpleNamespace(t=0), exc=SimpleNamespace(vars={"spikeCount": counter}),
                                  phase="", phase_tick=0, attempt=0, close=mock.Mock(),
                                  normalize=mock.Mock(), validate_runtime=mock.Mock(),
                                  weights=lambda: np.zeros((3, 1)), theta_mv=lambda: np.zeros(1))

        def set_image(pixels, intensity):
            network.attempt += 1
            network.phase, network.phase_tick = "presentation", 0

        def zero_input():
            network.phase, network.phase_tick = "rest", 0

        def step(count):
            self.assertGreaterEqual(count, 0)
            for _ in range(count):
                if network.phase_tick == 0:
                    if network.phase == "rest":
                        counter.view[0] += 1
                    elif network.attempt > 1:
                        counter.view[0] += 2
                network.phase_tick += 1
                network.model.t += 0.5

        network.set_image, network.set_zero_input, network._step = set_image, zero_input, step
        args = SimpleNamespace(presentation_snapshots=3, snapshots=5, window_steps=1,
                               selection_seed=9, checkpoint=None, initial_weights=None,
                               build_path=Path("build"), reuse_build=None, end_sample=2,
                               output=Path("out"), poisson_seed=123, rule="two-trace",
                               runaway_spikes=5000, max_intensity=20, checkpoint_interval=1,
                               progress_interval=10)
        saved = {}

        def capture(path, **kwargs):
            selection = kwargs["selection"]
            ticks = selection.attempt_index * 7 + selection.phase_tick
            if selection.phase == "rest":
                ticks += 4
            self.assertEqual(network.model.t, ticks * 0.5)
            self.assertEqual(kwargs["collection_mode"], "single-pass")
            saved[selection.index] = selection

        dependencies = (None, model, None, None, None, None)
        data = SimpleNamespace(images=np.zeros((2, 3), dtype=np.uint8), labels=np.zeros(2))
        with mock.patch.object(snapshots, "make_network", return_value=network) as make, \
             mock.patch.object(snapshots, "save_snapshot", side_effect=capture), \
             mock.patch.object(snapshots, "save_training_checkpoint") as save_checkpoint:
            attempts, selections, _, stats = snapshots.train_and_capture(
                args, SimpleNamespace(accepted_samples=0), data, dependencies,
            )
        make.assert_called_once()
        network.close.assert_called_once()
        self.assertEqual(network.model.t, 10.5)
        self.assertEqual([a.retry for a in attempts], [True, False, False])
        self.assertEqual([a.excitatory_spikes for a in attempts], [0, 3, 3])
        self.assertEqual([a.intensity for a in attempts], [2.0, 3.0, 2.0])
        self.assertEqual([c.args[2] for c in save_checkpoint.call_args_list], [1, 2])
        self.assertEqual(selections, sorted(saved.values(), key=lambda s: s.index))
        self.assertEqual(stats["presentation"], 12)
        self.assertEqual(stats["rest"], 9)


if __name__ == "__main__":
    unittest.main()
