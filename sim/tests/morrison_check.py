#!/usr/bin/env python3
"""FP32 state/learning oracle for explicitly deterministic functional fixtures."""
import argparse
import csv
import json
from pathlib import Path
import struct
import sys
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "datagen"))
from morrison_model import F, NODE_BASE, NODE_BYTES, CONFIG, FIRED_LIST, JOBS, constants, poisson_thresholds
from morrison_validate import Image


def check(directory, run):
    image, run = Image(directory), Path(run)
    spec, manifest = image.spec, image.manifest
    if not manifest["functional"]:
        raise ValueError("this oracle is for functional fixtures, not benchmarks")
    # Only active forward/reverse rows are materialized. This also supports the
    # full 810M-edge image without a Python object or host copy per synapse.
    changed = [dict() for _ in range(spec.mc)]
    def weights_at(mc, addresses):
        memory = image.memory[mc]
        return np.asarray([changed[mc].get(int(a), memory[int(a)//4].view(F)) for a in addresses], F)
    rng_state = np.asarray([spm[43:45] for spm in image.spms]).copy()
    thresholds = poisson_thresholds()
    state = np.load(Path(directory)/"neurons.npy").copy()
    c = constants()
    report = []
    observed_rows = list(csv.DictReader((run/"morrison-activity.csv").open()))
    for step in range(1, manifest["warmup"]+manifest["timed"]+1):
        f = state.view(F)
        # Independently express the documented combined-alpha recurrence. Each
        # elementary operation rounds to binary32, as in the payload.
        expected = state.copy()
        ef = expected.view(F)
        refractory = state[:, 7] & 255
        free = refractory == 0
        voltage = F(F(c["p33"]*f[:, 0]) + F(F(c["p31"]*f[:, 2])+F(c["p32"]*f[:, 1])))
        ef[free, 0] = voltage[free]
        expected[:, 7] = np.maximum(refractory.astype(np.int32)-1, 0).astype(np.uint32)
        ef[:, 1] = F(F(c["p11"]*f[:, 1])+F(c["p21"]*f[:, 2]))
        ef[:, 2] = F(F(c["p11"]*f[:, 2])+F(f[:, 5]+f[:, 6]))
        fired = (expected[:, 7] == 0) & (ef[:, 0] >= F(20.))
        expected[fired, 7] = 5 | (1 << 8)
        ef[fired, 0] = 0.
        ef[:, 3] = F(f[:, 3]*c["decay_pre"])
        ef[:, 4] = F(f[:, 4]*c["decay_post"])
        ef[:, 5:7] = 0.
        forward = reverse = 0
        for source in np.flatnonzero(fired):
            peers = source % spec.pu // image.group * image.group + np.arange(image.group)
            slot = source // spec.pu * image.group + source % image.group
            for owner in peers:
                mc = int(owner // spec.pu_per_mc)
                row = image.rows[owner][slot]
                post, _, addresses = image.forward(owner, row)
                weights = weights_at(mc, addresses)
                np.add.at(ef[:, 5], post, weights)
                forward += len(post)
                quiet = ~fired[post]
                new = np.maximum(F(0.), F(weights[quiet]-F(weights[quiet]*ef[post[quiet], 4])))
                changed[mc].update((int(a), w) for a, w in zip(addresses[quiet], new))
                for target_mc, (base, count) in enumerate(image.directory_entries(mc, int(row[5]))):
                    entries = image.read(target_mc, int(base), int(count)*2).reshape(-1, 2)
                    packed = entries[:, 0]
                    col, field = (5, 20) if source < spec.ne else (6, 24)
                    target = ((packed>>16)-NODE_BASE-field)//NODE_BYTES*spec.pu+(packed&65535)-1
                    np.add.at(ef[:, col], target, entries[:, 1].view(F))
                    forward += int(count)
        for post in np.flatnonzero(fired[:spec.ne]):
            owner = post % spec.pu
            slot = post // spec.pu * image.group + post % image.group
            row = image.rows[owner][slot]
            for mc, (base, count) in enumerate(image.directory_entries(owner//spec.pu_per_mc, int(row[6]))):
                entries = image.read(mc, int(base), int(count)*2).reshape(-1, 2)
                packed, addresses = entries[:, 0], entries[:, 1]
                pre = (((packed>>16)&0xfff0)-NODE_BASE)//NODE_BYTES*spec.pu+(packed&65535)-1
                weights = weights_at(mc, addresses)
                active = fired[pre]
                weights[active] = np.maximum(F(0.), F(weights[active]-F(weights[active]*ef[post, 4])))
                weights = F(weights + F(ef[pre, 3]*np.power(weights.astype(np.float64), 2./5.).astype(F)))
                changed[mc].update((int(a), w) for a, w in zip(addresses, weights))
                reverse += int(count)
        expected_forward, expected_reverse = forward, reverse
        exc_fire = np.flatnonzero(fired[:spec.ne])
        ef[exc_fire, 3] = F(ef[exc_fire, 3]+c["pot"])
        ef[exc_fire, 4] = F(ef[exc_fire, 4]+c["dep"])
        blob = (run/f"step-{step:04d}.bin").read_bytes()
        magic, pus = struct.unpack_from("<2I", blob)
        if (magic, pus) != (0x42534431, spec.pu):
            raise ValueError("bad snapshot header")
        cursor, forward, reverse = 8, 0, 0
        actual = np.empty_like(state)
        next_rng = np.empty_like(rng_state)
        for pu in range(spec.pu):
            ident, size = struct.unpack_from("<2I", blob, cursor)
            cursor += 8
            if (ident, size) != (pu+1, 16384):
                raise ValueError("bad snapshot PU/size")
            spm = np.frombuffer(blob, dtype="<u4", count=4096, offset=cursor)
            cursor += size
            n = int(spm[4])
            next_rng[pu] = spm[43:45]
            actual[pu::spec.pu] = spm[64:64+n*8].reshape(-1, 8)
            if spm[5] != step or spm[18] != spm[19] or spm[39] or spm[42]:
                raise ValueError("incomplete step/memory/MSHR drain")
            if spm[0x3f00//4] != 0x19ca7ead or spm[0x3f80//4] != 0x19ca7ead:
                raise ValueError("stack guard overwritten")
            pool = spm[0x2900//4:0x2d00//4].reshape(16, 16)
            if np.any(pool[:, 11]):
                raise ValueError("live MSHR at completion")
            expected_local = np.flatnonzero(fired[pu::spec.pu])
            count = int(spm[53])
            pointers = spm[FIRED_LIST//4:FIRED_LIST//4+count]
            if not np.array_equal(np.sort(pointers), 0x20000000+NODE_BASE+expected_local*NODE_BYTES):
                raise ValueError("fired list mismatch or overflow")
            config = spm[CONFIG//4:CONFIG//4+16]
            jobs = np.frombuffer(blob, dtype="<u2", count=int(config[10]), offset=cursor-size+JOBS)
            peers = pu//image.group*image.group+np.arange(image.group)
            ids = (np.arange(220)[:, None]*spec.pu+peers).ravel()
            valid = ids < spec.n
            expected_jobs = np.flatnonzero(valid & fired[np.minimum(ids, spec.n-1)])+1
            if not np.array_equal(np.sort(jobs), expected_jobs) or config[11] != image.group or config[12] != len(jobs):
                raise ValueError("proxy notification/compact job mismatch")
            forward += int(spm[16])
            reverse += int(spm[17])
        if cursor != len(blob):
            raise ValueError("trailing snapshot data")
        if not np.array_equal(actual[:, 7], expected[:, 7]):
            raise ValueError(f"step {step}: refractory/firing byte mismatch")
        af = actual.view(F)
        # Observed input accumulation order is asynchronous. Compare those sums
        # with a tolerance; use each completed state for the next local-equation
        # check, so transport rounding cannot hide a wrong alpha recurrence.
        poisson_counts = np.zeros(spec.n, np.int32)
        if manifest["external_drive"]:
            current = F(c["epsc"] * c["external_weight"])
            poisson_counts = np.rint((af[:, 2]-ef[:, 2])/current).astype(np.int32)
            if np.any(poisson_counts < 0) or np.any(poisson_counts >= len(thresholds)):
                raise ValueError("external input not an in-table Poisson count")
            ef[:, 2] = F(ef[:, 2] + F(poisson_counts.astype(F)*current))
            # Scheduling may assign a different neuron to each worker. Verify
            # the exact draws as a per-PU multiset and each worker's final RNG.
            for pu in range(spec.pu):
                draws = []
                for worker in range(2):
                    value, target = int(rng_state[pu, worker]), int(next_rng[pu, worker])
                    for _ in range(len(state[pu::spec.pu])+1):
                        if value == target:
                            break
                        value ^= (value << 13) & 0xffffffff
                        value ^= value >> 17
                        value ^= (value << 5) & 0xffffffff
                        draws.append(int(np.searchsorted(thresholds, value, side="left")))
                    else:
                        raise ValueError("worker RNG state does not match bounded neuron work")
                if not np.array_equal(np.sort(draws), np.sort(poisson_counts[pu::spec.pu])):
                    raise ValueError("Poisson counts differ from native worker RNG draws")
        rng_state = next_rng
        for col in range(7):
            tolerance = 2e-5 if col in (5, 6) else 2e-6
            if not np.allclose(af[:, col], ef[:, col], rtol=tolerance, atol=2e-5):
                bad = int(np.argmax(np.abs(af[:, col]-ef[:, col])))
                raise ValueError(f"step {step} field {col} neuron {bad}: actual={af[bad,col]} expected={ef[bad,col]}")
        if (forward, reverse) != (expected_forward, expected_reverse):
            raise ValueError("per-step receipt count/reset mismatch")
        row = observed_rows[step-1]
        observed = tuple(int(row[k]) for k in ("step", "e_spikes", "i_spikes", "forward_deliveries", "reverse_visits"))
        expected_counts = (step, int(fired[:spec.ne].sum()), int(fired[spec.ne:].sum()), forward, reverse)
        if observed != expected_counts:
            raise ValueError("passive activity observer mismatch")
        report.append(dict(step=step, e_spikes=expected_counts[1], i_spikes=expected_counts[2],
                           forward_deliveries=forward, reverse_visits=reverse,
                           external_events=int(poisson_counts.sum()), external_ge10=int((poisson_counts>=10).sum())))
        state = actual
    if len(observed_rows) != len(report):
        raise ValueError("observer timestep coverage mismatch")
    final = [np.memmap(run/f"dram.{mc}", mode="r", dtype="<u4") for mc in range(spec.mc)]
    peak_error = 0.
    for mc in range(spec.mc):
        addresses = np.asarray(list(changed[mc]), np.int64)
        expected_weights = np.asarray(list(changed[mc].values()), F)
        actual_weights = final[mc][addresses//4].view(F)
        if not np.allclose(actual_weights, expected_weights, rtol=4e-7, atol=2e-5):
            bad = int(np.argmax(np.abs(actual_weights-expected_weights)))
            raise ValueError(f"MC {mc} weight at {addresses[bad]}: actual={actual_weights[bad]} expected={expected_weights[bad]}")
        peak_error = max(peak_error, float(np.max(np.abs(actual_weights-expected_weights), initial=0.)))
        allowed = set(changed[mc])
        if mc == 0:
            for i in range(32):
                address = 0x8000 + i*4
                expected_word = 0x19260000 + (i&~7)*32 + (i&7)
                if final[mc][address//4] != expected_word:
                    raise ValueError("boot scalar memory self-check value mismatch")
                allowed.add(address)
        # Compare every other backing word, catching unintended writes even in
        # inactive rows. No full-network array is materialized in host RAM.
        for start in range(0, len(final[mc]), 1<<20):
            before, after = image.memory[mc][start:start+(1<<20)], final[mc][start:start+(1<<20)]
            difference = (np.flatnonzero(before != after) + start)*4
            if any(int(address) not in allowed for address in difference):
                raise ValueError(f"unexpected backing-memory write in MC {mc}")
    return dict(functional=True, steps=report,
                touched_plastic_weights_checked=sum(map(len, changed)),
                weight_max_absolute_error=peak_error,
                other_initialized_image_words_unchanged=True,
                all_neuron_fields_and_job_lists_checked=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", type=Path)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    print(json.dumps(check(args.images, args.run), indent=2))


if __name__ == "__main__":
    main()
