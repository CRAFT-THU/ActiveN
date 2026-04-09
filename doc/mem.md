The global memory requests gets mapped to events. For memory controller X, the dest of the event is 0x8000 + X (X starts from 1). The MMIO handler is at 0x8000.

This document contains two parts: how is the memory pages arranged, and how are the event IDs mapped.

## Memory arrangement

By default, all global memories are arranged sequentially. For example, if the memory controler 0 has 16MB memory, and the memory controller 1 has 32MB memory, then the address range 0x80000000-0x80FFFFFF is mapped to memory controller 0, and the address range 0x81000000-0x82FFFFFF is mapped to memory controller 1.

This only matters if the users uses ordinary load/store instructions to access the global memory. The address -> index mapping is handled by the core. If the user directly uses AM to access the global memory, then it should use the address inside the individual memory controller.

## Event Tag mapping

We currently allocates the following events Tag for memory requests / responses.

- 0xFF00: Normal load
  - operand[0]: the address, aligned to size
  - operand[1]: The request ID
  - FIXME: update to have size
- 0xFF01: Normal store
  - operand[0]: the address, aligned to size
  - operand[1][31:16]: The size of the access (log2, can be 0, 1, 2 for now)
  - operand[1][15:0]: The request ID
  - operand[2]: The data to write, replicated to fill 32 bits (e.g. for byte access, the byte is replicated 4 times in operand[2])
- 0xFF02: AMO
  - operand[0]: the address, aligned to size
  - operand[1][15:0]: The request ID
  - operand[1][31:16]: The AMO operation (with size, to be defined)
  - operand[2]: The AMO operand
- 0xFF03: CSR access
  - operand[0]: the start of the row
  - operand[1][31:16]: the length of the access (in 8bytes)
  - operand[1][15:0]: the return tag value (which is scattered)

## Response
Memory response is sent on the memory response bus, which is separated from the normal AM NoC.

The width of the bus is based on a parameter.

## Address map

0x20000000-0x3FFFFFFF: Scratchpad
0x40000000-0x7FFFFFFF: Peripheral
0x80000000-0xFFFFFFFF: Global memory

## Bus Interface Encoding
- Address might no align to the bus width, but is always aligned to the size of the request.
- Data (written data, response data, wbe) are lane aligned.
- Data outside the size of the request is undefined / ignored.