import unittest

from spike_counts import parse_spike_total


class SpikeCountLogTest(unittest.TestCase):
    def test_zero_and_full_width(self):
        self.assertEqual(parse_spike_total(
            "[Periph] Output: 53504b31\n[Periph] Output: 0\n"
            "[Periph] Output: 0\n[System] Result: 0 (0x0)\n"), 0)
        self.assertEqual(parse_spike_total(
            "[Periph] Output: 53504b31\n[Periph] Output: 89abcdef\n"
            "[Periph] Output: 12345678\n[System] Result: 0 (0x0)\n"),
            0x1234567889ABCDEF)

    def test_checksum_equal_to_marker(self):
        self.assertEqual(parse_spike_total(
            "[Periph] Output: 53504b31\n[Periph] Output: 53504b31\n"
            "[Periph] Output: 7\n[Periph] Output: 0\n"
            "[System] Result: 0 (0x0)\n[System] Timer: 123 cycles\n"), 7)

    def test_incomplete_or_failed_run(self):
        frame = "[Periph] Output: 53504b31\n[Periph] Output: 1\n[Periph] Output: 0\n"
        for suffix in ("", "[System] Timed out at cycle 99\n",
                       "[System] Interrupted at cycle 99\n", "[System] Result: 1 (0x1)\n",
                       "[System] Result: 0 (0x1)\n",
                       "[System] Result: 0 (0x0)\n[System] Result: 0 (0x0)\n"):
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                parse_spike_total(frame + suffix)

    def test_frame_must_be_complete_and_last(self):
        success = "[System] Result: 0 (0x0)\n"
        for words in ([], ["53504b31"], ["53504b31", "1"], ["1", "2", "3"],
                      ["53504b31", "1", "0", "9"], ["53504b31", "100000000", "0"],
                      ["53504b31", "-1", "0"], ["53504b31", "xyz", "0"]):
            with self.subTest(words=words), self.assertRaises(ValueError):
                parse_spike_total("".join(f"[Periph] Output: {w}\n" for w in words) + success)
        with self.assertRaises(ValueError):
            parse_spike_total(success + "[Periph] Output: 53504b31\n")


if __name__ == "__main__":
    unittest.main()
