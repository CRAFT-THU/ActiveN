"""Small numerical checks; full cache/transport tests live in sim/tests."""

import ctypes
import unittest
import subprocess
import tempfile
from pathlib import Path
import numpy as np
from brunel import F, constants, poisson, poisson_thresholds
from brunel_validate import pending_power
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
