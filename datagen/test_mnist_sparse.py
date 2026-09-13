"""Structural checks for compact forward/reverse graphs, independent of RTL."""
from pathlib import Path
import re
import struct
import tempfile
from types import SimpleNamespace
import unittest

import mnist_sparse as sparse
import mnist_train as dense


def spm_images(images):
    section = struct.unpack_from("<I", images[0], 4)[0]
    result = []
    for pu in range(512):
        offset, size = struct.unpack_from("<II", images[0], section + pu * 8)
        result.append(images[0][section + offset:section + offset + size])
    return result


def header(spm, key):
    return struct.unpack_from("<I", spm, sparse.HEADER[key])[0]


class SparseImagesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.payload = Path(cls.directory.name) / "payload.bin"
        cls.payload.write_bytes(bytes.fromhex("6f000000"))
        cls.args = dict(payload=cls.payload, connection_rate=0.125, connectivity_seed=20260723,
            rule="three-trace", runtime_snapshot=None, weight_seed=19260817,
            poisson_seed=0x19260817, input_intensity=2.0, warmup_steps=0, timed_steps=1)
        cls.cases = {mode: sparse.make_images(SimpleNamespace(reverse_mode=mode, **cls.args))
                     for mode in sparse.MODES}

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_mask_nesting_and_realized_counts(self):
        masks = [sparse.structural_mask(p, 20260723) for p in (0.125, 0.25, 0.5)]
        self.assertEqual(list(map(sum, masks)), [39001, 77989, 156873])
        self.assertTrue(all(not a or b for a, b in zip(masks[0], masks[1])))
        self.assertTrue(all(not a or b for a, b in zip(masks[1], masks[2])))

    def test_local_reverse_resolves_every_forward_edge_exactly_once(self):
        for mode in ("local", "pu-scatter"):
            images, manifest = self.cases[mode]
            spms = spm_images(images)
            forward = set()
            reverse = set()
            for pu, spm in enumerate(spms):
                count, base = header(spm, "INPUT_COUNT"), header(spm, "INPUT_BASE")
                if not count:
                    continue
                for i in range(count):
                    state = base + i * sparse.INPUT_SIZE
                    row, length = struct.unpack_from("<II", spm, state + 20)
                    for index in range(length):
                        weight = row + index * 8 + 4
                        dst, descriptor = struct.unpack_from("<HH", spm, weight - 4)
                        target_spm = spms[dst - 1]
                        target_state = header(target_spm, "EXC_BASE") + (descriptor & 1) * sparse.EXC_SIZE
                        post = struct.unpack_from("<I", target_spm, target_state + 32)[0]
                        self.assertEqual(header(target_spm, "GROUP"), header(spm, "GROUP"))
                        forward.add((pu, state, weight, post))
                pointer = header(spm, "REVERSE_PTR")
                for post in range(50):
                    begin, end = struct.unpack_from("<HH", spm, pointer + post * 2)
                    degree = struct.unpack_from("<H", spm, header(spm, "DEGREES") + post * 2)[0]
                    self.assertEqual(end - begin, degree * 4)
                    states = [struct.unpack_from("<H", spm, offset)[0]
                              for offset in range(begin, end, 4)]
                    self.assertEqual(states, sorted(set(states)),
                                     "binary reverse lookup requires strictly ordered rows")
                    for offset in range(begin, end, 4):
                        state, weight = struct.unpack_from("<HH", spm, offset)
                        entry = (pu, state, weight, post)
                        self.assertNotIn(entry, reverse)
                        reverse.add(entry)
            self.assertEqual(len(forward), manifest["structural_synapses"])
            self.assertEqual(reverse, forward)

    def test_relevant_mask_descriptors_preserve_edges_and_encode_post(self):
        for mode in ("local", "dense"):
            images, manifest = sparse.make_images(SimpleNamespace(
                reverse_mode=mode, relevant_masks=True, **self.args))
            spms = spm_images(images)
            original = spm_images(self.cases[mode][0])
            self.assertTrue(manifest["relevant_masks"])
            for pu, spm in enumerate(spms):
                base, count = header(spm, "FORWARD_BASE"), header(spm, "FORWARD_COUNT")
                for edge in range(base, base + count * 8, 8):
                    dst, descriptor, weight = struct.unpack_from("<HHI", spm, edge)
                    old_dst, old_descriptor, old_weight = struct.unpack_from(
                        "<HHI", original[pu], edge)
                    target = spms[dst - 1]
                    post_offset = header(target, "EXC_BASE") + (descriptor & 1) * sparse.EXC_SIZE + 32
                    post = struct.unpack_from("<I", target, post_offset)[0]
                    self.assertEqual(descriptor >> 6, post)
                    self.assertEqual((dst, descriptor & 63, weight),
                                     (old_dst, old_descriptor, old_weight))

    def test_group_mask_trees_cover_each_group_once(self):
        for images, manifest in self.cases.values():
            spms = spm_images(images)
            for group in range(8):
                visited = set()
                pending = [manifest["neuron_owner_groups"][group][0]]
                while pending:
                    pu = pending.pop()
                    self.assertNotIn(pu, visited)
                    visited.add(pu)
                    spm = spms[pu - 1]
                    self.assertEqual(header(spm, "GROUP"), group)
                    count = header(spm, "MASK_CHILDREN")
                    self.assertLessEqual(count, 6)
                    children = struct.unpack_from(f"<{count}H", spm, header(spm, "PEERS"))
                    for child in children:
                        self.assertEqual(header(spms[child - 1], "GROUP_PARENT"), pu)
                    pending.extend(children)
                self.assertEqual(visited, set(manifest["input_owner_groups"][group]
                                             + manifest["neuron_owner_groups"][group]))

    def test_dense_direct_lookup_and_skips_match_compact_reverse(self):
        compact = spm_images(self.cases["local"][0])
        images, manifest = self.cases["dense"]
        self.assertTrue(all(p["scatter_rows"] == 0 for p in manifest["post_targets"]))
        empty_rows = 0
        for spm, reference in zip(spm_images(images), compact):
            count, base = header(spm, "INPUT_COUNT"), header(spm, "INPUT_BASE")
            if not count:
                continue
            for post in range(sparse.POSTS):
                begin, end = struct.unpack_from("<HH", spm, header(spm, "REVERSE_PTR") + post * 2)
                rb, re = struct.unpack_from("<HH", reference, header(reference, "REVERSE_PTR") + post * 2)
                expected = {(state - base) // sparse.INPUT_SIZE: weight for state, weight in
                            struct.iter_unpack("<HH", reference[rb:re])}
                self.assertEqual(end - begin, count * 4)
                empty_rows += not expected
                for i in range(count):
                    weight, skip = struct.unpack_from("<HH", spm, begin + i * 4)
                    self.assertEqual(weight, expected.get(i, 0))
                    self.assertEqual(skip, min((j for j in expected if j > i), default=count) - i)
                    # The payload's exact division must recover every local index.
                    self.assertEqual((((i * 28) >> 2) * 0xb6db6db7) & 0xffffffff, i)
                visited, i = {}, 0
                while i < count:
                    weight, skip = struct.unpack_from("<HH", spm, begin + i * 4)
                    if weight:
                        visited[i] = weight
                    i += skip
                self.assertEqual(i, count)
                self.assertEqual(visited, expected)
        self.assertGreater(empty_rows, 0)

    def test_global_rows_are_partitioned_by_destination_mc_and_only_nonempty(self):
        for mode in ("pu-scatter", "neuron-scatter"):
            images, manifest = self.cases[mode]
            spms = spm_images(images)
            observed = set()
            for pu, spm in enumerate(spms):
                group = header(spm, "GROUP")
                for local in range(header(spm, "EXC_COUNT")):
                    eo = header(spm, "EXC_BASE") + local * sparse.EXC_SIZE
                    post = group * 50 + struct.unpack_from("<I", spm, eo + 32)[0]
                    desc, count = struct.unpack_from("<II", spm, eo + 36)
                    for index in range(count):
                        mc_dst, lines, start, low, high = struct.unpack_from("<HHIII", spm, desc + index * 16)
                        mc = mc_dst - 0x8001
                        self.assertGreater(lines, 0)
                        self.assertEqual(start % 32, 0)
                        self.assertEqual(low | (high << 32), sum(1 << rank for rank, target in
                            enumerate(manifest["input_owner_groups"][group]) if (target - 1) // 32 == mc))
                        entries = 0
                        for pos in range(start, start + lines * 32, 8):
                            dst, state, data = struct.unpack_from("<HHI", images[mc], pos)
                            if dst == 0:
                                self.assertEqual((state, data), (0, 0))
                                continue
                            entries += 1
                            self.assertEqual((dst - 1) // 32, mc)
                            target = spms[dst - 1]
                            if mode == "pu-scatter":
                                self.assertEqual(state, 0)
                                self.assertEqual(data, post % 50)
                                self.assertGreater(struct.unpack_from("<H", target,
                                    header(target, "DEGREES") + data * 2)[0], 0)
                                item = (post, dst)
                            else:
                                self.assertEqual(header(target, "REVERSE_PTR"), 0)
                                self.assertEqual((state - header(target, "INPUT_BASE")) % 28, 0)
                                begin, length = struct.unpack_from("<II", target, state + 20)
                                self.assertTrue(begin + 4 <= data < begin + length * 8)
                                self.assertEqual((data - begin - 4) % 8, 0)
                                forward_dst, flags = struct.unpack_from("<HH", target, data - 4)
                                self.assertEqual((forward_dst, flags & 1), (pu + 1, local))
                                item = (post, dst, state, data)
                            self.assertNotIn(item, observed)
                            observed.add(item)
                        self.assertGreater(entries, 0)
            expected = (sum(t["pus"] for t in manifest["post_targets"]) if mode == "pu-scatter"
                        else manifest["structural_synapses"])
            self.assertEqual(len(observed), expected)

    def test_largest_rate_fits_and_keeps_identical_ownership(self):
        baseline = self.cases["local"][1]
        for mode in sparse.MODES:
            args = SimpleNamespace(**{**self.args, "connection_rate": 0.5, "reverse_mode": mode})
            _, manifest = sparse.make_images(args)
            self.assertEqual(manifest["logical_owners"], baseline["logical_owners"])
            self.assertTrue(all(pu["bytes"] <= sparse.SPM_LIMIT for pu in manifest["spm_usage"]))
            self.assertEqual(manifest["structural_synapses"], 156873)

    def test_header_matches_assembly(self):
        source = Path(__file__).resolve().parents[1] / "sim/payloads/sys/mnist_sparse_layout.inc"
        offsets = {name: int(value) for name, value in re.findall(r"#define H_(\w+)\s+(\d+)", source.read_text())}
        self.assertEqual(offsets, sparse.HEADER)

    def test_each_forward_edge_uses_its_global_post_cap(self):
        # Independently count forward edges across all owners. In particular,
        # per-PU local degree must never replace a post's global incoming count.
        from collections import Counter
        for mode in sparse.MODES:
            images, manifest = sparse.make_images(SimpleNamespace(**{**self.args, "reverse_mode": mode}))
            spms = spm_images(images)
            entries = []
            for spm in spms:
                for local in range(header(spm, "INPUT_COUNT")):
                    state = header(spm, "INPUT_BASE") + local * sparse.INPUT_SIZE
                    begin, count = struct.unpack_from("<II", spm, state + 20)
                    for edge in range(begin, begin + count * 8, 8):
                        dst, descriptor = struct.unpack_from("<HH", spm, edge)
                        target = spms[dst - 1]
                        eo = header(target, "EXC_BASE") + (descriptor & 1) * sparse.EXC_SIZE
                        post = header(target, "GROUP") * 50 + struct.unpack_from("<I", target, eo + 32)[0]
                        index = descriptor if mode == "neuron-scatter" else post % 50
                        cap = struct.unpack_from("<f", spm, header(spm, "WMAX_BASE") + 4 * index)[0]
                        entries.append((post, cap))
            degrees = Counter(post for post, _ in entries)
            for post, cap in entries:
                self.assertEqual(cap, dense.f32(784 / degrees[post]))
            self.assertEqual(manifest["fan_in"], [degrees[j] for j in range(400)])


if __name__ == "__main__":
    unittest.main()
