#!/usr/bin/env python3
"""Validate observed Brunel neuron states and logical cached/backing weights."""

import argparse
import gzip
import json
import re
import struct
from pathlib import Path
import numpy as np
from brunel import NE, NI, PU, MC, F, offsets, ranges, Reference


def pending_power(w):
    """Reconstruct the FP32 runtime helper for historical cache experiments.

    The independent neuron/weight reference continues to use numpy.power.
    Unit tests check this reconstruction bit for bit against the C helper.
    """
    w = F(w)
    if w == 0:
        return F(0)
    scale = 0
    if w.view(np.uint32) < 0x02000000:
        w = F(w * F(2.0**100))
        scale = 40 << 23
    r = np.uint32(0x4C2BAC89 - int(w.view(np.uint32)) // 5).view(F)
    m = F(w * F(0.2))
    r2 = F(r * r)
    r4 = F(r2 * r2)
    r = F(F(r * F(1.20148)) - F(F(m * r2) * r4))
    r2 = F(r * r)
    q = F(F(w * r2) * r)
    d = F(F(1) - F(q * r2))
    correction = F(F(d * F(0.48)) + F(0.60002))
    result = F(q + F(F(q * d) * correction))
    return np.uint32(int(result.view(np.uint32)) - scale).view(F)


def dump(path):
    path = Path(path)
    if not path.exists() and path.suffix != ".gz":
        path = path.with_suffix(path.suffix + ".gz")
    b = gzip.decompress(path.read_bytes()) if path.suffix == ".gz" else path.read_bytes()
    magic, count = struct.unpack_from("<II", b)
    if (magic, count) != (0x42534431, PU):
        raise ValueError("not a 512-PU Brunel dump")
    pos = 8
    spms = []
    neurons = np.zeros((NE + NI, 20), np.uint32)
    for pu in range(PU):
        identity, size = struct.unpack_from("<II", b, pos)
        pos += 8
        if identity != pu + 1 or size not in (10240, 16384):
            raise ValueError("invalid SPM record")
        s = np.frombuffer(b[pos : pos + size], "<u4")
        pos += size
        spms.append(s)
        n = s[64 : 64 + int(s[4]) * 20].reshape(-1, 20)
        neurons[n[:, 11]] = n
    if pos != len(b):
        raise ValueError("trailing SPM data")
    return spms, neurons


def compare_nodes(actual, expected, single_fire=False, combined_currents=False):
    out = {}
    for i, name in enumerate(
        (
            "V",
            "Iex",
            "dIex",
            "Iin",
            "dIin",
            "refractory",
            "pre_trace",
            "post_trace",
            "rng",
            "fire",
            "pre_fire",
            "gid",
        )
    ):
        if combined_currents and 1 <= i <= 4:
            name = ("I_total", "dI_total", "reserved_Iin", "reserved_dIin")[i - 1]
        if i == 10 and single_fire:
            name = "reserved_fire"
        if i in (5, 8, 9, 10, 11):
            out[name] = {
                "different": int(np.count_nonzero(actual[:, i] != expected[:, i]))
            }
        else:
            a = actual[:, i].view(np.float32)
            b = expected[:, i].view(np.float32)
            if not np.all(np.isfinite(a)):
                raise ValueError("nonfinite " + name)
            err = np.abs(a.astype(float) - b.astype(float))
            out[name] = {
                "max_absolute": float(err.max()),
                "rms": float(np.sqrt(np.mean(err**2))),
                "max_relative_scaled": float(np.max(err / np.maximum(1, np.abs(b)))),
            }
    return out


def compare_combined_derivative(previous, observed, noise, c):
    """Check the combined recurrence against FP64 with a rounding bound.

    Excitation and inhibition can nearly cancel. A tolerance normalized only
    by their final sum incorrectly rejects valid FP32 regrouping. Each term
    passes through at most three FP32 rounding operations, so gamma(3) times
    the sum of absolute terms bounds the error, including cancellation. The
    small absolute term covers gradual underflow; FP64 evaluation error is
    included separately. This does not reproduce the assembly's FP32 order.
    """
    f = previous.view(np.float32)
    external_unit = float(F(c["epsc"] * c["external_weight"]))
    terms = np.array((float(c["p11"]) * f[:, 2].astype(float),
                      f[:, 17].astype(float), f[:, 18].astype(float),
                      external_unit * noise.astype(float)))
    exact = terms.sum(axis=0)
    u = 2.0**-24
    gamma = 3 * u / (1 - 3 * u) + 4 * np.finfo(np.float64).eps
    bound = gamma * np.abs(terms).sum(axis=0) + 5 * float(np.nextafter(F(0), F(1)))
    error = np.abs(observed.astype(float) - exact)
    return dict(max_absolute=float(error.max()),
                max_bound_fraction=float(np.max(error / bound)),
                violations=int(np.count_nonzero(~np.isfinite(error) | (error > bound))))


def logical_weights(spms, nodes, initial, path, rule):
    drams = [np.fromfile(path / f"dram.{mc}", dtype="<u4") for mc in range(MC)]
    if "weight_owner" in initial:
        owners, addresses = initial["weight_owner"], initial["weight_address"]
        w = np.empty(len(owners), np.float32)
        for mc in range(MC):
            mask = owners // 32 == mc
            w[mask] = drams[mc][addresses[mask] // 4].view(np.float32)
        if any(s[18] != s[19] or s[39] or s[42] for s in spms):
            raise ValueError("unfinished proxy transport")
        return w, {"persistent_cache": False, "proxy_groups": True}
    off = offsets(initial["pre"], NE)
    w = np.empty(len(initial["pre"]), np.float32)
    scalar = int(spms[0][0]) == 0x42524E32
    slots, stride = (800, 2) if scalar else (160, 10)
    cache = dict(
        resident_entries=0,
        pending_entries=0,
        resident_synapses=0,
        pending_synapses=0,
        dirty_synapses=0,
        entry_bytes=stride * 4,
        entries_per_pu=slots,
    )
    for pre in range(NE):
        lo, hi = off[pre : pre + 2]
        base = int(nodes[pre, 12])
        mc = (pre % PU) // 32
        if int(spms[0][0]) == 0x42524E34:
            lane = np.arange(hi - lo)
            index = base // 4 + (lane // 128) * 256 + lane % 128
            w[lo:hi] = drams[mc][index].view(np.float32)
        else:
            w[lo:hi] = drams[mc][base // 4 : base // 4 + hi - lo].view(np.float32)
    if int(spms[0][0]) in (0x42524E33, 0x42524E34):
        if any(s[18] != s[19] or s[39] or s[42] for s in spms):
            raise ValueError("unfinished buffer transport")
        return w, {"persistent_cache": False}
    for pu, s in enumerate(spms):
        if s[18] != s[19] or np.any(s[38:46]):
            raise ValueError("snapshot contains unfinished memory requests")
        owners = [pre for pre in range(pu, NE, PU)]
        seen = set()
        for slot in range(slots):
            e = s[576 + slot * stride : 576 + (slot + 1) * stride]
            tag = int(e[0])
            if not tag:
                continue
            if scalar:
                state, tag = tag & 3, tag & ~3
                h = ((tag >> 2) * 2654435761) & 0xFFFFFFFF
                home = ((h >> 16) * slots) >> 16
                if (slot - home) % slots >= 4:
                    raise ValueError("cache tag outside bounded probe range")
                dirty_mask = int(state == 1)
                resident, mask, count = state < 2, 1, 1
                data = e[1:2].view(np.float32)
                potentials = 1 if state == 3 else 0
            else:
                ctrl = int(e[1])
                dirty_mask = (ctrl >> 16) & 255
                resident, mask, count = bool(ctrl & (1 << 24)), ctrl & 255, 8
                data = e[2:].view(np.float32)
                potentials = (ctrl >> 8) & 255
                if tag & 31 or (not resident and not mask):
                    raise ValueError("invalid legacy cache entry")
            if tag in seen:
                raise ValueError("duplicate cache tag")
            seen.add(tag)
            for pre in owners:
                base, owner_count = map(int, nodes[pre, 12:14])
                if base <= tag < base + owner_count * 4:
                    break
            else:
                raise ValueError("cache tag outside owner weights")
            lane0 = (tag - base) // 4
            count = min(count, owner_count - lane0)
            lo = off[pre] + lane0
            if resident:
                cache["resident_entries"] += 1
                cache["resident_synapses"] += count
                cache["dirty_synapses"] += (dirty_mask & ((1 << count) - 1)).bit_count()
                w[lo : lo + count] = data[:count]
            else:
                cache["pending_entries"] += 1
                cache["pending_synapses"] += (mask & ((1 << count) - 1)).bit_count()
                for lane in range(count):
                    if mask & (1 << lane):
                        v, c = w[lo + lane], data[lane]
                        pot = bool(potentials & (1 << lane))
                        if rule == "morrison":
                            c = F(c * (pending_power(v) if pot else v))
                        v = max(F(0), F(v + c if pot else v - c))
                        if rule == "additive":
                            v = min(F(2 * 45.609600316540956), v)
                        w[lo + lane] = v
    cache["resident_synapse_fraction"] = cache["resident_synapses"] / len(w)
    cache["buffered_synapse_fraction"] = (
        cache["resident_synapses"] + cache["pending_synapses"]
    ) / len(w)
    cache["mean_pending_synapses_per_entry"] = (
        cache["pending_synapses"] / cache["pending_entries"]
        if cache["pending_entries"]
        else None
    )
    return w, cache


def validate_buffer(args, manifest, initial, log, steps):
    """New runtime has no deferred learning: final DRAM is authoritative."""
    reference = Reference(
        tuple(initial[k] for k in ("pre", "post", "static_pre", "static_post")),
        manifest["rule"],
        state=(initial["neurons"], initial["weights"]),
    )
    comparisons = {}
    previous = np.zeros(3, np.int64)
    shared_rng = manifest.get("rng", "").startswith("per-SMT")
    single_fire = manifest.get("firing_flag") == "N_FIRE"
    combined = manifest.get("current_state") == "combined"
    scaled_traces = manifest.get("trace_state") == "learning-scaled"
    excitatory_traces = manifest.get("trace_population") == "excitatory-only"
    if excitatory_traces and manifest.get("inhibitory_trace_state") != "zero":
        raise ValueError("unsupported inhibitory trace contract")
    if combined:
        # The image deliberately retains independently presimulated E/I state.
        # Reproduce the one-time boot conversion, preserving the original model
        # for subsequent independent split-current updates and learning.
        f = reference.n.view(np.float32)
        f[:, 1] = F(f[:, 1] + f[:, 3])
        f[:, 2] = F(f[:, 2] + f[:, 4])
        f[:, 3:5] = F(0)
    noise_counts = np.zeros(10, np.int64)
    maximum_noise_residual = 0.0
    derivative_roundoff = dict(max_absolute=0.0, max_bound_fraction=0.0, violations=0)
    delivery_error = dict(max_absolute=0.0, max_relative_scaled=0.0)
    previous_stream = np.zeros(4, np.int64)
    reverse_expected = 0
    reverse_degree = np.bincount(initial["post"], minlength=NE)
    for s in steps:
        s.setdefault("pool_full", s["allocation_waits"])
        spms, actual = dump(args.dumps / f"step-{s['step']:04}.bin")
        if manifest.get("proxy_groups", "none") != "none":
            from brunel_proxy import CONFIG, JOBS, ROWS, capacity, magic
            group_size = manifest["proxy_size"]
            replicated = manifest["proxy_updates"] == "replicated"
            for spm in spms:
                cfg = spm[CONFIG // 4:CONFIG // 4 + 16]
                rows = spm[ROWS // 4:ROWS // 4 + capacity(group_size) * 8].reshape(-1, 8)[:int(cfg[8])]
                gids = rows[:, 2]
                if not np.array_equal(rows[:, 1], actual[gids, 9]):
                    raise ValueError("proxy job flags differ from authoritative home")
                if replicated and not np.array_equal(rows[:, 0], actual[gids, 6]):
                    raise ValueError("proxy trace differs from authoritative home")
                if not replicated and np.any(rows[:, 0]):
                    raise ValueError("non-replicating variant unexpectedly replicated traces")
                if cfg[11] != group_size or cfg[10] != cfg[12] or cfg[15]:
                    raise ValueError("incomplete group notification exchange")
                if cfg[14] != magic(group_size, manifest["proxy_updates"]):
                    raise ValueError("wrong proxy variant magic")
                jobs = spm[JOBS // 4:JOBS // 4 + int(cfg[10])]
                expected_jobs = 0x20000000 + ROWS + np.flatnonzero(rows[:, 1]) * 32
                if not np.array_equal(np.sort(jobs), expected_jobs):
                    raise ValueError("proxy fired-job list mismatch")
            # Each reverse CSR entry must complete exactly once, including
            # zero-effect events. Relay events count only at the final proxy.
            reverse_expected += int(reverse_degree[actual[:NE, 9] != 0].sum())
            if sum(int(spm[17]) for spm in spms) != reverse_expected:
                raise ValueError("reverse barrier counted an event early, twice, or not at all")
        if excitatory_traces and np.any(actual[NE:, 6:8]):
            raise ValueError("unused inhibitory traces are not zero")
        for spm in spms:
            if spm[0] not in (0x42524E33, 0x42524E34) or spm[18] != spm[19] or spm[39] or spm[42]:
                raise ValueError("unfinished buffer/MSHR transport")
            if np.any(spm[0x2E00 // 4 : 0x2F00 // 4]):
                raise ValueError("unflushed dirty weights")
            if any(spm[base // 4] != 0x19CA7EAD for base in manifest["spm"].get("stack_bases", [0x3800, 0x3C00])):
                raise ValueError("stack guard damaged")
            if np.any(spm[0x2900 // 4 + 11 : 0x2D00 // 4 : 16]):
                raise ValueError("live MSHR at timestep boundary")
            if manifest.get("fired_list"):
                count = int(spm[manifest["spm"]["fired_count_offset"] // 4])
                nn = int(spm[4])
                if count > nn or count > manifest["spm"]["fired_list_capacity"]:
                    raise ValueError("fired-neuron list overflow")
                start = manifest["spm"]["fired_list_offset"] // 4
                rows = spm[64 : 64 + nn * 20].reshape(nn, 20)
                if excitatory_traces:
                    exc_count = int(np.count_nonzero(rows[:, 11] < NE))
                    if (np.any(rows[:exc_count, 11] >= NE)
                            or np.any(rows[exc_count:, 11] < NE)
                            or int(spm[11]) != 0x20000100 + 80 * exc_count):
                        raise ValueError("invalid excitatory-neuron extent")
                expected = 0x20000100 + 80 * np.flatnonzero(rows[:, 9])
                if not np.array_equal(np.sort(spm[start : start + count]), expected):
                    raise ValueError("fired-neuron list disagrees with firing flags")
        if shared_rng:
            # Infer the independent Poisson integer from the stored derivative
            # (dIex or dI_total). Re-run the deterministic equations, including
            # delivery, with that integer.
            # Recurrent delivery is independently checked below before resync;
            # this conditional integration check alone cannot establish it.
            c = reference.c
            # Integration now consumes the one-step input fields populated by
            # the preceding forward phase.  Recurrent deliveries are already
            # present in fields 17/18, so the only unknown increment is the
            # external Poisson sample.
            f = reference.n.view(np.float32)
            recurrent = F(f[:, 17] + f[:, 18]) if combined else f[:, 17]
            base = F(F(c["p11"] * f[:, 2]) + recurrent)
            delta = (actual[:, 2].view(np.float32).astype(float) - base) / float(F(c["epsc"] * c["external_weight"]))
            noise = np.rint(delta).astype(np.int32)
            residual = float(np.max(np.abs(delta - noise)))
            maximum_noise_residual = max(maximum_noise_residual, residual)
            if np.any(noise < 0) or np.any(noise > 9) or residual > 2e-4:
                raise ValueError(f"invalid external Poisson increment: residual={residual}")
            if combined:
                check = compare_combined_derivative(reference.n, actual[:, 2].view(F), noise, c)
                if check["violations"]:
                    raise ValueError(f"combined derivative exceeds FP32 roundoff at step {s['step']}: {check}")
                for key in derivative_roundoff:
                    derivative_roundoff[key] = max(derivative_roundoff[key], check[key])
            noise_counts += np.bincount(noise, minlength=10)
            reference.tick(external=noise)
        else:
            reference.tick()
        expected_nodes = reference.n
        if single_fire or combined or scaled_traces or excitatory_traces:
            expected_nodes = reference.n.copy()
        if single_fire:
            # Offset 40 is no longer updated. Validate that it retains its
            # input-image value, independently of the authoritative N_FIRE.
            expected_nodes[:, 10] = initial["neurons"][:, 10]
        if combined:
            # Compare the independently advanced channels through their sum.
            # Do not replace the reference recurrence with the assembly's
            # grouped arithmetic: this check must expose rounding/error drift.
            f = expected_nodes.view(np.float32)
            f[:, 1] = F(f[:, 1] + f[:, 3])
            f[:, 2] = F(f[:, 2] + f[:, 4])
            f[:, 3:5] = F(0)
            if np.any(actual[:, 3:5]):
                raise ValueError("combined-current reserved fields changed")
        if scaled_traces:
            # Keep the oracle in canonical trace units throughout the run.
            # Only the comparison copy is converted to runtime units; do not
            # reproduce the optimized recurrence or normalize observed traces.
            f = expected_nodes.view(np.float32)
            f[:, 6] = F(reference.c["pot"] * f[:, 6])
            f[:, 7] = F(reference.c["dep"] * f[:, 7])
        if excitatory_traces:
            # The canonical oracle still evolves all neurons' traces. I traces
            # cannot affect this graph's E->E learning and are runtime zeros.
            expected_nodes[NE:, 6:8] = 0
        comparisons[str(s["step"])] = compare_nodes(actual, expected_nodes, single_fire, combined)
        # Compare this step's delivered currents before importing observed state.
        # The oracle uses independently advanced weights and a different FP32
        # reduction order. A missing spike is much larger than this tolerance.
        delivered = actual[:, 17:19].view(F).astype(float)
        expected_delivery = expected_nodes[:, 17:19].view(F).astype(float)
        delta = np.abs(delivered - expected_delivery)
        relative = delta / np.maximum(1, np.abs(expected_delivery))
        delivery_error["max_absolute"] = max(delivery_error["max_absolute"], float(delta.max()))
        delivery_error["max_relative_scaled"] = max(delivery_error["max_relative_scaled"], float(relative.max()))
        if not np.all(np.isfinite(delivered)) or np.any(relative > 2e-5):
            raise ValueError(f"recurrent delivery mismatch at step {s['step']}")
        if shared_rng:
            # Carry the observed stochastic trajectory into the next step;
            # weights and scaled-trace learning's canonical traces remain
            # independently computed throughout the run.
            canonical_traces = reference.n[:, 6:8].copy() if scaled_traces else None
            reference.n = actual.copy()
            if scaled_traces:
                reference.n[:, 6:8] = canonical_traces
        s["reference_E"], s["reference_I"] = manifest["reference_spikes"][s["step"] - 1]
        replay = 0x2D80 // 4
        current = np.array(
            [sum(int(spm[replay + k]) for spm in spms) for k in (4, 5, 6)]
        )
        for key, value in zip(
            ("spill_records", "manual_reverse", "scatter_reverse"), current - previous
        ):
            s[key] = int(value)
        previous = current
        s["maximum_buffer_bytes_per_pu"] = max(int(spm[38]) for spm in spms)
        s["mshr_peak_per_pu"] = max(int(spm[45]) for spm in spms)
        s["record_peak_per_pu"] = max(int(spm[replay + 1]) for spm in spms)
        if "stream_offset" in manifest["spm"]:
            stream = manifest["spm"]["stream_offset"] // 4
            current_stream = np.array([sum(int(spm[stream + k]) for spm in spms) for k in range(1, 5)])
            for key, value in zip(("bulk_copies", "overlap_checks", "overlap_ready", "ac_wait_polls"), current_stream - previous_stream):
                s[key] = int(value)
            previous_stream = current_stream
    if len(steps) != manifest["warmup"] + manifest["timed"]:
        raise ValueError("incomplete timestep sequence")
    observed, _ = logical_weights(spms, actual, initial, args.dumps, manifest["rule"])
    error = np.abs(observed.astype(float) - reference.w.astype(float))
    timed = steps[manifest["warmup"] :]
    keys = (
        "E",
        "I",
        "buffer_updates",
        "mshr_loads",
        "mshr_commits",
        "overflow_rows",
        "write_words",
        "pool_full",
        "allocation_waits",
        "forward_weights",
        "reverse_events",
        "spill_records",
        "manual_reverse",
        "scatter_reverse",
    )
    if shared_rng:
        keys += ("bulk_copies", "overlap_checks", "overlap_ready", "ac_wait_polls")
    timer = re.search(r"\[System\] Timer: (\d+) cycles", log)
    if timer is None:
        raise ValueError("missing timer")
    report = dict(
        rule=manifest["rule"],
        kernel=manifest["kernel"],
        steps=steps,
        neurons=comparisons,
        cycles_per_timestep=int(timer[1]) / len(timed),
        average_per_timestep={k: sum(s[k] for s in timed) / len(timed) for k in keys},
        maximum_buffer_bytes_per_pu=max(
            s["maximum_buffer_bytes_per_pu"] for s in steps
        ),
        mshr_peak_per_pu=max(s["mshr_peak_per_pu"] for s in steps),
        record_peak_per_pu=max(s["record_peak_per_pu"] for s in steps),
        weights=dict(
            count=len(observed),
            max_absolute=float(error.max()),
            rms=float(np.sqrt(np.mean(error**2))),
            different=int(np.count_nonzero(observed != reference.w)),
        ),
        stochastic_replay=shared_rng,
        firing_flag=manifest.get("firing_flag", "N_FIRE and N_PRE_FIRE"),
        current_state=manifest.get("current_state", "separate"),
        trace_state=manifest.get("trace_state", "unscaled"),
        trace_population=manifest.get("trace_population", "all"),
        inferred_poisson_histogram=noise_counts.tolist() if shared_rng else None,
        maximum_poisson_residual=maximum_noise_residual if shared_rng else None,
        combined_derivative_roundoff=derivative_roundoff if combined and shared_rng else None,
        independent_delivery_error=delivery_error,
        reference_average_per_timestep={k: float(np.mean([manifest["reference_spikes"][s["step"] - 1][i] for s in timed])) for i, k in enumerate(("E", "I"))},
    )
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    for c in comparisons.values():
        flag = "reserved_fire" if single_fire else "pre_fire"
        integer_fields = ("gid", "fire", flag, "refractory") if shared_rng else ("rng", "gid", "fire", flag, "refractory")
        if any(c[k]["different"] for k in integer_fields):
            raise ValueError("integer neuron state divergence")
        if c["V"]["max_absolute"] > 0.02:
            raise ValueError("voltage divergence")
        current_keys = ("I_total", "dI_total") if combined else ("Iex", "dIex", "Iin", "dIin")
        for k in (*current_keys, "pre_trace", "post_trace"):
            if k == "dI_total" and shared_rng:
                # Checked against the independent FP64 expression and its
                # per-neuron rounding bound above, including near-zero sums.
                continue
            if c[k]["max_relative_scaled"] > 2e-4:
                raise ValueError("neuron state divergence: " + k)
    if not np.all(np.isfinite(observed)) or error.max() > (0.02 if shared_rng else 0.002):
        raise ValueError("plastic weight divergence")
    if sum(s["mshr_loads"] for s in steps) != sum(s["mshr_commits"] for s in steps):
        raise ValueError("MSHR accounting mismatch")
    print(
        json.dumps(
            {k: v for k, v in report.items() if k not in ("steps", "neurons")}, indent=2
        )
    )


def validate(args):
    manifest = json.loads((args.images / "manifest.json").read_text())
    initial = np.load(args.images / "initial.npz")
    log = args.log.read_text()
    if "[System] Result: 0 (0x0)" not in log:
        raise ValueError("simulator did not report success")
    steps = []
    for line in log.splitlines():
        if line.startswith("[Brunel step]"):
            steps.append({k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)})
    if manifest.get("kernel") == "timestep-buffer-mshr":
        return validate_buffer(args, manifest, initial, log, steps)
    expected = manifest["reference_spikes"]
    reference = Reference(
        tuple(initial[k] for k in ("pre", "post", "static_pre", "static_post")),
        manifest["rule"],
        state=(initial["neurons"], initial["weights"]),
    )
    comparisons = {}
    popcount = np.array([i.bit_count() for i in range(256)], np.int32)
    hot = manifest.get("cache_initial", {})
    occupied = hot.get("occupied", 0)
    previous_loads = previous_weight_loads = previous_resident = 0
    previous_replay = [0, 0, 0]
    scalar = manifest["spm"].get("cache_entry_bytes") == 8
    slots = 800 if scalar else 160
    for s in steps:
        if "resident_lines" in s:
            s["resident_entries"] = s["resident_lines"]
            s["pending_entries"] = s["pending_lines"]
        spms_step, observed_nodes = dump(args.dumps / f"step-{s['step']:04}.bin")
        pending_coefficients = dirty_weights = 0
        for spm in spms_step:
            entries = spm[576:2176].reshape(slots, -1)
            valid = entries[:, 0] != 0
            if scalar:
                state = entries[:, 0] & 3
                pending_coefficients += int(np.count_nonzero(valid & (state >= 2)))
                dirty_weights += int(np.count_nonzero(valid & (state == 1)))
            else:
                ctrl = entries[:, 1]
                resident = (ctrl & (1 << 24)) != 0
                pending_coefficients += int(
                    popcount[ctrl[valid & ~resident] & 255].sum()
                )
                dirty_weights += int(
                    popcount[(ctrl[valid & resident] >> 16) & 255].sum()
                )
        s["pending_coefficients"] = pending_coefficients
        s["dirty_weights"] = dirty_weights
        reference.tick()
        comparisons[str(s["step"])] = compare_nodes(observed_nodes, reference.n)
        replay = manifest["spm"].get("replay_record_bytes") == 8
        spill_total = (
            sum(int(spm[0x2400 // 4 + 4]) for spm in spms_step) if replay else 0
        )
        if replay:
            current_replay = [spill_total] + [
                sum(int(spm[0x2400 // 4 + i]) for spm in spms_step) for i in (5, 6)
            ]
            for j, key in enumerate(
                ("spill_records", "manual_reverse", "scatter_reverse")
            ):
                s[key] = current_replay[j] - previous_replay[j]
            previous_replay = current_replay
            s["record_peak_per_pu"] = max(
                int(spm[0x2400 // 4 + 1]) for spm in spms_step
            )
        loads = sum(int(spm[18]) - int(spm[34]) for spm in spms_step) - 2 * spill_total
        s["async_line_loads"] = loads - previous_loads
        previous_loads = loads
        if scalar:
            weight_loads = sum(int(spm[62]) for spm in spms_step)
            s["weight_line_loads"] = weight_loads - previous_weight_loads
            previous_weight_loads = weight_loads
        else:
            # A legacy line fill either creates a resident table entry or
            # materializes an evicted pending line. Net resident growth plus
            # all evictions therefore counts weight fills exactly.
            s["weight_line_loads"] = (
                s["resident_entries"] - previous_resident + s["evictions"]
            )
        previous_resident = s["resident_entries"]
        s["other_async_line_loads"] = s["async_line_loads"] - s["weight_line_loads"]
        s["reference_E"], s["reference_I"] = expected[s["step"] - 1]
        total = s["hits"] + s["misses"]
        s["hit_rate"] = s["hits"] / total if total else None
        current = s["resident_entries"] + s["pending_entries"]
        # Each insertion adds one entry unless it evicts one existing tag.
        s["table_inserts"] = current - occupied + s["evictions"]
        occupied = current
        if not 0 <= s["table_inserts"] <= s["misses"]:
            raise ValueError("invalid cache accounting")
        s["tag_hit_rate"] = 1 - s["table_inserts"] / total if total else None
        s["occupancy_fraction"] = current / (512 * slots)
        s["resident_weight_capacity_fraction"] = (
            s["resident_entries"] * (1 if scalar else 8) / 4050000
        )
    if len(steps) != manifest["warmup"] + manifest["timed"]:
        raise ValueError(f"only {len(steps)} completed steps")
    if [s["step"] for s in steps] != list(range(1, len(steps) + 1)):
        raise ValueError("missing or duplicate steps")
    for step in (1, manifest["warmup"], len(steps)):
        if not step:
            continue
        ref = np.load(args.images / f"reference-{step:04}.npz")
        spms, actual = dump(args.dumps / f"step-{step:04}.bin")
        checkpoint = compare_nodes(actual, ref["neurons"])
        if checkpoint != comparisons[str(step)]:
            raise ValueError("saved checkpoint disagrees with independent replay")
    observed, cache = logical_weights(
        spms, actual, initial, args.dumps, manifest["rule"]
    )
    expected_w = ref["weights"]
    if not np.all(np.isfinite(observed)):
        raise ValueError("nonfinite plastic weights")
    err = np.abs(observed.astype(float) - expected_w.astype(float))
    weights = {
        "count": len(observed),
        "max_absolute": float(err.max()),
        "rms": float(np.sqrt(np.mean(err**2))),
        "different": int(np.count_nonzero(observed != expected_w)),
    }
    timed = steps[manifest["warmup"] :]
    blocks = []
    for start in range(0, len(timed), 10):
        block = timed[start : start + 10]
        h = sum(s["hits"] for s in block)
        m = sum(s["misses"] for s in block)
        inserts = sum(s["table_inserts"] for s in block)
        blocks.append(
            {
                "steps": [s["step"] for s in block],
                "hits": h,
                "misses": m,
                "hit_rate": h / (h + m) if h + m else None,
                "tag_hit_rate": 1 - inserts / (h + m) if h + m else None,
            }
        )
    rates = [s["hit_rate"] for s in blocks if s["hit_rate"] is not None]
    timer = re.search(r"\[System\] Timer: (\d+) cycles", log)
    if not timer:
        raise ValueError("missing workload timer")
    totals = {
        key: sum(s[key] for s in timed)
        for key in (
            "E",
            "I",
            "hits",
            "misses",
            "pending_ops",
            "evictions",
            "write_words",
            "forced",
            "probes",
            "reverse_events",
            "table_inserts",
        )
    }
    totals["async_line_loads"] = sum(s["async_line_loads"] for s in timed)
    totals["weight_line_loads"] = sum(s["weight_line_loads"] for s in timed)
    totals["other_async_line_loads"] = sum(s["other_async_line_loads"] for s in timed)
    if replay:
        for key in ("spill_records", "manual_reverse", "scatter_reverse"):
            totals[key] = sum(s[key] for s in timed)
    demands = totals["hits"] + totals["misses"]
    report = {
        "rule": manifest["rule"],
        "presim_steps": manifest["presim"],
        "steps": steps,
        "neurons": comparisons,
        "weights": weights,
        "final_cache": cache,
        "timed_blocks": blocks,
        "timed_totals": totals,
        "average_per_timestep": {k: v / len(timed) for k, v in totals.items()},
        "table_state_change_per_timestep": {
            k: (
                steps[-1][k]
                - (
                    steps[manifest["warmup"] - 1][k]
                    if manifest["warmup"]
                    else hot.get(k, 0)
                )
            )
            / len(timed)
            for k in ("pending_coefficients", "dirty_weights")
        },
        "cycles_per_timestep": int(timer[1]) / len(timed),
        "timed_resident_hit_rate": totals["hits"] / demands if demands else None,
        "timed_tag_hit_rate": 1 - totals["table_inserts"] / demands
        if demands
        else None,
        "written_words_per_eviction": totals["write_words"] / totals["evictions"]
        if totals["evictions"]
        else None,
        "timed_population_rate_hz": {
            "E": totals["E"] / (NE * len(timed) * 0.0001),
            "I": totals["I"] / (NI * len(timed) * 0.0001),
        },
        "hit_rate_block_range": max(rates) - min(rates) if rates else None,
        "spike_count_max_difference": max(
            max(abs(s["E"] - s["reference_E"]), abs(s["I"] - s["reference_I"]))
            for s in steps
        ),
        "timed_cycles": int(timer[1]),
        "observed_step_cycles": steps[-1]["cycle"]
        - steps[manifest["warmup"] - 1]["cycle"]
        if manifest["warmup"]
        else None,
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    # RNG and identity must be exact independently of network sensitivity.
    for c in comparisons.values():
        if any(
            c[key]["different"]
            for key in ("rng", "gid", "fire", "pre_fire", "refractory")
        ):
            raise ValueError("integer neuron state divergence")
        if c["V"]["max_absolute"] > 0.02:
            raise ValueError("voltage diverged by >0.02 mV")
        for key in ("Iex", "dIex", "Iin", "dIin", "pre_trace", "post_trace"):
            if c[key]["max_relative_scaled"] > 2e-4:
                raise ValueError(key + " diverged by >2e-4 scaled relative error")
    if report["spike_count_max_difference"] > 2:
        raise ValueError("per-step spike count differs by >2")
    if weights and weights["max_absolute"] > 0.002:
        raise ValueError("weights differ by >0.002 pA")
    print(
        json.dumps(
            {k: v for k, v in report.items() if k not in ("steps", "neurons")}, indent=2
        )
    )


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--images", type=Path, required=True)
    p.add_argument("--dumps", type=Path, required=True)
    p.add_argument("--log", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    validate(p.parse_args())
