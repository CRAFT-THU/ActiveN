"""Brunel proxy placement; neuron state always stays at its home PU."""
import numpy as np

PU, NE, NI = 512, 9000, 2250
CONFIG, JOBS, ROWS = 0x980, 0xA00, 0xD00


def capacity(size):
    if size not in (2, 4, 8):
        raise ValueError("proxy group size must be 2, 4 or 8")
    return 22 * size


def magic(size, updates, pipeline="legacy", mode="consecutive"):
    if pipeline not in ("legacy", "ready"):
        raise ValueError("unknown Brunel pipeline")
    if pipeline == "ready" and (size, updates, mode) != (4, "home", "consecutive"):
        raise ValueError("ready pipeline requires consecutive home G4")
    flags = 0xc00 if pipeline == "ready" else 0
    return 0x50580000 | ({"home": 0, "replicated": 1, "relay": 2}[updates] << 8) | size | flags


def topology():
    """Exact Topology.hilbert(32, 16), with (row, column) coordinates."""
    result = []
    for d in range(PU):
        x = y = 0
        t, s = d, 1
        while s < 32:
            rx, ry = (t >> 1) & 1, (t ^ (t >> 1)) & 1
            if not ry:
                if rx:
                    x, y = s - 1 - x, s - 1 - y
                x, y = y, x
            x, y = x + s * rx, y + s * ry
            t, s = t >> 2, s << 1
        result.append((x, y))
    return np.array(result, dtype=np.int16)


def groups(mode, size=4):
    capacity(size)
    ids = np.arange(PU, dtype=np.int32)
    if mode == "consecutive":
        return ids[:, None] // size * size + np.arange(size), ids % size
    if mode == "distant":
        stride = PU // size
        return ids[:, None] % stride + np.arange(size) * stride, ids // stride
    raise ValueError(mode)


def assign(pre, post, members):
    """Equal per-row quotas, then locality-first assignment with regret ordering.

    Place edges whose best proxy has the greatest distance advantage first.
    This is a deterministic capacitated heuristic, not a global optimum.
    Equal-cost choices rotate with the neuron ID to avoid a systematic bias.
    """
    xy = topology()
    size = members.shape[1]
    distances = np.abs(xy[:, None] - xy[None, :]).sum(axis=2)
    owners = np.empty(len(pre), dtype=np.uint16)
    cuts = np.searchsorted(pre, np.arange(NE + NI + 1))
    before = after = 0
    for src in range(NE + NI):
        lo, hi = cuts[src:src + 2]
        n = hi - lo
        if not n:
            continue
        peers = members[src % PU]
        rotation = (np.arange(size) + src) % size
        cost = distances[post[lo:hi] % PU][:, peers[rotation]]
        preference = np.argsort(cost, axis=1, kind="stable")
        sorted_cost = np.take_along_axis(cost, preference, axis=1)
        order = np.argsort(-(sorted_cost[:, 1] - sorted_cost[:, 0]), kind="stable")
        quota = np.full(size, n // size, dtype=np.int32)
        quota[:n % size] += 1
        chosen = np.empty(n, dtype=np.int32)
        for edge in order:
            for choice in preference[edge]:
                if quota[choice]:
                    chosen[edge] = choice
                    quota[choice] -= 1
                    break
        owners[lo:hi] = peers[rotation[chosen]]
        before += int(distances[src % PU, post[lo:hi] % PU].sum())
        after += int(cost[np.arange(n), chosen].sum())
    return owners, dict(home_mean_hops=before / len(pre), proxy_mean_hops=after / len(pre))


def layout(ref, mode, size=4, updates="replicated", pipeline="legacy"):
    magic(size, updates, pipeline, mode)
    members, rank = groups(mode, size)
    owners, plastic_hops = assign(ref.pre, ref.post, members)
    static_owners, static_hops = assign(ref.sp, ref.st, members)
    images = [bytearray(0x10000) for _ in range(16)]
    addresses = np.empty(len(owners), dtype=np.uint32)
    rows = np.zeros((PU, capacity(size), 8), dtype=np.uint32)
    rows[:, :, 2] = 0xFFFFFFFF
    for pu in range(PU):
        gids = (np.arange(22)[:, None] * PU + members[pu]).ravel()
        valid = gids < NE + NI
        rows[pu, valid, 2] = gids[valid]
    for src in range(NE):
        lo, hi = ref.po[src:src + 2]
        slot = (src // PU) * size + rank[src % PU]
        for proxy in members[src % PU]:
            indices = np.flatnonzero(owners[lo:hi] == proxy) + lo
            image = images[proxy // 32]
            base = len(image)
            rows[proxy, slot, 3:5] = base, len(indices)
            for chunk in range(0, len(indices), 128):
                ids = indices[chunk:chunk + 128]
                dst = ref.post[ids]
                target = ((256 + (dst // PU) * 80 + 68).astype(np.uint32) << 16) | (dst % PU + 1).astype(np.uint32)
                addresses[ids] = len(image) + np.arange(len(ids), dtype=np.uint32) * 4
                image.extend(np.pad(ref.w[ids], (0, 128 - len(ids))).tobytes())
                image.extend(np.pad(target, (0, 128 - len(ids))).tobytes())
        ref.n[src, 13] = hi - lo
    gids = np.arange(NE + NI)
    slots = (gids // PU) * size + rank[gids % PU]
    ref.n[:, 19] = ROWS + slots * 32
    return images, addresses, dict(members=members, rank=rank, owners=owners,
        static_owners=static_owners, rows=rows, slots=slots, size=size, updates=updates,
        pipeline=pipeline, mode=mode,
        placement=dict(plastic=plastic_hops, static=static_hops,
                       balance="each neuron's plastic and static row separately differ by at most one edge"))


def write_weights(images, ref, addresses, owners):
    for mc in range(16):
        mask = owners // 32 == mc
        words = np.frombuffer(images[mc], dtype=np.uint32)
        words[addresses[mask] // 4] = ref.w[mask].view(np.uint32)


def initialize_spm(blob, pu, plan, neurons):
    """32-byte descriptors, with only X and fired copied as dynamic state."""
    rows = plan["rows"][pu].copy()
    valid = rows[:, 2] < NE + NI
    excitatory = rows[:, 2] < NE
    if plan["updates"] == "replicated":
        rows[excitatory, 0] = neurons[rows[excitatory, 2], 6]
    count, exc = int(valid.sum()), int(excitatory.sum())
    assert np.all(valid[:count]) and np.all(excitatory[:exc])
    config = np.zeros(16, dtype=np.uint32)
    config[:plan["size"]] = plan["members"][pu] + 1
    config[8:10] = count, 0x20000000 + ROWS + exc * 32
    config[14] = magic(plan["size"], plan["updates"], plan.get("pipeline", "legacy"),
                       plan.get("mode", "consecutive"))
    blob[CONFIG:CONFIG + config.nbytes] = config.tobytes()
    blob[ROWS:ROWS + rows.nbytes] = rows.tobytes()
