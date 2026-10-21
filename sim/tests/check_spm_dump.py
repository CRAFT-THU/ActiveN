#!/usr/bin/env python3
"""Check every byte emitted for spm_dump.S; no simulator internals are read."""
import argparse
import json
from pathlib import Path
import struct


def expected_spm(pu, size, line):
    data = bytearray().join(struct.pack('<I', (0x9e3779b9 ^ (pu << 20) ^ i) & 0xffffffff)
                            for i in range(size // 4))
    data[:2 * line] = bytes((pu + 3 * i) & 255 for i in range(2 * line))
    for offset in range(size - line, size, 2):
        struct.pack_into('<H', data, offset, (0xbeef ^ pu ^ offset) & 0xffff)
    for i in range(3 * line // 4):
        struct.pack_into('<I', data, 0x100 + i * 4, (0x76543210 + i * 0x01020304) & 0xffffffff)
    return data


def check(directory, expected_pus, expected_backends, result):
    manifest = json.loads((directory / 'manifest.json').read_text())
    assert manifest['schema'] == 'activen-spm-dump-v1'
    assert manifest['byte_order'] == 'little'
    assert manifest['stop_reason'] == ('failure' if result else 'success')
    assert manifest['result'] == result
    assert manifest['stop_phase'] == 'after_step'
    assert manifest['stop_cycle'] == manifest['last_completed_cycle'] > 0
    assert 0 < manifest['timer_cycles'] < manifest['stop_cycle']
    assert sorted(b['name'] for b in manifest['backends']) == sorted(expected_backends)
    files = {'manifest.json'}
    for backend in manifest['backends']:
        assert backend['num_pu'] == expected_pus
        assert len(backend['spms']) == expected_pus
        assert sorted(s['pu'] for s in backend['spms']) == list(range(1, expected_pus + 1))
        for spm in backend['spms']:
            pu = spm['pu']
            assert spm['size_bytes'] == 16384 and spm['line_bytes'] == 32
            assert spm['file'] == f"{backend['name']}/pu-{pu}.bin"
            prefix = f'system.System.pu_{pu}' if backend['name'] == 'hard' else f'soft_pu_{pu}.Core'
            assert spm['scope'] == prefix + '.exec.lsu.spm.spm_dpi'
            actual = (directory / spm['file']).read_bytes()
            expected = expected_spm(pu, spm['size_bytes'], spm['line_bytes'])
            if actual != expected:
                mismatch = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b), None)
                raise AssertionError(f"{spm['file']}: {len(actual)} bytes, first mismatch {mismatch}")
            files.add(spm['file'])
    assert files == {str(p.relative_to(directory)) for p in directory.rglob('*') if p.is_file()}
    return {'backends': expected_backends, 'pus_per_backend': expected_pus,
            'bytes_checked': expected_pus * len(expected_backends) * 16384,
            'timer_cycles': manifest['timer_cycles'], 'stop_cycle': manifest['stop_cycle']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--pus', required=True, type=int)
    parser.add_argument('--backends', nargs='+', required=True)
    parser.add_argument('--result', type=int, default=0)
    args = parser.parse_args()
    print(json.dumps(check(args.directory, args.pus, args.backends, args.result)))
