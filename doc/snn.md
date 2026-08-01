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
- end - 0xC (4 bytes): the type of neurons during initialization:
  - 0x0: current-based FP32 LIF neurons.
  - 0x1: conductance-based FP32 LIF neurons.
- Before that: neuron group specific data.
  - For FP32 LIF neurons:
    - end - 0x10 (4 bytes): the threshold for firing, in FP32
    - end - 0x14 (4 bytes): the decay factor, in FP32 (e^-tau)
- end - 0x24 (4 bytes): expected post-timestep numerical checksum. This value
  is repeated in every PU image so the leader can read its local copy.

The words from end - 0x18 through end - 0x20 are reserved for runtime
termination state. After initialization, the type word at end - 0x0C is reused
for packed drain/final-preparation counters. The word at end - 0x28 stores the
MC-domain-size marker in its high 16 bits and tracks overlapped next-step
preparation in its low 16 bits. These runtime fields are initialized by the
payload before use.

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

## Numerical verification and termination

After each PU finishes its neuron update, completion is reduced through a
binary tree over the 1-based PU IDs. For nonroot zero-based index `i`,
`lowbit(i)` is the size of its subtree and subtracting that value from its
1-based PU ID gives its parent. A node sends one message containing its complete
subtree size after both its local update and all child subtrees complete. PU1
therefore receives only the completed top-level subtrees rather than one report
from every PU.

PU1 then passes an idle-notifier installation token through the first PU in
each MC domain. Domain size is computed once during initialization and cached
only on those leaders. A domain leader registers a notifier only with its local
MemIf. When that MemIf reports the configured idle interval, the leader starts
the selected lowest-priority drain protocol. The default `SNN_DRAIN_MODE=1`
uses a reverse daisy chain whose ordinary PUs unconditionally forward to the
preceding PU. `SNN_DRAIN_MODE=2` broadcasts and collects over aligned local
trees before continuing collection over the corresponding subleader tree.
Only the selected implementation is emitted into the payload. Since spike
handlers have higher priority, either drain protocol runs only after the spike
events already queued at each visited PU.

Each PU disables its spike handler before forwarding the drain token, then uses
the lowest-priority local handler to begin adding its accumulated input to
neuron state and clearing the accumulator for the next step. Once all domains
report completion, PU1 starts the next step with a high-priority token. Each PU
forwards that token, synchronously finishes any remaining preparation, and
re-enables the spike handler. Spikes from PUs that start the next step earlier
remain buffered at a later PU until its preparation is complete, so this
transition needs no additional global barrier.

The drain protocol also accumulates the optional numerical checksum. Each PU
quantizes its final mutable neuron fields by multiplying them by 10.0 in FP32
and converting to a signed integer with round-to-nearest, ties-to-even. It XORs
those words into the token. Current-based neurons include state and input;
conductance neurons include g_e, g_i, and membrane potential. PU1 XORs the
completed-domain values and compares the result with the expected checksum at
end - 0x24. A checksum XOR difference of at most 15 is accepted to tolerate
small FP accumulation-order differences.

For current-based neurons, the final timed step also performs the following
state preparation before the timer stops. A separate tree reduction occupies
the high 16 bits of the packed drain counter, allowing preparation completion
and asynchronous domain draining to overlap without corrupting either count.

Numerical verification is enabled by default. Performance payloads can compile
it out while retaining the idle and domain-drain termination barrier with:

```sh
make -B sys/snn_main.bin NUM_PU=64 SNN_VERIFY_RESULT=0
```

The payload defaults to one warmup step followed by four timed steps. Override
these compile-time counts with `SNN_WARMUP_STEPS` and `SNN_TIMED_STEPS`. When
generating its DRAM image, pass their sum as datagen's `--runtime-steps` so its
per-step firing counts and final checksum cover the same interval.
