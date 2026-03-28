#!/usr/bin/env python3
"""Generate Verilator port accessor .inc files for system.cpp."""
import sys, os

num_pu = int(sys.argv[1])
num_mc = int(sys.argv[2])
outdir = sys.argv[3]

# mem_ports.inc: per-MC memory request/response ports
with open(os.path.join(outdir, "mem_ports.inc"), "w") as f:
    for i in range(num_mc):
        f.write(f"case {i}: "
                f"p.req_valid = &sys->io_mem_{i}_req_valid; "
                f"p.req_ready = &sys->io_mem_{i}_req_ready; "
                f"p.req_id = &sys->io_mem_{i}_req_bits_id; "
                f"p.req_addr = &sys->io_mem_{i}_req_bits_addr; "
                f"p.req_wdata = sys->io_mem_{i}_req_bits_wdata; "
                f"p.req_wbe = &sys->io_mem_{i}_req_bits_wbe; "
                f"p.req_write = &sys->io_mem_{i}_req_bits_write; "
                f"p.resp_valid = &sys->io_mem_{i}_resp_valid; "
                f"p.resp_id = &sys->io_mem_{i}_resp_bits_id; "
                f"p.resp_rdata = sys->io_mem_{i}_resp_bits_rdata; "
                f"break;\n")

