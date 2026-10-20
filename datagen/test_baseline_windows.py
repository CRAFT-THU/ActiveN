"""Functional contracts for baseline sampling/import; no simulation timings."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

import numpy as np

from baseline_checkpoints import classification_view, inspect_checkpoint, latest_checkpoint, load_cases, sha256
from baseline_sampling import eligible_intervals, sample_case, spread_starts, validate_sample
from baseline_windows import compare_counts, load_arrays, reference_record, restore_arrays, save_arrays
from baseline_report import summarize_case


@dataclass
class SyntheticCase:
    phases: tuple[int, int] | None = (700, 300)
    region: tuple[int, int] = (1000000, 2000000)
    k: int = 1000
    name: str = "fixture"
    workload: str = "mnist"

    def record(self):
        return dict(case=self.name, workload=self.workload, region=self.region)


class SamplingTests(unittest.TestCase):
    def test_phase_quota_and_actual_101_step_containment(self):
        for phases, quotas in [((700, 300), {"presentation": 700, "rest": 300}),
                               ((200, 300), {"presentation": 400, "rest": 600})]:
            c = SyntheticCase(phases=phases)
            r = sample_case(c, 1234)
            self.assertEqual(r["quotas"], quotas)
            self.assertEqual(len(r["samples"]), 1000)
            observed = {k: 0 for k in quotas}
            points = set()
            for row in r["samples"]:
                u = row["warmup_start_tick"]
                self.assertNotIn(u, points); points.add(u)
                observed[row["phase"]] += 1
                self.assertEqual(row["timed_start_tick"], u + 1)
                self.assertEqual(row["timed_end_tick"], u + 101)
                phase_index = 0 if row["phase"] == "presentation" else 1
                offset = u % sum(phases) - (phases[0] if phase_index else 0)
                self.assertGreaterEqual(offset, 0)
                self.assertLessEqual(offset + 101, phases[phase_index])
                self.assertGreaterEqual(u, c.region[0])
                self.assertLessEqual(u + 101, c.region[1])
            self.assertEqual(observed, quotas)
            self.assertEqual(r, sample_case(c, 1234))
            self.assertNotEqual(r["samples"], sample_case(c, 4321)["samples"])

    def test_partial_edges_and_exact_length_phase(self):
        self.assertEqual(eligible_intervals(0, 101, "continuous", None), [(0, 1)])
        self.assertEqual(eligible_intervals(698, 1101, "rest", (700, 300)), [(700, 900)])
        self.assertEqual(eligible_intervals(0, 404, "presentation", (101, 101)), [(0, 1), (202, 203)])
        with self.assertRaises(ValueError):
            eligible_intervals(0, 1000, "presentation", (100, 300))

    def test_no_phase_aliasing_and_rank_bins(self):
        intervals = [(100, 103), (1000, 1003)]
        self.assertEqual(spread_starts(intervals, 6, 1), [100, 101, 102, 1000, 1001, 1002])
        with self.assertRaises(ValueError):
            spread_starts(intervals, 7, 1)
        recurrent = sample_case(SyntheticCase(phases=None, workload="brunel"), 0)
        self.assertEqual(recurrent["quotas"], {"continuous": 1000})
        self.assertTrue(all(s["phase"] == "continuous" for s in recurrent["samples"]))

    def test_external_schedule_cannot_escape_region_or_phase(self):
        c = SyntheticCase()
        sample = sample_case(c, 1)["samples"][0]
        validate_sample(c, sample)
        for change in (dict(timed_start_tick=sample["timed_start_tick"] + 1),
                       dict(warmup_start_tick=c.region[1], timed_start_tick=c.region[1] + 1,
                            timed_end_tick=c.region[1] + 101),
                       dict(phase="continuous"), dict(phase_offset=699)):
            with self.assertRaises(ValueError):
                validate_sample(c, dict(sample, **change))


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def native(self):
        # A present zero-weight edge must survive; padded garbage must not
        # become an edge. This fixture also preserves a pending input and RNG.
        arrays = {"feedforward__rowLength": np.array([1, 2], dtype="<u4"),
                  "feedforward__ind": np.array([1, 999, 0, 1], dtype="<u4"),
                  "feedforward__vars__g": np.array([0, 999, .5, .25], dtype="<f4"),
                  "feedforward__out_post": np.array([[.125, .25]], dtype="<f4"),
                  "inputs__rng": np.arange(12, dtype="<u4").reshape(2, 6)}
        mask = np.array([[0, 1], [1, 1]], dtype=np.uint8)
        m = dict(schema="mnist-genn-native-v1", timestep=123, boundary="after-neurons-before-synapses",
                 constants=dict(n_input=2, n_exc=2), structural_mask_sha256=hashlib.sha256(mask.tobytes()).hexdigest(),
                 arrays={k:dict(dtype=v.dtype.str, shape=list(v.shape), sha256=hashlib.sha256(v.tobytes()).hexdigest()) for k,v in arrays.items()})
        np.savez_compressed(self.root / "state.npz", **arrays)
        m["archive_sha256"] = sha256(self.root / "state.npz")
        (self.root / "manifest.json").write_text(json.dumps(m))
        return arrays

    def test_structural_zeros_padding_and_native_state(self):
        native = self.native()
        cp = inspect_checkpoint(self.root, verify=True)
        result = classification_view(cp)
        np.testing.assert_array_equal(result["structural_mask"], [[0, 1], [1, 1]])
        np.testing.assert_array_equal(result["weights"], [[0, 0], [.5, .25]])
        for key in native:
            np.testing.assert_array_equal(result["arrays"][key], native[key])
        self.assertEqual(latest_checkpoint([cp], 124), cp)
        with self.assertRaises(ValueError):
            latest_checkpoint([cp], 122)

    def test_corrupt_payload_and_endpoint_rejected(self):
        self.native()
        with (self.root / "state.npz").open("ab") as stream:
            stream.write(b"corrupt")
        with self.assertRaisesRegex(ValueError, "checksum"):
            inspect_checkpoint(self.root, verify=True)
        (self.root / "manifest.json").write_text(json.dumps(dict(schema="stdp-benchmark-endpoint-v1")))
        with self.assertRaisesRegex(ValueError, "restart"):
            inspect_checkpoint(self.root)

    def test_bundle_layout_and_integrity(self):
        class HostTransfer:
            def sync(self): pass
            def copy(self, device, host, *, to_device=False):
                np.copyto(device if to_device else host, host if to_device else device)
        device = np.array([3, 7, 11], dtype="<u4")
        registry = {"pending": (device, device.dtype, device.shape)}
        directory = self.root / "state"
        records = save_arrays(directory, registry, HostTransfer())
        device[:] = 0
        restore_arrays(directory, records, registry, HostTransfer())
        np.testing.assert_array_equal(device, [3, 7, 11])
        records["pending"]["file"] = "../escape.npy"
        np.save(self.root / "escape.npy", device)
        with self.assertRaisesRegex(ValueError, "escapes"):
            load_arrays(directory, records)

    def test_foreign_window_rejected_before_device_access(self):
        from types import SimpleNamespace
        from baseline_genn import Replay
        replay = object.__new__(Replay)
        replay.case = SimpleNamespace(workload="mnist", name="fixture", boundary="after-neurons-before-synapses")
        replay.parent = SimpleNamespace(identity="a" * 64)
        with self.assertRaisesRegex(ValueError, "different case"):
            replay.restore_window(self.root, dict(workload="mnist", case="other", boundary=replay.case.boundary,
                                                  checkpoint_sha256="a" * 64))


class CountTests(unittest.TestCase):
    def record(self):
        window = dict(workload="mnist", case="fixture", index=0, boundary="after-neurons-before-synapses",
                      warmup_start_tick=100, timed_start_tick=101, timed_end_tick=201, phase="rest",
                      rng=dict(mode="native-genn"), checkpoint_sha256="a" * 64, window_manifest_sha256="b" * 64)
        counts = np.zeros((101, 3), dtype=np.uint32)
        counts[0] = [999, 888, 777]
        counts[1:] = [0, 1, 2]
        return reference_record(window, counts, ["input", "exc", "inh"])

    def test_warmup_exclusion_and_failure_withholds_cycles(self):
        r = self.record()
        self.assertEqual(r["totals"], [0, 100, 200])
        observed = dict(r, timer_cycles=10000)
        self.assertEqual(compare_counts(r, observed)["cycles_per_step"], 100)
        observed["totals"] = [0, 99, 201]
        result = compare_counts(r, observed)
        self.assertFalse(result["passed"])
        self.assertIsNone(result["accepted_timer_cycles"])
        self.assertEqual(result["observed_timer_cycles"], 10000)

    def test_interval_mismatch_and_unapproved_tolerance(self):
        r = self.record()
        with self.assertRaisesRegex(ValueError, "interval"):
            compare_counts(r, dict(r, timed_start_tick=100, timer_cycles=1))
        with self.assertRaisesRegex(ValueError, "identified policy"):
            compare_counts(r, dict(r, timer_cycles=1), tolerance={})
        policy = dict(approval="explicit unit-test fixture only", populations={p:dict(absolute=1, relative=0.) for p in r["populations"]})
        result = compare_counts(r, dict(r, totals=[0, 99, 201], timer_cycles=200), tolerance=policy)
        self.assertTrue(result["passed"])

    def test_negative_counts_and_missing_identity_rejected(self):
        r = self.record()
        missing = dict(r)
        del missing["window_manifest_sha256"]
        with self.assertRaisesRegex(ValueError, "identity"):
            compare_counts(missing, dict(missing, timer_cycles=1))
        counts = np.zeros((101, 3), dtype=np.int64)
        counts[0, 0] = -1
        with self.assertRaisesRegex(ValueError, "integer"):
            reference_record({}, counts, ["input", "exc", "inh"])
        for change in (dict(timed_end_tick=202), dict(warmup_start_tick=99), dict(warmup_steps=0)):
            malformed = dict(r, **change)
            with self.assertRaisesRegex(ValueError, "Incomplete"):
                compare_counts(malformed, dict(malformed, timer_cycles=1))
        empty = dict(r, populations=[], totals=[])
        with self.assertRaisesRegex(ValueError, "populations"):
            compare_counts(empty, dict(empty, timer_cycles=1))

    def test_complete_case_denominator_and_failure_retention(self):
        first = self.record()
        second = dict(first, sample=1, warmup_start_tick=1100, timed_start_tick=1101, timed_end_tick=1201,
                      window_manifest_sha256="c" * 64)
        spec = dict(workload="mnist", case="fixture", windows=2, scope="functional fixture",
                    samples=[dict(index=r["sample"], phase=r["phase"], timed_start_tick=r["timed_start_tick"],
                                  timed_end_tick=r["timed_end_tick"]) for r in (first, second)])
        observed = [dict(first, timer_cycles=10000), dict(second, timer_cycles=12000)]
        result = summarize_case(spec, [first, second], observed)
        self.assertEqual(result["cycles_per_timestep"], 110)
        self.assertEqual(result["measured_timesteps"], 200)
        observed[1]["totals"] = [0, 99, 200]
        result = summarize_case(spec, [first, second], observed)
        self.assertFalse(result["passed"])
        self.assertEqual(len(result["samples"]), 2)
        self.assertIsNone(result["accepted_timer_cycles"])
        with self.assertRaisesRegex(ValueError, "every declared"):
            summarize_case(spec, [first, second], observed[:1])


if __name__ == "__main__":
    unittest.main()
