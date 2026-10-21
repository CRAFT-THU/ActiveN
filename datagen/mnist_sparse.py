#!/usr/bin/env python3
"""Compact sparse MNIST images with four interchangeable reverse mappings.

The mask is structural: a present zero weight is retained. Ownership follows
the dense eight-way post-PU partition, independently of realized edge counts.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

import mnist_train as dense
from mnist_weight_caps import POLICY, fan_in_caps, caps_sha256

MAGIC = 0x4D535034
MODES = {"local": 1, "pu-scatter": 2, "neuron-scatter": 3, "dense": 4}
HEADER_SIZE = 256
INPUT_SIZE = 28
EXC_SIZE = 44
INH_SIZE = 20
POSTS = 50
INPUT_PUS = 36
NEURON_PUS = 28
GROUP_PUS = 64
LINE_BYTES = 32
SPM_LIMIT = 0x3000
assert SPM_LIMIT + 32 <= dense.SPIKE_COUNT_OFFSET

# Keep offsets in sync with sys/mnist_sparse.inc; all pointers are SPM offsets.
HEADER = {name: index * 4 for index, name in enumerate((
    "MAGIC", "MODE", "RULE", "ID", "GROUP", "LEADER", "RANK",
    "INPUT_COUNT", "INPUT_BASE", "EXC_COUNT", "EXC_BASE", "INH_BASE",
    "FORWARD_BASE", "FORWARD_COUNT", "REVERSE_PTR", "DEGREES", "DEST_BASE",
    "FF_COUNTS", "FF_PENDING", "FF_ACKS", "FF_CURSOR", "FF_ROW", "FF_END",
    "PHASE", "DONE", "EXPECTED", "RECEIVED", "GROUP_COUNT",
    "POST_LO", "POST_HI", "ACTIVE_LO", "ACTIVE_HI", "REDUCE_COUNT",
    "REDUCE_VALUE", "TREE_SIZE", "TREE_PARENT", "STEPS", "WARMUP", "TIMED",
    "WMAX_BASE", "POT", "DEP", "PRE_TARGET", "LOCAL_INH", "CHECKSUM", "PEERS",
    "FIRED_COUNT", "FIRED_BASE", "FIRED_END", "BCAST_CURSOR", "REV_POST",
    "REV_END", "REV_CURSOR", "REV_MINUS", "REV_PLUS", "REV_FIRED",
    "DATA_END", "WORKER_COUNT", "LOCAL_SPIKES", "EXPECTED_CHECKSUM", "MASK_CHILDREN", "CONNECTED_POSTS",
    "GROUP_PARENT", "PREPARED",
))}


def structural_mask(rate: float, seed: int) -> list[bool]:
    if not 0 < rate <= 1:
        raise ValueError("connection rate must be in (0, 1]")
    if not 0 <= seed < 2**32:
        raise ValueError("connectivity seed must be a uint32")
    rng = dense.Mt19937(seed)
    return [rng.uniform() < rate for _ in range(dense.N_INPUT * dense.N_EXC)]


def parameters(rule: str, rate: float) -> dict[str, float]:
    scale = rate ** (-1.0 if rule == "three-trace" else -0.8)
    return dict(weight_max=1 / rate,
                potentiation_rate=(0.01 if rule == "three-trace" else 0.0005) * scale,
                depression_rate=0.0001 * scale, pre_trace_target=0.4)


def initial_weights(mask: list[bool], seed: int) -> list[float]:
    weights = dense.initial_weights(seed, 78.0)
    totals = [sum(weights[i * 400 + j] for i in range(784) if mask[i * 400 + j])
              for j in range(400)]
    if any(total == 0 for total in totals):
        raise ValueError("a post has no positive structural input")
    return [dense.f32(value * 78.0 / totals[index % 400]) if mask[index] else 0.0
            for index, value in enumerate(weights)]


def make_images(args: argparse.Namespace) -> tuple[list[bytes], dict]:
    mode = MODES[args.reverse_mode]
    relevant_masks = getattr(args, "relevant_masks", False)
    mask = structural_mask(args.connection_rate, args.connectivity_seed)
    fan_in, weight_caps = fan_in_caps(mask)
    weight_max_policy = getattr(args, "weight_max_policy", POLICY)
    if weight_max_policy == "connection-rate":
        weight_caps = [dense.f32(1.0 / args.connection_rate)] * dense.N_EXC
    elif weight_max_policy != POLICY:
        raise ValueError("unsupported weight cap policy")
    params = parameters(args.rule, args.connection_rate)
    snapshot = dense.load_runtime_snapshot(args.runtime_snapshot) if args.runtime_snapshot else None
    if snapshot:
        meta = snapshot.metadata
        if (meta.get("weight_max_policy") != weight_max_policy or
                meta.get("weight_max_sha256") != caps_sha256(weight_caps)):
            raise ValueError("snapshot weight cap policy or fan-in differs from sparse image")
        variant = meta.get("variant", {})
        for field, expected in {"topology": "bernoulli", "connection_rate": args.connection_rate,
                                "connectivity_seed": args.connectivity_seed, **params}.items():
            if variant.get(field) != expected:
                raise ValueError(f"snapshot {field} differs from sparse image parameters")
        if meta.get("rule") != args.rule:
            raise ValueError("snapshot learning rule differs from image")
        with zipfile.ZipFile(args.runtime_snapshot) as archive:
            stored_mask = dense._npz_array(archive, "structural_mask", (784, 400))
            stored_caps = dense._npz_array(archive, "weight_max_by_post", (400,))
        if stored_caps != weight_caps:
            raise ValueError("snapshot weight cap array differs from structural fan-in")
        if any(value not in (0, 1) for value in stored_mask) or list(map(bool, stored_mask)) != mask:
            raise ValueError("snapshot structural mask differs from selected connectivity")
        weights = snapshot.weights
        if any(w != 0 for w, present in zip(weights, mask) if not present):
            raise ValueError("snapshot contains a weight at an absent connection")
    else:
        weights = initial_weights(mask, args.weight_seed)
    if any(w < 0 for w in weights):
        raise ValueError("weights must be nonnegative")

    ips, nps, _ = dense.grouped_pus(512, 16, 288, 224, "near", "network-balanced")
    copies, roles, input_groups = dense.horizontal_input_placement(
        [1.0] * 784, ips, 512, "full-8", 3)
    # Runtime MC-activity tests use one four-rank nibble per input MC.
    for group in input_groups:
        if any(len({pu // 32 for pu in group[start:start + 4]}) != 1
               for start in range(0, INPUT_PUS, 4)):
            raise ValueError("input placement is incompatible with sparse MC activity masks")
    excs, locations, neuron_groups, neuron_group = dense.horizontal_neuron_placement(nps, 512, 8)
    pixels = snapshot.pixels if snapshot else [((i * 73 + 19) ^ (i >> 2)) & 255 for i in range(784)]
    intensity = snapshot.input_intensity if snapshot else args.input_intensity
    cutoffs = [dense.poisson_runtime_cutoff(dense.poisson_threshold(pixel, intensity)) for pixel in pixels]
    payload = args.payload.read_bytes()
    images = [bytearray(16) for _ in range(16)]
    images[0].extend(payload)
    for image in images:
        dense.align(image, LINE_BYTES)
    spms = []
    usage = []
    reverse = [[] for _ in range(400)]
    post_descriptors = {}
    logical_owners = [[] for _ in range(784)]

    def put(spm, field, value):
        struct.pack_into("<I", spm, HEADER[field], value)

    for pu in range(512):
        group = neuron_group[pu] if excs[pu] else roles[pu].bit_length() - 1
        leader = neuron_groups[group][0]
        own_inputs = copies[pu]
        rank = input_groups[group].index(pu) if own_inputs else neuron_groups[group].index(pu)
        spm = bytearray(HEADER_SIZE)
        def allocate(size, alignment=4):
            dense.align(spm, alignment)
            start = len(spm)
            spm.extend(bytes(size))
            return start
        inp_base = allocate(len(own_inputs) * INPUT_SIZE)
        exc_base = allocate(len(excs[pu]) * EXC_SIZE)
        inh_base = allocate(len(excs[pu]) * INH_SIZE)
        forward_base = len(spm)
        local_reverse = [[] for _ in range(POSTS)]
        count = 0
        for local, copy in enumerate(own_inputs):
            pre = copy.input_index
            state = inp_base + local * INPUT_SIZE
            row_start = len(spm)
            for post in range(group * POSTS, (group + 1) * POSTS):
                if not mask[pre * 400 + post]:
                    continue
                dst, post_local = locations[post]
                descriptor = neuron_groups[group].index(dst) * 2 + post_local
                if relevant_masks and mode in (1, 4):
                    descriptor |= (post % POSTS) << 6
                offset = len(spm) + 4
                spm.extend(struct.pack("<HHf", dst + 1, descriptor, weights[pre * 400 + post]))
                local_reverse[post % POSTS].append((state, offset))
                reverse[post].append((pu, state, offset))
                count += 1
            rng = snapshot.input_rng_state[pre] if snapshot else dense.initial_rng_state(args.poisson_seed, pre)
            trace = snapshot.input_pre_trace[pre] if snapshot else dense.f32((pre % 29) / 31.0)
            identity = pre | (dense.INPUT_GLOBAL_PRIMARY if copy.primary else 0)
            struct.pack_into("<IIfIIII", spm, state, identity, cutoffs[pre], trace, rng, 0,
                             row_start, (len(spm) - row_start) // 8)
            logical_owners[pre].append({"pu": pu + 1, "local": local, "state_offset": state,
                                        "group": group, "primary": copy.primary})
        reverse_ptr = allocate((POSTS + 1) * 2, 2) if own_inputs and mode != 3 else 0
        if reverse_ptr:
            for post, entries in enumerate(local_reverse):
                struct.pack_into("<H", spm, reverse_ptr + post * 2, len(spm))
                if mode == 4:
                    # A zero offset denotes absence, never a zero-valued weight.
                    # Each skip reaches the next present slot strictly after i,
                    # or the row end. This also skips holes after a present slot.
                    weights_by_input = dict(entries)
                    row = bytearray(len(own_inputs) * 4)
                    next_present = len(own_inputs)
                    for i in reversed(range(len(own_inputs))):
                        weight = weights_by_input.get(inp_base + i * INPUT_SIZE, 0)
                        struct.pack_into("<HH", row, i * 4, weight, next_present - i)
                        if weight:
                            next_present = i
                    spm.extend(row)
                else:
                    for state, weight in entries:
                        spm.extend(struct.pack("<HH", state, weight))
            struct.pack_into("<H", spm, reverse_ptr + POSTS * 2, len(spm))
        degree_base = allocate(POSTS * 2, 2) if own_inputs else 0
        if degree_base:
            struct.pack_into("<50H", spm, degree_base, *(len(row) for row in local_reverse))
        # Neuron scatter can index directly with the forward entry descriptor.
        # The local/PU modes already receive the logical post slot instead.
        cap_slots = NEURON_PUS * 2 if mode == 3 else POSTS
        cap_base = allocate(cap_slots * 4) if own_inputs else 0
        if cap_base:
            for post in range(group * POSTS, (group + 1) * POSTS):
                dst, post_local = locations[post]
                slot = (neuron_groups[group].index(dst) * 2 + post_local
                        if mode == 3 else post % POSTS)
                struct.pack_into("<f", spm, cap_base + slot * 4, weight_caps[post])
        ff_counts = allocate(NEURON_PUS * 4) if own_inputs else 0
        # Dense reverse setup caches a row byte index beside each fired state
        # pointer. Legacy four-byte-vector payloads also fit this allocation.
        fired_base = allocate(len(own_inputs) * (8 if mode == 4 else 4)) if own_inputs else 0
        dest_base = allocate(INPUT_PUS * 4) if excs[pu] else 0
        if dest_base:
            # Rotate send order while preserving the original group rank bits.
            rotation = rank * 83 % INPUT_PUS
            for index in range(INPUT_PUS):
                dest_rank = (index + rotation) % INPUT_PUS
                struct.pack_into("<HH", spm, dest_base + index * 4,
                                 input_groups[group][dest_rank] + 1, dest_rank)
        group_order = [leader] + input_groups[group] + neuron_groups[group][1:]
        tree_index = group_order.index(pu)
        tree_span = (tree_index & -tree_index) if tree_index else GROUP_PUS
        children = []
        stride = tree_span // 2
        while stride:
            children.append(group_order[tree_index + stride] + 1)
            stride //= 2
        peers = allocate(len(children) * 2, 2) if children else 0
        if peers:
            struct.pack_into(f"<{len(children)}H", spm, peers, *children)
        for local, post in enumerate(excs[pu]):
            eo, io = exc_base + local * EXC_SIZE, inh_base + local * INH_SIZE
            ev = [snapshot.exc_ge[post], snapshot.exc_gi[post], snapshot.exc_v[post],
                  snapshot.theta_mv[post], snapshot.exc_post1[post], snapshot.exc_post2[post]] if snapshot else [
                  0.0, 0.0, -105.0, 20.0, (post % 23) / 25.0, (post % 31) / 33.0]
            iv = [snapshot.inh_ge[post], snapshot.inh_gi[post], snapshot.inh_v[post]] if snapshot else [0, 0, -100]
            struct.pack_into("<6fIII", spm, eo, *ev,
                             snapshot.exc_refractory[post] if snapshot else 0, 0, post % POSTS)
            struct.pack_into("<3fII", spm, io, *iv,
                             snapshot.inh_refractory[post] if snapshot else 0, 0)
            post_descriptors[post] = (pu, eo)
        values = dict(MAGIC=MAGIC, MODE=mode, RULE=dense.RULES[args.rule], ID=pu + 1, GROUP=group,
            LEADER=leader + 1, RANK=rank, INPUT_COUNT=len(own_inputs), INPUT_BASE=inp_base,
            EXC_COUNT=len(excs[pu]), EXC_BASE=exc_base, INH_BASE=inh_base,
            FORWARD_BASE=forward_base, FORWARD_COUNT=count, REVERSE_PTR=reverse_ptr, DEGREES=degree_base,
            DEST_BASE=dest_base, FF_COUNTS=ff_counts, FIRED_BASE=fired_base, PEERS=peers,
            MASK_CHILDREN=len(children),
            CONNECTED_POSTS=sum(bool(row) for row in local_reverse),
            GROUP_PARENT=(group_order[tree_index - (tree_index & -tree_index)] + 1) if tree_index else 0,
            TREE_SIZE=(pu & -pu) if pu else 512, TREE_PARENT=pu - (pu & -pu) + 1 if pu else 0,
            WARMUP=args.warmup_steps, TIMED=args.timed_steps, WMAX_BASE=cap_base,
            POT=dense.f32_bits(params["potentiation_rate"]), DEP=dense.f32_bits(params["depression_rate"]),
            PRE_TARGET=dense.f32_bits(params["pre_trace_target"]))
        for key, value in values.items():
            put(spm, key, value)
        spms.append(spm)
        usage.append(dict(pu=pu + 1, group=group, inputs=len(own_inputs), neurons=len(excs[pu]),
            synapses=count, forward_bytes=count * 8,
            local_reverse_bytes=((len(own_inputs) * POSTS * 4 if mode == 4 else count * 4)
                                 + 102) if reverse_ptr else 0,
            reverse_degree_bytes=100 if degree_base else 0))

    targets = []
    for post, entries in enumerate(reverse):
        pu, eo = post_descriptors[post]
        spm = spms[pu]
        dense.align(spm, 4)
        desc_start = len(spm)
        group = post // POSTS
        by_mc = {}
        if mode in (2, 3):
            if mode == 2:
                row_entries = [(dst, 0, post % POSTS) for dst in sorted({dst for dst, _, _ in entries})]
            else:
                row_entries = entries
            for dst, index, data in row_entries:
                by_mc.setdefault(dst // 32, bytearray()).extend(struct.pack("<HHI", dst + 1, index, data))
            for mc, row in sorted(by_mc.items()):
                start, lines = dense.append_csr_row(images[mc], row, LINE_BYTES)
                active = sum(1 << rank for rank, dst in enumerate(input_groups[group]) if dst // 32 == mc)
                spm.extend(struct.pack("<HHIII", 0x8001 + mc, lines, start, active & 0xFFFFFFFF, active >> 32))
        struct.pack_into("<II", spm, eo + 36, desc_start, len(by_mc))
        targets.append(dict(post=post, synapses=len(entries), pus=len({pu for pu, _, _ in entries}),
            mcs=sorted({pu // 32 for pu, _, _ in entries}), scatter_rows=len(by_mc),
            csr_bytes=sum(dense.align_up(len(row), LINE_BYTES) for row in by_mc.values())))
    for spm, info in zip(spms, usage):
        end = len(spm)
        if end > SPM_LIMIT:
            raise ValueError(f"PU {info['pu']} sparse data ends at {end}, beyond {SPM_LIMIT}")
        put(spm, "DATA_END", end)
        info.update(bytes=end, free_before_stack=SPM_LIMIT - end)
        # The startup copy need only cover initialized data. Both reserved
        # stacks and the unused SPM tail are outside the runtime's data ABI.
        dense.align(spm, LINE_BYTES)
    dense.align(images[0], LINE_BYTES)
    init_offset = len(images[0])
    data_offset = 512 * 8
    for spm in spms:
        images[0].extend(struct.pack("<II", data_offset, len(spm)))
        data_offset += len(spm)
    for spm in spms:
        images[0].extend(spm)
    struct.pack_into("<4I", images[0], 0, dense.encode_jal_x0(16), init_offset, 16, 512)
    checksum = 0
    for w, present in zip(weights, mask):
        if present:
            checksum ^= dense.f32_bits(w)
    manifest = dict(schema="activen-mnist-sparse-v4", rule=args.rule, reverse_mode=args.reverse_mode,
        relevant_masks=relevant_masks, experimental_scratch_region=dict(offset=SPM_LIMIT, bytes=32),
        spike_count_state=dict(offset=dense.SPIKE_COUNT_OFFSET, bytes=dense.SPIKE_COUNT_BYTES),
        startup_stack=dict(base=dense.STACK0_BASE, top=dense.STACK0_TOP),
        connection_rate=args.connection_rate, connectivity_seed=args.connectivity_seed,
        structural_synapses=sum(mask), mask_sha256=hashlib.sha256(bytes(mask)).hexdigest(),
        payload_sha256=hashlib.sha256(payload).hexdigest(), horizontal_partition="full-8",
        input_owner_groups=[[pu + 1 for pu in group] for group in input_groups],
        neuron_owner_groups=[[pu + 1 for pu in group] for group in neuron_groups],
        logical_owners=logical_owners, post_targets=targets, spm_usage=usage,
        parameters=params, weight_max_policy=weight_max_policy, fan_in=fan_in,
        weight_max_by_post=weight_caps, weight_max_sha256=caps_sha256(weight_caps),
        initial_weight_checksum=checksum,
        runtime_snapshot=snapshot.metadata if snapshot else None, warmup_steps=args.warmup_steps,
        timed_steps=args.timed_steps, num_pu=512, num_mc=16, mem_line_width=256)
    return list(map(bytes, images)), manifest


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reverse-mode", choices=MODES, required=True)
    parser.add_argument("--relevant-masks", action="store_true", help="encode logical post slots for the relevant-mask payload")
    parser.add_argument("--rule", choices=dense.RULES, default="three-trace")
    parser.add_argument("--connection-rate", type=float, default=0.125)
    parser.add_argument("--connectivity-seed", type=int, default=20260723)
    parser.add_argument("--weight-seed", type=int, default=19260817)
    parser.add_argument("--weight-max-policy", choices=(POLICY, "connection-rate"), default=POLICY,
                        help="explicit legacy scalar-cap profile; default retains realized fan-in caps")
    parser.add_argument("--poisson-seed", type=lambda value: int(value, 0), default=0x19260817)
    parser.add_argument("--runtime-snapshot", type=Path)
    parser.add_argument("--input-intensity", type=float, default=2.0)
    parser.add_argument("--warmup-steps", type=int, default=4)
    parser.add_argument("--timed-steps", type=int, default=12)
    args = parser.parse_args()
    if args.warmup_steps < 0 or args.timed_steps <= 0:
        parser.error("warmup must be nonnegative and timed steps positive")
    return args


def main():
    args = parse_args()
    images, manifest = make_images(args)
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        raise ValueError(f"output directory is not empty: {args.output}")
    for mc, image in enumerate(images):
        (args.output / f"dram.{mc}").write_bytes(image)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{manifest['structural_synapses']} synapses; max SPM data {max(p['bytes'] for p in manifest['spm_usage'])} bytes")


if __name__ == "__main__":
    main()
