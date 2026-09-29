#!/usr/bin/env python3
"""Complete-window Morrison activity, weight and end-to-end timing analysis."""
import argparse
import csv
import json
from pathlib import Path
import re
import struct
import numpy as np

from morrison_model import MAGIC, Spec, constants
from morrison_validate import Image, WeightMoments


def analyze(images, output, log, system_config, frequency):
    image, output = Image(images), Path(output)
    spec, manifest = image.spec, image.manifest
    raw = Path(log).read_text()
    config = json.loads(Path(system_config).read_text())
    core = config["core"]
    if (spec != Spec() or manifest["functional"] or not manifest["external_drive"]
            or config["numPU"] != 512 or config["numMC"] != 16
            or core["memBusWidth"] != 256 or core["scratchpadSize"] != 16384
            or core["pipeCnt"] != 2 or core["iCacheLines"]*core["iCacheBlockSize"] != 8192
            or [int(x, 0) for x in core["memCtrlSizes"]] != [1 << 30]*16):
        raise ValueError("performance analysis requires the full 512-PU/16-MC target")
    if frequency <= 0:
        raise ValueError("frequency must be positive")
    if "[System] Result: 0 (0x0)" not in raw or "Timer did not stop" in raw:
        raise ValueError("workload did not complete its timer interval")
    enabled = re.findall(r'Ramulator enabled: tCK=([\d.]+) ns, transaction=(\d+) bytes, ticks/core-cycle=([\d.]+)', raw)
    if len(enabled) != 1 or int(enabled[0][1]) != 32 or not np.isclose(float(enabled[0][2]), 1/(float(enabled[0][0])*frequency)):
        raise ValueError("missing/incompatible HBM timing startup")
    timers = re.findall(r'\[System\] Timer: (\d+) cycles', raw)
    memory_cycles = re.findall(r'\[Timed memory\] Cycles: (\d+)', raw)
    starts = re.findall(r'\[Timed memory\] Started at cycle (\d+)', raw)
    if len(timers) != 1 or memory_cycles != timers or len(starts) != 1:
        raise ValueError("timer and memory measurement intervals differ")
    timer, timer_start = int(timers[0]), int(starts[0])
    warmup, timed = manifest["warmup"], manifest["timed"]
    rows = list(csv.DictReader((output/"morrison-activity.csv").open()))
    if [int(row["step"]) for row in rows] != list(range(1, warmup+timed+1)):
        raise ValueError("incomplete or duplicated activity interval")
    if warmup and not int(rows[warmup-1]["cycle"]) < timer_start < int(rows[warmup]["cycle"]):
        raise ValueError("timer start does not follow the warmup completion")
    if timer_start+timer < int(rows[-1]["cycle"]):
        raise ValueError("timer stops before final step completion")
    measured = rows[warmup:]
    e_spikes = sum(int(row["e_spikes"]) for row in measured)
    i_spikes = sum(int(row["i_spikes"]) for row in measured)
    outdegree = np.load(Path(images)/"plastic-outdegree.npy")
    pre_opportunities = 0
    with (output/"morrison-spikes.bin").open("rb") as stream:
        if struct.unpack("<4I", stream.read(16)) != (MAGIC, spec.ne, spec.ni, spec.pu):
            raise ValueError("bad spike stream header")
        for row in rows:
            step, count = struct.unpack("<2I", stream.read(8))
            ids = np.frombuffer(stream.read(count*4), dtype="<u4")
            if (step != int(row["step"]) or len(ids) != count or np.any(ids >= spec.n)
                    or len(np.unique(ids)) != count
                    or int((ids < spec.ne).sum()) != int(row["e_spikes"])
                    or int((ids >= spec.ne).sum()) != int(row["i_spikes"])):
                raise ValueError("spike IDs disagree with activity counts")
            if step > warmup:
                pre_opportunities += int(outdegree[ids[ids < spec.ne]].sum(dtype=np.uint64))
        if stream.read(1):
            raise ValueError("trailing spike records")
    post_opportunities = e_spikes*spec.ce
    if sum(int(row["reverse_visits"]) for row in measured) != post_opportunities:
        raise ValueError("reverse visits do not match fixed E-E fan-in")
    backing = Image(images, backing=output).memory
    moments = WeightMoments()
    for pu, descriptors in enumerate(image.rows):
        for descriptor in descriptors:
            _, values, _ = image.forward(pu, descriptor, backing=backing)
            if not len(values):
                continue
            if np.any(~np.isfinite(values)) or np.any(values < 0):
                raise ValueError("invalid final weight")
            values = values.astype(np.float64)/float(constants()["epsc"])
            moments.add(values)
    if moments.count != spec.ne*spec.ce:
        raise ValueError("incomplete final weight population")
    phases = {}
    for cycle, phase, step in re.findall(r'\[Soft\] MNIST phase start cycle=(\d+) phase=(\d+) low=(\d+) high=0', raw):
        key = (int(step), int(phase))
        if key in phases:
            raise ValueError("duplicate phase boundary")
        phases[key] = int(cycle)
    phase_sums = dict(integration=0, forward=0, reverse=0, finish=0)
    for row in measured:
        step = int(row["step"])
        bounds = [phases[(step, phase)] for phase in (2, 3, 4, 5)] + [int(row["cycle"])]
        if bounds != sorted(bounds):
            raise ValueError("unordered phase boundaries")
        for name, first, last in zip(phase_sums, bounds, bounds[1:]):
            phase_sums[name] += last-first
    if not timer_start < phases[(warmup+1, 2)] < timer_start+timer:
        raise ValueError("first measured integration is outside the timer interval")
    stats = sorted((output/"ramulator").glob("*.yaml"))
    if len(stats) != spec.mc or set(map(int, re.findall(r'\[Timed memory MC (\d+)\]', raw))) != set(range(spec.mc)):
        raise ValueError("missing per-MC timed memory statistics")
    seconds = timed*manifest["model"]["dt_ms"]/1000
    return dict(starting_state=manifest["source"], graph_sha256=manifest["graph_sha256"],
                comparison_scope="cold-start estimate" if manifest["source"]["kind"] == "fresh" else "checkpoint start",
                warmup_steps=warmup, measured_steps=timed, measured_biological_seconds=seconds,
                timer_cycles=timer, cycles_per_timestep=timer/timed, frequency_ghz=frequency,
                hardware_seconds=timer/(frequency*1e9), biological_to_hardware_ratio=seconds*frequency*1e9/timer,
                phase_cycles_per_timestep={name: value/timed for name, value in phase_sums.items()},
                phase_note="phase markers are supplementary; end-to-end metric is the peripheral timer",
                e_spikes=e_spikes, i_spikes=i_spikes, e_rate_hz=e_spikes/(spec.ne*seconds),
                i_rate_hz=i_spikes/(spec.ni*seconds),
                presynaptic_ltd_opportunities=pre_opportunities, postsynaptic_ltp_opportunities=post_opportunities,
                forward_deliveries=sum(int(row["forward_deliveries"]) for row in measured),
                final_weights=moments.count, final_weight_mean_pa=moments.mean,
                final_weight_std_pa=moments.std)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("images", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--system-config", type=Path, required=True)
    parser.add_argument("--freq-ghz", type=float, required=True)
    args = parser.parse_args()
    print(json.dumps(analyze(args.images, args.output, args.log, args.system_config, args.freq_ghz), indent=2))


if __name__ == "__main__":
    main()
