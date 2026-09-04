from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import mnist_snapshot_benchmark as benchmark


class SnapshotBenchmarkTest(unittest.TestCase):
    def test_timer_parser_ignores_final_cumulative_stats(self) -> None:
        log = """\
[Soft] Timer interval 1 (cycles 101..200, span=100):
  unicast_mem_requests: 0
  icache_accesses: 1234
  icache_fills:    0 (miss rate 0.00000%)
  icache_stalls:   0 core-cycles (0.00000% of PU cycles)
  mnist_feedforward_messages: 448
  mnist_trace_messages: 800
  mnist_post_fired_trace_messages: 392
  mnist_inhibitory_spikes: 5123
[Periph] Output: 1a2b3c
[System] Result: 0 (0x0)
[System] Cycles: 250
[System] Timer: 100 cycles
[System] Runtime: 1.250s
[Soft] Final stats (over 250 cycles):
  unicast_mem_requests: 999
  icache_accesses: 999
  icache_fills:    999 (miss rate 1.00000%)
  icache_stalls:   999 core-cycles (1.00000% of PU cycles)
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "simulate.log"
            path.write_text(log, encoding="ascii")
            parsed = benchmark.parse_simulator_log(
                path, expected_intervals=1, input_pus=196, neuron_pus=224
            )
        interval = parsed["intervals"][0]
        self.assertEqual(interval["unicast_mem_requests"], 0)
        self.assertEqual(interval["icache_fills"], 0)
        self.assertEqual(interval["input_spikes"], 2)
        self.assertEqual(interval["excitatory_spikes"], 2)
        self.assertEqual(interval["inhibitory_spikes"], 3)
        self.assertTrue(interval["strictly_hot"])
        self.assertEqual(parsed["checksum"], 0x1A2B3C)

    def test_cached_activity_normalization_masks_post_count(self) -> None:
        result = {
            "intervals": [
                {"mnist_inhibitory_spikes": 7 | (11 << 10), "inhibitory_spikes": 11271}
            ]
        }
        self.assertTrue(benchmark.normalize_result_activity(result))
        self.assertEqual(result["intervals"][0]["mnist_inhibitory_spikes"], 7)
        self.assertEqual(result["intervals"][0]["inhibitory_spikes"], 7)
        self.assertFalse(benchmark.normalize_result_activity(result))

    def test_percentile_uses_linear_interpolation(self) -> None:
        self.assertEqual(benchmark.percentile([10.0, 20.0], 0.95), 19.5)
        self.assertIsNone(benchmark.percentile([], 0.5))


if __name__ == "__main__":
    unittest.main()
