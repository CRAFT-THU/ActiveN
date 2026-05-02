The active messaging system primarily contains two ISA extensions: a handler registration mechanism through custom CSRs, and a set of instruction to facilitate sending messages.

## Handler registration

This part of code needs revision, document will be updated after the code is stabilized.

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