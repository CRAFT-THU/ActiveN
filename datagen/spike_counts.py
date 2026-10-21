"""Read per-PU firing counts from a completed simulator log.

Each PU emits one auxiliary word tagged with ``0x40000000`` after the final
timed region. The lower 30 bits contain that PU's local u32 count. The caller
must still check the simulator process exit status and compare the returned
records with the reference per-PU counts.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

TAG = 0x40000000
TAG_MASK = 0xC0000000
COUNT_MASK = 0x3FFFFFFF
OUTPUT = re.compile(r"\[Periph\] Output: ([0-9a-fA-F]{1,8})")
RESULT = re.compile(r"\[System\] Result: ([0-9]+) \(0x([0-9a-fA-F]+)\)")


def _outputs(log: str) -> tuple[list[int], list[int]]:
    words: list[int] = []
    results: list[int] = []
    for line in log.splitlines():
        if line.startswith("[Periph] Output:"):
            match = OUTPUT.fullmatch(line)
            if match is None:
                raise ValueError("malformed 32-bit peripheral output")
            if results:
                raise ValueError("peripheral output after simulator result")
            words.append(int(match[1], 16))
        elif line.startswith("[System] Result:"):
            match = RESULT.fullmatch(line)
            if match is None or int(match[1]) != int(match[2], 16):
                raise ValueError("malformed simulator result")
            results.append(int(match[1]))
        elif line.startswith(("[System] Timed out", "[System] Interrupted")):
            raise ValueError("simulation did not complete")
    if results != [0]:
        raise ValueError("expected exactly one successful simulator result")
    return words, results


def parse_spike_counts(log: str, expected_pus: int | None = None) -> list[int]:
    """Return the trailing tagged local counts in peripheral-output order."""
    words, _ = _outputs(log)
    first = len(words)
    while first and (words[first - 1] & TAG_MASK) == TAG:
        first -= 1
    tagged = words[first:]
    if not tagged:
        raise ValueError("missing trailing per-PU firing-count records")
    if any((word & TAG_MASK) != TAG for word in tagged):
        raise ValueError("non-contiguous tagged firing-count records")
    if expected_pus is not None:
        if len(tagged) < expected_pus:
            raise ValueError("incomplete per-PU firing-count records")
        tagged = tagged[-expected_pus:]
    return [word & COUNT_MASK for word in tagged]


def parse_spike_total(log: str, expected_pus: int | None = None) -> int:
    """Compatibility helper returning the sum of local records."""
    return sum(parse_spike_counts(log, expected_pus))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--expected-pus", type=int)
    args = parser.parse_args()
    counts = parse_spike_counts(args.log.read_text(), args.expected_pus)
    print(json.dumps({"spike_counts": counts, "spike_total": sum(counts)}))


if __name__ == "__main__":
    main()
