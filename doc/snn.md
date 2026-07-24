# SNN DRAM layout

For SNN payloads, the layout of the DRAM images is specified as follows. This affects how the SNN workload reads them, as well as how the data generator produces them.

## Shared conventions

For all workloads, we assume that each memory controller has a size of 4GiB. Since we're using a 32-bit address space, only the first half of the first DRAM image is directly accessibile with standard load/store/AMO instructions. However, CSR scatter and bulk loads uses local addresses, and can access the content of other DRAM images.

The first DRAM image contains a header:
- First 4 bytes: a jump instruction that actually jumps after the header.
- Second 4 bytes: the offset inside this image where the SPM initializer headers are situated.
- Third 4 bytes: the number of expected memory controllers
- Fourth 4 bytes: the number of expected PUs

The first 16 bytes of other DRAM images should be zero.

The CSR section in every image begins at the next configured memory-line
boundary after the header and, for image 0, the executable. Padding bytes are
zero. Each per-neuron CSR row is also padded through the end of a memory line,
because scatter responses do not include a validity mask for individual
entries.

After the headers, each DRAM image will contain:
- (Only DRAM image 0): The executable.
- The CSR synapse connection data (or other opaque data)
- (Only DRAM image 0): The SPM initializer headers and data

## SPM initializer header format

The SPM initializer header is a list of individual descriptors of length numPU. Each descriptor is 8 bytes, which contains:
- First 4 bytes: the offset of the SPM initializer data for this PU, relative to the start of the initializer (including the header, i.e. relative to the content of the second 4 bytes in the DRAM image 0 header).
- Second 4 bytes: the size of the SPM initializer data for this PU, in bytes.

## SPM content format

The SPM contains a list of neuron states, and at the end of the SPM, a small descriptor that describes the work to be done for this PU.

Each LIF neuron state contains:
- state (4 bytes): the current state of the neuron in FP32
- input (4 bytes): the input accumulator for the current timestep
- starts (4 bytes * numMC): the (local) offsets of the synapse connection data for this neuron in each DRAM image, relative to the START OF THE DRAM IMAGE (so includes the 16 bytes of header).

Each conductance neuron state contains:
- g_e (4 bytes): excitatory conductance in FP32
- g_i (4 bytes): inhibitory conductance in FP32
- membrane potential (4 bytes): membrane potential in FP32 volts
- metadata (4 bytes): bit 0 is the neuron type (0 excitatory, 1 inhibitory); other bits are reserved
- starts (4 bytes * numMC): the per-image CSR offsets described above

The list is terminated by a dummy neuron state: the starts is the end of the synapse connection data in each DRAM images. See the simulation flow below for how this is used.

The end of the SNN SPM contains a descriptor of the data:
- end - 0x4 (4 bytes): the total number of neurons in this PU (not including the dummy neuron at the end)
- end - 0x8 (4 bytes): the size of each neuron state (8 + 4 * numMC bytes for type 0; 16 + 4 * numMC bytes for type 1)
- end - 0xC (4 bytes): the type of neurons:
  - 0x0: current-based FP32 LIF neurons.
  - 0x1: conductance-based FP32 LIF neurons.
- Before that: neuron group specific data.
  - For FP32 LIF neurons:
    - end - 0x10 (4 bytes): the threshold for firing, in FP32
    - end - 0x14 (4 bytes): the decay factor, in FP32 (e^-tau)

## CSR synapse data format

The CSR synapse format matches the ISA definition for scatter messages. Each data point is 8 bytes, which contains:
- First 2 bytes: the ID of the receiving PU (small endian in itself)
- Next 2 bytes: the sub-index of the neuron in the receiving PU (small endian in itself). This will be passed as a0 to the handler.
- Next 4 bytes: the weight of the synapse, in whatever format, passed as a1 (small endian)

For conductance neurons, the scatter request also carries the presynaptic
metadata. The response handler receives it as a2 and uses bit 0 to add the
positive CSR weight to either the postsynaptic g_e or g_i accumulator.

See Flit.scala and BIU.scala for detail.

# SNN simulation flow

For type 0, the current input is accumulated into the state and then cleared at
the start of each timestep. Type 1 images instead contain the pre-simulated
conductances and membrane potential directly, so this initialization step does
not modify their neuron records.

Then, each PU re-iterate through its neurons, look for fired neurons, and for each fired neuron, resets it state, and sends out the corresponding CSR scatter messages to ALL DRAM controllers.

The length of the scatter request is computed by the difference between the starts of the next neuron and the current neuron.
This byte difference is shifted by the line-size value read from configuration
ROM to obtain the request's memory-line count.
