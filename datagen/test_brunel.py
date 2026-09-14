"""Small numerical checks; full cache/transport tests live in sim/tests."""

import ctypes
import unittest
import subprocess
import tempfile
from pathlib import Path
import numpy as np
from brunel import constants, poisson
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
            '#include "brunel_numeric.h"\nF polynomial(F w){return pow04(w); }\n'
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

    def test_polynomial(self):
        path = self.library
        lib = ctypes.CDLL(str(path))
        fn = lib.polynomial
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
        self.assertLess(np.max(np.abs(y / x.astype(float) ** 0.4 - 1)), 2e-6)

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
