  # CSR scatter test — dram.1 image (MC1 data)
  #
  # Layout: one 64-byte zero memory line + CSR1 data
  # This gets assembled into a flat binary for dram.1.

  .section .text

  # One zero memory line so CSR1 begins at offset 0x40.
  .word 0, 0, 0, 0
  .word 0, 0, 0, 0
  .word 0, 0, 0, 0
  .word 0, 0, 0, 0

  # CSR1 data starts at offset 0x40
#include "csr_scatter_data.S"
  CSR1
