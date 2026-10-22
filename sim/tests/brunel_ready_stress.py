#!/usr/bin/env python3
"""Deterministic real-simulator fixture for the Brunel ready transport/fence."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import numpy as np

def generate(payload, rule, out):
    code = payload.read_bytes()
    if len(code) > 0x1ff0:
        raise ValueError('stress payload exceeds the shared 8 KiB code contract')
    out.mkdir(parents=True, exist_ok=False)
    images = [bytearray(65536) for _ in range(16)]
    images[0][16:16+len(code)] = code
    for mc in range(16):
        for slot in range(65):
            struct.pack_into('<I', images[mc], 0xa000+slot*32, 0x40000000)
    for slot in range(65):
        struct.pack_into('<II', images[0], 0xc000+slot*8,
                         (slot << 16) | 1, 0xa000+slot*32)
    section = len(images[0])
    images[0].extend(bytes(512*8))
    for pu in range(512):
        blob = bytearray(0x3f00)
        for offset, value in {0:0x42524e34, 4:pu+1, 8:512, 12:rule, 36:1,
                              40:pu//32, 204:0x3f800000, 220:0x3f800000,
                              232:0x41200000, 0x2d9c:256, 0x9b8:0x50580c04}.items():
            struct.pack_into('<I', blob, offset, value)
        struct.pack_into('<4I', blob, 0x980, *(pu//4*4+i+1 for i in range(4)))
        struct.pack_into('<II', images[0], section+pu*8, len(images[0])-section, len(blob))
        images[0].extend(blob)
    struct.pack_into('<4I', images[0], 0, 0x0100006f, section, 16, 512)
    identities = []
    for mc, data in enumerate(images):
        p = out/f'dram.{mc}'
        p.write_bytes(data)
        identities.append(dict(path=str(p), bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    manifest = dict(format='brunel-ready-stress-v1', rule=rule, pus=512, mcs=16,
                    operations=65, payload=str(payload), payload_sha256=hashlib.sha256(code).hexdigest(),
                    images=identities, measurement='functional fixture; no performance timer')
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps(manifest, indent=2))

def check(images, dump, log):
    manifest = json.loads((images/'manifest.json').read_text())
    text = log.read_text()
    if '[System] Result: 0 (0x0)' not in text:
        raise ValueError('stress payload did not report success')
    data = (dump/'step-0001.bin').read_bytes()
    if struct.unpack_from('<II', data) != (0x42534431, 512):
        raise ValueError('invalid complete SPM snapshot')
    cursor = 8
    for pu in range(512):
        ident, size = struct.unpack_from('<II', data, cursor)
        cursor += 8
        if (ident,size) != (pu+1,16384):
            raise ValueError('invalid PU record')
        s = np.frombuffer(data[cursor:cursor+size], '<u4')
        cursor += size
        if s[18] != s[19] or any(s[x//4] for x in (152,156,160,164,168,180,0x2910)):
            raise ValueError('transport did not drain')
        if np.any(s[0x1800//4+11:0x2800//4:16]):
            raise ValueError('live descriptor at termination')
        if any(s[x//4] != 0x19ca7ead for x in (0x3f00,0x3f80)):
            raise ValueError('damaged stack guard')
        # All PUs ran their actual sixteen-MC route-marker sweep.
        if pu == 0:
            root = s.copy()
        elif s[18] != 16:
            raise ValueError('nonroot marker sweep incomplete')
    if cursor != len(data):
        raise ValueError('trailing snapshot data')
    # Root boot resets its legacy counters after the transport self-test.
    if root[18] != 20 or root[17] != 65:
        raise ValueError('root read/event counts differ')
    stage, n0, n1, reordered = map(int,root[0x2a00//4:0x2a10//4])
    if stage != 3 or min(n0,n1) == 0 or n0+n1 != 65:
        raise ValueError('full-pool transition or both-SMT consumption missing')
    order = root[0x2b00//4:0x2c00//4]
    expected_order = np.arange(0x1800,0x2800,64,dtype=np.uint32)
    if not np.array_equal(np.sort(order),expected_order):
        raise ValueError('completion order lost or duplicated a descriptor')
    if reordered != int(not np.array_equal(order,expected_order)):
        raise ValueError('incorrect completion-order observation')
    f = np.float32
    expected = f(2.25) if manifest['rule'] == 0 else f(f(1.5)+f(f(0.5)*f(float(f(1.5))**(2/5))))
    errors = []
    for mc in range(16):
        before = np.fromfile(images/f'dram.{mc}', dtype='<u4')
        after = np.fromfile(dump/f'dram.{mc}', dtype='<u4')
        if len(before) != len(after):
            raise ValueError('DRAM dump extent differs')
        allowed = np.zeros(len(before),dtype=bool)
        if mc == 0:
            allowed[0x8000//4:0x8080//4] = True  # ordinary boot transport self-test
        for slot in range(65):
            index = (0xa000+slot*32)//4
            if slot%16 == mc:
                allowed[index] = True
                observed = after[index].view(f)
                distance = abs(int(after[index])-int(expected.view(np.uint32)))
                if distance > 2:
                    raise ValueError(f'wrong final weight {mc}/{slot}: {observed} vs {expected}')
                errors.append(distance)
        if np.any(before[~allowed] != after[~allowed]):
            raise ValueError('unexpected nonweight DRAM modification')
    print(json.dumps(dict(status='passed', operations=65, updater0=n0, updater1=n1,
                          completion_reordering=bool(reordered), completion_order=order.tolist(),
                          maximum_weight_ulp=max(errors), marker_routes=512*16), indent=2))

if __name__ == '__main__':
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest='mode', required=True)
    g = sub.add_parser('generate')
    g.add_argument('--payload',type=Path,required=True)
    g.add_argument('--rule',type=int,choices=(0,1),required=True)
    g.add_argument('--out',type=Path,required=True)
    c = sub.add_parser('check')
    c.add_argument('--images',type=Path,required=True)
    c.add_argument('--dump',type=Path,required=True)
    c.add_argument('--log',type=Path,required=True)
    a = p.parse_args()
    if a.mode == 'generate': generate(a.payload,a.rule,a.out)
    else: check(a.images,a.dump,a.log)
