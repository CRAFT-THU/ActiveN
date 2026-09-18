"""Small numerical checks; full cache/transport tests live in sim/tests."""

import ctypes
import unittest
import subprocess
import tempfile
from pathlib import Path
import numpy as np
from brunel import F, constants, poisson, poisson_thresholds
from brunel_validate import pending_power, compare_combined_derivative
from brunel_reuse import working_sets


class Numerics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        (root / "copilot/tmp").mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(
            prefix="brunel-numeric-", dir=root / "copilot/tmp"
        )
        directory = Path(cls.temp.name)
        source = directory / "numeric.c"
        source.write_text(
            '#include "brunel_numeric.h"\nF root_power(F w){return pow04(w); }\n'
        )
        cls.library = directory / "numeric.so"
        subprocess.run(
            [
                "cc",
                "-shared",
                "-fPIC",
                "-O2",
                "-ffp-contract=off",
                "-I",
                str(root / "sim/payloads/sys"),
                str(source),
                "-o",
                str(cls.library),
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_iterative_power(self):
        path = self.library
        lib = ctypes.CDLL(str(path))
        fn = lib.root_power
        fn.argtypes = [ctypes.c_uint32]
        fn.restype = ctypes.c_uint32
        x = np.geomspace(1e-44, 3e38, 100001).astype(np.float32)
        y = np.array([fn(int(i)) for i in x.view(np.uint32)], np.uint32).view(
            np.float32
        )
        self.assertEqual(fn(0), 0)
        self.assertEqual(pending_power(np.float32(0)), 0)
        reconstructed = np.array([pending_power(v) for v in x], np.float32)
        np.testing.assert_array_equal(reconstructed.view(np.uint32), y.view(np.uint32))
        reference = F(np.power(x.astype(np.float64), 2 / 5))
        ulp = np.abs(y.view(np.uint32).astype(np.int64) - reference.view(np.uint32))
        self.assertLessEqual(int(ulp.max()), 4)

    def test_poisson(self):
        for rule in ("additive", "morrison"):
            c = constants(rule)
            r = np.arange(1, 100001, dtype=np.uint32) * 1234567
            draws = []
            for _ in range(10):
                draws.append(poisson(r, c["external_limit"]))
            a = np.concatenate(draws)
            self.assertLess(abs(a.mean() - c["external_mean"]), 0.015)
            self.assertLess(abs(a.var() - c["external_mean"]), 0.03)

    def test_single_draw_poisson_distribution(self):
        for rule in ("additive", "morrison"):
            mean = float(constants(rule)["external_mean"])
            cdf = poisson_thresholds(mean).astype(float) / 2**32
            probability = np.diff(np.r_[0, cdf, 1])
            values = np.arange(10)
            self.assertTrue(np.all(probability >= 0))
            self.assertLess(abs(probability @ values - mean), 2e-6)
            self.assertLess(abs(probability @ (values - mean)**2 - mean), 3e-5)

    def test_scaled_learning_equivalence(self):
        for rule in ("additive", "morrison"):
            c = constants(rule)
            scale = float(c["epsc"]) * float(c["delivery"])
            w = np.linspace(0, 2 * 45.609600316540956, 1001)
            for trace in (0.0, 0.1, 1.0, 3.0):
                if rule == "additive":
                    expected = np.clip(w - 0.01 * 1.05 * 2 * 45.609600316540956 * trace, 0, 2 * 45.609600316540956) * scale
                    actual = np.clip(w * scale - float(c["dep"]) * trace, 0, c["wmax"])
                else:
                    expected = (w * (1 - 0.01026 * trace) + 0.1 * trace * w**0.4) * scale
                    actual = w * scale * (1 - float(c["dep"]) * trace) + float(c["pot"]) * trace * (w * scale)**0.4
                np.testing.assert_allclose(actual, expected, rtol=2e-6, atol=2e-4)

    def test_combined_alpha_filter(self):
        """Bound accumulated rounding against two independent FP64 channels.

        Exercise sustained excitation, inhibition, cancellation and quiet input
        for 500 steps without resynchronizing the combined FP32 recurrence.
        """
        c = constants("morrison")
        rng = np.random.default_rng(928)
        split = rng.uniform(0, 2000, (4, 2048)).astype(np.float64)
        split[2:] *= -1
        current = F(split[0] + split[2])
        derivative = F(split[1] + split[3])
        voltage = F(rng.uniform(-10, 20, 2048))
        reference_v = voltage.astype(np.float64)
        maximum = np.zeros(3)
        for _ in range(500):
            e = F((rng.random(2048) < 0.08) * rng.uniform(0, 2000, 2048))
            i = F(-((rng.random(2048) < 0.08) * rng.uniform(0, 8000, 2048)))
            ext = F(rng.poisson(float(c["external_mean"]), 2048) * F(c["epsc"] * c["external_weight"]))
            reference_v = (float(c["p33"]) * reference_v
                           + float(c["p31"]) * (split[1] + split[3])
                           + float(c["p32"]) * (split[0] + split[2]))
            split[[0, 2]] = float(c["p11"]) * split[[0, 2]] + float(c["p21"]) * split[[1, 3]]
            split[1] = float(c["p11"]) * split[1] + e.astype(float) + ext.astype(float)
            split[3] = float(c["p11"]) * split[3] + i.astype(float)
            voltage = F(F(c["p33"] * voltage) + F(F(c["p31"] * derivative) + F(c["p32"] * current)))
            current = F(F(c["p11"] * current) + F(c["p21"] * derivative))
            derivative = F(F(F(c["p11"] * derivative) + F(e + i)) + ext)
            maximum = np.maximum(maximum, [
                np.max(np.abs(voltage.astype(float) - reference_v)),
                np.max(np.abs(current.astype(float) - split[0] - split[2])),
                np.max(np.abs(derivative.astype(float) - split[1] - split[3])),
            ])
        self.assertLess(maximum[0], 0.002)
        self.assertLess(maximum[1], 0.002)
        self.assertLess(maximum[2], 0.02)

    def test_learning_scaled_trace_recurrence(self):
        """Bound accumulated error against canonical FP32 and FP64 traces.

        Include the maximum refractory-limited rate, bursts, sparse firing,
        and silent neurons. Neither reference is resynchronized to the scaled
        recurrence, so a changed decay or firing increment cannot be hidden.
        """
        for rule in ("additive", "morrison"):
            c = constants(rule)
            rng = np.random.default_rng(94721)
            decay = np.array([c["decay_pre"], c["decay_post"]], F)[:, None]
            rate = np.array([c["pot"], c["dep"]], F)[:, None]
            raw = F(rng.random((2, 256)) / (1 - decay.astype(float)**5))
            scaled = F(raw * rate)
            precise = raw.astype(float)
            phase = rng.integers(5, size=(2, 256))
            maximum = np.zeros(2)
            for step in range(5000):
                fired = (step % 5 == phase)
                fired[:, 64:128] &= step % 500 < 75
                fired[:, 128:192] &= rng.random((2, 64)) < 0.004
                fired[:, 192:] = False
                raw = F(F(raw * decay) + fired.astype(F))
                scaled = F(F(scaled * decay) + F(fired.astype(F) * rate))
                precise = precise * decay.astype(float) + fired
                for i, expected in enumerate((F(raw * rate), precise * rate)):
                    maximum[i] = max(maximum[i], np.max(
                        np.abs(scaled.astype(float) - expected)
                        / np.maximum(1, np.abs(expected))))
            self.assertLess(maximum[0], 2e-5)
            self.assertLess(maximum[1], 2e-5)

    def test_combined_derivative_cancellation_bound(self):
        c = constants("morrison")
        n = np.zeros((2, 20), np.uint32)
        f = n.view(F)
        f[0, 2] = F(6424.647026342573 / float(c["p11"]))
        f[0, 17] = F(6806.7275390625)
        f[0, 18] = F(-13613.455078125)
        noise = np.array([1, 0])
        observed = np.array([-1.572906494140625, 0], F)
        result = compare_combined_derivative(n, observed, noise, c)
        self.assertEqual(result["violations"], 0)
        # A real missing input must not be accepted by the cancellation bound.
        corrupted = observed.copy()
        corrupted[0] += F(1)
        self.assertEqual(compare_combined_derivative(n, corrupted, noise, c)["violations"], 1)


class WorkingSets(unittest.TestCase):
    def test_distinct_sets_and_reuse_bounds(self):
        # Two synapses recur after one intervening step. Only PU0 touches a
        # different synapse in between; duplicate edges remain separate IDs.
        steps = [np.array(x, dtype=np.int64) for x in ([0, 1], [2], [0, 1])]
        steps += [np.empty(0, dtype=np.int64) for _ in range(27)]
        result = working_sets(steps, np.array([0, 1, 0]), 3, 2)
        self.assertEqual([w["prefix_distinct"] for w in result["windows"]], [3] * 3)
        self.assertEqual(result["repeat_gap_histogram"], {"2": 2})
        self.assertEqual(result["mean_repeat_gap_timesteps"], 2)
        self.assertEqual(
            result["mean_distinct_other_synapses_between_reuses_lower"], 0.5
        )
        self.assertEqual(
            result["mean_distinct_other_synapses_between_reuses_upper"], 0.5
        )
        self.assertAlmostEqual(result["repeat_fraction_of_step_touches"], 2 / 5)

    def test_adjacent_steps_have_unknown_within_step_order(self):
        steps = [np.array(x, dtype=np.int64) for x in ([0, 1], [0, 2])]
        result = working_sets(steps, np.zeros(3, dtype=np.int32), 3, 1)
        self.assertEqual(result["mean_repeat_gap_timesteps"], 1)
        self.assertEqual(result["mean_distinct_other_synapses_between_reuses_lower"], 0)
        self.assertEqual(result["mean_distinct_other_synapses_between_reuses_upper"], 2)


if __name__ == "__main__":
    unittest.main()
