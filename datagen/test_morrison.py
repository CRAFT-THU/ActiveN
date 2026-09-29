"""Functional image/checkpoint contract tests; no performance measurements."""
import json
import math
import os
from pathlib import Path
import unittest
import numpy as np

from morrison import Images, plastic_row, write_images
from morrison_graph import fresh_graph, checkpoint_graph
from morrison_model import Spec, MC_BYTES, F, constants, poisson_thresholds
from morrison_validate import check_images, WeightMoments

ROOT = Path(__file__).resolve().parents[1]


class MorrisonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = Path(os.environ["MR_TEST_DIR"])
        cls.output.mkdir(parents=True, exist_ok=False)
        cls.spec = Spec(ne=256, ni=64, ce=129, ci=8, pu=64, mc=2)
        cls.graph = fresh_graph(cls.output / "graph", cls.spec, 713)

    def test_compact_chunks_and_physical_bound(self):
        directory = self.output / "chunks"
        directory.mkdir()
        images = Images(directory, 2)
        try:
            for count in (0, 1, 7, 8, 9, 127, 128, 129, 255, 256, 257):
                weights = np.arange(count, dtype=F)
                target = np.arange(count, dtype=np.uint32) + 0x12340001
                start = images.files[0].tell()
                base, addresses = plastic_row(images, 0, weights, target)
                self.assertEqual(base, start)
                size = (count//128)*1024 + ((count%128+7)//8)*64
                self.assertEqual(images.files[0].tell()-base, size)
                images.files[0].flush()
                data = np.fromfile(images.paths[0], dtype="<u4")
                for i, address in enumerate(addresses):
                    half = 512 if i < count//128*128 else ((count%128+7)//8)*32
                    self.assertEqual(data[address//4], weights[i].view(np.uint32))
                    self.assertEqual(data[(address+half)//4], target[i])
            images.files[1].seek(MC_BYTES-32)
            with self.assertRaisesRegex(ValueError, "physical 1 GiB"):
                images.place(1, bytes(64), "intentional-overflow")
        finally:
            images.close()

    def test_full_sampler_tail(self):
        table = poisson_thresholds()
        self.assertTrue(np.all(table[1:] >= table[:-1]))
        self.assertEqual(int(table[-1]), 2**32-1)
        probability = np.diff(np.r_[0., table.astype(np.float64)])/(2**32-1)
        self.assertAlmostEqual(float(np.dot(np.arange(len(table)), probability)), 2.088, delta=1e-8)
        exact_tail = 1-sum(math.exp(-2.088)*2.088**k/math.factorial(k) for k in range(10))
        self.assertAlmostEqual(float(probability[10:].sum()), exact_tail, delta=1/(2**32-1))
        self.assertGreater(probability[10:].sum(), 6.6e-5)
        self.assertTrue(np.all(poisson_thresholds(0.) == 2**32-1))

    def test_weight_moments_resolve_small_variance(self):
        base = F(constants()["epsc"] * F(45.61))
        neighbor = np.nextafter(base, F(np.inf))
        moments = WeightMoments()
        moments.add(np.empty(0, F))
        for count in (137, 864):
            moments.add(np.full(count, base, F))
        self.assertEqual(moments.count, 1001)
        self.assertEqual(moments.mean, float(base))
        self.assertEqual(moments.std, 0.)
        moments.add(np.array([neighbor], F))
        # Exact two-point population: 1001 copies of a and one of a+delta.
        # E[w*w]-E[w]**2 loses this variance at the stored weight's magnitude.
        delta = float(neighbor) - float(base)
        self.assertEqual(moments.count, 1002)
        self.assertAlmostEqual(moments.mean, float(base)+delta/1002, delta=1e-12)
        self.assertAlmostEqual(moments.std, delta*math.sqrt(1001)/1002, delta=1e-18)

    def test_graph_identity_g1_g4(self):
        results = {}
        for group in (1, 4):
            directory = self.output / f"image-g{group}"
            directory.mkdir()
            (directory / "graph").symlink_to(self.graph.directory.resolve(), target_is_directory=True)
            manifest = write_images(self.graph, directory, ROOT / f"sim/payloads/sys/morrison_g{group}.bin",
                                    group=group, functional=True, warmup=0, timed=2)
            result = check_images(directory)
            self.assertEqual(result["plastic"], 33024)
            self.assertEqual(result["reverse"], 33024)
            self.assertEqual(result["static"], 10816)
            self.assertTrue(all(n < MC_BYTES for n in result["image_bytes"]))
            self.assertFalse(manifest["benchmark_ready"])
            self.assertEqual(result["weight_std"], 0.)
            results[str(group)] = result
        (self.output / "image-results.json").write_text(json.dumps(results, indent=2)+"\n")

    def test_checkpoint_requires_model_and_uses_20ms_post_trace(self):
        s, graph = self.spec, self.graph
        data = {}
        for name, count in (("exc", s.ne), ("inh", s.ni)):
            for key, value in (("V", 3.), ("Iex", 4.), ("Iin", -2.), ("dIex", 5.), ("dIin", -3.)):
                data[f"{name}_{key}"] = np.full(count, F(value))
            data[f"{name}_refrac"] = np.zeros(count, np.uint32)
            data[f"{name}_tick"] = np.full(count, 40, np.uint32)
        for name, (pre, post) in graph.arrays.items():
            data[f"{name}_pre"], data[f"{name}_post"] = pre, post
            data[f"{name}_input"] = np.zeros(s.ne if name[1] == "e" else s.ni, F)
        edges = s.ne*s.ce
        data["ee_g"] = np.linspace(1., 90., edges, dtype=F)
        data["ee_lastTraceTick"] = np.full(edges, 20, np.uint32)
        # At tick 40, both lazy traces must decay by exp(-2 ms/20 ms).
        data["ee_preTrace"] = np.full(edges, F(2.))
        data["ee_postTrace"] = np.full(edges, F(3.))
        metadata = dict(format="brunel-genn-phase-checkpoint-v1", rule="morrison", dt_ms=.1,
                        boundary="after-synapses-before-neurons", precision="float32",
                        delay_steps=0, timestep=40, model=s.model())
        data["metadata"] = np.array(json.dumps(metadata))
        checkpoint = self.output / "complete-checkpoint.npz"
        np.savez(checkpoint, **data)
        imported = checkpoint_graph(checkpoint, self.output / "imported", s)
        expected_post = F(F(3.*math.exp(-.1))*constants()["dep"])
        self.assertTrue(np.all(imported.state[:s.ne, 4].view(F) == expected_post))
        self.assertTrue(np.all(imported.state[:, 1].view(F) == F(2.)))
        self.assertTrue(np.all(imported.state[:, 2].view(F) == F(2.)))
        self.assertTrue(np.array_equal(imported.weights, data["ee_g"]))
        directory = self.output / "checkpoint-image"
        directory.mkdir()
        (directory / "graph").symlink_to(imported.directory.resolve(), target_is_directory=True)
        write_images(imported, directory, ROOT / "sim/payloads/sys/morrison_g4.bin",
                     group=4, functional=True, warmup=0, timed=2)
        check_images(directory)
        data["ee_lastTraceTick"] = data["ee_lastTraceTick"].astype(np.int64)
        bad_timestamp = self.output / "wrong-timestamp-type.npz"
        np.savez(bad_timestamp, **data)
        with self.assertRaisesRegex(ValueError, "timestamp type"):
            checkpoint_graph(bad_timestamp, self.output / "rejected-timestamp", s)
        data["ee_lastTraceTick"] = data["ee_lastTraceTick"].astype(np.uint32)
        # A Python integer may be rounded to FP32 by NumPy's comparison.
        # Both populations must reject clocks from that different boundary.
        metadata["timestep"] = 2**24+1
        for pop, n in (("exc", s.ne), ("inh", s.ni)):
            data[f"{pop}_tick"] = np.full(n, metadata["timestep"], np.uint32)
        for pop in ("exc", "inh"):
            key = f"{pop}_tick"
            exact = data[key]
            data[key] = exact.astype(F)
            data["metadata"] = np.array(json.dumps(metadata))
            bad_clock = self.output / f"inexact-{pop}-clock.npz"
            np.savez(bad_clock, **data)
            with self.assertRaisesRegex(ValueError, f"{pop}_tick shape/type"):
                checkpoint_graph(bad_clock, self.output / f"rejected-{pop}-clock", s)
            data[key] = exact
        metadata["timestep"] = 40
        data["exc_tick"] = np.full(s.ne, 40, np.uint32)
        data["inh_tick"] = np.full(s.ni, 40, np.uint32)
        del metadata["model"]
        data["metadata"] = np.array(json.dumps(metadata))
        incomplete = self.output / "unbound-checkpoint.npz"
        np.savez(incomplete, **data)
        with self.assertRaisesRegex(ValueError, "model.family"):
            checkpoint_graph(incomplete, self.output / "rejected", s)


if __name__ == "__main__":
    unittest.main(verbosity=2)
