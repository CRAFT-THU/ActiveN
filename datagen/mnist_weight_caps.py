"""Static weight caps from structural fan-in (including present zero weights)."""
from __future__ import annotations

import hashlib
import struct

POLICY = "fan-in"


def fan_in_caps(mask, n_input=784, n_post=400):
    if len(mask) != n_input * n_post:
        raise ValueError("structural mask has the wrong size")
    degrees = [sum(bool(mask[pre * n_post + post]) for pre in range(n_input))
               for post in range(n_post)]
    # Empty columns have no weights to update. Training normalization separately
    # rejects them; zero is a finite sentinel for directed routing tests.
    caps = [struct.unpack("<f", struct.pack("<f", n_input / degree))[0]
            if degree else 0.0 for degree in degrees]
    return degrees, caps


def caps_sha256(caps):
    return hashlib.sha256(struct.pack(f"<{len(caps)}f", *caps)).hexdigest()
