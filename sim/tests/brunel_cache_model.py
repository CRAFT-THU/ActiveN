"""Differential cache state/traffic regression, plus five-policy eager oracle."""

import argparse
import ctypes as C
import sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "datagen"))
from brunel_cache import CacheModel, METRICS, ptr  # noqa: E402
from brunel_validate import pending_power  # noqa: E402


def run(args):
    oracle = C.CDLL(str(args.oracle.resolve()))
    oracle.oracle_reset.argtypes = [C.c_void_p, C.c_uint, C.c_uint]
    oracle.oracle_apply.argtypes = [C.c_uint] * 3
    oracle.oracle_forward.argtypes = [C.c_uint, C.c_uint, C.c_void_p]
    oracle.oracle_export.argtypes = [C.c_void_p]
    rng = np.random.default_rng(19260817)
    for rule in ("additive", "morrison"):
        for policy in range(1, 6):
            base = 10 + np.arange(65536, dtype=np.float32) % 101 * np.float32(0.3)
            model = CacheModel(
                args.library,
                [base.tobytes()] * 16,
                rule,
                91.2192,
                policy=policy,
                pus=16,
            )
            expected = base.copy()
            dram = base.view(np.uint32).copy()
            oracle.oracle_reset(
                ptr(dram), rule == "morrison", int(np.float32(91.2192).view(np.uint32))
            )
            spm = np.empty(2560, dtype=np.uint32)
            for step in range(12000):
                addr = 0x10000 + 4 * int(rng.integers(8192))
                if step % 3 == 0:
                    addr &= ~31
                    count = int(rng.integers(1, 9))
                    got = model.forward([(0, addr, count, 0)], count)
                    np.testing.assert_array_equal(
                        got, expected[addr // 4 : addr // 4 + count]
                    )
                    if policy == 1:
                        want = np.empty(count, dtype=np.uint32)
                        oracle.oracle_forward(addr, count, ptr(want))
                        np.testing.assert_array_equal(got.view(np.uint32), want)
                else:
                    pot = int(rng.integers(2))
                    # Include zero coefficients, clipping, repeated pending updates.
                    c = np.float32(rng.choice([0, 0.0001, 0.05, 0.9, 5]))
                    cb = int(c.view(np.uint32))
                    model.apply([(0, addr, cb, pot)])
                    w = expected[addr // 4]
                    coefficient = c
                    if rule == "morrison":
                        coefficient = np.float32(c * (pending_power(w) if pot else w))
                    w = max(
                        np.float32(0),
                        np.float32(w + coefficient if pot else w - coefficient),
                    )
                    expected[addr // 4] = (
                        w if rule == "morrison" else min(np.float32(91.2192), w)
                    )
                    if policy == 1:
                        oracle.oracle_apply(addr, cb, pot)
                if step % 97 == 0 or step == 11999:
                    state = model.export()
                    tags = state["entries"][0, :, 0]
                    addresses = np.arange(0x10000, 0x18000, 4, dtype=np.uint32)
                    got = model.logical(
                        np.zeros(len(addresses), dtype=np.uint32), addresses
                    )
                    np.testing.assert_array_equal(got, expected[0x4000:0x6000])
                    assert len(
                        np.unique(tags[tags != 0] & ~np.uint32(3))
                    ) == np.count_nonzero(tags)
                    assert np.all(state["usefulness"] <= 3)
                    if policy >= 4:
                        assert np.all(state["usefulness"][0, tags != 0] > 0)
                    if policy == 1:
                        oracle.oracle_export(ptr(spm))
                        np.testing.assert_array_equal(
                            state["entries"][0], spm[576:2176].reshape(800, 2)
                        )
                        np.testing.assert_array_equal(model.drams[0], dram)
                        for name, word in dict(
                            hits=30,
                            misses=31,
                            pending_ops=32,
                            evictions=33,
                            write_words=34,
                            forced=35,
                            probes=36,
                            weight_line_loads=62,
                        ).items():
                            assert (
                                state["stats"][0, METRICS.index(name)] == spm[word]
                            ), (step, name)
            print(
                rule,
                policy,
                "PASS: eager weights, legal states"
                + (", exact C slots/backing/counters" if policy == 1 else ""),
                flush=True,
            )
            model.close()


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--oracle", type=Path, required=True)
    run(p.parse_args())
