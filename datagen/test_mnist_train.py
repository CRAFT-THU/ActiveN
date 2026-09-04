from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
import struct
from types import SimpleNamespace

import mnist_train


def f32_add(left: float, right: float) -> float:
    return mnist_train.f32(mnist_train.f32(left) + mnist_train.f32(right))


def f32_sub(left: float, right: float) -> float:
    return mnist_train.f32(mnist_train.f32(left) - mnist_train.f32(right))


def f32_mul(left: float, right: float) -> float:
    return mnist_train.f32(mnist_train.f32(left) * mnist_train.f32(right))


def reference_fifth_root(value: float) -> float:
    bits = mnist_train.f32_bits(value)
    shifted_exponent = (bits >> 23) + 3
    quotient = shifted_exponent // 5
    remainder = shifted_exponent - quotient * 5
    root_exponent = quotient - 26
    normalized = struct.unpack(
        "<f", struct.pack("<I", 0x3F800000 | (bits & 0x007FFFFF))
    )[0]
    z = f32_sub(f32_mul(normalized, 2.0), 3.0)
    estimate = mnist_train.f32(float.fromhex("0x1.4ded0ap-17"))
    coefficients = (
        (-1.0, "0x1.2c0604p-15"),
        (1.0, "0x1.d9c12ep-14"),
        (-1.0, "0x1.d40d74p-12"),
        (1.0, "0x1.f9744cp-10"),
        (-1.0, "0x1.3be602p-7"),
        (1.0, "0x1.28221p-4"),
        (1.0, "0x1.159ff2p+0"),
    )
    for sign, coefficient in coefficients:
        estimate = f32_add(
            f32_mul(estimate, z),
            sign * mnist_train.f32(float.fromhex(coefficient)),
        )
    remainder_scales = (
        "0x1p+0",
        "0x1.261118p+0",
        "0x1.51cb46p+0",
        "0x1.840600p+0",
        "0x1.bdb8cep+0",
    )
    remainder_scale = mnist_train.f32(
        float.fromhex(remainder_scales[remainder])
    )
    exponent_scale = struct.unpack(
        "<f", struct.pack("<I", (root_exponent + 127) << 23)
    )[0]
    return f32_mul(f32_mul(estimate, remainder_scale), exponent_scale)


def reference_first_step_checksums(
    post_fired_column: int | None = None,
    post_fired_columns: tuple[int, ...] = (),
) -> list[int]:
    fired_posts = set(post_fired_columns)
    if post_fired_column is not None:
        fired_posts.add(post_fired_column)
    pixels = [((index * 73 + 19) ^ (index >> 2)) & 0xFF for index in range(784)]
    fired = set(mnist_train.predict_input_spikes(pixels, 2.0, 0x19260817))
    weights = mnist_train.initial_weights(19260817, 78.0)
    pre_decay = mnist_train.f32(0.9875778005)
    post1_decay = mnist_train.f32(0.9875778005)
    post2_decay = mnist_train.f32(0.9937694906)
    checksums = []
    for rule in (1, 2, 3):
        checksum = 0
        for input_index in range(784):
            pre_fired = input_index in fired
            pre = f32_mul(
                mnist_train.f32((input_index % 29) / 31.0), pre_decay
            )
            effective_pre = (
                1.0 if rule == 3 and pre_fired
                else f32_add(pre, 1.0 if pre_fired else 0.0)
            )
            for column in range(400):
                weight = weights[input_index * 400 + column]
                post_fired = column in fired_posts
                post1 = f32_mul(
                    mnist_train.f32((column % 23) / 25.0), post1_decay
                )
                post2 = f32_mul(
                    mnist_train.f32((column % 31) / 33.0), post2_decay
                )
                if rule == 3:
                    if pre_fired:
                        weight = max(
                            0.0,
                            f32_sub(
                                weight,
                                f32_mul(mnist_train.f32(0.0001), post1),
                            ),
                        )
                    if post_fired:
                        weight = min(
                            1.0,
                            f32_add(
                                weight,
                                f32_mul(
                                    f32_mul(
                                        mnist_train.f32(0.01), post2
                                    ),
                                    effective_pre,
                                ),
                            ),
                        )
                elif rule == 2:
                    if pre_fired:
                        weight = max(
                            0.0,
                            f32_sub(
                                weight,
                                f32_mul(
                                    f32_mul(mnist_train.f32(0.0001), post1),
                                    reference_fifth_root(weight),
                                ),
                            ),
                        )
                    if post_fired:
                        weight = min(
                            1.0,
                            max(
                                0.0,
                                f32_add(
                                    weight,
                                    f32_mul(
                                        f32_mul(
                                            mnist_train.f32(0.0005),
                                            f32_sub(
                                                effective_pre,
                                                mnist_train.f32(0.4),
                                            ),
                                        ),
                                        reference_fifth_root(
                                            f32_sub(1.0, weight)
                                        ),
                                    ),
                                ),
                            ),
                        )
                elif post_fired:
                    weight = min(
                        1.0,
                        max(
                            0.0,
                            f32_add(
                                weight,
                                f32_mul(
                                    f32_mul(
                                        mnist_train.f32(0.0005),
                                        f32_sub(
                                            effective_pre, mnist_train.f32(0.4)
                                        ),
                                    ),
                                    reference_fifth_root(f32_sub(1.0, weight)),
                                ),
                            ),
                        ),
                    )
                checksum ^= mnist_train.f32_bits(weight)
        checksums.append(checksum)
    return checksums


def reference_three_trace_no_post_checksum(steps: int) -> int:
    pixels = [((index * 73 + 19) ^ (index >> 2)) & 0xFF
              for index in range(784)]
    thresholds = [mnist_train.poisson_threshold(pixel, 2.0)
                  for pixel in pixels]
    states = [mnist_train.initial_rng_state(0x19260817, index)
              for index in range(784)]
    weights = mnist_train.initial_weights(19260817, 78.0)
    post1 = [mnist_train.f32((column % 23) / 25.0)
             for column in range(400)]
    decay = mnist_train.f32(0.9875778005)
    eta = mnist_train.f32(0.0001)
    for _ in range(steps):
        states = [mnist_train.xorshift32(state) for state in states]
        fired = [index for index, (state, threshold)
                 in enumerate(zip(states, thresholds)) if state < threshold]
        post1 = [f32_mul(value, decay) for value in post1]
        depression = [f32_mul(eta, value) for value in post1]
        for input_index in fired:
            base = input_index * 400
            for column, delta in enumerate(depression):
                weights[base + column] = max(
                    0.0, f32_sub(weights[base + column], delta)
                )
    checksum = 0
    for weight in weights:
        checksum ^= mnist_train.f32_bits(weight)
    return checksum


class MnistTrainGeneratorTest(unittest.TestCase):
    @staticmethod
    def args(payload: Path) -> SimpleNamespace:
        return SimpleNamespace(
            payload=payload,
            pu_count=512,
            mc_count=16,
            mem_line_width=256,
            rule="three-trace",
            input_pus=196,
            neuron_pus=224,
            placement_policy="near",
            neuron_zone_policy="network-balanced",
            input_assignment="rate-balanced",
            horizontal_partition="none",
            recurrent_mode="tree",
            trace_destination_stride=mnist_train.TRACE_DESTINATION_STRIDE,
            trace_bundle=False,
            weight_seed=19260817,
            poisson_seed=0x19260817,
            checkpoint=None,
            frame=None,
            frame_index=0,
            input_intensity=2.0,
            warmup_steps=1,
            timed_steps=2,
        )

    @classmethod
    def setUpClass(cls) -> None:
        cls.tempdir = tempfile.TemporaryDirectory()
        cls.payload = Path(cls.tempdir.name) / "payload.bin"
        cls.payload.write_bytes(bytes(64))
        cls.dense_images, cls.dense = mnist_train.make_images(cls.args(cls.payload))

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tempdir.cleanup()

    def spm(self, images: list[bytes], manifest: dict, pu: int) -> bytes:
        start = manifest["spm_init_offset"] + 512 * 8 + pu * mnist_train.SPM_SIZE
        return images[0][start:start + mnist_train.SPM_SIZE]

    def test_populations_use_disjoint_specialized_pus(self) -> None:
        input_pus, neuron_pus, neuron_zones = mnist_train.grouped_pus(
            512, 16, 196, 224, "near"
        )
        input_per_pu, _ = mnist_train.placement(784, 512, input_pus)
        exc_per_pu, exc_locations = mnist_train.placement(400, 512, neuron_pus)
        self.assertFalse(set(input_pus) & set(neuron_pus))
        self.assertEqual(neuron_zones, [5, 6, 8, 15, 4, 9, 7])
        self.assertEqual({len(input_per_pu[pu]) for pu in input_pus}, {4})
        self.assertEqual({len(exc_per_pu[pu]) for pu in neuron_pus}, {1, 2})
        self.assertEqual(exc_locations[0][0], neuron_pus[0])
        self.assertEqual(exc_locations[-1][0], neuron_pus[-1])

    def test_neurons_fill_complete_mc_broadcast_ranges(self) -> None:
        for neuron_pu_count, expected_zones in (
            (128, [5, 6, 8, 15]),
            (192, [5, 6, 8, 15, 4, 9]),
            (224, [5, 6, 8, 15, 4, 9, 7]),
            (256, [5, 6, 8, 15, 4, 9, 7, 14]),
        ):
            _, neuron_pus, zones = mnist_train.grouped_pus(
                512, 16, 172, neuron_pu_count, "near"
            )
            self.assertEqual(zones, expected_zones)
            for zone in range(16):
                members = set(range(zone * 32, (zone + 1) * 32))
                self.assertIn(len(members & set(neuron_pus)), (0, 32))

    def test_network_balanced_neuron_zones_are_mc_aligned(self) -> None:
        _, neuron_pus, zones = mnist_train.grouped_pus(
            512, 16, 196, 224, "near", "network-balanced"
        )
        self.assertEqual(zones, [2, 4, 6, 7, 9, 14, 15])
        self.assertEqual(len(neuron_pus), 224)
        for zone in range(16):
            members = set(range(zone * 32, (zone + 1) * 32))
            self.assertIn(len(members & set(neuron_pus)), (0, 32))

        for neuron_pu_count, policy, expected in (
            (320, "network-balanced-320", [0, 1, 2, 4, 5, 9, 10, 11, 13, 15]),
            (352, "network-balanced-352", [0, 2, 3, 4, 7, 8, 10, 11, 12, 13, 15]),
        ):
            _, neuron_pus, zones = mnist_train.grouped_pus(
                512, 16, 512 - neuron_pu_count, neuron_pu_count,
                "near", policy,
            )
            self.assertEqual(zones, expected)
            self.assertEqual(len(neuron_pus), neuron_pu_count)

    def test_grouped_placement_rejects_empty_population_pus(self) -> None:
        with self.assertRaisesRegex(ValueError, "at least one neuron"):
            mnist_train.grouped_pus(512, 16, 96, 416, "near")

    def test_compact_layout_supports_more_than_256_neuron_pus(self) -> None:
        args = self.args(self.payload)
        args.input_pus = 160
        args.neuron_pus = 352
        _, manifest = mnist_train.make_images(args)
        self.assertEqual(manifest["feedforward_weight_slots_per_row"], 400)
        self.assertLessEqual(manifest["max_spm_bytes"], mnist_train.STACK0_BASE)

    def test_rate_balanced_input_assignment_preserves_capacity(self) -> None:
        input_pus, _, _ = mnist_train.grouped_pus(
            512, 16, 196, 224, "near"
        )
        pixels = [((index * 73 + 19) ^ (index >> 2)) & 0xFF
                  for index in range(784)]
        thresholds = [mnist_train.poisson_threshold(pixel, 2.0)
                      for pixel in pixels]
        per_pu, locations = mnist_train.input_placement(
            thresholds, 512, input_pus, "rate-balanced"
        )
        self.assertEqual({len(per_pu[pu]) for pu in input_pus}, {4})
        self.assertEqual(len(set(locations)), 784)
        loads = [sum(thresholds[index] for index in per_pu[pu])
                 for pu in input_pus]
        self.assertLess(max(loads), min(loads) * 1.02)

    def test_dense_layout_fits_each_pu(self) -> None:
        self.assertEqual(self.dense["feedforward_bytes"], 784 * 400 * 4)
        self.assertEqual(self.dense["feedforward_weight_slots_per_row"], 400)
        self.assertEqual(self.dense["feedforward_padding_slots_per_row"], 0)
        self.assertEqual(
            self.dense["feedforward_physical_weight_bytes"], 784 * 400 * 4
        )
        self.assertLessEqual(self.dense["max_spm_bytes"], 7800)
        self.assertGreaterEqual(self.dense["min_spm_free_bytes"], 8500)
        self.assertTrue(
            all(item["bytes"] <= mnist_train.STACK0_BASE
                for item in self.dense["per_pu"])
        )
        self.assertEqual(mnist_train.STACK1_TOP, mnist_train.SPM_SIZE)

    def test_dense_rows_are_bare_fp32_weights(self) -> None:
        args = self.args(self.payload)
        input_pus = [pu - 1 for pu in self.dense["input_pus"]]
        neuron_pus = [pu - 1 for pu in self.dense["neuron_pus"]]
        neuron_pu_ranks = {pu: rank for rank, pu in enumerate(neuron_pus)}
        _, exc_locations = mnist_train.placement(400, 512, neuron_pus)
        pixels = [((index * 73 + 19) ^ (index >> 2)) & 0xFF
                  for index in range(784)]
        thresholds = [mnist_train.poisson_threshold(pixel, args.input_intensity)
                      for pixel in pixels]
        input_per_pu, _ = mnist_train.input_placement(
            thresholds, 512, input_pus, args.input_assignment
        )
        pu = input_pus[0]
        spm = self.spm(self.dense_images, self.dense, pu)
        input_count, input_offset = struct.unpack_from("<2I", spm, 24)
        weight_count, weight_offset = struct.unpack_from("<2I", spm, 48)
        self.assertEqual(input_count, len(input_per_pu[pu]))
        self.assertEqual(weight_count, input_count * 400)
        first_offset, first_count = struct.unpack_from("<2I", spm, input_offset + 20)
        second_offset, second_count = struct.unpack_from(
            "<2I", spm, input_offset + mnist_train.INPUT_RECORD_SIZE + 20
        )
        self.assertEqual((first_offset, first_count), (weight_offset, 400))
        self.assertEqual((second_offset, second_count), (weight_offset + 1600, 400))

        logical_row = mnist_train.initial_weights(args.weight_seed, 78.0)[
            input_per_pu[pu][0] * 400:(input_per_pu[pu][0] + 1) * 400
        ]
        expected_slots = [0.0] * 400
        for column, (target_pu, target_local) in enumerate(exc_locations):
            target_rank = neuron_pu_ranks[target_pu]
            double_count = 400 - len(neuron_pus)
            slot = (
                target_rank * 2 + target_local
                if target_rank < double_count
                else target_rank + double_count
            )
            expected_slots[slot] = logical_row[column]
        expected = b"".join(struct.pack("<f", value) for value in expected_slots)
        self.assertEqual(spm[weight_offset:weight_offset + 1600], expected)

    def test_packed_destination_tables_match_placements(self) -> None:
        input_pus = [pu - 1 for pu in self.dense["input_pus"]]
        neuron_pus = [pu - 1 for pu in self.dense["neuron_pus"]]
        _, exc_locations = mnist_train.placement(400, 512, neuron_pus)

        for source_rank in (0, 1, 97, len(input_pus) - 1):
            input_spm = self.spm(
                self.dense_images, self.dense, input_pus[source_rank]
            )
            header = struct.unpack_from("<7I", input_spm, 268)
            self.assertEqual(header[:2], (196, 224))
            self.assertEqual(header[2], 0)
            self.assertNotEqual(header[3], 0)
            rotation = (
                source_rank * mnist_train.FEEDFORWARD_DESTINATION_STRIDE
            ) % len(neuron_pus)
            self.assertEqual(header[6], rotation)
            slotted_columns = []
            for rank, target_pu in enumerate(neuron_pus):
                record, = struct.unpack_from(
                    "<I", input_spm,
                    header[3] + rank * mnist_train.EXC_TARGET_RECORD_SIZE,
                )
                destination = record & 0xFFFF
                local0 = (record >> 16) & 0xFF
                local1 = (record >> 24) & 0xFF
                expected = [
                    (column, local)
                    for column, (pu, local) in enumerate(exc_locations)
                    if pu == target_pu
                ]
                self.assertIn(len(expected), (1, 2))
                self.assertEqual(destination, target_pu + 1)
                self.assertEqual(local0, expected[0][1])
                slotted_columns.append((rank * 2 + local0, expected[0][0]))
                if len(expected) == 2:
                    self.assertEqual(local1, expected[1][1])
                    slotted_columns.append((rank * 2 + local1, expected[1][0]))
                else:
                    self.assertEqual(local1, 0xFF)
            self.assertEqual(
                [column for _, column in sorted(slotted_columns)], list(range(400))
            )
        self.assertEqual(self.dense["feedforward_messages_per_input_spike"], 224)
        self.assertEqual(
            self.dense["feedforward_destination_order"], "input-pu-cyclic"
        )
        self.assertEqual(self.dense["feedforward_destination_stride"], 167)
        self.assertFalse(self.dense["trace_bundle"])
        self.assertEqual(
            self.dense["reverse_trace_messages_per_step"],
            400 * self.dense["expected_active_input_pus"],
        )
        self.assertEqual(
            self.dense["reverse_trace_max_messages_per_step"], 400 * 196
        )

        for rank in (0, 1, 195, 196, len(neuron_pus) - 1):
            neuron_spm = self.spm(
                self.dense_images, self.dense, neuron_pus[rank]
            )
            header = struct.unpack_from("<6I", neuron_spm, 268)
            self.assertNotEqual(header[2], 0)
            self.assertEqual(header[3], 0)
            destinations = struct.unpack_from("<196H", neuron_spm, header[2])
            rotation = (
                rank * self.args(self.payload).trace_destination_stride
            ) % len(input_pus)
            expected = input_pus[rotation:] + input_pus[:rotation]
            self.assertEqual(destinations, tuple(pu + 1 for pu in expected))
            self.assertEqual(set(destinations), {pu + 1 for pu in input_pus})
            fired_vector, observed_mask, observed_vector, observed_count = (
                struct.unpack_from("<4I", neuron_spm, 296)
            )
            self.assertEqual(fired_vector, 0)
            self.assertNotEqual(observed_mask, 0)
            self.assertEqual(observed_vector, observed_mask + 64)
            self.assertEqual(observed_count, 0)
            self.assertGreaterEqual(
                len(neuron_spm), observed_vector + len(input_pus) * 2
            )
            _, exc_offset = struct.unpack_from("<2I", neuron_spm, 32)
            exc_count, = struct.unpack_from("<I", neuron_spm, 32)
            for local in range(exc_count):
                slot, = struct.unpack_from(
                    "<I", neuron_spm,
                    exc_offset + local * mnist_train.EXC_RECORD_SIZE + 32,
                )
                double_count = 400 - len(neuron_pus)
                expected_slot = (
                    rank * 2 + local
                    if rank < double_count
                    else rank + double_count
                )
                self.assertEqual(slot, expected_slot)
        self.assertEqual(
            self.dense["trace_destination_order"], "neuron-pu-cyclic"
        )
        self.assertEqual(
            self.dense["trace_destination_stride"],
            mnist_train.TRACE_DESTINATION_STRIDE,
        )

        input_spm = self.spm(self.dense_images, self.dense, input_pus[0])
        fired_vector, observed_mask, observed_vector, observed_count = (
            struct.unpack_from("<4I", input_spm, 296)
        )
        input_count, = struct.unpack_from("<I", input_spm, 24)
        self.assertNotEqual(fired_vector, 0)
        self.assertEqual((observed_mask, observed_vector, observed_count), (0, 0, 0))
        self.assertGreaterEqual(len(input_spm), fired_vector + input_count * 4)

    def test_full_horizontal_partition_owns_every_synapse_once(self) -> None:
        args = self.args(self.payload)
        args.horizontal_partition = "full"
        images, manifest = mnist_train.make_images(args)
        self.assertEqual(manifest["input_pu_count"], 288)
        self.assertEqual(manifest["neuron_pu_count"], 224)
        self.assertEqual(manifest["physical_input_record_count"], 1568)
        self.assertEqual(
            manifest["feedforward_physical_weight_bytes"], 1568 * 400 * 4
        )
        self.assertEqual(manifest["feedforward_padding_slots_per_row"], 200)
        self.assertEqual([len(group) for group in manifest["input_owner_groups"]],
                         [144, 144])
        self.assertEqual([len(group) for group in manifest["neuron_groups"]],
                         [112, 112])
        self.assertEqual(
            {item["inputs"] for item in manifest["per_pu"] if item["inputs"]},
            {5, 6},
        )
        self.assertLess(manifest["max_spm_bytes"], mnist_train.STACK0_BASE)

        for zone in manifest["neuron_mc_zones"]:
            zone_members = set(range(zone * 32 + 1, (zone + 1) * 32 + 1))
            self.assertEqual(
                [len(zone_members & set(group)) for group in manifest["neuron_groups"]],
                [16, 16],
            )

        initial_weights = mnist_train.initial_weights(args.weight_seed, 78.0)
        for input_index in (0, 143, 399, 783):
            owners = sorted(
                manifest["input_owners"][input_index],
                key=lambda owner: owner["group_mask"],
            )
            self.assertEqual([owner["group_mask"] for owner in owners], [1, 2])
            self.assertEqual([owner["primary"] for owner in owners], [True, False])
            records = []
            rows = []
            for owner in owners:
                spm = self.spm(images, manifest, owner["pu"] - 1)
                input_offset, = struct.unpack_from("<I", spm, 28)
                record = struct.unpack_from(
                    "<IIfIIII", spm,
                    input_offset + owner["local"] * mnist_train.INPUT_RECORD_SIZE,
                )
                records.append(record)
                row = struct.unpack_from("<400I", spm, record[5])
                rows.append(row)
            self.assertEqual(records[0][0] & 0x3FF, input_index)
            self.assertEqual(records[1][0] & 0x3FF, input_index)
            self.assertTrue(records[0][0] & mnist_train.INPUT_GLOBAL_PRIMARY)
            self.assertFalse(records[1][0] & mnist_train.INPUT_GLOBAL_PRIMARY)
            self.assertTrue(records[0][0] & mnist_train.INPUT_GLOBAL_REPLICATED)
            self.assertTrue(records[1][0] & mnist_train.INPUT_GLOBAL_REPLICATED)
            self.assertEqual(records[0][2:4], records[1][2:4])
            self.assertTrue(all(rows[0][slot] != 0 for slot in range(200)))
            self.assertTrue(all(rows[0][slot] == 0 for slot in range(200, 400)))
            self.assertTrue(all(rows[1][slot] == 0 for slot in range(200)))
            self.assertTrue(all(rows[1][slot] != 0 for slot in range(200, 400)))
            physical = sorted(value for row in rows for value in row if value)
            logical = sorted(
                mnist_train.f32_bits(value)
                for value in initial_weights[input_index * 400:(input_index + 1) * 400]
            )
            self.assertEqual(physical, logical)

        first_owner = manifest["input_owners"][0][0]
        first_spm = self.spm(images, manifest, first_owner["pu"] - 1)
        horizontal_header = struct.unpack_from("<9I", first_spm, 312)
        self.assertEqual(horizontal_header[:6], (288, 112, 200, 1, 1, 0))
        self.assertEqual(horizontal_header[7:], (200, 0))

        expected_frame = {}
        pixels = [((index * 73 + 19) ^ (index >> 2)) & 0xFF
                  for index in range(784)]
        thresholds = [mnist_train.poisson_threshold(pixel, args.input_intensity)
                      for pixel in pixels]
        for input_index, owners in enumerate(manifest["input_owners"]):
            for owner in owners:
                expected_frame[(owner["pu"], owner["local"])] = thresholds[input_index]
        actual_frame = {}
        for mc, (start, lines) in enumerate(
            zip(manifest["frame_starts"], manifest["frame_lines"])
        ):
            frame = images[mc][start:start + lines * 32]
            for offset in range(0, len(frame), 8):
                pu, local, threshold = struct.unpack_from("<HHI", frame, offset)
                if pu:
                    actual_frame[(pu, local)] = threshold
        self.assertEqual(actual_frame, expected_frame)

    def test_four_way_horizontal_partition_uses_compact_rows(self) -> None:
        args = self.args(self.payload)
        args.horizontal_partition = "full-4"
        images, manifest = mnist_train.make_images(args)
        self.assertEqual(manifest["horizontal_group_count"], 4)
        self.assertEqual(manifest["input_pu_count"], 288)
        self.assertEqual(manifest["neuron_pu_count"], 224)
        self.assertEqual(manifest["physical_input_record_count"], 4 * 784)
        self.assertEqual(manifest["feedforward_weight_slots_per_row"], 100)
        self.assertEqual(manifest["feedforward_padding_slots_per_row"], 0)
        self.assertEqual(manifest["feedforward_padding_slots"], 0)
        self.assertEqual(
            manifest["feedforward_physical_weight_bytes"], 784 * 400 * 4
        )
        self.assertEqual(
            [len(group) for group in manifest["input_owner_groups"]],
            [72, 72, 72, 72],
        )
        self.assertEqual(
            [len(group) for group in manifest["neuron_groups"]],
            [56, 56, 56, 56],
        )
        self.assertEqual(
            {item["inputs"] for item in manifest["per_pu"] if item["inputs"]},
            {10, 11},
        )
        self.assertEqual(manifest["reverse_trace_max_messages_per_step"], 28800)
        self.assertLess(manifest["max_spm_bytes"], mnist_train.STACK0_BASE)

        for zone in manifest["neuron_mc_zones"]:
            zone_members = set(range(zone * 32 + 1, (zone + 1) * 32 + 1))
            self.assertEqual(
                [len(zone_members & set(group)) for group in manifest["neuron_groups"]],
                [8, 8, 8, 8],
            )

        initial_weights = mnist_train.initial_weights(args.weight_seed, 78.0)
        for input_index in (0, 143, 399, 783):
            owners = sorted(
                manifest["input_owners"][input_index],
                key=lambda owner: owner["group_mask"],
            )
            self.assertEqual(
                [owner["group_mask"] for owner in owners], [1, 2, 4, 8]
            )
            self.assertEqual(
                [owner["primary"] for owner in owners], [True, False, False, False]
            )
            self.assertEqual([owner["post_count"] for owner in owners], [100] * 4)
            rows = []
            for owner in owners:
                spm = self.spm(images, manifest, owner["pu"] - 1)
                input_offset, = struct.unpack_from("<I", spm, 28)
                record = struct.unpack_from(
                    "<IIfIIII", spm,
                    input_offset + owner["local"] * mnist_train.INPUT_RECORD_SIZE,
                )
                self.assertEqual(record[6], 100)
                rows.append(struct.unpack_from("<100I", spm, record[5]))
                horizontal_header = struct.unpack_from("<9I", spm, 312)
                self.assertEqual(horizontal_header[1:4], (56, 100, 1))
                self.assertEqual(horizontal_header[5], 0)
                self.assertEqual(horizontal_header[7], 100)
            physical = sorted(value for row in rows for value in row)
            logical = sorted(
                mnist_train.f32_bits(value)
                for value in initial_weights[
                    input_index * 400:(input_index + 1) * 400
                ]
            )
            self.assertEqual(physical, logical)

        for group_index, group in enumerate(manifest["neuron_groups"]):
            slots = []
            for pu_number in group:
                spm = self.spm(images, manifest, pu_number - 1)
                exc_count, exc_offset = struct.unpack_from("<2I", spm, 32)
                neuron_group, = struct.unpack_from("<I", spm, 336)
                self.assertEqual(neuron_group, group_index)
                for local in range(exc_count):
                    slot, = struct.unpack_from(
                        "<I", spm,
                        exc_offset + local * mnist_train.EXC_RECORD_SIZE + 32,
                    )
                    slots.append(slot)
            self.assertEqual(sorted(slots), list(range(100)))

    def test_partial_horizontal_partition_uses_every_available_input_pu(self) -> None:
        args = self.args(self.payload)
        args.horizontal_partition = "hot-partial"
        _, manifest = mnist_train.make_images(args)
        owner_counts = [len(owners) for owners in manifest["input_owners"]]
        self.assertEqual(owner_counts.count(2), 368)
        self.assertEqual(owner_counts.count(1), 416)
        self.assertEqual(
            {item["inputs"] for item in manifest["per_pu"] if item["inputs"]},
            {4},
        )
        self.assertEqual([len(group) for group in manifest["input_owner_groups"]],
                         [196, 196])
        pixels = [((index * 73 + 19) ^ (index >> 2)) & 0xFF
                  for index in range(784)]
        scores = [mnist_train.poisson_threshold(pixel, args.input_intensity)
                  for pixel in pixels]
        expected_hot = set(sorted(range(784), key=lambda i: (-scores[i], i))[:368])
        actual_hot = {index for index, count in enumerate(owner_counts) if count == 2}
        self.assertEqual(actual_hot, expected_hot)
        self.assertEqual(manifest["physical_input_record_count"], 1152)
        self.assertLess(manifest["max_spm_bytes"], mnist_train.STACK0_BASE)

    def test_full_horizontal_mapping_does_not_depend_on_input_rates(self) -> None:
        input_pus, _, _ = mnist_train.grouped_pus(
            512, 16, 288, 224, "near", "network-balanced"
        )
        first, _, _ = mnist_train.horizontal_input_placement(
            [float(index) for index in range(784)], input_pus, 512, "full"
        )
        second, _, _ = mnist_train.horizontal_input_placement(
            [float(783 - index) for index in range(784)], input_pus, 512, "full"
        )
        self.assertEqual(first, second)
        first4, _, _ = mnist_train.horizontal_input_placement(
            [float(index) for index in range(784)], input_pus, 512, "full-4"
        )
        second4, _, _ = mnist_train.horizontal_input_placement(
            [float(783 - index) for index in range(784)], input_pus, 512, "full-4"
        )
        self.assertEqual(first4, second4)

    def test_deterministic_frame_spikes(self) -> None:
        pixels = [((index * 73 + 19) ^ (index >> 2)) & 0xFF for index in range(784)]
        self.assertEqual(
            mnist_train.predict_input_spikes(pixels, 2.0, 0x19260817),
            [100, 117, 205, 265, 273, 302, 305, 351, 583, 643, 671, 720],
        )
        self.assertEqual(
            mnist_train.predict_input_spikes(pixels, 0.2, 0x19260817), [265]
        )

    def test_images_have_expected_shape(self) -> None:
        self.assertEqual(len(self.dense_images), 16)
        nonempty = sum(lines != 0 for lines in self.dense["frame_lines"])
        self.assertGreater(nonempty, 0)
        self.assertLessEqual(
            sum(self.dense["frame_lines"]) * 4,
            784 + 3 * nonempty,
        )
        self.assertGreaterEqual(sum(self.dense["frame_lines"]) * 4, 784)
        self.assertNotEqual(self.dense["initial_weight_checksum"], 0)
        self.assertEqual(self.dense["warmup_steps"], 1)
        self.assertEqual(self.dense["timed_steps"], 2)

    def test_step_counts_are_stored_in_each_spm_header(self) -> None:
        args = self.args(self.payload)
        spm_start = self.dense["spm_init_offset"] + args.pu_count * 8
        for pu in (0, 255, 511):
            start = spm_start + pu * mnist_train.SPM_SIZE
            warmup, timed, completed = struct.unpack_from(
                "<3I", self.dense_images[0], start + 256
            )
            self.assertEqual((warmup, timed, completed), (1, 2, 0))

    def test_reference_initial_membrane_voltages(self) -> None:
        args = self.args(self.payload)
        pu = self.dense["neuron_pus"][0] - 1
        spm = self.spm(self.dense_images, self.dense, pu)
        _, exc_offset = struct.unpack_from("<2I", spm, 32)
        _, inh_offset = struct.unpack_from("<2I", spm, 40)
        self.assertEqual(struct.unpack_from("<f", spm, exc_offset + 8)[0], -105.0)
        self.assertEqual(struct.unpack_from("<f", spm, inh_offset + 8)[0], -100.0)

    def test_recurrent_modes_encode_only_i_to_e_scatter(self) -> None:
        tree_spm = self.spm(
            self.dense_images, self.dense, self.dense["neuron_pus"][0] - 1
        )
        _, tree_inh_offset = struct.unpack_from("<2I", tree_spm, 40)
        self.assertEqual(
            struct.unpack_from("<16I", tree_spm, tree_inh_offset + 84),
            (0,) * 16,
        )

        args = self.args(self.payload)
        args.recurrent_mode = "scatter"
        scatter_images, scatter = mnist_train.make_images(args)
        scatter_spm = self.spm(
            scatter_images, scatter, scatter["neuron_pus"][0] - 1
        )
        _, scatter_inh_offset = struct.unpack_from("<2I", scatter_spm, 40)
        lines = struct.unpack_from("<16I", scatter_spm, scatter_inh_offset + 84)
        self.assertEqual({index for index, count in enumerate(lines) if count},
                         set(scatter["neuron_mc_zones"]))

    def test_first_step_weight_checksums_for_all_rules(self) -> None:
        self.assertEqual(
            reference_first_step_checksums(),
            [0x032674CA, 0x02EB6E72, 0x0239BEC0],
        )

    def test_same_step_pre_and_post_weight_checksums_for_all_rules(self) -> None:
        self.assertEqual(
            reference_first_step_checksums(post_fired_column=1),
            [0x0561F569, 0x04AC8FFC, 0x0480E684],
        )

    def test_two_same_step_posts_weight_checksums_for_all_rules(self) -> None:
        self.assertEqual(
            reference_first_step_checksums(post_fired_columns=(0, 224)),
            [0x0400DB44, 0x05CBE328, 0x07851CB3],
        )

    def test_colocated_same_step_posts_weight_checksums_for_all_rules(self) -> None:
        _, neuron_pus, _ = mnist_train.grouped_pus(
            512, 16, 196, 224, "near", "network-balanced"
        )
        neuron_pus = mnist_train.compact_neuron_pu_order(neuron_pus)
        _, locations = mnist_train.placement(400, 512, neuron_pus)
        self.assertEqual(locations[2][0], locations[3][0])
        self.assertNotEqual(locations[2][1], locations[3][1])
        self.assertEqual(
            reference_first_step_checksums(post_fired_columns=(2, 3)),
            [0x049C8764, 0x05506233, 0x0409FC8F],
        )

    def test_sixteen_step_three_trace_checksum_without_posts(self) -> None:
        self.assertEqual(reference_three_trace_no_post_checksum(16), 0x0542959E)


if __name__ == "__main__":
    unittest.main()
