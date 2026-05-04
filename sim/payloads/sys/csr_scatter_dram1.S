  # CSR scatter test — dram.1 image (MC1 data)
  #
  # Layout: 16-byte zero header + CSR1 data
  # This gets assembled into a flat binary for dram.1.

  .section .text

  # 32-byte header (zeros for non-primary DRAM images)
  # 32 bytes so CSR1's .balign 32 is a no-op and data starts at offset 0x20
  .word 0, 0, 0, 0
  .word 0, 0, 0, 0

  # CSR1 data starts at offset 0x20
#include "csr_scatter_data.S"
  CSR1

