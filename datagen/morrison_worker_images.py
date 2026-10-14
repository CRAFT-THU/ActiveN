#!/usr/bin/env python3
"""Write fresh ABI4 worker images from canonical ABI3 geometry/state."""
from pathlib import Path
import argparse
import json
import shutil

from morrison_graph import sha256
from morrison_model import MC_BYTES
from morrison_worker import WorkerLayout
from workload_paths import resolve_retained_path


def write_images(images, payload, helper, output, *, seed=0x20261011,
                 reduced=None, native_graph=None, archive_root=None):
    images = resolve_retained_path(images, archive_root=archive_root)
    payload, helper, output = map(Path, (payload, helper, output))
    if output.exists():
        raise FileExistsError(f'output must be a fresh directory: {output}')
    if reduced is None:
        layout = WorkerLayout(images, payload, helper, random_seed=seed)
    else:
        from morrison_checkpoint_layout import CheckpointWorkerLayout
        reduced = resolve_retained_path(reduced, archive_root=archive_root)
        layout = CheckpointWorkerLayout(images, payload, helper, reduced,
            random_seed=seed, graph_path=native_graph, archive_root=archive_root)
    m = layout.image.manifest
    output.mkdir(parents=True, exist_ok=False)
    records = []
    for mc in range(layout.spec.mc):
        data, record = layout.initial(mc)
        data.tofile(output / f'dram.{mc}')
        records.append(record)
    physical = m | dict(format='activen-morrison-image-v4',
        image_bytes=list(map(int, layout.sizes)),
        image_sha256=[r['physical_sha256'] for r in records],
        payload_sha256=sha256(payload), dispatch_layout=layout.layout,
        placement_seed=seed, placement_algorithm='numpy-pcg64-per-source-v1')
    # A replacement payload does not inherit the old payload's execution policy
    # or validation status. This converter records code identity, not its ABI
    # implementation details; select the runtime through workloads/profiles.json.
    for key in ('reverse_schedule', 'scheduling_control', 'shared_updater_id',
                'forward_receiver_count', 'payload_patch', 'ordering',
                'store_policy', 'forward_completion'):
        physical.pop(key, None)
    physical['benchmark_ready'] = False
    physical['runtime_validation'] = 'not performed by image conversion'
    physical['canonical_manifest_sha256'] = sha256(images / 'manifest.json')
    physical['component_bytes'] = [dict(x) for x in m['component_bytes']]
    for mc, parts in enumerate(physical['component_bytes']):
        parts['plastic'] = int(layout.new_end[mc] - layout.old_start[mc])
        parts['worker-directories'] = int(layout.sizes[mc] - m['image_bytes'][mc] - layout.shift[mc])
        if sum(parts.values()) != int(layout.sizes[mc]):
            raise ValueError('inconsistent component accounting')
    physical['per_mc_remaining_bytes'] = [int(MC_BYTES - n) for n in layout.sizes]
    physical['plastic_layout'] = ('randomized individual edges; contiguous worker-MC ranges; '
                                  'chunk128 weights then targets; sentinel padding')
    for file in ('neurons.npy', 'plastic-outdegree.npy'):
        source = reduced if reduced is not None and file == 'neurons.npy' else images
        shutil.copy2(source / file, output / file)
    physical['neuron_sha256'] = sha256(output / 'neurons.npy')
    provenance = dict(canonical=str(images), canonical_manifest_sha256=sha256(images / 'manifest.json'),
        payload=str(payload.resolve()), payload_sha256=sha256(payload),
        helper=str(helper.resolve()), helper_sha256=sha256(helper),
        placement_seed=seed, warmup=m['warmup'], timed=m['timed'], records=records)
    if reduced is not None:
        checkpoint = layout.import_record
        physical['source'] = dict(kind='reduced-native-checkpoint',
            checkpoint_manifest_sha256=checkpoint['expected'], tick=checkpoint['tick'],
            canonical_geometry=m['source'])
        provenance['reduced'] = str(reduced)
        provenance['reduced_import_sha256'] = sha256(reduced / 'import.json')
    for name, value in (('manifest.json', physical), ('generation.json', provenance)):
        (output / name).write_text(json.dumps(value, indent=2) + '\n')
    return provenance


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--images', required=True, help='canonical ABI3 path or retained asset ID')
    p.add_argument('--payload', type=Path, required=True)
    p.add_argument('--helper', type=Path, required=True, help='current libmorrison_locality.so')
    p.add_argument('--output', type=Path, required=True, help='fresh output directory')
    p.add_argument('--placement-seed', type=lambda s: int(s, 0), default=0x20261011)
    p.add_argument('--reduced', help='optional reduced checkpoint path or asset ID')
    p.add_argument('--native-graph', help='override the reduced checkpoint graph provenance path')
    p.add_argument('--archive-root', type=Path, help='explicit archive/tmp mount for legacy paths')
    args = p.parse_args()
    if args.native_graph and not args.reduced:
        p.error('--native-graph requires --reduced')
    result = write_images(args.images, args.payload, args.helper, args.output,
        seed=args.placement_seed, reduced=args.reduced,
        native_graph=args.native_graph, archive_root=args.archive_root)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
