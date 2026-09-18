"""Validate an ASM timestep-buffer image before running the simulator."""

import argparse
import json
import struct
import sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "datagen"))
from brunel_validate import logical_weights  # noqa: E402
from brunel import DATA_END, MAGIC, POISSON_BASE, poisson_thresholds  # noqa: E402


def check(images):
    manifest = json.loads((images / "manifest.json").read_text())
    assert manifest["kernel"] == "timestep-buffer-mshr"
    initial = np.load(images / "initial.npz")
    data = (images / "dram.0").read_bytes()
    section, mcs, pus = struct.unpack_from("<III", data, 4)
    assert (mcs, pus) == (16, 512)
    spms = []
    neurons = np.zeros_like(initial["neurons"])
    for pu in range(pus):
        off, size = struct.unpack_from("<II", data, section + pu * 8)
        assert size == DATA_END
        s = np.frombuffer(data[section + off : section + off + size], "<u4")
        assert s[0] == MAGIC
        assert not np.any(s[11:46]) and not np.any(s[61:64])
        assert not np.any(s[0x900 // 4 : 0x2D80 // 4])
        assert not np.any(s[0x2E00 // 4 : 0x3800 // 4])
        assert s[0x2D80 // 4 + 7] == 256
        np.testing.assert_array_equal(s[POISSON_BASE // 4 : POISSON_BASE // 4 + 9], poisson_thresholds(manifest["constants"]["external_mean"]))
        rows = s[64 : 64 + int(s[4]) * 20].reshape(-1, 20)
        assert not np.any(rows[:, 19])
        neurons[rows[:, 11]] = rows
        spms.append(s)
    np.testing.assert_array_equal(neurons[:, :12], initial["neurons"][:, :12])
    logical, _ = logical_weights(spms, neurons, initial, images, manifest["rule"])
    np.testing.assert_array_equal(logical, initial["weights"])
    drams = [np.fromfile(images / f"dram.{mc}", dtype="<u4") for mc in range(mcs)]
    offsets = np.searchsorted(initial["pre"], np.arange(9001))
    addresses = np.empty(len(initial["pre"]), dtype=np.uint32)
    for src in range(9000):
        lo, hi = offsets[src : src + 2]
        lane = np.arange(hi - lo)
        base = int(neurons[src, 12]) // 4
        indices = base + lane // 128 * 256 + lane % 128
        addresses[lo:hi] = indices * 4
        actual_targets = drams[(src % pus) // 32][indices + 128]
        post = initial["post"][lo:hi]
        expected_targets = ((256 + (post // pus) * 80 + 68).astype(np.uint32) << 16) | (post % pus + 1).astype(np.uint32)
        np.testing.assert_array_equal(actual_targets, expected_targets)
        delta = (indices - base) * 4
        np.testing.assert_array_equal((delta >> 10) * 512 + (delta & 511), lane * 4)
    # Independently check every reverse entry, including its full weight address.
    # The new encoding replaces the local neuron index with its SPM byte offset.
    pre, post = initial["pre"], initial["post"]
    local = pre // pus
    if manifest.get("reverse_index") == "neuron-spm-byte-offset":
        local = 256 + local * 80
    expected = ((pre % pus + 1).astype(np.uint64)
                | (local.astype(np.uint64) << 16)
                | (addresses.astype(np.uint64) << 32))
    keys = post.astype(np.int64) * mcs + (pre % pus) // 32
    order = np.argsort(keys, kind="stable")
    expected = expected[order]
    ro = np.r_[0, np.cumsum(np.bincount(keys, minlength=9000 * mcs))]
    for post_id in range(9000):
        home_mc = (post_id % pus) // 32
        start = int(neurons[post_id, 16]) // 4
        directory = drams[home_mc][start : start + mcs * 2].reshape(mcs, 2)
        for mc, (address, count) in enumerate(directory):
            lo, hi = ro[post_id * mcs + mc : post_id * mcs + mc + 2]
            assert int(count) == hi - lo
            if count:
                start = int(address) // 4
                actual = drams[mc][start : start + int(count) * 2].view(np.uint64)
                np.testing.assert_array_equal(actual, expected[lo:hi])
    print("PASS: exact neurons/weights/forward and reverse CSR; empty buffers/MSHRs")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("images", type=Path)
    check(parser.parse_args().images)
