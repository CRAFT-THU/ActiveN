from types import SimpleNamespace
import unittest
import sys
from unittest import mock

import numpy as np

from mnist_weight_caps import fan_in_caps
import mnist_genn_snapshots as snapshots
from mnist_genn_snapshots import configured_caps


class FanInCapsTest(unittest.TestCase):
    def test_dense_and_unequal_fan_in(self):
        degrees, caps = fan_in_caps([1, 1, 1, 0, 1, 0, 1, 0], 4, 2)
        self.assertEqual(degrees, [4, 1])
        self.assertEqual(caps, [1.0, 4.0])
        self.assertEqual(fan_in_caps([1] * (784 * 2), 784, 2)[1], [1.0, 1.0])

    def test_empty_column_has_finite_sentinel_but_cannot_normalize(self):
        mask = np.array([[1, 0], [1, 0]], dtype=bool)
        self.assertEqual(fan_in_caps(mask.ravel(), 2, 2), ([2, 0], [1.0, 0.0]))
        with self.assertRaisesRegex(ValueError, "at least one input"):
            configured_caps(SimpleNamespace(weight_max_policy="fan-in"), None, mask)

    def test_cap_tracks_structural_count_not_nominal_rate_or_weight(self):
        mask = np.ones((784, 3), dtype=bool)
        mask[98:, 0] = False
        mask[74:, 1] = False
        mask[123:, 2] = False
        caps = configured_caps(SimpleNamespace(weight_max_policy="fan-in"), None, mask)
        np.testing.assert_array_equal(caps, np.array([8, 784 / 74, 784 / 123], dtype=np.float32))

    def test_old_or_different_fan_in_checkpoint_cannot_resume(self):
        args = SimpleNamespace(weight_max_policy="fan-in", checkpoint="checkpoint.npz")
        variant = SimpleNamespace()
        checkpoint = SimpleNamespace(manifest={})
        deps = (None, None, lambda path: checkpoint, None,
                lambda variant: np.ones((4, 2), dtype=bool), None)
        with mock.patch.object(snapshots, "selected_variant", return_value=variant):
            for metadata in ({}, {"weight_max_policy": "fan-in", "weight_max_sha256": "wrong"}):
                checkpoint.manifest = metadata
                with self.assertRaisesRegex(ValueError, "cap policy or fan-in"):
                    snapshots.initial_state(args, deps)

    def test_cli_defaults_preserve_dense_and_select_sparse_fan_in(self):
        for rate, policy in (("1", "connection-rate"), ("0.125", "fan-in")):
            with mock.patch.object(sys, "argv", ["export", "--initial-weights", "weights.npy",
                                   "--output", "out", "--build-path", "build", "--connection-rate", rate]):
                self.assertEqual(snapshots.parse_args().weight_max_policy, policy)


if __name__ == "__main__":
    unittest.main()
