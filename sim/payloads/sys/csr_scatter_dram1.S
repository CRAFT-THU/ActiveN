  # CSR scatter test image for every non-boot memory controller.
  #
  # Layout: one zero memory line followed by the common CSR table. The runtime
  # payload uses this same offset for MC1 through MC(N-1), and each MemIf
  # filters the table to the PUs in its own zone.

#ifndef MEM_LINE_BYTES
#define MEM_LINE_BYTES 64
#endif

  .section .text

  # Keep offset zero empty so the table starts at one configured memory line.
  .space MEM_LINE_BYTES

#include "csr_scatter_data.S"
  CSR_DATA
