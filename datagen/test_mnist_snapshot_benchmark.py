from __future__ import annotations

import tempfile
import unittest
import json
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import mnist_snapshot_benchmark as benchmark


class SnapshotBenchmarkTest(unittest.TestCase):
    @staticmethod
    def case_args(directory: Path) -> dict:
        return dict(
            repository=directory, generator=directory / "generator.py",
            output=directory, payload=directory / "payload.bin",
            simulator=directory / "simulator", warmup_steps=4, timed_steps=12,
            max_cycles=3000000, soft_threads=8, input_pus=288, neuron_pus=224,
            horizontal_partition="full-8", horizontal_replica_stride=3,
            feedforward_destination_stride=167, trace_destination_stride=83,
            labels=[0],
        )

    def test_generator_receives_selected_rule(self) -> None:
        for rule in ("one-trace", "two-trace", "three-trace"):
            with self.subTest(rule=rule), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                with mock.patch.object(benchmark, "run_logged", return_value=(1, 0)) as run:
                    with self.assertRaisesRegex(RuntimeError, "image generation failed"):
                        benchmark.benchmark_one(
                            root / "snapshot_000.npz", rule=rule,
                            **self.case_args(root),
                        )
                command = run.call_args.args[0]
                self.assertEqual(command[command.index("--rule") + 1], rule)

    def test_cached_result_rejects_different_rule(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = root / "snapshot_000"
            case.mkdir()
            (case / "result.json").write_text(json.dumps({"rule": "two-trace"}))
            with self.assertRaisesRegex(ValueError, "wrong STDP rule"):
                benchmark.benchmark_one(
                    root / "snapshot_000.npz", **self.case_args(root)
                )

    def test_cached_legacy_result_is_three_trace_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = root / "snapshot_000"
            case.mkdir()
            (case / "result.json").write_text(json.dumps({"intervals": []}))
            self.assertEqual(
                benchmark.benchmark_one(
                    root / "snapshot_000.npz", **self.case_args(root)
                )["intervals"], [],
            )
            with self.assertRaisesRegex(ValueError, "wrong STDP rule"):
                benchmark.benchmark_one(
                    root / "snapshot_000.npz", rule="one-trace",
                    **self.case_args(root),
                )

    def test_cached_image_rejects_different_rule(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "snapshot_000" / "image"
            image.mkdir(parents=True)
            (image / "manifest.json").write_text(json.dumps({"rule": "three-trace"}))
            for mc in range(16):
                (image / f"dram.{mc}").touch()
            with self.assertRaisesRegex(ValueError, "wrong STDP rule"):
                benchmark.benchmark_one(
                    root / "snapshot_000.npz", rule="one-trace",
                    **self.case_args(root),
                )

    def test_summary_rejects_mixed_rules(self) -> None:
        with self.assertRaisesRegex(ValueError, "different STDP rules"):
            benchmark.make_summary(
                [{"rule": "two-trace"}], SimpleNamespace(rule="one-trace")
            )

    def test_rule_cli(self) -> None:
        for rule in ("one-trace", "two-trace", "three-trace"):
            with self.subTest(rule=rule), mock.patch(
                "sys.argv", ["benchmark", "--snapshots", ".", "--output", ".",
                             "--rule", rule],
            ):
                self.assertEqual(benchmark.parse_args().rule, rule)

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
