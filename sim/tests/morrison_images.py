#!/usr/bin/env python3
"""Deterministic compact-runtime functional cases, explicitly not benchmarks."""
import argparse
from dataclasses import replace
import json
from pathlib import Path
import struct
import sys
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "datagen"))
from morrison import write_images
from morrison_graph import Graph, fresh_graph, sha256
from morrison_model import Spec, F, constants, CONFIG
from morrison_validate import check_images


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", choices=("mixed", "occupancy", "chunks", "full"), required=True)
    parser.add_argument("--group", type=int, choices=(1, 4), required=True)
    parser.add_argument('--bulk', action='store_true')
    parser.add_argument('--pu', type=int)
    parser.add_argument('--mc', type=int)
    parser.add_argument('--cross-mc-jobs', action='store_true')
    parser.add_argument('--timed', type=int)
    parser.add_argument('--dispatch-width', type=int, choices=(1,2,4,8,16,32), default=4)
    parser.add_argument('--dispatch-policy', choices=('legacy','balanced'), default='balanced')
    parser.add_argument('--forward-completion', choices=('poll','tree'), default='poll')
    parser.add_argument('--stagger-fire', action='store_true')
    parser.add_argument('--static-burst', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    # Occupancy puts 176 E + 44 I on every PU and fills all 880 G4 job slots.
    # Mixed includes long, partial rows and a mix of quiet and simultaneous pairs.
    spec = (Spec() if args.case == "full" else
            Spec(ne=11264, ni=2816, ce=1, ci=1, pu=64, mc=2) if args.case == "occupancy"
            else Spec(ne=512, ni=128, ce=64, ci=8, pu=64, mc=2) if args.case == "chunks"
            else Spec(ne=256, ni=64, ce=129, ci=8, pu=64, mc=2))
    if (args.stagger_fire or args.static_burst) and args.case != 'chunks':
        raise ValueError('stagger/burst controls require the chunks fixture')
    if args.static_burst: spec=replace(spec,ci=256)
    if args.pu is not None or args.mc is not None:
        spec = replace(spec, pu=args.pu or spec.pu, mc=args.mc or spec.mc)
        spec.validate(True)
    graph = fresh_graph(args.output / "graph", spec, 92713)
    chunk_rows = {}
    empty_source = None
    if args.case == 'chunks':
        # Exact physical chunk boundaries, while retaining fixed indegrees and
        # excluding autapses. Parallel edges are part of the reference model.
        chunk_rows = {0:3967,16:4095,32:4096,48:9215,4:127,20:128}
        post = np.repeat(np.arange(spec.ne,dtype='<u4'),spec.ce)
        pre = np.full(post.size,spec.ne,dtype='<u4')
        for source,count in chunk_rows.items():
            chosen = np.flatnonzero((pre == spec.ne) & (post != source))[:count]
            if len(chosen) != count: raise ValueError('insufficient fixture edges')
            pre[chosen] = source
        remaining = pre == spec.ne
        fallback = (post[remaining]+1) % spec.ne
        selected = np.asarray(list(chunk_rows),dtype='<u4')
        while np.any(np.isin(fallback,selected)):
            mask = np.isin(fallback,selected)
            fallback[mask] = (fallback[mask]+1) % spec.ne
        pre[remaining] = fallback
        state,provenance = graph.state,graph.provenance
        del graph
        order = np.argsort(pre,kind='stable')
        np.save(args.output/'graph/ee_pre.npy',pre[order])
        np.save(args.output/'graph/ee_post.npy',post[order])
        if args.static_burst:
            # Long, immutable scatter requests can outlive range emission and
            # expose late deliveries after receiver-closing is armed.
            for name,posts in [('ie',spec.ne),('ii',spec.ni)]:
                target=np.repeat(np.arange(posts,dtype='<u4'),spec.ci)
                origin=np.zeros(target.size,dtype='<u4')
                if name=='ii': origin[target==0]=1
                ordering=np.argsort(origin,kind='stable')
                np.save(args.output/'graph'/f'{name}_pre.npy',origin[ordering])
                np.save(args.output/'graph'/f'{name}_post.npy',target[ordering])
        graph = Graph(args.output/'graph',spec,state,provenance)
        counts = np.diff(graph.offsets['ee'])
        if any(int(counts[s]) != n for s,n in chunk_rows.items()):
            raise ValueError('fixture row count mismatch')
        empty_source = int(np.flatnonzero(counts == 0)[0])
        graph.provenance['graph_rng'] = ('seeded E-I; controlled E-E/I-E/I-I with fixed indegrees and no autapses' if args.static_burst
                                         else 'seeded static projections; controlled E-E chunk boundaries with fixed indegrees')
    f = graph.state.view(F)
    c = constants()
    f[:, 0] = F(21.)
    if args.case == "mixed":
        f[1::2, 0] = F(0.)
        # Refractory byte and fire byte must not alias; these neurons are quiet.
        graph.state[3::7, 7] = 3
    elif args.case == "full":
        # Exercise full fan-in/out, all MCs and native Poisson drive with two
        # known initial spikes. This cold diagnostic is never a benchmark.
        f[:, 0] = F(0.)
        f[[0, spec.ne], 0] = F(21.)
    elif args.case == 'chunks':
        f[:,0] = F(0.)
        f[list(chunk_rows)+[empty_source,spec.ne],0] = F(21.)
        if args.stagger_fire:
            for source,refractory in [(16,2),(32,3),(48,4),(20,5)]:
                graph.state[source,7]=refractory
    f[:spec.ne, 3] = F(c["pot"] * F(.5))
    f[:spec.ne, 4] = F(c["dep"] * F(.75))
    graph.provenance["fixture"] = args.case
    graph.provenance["physical_state"] = (
        "controlled functional fixture: zero currents and pending inputs; "
        "E trace coefficients X=pot*0.5, Y=dep*0.75; "
        + {"mixed": "alternating V=21/0 mV, refractory=3 for GIDs 3::7",
           "occupancy": "all V=21 mV, refractory=0",
           "chunks": "V=21 on selected 31/32/33/72-chunk, short and empty E rows plus I0; V=0 otherwise",
           "full": "V=0 except E0/I0 at 21 mV, refractory=0"}[args.case])
    if args.stagger_fire:
        graph.provenance['physical_state'] += '; source 16/32/48/20 refractory counters are 2/3/4/5'
    if args.bulk:
        from morrison_bulk import write_images as write_bulk
        payload = 'morrison_bulk_tree.bin' if args.forward_completion=='tree' else 'morrison_bulk.bin'
        manifest = write_bulk(graph, args.output, ROOT/'sim/payloads/sys'/payload,
                              dispatch_width=args.dispatch_width, dispatch_policy=args.dispatch_policy, functional=True,warmup=0,
                              timed=args.timed or (7 if args.case in ('mixed','chunks') else 2),
                              seed=19260817,external=args.case=='full',forward_completion=args.forward_completion)
    else:
        manifest = write_images(graph, args.output, ROOT / f"sim/payloads/sys/morrison_g{args.group}.bin",
                                group=args.group, functional=True, warmup=0,
                                timed=args.timed or (7 if args.case == "mixed" else 2),
                                seed=19260817, external=args.case == "full")
    # Seed cumulative-style counters near wrap. Phase 1 must reset all four
    # before any new delivery, on tree leaves as well as the root.
    image = args.output / "dram.0"
    with image.open("r+b") as stream:
        header = struct.unpack("<4I", stream.read(16))
        for pu in range(spec.pu):
            stream.seek(header[1] + 8*pu)
            offset, size = struct.unpack("<2I", stream.read(8))
            base = header[1] + offset
            if args.cross_mc_jobs:
                if not args.bulk: raise ValueError('cross-MC jobs require ABI2')
                stream.seek(base+CONFIG)
                stream.write(struct.pack('<I', spec.pu//2))
            for field in (64, 68, 104, 108):
                stream.seek(base+field)
                stream.write(struct.pack("<I", 0xFFFFFFF0))
    manifest["image_sha256"][0] = sha256(image)
    manifest["fixture"] = dict(cross_mc_jobs=args.cross_mc_jobs, name=args.case, external_drive=args.case == "full", counter_seed=0xFFFFFFF0,
                                measured_performance=False)
    if chunk_rows:
        manifest['fixture'].update(plastic_row_counts=chunk_rows,empty_source=empty_source,stagger_fire=args.stagger_fire,static_burst=args.static_burst)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    print(json.dumps(check_images(args.output), indent=2), flush=True)


if __name__ == "__main__":
    main()
