"""Read observer dumps and compare logical sparse state across physical layouts."""
from __future__ import annotations

import hashlib
from pathlib import Path
import struct

from mnist_sparse import HEADER, INPUT_SIZE, EXC_SIZE, INH_SIZE


def word(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def header(data: bytes, name: str) -> int:
    return word(data, HEADER[name])


def read_dump(path: Path) -> list[bytes]:
    data = path.read_bytes()
    magic, count = struct.unpack_from("<II", data)
    if (magic, count) != (0x4D535044, 512):
        raise ValueError("unsupported sparse observer dump")
    result, position = [], 8
    for index in range(count):
        pu, size = struct.unpack_from("<II", data, position)
        position += 8
        if pu != index + 1 or not 256 <= size <= 0x3000:
            raise ValueError("invalid sparse observer PU record")
        result.append(data[position:position + size])
        position += size
    if position != len(data):
        raise ValueError("truncated or oversized sparse observer dump")
    return result


def logical_state(spms: list[bytes]) -> tuple[dict, dict, dict]:
    weights, inputs, neurons = {}, {}, {}
    for spm in spms:
        for local in range(header(spm, "INPUT_COUNT")):
            state = header(spm, "INPUT_BASE") + local * INPUT_SIZE
            identity = word(spm, state) & 0x7fffffff
            inputs[(identity, header(spm, "GROUP"))] = tuple(word(spm, state + off) for off in (8, 12, 16))
            begin, count = struct.unpack_from("<II", spm, state + 20)
            for edge in range(begin, begin + count * 8, 8):
                dest, descriptor = struct.unpack_from("<HH", spm, edge)
                target = spms[dest - 1]
                eo = header(target, "EXC_BASE") + (descriptor & 1) * EXC_SIZE
                post = header(target, "GROUP") * 50 + word(target, eo + 32)
                if (identity, post) in weights:
                    raise ValueError("duplicate physical weight ownership")
                weights[(identity, post)] = word(spm, edge + 4)
        for local in range(header(spm, "EXC_COUNT")):
            eo = header(spm, "EXC_BASE") + local * EXC_SIZE
            io = header(spm, "INH_BASE") + local * INH_SIZE
            post = header(spm, "GROUP") * 50 + word(spm, eo + 32)
            neurons[post] = (tuple(word(spm, eo + off) for off in range(0, 28, 4))
                            + tuple(word(spm, io + off) for off in range(0, 20, 4)))
    return weights, inputs, neurons


def state_hashes(path: Path) -> dict[str, str]:
    return {name: hashlib.sha256(repr(sorted(values.items())).encode("ascii")).hexdigest()
            for name, values in zip(("weights", "inputs", "neurons"), logical_state(read_dump(path)))}
