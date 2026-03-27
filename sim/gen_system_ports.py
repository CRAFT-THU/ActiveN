#!/usr/bin/env python3
"""Generate Verilator port accessor .inc files for system.cpp."""
import sys, os

num_pu = int(sys.argv[1])
num_mc = int(sys.argv[2])
outdir = sys.argv[3]

with open(os.path.join(outdir, "resp_ports.inc"), "w") as f:
    for i in range(num_pu):
        f.write(f"case {i}: p.valid = &sys->io_memResp_{i}_valid; "
                f"p.tag = &sys->io_memResp_{i}_bits_tag; "
                f"p.data = sys->io_memResp_{i}_bits_data; break;\n")

with open(os.path.join(outdir, "out_ports.inc"), "w") as f:
    for i in range(num_mc):
        f.write(f"case {i}: p.valid = &sys->io_memOut_{i}_valid; "
                f"p.ready = &sys->io_memOut_{i}_ready; "
                f"p.src = &sys->io_memOut_{i}_bits_src; "
                f"p.dst = &sys->io_memOut_{i}_bits_dst; "
                f"p.tag = &sys->io_memOut_{i}_bits_tag; "
                f"p.data = &sys->io_memOut_{i}_bits_data; break;\n")
