"""Read the workload's final measured-window firing total from a simulator log.

All populations are included, warmup is excluded. Callers must also check the
simulator process's exit status; a saved log cannot establish that exit status.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

MARKER = 0x53504B31  # SPK1; see sim/payloads/sys/spike_count.h
OUTPUT = re.compile(r"\[Periph\] Output: ([0-9a-fA-F]{1,8})")
RESULT = re.compile(r"\[System\] Result: ([0-9]+) \(0x([0-9a-fA-F]+)\)")


def parse_spike_total(log: str) -> int:
    """Return the final SPK1 low/high frame, requiring one successful result.

    Earlier auxiliary words (such as the existing MNIST checksum) are not part
    of the frame. Read from the end so a checksum equal to SPK1 is unambiguous.
    """
    words = []
    results = []
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
    if len(words) < 3 or words[-3] != MARKER:
        raise ValueError("missing final SPK1 low/high firing-count frame")
    return words[-2] + (words[-1] << 32)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    print(json.dumps({"spike_total": parse_spike_total(args.log.read_text())}))


if __name__ == "__main__":
    main()
