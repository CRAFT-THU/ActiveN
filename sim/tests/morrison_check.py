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
    late_receipts = []
    observed_rows = list(csv.DictReader((run/"morrison-activity.csv").open()))
    uncounted = manifest.get('forward_receiver_count') == 'none-experimental'
    passive_receipts = {}
    if uncounted:
        for row in csv.DictReader((run/'uncounted-receipts.csv').open()):
            key = (int(row['step']), int(row['pu'])-1)
            if key in passive_receipts:
                raise ValueError('duplicate passive receipt row')
            passive_receipts[key] = int(row['completed'])
        expected_keys = {(step, pu) for step in range(1, manifest['warmup']+manifest['timed']+1)
                         for pu in range(spec.pu)}
        if passive_receipts.keys() != expected_keys:
            raise ValueError('passive receipt coverage mismatch')
    copy_profiles = {}
    profile_path = run/'pu-profile.csv'
    if image.bulk and profile_path.exists():
        for row in csv.DictReader(profile_path.open()):
            if int(row['phase']) == 2:
                key = (int(row['step']),int(row['pu'])-1)
                if key in copy_profiles: raise ValueError('duplicate copy profile')
                copy_profiles[key] = (int(row['bulk_commands']),int(row['bulk_lines']))
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
        expected_pu_receipts = np.zeros(spec.pu, np.int64)
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
                np.add.at(expected_pu_receipts, post % spec.pu, 1)
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
                    np.add.at(expected_pu_receipts, target % spec.pu, 1)
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
        if copy_profiles:
            # Reconstruct destination ownership and actual read volume from the
            # fired source rows, independently of the emitter's local counters.
            commands = np.zeros(spec.pu,np.uint64)
            copy_lines = np.zeros(spec.pu,np.uint64)
            width = manifest['dispatch_width']
            balanced = manifest.get('dispatch_policy','legacy') == 'balanced'
            override = spec.pu//2 if manifest.get('fixture',{}).get('cross_mc_jobs') else 0
            for source in np.flatnonzero(fired):
                home = int(source % spec.pu)
                sizes = [(max(32,spec.mc*8),32,min(width,4))]
                if source < spec.ne:
                    n = int(image.rows[home][source//spec.pu,4])
                    sizes.append((((n+8)//8)*64,1024,width))
                for size,unit,workers in sizes:
                    units = (size+unit-1)//unit
                    cuts = ([units//workers+(rank<units%workers) for rank in range(workers)] if balanced
                            else [min((units+workers-1)//workers,max(0,units-rank*((units+workers-1)//workers))) for rank in range(workers)])
                    offset = 0
                    for rank,count in enumerate(cuts):
                        if not count: continue
                        if manifest.get('dispatch_layout') == 'distant-locality-source-mc-v1':
                            stride = spec.pu//workers
                            rotation = home//stride if unit == 1024 else 0
                            owner = (home%stride+((rotation+rank)%workers)*stride)^override
                        else:
                            owner = ((home//workers*workers)^override)+(home%workers+rank)%workers
                        length = min(size-offset,count*unit)
                        bank_bytes = manifest.get("forward_copy_bytes",manifest.get("forward_bank_bytes",1024))
                        commands[owner] += (length+bank_bytes-1)//bank_bytes
                        copy_lines[owner] += length//32
                        offset += length
                    if offset != size: raise ValueError('incomplete expected copy partition')
            for pu in range(spec.pu):
                if copy_profiles.get((step,pu)) != (int(commands[pu]),int(copy_lines[pu])):
                    raise ValueError(f'step{step} PU{pu+1}: bulk copy placement/count mismatch')
        exc_fire = np.flatnonzero(fired[:spec.ne])
        ef[exc_fire, 3] = F(ef[exc_fire, 3]+c["pot"])
        ef[exc_fire, 4] = F(ef[exc_fire, 4]+c["dep"])
        blob = (run/f"step-{step:04d}.bin").read_bytes()
        magic, pus = struct.unpack_from("<2I", blob)
        if (magic, pus) != (0x42534431, spec.pu):
            raise ValueError("bad snapshot header")
        cursor, forward, reverse = 8, 0, 0
        jobs_produced = jobs_expanded = 0
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
            if image.abi >= 3:
                pool = np.concatenate((spm[0x2500//4:0x2d00//4],spm[0x3700//4:0x3f00//4])).reshape(64,16)
                ready = spm[0x2140//4:0x21c0//4]
                if ready[0] != ready[8] or any(ready[i//4] for i in (64,100,120)):
                    raise ValueError('live ready queue/credit/job/directory at completion')
                if manifest.get('reverse_schedule') == 'ready-queue-chains-v1' and spm[0x21c0//4] != 1:
                    raise ValueError('updater chains were not closed')
            else:
                pool = spm[0x2900//4:0x2d00//4].reshape(16, 16)
            if np.any(pool[:, 11]):
                raise ValueError("live MSHR at completion")
            expected_local = np.flatnonzero(fired[pu::spec.pu])
            count = int(spm[53])
            pointers = spm[FIRED_LIST//4:FIRED_LIST//4+count]
            if not np.array_equal(np.sort(pointers), 0x20000000+NODE_BASE+expected_local*NODE_BYTES):
                raise ValueError("fired list mismatch or overflow")
            if image.bulk:
                control = spm[0x2000//4:0x2100//4]
                for offset in (64,68,72,164,192,224):
                    if control[offset//4]: raise ValueError('live bulk cursor/copy/bank at completion')
                if spm[56//4] != spec.mc or control[180//4] != 15:
                    raise ValueError('incomplete route-marker/MC-idle barrier')
                jobs_produced += int(control[84//4])
                jobs_expanded += int(control[80//4])
                if manifest.get('forward_completion','poll')=='tree':
                    tree=spm[(0x2000+256)//4:(0x2000+300)//4]
                    if int(tree[0])!=2 or int(tree[1])!=int(control[84//4]) or (not uncounted and int(tree[3])):
                        raise ValueError('incomplete home/receiver completion tree')
                    if pu==0 and not uncounted:
                        if int(tree[7])+int(tree[8])!=expected_forward or tuple(tree[9:11])!=(1,1):
                            raise ValueError('root receiver reduction does not match deliveries')
                        late_receipts.append(int(tree[8]))
                    if pu == 0 and int(spm[26]) != expected_forward:
                        raise ValueError('sender reduction does not match expected deliveries')
                width = manifest['dispatch_width']
                balanced = manifest.get('dispatch_policy', 'legacy') == 'balanced'
                expected_jobs = 0
                lines = max(32,spec.mc*8)//32
                static_width = min(width, 4)
                span = (lines+static_width-1)//static_width
                for local in expected_local:
                    src = pu+int(local)*spec.pu
                    expected_jobs += min(lines,static_width) if balanced else (lines+span-1)//span
                    if src < spec.ne:
                        n = int(image.rows[pu][local,4])
                        chunks = (((n+8)//8)*64+1023)//1024
                        block = (chunks+width-1)//width
                        expected_jobs += min(chunks,width) if balanced else (chunks+block-1)//block
                if int(control[84//4]) != expected_jobs:
                    raise ValueError('home range production count mismatch')
            else:
                config = spm[CONFIG//4:CONFIG//4+16]
                jobs = np.frombuffer(blob, dtype="<u2", count=int(config[10]), offset=cursor-size+JOBS)
                peers = pu//image.group*image.group+np.arange(image.group)
                ids = (np.arange(220)[:, None]*spec.pu+peers).ravel()
                valid = ids < spec.n
                expected_jobs = np.flatnonzero(valid & fired[np.minimum(ids, spec.n-1)])+1
                if not np.array_equal(np.sort(jobs), expected_jobs) or config[11] != image.group or config[12] != len(jobs):
                    raise ValueError("proxy notification/compact job mismatch")
            if uncounted:
                if spm[16] != image.spms[pu][16]:
                    raise ValueError('unused receiver count was modified')
                delivered = passive_receipts[step,pu]
                if delivered != expected_pu_receipts[pu]:
                    raise ValueError(f'step{step} PU{pu+1}: passive delivery total mismatch')
                forward += delivered
            else:
                forward += int(spm[16])
            reverse += int(spm[17])
        if image.bulk and jobs_produced != jobs_expanded:
            raise ValueError("global range accounting mismatch")
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
                all_neuron_fields_checked=True,
                proxy_job_lists_checked=not image.bulk,
                range_accounting_checked=image.bulk,
                forward_tree_checked=manifest.get('forward_completion','poll')=='tree',
                passive_per_pu_deliveries_checked=uncounted,
                late_forward_deliveries=late_receipts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", type=Path)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    print(json.dumps(check(args.images, args.run), indent=2))


if __name__ == "__main__":
    main()
