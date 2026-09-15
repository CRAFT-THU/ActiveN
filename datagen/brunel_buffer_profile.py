#!/usr/bin/env python3
"""Analyze buffer/MSHR handshakes and sampled PU/MC activity, without changing timing."""

import argparse
import bisect
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path
import numpy as np


def analyze(images, run, symbols, output):
    manifest = json.loads((images / "manifest.json").read_text())
    warmup, duration = manifest["warmup"], manifest["timed"]
    report = json.loads((run / "report.json").read_text())
    pending = {}
    latency = []
    requests = Counter()
    outstanding = Counter()
    peak_outstanding = 0
    for row in csv.DictReader((run / "mshr-memory.csv").open()):
        step, cycle, pu, kind, token, address = map(int, row.values())
        key = (pu, token)
        if kind == 0:
            if key in pending:
                raise ValueError("MSHR landing line reused before its response")
            pending[key] = (step, cycle)
            outstanding[pu] += 1
            if step > warmup:
                requests[step, pu, address] += 1
                peak_outstanding = max(peak_outstanding, outstanding[pu])
        else:
            issued_step, issued_cycle = pending.pop(key)
            if issued_step != step:
                raise ValueError("MSHR response crosses timestep boundary")
            outstanding[pu] -= 1
            if step > warmup:
                latency.append(cycle - issued_cycle)
    if pending:
        raise ValueError("unmatched MSHR requests")
    timed_requests = sum(requests.values())
    measured_requests = sum(
        s["mshr_loads"] for s in report["steps"] if s["step"] > warmup
    )
    if timed_requests != measured_requests:
        raise ValueError("handshake and runtime MSHR request counts disagree")
    multiplicity = Counter(requests.values())
    repeats = sum(n - 1 for n in requests.values())
    memory = dict(
        requests_per_timestep=timed_requests / duration,
        distinct_lines_per_timestep=len(requests) / duration,
        repeat_loads_per_timestep=repeats / duration,
        repeat_load_fraction=repeats / timed_requests if timed_requests else 0,
        line_multiplicity_histogram_per_timestep={
            str(k): v / duration for k, v in sorted(multiplicity.items())
        },
        maximum_outstanding_reads_per_pu=peak_outstanding,
        latency_definition="Source-router acceptance to scalar response at the source PU; excludes software issue/notification/service delay",
        response_latency_cycles={
            str(p): float(np.percentile(latency, p)) for p in (0, 50, 90, 95, 99, 100)
        }
        if latency
        else {},
        mean_response_latency_cycles=float(np.mean(latency)) if latency else None,
    )
    symbol_rows = []
    for line in symbols.read_text().splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] in ("t", "T"):
            symbol_rows.append((int(fields[0], 16), fields[2]))
    symbol_rows.sort()
    addresses = [row[0] for row in symbol_rows]
    pcs = defaultdict(Counter)
    for row in csv.DictReader((run / "pc-profile.csv").open()):
        phase, pu, thread, pc, samples = map(int, row.values())
        pos = bisect.bisect_right(addresses, pc) - 1
        name = symbol_rows[pos][1] if pos >= 0 else "unknown"
        pcs[phase, thread][name] += samples
    activity = defaultdict(Counter)
    pu_rows = []
    for row in csv.DictReader((run / "pu-profile.csv").open()):
        row = {k: int(v) for k, v in row.items()}
        if row["step"] > warmup:
            pu_rows.append(row)
            for k, v in row.items():
                if k not in ("step", "phase", "pu"):
                    activity[row["phase"]][k] += v
    mc = defaultdict(Counter)
    for row in csv.DictReader((run / "mc-profile.csv").open()):
        row = {k: int(v) for k, v in row.items()}
        if row["step"] > warmup:
            for k, v in row.items():
                if k not in ("step", "phase", "mc", "max_inflight"):
                    mc[row["phase"]][k] += v
    markers = defaultdict(dict)
    for line in (run / "run.log").read_text().splitlines():
        if "MNIST phase start" in line:
            row = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
            markers[row["low"]][row["phase"]] = row["cycle"]
    end_cycles = {s["step"]: s["cycle"] for s in report["steps"]}
    phases = {}
    for phase, name, start, end in (
        (1, "forward", 2, 3),
        (2, "integration", 3, 4),
        (3, "reverse", 4, 5),
        (6, "flush", 5, None),
    ):
        a, m = activity[phase], mc[phase]
        spans = [
            (markers[s][end] if end else end_cycles[s]) - markers[s][start]
            for s in range(warmup + 1, warmup + duration + 1)
        ]
        phases[name] = dict(
            cycles_per_timestep=float(np.mean(spans)),
            minimum_cycles_per_timestep=int(min(spans)),
            maximum_cycles_per_timestep=int(max(spans)),
            mean_active0_cycles_per_pu_per_timestep=a["active0"]
            * 256
            / (512 * duration),
            mean_active1_cycles_per_pu_per_timestep=a["active1"]
            * 256
            / (512 * duration),
            mean_mshr_occupancy_per_pu=a["mshr_occupancy"] / a["samples"],
            full_mshr_sample_fraction=a["mshr_full"] / a["samples"],
            full_credit_sample_fraction=a["credit_full"] / a["samples"],
            reverse_queue_full_sample_fraction=a["reverse_full"] / a["samples"],
            injection_blocked_sample_fraction=a["injection_blocked"] / a["samples"],
            instruction_stall_sample_fraction=a["stalled"] / a["executing"]
            if a["executing"]
            else 0,
            mc_requests_per_timestep=m["accepted"] / duration,
            mc_request_backpressure_fraction=(m["presented"] - m["accepted"])
            / m["presented"]
            if m["presented"]
            else 0,
            mc_mean_inflight=m["inflight"] / m["cycles"],
            mc_mean_unissued=m["unissued"] / m["cycles"],
            mc_mean_fulfilled=m["fulfilled"] / m["cycles"],
            pc_sample_attribution={
                str(t): [
                    dict(
                        function=k,
                        fraction=v / sum(pcs[phase, t].values()),
                        sampled_cycles_per_pu_per_timestep=v * 256 / (512 * duration),
                    )
                    for k, v in pcs[phase, t].most_common(12)
                ]
                for t in (0, 1)
            },
        )
        if "issued0" in a:
            phases[name].update(
                instructions_per_pu_per_timestep=(a["issued0"] + a["issued1"]) / (512 * duration),
                neurons_per_smt_per_timestep=[a["neurons0"] / (512 * duration), a["neurons1"] / (512 * duration)],
                ac_wait_cycles_per_pu_per_timestep=a["ac_wait_cycles"] / (512 * duration),
                biu_blocked_cycles_per_pu_per_timestep=a["biu_blocked_cycles"] / (512 * duration),
            )
    with (run / "phase-cycles.csv").open("w") as f:
        writer = csv.writer(f)
        writer.writerow(("step", "forward", "integration", "reverse", "flush", "between_steps", "total"))
        for step in range(warmup + 1, warmup + duration + 1):
            marker = markers[step]
            phase_cycles = [marker[3] - marker[2], marker[4] - marker[3], marker[5] - marker[4], end_cycles[step] - marker[5]]
            start = end_cycles.get(step - 1, marker[2])
            total = end_cycles[step] - start
            writer.writerow([step, *phase_cycles, total - sum(phase_cycles), total])
    ac = None
    ac_path = run / "ac-memory.csv"
    if ac_path.exists():
        copies = {}
        ac_latency = []
        total_copies = 0
        for row in csv.DictReader(ac_path.open()):
            step, cycle, pu, kind, lines, address = map(int, row.values())
            if kind == 2:
                if pu in copies:
                    raise ValueError("AC rearmed before every previous response arrived")
                copies[pu] = (step, cycle, lines, set())
            elif kind == 3:
                started_step, started_cycle, count, seen = copies[pu]
                if step != started_step or lines in seen or lines >= count:
                    raise ValueError("AC duplicate, out-of-range or cross-step response")
                seen.add(lines)
                if len(seen) == count:
                    if step > warmup:
                        ac_latency.append(cycle - started_cycle)
                        total_copies += 1
                    del copies[pu]
        if copies:
            raise ValueError("unfinished AC requests")
        timed_steps = [s for s in report["steps"] if s["step"] > warmup]
        if total_copies != sum(s["bulk_copies"] for s in timed_steps):
            raise ValueError("AC handshakes disagree with software accounting")
        checks = sum(s["overlap_checks"] for s in timed_steps)
        ready = sum(s["overlap_ready"] for s in timed_steps)
        ac = dict(
            copies_per_timestep=total_copies / duration,
            overlap_checks_per_timestep=checks / duration,
            copies_ready_before_drain_fraction=ready / checks if checks else None,
            late_copies_per_timestep=(checks - ready) / duration,
            response_latency_cycles={str(p): float(np.percentile(ac_latency, p)) for p in (0, 50, 90, 99, 100)} if ac_latency else {},
            maximum_wait_cycles_per_pu_per_timestep=max((r.get("ac_wait_cycles", 0) for r in pu_rows if r["phase"] == 1), default=0),
        )
    forward_critical = []
    for step in range(warmup + 1, warmup + duration + 1):
        rows = [r for r in pu_rows if r["step"] == step and r["phase"] == 1]
        if rows and "last_forward_injection" in rows[0]:
            last = max(rows, key=lambda r: r["last_forward_injection"])
            if last["last_forward_injection"]:
                forward_critical.append({k: last[k] for k in ("step", "pu", "last_forward_injection", "biu_blocked_cycles", "ac_wait_cycles", "issued0", "issued1")})
    result = dict(
        rule=manifest["rule"],
        timed_steps=duration,
        cycles_per_timestep=report["cycles_per_timestep"],
        mshr=memory,
        phases=phases,
        between_phase_markers_and_timer_cycles_per_timestep=report["cycles_per_timestep"] - sum(p["cycles_per_timestep"] for p in phases.values()),
        async_copy=ac,
        last_plastic_forward_pu_each_timestep=forward_critical,
    )
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--images", type=Path, required=True)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    analyze(args.images, args.run, args.symbols, args.output)
