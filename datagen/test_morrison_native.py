"""Native checkpoint conversion tests and a nonzero-state runtime fixture."""
import json
import os
from pathlib import Path
import shutil
import unittest
import numpy as np

from morrison import write_images
from morrison_graph import checkpoint_graph, fresh_graph, sha256
from morrison_model import F, Spec, constants
from morrison_validate import check_images

ROOT = Path(__file__).resolve().parents[1]


def write_array(directory, metadata, name, value):
    path = directory / f"{name}.npy"
    np.save(path, value)
    metadata["arrays"][name] = dict(dtype=value.dtype.str, shape=list(value.shape),
                                   bytes=path.stat().st_size, sha256=sha256(path))


def fixture(directory, graph):
    """Construct the producer's physical layout without the importer helpers."""
    s = graph.spec
    directory.mkdir()
    native, connections = directory / "settled", directory / "graph"
    native.mkdir()
    connections.mkdir()
    tick = 2**24 + 101
    meta = dict(format="genn-native-phase-checkpoint-v1", kind="state", arrays={},
                boundary="after-synapses-before-neurons", timestep=tick, dt_ms=.1,
                graph="../graph", model=s.model(),
                compatibility=dict(scalar_dtype="<f4", dt_ms=.1, post_delay_steps=0,
                    axonal_delay_steps=dict.fromkeys(s.projections(), 0),
                    stdp_timing="arrival", stdp_tie_order="nest_causal_boundary"))
    graph_meta = dict(format=meta["format"], kind="graph", arrays={})
    portable, saved = {}, {}
    for pop, n in (("exc", s.ne), ("inh", s.ni)):
        ids = np.arange(n)
        values = dict(V=np.where(ids % 2 == 0, 21., 0.).astype(F),
                      Iex=(4.+ids/16.).astype(F), Iin=(-2.-ids/32.).astype(F),
                      dIex=(5.+ids/8.).astype(F), dIin=(-3.-ids/16.).astype(F),
                      refrac=np.where(ids % 7 == 3, 3, 0).astype("<u4"),
                      tick=np.full(n, tick, "<u4"),
                      lastSpikeTick=np.full(n, tick-5, "<u4"),
                      spikeCount=ids.astype("<u4"), rng=np.ones((n, 6), "<u4"))
        for field, value in values.items():
            key = f"{pop}_{field}"
            write_array(native, meta, key, value)
            portable[key] = value
    pre_trace = .25 + np.arange(s.ne, dtype=np.float64)/1024.
    post_trace = .5 + np.arange(s.ne, dtype=np.float64)/512.
    for name, (pre, post) in graph.arrays.items():
        npre, npost, _, _, _ = s.projections()[name]
        lengths = np.bincount(pre, minlength=npre).astype("<u4")
        stride = int(lengths.max()) + 7
        indices = np.full((npre, stride), 0xffffffff, "<u4")
        fields = {}
        if name == "ee":
            ages = np.arange(len(pre)) % 97
            edge_values = dict(g=np.linspace(2., 85., len(pre), dtype=F),
                preTrace=(pre_trace[pre] * np.exp(ages*.005)).astype(F),
                postTrace=(post_trace[post] * np.exp(ages*.005)).astype(F),
                lastTraceTick=(tick-ages).astype("<u4"),
                lastPreUpdateTick=np.full(len(pre), tick-1, "<u4"),
                lastPostUpdateTick=np.full(len(pre), tick-2, "<u4"))
            for field, value in edge_values.items():
                fill = np.nan if value.dtype == F else 0xffffffff
                fields[field] = np.full((npre, stride), fill, value.dtype)
                portable[f"ee_{field}"] = value
        cursor = 0
        for source, length in enumerate(lengths):
            end = cursor + int(length)
            indices[source, :length] = post[cursor:end]
            for field, matrix in fields.items():
                matrix[source, :length] = edge_values[field][cursor:end]
            cursor = end
        write_array(connections, graph_meta, f"{name}_rowLength", lengths)
        write_array(connections, graph_meta, f"{name}_ind", indices.ravel())
        for field, matrix in fields.items():
            write_array(native, meta, f"ee_{field}", matrix.ravel())
        if name == "ee":
            slots = np.concatenate([np.arange(int(n), dtype="<u4")+i*stride for i, n in enumerate(lengths)])
            write_array(connections, graph_meta, "ee_colLength", np.full(npost, s.ce, "<u4"))
            write_array(connections, graph_meta, "ee_remap", slots[np.argsort(post, kind="stable")])
        pending = (np.arange(npost, dtype=F)/16.+F(10.)) * (1 if name[0] == "e" else -2)
        write_array(native, meta, f"{name}_outPost", pending.reshape(1, -1))
        portable[f"{name}_input"] = pending
        portable[f"{name}_pre"], portable[f"{name}_post"] = pre, post
        saved[name] = pending
    graph_file = connections / "manifest.json"
    graph_file.write_text(json.dumps(graph_meta, indent=2)+"\n")
    meta["graph_manifest_sha256"] = sha256(graph_file)
    (native / "manifest.json").write_text(json.dumps(meta, indent=2)+"\n")
    portable["metadata"] = np.array(json.dumps(dict(format="brunel-genn-phase-checkpoint-v1",
        rule="morrison", dt_ms=.1, precision="float32", delay_steps=0, timestep=tick,
        model=s.model(), boundary=meta["boundary"])))
    np.savez(directory / "portable.npz", **portable)
    return native, portable, pre_trace, post_trace


class NativeCheckpointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = Path(os.environ["MR_NATIVE_TEST_DIR"])
        cls.output.mkdir(parents=True, exist_ok=False)
        cls.spec = Spec(ne=256, ni=64, ce=129, ci=8, pu=64, mc=2)
        cls.graph = fresh_graph(cls.output / "source-graph", cls.spec, 713)
        cls.path, cls.physical, cls.pre, cls.post = fixture(cls.output / "native", cls.graph)

    def test_state_edges_and_portable_equivalence(self):
        native = checkpoint_graph(self.path, self.output / "imported", self.spec)
        portable = checkpoint_graph(self.path.parent / "portable.npz", self.output / "portable-imported", self.spec)
        s, values, c = self.spec, self.physical, constants()
        f = native.state.view(F)
        np.testing.assert_array_equal(native.state, portable.state)
        for pop, start, n in (("exc", 0, s.ne), ("inh", s.ne, s.ni)):
            np.testing.assert_array_equal(f[start:start+n, 0], values[f"{pop}_V"])
            np.testing.assert_array_equal(f[start:start+n, 1], F(values[f"{pop}_Iex"]+values[f"{pop}_Iin"]))
            np.testing.assert_array_equal(f[start:start+n, 2], F(values[f"{pop}_dIex"]+values[f"{pop}_dIin"]))
            np.testing.assert_array_equal(native.state[start:start+n, 7], values[f"{pop}_refrac"])
        np.testing.assert_allclose(f[:s.ne, 3], F(self.pre.astype(F)*c["pot"]), rtol=2e-7, atol=0)
        np.testing.assert_allclose(f[:s.ne, 4], F(self.post.astype(F)*c["dep"]), rtol=2e-7, atol=0)
        for name, start, n, col in (("ee", 0, s.ne, 5), ("ie", 0, s.ne, 6),
                                  ("ei", s.ne, s.ni, 5), ("ii", s.ne, s.ni, 6)):
            np.testing.assert_array_equal(f[start:start+n, col], values[f"{name}_input"])
            for a, b in zip(native.arrays[name], self.graph.arrays[name]):
                np.testing.assert_array_equal(a, b)
        np.testing.assert_array_equal(native.weights, values["ee_g"])
        pairs = np.stack(native.arrays["ee"], axis=1)
        self.assertLess(len(np.unique(pairs, axis=0)), len(pairs))
        results, manifests = {}, []
        for name, graph in (("native-runtime", native), ("portable-runtime", portable)):
            output = self.output / name
            output.mkdir()
            (output / "graph").symlink_to(graph.directory.resolve(), target_is_directory=True)
            manifests.append(write_images(graph, output, ROOT / "sim/payloads/sys/morrison_g4.bin",
                group=4, functional=True, warmup=0, timed=7, seed=19260817, external=False))
            results[name] = check_images(output)
        self.assertEqual(manifests[0]["image_sha256"], manifests[1]["image_sha256"])
        (self.output / "image-results.json").write_text(json.dumps(results, indent=2)+"\n")

    def test_rejects_corruption_and_incompatible_state(self):
        for case in ("hash", "graph-hash", "model", "delay", "clock", "geometry", "trace"):
            with self.subTest(case=case):
                base = self.output / f"bad-{case}"
                shutil.copytree(self.path.parent, base)
                path = base / "settled"
                meta_path, graph_path = path / "manifest.json", base / "graph" / "manifest.json"
                meta, gm = json.loads(meta_path.read_text()), json.loads(graph_path.read_text())
                expected = {"hash": "size/hash", "graph-hash": "graph manifest hash", "model": "model.ce",
                            "delay": "checkpoint contract", "clock": "neuron clock", "geometry": "row geometry",
                            "trace": "edge trace copies disagree"}[case]
                if case == "hash":
                    meta["arrays"]["ee_g"]["sha256"] = "0"*64
                elif case == "graph-hash":
                    meta["graph_manifest_sha256"] = "0"*64
                elif case == "model":
                    meta["model"]["ce"] += 1
                elif case == "delay":
                    meta["compatibility"]["axonal_delay_steps"]["ee"] = 1
                elif case in ("clock", "trace"):
                    key = "exc_tick" if case == "clock" else "ee_preTrace"
                    data = np.load(path / f"{key}.npy")
                    data[0] += 1
                    write_array(path, meta, key, data)
                else:
                    key = "ee_rowLength"
                    data = np.load(base / "graph" / f"{key}.npy")
                    data[0] = 0xffffffff
                    write_array(base / "graph", gm, key, data)
                    graph_path.write_text(json.dumps(gm)+"\n")
                    meta["graph_manifest_sha256"] = sha256(graph_path)
                meta_path.write_text(json.dumps(meta)+"\n")
                with self.assertRaisesRegex(ValueError, expected):
                    checkpoint_graph(path, self.output / f"rejected-{case}", self.spec)


if __name__ == "__main__":
    unittest.main(verbosity=2)
