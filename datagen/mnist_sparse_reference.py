"""Independent FP32 plasticity replay from initial images and observed post spikes.

This checks ActiveN's inherited arithmetic, not GeNN trajectory equivalence.
The fifth-root operation sequence is also checked against FP64 roots by the
dense arithmetic tests. No target instructions or reverse CSR lists are used.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import re
import struct

import mnist_train as dense
from mnist_sparse_state import header, logical_state, read_dump, word
from mnist_sparse import MAGIC


def scalar(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def add(a: float, b: float) -> float:
    return dense.f32(dense.f32(a) + dense.f32(b))


def sub(a: float, b: float) -> float:
    return dense.f32(dense.f32(a) - dense.f32(b))


def mul(a: float, b: float) -> float:
    return dense.f32(dense.f32(a) * dense.f32(b))


def fifth_root(value: float) -> float:
    if value <= 0:
        return 0.0
    adjustment = 0
    bits = dense.f32_bits(value)
    if bits < 0x02000000:
        value = mul(value, 2.0 ** 100)
        bits = dense.f32_bits(value)
        adjustment = 20 << 23
    inverse = scalar(0x4C2BAC89 - ((bits * 0x33333333) >> 32))
    squared = mul(inverse, inverse)
    fourth = mul(squared, squared)
    inverse = sub(mul(1.20148, inverse), mul(mul(mul(value, 0.2), squared), fourth))
    squared = mul(inverse, inverse)
    estimate = mul(value, mul(squared, squared))
    difference = sub(mul(estimate, inverse), 1.555575)
    estimate = mul(estimate, add(0.7777622219500002, mul(0.72, mul(difference, difference))))
    return scalar(dense.f32_bits(estimate) - adjustment)


def initial_spms(path: Path) -> list[bytes]:
    data = path.read_bytes()
    section = word(data, 4)
    spms = []
    for pu in range(512):
        offset, size = struct.unpack_from("<II", data, section + pu * 8)
        spms.append(data[section + offset:section + offset + size])
    return spms


def post_history(path: Path, steps: int) -> list[set[int]]:
    history = []
    for match in re.finditer(r"\[Sparse step\] completed=(\d+) posts=([0-9a-f,]+)", path.read_text()):
        if int(match[1]) != len(history) + 1:
            raise ValueError("invalid sparse step observation sequence")
        masks = [int(value, 16) for value in match[2].split(",")]
        if len(masks) != 8 or any(mask >> 50 for mask in masks):
            raise ValueError("invalid sparse post mask")
        history.append({group * 50 + bit for group, mask in enumerate(masks)
                        for bit in range(50) if mask & (1 << bit)})
    if len(history) != steps:
        raise ValueError(f"expected {steps} observed steps, got {len(history)}")
    return history


def validate(dram0: Path, log: Path, final_dump: Path, steps: int) -> dict:
    spms = initial_spms(dram0)
    if any(header(spm, "MAGIC") != MAGIC for spm in spms):
        raise ValueError("FP32 replay requires fan-in-cap sparse images (MSP4)")
    initial_weights, initial_inputs, initial_neurons = logical_state(spms)
    final_weights, final_inputs, final_neurons = logical_state(read_dump(final_dump))
    if set(initial_weights) != set(final_weights):
        raise ValueError("structural ownership changed during simulation")
    rule = header(spms[0], "RULE")
    pot, dep, target = (scalar(header(spms[0], key)) for key in ("POT", "DEP", "PRE_TARGET"))
    weights = {edge: scalar(bits) for edge, bits in initial_weights.items()}
    outgoing, incoming = [[] for _ in range(784)], [[] for _ in range(400)]
    for pre, post in weights:
        outgoing[pre].append(post)
        incoming[post].append(pre)
    # Derive independently from logical forward ownership, never from cap tables
    # or local/global reverse lists produced by the generator.
    caps = [dense.f32(784 / len(row)) if row else 0.0 for row in incoming]
    cutoffs = {}
    for spm in spms:
        for local in range(header(spm, "INPUT_COUNT")):
            state = header(spm, "INPUT_BASE") + local * 28
            pre = word(spm, state) & 0x7fffffff
            cutoff = word(spm, state + 4)
            if pre in cutoffs and cutoffs[pre] != cutoff:
                raise ValueError("inconsistent replicated input cutoff")
            cutoffs[pre] = cutoff
    for pre in range(784):
        if len({initial_inputs[(pre, group)] for group in range(8)}) != 1:
            raise ValueError("inconsistent replicated input state")
    x = [scalar(initial_inputs[(pre, 0)][0]) for pre in range(784)]
    rng = [initial_inputs[(pre, 0)][1] for pre in range(784)]
    y1 = [scalar(initial_neurons[post][4]) for post in range(400)]
    y2 = [scalar(initial_neurons[post][5]) for post in range(400)]
    pre_counts, post_counts = [], []
    for fired_posts in post_history(log, steps):
        rng = [dense.xorshift32(value) for value in rng]
        fired_pres = {pre for pre in range(784) if rng[pre] <= cutoffs[pre]}
        pre_counts.append(len(fired_pres))
        post_counts.append(len(fired_posts))
        x = [mul(value, scalar(0x3f7cd1e6)) for value in x]
        if rule != 1:
            y1 = [mul(value, scalar(0x3f7cd1e6)) for value in y1]
        if rule == 3:
            y2 = [mul(value, scalar(0x3f7e67ad)) for value in y2]
        if rule != 1:
            minus = [mul(value, dep) for value in y1]
            for pre in fired_pres:
                for post in outgoing[pre]:
                    wmax = caps[post]
                    weight = weights[pre, post]
                    delta = mul(minus[post], fifth_root(weight)) if rule == 2 else minus[post]
                    weights[pre, post] = min(wmax, max(0.0, sub(weight, delta)))
        for post in fired_posts:
            wmax = caps[post]
            plus = mul(y2[post], pot) if rule == 3 else 0.0
            for pre in incoming[post]:
                weight, trace = weights[pre, post], x[pre]
                if pre in fired_pres:
                    trace = 1.0 if rule == 3 else add(trace, 1.0)
                delta = (mul(plus, trace) if rule == 3 else
                         mul(mul(pot, sub(trace, target)), fifth_root(sub(wmax, weight))))
                weights[pre, post] = min(wmax, max(0.0, add(weight, delta)))
        for pre in fired_pres:
            x[pre] = 1.0 if rule == 3 else add(x[pre], 1.0)
        for post in fired_posts:
            if rule == 3: y1[post] = y2[post] = 1.0
            elif rule == 2: y1[post] = add(y1[post], 1.0)
    expected_weights = {edge: dense.f32_bits(value) for edge, value in weights.items()}
    differences = [(edge, hex(bits), hex(final_weights[edge])) for edge, bits in expected_weights.items()
                   if bits != final_weights[edge]]
    if differences:
        raise ValueError(f"FP32 weight replay failed ({len(differences)} weights): {differences[:8]}")
    expected_inputs = {(pre, group): (dense.f32_bits(x[pre]), rng[pre], int(pre in fired_pres))
                       for pre in range(784) for group in range(8)}
    if expected_inputs != final_inputs:
        raise ValueError("FP32 input-state replay failed")
    for post in range(400):
        if final_neurons[post][4:6] != (dense.f32_bits(y1[post]), dense.f32_bits(y2[post])):
            raise ValueError(f"FP32 post-trace replay failed at post {post}")
    digest = hashlib.sha256(repr(sorted(expected_weights.items())).encode("ascii")).hexdigest()
    return dict(status="passed", steps=steps, structural_synapses=len(weights), input_spikes=pre_counts,
                excitatory_spikes=post_counts, expected_weight_sha256=digest,
                arithmetic="inherited ActiveN FP32 sequence; observed post spikes")
