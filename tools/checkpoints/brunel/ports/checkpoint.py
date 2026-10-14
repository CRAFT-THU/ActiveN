"""Complete CUDA Brunel checkpoints at the after-synapses/before-neurons boundary.

The portable arrays include the sparse multigraph, lazy STDP state, incoming
currents, neuron state and native XORWOW states. Loading on another GPU preserves
the state but does not promise an identical floating-point trajectory.
"""
from __future__ import annotations

import ctypes
import hashlib
import json
from pathlib import Path

import numpy as np

FORMAT = "brunel-genn-phase-checkpoint-v1"
GROUPS = ("ee", "ei", "ie", "ii")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class PhaseBoundary:
    """Rotate GeNN's synapse/neuron sequence without changing its kernels."""
    def __init__(self, network, build_path, rule):
        self.net = network
        self.lib = ctypes.CDLL(str(Path(build_path).resolve() / f"brunel_{rule}_CODE/librunner.so"))
        self.lib.updateSynapses.argtypes = [ctypes.c_float]
        self.lib.updateSynapses.restype = None
        self.lib.updateNeurons.argtypes = [ctypes.c_float, ctypes.c_uint]
        self.lib.updateNeurons.restype = None
        self.ready = False

    def enter(self):
        if self.ready:
            raise RuntimeError("already at the checkpoint boundary")
        self.lib.updateSynapses(ctypes.c_float(self.net.model.t))
        self.ready = True

    def advance(self, steps):
        if not self.ready or steps < 1:
            raise ValueError("advance requires a boundary and positive step count")
        # Recording is disabled: timestep index is unused by this kernel.
        self.lib.updateNeurons(ctypes.c_float(self.net.model.t), 0)
        self.net.model.timestep += 1
        for _ in range(steps - 1):
            self.net.model.step_time()
        self.lib.updateSynapses(ctypes.c_float(self.net.model.t))


def rng_state(net, pop, value=None):
    """GeNN CUDA's generated XORWowStateInternal is six uint32 words."""
    array = net.model._runtime.get_array(pop, "rng")
    pointer = ctypes.c_void_p(array._device_pointer)
    runtime = ctypes.CDLL("libcudart.so.12")
    runtime.cudaMemcpy.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    runtime.cudaMemcpy.restype = ctypes.c_int
    if value is None:
        data = np.empty((pop.num_neurons, 6), np.uint32)
        code = runtime.cudaMemcpy(data.ctypes.data, pointer, data.nbytes, 2)
    else:
        data = np.ascontiguousarray(value, dtype=np.uint32)
        if data.shape != (pop.num_neurons, 6):
            raise ValueError("RNG shape mismatch")
        code = runtime.cudaMemcpy(pointer, data.ctypes.data, data.nbytes, 1)
    if code:
        raise RuntimeError(f"cudaMemcpy of RNG state failed: {code}")
    return data


def capture(net, boundary, rule, seed):
    if not boundary.ready:
        raise RuntimeError("checkpoint requires the phase boundary")
    data = {}
    for name in ("exc", "inh"):
        pop = getattr(net, name)
        for varname, var in pop.vars.items():
            var.pull_from_device()
            data[f"{name}_{varname}"] = var.view.copy()
        data[f"{name}_rng"] = rng_state(net, pop)
    for name in GROUPS:
        syn = getattr(net, name)
        syn.pull_connectivity_from_device()
        pre, post = syn.get_sparse_pre_inds(), syn.get_sparse_post_inds()
        # Canonical sorting is stable, so parallel edges retain identity.
        order = np.lexsort((post, pre))
        data[f"{name}_pre"] = np.asarray(pre[order], np.int32)
        data[f"{name}_post"] = np.asarray(post[order], np.int32)
        for varname, var in syn.vars.items():
            var.pull_from_device()
            data[f"{name}_{varname}"] = var.values[order].copy()
        syn.out_post.pull_from_device()
        data[f"{name}_input"] = syn.out_post.view.copy()
    metadata = dict(format=FORMAT, rule=rule, seed=seed, dt_ms=0.1,
                    timestep=int(net.model.timestep), boundary="after-synapses-before-neurons",
                    precision="float32", rng="GeNN CUDA XORWowStateInternal, 24 bytes/neuron",
                    stdp_timestamps="uint32 ticks", delay_steps=0,
                    graph_sha256=hashlib.sha256(b"".join(data[f"{g}_{v}"].tobytes()
                        for g in GROUPS for v in ("pre", "post"))).hexdigest())
    data["metadata"] = np.array(json.dumps(metadata))
    return data


def load(path):
    with np.load(path, allow_pickle=False) as file:
        data = {key: file[key] for key in file.files}
    metadata = json.loads(str(data["metadata"]))
    if metadata["format"] != FORMAT or metadata["delay_steps"] != 0:
        raise ValueError("unsupported checkpoint")
    return data, metadata


def restore(net, boundary, data):
    metadata = json.loads(str(data["metadata"]))
    for name in ("exc", "inh"):
        pop = getattr(net, name)
        for varname, var in pop.vars.items():
            var.view[:] = data[f"{name}_{varname}"]
            var.push_to_device()
        rng_state(net, pop, data[f"{name}_rng"])
    for name in GROUPS:
        syn = getattr(net, name)
        syn.pull_connectivity_from_device()
        pre, post = syn.get_sparse_pre_inds(), syn.get_sparse_post_inds()
        order = np.lexsort((post, pre))
        if not (np.array_equal(pre[order], data[f"{name}_pre"])
                and np.array_equal(post[order], data[f"{name}_post"])):
            raise ValueError(f"{name} graph differs from the checkpoint")
        for varname, var in syn.vars.items():
            values = np.empty_like(data[f"{name}_{varname}"])
            values[order] = data[f"{name}_{varname}"]
            var.values = values
            var.push_to_device()
        syn.out_post.view[:] = data[f"{name}_input"]
        syn.out_post.push_to_device()
    net.model.timestep = metadata["timestep"]
    boundary.ready = True


def canonical_traces(data):
    """Convert lazy edge traces to one unscaled trace per E neuron.

    Mathematically identical copies can differ from FP32 lazy-decay rounding;
    retain their mean and report the maximum conversion error explicitly.
    """
    meta = json.loads(str(data["metadata"]))
    tick = meta["timestep"]
    last = data["ee_lastTraceTick"].astype(np.int64)
    if np.any(last > tick):
        raise ValueError("future STDP timestamp")
    result, errors = {}, {}
    n = len(data["exc_V"])
    for name, index, tau in (("pre", "ee_pre", 20.0), ("post", "ee_post", 30.0)):
        normalized = data[f"ee_{name}Trace"].astype(np.float64) * np.exp(-(tick - last) * .1 / tau)
        ids = data[index]
        counts = np.bincount(ids, minlength=n)
        if np.any(counts == 0):
            raise ValueError("cannot reconstruct an unrepresented neuron's trace")
        mean = (np.bincount(ids, weights=normalized, minlength=n) / counts).astype(np.float32)
        error = np.abs(normalized - mean[ids])
        if not np.all(error <= 2e-5 * np.maximum(1., np.abs(mean[ids]))):
            raise ValueError(f"{name} trace copies disagree beyond FP32 conversion tolerance")
        result[name] = mean
        errors[name] = dict(max_absolute=float(error.max()), rms=float(np.sqrt(np.mean(error**2))))
    return result, errors
