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
    with np.load(images / "initial.npz") as archive:
        initial = {key: archive[key] for key in archive.files}
    proxy = "weight_owner" in initial
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
        if proxy:
            from brunel_proxy import CONFIG, ROWS, capacity, groups, magic
            group_size = manifest["proxy_size"]
            assert s[(CONFIG + 56) // 4] == magic(group_size, manifest["proxy_updates"])
            members, rank = groups(manifest["proxy_groups"], group_size)
            np.testing.assert_array_equal(s[CONFIG // 4:CONFIG // 4 + group_size], members[pu] + 1)
            assert not np.any(s[0x900 // 4:CONFIG // 4])
            assert not np.any(s[0xA00 // 4:ROWS // 4])
            assert not np.any(s[(ROWS + capacity(group_size) * 32) // 4:0x2D80 // 4])
            if manifest["proxy_updates"] != "replicated":
                assert not np.any(s[ROWS // 4:(ROWS + capacity(group_size) * 32) // 4:8])
        else:
            assert not np.any(s[0x900 // 4 : 0x2D80 // 4])
        assert not np.any(s[0x2E00 // 4 : 0x3800 // 4])
        assert s[0x2D80 // 4 + 7] == 256
        np.testing.assert_array_equal(s[POISSON_BASE // 4 : POISSON_BASE // 4 + 9], poisson_thresholds(manifest["constants"]["external_mean"]))
        rows = s[64 : 64 + int(s[4]) * 20].reshape(-1, 20)
        if not proxy:
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
        if proxy:
            expected_owners = initial["weight_owner"][lo:hi]
            count = []
            for owner in members[src % pus]:
                subset = np.flatnonzero(expected_owners == owner) + lo
                slot = int(initial["proxy_slots"][src])
                row = spms[owner][ROWS // 4 + slot * 8:ROWS // 4 + (slot + 1) * 8]
                assert row[2] == src and row[4] == len(subset)
                count.append(len(subset))
                lane = np.arange(len(subset))
                indices = row[3] // 4 + lane // 128 * 256 + lane % 128
                addresses[subset] = indices * 4
                target = initial["post"][subset]
                expected = ((256 + (target // pus) * 80 + 68).astype(np.uint32) << 16) | (target % pus + 1).astype(np.uint32)
                np.testing.assert_array_equal(drams[owner // 32][indices + 128], expected)
            assert max(count) - min(count) <= 1
            continue
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
    if proxy:
        np.testing.assert_array_equal(addresses, initial["weight_address"])
        owner = initial["weight_owner"]
        unique = owner.astype(np.uint64) // 32 << 32 | addresses
        assert len(np.unique(unique)) == len(unique)
    else:
        owner = pre % pus
    local = pre // pus
    if manifest.get("reverse_index") == "neuron-spm-byte-offset":
        local = 256 + local * 80
    if proxy:
        if manifest["proxy_updates"] == "home":
            local = (256 + (pre // pus) * 80) | (owner // 32)
            owner = pre % pus
        elif manifest["proxy_updates"] == "relay":
            local = (256 + (pre // pus) * 80) | rank[owner]
            # Decode exactly the destination used by the home handler; this
            # must name the physical weight owner even within a shared MC.
            np.testing.assert_array_equal(members[pre % pus, local & 15], owner)
            np.testing.assert_array_equal(local & ~15, 256 + (pre // pus) * 80)
            owner = pre % pus
        else:
            local = ROWS + initial["proxy_slots"][pre] * 32
    expected = ((owner + 1).astype(np.uint64)
                | (local.astype(np.uint64) << 16)
                | (addresses.astype(np.uint64) << 32))
    keys = post.astype(np.int64) * mcs + owner // 32
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
    # Static routing is checked independently too: grouping changes the sender
    # and row directory, but must neither lose nor duplicate a destination.
    pre, post = initial["static_pre"], initial["static_post"]
    from brunel import constants, F
    c = constants(manifest["rule"])
    weight = np.where(pre < 9000, F(c["epsc"] * c["delivery"] * c["external_weight"]),
                      F(c["epsc"] * c["delivery"] * c["inhibitory_weight"])).astype(F).view(np.uint32)
    source = pre if not proxy else pre * group_size + rank[initial["static_owner"]]
    nsource = 11250 * (group_size if proxy else 1)
    if proxy:
        counts = np.bincount(source, minlength=nsource).reshape(-1, group_size)
        assert np.all(counts.max(axis=1) - counts.min(axis=1) <= 1)
        actual_owner = initial["static_owner"]
        np.testing.assert_array_equal(members[pre % pus, rank[actual_owner]], actual_owner)
    keys = source.astype(np.int64) * mcs + (post % pus) // 32
    expected = ((post % pus + 1).astype(np.uint64)
        | ((256 + (post // pus) * 80 + np.where(pre < 9000, 68, 72)).astype(np.uint64) << 16)
        | (weight.astype(np.uint64) << 32))
    order = np.argsort(keys, kind="stable")
    expected = expected[order]
    off = np.r_[0, np.cumsum(np.bincount(keys, minlength=nsource * mcs))]
    for src in range(nsource):
        gid = src // group_size if proxy else src
        owner = members[gid % pus, src % group_size] if proxy else gid % pus
        if proxy:
            slot = int(initial["proxy_slots"][gid])
            ptr = int(spms[owner][ROWS // 4 + slot * 8 + 5])
        else:
            ptr = int(neurons[gid, 15])
        directory = drams[owner // 32][ptr // 4:ptr // 4 + 32].reshape(mcs, 2)
        for mc, (address, count) in enumerate(directory):
            lo, hi = off[src * mcs + mc:src * mcs + mc + 2]
            assert int(count) == hi - lo
            if count:
                actual = drams[mc][int(address) // 4:int(address) // 4 + int(count) * 2].view(np.uint64)
                np.testing.assert_array_equal(actual, expected[lo:hi])
    print("PASS: exact neurons/weights/plastic, static and reverse routing; balanced proxy rows")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("images", type=Path)
    check(parser.parse_args().images)
