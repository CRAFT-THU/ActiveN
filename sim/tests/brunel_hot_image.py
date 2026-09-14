"""Check the complete hot image before running its bare-metal payload."""

import argparse
import json
import struct
import sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "datagen"))
from brunel_validate import logical_weights  # noqa: E402


def check(images):
    manifest = json.loads((images / "manifest.json").read_text())
    initial = np.load(images / "initial.npz")
    hot = np.load(images / "cache-initial.npz")
    data = (images / "dram.0").read_bytes()
    section, mcs, pus = struct.unpack_from("<III", data, 4)
    assert (mcs, pus) == (16, 512)
    spms = []
    neurons = np.zeros_like(initial["neurons"])
    for pu in range(pus):
        off, size = struct.unpack_from("<II", data, section + pu * 8)
        assert size == 10240
        s = np.frombuffer(data[section + off : section + off + size], "<u4")
        spms.append(s)
        rows = s[64 : 64 + int(s[4]) * 20].reshape(-1, 20)
        neurons[rows[:, 11]] = rows
        np.testing.assert_array_equal(s[576:2176].reshape(800, 2), hot["entries"][pu])
        assert not np.any(s[11:46]) and not np.any(s[61:64])
        assert not np.any(s[5:8])
        packed = np.frombuffer(
            data[section + off + 0x2280 : section + off + 0x2280 + 200], np.uint8
        )
        unpacked = ((packed[:, None] >> (2 * np.arange(4))) & 3).reshape(-1)
        np.testing.assert_array_equal(unpacked, hot["usefulness"][pu])
    np.testing.assert_array_equal(neurons[:, :12], initial["neurons"][:, :12])
    logical, state = logical_weights(spms, neurons, initial, images, manifest["rule"])
    np.testing.assert_array_equal(logical, initial["weights"])
    assert state["pending_entries"] > 0 and state["dirty_synapses"] > 0
    print(
        "PASS: exact neurons, slots, packed sidecar, logical weights; quiescent runtime state"
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("images", type=Path)
    check(p.parse_args().images)
