The active messaging system primarily contains two ISA extensions: a handler registration mechanism through custom CSRs, and a set of instruction to facilitate sending messages.

## Handler registration

Each handler has five piece of data, that's encoded in three CSRs. In case of the handler for message tag 0:

1. 0x700 + ID: `handler0`: The address of the handler function
2. 0x710 + ID: `hancfg0`: The configuration of the handler, which includes:
   - Lowest 3 bits: The number of argument for this handler. Although we use a 3-bit field, at most four registers can be passed.
   - Higher bits from bit 3: a bitmask indicating which of the SMT threads that this handler can be scheduled onto. So bit 3 is for thread 0, bit 4 is for thread 1. Right now there is only two threads at most.
3. 0x720 + ID: `hanmargin0`: The quota and margin for this handler.
  - Low 16 bits: Margin
  - High 16 bits: Quota

### Reset state

During reset, all two SMT threads are active. register a0 contains the ID of the SMT thread (0, 1, ...), **not hartid**.

Handlers for each message are all reset to 0.

The cfg CSR is reset to 0. Crucially, this means that the handler by default won't be schedulable.

Both quota and margin is reset to 0. User should set then correctly before enabling the handler.

### Setup procedure

User is expected to set up each handler in the following way:

First, set the handler function address, quota and margin. During this time, the handler is not yet enabled.

Then, if the handler can be immediately triggered in the other SMT thread, we can enable it in a single `CSRWI` instruction. Note that we specifically uses 5 bits for the handler index (for 2 SMT threads). So the user can directly writes `(3 << 3) | argcnt` to enable the handler with argument count `argcnt`.

If the handler cannot be enabled for everyone, or is intended to be scheduled onto certain ones, you can use `CSRS` to enable it for individual threads.

A example for this is the SNN case. Each thread needs to setup some persistent registers before being able to receive messages. However, only thread 0 does the SPM initialization. So the startup sequence should be:

1. Thread 0 initializes SPM, while thread 1 register a handler "spm_init_finished" that can only be scheduled onto thread 1. Note that the two threads share CSRs, so it need a dedicated handler index.
2. Thread 1 enters WFI.
3. After thread 0 initializes SPM and its own registers, it enables the handlers for external messages, but only enabled for thread 0. It then triggers the "spm_init_finished" handler, then enters jumps to the main work loop.
4. Meanwhile, thread 1 eventually wakes up to execute "spm_init_finished" handler. It reads the SPM and setup the registers, then `CSRS ..., 1 << tid` to add itself into the schedulable pool onto handlers. Then it WFIs, and waits for external messages.

## ABI

Message sending / receiving behaves like a direct RPC call to a function with maximum 4 arguments. So a send instruction will implicitly read a0, a1, a2, a3 as the payload of the message, and when a handler is triggered, these registers will be written with the payload of the message. Precisely how much of the register is overwritten (i.e. the arity of the handler) is determined by the handler itself (through a CSR).

## Message sending instructions

Message sending instruction has two forms: a R-type form and an I-type form.

The R-type form takes two arguments: rs1 is the destination of the message, and rs2 is the tag. Tag will be truncated to 12-bit, and destination will be truncated to exactly 16-bit. There is one special values for the destination: 0, which stands for local PU.

The I-type form statically gives the tag, and only take rs1 (the destination).

The funct3 field of the instruction encode other special meanings:
- Bit 0: If set, marks this send as a "yield" send. Semantically, it's equivalent to executing a WFI immediately after **successfully** sending the message. Specifically, if there is a lower-priority message, it will be scheduled, and the outgoing message will be placed in the event queue.
  A special optimization is that if the destination is 0 (this does not include using the local PU ID, only 0), and there is no pending message, then the handler will be immediately scheduled during the same cycle, and the message will not be put into the event queue.
- Bit 1: If bit 1 is set, marks this send as a "non-blocking" send. If if there is not enough quota (live quota === 0), the instruction will fail. See the section below.

Memnomic for the instruction: `send{.yield}{.nb} rd, dest, tag` and `sendi{.yield}{.nb} rd, dest`.

The result of this instruction marks the successfulness of the send. Writes back 1 if the send is successful, and 0 if it fails. Note that with blocking send, this instruction will effectively always succeed. With yield, then the software will also never observe a successful send, although the writeback will be performed.

## Margin & Quota

To avoid deadlock, each handler have a **margin** CSR which stands for the minimum required space in the egress queue for this handler to be scheduled. It also have a live **quota** counter for remaining margin during this one execution. See deadlock.md for more details.

We have additional instruction for querying and manipulating the quota during runtime:

- `quota{.to}{.dealloc}` and `quotai{.to}{.dealloc}` will try to allocate additional quota for the current live execution. rs1 the the safe margin **after this allocation** (so is analogous to the margin CSR minus the quota CSR, the spece "left for higher priority handlers"). rs2 / the immediate is the amount of quota to allocate (or the wanted quota after the allocation if `.to` is used).
  This instruction writes back the updated quota. If you want to query for quota, use `quota rd, x0, x0`.
  By default, the quota will never decrease, so if you give in a negative number or a number smaller than the current quota with `.to`, this instruction will not change the quota. If you really want to deallocate quota, you can use the `.dealloc` suffix. Note that even with deallocation, the quota will never be negative.
  Semantics: bump quota if:
  - Target quota >= 0
  - send queue space >= target quota + sum(all **other** live threads' quota) + margin given in the instruction

We explicitly did not include a query instruction for remaining spaces, because during SMT execution, this value is highly suspectible to racing. Software should use quota to protect itself from over-saturating the send queue, and be really careful when dynamically allocating quota.
