#!/usr/bin/env python3
"""Build hybrid-SRAM MNIST training images for the ActiveN 512-PU target."""

from __future__ import annotations

import argparse
import ast
import heapq
import json
import math
import struct
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


N_INPUT = 784
N_EXC = 400
N_INH = 400
SPM_SIZE = 16 * 1024
SPM_HEADER_SIZE = 368
MAX_MC = 16
DRAM_HEADER_SIZE = 16
SPM_DESCRIPTOR_SIZE = 8
MAGIC = 0x4D543131

RULES = {"one-trace": 1, "two-trace": 2, "three-trace": 3}
RECURRENT_MODES = {"scatter": 0, "tree": 1, "tree-aggregate": 2}
PLACEMENT_POLICIES = ("edge", "near", "balanced")
INPUT_ASSIGNMENTS = ("contiguous", "rate-balanced")
FULL_HORIZONTAL_GROUP_COUNTS = {
    "full": 2,
    "full-4": 4,
    "full-8": 8,
    "full-16": 16,
}
HORIZONTAL_PARTITIONS = ("none", "hot-partial", *FULL_HORIZONTAL_GROUP_COUNTS)
TRACE_DESTINATION_STRIDE = 83
FEEDFORWARD_DESTINATION_STRIDE = 167
HORIZONTAL_REPLICA_STRIDE = 3
TRACE_SLOT_NONE = 0x1FF
INPUT_RECORD_SIZE = 28
EXC_RECORD_SIZE = 36
INH_RECORD_SIZE = 148
WEIGHT_RECORD_SIZE = 4
EXC_TARGET_RECORD_SIZE = 4
FIRED_VECTOR_RECORD_SIZE = 8
STACK_SIZE = 2 * 1024
STACK0_BASE = 0x3000
STACK0_TOP = STACK0_BASE + STACK_SIZE
STACK1_BASE = STACK0_TOP
STACK1_TOP = STACK1_BASE + STACK_SIZE
FIFTH_ROOT_LUT_OFFSET = 0x2000
FIFTH_ROOT_LUT_BYTES = (256 + 128) * 4


def fifth_root_lut() -> bytes:
    """FP32 exponent factors and midpoint reciprocal-fifth-root mantissas."""
    values = [2.0 ** (-(exponent - 127) / 5.0) for exponent in range(256)]
    values += [(1.0 + (index + 0.5) / 128.0) ** -0.2 for index in range(128)]
    return struct.pack("<384f", *values)

INPUT_GROUP_0 = 1
INPUT_GROUP_1 = 2
INPUT_GROUP_BOTH = INPUT_GROUP_0 | INPUT_GROUP_1
INPUT_GLOBAL_MASK = 0x3FF
INPUT_GLOBAL_REPLICATED = 1 << 30
INPUT_GLOBAL_PRIMARY = 1 << 31

# Controller zones are 32 consecutive Hilbert IDs in the 512-PU/16-MC target.
# Prefixes retain whole zones and add geometrically paired zones whenever a
# central-distance tier cannot be taken in full.
NEURON_ZONE_ORDERS = {
    "central": (5, 6, 8, 15, 4, 9, 7, 14, 1, 12, 3, 11, 0, 13, 2, 10),
    # For N=196/M=224, the seven-zone prefix minimizes modeled peak XY trace load.
    "network-balanced": (2, 4, 6, 7, 9, 14, 15, 0, 3, 5, 8, 10, 12, 1, 11, 13),
    # Wider mappings need different complete-zone sets; extending the seven-zone
    # prefix creates a substantially larger central-link peak.
    "network-balanced-320": (0, 1, 2, 4, 5, 9, 10, 11, 13, 15, 3, 6, 7, 8, 12, 14),
    "network-balanced-352": (0, 2, 3, 4, 7, 8, 10, 11, 12, 13, 15, 1, 5, 6, 9, 14),
}

# Only thread 0 calls the C initializer and therefore uses a stack. Thread 1's
# one-past-SPM stack pointer is retained as a guard against accidental use.
assert STACK0_TOP <= SPM_SIZE
assert STACK1_TOP == SPM_SIZE


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def f32_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def align(data: bytearray, alignment: int) -> None:
    data.extend(bytes((-len(data)) % alignment))


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & -alignment


def encode_jal_x0(offset: int) -> int:
    if offset < -(1 << 20) or offset >= (1 << 20) or offset & 1:
        raise ValueError(f"invalid JAL offset {offset}")
    imm = offset & 0xFFFFFFFF
    return (
        (((imm >> 20) & 1) << 31)
        | (((imm >> 1) & 0x3FF) << 21)
        | (((imm >> 11) & 1) << 20)
        | (((imm >> 12) & 0xFF) << 12)
        | 0x6F
    )


class Mt19937:
    """Legacy MT19937 stream used by NumPy RandomState."""

    def __init__(self, seed: int) -> None:
        self.state = [0] * 624
        self.state[0] = seed & 0xFFFFFFFF
        for i in range(1, 624):
            previous = self.state[i - 1]
            self.state[i] = (
                1812433253 * (previous ^ (previous >> 30)) + i
            ) & 0xFFFFFFFF
        self.index = 624

    def _twist(self) -> None:
        for i in range(624):
            value = (self.state[i] & 0x80000000) | (
                self.state[(i + 1) % 624] & 0x7FFFFFFF
            )
            twisted = value >> 1
            if value & 1:
                twisted ^= 0x9908B0DF
            self.state[i] = self.state[(i + 397) % 624] ^ twisted
        self.index = 0

    def u32(self) -> int:
        if self.index == 624:
            self._twist()
        value = self.state[self.index]
        self.index += 1
        value ^= value >> 11
        value ^= (value << 7) & 0x9D2C5680
        value ^= (value << 15) & 0xEFC60000
        value ^= value >> 18
        return value & 0xFFFFFFFF

    def uniform(self) -> float:
        high = self.u32() >> 5
        low = self.u32() >> 6
        return (high * 67108864.0 + low) / 9007199254740992.0


def read_idx_frame(path: Path, frame_index: int) -> list[int]:
    data = path.read_bytes()
    if len(data) < 16 or data[:4] != b"\x00\x00\x08\x03":
        raise ValueError(f"{path} is not an unsigned-byte three-dimensional IDX file")
    count, rows, columns = struct.unpack_from(">III", data, 4)
    if rows * columns != N_INPUT:
        raise ValueError(f"{path} has {rows}x{columns} images, expected 28x28")
    if not 0 <= frame_index < count:
        raise ValueError(f"frame index {frame_index} is outside [0, {count})")
    start = 16 + frame_index * N_INPUT
    end = start + N_INPUT
    if len(data) != 16 + count * N_INPUT:
        raise ValueError(f"{path} has an inconsistent IDX payload length")
    return list(data[start:end])


def _read_npy(raw: bytes) -> tuple[tuple[int, ...], list[int | float]]:
    if raw[:6] != b"\x93NUMPY":
        raise ValueError("missing NPY magic")
    major, minor = raw[6], raw[7]
    if (major, minor) == (1, 0):
        header_len = struct.unpack_from("<H", raw, 8)[0]
        header_start = 10
    elif major in (2, 3):
        header_len = struct.unpack_from("<I", raw, 8)[0]
        header_start = 12
    else:
        raise ValueError(f"unsupported NPY version {major}.{minor}")
    header = ast.literal_eval(raw[header_start:header_start + header_len].decode("latin1"))
    if header["fortran_order"]:
        raise ValueError("Fortran-order NPY arrays are unsupported")
    shape = tuple(int(value) for value in header["shape"])
    dtype = header["descr"]
    formats = {
        "<f4": ("<f", 4),
        "<f8": ("<d", 8),
        "|f4": ("<f", 4),
        "|u1": ("<B", 1),
        "<u4": ("<I", 4),
        "<i4": ("<i", 4),
        "<i8": ("<q", 8),
    }
    if dtype not in formats:
        raise ValueError(f"unsupported NPY dtype {dtype!r}")
    fmt, width = formats[dtype]
    count = math.prod(shape)
    payload = raw[header_start + header_len:]
    if len(payload) != count * width:
        raise ValueError("NPY payload length does not match its shape")
    return shape, [value[0] for value in struct.iter_unpack(fmt, payload)]


def _npz_array(
    archive: zipfile.ZipFile,
    name: str,
    shape: tuple[int, ...],
) -> list[int | float]:
    filename = f"{name}.npy"
    if filename not in archive.namelist():
        raise ValueError(f"runtime snapshot is missing {filename}")
    actual_shape, values = _read_npy(archive.read(filename))
    if actual_shape != shape:
        raise ValueError(
            f"runtime snapshot {name} has shape {actual_shape}, expected {shape}"
        )
    return values


@dataclass(frozen=True)
class RuntimeSnapshot:
    weights: list[float]
    theta_mv: list[float]
    input_pre_trace: list[float]
    input_rng_state: list[int]
    exc_v: list[float]
    exc_ge: list[float]
    exc_gi: list[float]
    exc_post1: list[float]
    exc_post2: list[float]
    exc_refractory: list[int]
    inh_v: list[float]
    inh_ge: list[float]
    inh_gi: list[float]
    inh_refractory: list[int]
    pixels: list[int]
    placement_scores: list[float]
    input_intensity: float
    metadata: dict[str, Any]


def load_runtime_snapshot(path: Path) -> RuntimeSnapshot:
    with zipfile.ZipFile(path) as archive:
        if "metadata_json.npy" not in archive.namelist():
            raise ValueError("runtime snapshot is missing metadata_json.npy")
        metadata_shape, metadata_raw = _read_npy(archive.read("metadata_json.npy"))
        if len(metadata_shape) != 1:
            raise ValueError("runtime snapshot metadata_json must be one-dimensional")
        metadata = json.loads(bytes(int(value) for value in metadata_raw).decode("ascii"))
        if metadata.get("schema") != "activen-mnist-runtime-snapshot-v1":
            raise ValueError(f"unsupported runtime snapshot schema in {path}")

        def floats(name: str, shape: tuple[int, ...]) -> list[float]:
            values = [float(value) for value in _npz_array(archive, name, shape)]
            if not all(math.isfinite(value) for value in values):
                raise ValueError(f"runtime snapshot {name} contains non-finite values")
            return values

        def integers(name: str, shape: tuple[int, ...]) -> list[int]:
            return [int(value) for value in _npz_array(archive, name, shape)]

        weights = floats("weights", (N_INPUT, N_EXC))
        theta_mv = floats("theta_mv", (N_EXC,))
        input_pre_trace = floats("input_pre_trace", (N_INPUT,))
        input_rng_state = integers("input_rng_state", (N_INPUT,))
        exc_v = floats("exc_v", (N_EXC,))
        exc_ge = floats("exc_ge", (N_EXC,))
        exc_gi = floats("exc_gi", (N_EXC,))
        exc_post1 = floats("exc_post1", (N_EXC,))
        exc_post2 = floats("exc_post2", (N_EXC,))
        exc_refractory = integers("exc_refractory", (N_EXC,))
        inh_v = floats("inh_v", (N_INH,))
        inh_ge = floats("inh_ge", (N_INH,))
        inh_gi = floats("inh_gi", (N_INH,))
        inh_refractory = integers("inh_refractory", (N_INH,))
        pixels = integers("pixels", (N_INPUT,))
        placement_scores = floats("placement_scores", (N_INPUT,))

    if any(state == 0 or state > 0xFFFFFFFF for state in input_rng_state):
        raise ValueError("runtime snapshot input RNG states must be nonzero uint32 values")
    if any(value < 0 for value in exc_refractory + inh_refractory):
        raise ValueError("runtime snapshot refractory counters must be non-negative")
    if any(pixel < 0 or pixel > 255 for pixel in pixels):
        raise ValueError("runtime snapshot pixels must be uint8 values")
    if any(value < 0.0 for value in input_pre_trace + exc_post1 + exc_post2):
        raise ValueError("runtime snapshot traces must be non-negative")
    input_intensity = float(metadata["input_intensity"])
    if not math.isfinite(input_intensity) or input_intensity < 0.0:
        raise ValueError("runtime snapshot input intensity must be finite and non-negative")

    return RuntimeSnapshot(
        weights=weights,
        theta_mv=theta_mv,
        input_pre_trace=input_pre_trace,
        input_rng_state=input_rng_state,
        exc_v=exc_v,
        exc_ge=exc_ge,
        exc_gi=exc_gi,
        exc_post1=exc_post1,
        exc_post2=exc_post2,
        exc_refractory=exc_refractory,
        inh_v=inh_v,
        inh_ge=inh_ge,
        inh_gi=inh_gi,
        inh_refractory=inh_refractory,
        pixels=pixels,
        placement_scores=placement_scores,
        input_intensity=input_intensity,
        metadata=metadata,
    )


def load_checkpoint(path: Path) -> tuple[list[float], list[float] | None]:
    if path.suffix == ".npz":
        with zipfile.ZipFile(path) as archive:
            weight_shape, weights = _read_npy(archive.read("weights.npy"))
            theta = None
            if "theta_mv.npy" in archive.namelist():
                theta_shape, theta = _read_npy(archive.read("theta_mv.npy"))
                if theta_shape != (N_EXC,):
                    raise ValueError(f"unexpected theta shape {theta_shape}")
    else:
        weight_shape, weights = _read_npy(path.read_bytes())
        theta = None
    if weight_shape != (N_INPUT, N_EXC):
        raise ValueError(f"unexpected weight shape {weight_shape}")
    return weights, theta


def initial_weights(seed: int, target: float) -> list[float]:
    rng = Mt19937(seed)
    weights = [f32(0.01 + 0.3 * rng.uniform()) for _ in range(N_INPUT * N_EXC)]
    sums = [0.0] * N_EXC
    for row in range(N_INPUT):
        base = row * N_EXC
        for column in range(N_EXC):
            sums[column] += weights[base + column]
    scales = [target / total for total in sums]
    for row in range(N_INPUT):
        base = row * N_EXC
        for column in range(N_EXC):
            weights[base + column] = f32(weights[base + column] * scales[column])
    return weights


def xorshift32(value: int) -> int:
    value ^= (value << 13) & 0xFFFFFFFF
    value ^= value >> 17
    value ^= (value << 5) & 0xFFFFFFFF
    return value & 0xFFFFFFFF


def initial_rng_state(seed: int, input_index: int) -> int:
    return xorshift32(seed ^ (input_index * 0x9E3779B9)) or 1


def poisson_threshold(pixel: int, intensity: float) -> int:
    rate_hz = f32((pixel / 8.0) * intensity)
    probability = f32(rate_hz * f32(0.0005))
    if probability >= 1.0:
        return 0xFFFFFFFF
    if probability <= 0.0:
        return 0
    return min(int(probability * 4294967296.0), 0xFFFFFFFE)


def poisson_runtime_cutoff(threshold: int) -> int:
    """Encode `state < threshold` as `state <= cutoff` for nonzero RNGs."""
    if not 0 <= threshold <= 0xFFFFFFFF:
        raise ValueError("Poisson threshold must be uint32")
    if threshold == 0 or threshold == 0xFFFFFFFF:
        return threshold
    return threshold - 1


def predict_input_spikes(pixels: list[int], intensity: float,
                         poisson_seed: int) -> list[int]:
    thresholds = [poisson_threshold(pixel, intensity) for pixel in pixels]
    states = [initial_rng_state(poisson_seed, index) for index in range(N_INPUT)]
    return predict_input_spikes_from_states(thresholds, states)


def predict_input_spikes_from_states(
    thresholds: list[int], states: list[int]
) -> list[int]:
    if len(thresholds) != N_INPUT or len(states) != N_INPUT:
        raise ValueError("Poisson prediction requires one threshold and state per input")
    fired = []
    for input_index, (threshold, state) in enumerate(zip(thresholds, states)):
        state = xorshift32(state)
        if threshold == 0xFFFFFFFF or state < threshold:
            fired.append(input_index)
    return fired


def hilbert_d2xy(size: int, distance: int) -> tuple[int, int]:
    row = 0
    column = 0
    scale = 1
    value = distance
    while scale < size:
        rx = 1 if value & 2 else 0
        ry = 1 if (value ^ rx) & 1 else 0
        if ry == 0:
            if rx == 1:
                row = scale - 1 - row
                column = scale - 1 - column
            row, column = column, row
        row += scale * rx
        column += scale * ry
        value >>= 2
        scale <<= 1
    return row, column


def pu_positions(pu_count: int) -> list[tuple[int, int]]:
    if pu_count != 512:
        raise ValueError("grouped MNIST placement currently requires 512 PUs")
    return [hilbert_d2xy(32, distance) for distance in range(pu_count)]


def grouped_pus(
    pu_count: int,
    mc_count: int,
    input_pu_count: int,
    neuron_pu_count: int,
    policy: str,
    neuron_zone_policy: str = "central",
) -> tuple[list[int], list[int], list[int]]:
    if pu_count != 512 or mc_count != 16:
        raise ValueError("grouped MNIST placement requires 512 PUs and 16 MCs")
    if policy not in PLACEMENT_POLICIES:
        raise ValueError(f"unknown placement policy {policy!r}")
    if neuron_zone_policy not in NEURON_ZONE_ORDERS:
        raise ValueError(f"unknown neuron zone policy {neuron_zone_policy!r}")
    if input_pu_count <= 0 or neuron_pu_count <= 0:
        raise ValueError("input and neuron PU counts must be positive")
    if input_pu_count > N_INPUT or neuron_pu_count > N_EXC:
        raise ValueError("each selected population PU must own at least one neuron")
    if input_pu_count + neuron_pu_count > pu_count:
        raise ValueError("input and neuron PUs must be disjoint")

    zone_size = pu_count // mc_count
    if neuron_pu_count % zone_size != 0:
        raise ValueError(
            f"neuron PU count must be a multiple of the {zone_size}-PU MC zone"
        )
    zone_count = neuron_pu_count // zone_size
    neuron_zones = list(NEURON_ZONE_ORDERS[neuron_zone_policy][:zone_count])
    neuron_pus = sorted(
        pu
        for zone in neuron_zones
        for pu in range(zone * zone_size, (zone + 1) * zone_size)
    )
    available = sorted(set(range(pu_count)) - set(neuron_pus))
    positions = pu_positions(pu_count)

    def distance(left: int, right: int) -> int:
        left_row, left_column = positions[left]
        right_row, right_column = positions[right]
        return abs(left_row - right_row) + abs(left_column - right_column)

    distance_sum = {
        pu: sum(distance(pu, neuron_pu) for neuron_pu in neuron_pus)
        for pu in available
    }
    if policy == "edge":
        def boundary_depth(pu: int) -> int:
            row, column = positions[pu]
            return min(row, 15 - row, column, 31 - column)

        ordered = sorted(
            available,
            key=lambda pu: (boundary_depth(pu), distance_sum[pu], pu),
        )
        input_pus = ordered[:input_pu_count]
    elif policy == "near":
        input_pus = sorted(
            available,
            key=lambda pu: (distance_sum[pu], pu),
        )[:input_pu_count]
    else:
        remaining_zones = [
            zone for zone in range(mc_count) if zone not in neuron_zones
        ]
        by_zone = {
            zone: sorted(
                range(zone * zone_size, (zone + 1) * zone_size),
                key=lambda pu: (distance_sum[pu], pu),
            )
            for zone in remaining_zones
        }
        input_pus = []
        level = 0
        while len(input_pus) < input_pu_count:
            for zone in remaining_zones:
                if len(input_pus) == input_pu_count:
                    break
                input_pus.append(by_zone[zone][level])
            level += 1

    return sorted(input_pus), neuron_pus, neuron_zones


def placement(
    count: int,
    pu_count: int,
    selected_pus: Iterable[int] | None = None,
) -> tuple[list[list[int]], list[tuple[int, int]]]:
    pus = list(range(pu_count)) if selected_pus is None else list(selected_pus)
    if not pus:
        raise ValueError("placement requires at least one PU")
    if len(set(pus)) != len(pus) or any(pu < 0 or pu >= pu_count for pu in pus):
        raise ValueError("placement PU list contains an invalid or duplicate PU")
    per_pu = [[] for _ in range(pu_count)]
    locations: list[tuple[int, int]] = []
    base, remainder = divmod(count, len(pus))
    index = 0
    for rank, pu in enumerate(pus):
        for local in range(base + (rank < remainder)):
            per_pu[pu].append(index)
            locations.append((pu, local))
            index += 1
    if index != count:
        raise AssertionError("placement produced the wrong population size")
    return per_pu, locations


def compact_neuron_pu_order(
    pus: Iterable[int], neuron_count: int = N_EXC,
) -> list[int]:
    """Spread two-neuron owners physically, then group them in stream order."""
    ordered = list(pus)
    double_count = neuron_count - len(ordered)
    if not 0 <= double_count <= len(ordered):
        raise ValueError("compact layout requires one or two excitatory neurons per PU")
    if double_count == 0:
        return ordered
    double_indices = {
        rank * len(ordered) // double_count for rank in range(double_count)
    }
    doubles = [pu for rank, pu in enumerate(ordered) if rank in double_indices]
    singles = [pu for rank, pu in enumerate(ordered) if rank not in double_indices]
    if len(doubles) != double_count:
        raise AssertionError("compact layout selected the wrong number of double owners")
    return doubles + singles


def interleaved_zone_groups(
    pus: Iterable[int], group_count: int,
) -> list[list[int]]:
    """Split complete 32-PU MC zones into spatially interleaved groups."""
    if group_count <= 0 or 32 % group_count:
        raise ValueError("horizontal group count must divide a 32-PU MC zone")
    groups: list[list[int]] = [[] for _ in range(group_count)]
    by_zone: dict[int, list[int]] = {}
    for pu in sorted(pus):
        by_zone.setdefault(pu // 32, []).append(pu)
    for zone, members in sorted(by_zone.items()):
        if len(members) != 32 or members != list(range(zone * 32, (zone + 1) * 32)):
            raise ValueError("horizontal partitioning requires complete 32-PU zones")
        for group, owners in enumerate(groups):
            owners.extend(members[group::group_count])
    return groups


def interleaved_zone_halves(pus: Iterable[int]) -> tuple[list[int], list[int]]:
    """Split complete 32-PU MC zones into spatially interleaved 16-PU halves."""
    group0, group1 = interleaved_zone_groups(pus, 2)
    return group0, group1


@dataclass(frozen=True)
class InputCopy:
    input_index: int
    group_mask: int
    primary: bool
    replicated: bool


def assign_input_copies(
    indices: Iterable[int],
    scores: list[float],
    pus: Iterable[int],
    *,
    pu_count: int,
    group_mask: int,
    primary: bool,
    replicated: bool,
    rate_balanced: bool,
) -> list[list[InputCopy]]:
    """Assign a logical subset with fixed per-PU capacities."""
    selected = list(indices)
    owners = list(pus)
    if not owners:
        raise ValueError("input-copy assignment requires at least one PU")
    base, remainder = divmod(len(selected), len(owners))
    capacities = {
        pu: base + (rank < remainder) for rank, pu in enumerate(owners)
    }
    assigned: list[list[InputCopy]] = [[] for _ in range(pu_count)]
    if rate_balanced:
        heap = [
            (0.0, rank, pu) for rank, pu in enumerate(owners) if capacities[pu]
        ]
        heapq.heapify(heap)
        order = sorted(selected, key=lambda index: (-scores[index], index))
        for input_index in order:
            load, rank, pu = heapq.heappop(heap)
            assigned[pu].append(InputCopy(
                input_index, group_mask, primary, replicated,
            ))
            capacities[pu] -= 1
            if capacities[pu]:
                heapq.heappush(heap, (load + scores[input_index], rank, pu))
    else:
        for position, input_index in enumerate(sorted(selected)):
            pu = owners[position % len(owners)]
            assigned[pu].append(InputCopy(
                input_index, group_mask, primary, replicated,
            ))
    for pu in owners:
        assigned[pu].sort(key=lambda copy: copy.input_index)
    if sum(map(len, assigned)) != len(selected):
        raise AssertionError("input-copy assignment lost a logical input")
    return assigned


def merge_copy_assignments(
    destination: list[list[InputCopy]], source: list[list[InputCopy]],
) -> None:
    for pu, copies in enumerate(source):
        if copies and destination[pu]:
            raise AssertionError("an input PU was assigned two ownership roles")
        destination[pu].extend(copies)


def horizontal_input_placement(
    scores: list[float],
    input_pus: list[int],
    pu_count: int,
    mode: str,
    replica_stride: int = HORIZONTAL_REPLICA_STRIDE,
) -> tuple[list[list[InputCopy]], dict[int, int], list[list[int]]]:
    """Build physical input records and homogeneous per-PU target ownership."""
    if not input_pus:
        raise ValueError("horizontal partitioning requires input PUs")
    assigned: list[list[InputCopy]] = [[] for _ in range(pu_count)]
    if mode in FULL_HORIZONTAL_GROUP_COUNTS:
        group_count = FULL_HORIZONTAL_GROUP_COUNTS[mode]
        owner_groups = interleaved_zone_groups(input_pus, group_count)
        roles: dict[int, int] = {}
        for group, owners in enumerate(owner_groups):
            group_mask = 1 << group
            owners_per_zone = 32 // group_count
            assignment_owners = []
            for start in range(0, len(owners), owners_per_zone):
                zone_owners = owners[start:start + owners_per_zone]
                shift = group * replica_stride % owners_per_zone
                assignment_owners.extend(zone_owners[shift:] + zone_owners[:shift])
            merge_copy_assignments(assigned, assign_input_copies(
                range(N_INPUT), scores, assignment_owners,
                pu_count=pu_count, group_mask=group_mask,
                primary=group == 0, replicated=True, rate_balanced=False,
            ))
            roles.update({pu: group_mask for pu in owners})
        return assigned, roles, owner_groups

    if mode != "hot-partial":
        raise ValueError(f"unknown horizontal partition mode {mode!r}")
    # Start from three spatial classes, then move four members from each hot
    # class into the cold/full class to obtain 92 + 92 + 104 PUs.
    ordered = sorted(input_pus)
    hot0 = [pu for rank, pu in enumerate(ordered) if rank % 3 == 0]
    hot1 = [pu for rank, pu in enumerate(ordered) if rank % 3 == 1]
    cold = [pu for rank, pu in enumerate(ordered) if rank % 3 == 2]
    cold.extend(hot0[-4:])
    cold.extend(hot1[-4:])
    hot0 = hot0[:-4]
    hot1 = hot1[:-4]
    if (len(hot0), len(hot1), len(cold)) != (92, 92, 104):
        raise AssertionError("partial horizontal PU split has the wrong shape")
    hottest = sorted(range(N_INPUT), key=lambda index: (-scores[index], index))[:368]
    hot_set = set(hottest)
    cold_inputs = [index for index in range(N_INPUT) if index not in hot_set]
    merge_copy_assignments(assigned, assign_input_copies(
        hottest, scores, hot0,
        pu_count=pu_count, group_mask=INPUT_GROUP_0,
        primary=True, replicated=True, rate_balanced=True,
    ))
    merge_copy_assignments(assigned, assign_input_copies(
        hottest, scores, hot1,
        pu_count=pu_count, group_mask=INPUT_GROUP_1,
        primary=False, replicated=True, rate_balanced=True,
    ))
    merge_copy_assignments(assigned, assign_input_copies(
        cold_inputs, scores, cold,
        pu_count=pu_count, group_mask=INPUT_GROUP_BOTH,
        primary=True, replicated=False, rate_balanced=True,
    ))
    roles = {
        **{pu: INPUT_GROUP_0 for pu in hot0},
        **{pu: INPUT_GROUP_1 for pu in hot1},
        **{pu: INPUT_GROUP_BOTH for pu in cold},
    }
    return assigned, roles, [hot0 + cold, hot1 + cold]


def horizontal_neuron_placement(
    neuron_pus: list[int], pu_count: int, group_count: int = 2,
) -> tuple[
    list[list[int]], list[tuple[int, int]], list[list[int]], dict[int, int]
]:
    """Place equal E/I groups across spatially interleaved neuron-zone PUs."""
    if N_EXC % group_count:
        raise ValueError("excitatory count must divide evenly across groups")
    neurons_per_group = N_EXC // group_count
    raw_groups = interleaved_zone_groups(neuron_pus, group_count)
    ordered_groups = [
        compact_neuron_pu_order(group, neurons_per_group) for group in raw_groups
    ]
    per_pu = [[] for _ in range(pu_count)]
    locations: list[tuple[int, int]] = [(-1, -1)] * N_EXC
    for group_index, owners in enumerate(ordered_groups):
        base, remainder = divmod(neurons_per_group, len(owners))
        logical = group_index * neurons_per_group
        for rank, pu in enumerate(owners):
            for local in range(base + (rank < remainder)):
                per_pu[pu].append(logical)
                locations[logical] = (pu, local)
                logical += 1
        if logical != (group_index + 1) * neurons_per_group:
            raise AssertionError("horizontal neuron placement has the wrong size")
    groups = {pu: group for group, owners in enumerate(ordered_groups) for pu in owners}
    return per_pu, locations, ordered_groups, groups


def input_placement(
    thresholds: list[int],
    pu_count: int,
    selected_pus: Iterable[int],
    assignment: str,
) -> tuple[list[list[int]], list[tuple[int, int]]]:
    pus = list(selected_pus)
    if assignment == "contiguous":
        return placement(len(thresholds), pu_count, pus)
    if assignment != "rate-balanced":
        raise ValueError(f"unknown input assignment {assignment!r}")
    if not pus:
        raise ValueError("input placement requires at least one PU")

    base, remainder = divmod(len(thresholds), len(pus))
    remaining = {
        pu: base + (order < remainder) for order, pu in enumerate(pus)
    }
    heap = [(0, order, pu) for order, pu in enumerate(pus) if remaining[pu]]
    heapq.heapify(heap)
    assigned = [[] for _ in range(pu_count)]
    for input_index in sorted(
        range(len(thresholds)), key=lambda index: (-thresholds[index], index)
    ):
        load, order, pu = heapq.heappop(heap)
        assigned[pu].append(input_index)
        remaining[pu] -= 1
        if remaining[pu] != 0:
            heapq.heappush(heap, (load + thresholds[input_index], order, pu))

    locations: list[tuple[int, int]] = [(-1, -1)] * len(thresholds)
    for pu in pus:
        assigned[pu].sort()
        for local, input_index in enumerate(assigned[pu]):
            locations[input_index] = (pu, local)
    if any(pu < 0 for pu, _ in locations):
        raise AssertionError("input placement left an input unassigned")
    return assigned, locations


@dataclass
class CsrRecord:
    starts: list[int]
    lines: list[int]


def append_csr_row(block: bytearray, row: bytes, line_bytes: int) -> tuple[int, int]:
    start = len(block)
    block.extend(row)
    align(block, line_bytes)
    return start, (len(block) - start) // line_bytes


def make_recurrent_csr(
    pu_count: int,
    mc_count: int,
    line_bytes: int,
    exc_locations: list[tuple[int, int]],
    recurrent_mode: str,
) -> tuple[list[bytearray], dict[tuple[str, int], CsrRecord]]:
    pus_per_mc = pu_count // mc_count
    blocks = [bytearray() for _ in range(mc_count)]
    records: dict[tuple[str, int], CsrRecord] = {}

    def pack_target(location: tuple[int, int], weight: float) -> bytes:
        pu, local = location
        return struct.pack("<HHf", pu + 1, local, f32(weight))

    for source in range(N_INH):
        if recurrent_mode != "scatter":
            records[("i", source)] = CsrRecord(
                [0] * mc_count, [0] * mc_count
            )
            continue
        rows = [bytearray() for _ in range(mc_count)]
        for target_index, target in enumerate(exc_locations):
            if target_index != source:
                rows[target[0] // pus_per_mc].extend(pack_target(target, 25.5))
        starts, lines = [], []
        for mc, row in enumerate(rows):
            if row:
                start, count = append_csr_row(blocks[mc], row, line_bytes)
            else:
                start, count = len(blocks[mc]), 0
            starts.append(start)
            lines.append(count)
        records[("i", source)] = CsrRecord(starts, lines)
    return blocks, records


def pack_spm_header(
    spm: bytearray,
    *,
    rule: int,
    pu: int,
    pu_count: int,
    mc_count: int,
    line_shift: int,
    input_count: int,
    input_offset: int,
    exc_count: int,
    exc_offset: int,
    inh_count: int,
    inh_offset: int,
    weight_count: int,
    weight_offset: int,
    frame_starts: list[int],
    frame_lines: list[int],
    weight_max: float,
    potentiation_rate: float,
    warmup_steps: int,
    timed_steps: int,
    input_pu_count: int,
    neuron_pu_count: int,
    input_dest_offset: int,
    exc_target_offset: int,
    recurrent_mode: int,
    feedforward_start_rank: int,
    fired_vector_offset: int,
    reduce_expected: int,
    observed_vector_offset: int,
    physical_input_pu_count: int,
    target_count_per_segment: int,
    posts_per_segment: int,
    target_segment_count: int,
    input_group_mask: int,
    first_weight_slot: int,
    neuron_group: int,
    owned_post_count: int,
) -> None:
    values = [
        MAGIC, rule, pu + 1, pu_count, mc_count, line_shift,
        input_count, input_offset, exc_count, exc_offset,
        inh_count, inh_offset, weight_count, weight_offset,
    ]
    struct.pack_into("<14I", spm, 0, *values)
    struct.pack_into("<16I", spm, 56, *(frame_starts + [0] * (MAX_MC - mc_count)))
    struct.pack_into("<16I", spm, 120, *(frame_lines + [0] * (MAX_MC - mc_count)))
    reduce_parent = 0 if pu == 0 else pu - (pu & -pu) + 1
    struct.pack_into(
        "<4fI", spm, 236,
        weight_max, potentiation_rate, 0.0001, 0.4, reduce_parent,
    )
    struct.pack_into("<2I", spm, 256, warmup_steps, timed_steps)
    struct.pack_into(
        "<7I", spm, 268,
        input_pu_count, neuron_pu_count, input_dest_offset,
        exc_target_offset, recurrent_mode, 0, feedforward_start_rank,
    )
    struct.pack_into(
        "<4I", spm, 296,
        fired_vector_offset, reduce_expected, observed_vector_offset, 0,
    )
    struct.pack_into(
        "<9I", spm, 312,
        physical_input_pu_count, target_count_per_segment,
        posts_per_segment, target_segment_count, input_group_mask,
        first_weight_slot, neuron_group, owned_post_count, 0,
    )
    # Runtime-only phase-local SMT join value and reduction words.
    struct.pack_into("<3I", spm, 348, 0, 0, 0)
    # Validated absolute fired-vector bounds, published once per phase.
    struct.pack_into("<2I", spm, 360, 0, 0)


def make_images(args: argparse.Namespace) -> tuple[list[bytes], dict]:
    line_bytes = args.mem_line_width // 8
    line_shift = line_bytes.bit_length() - 1
    payload = args.payload.read_bytes()
    weight_max = 1.0
    potentiation_rate = 0.01 if args.rule == "three-trace" else 0.0005
    snapshot_path = getattr(args, "runtime_snapshot", None)
    snapshot = load_runtime_snapshot(snapshot_path) if snapshot_path else None
    if snapshot is not None:
        weights = [f32(value) for value in snapshot.weights]
        theta = snapshot.theta_mv
    elif args.checkpoint:
        checkpoint_weights, theta = load_checkpoint(args.checkpoint)
        weights = [f32(value) for value in checkpoint_weights]
    else:
        weights = initial_weights(args.weight_seed, 78.0)
        theta = None

    horizontal_partition = getattr(args, "horizontal_partition", "none")
    horizontal_replica_stride = getattr(
        args, "horizontal_replica_stride", HORIZONTAL_REPLICA_STRIDE
    )
    feedforward_destination_stride = getattr(
        args, "feedforward_destination_stride", FEEDFORWARD_DESTINATION_STRIDE
    )
    if horizontal_replica_stride < 0:
        raise ValueError("horizontal replica stride must be nonnegative")
    if feedforward_destination_stride < 0:
        raise ValueError("feedforward destination stride must be nonnegative")
    if horizontal_partition not in HORIZONTAL_PARTITIONS:
        raise ValueError(f"unknown horizontal partition mode {horizontal_partition!r}")
    if horizontal_partition != "none" and args.trace_bundle:
        raise ValueError("horizontal partitioning currently requires unbundled traces")
    physical_input_pu_count = (
        args.pu_count - args.neuron_pus
        if horizontal_partition != "none" else args.input_pus
    )
    input_pus, neuron_pus, neuron_zones = grouped_pus(
        args.pu_count, args.mc_count, physical_input_pu_count, args.neuron_pus,
        args.placement_policy, args.neuron_zone_policy,
    )
    if snapshot is not None:
        pixels = snapshot.pixels
        input_intensity = snapshot.input_intensity
    elif args.frame:
        pixels = read_idx_frame(args.frame, args.frame_index)
        input_intensity = args.input_intensity
    else:
        pixels = [((index * 73 + 19) ^ (index >> 2)) & 0xFF for index in range(N_INPUT)]
        input_intensity = args.input_intensity
    thresholds = [
        poisson_threshold(pixel, input_intensity) for pixel in pixels
    ]
    placement_scores = (
        snapshot.placement_scores if snapshot is not None else thresholds
    )
    if horizontal_partition == "none":
        horizontal_group_count = 1
        neuron_pus = compact_neuron_pu_order(neuron_pus)
        exc_per_pu, exc_locations = placement(N_EXC, args.pu_count, neuron_pus)
        neuron_groups = [neuron_pus]
        neuron_group_by_pu = {pu: 0 for pu in neuron_pus}
        logical_per_pu, input_locations = input_placement(
            placement_scores, args.pu_count, input_pus, args.input_assignment,
        )
        input_per_pu: list[list[InputCopy]] = [
            [InputCopy(index, INPUT_GROUP_BOTH, True, False) for index in indices]
            for indices in logical_per_pu
        ]
        input_group_by_pu = {pu: INPUT_GROUP_BOTH for pu in input_pus}
        trace_input_groups = [input_pus, input_pus]
    else:
        horizontal_group_count = FULL_HORIZONTAL_GROUP_COUNTS.get(
            horizontal_partition, 2
        )
        exc_per_pu, exc_locations, neuron_groups, neuron_group_by_pu = (
            horizontal_neuron_placement(
                neuron_pus, args.pu_count, horizontal_group_count,
            )
        )
        neuron_pus = [pu for group in neuron_groups for pu in group]
        input_per_pu, input_group_by_pu, trace_input_groups = (
            horizontal_input_placement(
                placement_scores, input_pus, args.pu_count, horizontal_partition,
                horizontal_replica_stride,
            )
        )
        input_locations = [(-1, -1)] * N_INPUT
        for pu, copies in enumerate(input_per_pu):
            for local, copy in enumerate(copies):
                if copy.primary:
                    input_locations[copy.input_index] = (pu, local)
        if any(pu < 0 for pu, _ in input_locations):
            raise AssertionError("horizontal placement omitted a primary input copy")
    inh_per_pu = [members.copy() for members in exc_per_pu]
    inh_locations = exc_locations.copy()
    if exc_locations != inh_locations:
        raise AssertionError("corresponding excitatory and inhibitory neurons split")
    if set(input_pus) & set(neuron_pus):
        raise AssertionError("input and neuron PU roles overlap")
    trace_destination_stride = args.trace_destination_stride
    if trace_destination_stride < 0 or any(not group for group in trace_input_groups):
        raise ValueError("trace destination stride and groups must be nonnegative/nonempty")
    neuron_pu_ranks = {pu: rank for rank, pu in enumerate(neuron_pus)}
    neuron_group_ranks = {
        pu: rank for group in neuron_groups for rank, pu in enumerate(group)
    }
    input_role_ranks = {
        pu: rank
        for role in sorted(set(input_group_by_pu.values()))
        for rank, pu in enumerate(
            sorted(owner for owner, owner_role in input_group_by_pu.items()
                   if owner_role == role)
        )
    }
    exc_by_pu: dict[int, list[tuple[int, int]]] = {
        pu: [] for pu in neuron_pus
    }
    for column, (target_pu, target_local) in enumerate(exc_locations):
        if target_local > 1:
            raise ValueError(
                f"PU {target_pu + 1} has unsupported local excitatory "
                f"index {target_local}"
            )
        exc_by_pu[target_pu].append((column, target_local))
    compact_weight_rows = horizontal_partition in ("full-4", "full-8", "full-16")
    weight_slots_per_row = N_EXC // horizontal_group_count if compact_weight_rows else N_EXC
    exc_weight_slots = [TRACE_SLOT_NONE] * N_EXC
    exc_target_groups: list[list[int]] = []
    next_weight_slot = 0
    for target_group in neuron_groups:
        targets: list[int] = []
        for target_pu in target_group:
            members = exc_by_pu[target_pu]
            if not 1 <= len(members) <= 2:
                raise ValueError(
                    f"PU {target_pu + 1} owns {len(members)} excitatory neurons"
                )
            column0, local0 = members[0]
            if local0 != 0:
                raise AssertionError("first excitatory neuron is not local slot zero")
            exc_weight_slots[column0] = (
                next_weight_slot % weight_slots_per_row
                if compact_weight_rows else next_weight_slot
            )
            next_weight_slot += 1
            if len(members) == 2:
                column1, local1 = members[1]
                if local1 != 1:
                    raise AssertionError("second excitatory neuron is not local slot one")
                exc_weight_slots[column1] = (
                    next_weight_slot % weight_slots_per_row
                    if compact_weight_rows else next_weight_slot
                )
                next_weight_slot += 1
            else:
                local1 = 0xFF
            payload_metadata = local0 | (local1 << 8)
            targets.append((payload_metadata << 16) | (target_pu + 1))
        exc_target_groups.append(targets)
    if TRACE_SLOT_NONE in exc_weight_slots:
        raise ValueError("an excitatory neuron occupies the reserved trace sentinel slot")
    if next_weight_slot != N_EXC:
        raise AssertionError("compact weight stream does not contain every column")
    csr, csr_records = make_recurrent_csr(
        args.pu_count, args.mc_count, line_bytes, exc_locations,
        args.recurrent_mode,
    )

    prefixes = [bytearray(DRAM_HEADER_SIZE) for _ in range(args.mc_count)]
    prefixes[0].extend(payload)
    for prefix in prefixes:
        align(prefix, line_bytes)
    csr_bases = [len(prefix) for prefix in prefixes]
    images = [prefix + block for prefix, block in zip(prefixes, csr)]

    frame_rows = [bytearray() for _ in range(args.mc_count)]
    pus_per_mc = args.pu_count // args.mc_count
    for pu, copies in enumerate(input_per_pu):
        for local, copy in enumerate(copies):
            frame_rows[pu // pus_per_mc].extend(struct.pack(
                "<HHI", pu + 1, local,
                poisson_runtime_cutoff(thresholds[copy.input_index]),
            ))

    frame_starts, frame_lines = [], []
    for mc in range(args.mc_count):
        align(images[mc], line_bytes)
        frame_starts.append(len(images[mc]))
        images[mc].extend(frame_rows[mc])
        align(images[mc], line_bytes)
        frame_lines.append((len(images[mc]) - frame_starts[mc]) // line_bytes)

    spm_images: list[bytes] = []
    usage: list[dict[str, int]] = []
    for pu in range(args.pu_count):
        input_group_mask = input_group_by_pu.get(pu, 0)
        if horizontal_partition == "none":
            local_target_groups = exc_target_groups if input_per_pu[pu] else []
            posts_per_segment = N_EXC
            first_group = 0
            first_weight_slot = 0
        else:
            local_target_groups = [
                exc_target_groups[group]
                for group in range(horizontal_group_count)
                if input_group_mask & (1 << group)
            ]
            posts_per_segment = N_EXC // horizontal_group_count
            first_group = (
                (input_group_mask & -input_group_mask).bit_length() - 1
                if input_per_pu[pu] else 0
            )
            first_weight_slot = 0 if compact_weight_rows else first_group * posts_per_segment
        local_exc_targets = [
            target for group in local_target_groups for target in group
        ]
        target_count_per_segment = (
            len(local_target_groups[0]) if local_target_groups else 0
        )
        target_segment_count = len(local_target_groups)
        owned_post_count = posts_per_segment * target_segment_count
        # The field identifies the owned column group on both sides.  Compact
        # layouts use it to select their packed firing-count field directly.
        neuron_group = neuron_group_by_pu.get(pu, first_group)
        trace_destinations = (
            trace_input_groups[neuron_group] if exc_per_pu[pu] else []
        )
        input_offset = SPM_HEADER_SIZE
        exc_offset = input_offset + len(input_per_pu[pu]) * INPUT_RECORD_SIZE
        inh_offset = exc_offset + len(exc_per_pu[pu]) * EXC_RECORD_SIZE
        weight_offset = inh_offset + len(inh_per_pu[pu]) * INH_RECORD_SIZE
        local_weight_count = len(input_per_pu[pu]) * weight_slots_per_row
        weight_end = weight_offset + local_weight_count * WEIGHT_RECORD_SIZE
        exc_target_offset = align_up(weight_end, 2) if input_per_pu[pu] else 0
        after_exc_targets = (
            exc_target_offset + len(local_exc_targets) * EXC_TARGET_RECORD_SIZE
            if input_per_pu[pu] else weight_end
        )
        fired_vector_offset = (
            align_up(after_exc_targets, 4) if input_per_pu[pu] else 0
        )
        after_fired_vector = (
            fired_vector_offset
            + len(input_per_pu[pu]) * FIRED_VECTOR_RECORD_SIZE
            if input_per_pu[pu] else after_exc_targets
        )
        input_dest_offset = (
            align_up(after_fired_vector, 2) if exc_per_pu[pu] else 0
        )
        after_input_dest = (
            input_dest_offset + len(trace_destinations) * 2
            if exc_per_pu[pu] else after_fired_vector
        )
        # Feedforward senders mark exactly one message per active source PU, so
        # the single-threaded receiver can append source IDs without a bitset.
        observed_mask_bytes = 0
        observed_vector_offset = (
            align_up(after_input_dest, 2)
            if exc_per_pu[pu] else 0
        )
        required = (
            observed_vector_offset + len(input_pus) * 2
            if exc_per_pu[pu] else after_input_dest
        )
        use_root_lut = getattr(args, "fifth_root_seed", "bit") == "lut"
        if use_root_lut and args.rule != "two-trace":
            raise ValueError("the fifth-root LUT is specific to two-trace training")
        root_lut_bytes = FIFTH_ROOT_LUT_BYTES if use_root_lut and input_per_pu[pu] else 0
        if root_lut_bytes:
            if required > FIFTH_ROOT_LUT_OFFSET:
                raise ValueError(f"PU {pu + 1} data overlaps the fifth-root LUT")
            required = FIFTH_ROOT_LUT_OFFSET + root_lut_bytes
        if required > STACK0_BASE:
            raise ValueError(
                f"PU {pu + 1} layout ends at {required}, overlapping the first "
                f"SMT stack at {STACK0_BASE}"
            )
        spm = bytearray(SPM_SIZE)
        if root_lut_bytes:
            spm[FIFTH_ROOT_LUT_OFFSET:required] = fifth_root_lut()
        feedforward_start_rank = (
            input_role_ranks[pu] * feedforward_destination_stride
        ) % target_count_per_segment if input_per_pu[pu] else 0
        pack_spm_header(
            spm, rule=RULES[args.rule], pu=pu, pu_count=args.pu_count,
            mc_count=args.mc_count, line_shift=line_shift,
            input_count=len(input_per_pu[pu]), input_offset=input_offset,
            exc_count=len(exc_per_pu[pu]), exc_offset=exc_offset,
            inh_count=len(inh_per_pu[pu]), inh_offset=inh_offset,
            weight_count=local_weight_count, weight_offset=weight_offset,
            frame_starts=frame_starts, frame_lines=frame_lines,
            weight_max=weight_max, potentiation_rate=potentiation_rate,
            warmup_steps=args.warmup_steps, timed_steps=args.timed_steps,
            input_pu_count=(
                len(trace_destinations) if trace_destinations else len(input_pus)
            ),
            neuron_pu_count=len(neuron_pus),
            input_dest_offset=input_dest_offset,
            exc_target_offset=exc_target_offset,
            recurrent_mode=RECURRENT_MODES[args.recurrent_mode],
            feedforward_start_rank=feedforward_start_rank,
            fired_vector_offset=fired_vector_offset,
            reduce_expected=(args.pu_count if pu == 0 else pu & -pu),
            observed_vector_offset=observed_vector_offset,
            physical_input_pu_count=len(input_pus),
            target_count_per_segment=target_count_per_segment,
            posts_per_segment=posts_per_segment if input_per_pu[pu] else 0,
            target_segment_count=target_segment_count,
            input_group_mask=input_group_mask,
            first_weight_slot=first_weight_slot,
            neuron_group=neuron_group,
            owned_post_count=owned_post_count,
        )

        weight_cursor = weight_offset
        for local, copy in enumerate(input_per_pu[pu]):
            input_index = copy.input_index
            row = weights[input_index * N_EXC:(input_index + 1) * N_EXC]
            slotted_row = [0.0] * weight_slots_per_row
            for column, weight in enumerate(row):
                column_group = column // (N_EXC // horizontal_group_count)
                if (horizontal_partition == "none"
                        or copy.group_mask & (1 << column_group)):
                    slotted_row[exc_weight_slots[column]] = weight
            rng_state = (
                snapshot.input_rng_state[input_index]
                if snapshot is not None
                else initial_rng_state(args.poisson_seed, input_index)
            )
            pre_trace = (
                snapshot.input_pre_trace[input_index]
                if snapshot is not None
                else f32((input_index % 29) / 31.0)
            )
            struct.pack_into(
                "<IIfIIII", spm, input_offset + local * INPUT_RECORD_SIZE,
                input_index
                | (INPUT_GLOBAL_PRIMARY if copy.primary else 0)
                | (INPUT_GLOBAL_REPLICATED if copy.replicated else 0),
                0, f32(pre_trace), rng_state, 0,
                weight_cursor, weight_slots_per_row,
            )
            for weight in slotted_row:
                struct.pack_into("<f", spm, weight_cursor, weight)
                weight_cursor += WEIGHT_RECORD_SIZE

        for local, exc_index in enumerate(exc_per_pu[pu]):
            offset = exc_offset + local * EXC_RECORD_SIZE
            voltage = f32(
                snapshot.exc_v[exc_index] if snapshot is not None else -105.0
            )
            theta_value = f32(theta[exc_index] if theta is not None else 20.0)
            ge = f32(snapshot.exc_ge[exc_index]) if snapshot is not None else 0.0
            gi = f32(snapshot.exc_gi[exc_index]) if snapshot is not None else 0.0
            post1 = (
                f32(snapshot.exc_post1[exc_index])
                if snapshot is not None
                else f32((exc_index % 23) / 25.0)
            )
            post2 = (
                f32(snapshot.exc_post2[exc_index])
                if snapshot is not None
                else f32((exc_index % 31) / 33.0)
            )
            refractory = (
                snapshot.exc_refractory[exc_index] if snapshot is not None else 0
            )
            struct.pack_into(
                "<6fIII", spm, offset,
                ge, gi, voltage, theta_value, post1, post2,
                refractory, 0, exc_weight_slots[exc_index],
            )

        for local, inh_index in enumerate(inh_per_pu[pu]):
            record = csr_records[("i", inh_index)]
            offset = inh_offset + local * INH_RECORD_SIZE
            ge = f32(snapshot.inh_ge[inh_index]) if snapshot is not None else 0.0
            gi = f32(snapshot.inh_gi[inh_index]) if snapshot is not None else 0.0
            voltage = (
                f32(snapshot.inh_v[inh_index]) if snapshot is not None else -100.0
            )
            refractory = (
                snapshot.inh_refractory[inh_index] if snapshot is not None else 0
            )
            struct.pack_into(
                "<3fII", spm, offset, ge, gi, voltage, refractory, 0
            )
            starts = [csr_bases[mc] + record.starts[mc] for mc in range(args.mc_count)]
            struct.pack_into("<16I", spm, offset + 20, *(starts + [0] * (MAX_MC - args.mc_count)))
            struct.pack_into("<16I", spm, offset + 84, *(record.lines + [0] * (MAX_MC - args.mc_count)))

        assert weight_cursor == weight_end
        if input_per_pu[pu]:
            for index, target in enumerate(local_exc_targets):
                struct.pack_into(
                    "<I", spm,
                    exc_target_offset + index * EXC_TARGET_RECORD_SIZE,
                    target,
                )
        if exc_per_pu[pu]:
            rotation = (
                neuron_group_ranks[pu] * trace_destination_stride
            ) % len(trace_destinations)
            destinations = (
                trace_destinations[rotation:] + trace_destinations[:rotation]
            )
            for index, destination_pu in enumerate(destinations):
                struct.pack_into(
                    "<H", spm, input_dest_offset + index * 2,
                    destination_pu + 1,
                )
        # Keep the legacy tail word initialized for compatibility with SPM tools.
        # The hybrid layout itself is identified and validated through its header.
        struct.pack_into("<I", spm, SPM_SIZE - 8, 1)
        spm_images.append(bytes(spm))
        usage.append({
            "pu": pu + 1,
            "inputs": len(input_per_pu[pu]),
            "excitatory": len(exc_per_pu[pu]),
            "inhibitory": len(inh_per_pu[pu]),
            "weights": local_weight_count,
            "exc_target_bytes": (
                len(local_exc_targets) * EXC_TARGET_RECORD_SIZE
                if input_per_pu[pu] else 0
            ),
            "input_dest_bytes": (
                len(trace_destinations) * 2 if exc_per_pu[pu] else 0
            ),
            "fired_vector_bytes": (
                len(input_per_pu[pu]) * FIRED_VECTOR_RECORD_SIZE
                if input_per_pu[pu] else 0
            ),
            "observed_mask_bytes": observed_mask_bytes if exc_per_pu[pu] else 0,
            "observed_vector_bytes": (
                len(input_pus) * 2 if exc_per_pu[pu] else 0
            ),
            "bytes": required,
            "fifth_root_lut_bytes": root_lut_bytes,
            "free_bytes": SPM_SIZE - required,
            "input_group_mask": input_group_mask,
            "neuron_group": neuron_group if exc_per_pu[pu] else None,
            "owned_posts": owned_post_count if input_per_pu[pu] else 0,
        })

    align(images[0], line_bytes)
    spm_init_offset = len(images[0])
    descriptor_bytes = args.pu_count * SPM_DESCRIPTOR_SIZE
    data_start = descriptor_bytes
    for pu in range(args.pu_count):
        images[0].extend(struct.pack("<II", data_start + pu * SPM_SIZE, SPM_SIZE))
    for spm in spm_images:
        images[0].extend(spm)

    struct.pack_into("<IIII", images[0], 0, encode_jal_x0(DRAM_HEADER_SIZE),
                     spm_init_offset, args.mc_count, args.pu_count)

    synapse_count = N_INPUT * N_EXC
    initial_weight_checksum = 0
    for weight in weights:
        initial_weight_checksum ^= f32_bits(weight)
    initial_rng_states = (
        snapshot.input_rng_state
        if snapshot is not None
        else [initial_rng_state(args.poisson_seed, index) for index in range(N_INPUT)]
    )
    expected_input_spikes = predict_input_spikes_from_states(
        thresholds, initial_rng_states,
    )
    logical_owners: list[list[dict[str, int | bool]]] = [
        [] for _ in range(N_INPUT)
    ]

    def posts_owned_by_mask(group_mask: int) -> int:
        if horizontal_partition == "none":
            return N_EXC
        return group_mask.bit_count() * (N_EXC // horizontal_group_count)

    for pu, copies in enumerate(input_per_pu):
        for local, copy in enumerate(copies):
            logical_owners[copy.input_index].append({
                "pu": pu + 1,
                "local": local,
                "group_mask": copy.group_mask,
                "primary": copy.primary,
                "replicated": copy.replicated,
                "post_count": posts_owned_by_mask(copy.group_mask),
            })
    expected_active_physical_pus = {
        int(owner["pu"]) - 1
        for index in expected_input_spikes for owner in logical_owners[index]
    }
    expected_active_input_pus = len(expected_active_physical_pus)
    positions = pu_positions(args.pu_count)

    def hop_count(left: int, right: int) -> int:
        left_row, left_column = positions[left]
        right_row, right_column = positions[right]
        return abs(left_row - right_row) + abs(left_column - right_column)

    reverse_hops = sum(
        hop_count(neuron_pu, input_pu)
        for group, group_neuron_pus in enumerate(neuron_groups)
        for neuron_pu in group_neuron_pus
        for input_pu in trace_input_groups[group]
    )
    reverse_pairs = sum(
        len(group_neuron_pus) * len(trace_input_groups[group])
        for group, group_neuron_pus in enumerate(neuron_groups)
    )
    feedforward_hops = sum(
        hop_count(int(owner["pu"]) - 1, neuron_pu)
        for owners in logical_owners
        for owner in owners
        for group, group_neuron_pus in enumerate(neuron_groups)
        if int(owner["group_mask"]) & (1 << group)
        for neuron_pu in group_neuron_pus
    )
    expected_reverse_messages = sum(
        posts_owned_by_mask(input_group_by_pu[pu])
        for pu in expected_active_physical_pus
    )
    max_reverse_messages = sum(
        (N_EXC // len(neuron_groups)) * len(trace_input_groups[group])
        for group in range(len(neuron_groups))
    )
    physical_input_records = sum(map(len, input_per_pu))
    physical_weight_slots = physical_input_records * weight_slots_per_row
    padding_weight_slots = physical_weight_slots - synapse_count
    if padding_weight_slots < 0:
        raise AssertionError("physical rows omitted dense feedforward synapses")
    padding_slots_per_row = padding_weight_slots / physical_input_records
    if padding_slots_per_row.is_integer():
        padding_slots_per_row = int(padding_slots_per_row)
    manifest = {
        "schema": "activen-mnist-training-v12",
        "rule": args.rule,
        "topology": "dense-grouped",
        "placement_policy": args.placement_policy,
        "neuron_zone_policy": args.neuron_zone_policy,
        "input_assignment": (
            args.input_assignment if horizontal_partition == "none"
            else "index-cyclic" if horizontal_partition in FULL_HORIZONTAL_GROUP_COUNTS
            else "rate-balanced-hot-partial"
        ),
        "horizontal_partition": horizontal_partition,
        "horizontal_group_count": horizontal_group_count,
        "horizontal_replica_stride": horizontal_replica_stride,
        "feedforward_destination_order": "input-pu-cyclic",
        "feedforward_destination_stride": feedforward_destination_stride,
        "trace_destination_order": "neuron-pu-cyclic",
        "trace_destination_stride": trace_destination_stride,
        "trace_bundle": args.trace_bundle,
        "recurrent_mode": args.recurrent_mode,
        "input_intensity": input_intensity,
        "runtime_snapshot": (
            {"path": str(snapshot_path), **snapshot.metadata}
            if snapshot is not None else None
        ),
        "warmup_steps": args.warmup_steps,
        "timed_steps": args.timed_steps,
        "expected_input_spikes": expected_input_spikes,
        "expected_active_input_pus": expected_active_input_pus,
        "expected_active_physical_input_pus": sorted(
            pu + 1 for pu in expected_active_physical_pus
        ),
        "pu_count": args.pu_count,
        "mc_count": args.mc_count,
        "memory_line_bits": args.mem_line_width,
        "input_count": N_INPUT,
        "physical_input_record_count": physical_input_records,
        "excitatory_count": N_EXC,
        "inhibitory_count": N_INH,
        "input_pu_count": len(input_pus),
        "neuron_pu_count": len(neuron_pus),
        "input_pus": [pu + 1 for pu in input_pus],
        "neuron_pus": [pu + 1 for pu in neuron_pus],
        "input_owner_groups": [
            [pu + 1 for pu in group] for group in trace_input_groups
        ],
        "neuron_groups": [
            [pu + 1 for pu in group] for group in neuron_groups
        ],
        "input_owners": logical_owners,
        "neuron_mc_zones": neuron_zones,
        "reverse_trace_messages_per_step": expected_reverse_messages,
        "reverse_trace_max_messages_per_step": max_reverse_messages,
        "estimated_reverse_trace_hops_per_step": reverse_hops,
        "estimated_reverse_trace_mean_hops": (
            reverse_hops / reverse_pairs
        ),
        "estimated_feedforward_mean_hops": (
            feedforward_hops / (N_INPUT * len(neuron_pus))
        ),
        "feedforward_messages_per_input_spike": len(neuron_pus),
        "feedforward_synapses": synapse_count,
        "feedforward_bytes": synapse_count * WEIGHT_RECORD_SIZE,
        "feedforward_weight_slots_per_row": weight_slots_per_row,
        "feedforward_padding_slots_per_row": padding_slots_per_row,
        "feedforward_padding_slots": padding_weight_slots,
        "feedforward_physical_weight_bytes": (
            physical_input_records * weight_slots_per_row * WEIGHT_RECORD_SIZE
        ),
        "initial_weight_checksum": initial_weight_checksum,
        "weight_max": weight_max,
        "potentiation_rate": potentiation_rate,
        "three_trace_potentiation_order": "f32(f32(eta_plus * post2) * effective_pre)",
        "max_spm_bytes": max(item["bytes"] for item in usage),
        "fifth_root_seed": getattr(args, "fifth_root_seed", "bit"),
        "min_spm_free_bytes": min(item["free_bytes"] for item in usage),
        "frame_starts": frame_starts,
        "frame_lines": frame_lines,
        "spm_init_offset": spm_init_offset,
        "image_sizes": [len(image) for image in images],
        "per_pu": usage,
    }
    return [bytes(image) for image in images], manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--payload", type=Path, default=Path("sim/payloads/sys/mnist_train.bin"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pu-count", type=int, default=512)
    parser.add_argument("--mc-count", type=int, default=16)
    parser.add_argument("--mem-line-width", type=int, default=256)
    parser.add_argument("--rule", choices=tuple(RULES), default="three-trace")
    parser.add_argument("--fifth-root-seed", choices=("bit", "lut"), default="bit",
                        help="must match the two-trace payload's MNIST_FIFTH_ROOT_SEED build option")
    parser.add_argument("--input-pus", type=int, default=196)
    parser.add_argument("--neuron-pus", type=int, default=224)
    parser.add_argument(
        "--placement-policy", choices=PLACEMENT_POLICIES, default="near"
    )
    parser.add_argument(
        "--neuron-zone-policy", choices=tuple(NEURON_ZONE_ORDERS),
        default="network-balanced",
    )
    parser.add_argument(
        "--input-assignment", choices=INPUT_ASSIGNMENTS, default="rate-balanced"
    )
    parser.add_argument(
        "--horizontal-partition", choices=HORIZONTAL_PARTITIONS, default="none",
        help="split selected or all dense input rows across column groups",
    )
    parser.add_argument(
        "--horizontal-replica-stride", type=int,
        default=HORIZONTAL_REPLICA_STRIDE,
        help="rotate replicas within each MC zone by this owner-rank stride",
    )
    parser.add_argument(
        "--recurrent-mode", choices=tuple(RECURRENT_MODES), default="tree"
    )
    parser.add_argument(
        "--trace-destination-stride", type=int,
        default=TRACE_DESTINATION_STRIDE,
    )
    parser.add_argument(
        "--feedforward-destination-stride", type=int,
        default=FEEDFORWARD_DESTINATION_STRIDE,
    )
    parser.add_argument("--trace-bundle", action="store_true")
    parser.add_argument("--weight-seed", type=int, default=19260817)
    parser.add_argument("--poisson-seed", type=int, default=0x19260817)
    parser.add_argument("--checkpoint", type=Path)
    parser.add_argument("--runtime-snapshot", type=Path)
    parser.add_argument("--frame", type=Path)
    parser.add_argument("--frame-index", type=int, default=0)
    parser.add_argument("--input-intensity", type=float, default=2.0)
    parser.add_argument("--warmup-steps", type=int, default=1)
    parser.add_argument("--timed-steps", type=int, default=2)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.pu_count != 512 or args.mc_count != 16:
        raise SystemExit("the current MNIST training payload targets the generated 512-PU/16-MC system")
    if args.mem_line_width != 256:
        raise SystemExit("the generated system uses 256-bit memory lines")
    if args.input_intensity < 0.0:
        raise SystemExit("input intensity must be non-negative")
    if args.runtime_snapshot and (args.checkpoint or args.frame):
        raise SystemExit(
            "--runtime-snapshot cannot be combined with --checkpoint or --frame"
        )
    if args.warmup_steps < 0:
        raise SystemExit("warmup steps must be non-negative")
    if args.timed_steps < 1:
        raise SystemExit("timed steps must be positive")
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(f"output directory is not empty: {args.output}")
    args.output.mkdir(parents=True, exist_ok=True)
    images, manifest = make_images(args)
    for mc, image in enumerate(images):
        (args.output / f"dram.{mc}").write_bytes(image)
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )
    print(json.dumps({
        key: value for key, value in manifest.items()
        if key not in {"per_pu", "input_owners"}
    },
                     indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
