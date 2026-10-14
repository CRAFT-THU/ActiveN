#!/usr/bin/env python3
"""Settled, shared-checkpoint Brunel baseline (isolated output per invocation)."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

import numpy as np
from scipy.stats import ks_2samp

from ports.common import make_model
from ports.genn_port import GeNNBrunel, stdp_timestamp_metadata
from ports.checkpoint import (GROUPS, PhaseBoundary, canonical_traces, capture,
                              load, restore, sha256)

SEED = 20260724
RULES = dict(minimum_seconds=200, window_seconds=100, check_every_seconds=50,
             consecutive_passes=2, std_drift_fraction=.01, std_range_fraction=.02,
             mean_drift_pa=.1, endpoint_ks=.02, rate_drift_fraction=.02)


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def append(path, value):
    with Path(path).open("a") as stream:
        stream.write(json.dumps(value, allow_nan=False) + "\n")


def weight_stats(values):
    a = np.asarray(values, np.float64)
    if not np.all(np.isfinite(a)) or np.min(a) < 0:
        raise ValueError("invalid weights")
    return dict(count=a.size, mean_pa=float(a.mean()), std_pa=float(a.std()),
                min_pa=float(a.min()), max_pa=float(a.max()),
                quantiles_pa=np.quantile(a, [0,.01,.05,.25,.5,.75,.95,.99,1]).tolist())


def settling_check(weights, activity, first, last):
    t = np.asarray([x["seconds"] for x in weights], float)
    std = np.asarray([x["std_pa"] for x in weights])
    mean = np.asarray([x["mean_pa"] for x in weights])
    window = RULES["window_seconds"]
    result = dict(std_drift_fraction=float(np.polyfit(t, std, 1)[0]*window/std.mean()),
                  std_range_fraction=float(np.ptp(std)/std.mean()),
                  mean_drift_pa=float(np.polyfit(t, mean, 1)[0]*window),
                  endpoint_ks=float(ks_2samp(first, last, method="asymp").statistic))
    rate_drifts = []
    for key in ("e_hz", "i_hz"):
        rates = np.asarray([x[key] for x in activity])
        ticks = np.asarray([x["seconds"] for x in activity])
        rate_drifts.append(float(np.polyfit(ticks, rates, 1)[0]*window/max(rates.mean(), 1e-30)))
    result["rate_drift_fraction"] = rate_drifts
    result["passed"] = bool(all(abs(result[k]) < RULES[k] for k in
        ("std_drift_fraction", "std_range_fraction", "mean_drift_pa", "endpoint_ks"))
        and max(map(abs, rate_drifts)) < RULES["rate_drift_fraction"])
    return result


def construct(args, data=None):
    import psutil
    psutil.cpu_count = lambda logical=True: 8
    spec = make_model(args.rule, 1., .05, delay_ms=0.,
                      recurrent_delivery_scale=np.sqrt(20),
                      external_rate_scale=.47 if args.rule == "additive" else .32)
    connectivity = None if data is None else {g:(data[f"{g}_pre"],data[f"{g}_post"]) for g in GROUPS}
    net = GeNNBrunel(spec=spec, seed=SEED, state_seed=SEED, backend="cuda",
        build_path=args.output/"build", recording_steps=0, precision="float",
        stdp_timing="arrival", stdp_tie_order="nest_causal_boundary",
        timing_enabled=False, record_spikes=False, connectivity=connectivity)
    boundary = PhaseBoundary(net, args.output/"build", args.rule)
    if data is not None:
        restore(net, boundary, data)
    return net, boundary


def settle(args):
    net, boundary = construct(args)
    rows, weights, samples = [], [], {}
    previous = net.population_spike_counts()
    passes = 0
    started = time.perf_counter()
    try:
        for sec in range(1, args.max_seconds+1):
            wall = net.step(10000)
            current = net.population_spike_counts()
            e, i = current[0]-previous[0], current[1]-previous[1]
            row = dict(seconds=sec, wall_seconds=wall, e_hz=float(e.mean()), i_hz=float(i.mean()),
                       e_spikes_per_timestep=float(e.sum()/10000), i_spikes_per_timestep=float(i.sum()/10000))
            previous = current
            rows.append(row)
            append(args.output/"activity.jsonl", row)
            if not 0 < min(row["e_hz"],row["i_hz"]) or max(row["e_hz"],row["i_hz"]) > 100:
                raise RuntimeError("silent or runaway population")
            if sec % 10 == 0:
                sample = net.sample_weights(100000).astype(np.float32)
                samples[sec] = sample
                summary = dict(seconds=sec, **weight_stats(sample))
                weights.append(summary)
                append(args.output/"weights.jsonl", summary)
                np.savez_compressed(args.output/f"sample-{sec:04d}.npz", weights=sample)
                print(json.dumps(dict(progress=sec, wall_seconds=time.perf_counter()-started, **summary)), flush=True)
            if sec >= RULES["minimum_seconds"] and sec % RULES["check_every_seconds"] == 0:
                start = sec-RULES["window_seconds"]
                check = settling_check([w for w in weights if w["seconds"]>=start],
                    [r for r in rows if r["seconds"]>start], samples[start], samples[sec])
                passes = passes+1 if check["passed"] else 0
                check.update(seconds=sec, consecutive_passes=passes)
                append(args.output/"settling-checks.jsonl", check)
                print(json.dumps(check), flush=True)
                if passes >= RULES["consecutive_passes"]:
                    boundary.enter()
                    data = capture(net, boundary, args.rule, SEED)
                    traces, errors = canonical_traces(data)
                    data.update(canonical_pre_trace=traces["pre"], canonical_post_trace=traces["post"])
                    np.savez_compressed(args.output/"checkpoint.npz", **data)
                    write(args.output/"result.json", dict(status="settled", seconds=sec,
                        checkpoint_sha256=sha256(args.output/"checkpoint.npz"),
                        weight_stats=weight_stats(data["ee_g"]), trace_conversion_error=errors,
                        wall_seconds=time.perf_counter()-started))
                    return
        write(args.output/"result.json", dict(status="did-not-settle", seconds=args.max_seconds))
        raise RuntimeError("settling cap reached; no benchmark checkpoint selected")
    finally:
        net.close()


def probe(args):
    net, boundary = construct(args)
    try:
        net.step(200)
        boundary.enter()
        saved = capture(net,boundary,args.rule,SEED)
        traces, error = canonical_traces(saved)
        saved.update(canonical_pre_trace=traces["pre"],canonical_post_trace=traces["post"])
        np.savez_compressed(args.output/"checkpoint.npz", **saved)
        boundary.advance(10)
        expected = capture(net,boundary,args.rule,SEED)
        restore(net,boundary,saved)
        boundary.advance(10)
        actual = capture(net,boundary,args.rule,SEED)
        diff = {}
        for key in expected:
            a,b = expected[key],actual[key]
            if np.issubdtype(a.dtype, np.floating):
                diff[key] = float(np.max(np.abs(a-b)))
                np.testing.assert_allclose(a,b,rtol=5e-6,atol=1e-4,err_msg=key)
            else:
                np.testing.assert_array_equal(a,b,err_msg=key)
        write(args.output/"result.json", dict(status="passed", roundtrip_max_absolute=diff,
                                              trace_conversion_error=error))
    finally:
        net.close()


def resume(args):
    data, meta = load(args.checkpoint)
    if meta["rule"] != args.rule:
        raise ValueError("checkpoint rule mismatch")
    net,boundary = construct(args,data)
    try:
        # Prove loaded physical state and graph, including every lazy edge trace.
        restored = capture(net,boundary,args.rule,SEED)
        for key in restored:
            np.testing.assert_array_equal(restored[key], data[key], err_msg=key)
        boundary.advance(args.warmup)
        counts = net.population_spike_counts()
        warm = capture(net,boundary,args.rule,SEED)
        np.savez_compressed(args.output/"measurement-start.npz", **warm)
        trace,_ = canonical_traces(warm)
        pre,post = warm["ee_pre"],warm["ee_post"]
        pre_order = np.argsort(pre,kind="stable")
        post_order = np.argsort(post,kind="stable")
        pre_off = np.r_[0,np.cumsum(np.bincount(pre,minlength=9000))]
        post_off = np.r_[0,np.cumsum(np.bincount(post,minlength=9000))]
        spike_steps,spike_ids,history = [],[],[]
        previous = counts
        for step in range(args.steps):
            boundary.advance(1)
            current = net.population_spike_counts()
            ef = np.flatnonzero(current[0]-previous[0])
            inf = np.flatnonzero(current[1]-previous[1])
            fired = np.zeros(9000,bool); fired[ef]=True
            trace["pre"] *= np.float32(np.exp(-.1/20))
            trace["post"] *= np.float32(np.exp(-.1/30))
            forward = np.concatenate([pre_order[pre_off[j]:pre_off[j+1]] for j in ef]) if len(ef) else np.empty(0,int)
            active = np.concatenate([post_order[post_off[j]:post_off[j+1]] for j in ef]) if len(ef) else np.empty(0,int)
            quiet = int(np.count_nonzero((~fired[post[forward]]) & (trace["post"][post[forward]]>0)))
            updates = int(np.count_nonzero((trace["pre"][pre[active]]>0) |
                                         (fired[pre[active]] & (trace["post"][post[active]]>0))))
            history.append([len(ef),len(inf),len(forward),len(active),quiet,updates])
            trace["pre"][ef] += np.float32(1); trace["post"][ef] += np.float32(1)
            ids = np.r_[ef,inf+9000]
            spike_steps.extend([step]*len(ids)); spike_ids.extend(ids.tolist())
            previous=current
        final = capture(net,boundary,args.rule,SEED)
        np.savez_compressed(args.output/"measurement-final.npz", **final)
        np.savez_compressed(args.output/"dynamics.npz", step=np.asarray(spike_steps,np.int32),
                            neuron=np.asarray(spike_ids,np.int32), history=np.asarray(history,np.int64))
        events = net.event_counters(counts,previous)
        mean=np.mean(history,axis=0)
        diagnostic=dict(e_spikes_per_timestep=mean[0], i_spikes_per_timestep=mean[1],
            e_hz=mean[0]/.9, i_hz=mean[1]/.225, forward_per_timestep=mean[2],
            active_traversals_per_timestep=mean[3], quiet_updates_per_timestep=mean[4],
            active_updates_per_timestep=mean[5], updates_per_timestep=mean[4]+mean[5],
            initial_weights=weight_stats(warm["ee_g"]), final_weights=weight_stats(final["ee_g"]),
            structural_events=events)
        timings=[]
        for repeat in range(args.repeats):
            restore(net,boundary,data)
            boundary.advance(args.warmup)
            net.population_spike_counts()
            started=time.perf_counter()
            boundary.advance(args.steps)
            completed=net.population_spike_counts()
            elapsed=time.perf_counter()-started
            timings.append(elapsed/args.steps*1e6)
            print(json.dumps(dict(repeat=repeat,microseconds_per_timestep=timings[-1])),flush=True)
        write(args.output/"result.json", dict(status="completed",rule=args.rule,seed=SEED,
            checkpoint_sha256=sha256(args.checkpoint),start_tick=meta["timestep"]+args.warmup,
            warmup=args.warmup,timed=args.steps,diagnostic=diagnostic,
            timing_us_per_timestep=timings,median_us_per_timestep=float(np.median(timings)),
            range_us_per_timestep=[min(timings),max(timings)],
            iqr_us_per_timestep=float(np.subtract(*np.percentile(timings,[75,25])))))
    finally:
        net.close()


def main():
    p=argparse.ArgumentParser()
    p.add_argument("mode",choices=("probe","settle","resume"))
    p.add_argument("--output",type=Path,required=True)
    p.add_argument("--rule",choices=("additive","morrison"),required=True)
    p.add_argument("--checkpoint",type=Path)
    p.add_argument("--max-seconds",type=int,default=2000)
    p.add_argument("--steps",type=int,default=10000)
    p.add_argument("--warmup",type=int,default=60)
    p.add_argument("--repeats",type=int,default=5)
    args=p.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    sources=[Path(__file__),Path(__file__).parent/"ports/genn_port.py",Path(__file__).parent/"ports/checkpoint.py",Path(__file__).parent/"ports/common.py"]
    manifest=dict(command=sys.argv,seed=SEED,rule=args.rule,mode=args.mode,
        parent="current sparse zero-delay integer-timestamp GeNN",settling_rules=RULES,
        source_sha256={str(p):sha256(p) for p in sources},
        checkpoint_sha256=sha256(args.checkpoint) if args.checkpoint else None,
        max_seconds=args.max_seconds,warmup=args.warmup,timed=args.steps,repeats=args.repeats,
        python=sys.version,numpy=np.__version__,host=platform.uname()._asdict(),
        gpu=subprocess.check_output(["nvidia-smi","--query-gpu=index,name,uuid,driver_version,memory.used,utilization.gpu,power.limit,clocks.sm","--format=csv"],text=True),
        environment={k:os.environ.get(k) for k in ("CUDA_VISIBLE_DEVICES","CUDA_PATH","CUDAHOSTCXX","LD_LIBRARY_PATH","OMP_NUM_THREADS","OPENBLAS_NUM_THREADS")},
        **stdp_timestamp_metadata())
    write(args.output/"manifest.json",manifest)
    try:
        globals()[args.mode](args)
    except BaseException as error:
        write(args.output/"failure.json",dict(error=repr(error)))
        raise


if __name__=="__main__":
    main()
