import unittest

from spike_counts import parse_spike_counts, parse_spike_total


class SpikeCountLogTest(unittest.TestCase):
    def test_tagged_local_counts_and_sum(self):
        log = ("[Periph] Output: 0\n"
               "[Periph] Output: 40000003\n[Periph] Output: 40000007\n"
               "[System] Result: 0 (0x0)\n")
        self.assertEqual(parse_spike_counts(log), [3, 7])
        self.assertEqual(parse_spike_counts(log, expected_pus=2), [3, 7])
        self.assertEqual(parse_spike_total(log, expected_pus=2), 10)

    def test_prior_output_is_not_a_report_record(self):
        log = ("[Periph] Output: 40000009\n"
               "[Periph] Output: 40000001\n[Periph] Output: 40000002\n"
               "[System] Result: 0 (0x0)\n")
        self.assertEqual(parse_spike_counts(log, expected_pus=2), [1, 2])

    def test_incomplete_or_failed_run(self):
        frame = "[Periph] Output: 40000001\n"
        for suffix in ("", "[System] Timed out at cycle 99\n",
                       "[System] Interrupted at cycle 99\n", "[System] Result: 1 (0x1)\n",
                       "[System] Result: 0 (0x1)\n",
                       "[System] Result: 0 (0x0)\n[System] Result: 0 (0x0)\n"):
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                parse_spike_counts(frame + suffix)

    def test_tag_and_record_validation(self):
        success = "[System] Result: 0 (0x0)\n"
        for words in ([], ["40000001"], ["1", "2"], ["40000001", "1"],
                      ["40000001", "40000002", "40000003"]):
            with self.subTest(words=words), self.assertRaises(ValueError):
                parse_spike_counts("".join(f"[Periph] Output: {w}\n" for w in words) + success,
                                   expected_pus=4)
        with self.assertRaises(ValueError):
            parse_spike_counts(success + "[Periph] Output: 40000001\n")


if __name__ == "__main__":
    unittest.main()
