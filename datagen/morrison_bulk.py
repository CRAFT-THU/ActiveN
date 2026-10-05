#!/usr/bin/env python3
"""ABI2 Morrison images: whole chunked rows and runtime range delegation."""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import struct
import numpy as np

from morrison import Images, scatter_row, REVERSE, REVERSE_BUCKET
from morrison_graph import Graph, partition, sha256, BLOCK
from morrison_model import (Spec, F, MAGIC, NODE_BASE, NODE_BYTES, CONFIG,
    PARAMETERS, POISSON, INIT_END, MC_BYTES, LINE, constants, poisson_thresholds)

ROW_POINTERS, ABI_VERSION = 0x2ea0, 2
DISPATCH_POLICIES = {'legacy': 0, 'balanced': 1}


def plastic_row(images, mc, weights, destinations):
    # Include one sentinel. Both halves of every partial chunk are line aligned.
    count = len(weights)
    slots = (count + 1 + 7) // 8 * 8
    padded_w = np.zeros(slots, '<u4')
    padded_d = np.full(slots, 0xffffffff, '<u4')
    padded_w[:count] = weights.view('<u4')
    padded_d[:count] = destinations
    full, tail = divmod(slots, 128)
    data = np.empty(slots * 2, '<u4')
    if full:
        blocks = data[:full*256].reshape(full, 256)
        blocks[:, :128] = padded_w[:full*128].reshape(full, 128)
        blocks[:, 128:] = padded_d[:full*128].reshape(full, 128)
    if tail:
        data[full*256:full*256+tail] = padded_w[full*128:]
        data[full*256+tail:] = padded_d[full*128:]
    base = images.place(mc, data, 'plastic')
    i = np.arange(count, dtype='<u4')
    return base, base + data.nbytes, base + i//128*1024 + i%128*4


def write_images(graph, output, payload, *, dispatch_width=4, dispatch_policy='balanced', warmup=60,
                 timed=1000, seed=20260724, functional=False, external=True, forward_completion='poll'):
    spec, output = graph.spec, Path(output)
    spec.validate(functional)
    if (forward_completion not in ('poll','tree') or dispatch_policy not in DISPATCH_POLICIES or dispatch_width < 1
            or dispatch_width & (dispatch_width-1) or dispatch_width > spec.pu_per_mc
            or (dispatch_policy == 'legacy' and dispatch_width > 4)
            or warmup < 0 or timed < 1):
        raise ValueError('invalid runtime dispatch width or window')
    if not external and not functional:
        raise ValueError('external drive may only be disabled in functional fixtures')
    if graph.state.shape != (spec.n, 8) or graph.state.dtype != np.dtype('<u4'):
        raise ValueError('invalid compact state shape/type')
    if np.any(~np.isfinite(graph.state[:, :7].view(F))) or np.any((graph.state[:, 7]&255)>5) or np.any(graph.state[:,7]>>9):
        raise ValueError('invalid compact state values')
    code = Path(payload).read_bytes()
    if len(code) + 16 > 8192:
        raise ValueError('payload exceeds 8 KiB')
    c = constants()
    images = Images(output, spec.mc)
    paths = [output / f'reverse-work.{mc}' for mc in range(spec.mc)]
    streams = [p.open('wb') for p in paths]
    boundaries = []
    statdirs = np.zeros((spec.n, spec.mc, 2), '<u4')
    revdirs = np.zeros((spec.ne, spec.mc, 2), '<u4')
    plastic_count = static_count = 0
    try:
        # No other object may intervene between consecutive boundaries of a home.
        for pu in range(spec.pu):
            mc = pu // spec.pu_per_mc
            pointers = []
            for src in range(pu, spec.ne, spec.pu):
                post, weights = graph.row('ee', src)
                target = ((NODE_BASE + post//spec.pu*NODE_BYTES + 20).astype(np.uint32) << 16) | (post % spec.pu + 1).astype(np.uint32)
                base, end, addresses = plastic_row(images, mc, weights, target)
                pointers.append(base)
                records = np.empty(len(post), REVERSE)
                records['post'] = post
                index = (NODE_BASE + src//spec.pu*NODE_BYTES) | mc
                records['entry'] = np.uint64((pu+1) | (index << 16)) | (addresses.astype(np.uint64) << 32)
                records.tofile(streams[mc])
                plastic_count += len(post)
            pointers.append(end if pointers else images.files[mc].tell())
            boundaries.append(np.asarray(pointers, '<u4'))
        for src in range(spec.n):
            if src < spec.ne:
                post, _ = graph.row('ei', src)
                target = post + spec.ne
                weight = F(c['epsc'] * c['external_weight'])
            else:
                e, _ = graph.row('ie', src-spec.ne)
                i, _ = graph.row('ii', src-spec.ne)
                target = np.r_[e, i+spec.ne]
                weight = F(c['epsc'] * c['inhibitory_weight'])
            mcs = (target % spec.pu)//spec.pu_per_mc
            target = target[np.argsort(mcs, kind='stable')]
            cuts = np.r_[0, np.cumsum(np.bincount(mcs, minlength=spec.mc))]
            index = NODE_BASE + target//spec.pu*NODE_BYTES + (20 if src < spec.ne else 24)
            entries = (target % spec.pu + 1).astype(np.uint64) | (index.astype(np.uint64) << 16) | (np.uint64(weight.view(np.uint32)) << np.uint64(32))
            for mc in range(spec.mc):
                lo, hi = cuts[mc:mc+2]
                statdirs[src, mc] = scatter_row(images, mc, entries[lo:hi], 'static')
            static_count += len(target)
        for f in streams:
            f.close()
        # Bound temporary sorting RAM to one postsynaptic bucket, as in ABI1.
        for mc, path in enumerate(paths):
            work = output / f'reverse-buckets.{mc}'
            work.mkdir()
            buckets = [work / str(i) for i in range((spec.ne+REVERSE_BUCKET-1)//REVERSE_BUCKET)]
            files = [p.open('wb') for p in buckets]
            try:
                with path.open('rb') as f:
                    while True:
                        records = np.fromfile(f, dtype=REVERSE, count=BLOCK)
                        if not len(records): break
                        partition(records, records['post']//REVERSE_BUCKET, files)
            finally:
                for f in files: f.close()
            for bucket, p in enumerate(buckets):
                if p.stat().st_size > REVERSE_BUCKET*spec.ce*REVERSE.itemsize:
                    raise ValueError('reverse bucket violates indegree bound')
                records = np.fromfile(p, dtype=REVERSE)
                entries = records['entry'][np.argsort(records['post'], kind='stable')]
                start, end = bucket*REVERSE_BUCKET, min(spec.ne, (bucket+1)*REVERSE_BUCKET)
                cuts = np.r_[0, np.cumsum(np.bincount(records['post']-start, minlength=end-start))]
                for local, post in enumerate(range(start, end)):
                    lo, hi = cuts[local:local+2]
                    revdirs[post, mc] = scatter_row(images, mc, entries[lo:hi], 'reverse')
                p.unlink()
            work.rmdir(); path.unlink()
            print(f'reverse MC {mc} complete', flush=True)
        stride = max(32, spec.mc*8)
        static_bases, reverse_bases = [], []
        for pu in range(spec.pu):
            mc = pu//spec.pu_per_mc
            for source, bases in ((statdirs, static_bases), (revdirs, reverse_bases)):
                rows = source[pu::spec.pu]
                table = np.zeros((len(rows), stride//4), '<u4')
                table[:, :spec.mc*2] = rows.reshape(len(rows), spec.mc*2)
                bases.append(images.place(mc, table, 'directories'))
        if plastic_count != spec.ne*spec.ce or static_count != spec.n*(spec.ce+spec.ci)-plastic_count:
            raise ValueError('edge conservation failure')
        images.patch(0, 16, code)
        section = images.place(0, bytes(spec.pu*8), 'spm-descriptors')
        rng = np.random.default_rng(seed)
        worker_rng = rng.integers(1, 2**32, size=(spec.pu, 2), dtype=np.uint32)
        for pu in range(spec.pu):
            state = graph.state[pu::spec.pu]
            header = np.zeros(64, '<u4')
            header[:11] = [MAGIC, pu+1, spec.pu, 1, len(state), 0, 0, 0, warmup, warmup+timed, pu//spec.pu_per_mc]
            header[43:45] = worker_rng[pu]
            for i, key in enumerate(('p11','p21','p31','p32','p33','epsc','delivery','external_mean','external_limit','external_weight','dep','pot','wmax','decay_pre','decay_post')):
                header[46+i] = c[key].view(np.uint32)
            if not external: header[55] = 0
            blob = bytearray(INIT_END)
            blob[:256] = header.tobytes()
            blob[NODE_BASE:NODE_BASE+state.nbytes] = state.tobytes()
            # Compatibility magic for common bootstrap; no proxy row descriptors.
            struct.pack_into('<I', blob, CONFIG+56, 0x50580004)
            struct.pack_into('<10I', blob, PARAMETERS, spec.ne, spec.ni, spec.mc, static_bases[pu], ABI_VERSION, dispatch_width, reverse_bases[pu], stride, DISPATCH_POLICIES[dispatch_policy], int(forward_completion=='tree'))
            p = boundaries[pu]
            if len(p) > 221: raise ValueError('row-boundary cache overflow')
            blob[ROW_POINTERS:ROW_POINTERS+p.nbytes] = p.tobytes()
            table = poisson_thresholds(2.088 if external else 0.)
            blob[POISSON:POISSON+table.nbytes] = table.tobytes()
            offset = images.place(0, blob, 'spm-initialization')
            images.patch(0, section+pu*8, struct.pack('<II', offset-section, len(blob)))
        images.patch(0, 0, struct.pack('<IIII', 0x0100006f, section, spec.mc, spec.pu))
    finally:
        for f in streams: f.close()
        images.close()
    np.save(output/'neurons.npy', graph.state)
    np.save(output/'plastic-outdegree.npy', np.diff(graph.offsets['ee']).astype('<u4'))
    sizes = [p.stat().st_size for p in images.paths]
    manifest = dict(format='activen-morrison-image-v2', model=spec.model(), spec=asdict(spec), system=spec.system(),
        functional=functional, source=graph.provenance, group_size=1, dispatch_width=dispatch_width,
        dispatch_policy=dispatch_policy,
        forward_completion=forward_completion,
        warmup=warmup, timed=timed, seed=seed, external_drive=external,
        constants={k:float(v) for k,v in c.items()}, plastic_edges=plastic_count, static_edges=static_count,
        graph_sha256=graph.graph_hash.hexdigest(), neuron_sha256=sha256(output/'neurons.npy'), payload_sha256=sha256(payload),
        image_bytes=sizes, image_sha256=[sha256(p) for p in images.paths], component_bytes=images.components,
        per_mc_remaining_bytes=[MC_BYTES-n for n in sizes],
        plastic_layout='whole home CSR; chunk128 weights then targets; sentinel-inclusive line-rounded halves',
        ordering='integrate; old-weight delivery and quiet LTD; post-active LTD then LTP; posted-store route markers and MC idle; trace increments',
        store_policy='posted-p0-route-markers-p2-mc-idle-v1',
        benchmark_ready=not functional and graph.provenance['kind']=='checkpoint')
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reuse-graph', required=True, type=Path, help='existing validated ABI1/2 images with canonical graph and initial neuron state')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--dispatch-width', type=int, choices=(1,2,4,8,16,32), default=4)
    parser.add_argument('--dispatch-policy', choices=tuple(DISPATCH_POLICIES), default='balanced')
    parser.add_argument('--forward-completion', choices=('poll','tree'), default='poll')
    parser.add_argument('--warmup', type=int, default=60)
    parser.add_argument('--timed', type=int, default=1000)
    args=parser.parse_args()
    payload=args.payload or Path('sim/payloads/sys/morrison_bulk_tree.bin' if args.forward_completion=='tree' else 'sim/payloads/sys/morrison_bulk.bin')
    prior=json.loads((args.reuse_graph/'manifest.json').read_text())
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output/'graph').symlink_to((args.reuse_graph/'graph').resolve(), target_is_directory=True)
    graph=Graph(args.output/'graph', Spec(**prior['spec']), np.load(args.reuse_graph/'neurons.npy'), prior['source'])
    if graph.graph_hash.hexdigest()!=prior['graph_sha256']: raise ValueError('canonical graph identity changed')
    result=write_images(graph,args.output,payload,dispatch_width=args.dispatch_width,dispatch_policy=args.dispatch_policy,
        warmup=args.warmup,timed=args.timed,seed=prior['seed'],functional=prior['functional'],external=prior['external_drive'],forward_completion=args.forward_completion)
    print(json.dumps({k:result[k] for k in ('format','image_bytes','per_mc_remaining_bytes')},indent=2))

if __name__=='__main__': main()
