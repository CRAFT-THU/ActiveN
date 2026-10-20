"""GeNN replay adapters using the baseline's frozen models and controllers.

One case owns one process: frozen packages use the same Python module names.
"""
from __future__ import annotations

import ctypes
import hashlib
import json
import os
import shlex
import shutil
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np

from baseline_checkpoints import Case, Checkpoint, sha256, write_json
from baseline_windows import Transfer, restore_arrays, save_arrays


def configure_cuda():
    """Use the declared toolkit headers/compiler for GeNN's NVCC probes too."""
    flags = shlex.split(os.environ.get("NVCC_PREPEND_FLAGS", ""))
    cuda = os.environ.get("CUDA_PATH")
    compiler = os.environ.get("CUDAHOSTCXX")
    additions = []
    if cuda:
        if not (Path(cuda) / "include/cuda_runtime.h").is_file():
            raise ValueError("CUDA_PATH must provide the runtime headers")
        additions.append("-I" + str(Path(cuda) / "include"))
    if compiler:
        if not Path(compiler).is_file():
            raise ValueError("CUDAHOSTCXX must name an existing host compiler")
        additions.append("-ccbin=" + compiler)
    for flag in additions:
        if flag not in flags:
            flags.append(flag)
    if flags:
        os.environ["NVCC_PREPEND_FLAGS"] = shlex.join(flags)


class Replay:
    def __init__(self, case: Case, output: Path, *, build_jobs=4):
        self.case = case
        output.mkdir(parents=True, exist_ok=False)
        self.output = output
        configure_cuda()
        write_json(output / "environment.json", {
            k: os.environ.get(k) for k in ("CUDA_PATH", "CUDAHOSTCXX", "NVCC_PREPEND_FLAGS",
                "CUDA_VISIBLE_DEVICES", "PATH", "PYTHONPATH", "LD_LIBRARY_PATH", "STDP_CUDART")})
        executors = output / "executor-source"
        shutil.copytree(case.baseline / "executors", executors,
                        ignore=shutil.ignore_patterns("reports", "tests", "__pycache__", "*.pyc"))
        write_json(output / "executor-source.json", {
            str(p.relative_to(executors)): sha256(p) for p in executors.rglob("*") if p.is_file()})
        if "common" in sys.modules:
            raise RuntimeError("Use a fresh process for each baseline case")
        sys.path.insert(0, str(executors))
        from common.checkpoints import inspect_checkpoint
        args = SimpleNamespace(baseline=case.baseline, backend="gpu", restore="native",
                               data=case.baseline / case.workload / "inputs", mode="timing",
                               output=output, build_jobs=build_jobs, spike_format="raw",
                               warmup=0 if case.classification else 60, steps=100, samples=1)
        if case.classification:
            if case.workload != "mnist":
                args.data /= "data"
            from common.classification import Classification
            self.engine = Classification(args, inspect_checkpoint(case.start.path))
            self.populations = ["input", "exc", "inh"]
        else:
            from common.recurrent import Recurrent
            self.engine = Recurrent(args, inspect_checkpoint(case.start.path))
            self.populations = ["exc", "inh"]
        self.net = self.engine.net
        self.transfer = Transfer()
        self.parent = case.start
        self._graphs = {}

    @property
    def tick(self):
        return int(self.net.model.timestep)

    def close(self):
        self.engine.steps.close()
        self.net.close()

    def restore_parent(self, cp: Checkpoint):
        if not self.case.classification:
            if cp.identity != self.case.start.identity:
                raise ValueError("Unexpected recurrent ancestor")
            # Recurrent replay starts at its already restored/warmed initial
            # boundary and proceeds monotonically; it never rewinds implicitly.
            if self.tick > cp.tick + 60:
                raise ValueError("Recurrent rewind requires restoring an exported window")
            return
        if self.case.workload == "mnist":
            state, meta = self.engine.native.load(cp.path)
            if meta["campaign_contract"] != self.engine.contract:
                raise ValueError("Ancestor campaign/input contract differs")
            values = self.engine.native.restore(self.net, state, meta)
            self.engine.ctl = self.engine.controller_type.from_dict(values)
        else:
            self.engine.ctl = self.engine.suite.restore(self.net, cp.path, self.engine.contract)
        self.parent = cp

    def _begin_attempt(self):
        e, ctl = self.engine, self.engine.ctl
        position = ctl.accepted if self.case.workload == "mnist" else ctl.stream_position
        pixels = e.images[e.order[position % len(e.order)]]
        e.normalize()
        e.set_image(pixels, ctl.intensity)
        ctl.phase, ctl.phase_tick = "presentation", 0
        if self.case.workload != "mnist":
            ctl.last_presented_intensity = ctl.intensity
            ctl.stimulus_counts = None

    def _end_presentation(self):
        e, ctl = self.engine, self.engine.ctl
        current = self.net._pulled(self.net.exc, "spikeCount").astype(np.uint32)
        counts = current.astype(np.int64) - self.net._count_baseline.astype(np.int64)
        if np.any(counts < 0):
            raise RuntimeError("Presentation counter moved backwards")
        self.net._count_baseline = current.copy()
        ctl.last_spikes, ctl.last_active = int(counts.sum()), int(np.count_nonzero(counts))
        ctl.pending_retry = ctl.last_spikes < e.c.minimum_exc_spikes
        if ctl.last_spikes >= (5000 if self.case.workload == "mnist" else 14000):
            raise RuntimeError("Runaway presentation during replay")
        if self.case.workload != "mnist":
            ctl.stimulus_counts = [int(x) for x in counts]
        self.net.set_zero_input()
        ctl.phase, ctl.phase_tick = "rest", 0

    def advance_to(self, target: int):
        if type(target) is not int or target < self.tick:
            raise ValueError("Replay target precedes current state")
        if not self.case.classification:
            if target > self.tick:
                self.engine.steps(target - self.tick, after_synapses=self.engine.full)
            self.transfer.sync()
            return
        e, ctl, c = self.engine, self.engine.ctl, self.engine.c
        while self.tick < target:
            if ctl.phase == "before-attempt":
                self._begin_attempt()
            length = c.stimulus_ticks if ctl.phase == "presentation" else c.rest_ticks
            remaining = length - ctl.phase_tick
            if remaining < 0:
                raise ValueError("Invalid controller phase position")
            n = min(target - self.tick, remaining)
            if n:
                e.steps(n)
                ctl.phase_tick += n
            if ctl.phase_tick == length:
                if ctl.phase == "presentation":
                    self._end_presentation()
                elif self.case.workload == "mnist":
                    ctl.finish_rest()
                else:
                    if ctl.stimulus_counts is None:
                        raise ValueError("Cannot continue a legacy rest state without stimulus activity")
                    ctl.finish_attempt(c)
        self.transfer.sync()

    def prepare(self, sample: dict):
        self.advance_to(sample["warmup_start_tick"])
        if self.case.classification:
            if self.engine.ctl.phase == "before-attempt":
                self._begin_attempt()
            ctl, c = self.engine.ctl, self.engine.c
            if ctl.phase != sample["phase"] or ctl.phase_tick != sample["phase_offset"]:
                raise ValueError("Selected tick and actual controller phase disagree")
            ages = (28000, 30000) if self.case.workload == "cifar10" else (160000, 180000)
            if not ages[0] <= ctl.accepted < ages[1]:
                raise ValueError("Replay left the accepted-image region; do not replace this sample")
            limit = c.stimulus_ticks if ctl.phase == "presentation" else c.rest_ticks
            if ctl.phase_tick + 101 > limit:
                raise ValueError("Window crosses a phase transition")

    def registry(self):
        if self.case.classification:
            registry = self.engine.native.registry(self.net)
            graph = {k: v for k, v in registry.items()
                     if k.rsplit("__", 1)[-1] in ("ind", "rowLength", "colLength", "remap")}
            state = {k: v for k, v in registry.items() if k not in graph}
        else:
            state, graph = self.engine.ck.arrays(self.net)
            # At an ordinary after-neurons boundary these queues have not yet
            # been consumed. Native phase writers omit them; windows must not.
            owners = set()
            for name in ("ee", "ei", "ie", "ii"):
                syn = getattr(self.net, name)
                for prefix, pop in (("src", syn.src), ("trg", syn.trg)):
                    try:
                        count = self.net.model._runtime.get_array(syn, prefix + "SpkCnt")
                    except (IndexError, KeyError, RuntimeError):
                        continue
                    spikes = self.net.model._runtime.get_array(syn, prefix + "Spk")
                    state[name + "__" + prefix + "SpkCnt"] = (count, np.dtype("<u4"), (1,))
                    state[name + "__" + prefix + "Spk"] = (spikes, np.dtype("<u4"), (pop.num_neurons,))
                    owners.add(pop.name)
            if owners != {self.net.exc.name, self.net.inh.name}:
                raise ValueError("Incomplete recurrent spike-queue capture")
        return state, graph

    def host_state(self):
        if self.case.classification:
            return dict(controller=self.engine.ctl.as_dict(),
                        presentation_count_baseline=self.net._count_baseline.tolist())
        return {}

    def save(self, directory: Path, sample: dict, *, rng: dict):
        directory.mkdir(parents=True, exist_ok=False)
        state, graph = self.registry()
        # Parent identity also binds native sparse ordering. Reuse only graphs
        # already captured for this exact restored ancestor.
        if self.case.classification:
            key = hashlib.sha256(json.dumps({k: self.parent.metadata["arrays"][k]
                                for k in graph}, sort_keys=True).encode()).hexdigest()
        else:
            key = self.parent.metadata.get("graph_manifest_sha256", self.parent.identity)
        if key not in self._graphs:
            graph_dir = self.output / ("graph-" + key)
            records = save_arrays(graph_dir, graph, self.transfer)
            write_json(graph_dir / "graph.json", dict(arrays=records))
            self._graphs[key] = dict(directory=str(graph_dir.resolve()), manifest_sha256=sha256(graph_dir / "graph.json"))
        records = save_arrays(directory / "state", state, self.transfer)
        from baseline_windows import SCHEMA
        m = dict(schema=SCHEMA, workload=self.case.workload, case=self.case.name,
                 **sample, warmup_steps=1, measured_steps=100, boundary=self.case.boundary,
                 checkpoint=str(self.parent.path), checkpoint_sha256=self.parent.identity,
                 ancestor_metadata=self.parent.metadata, runtime_host=self.host_state(), rng=rng,
                 graph=self._graphs[key], arrays=records,
                 native_state_conversion="none; original physical units, pending events and RNG retained")
        write_json(directory / "window.json", m)
        return m

    def cleanup_graphs(self):
        for graph in self._graphs.values():
            path = Path(graph["directory"])
            if path.is_symlink() or path.parent != self.output.resolve() or not path.name.startswith("graph-"):
                raise RuntimeError("Refusing to remove a graph outside this run")
            shutil.rmtree(path)
        self._graphs.clear()

    def restore_window(self, directory: Path, m: dict):
        if (m["boundary"] != self.case.boundary or m["workload"] != self.case.workload
                or m["case"] != self.case.name or m["checkpoint_sha256"] != self.parent.identity):
            raise ValueError("Window belongs to a different case, ancestor or event boundary")
        state, graph = self.registry()
        restore_arrays(directory / "state", m["arrays"], state, self.transfer)
        # Graph is immutable during a window. The adapter never changes the
        # ancestor/graph while this temporary checkpoint is live.
        self.net.model.timestep = m["warmup_start_tick"]
        if self.case.classification:
            values = m["runtime_host"]
            self.engine.ctl = type(self.engine.ctl).from_dict(values["controller"])
            self.net._count_baseline = np.asarray(values["presentation_count_baseline"], dtype=np.uint32)
        else:
            self.engine.boundary.ready = self.engine.full

    def record_window(self):
        """Record emission counts, without injecting recurrent outputs."""
        before = self.engine.counts()
        rows = []
        for _ in range(101):
            if self.case.classification:
                self.engine.steps(1)
                self.engine.ctl.phase_tick += 1
            else:
                self.engine.steps(1, after_synapses=self.engine.full)
            current = self.engine.counts()
            row = []
            for a, b in zip(before, current):
                delta = b.astype(np.int64) - a.astype(np.int64)
                if np.any(delta < 0):
                    raise RuntimeError("Cumulative spike count moved backwards")
                row.append(int(delta.sum()))
            rows.append(row)
            before = current
        self.transfer.sync()
        return np.asarray(rows, dtype=np.uint64)
