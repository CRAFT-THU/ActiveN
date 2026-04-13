The active messaging system primarily contains two ISA extensions: a handler registration mechanism through custom CSRs, and a set of instruction to facilitate sending messages.

## Handler registration

This part of code needs revision, document will be updated after the code is stabilized.

## Message sending instructions

Message sending instructions uses one or more source registers to specify the destination, the tag and the payload of the message.

The payload generation will be changed, so it directly sources its operands from regfile, so that it looks more like a direct RPC (with correct ABI).

There is a special flag on the fire instruction: `yield`, that has the semantics of immediately executing a `WFI` after sending the message. It also allows same-cycle re-scheduling of the called handler if the dst is local, and there is no other pending messages. Specifically, if there is a lower-priority message, it will be scheduled, and the outgoing message will be placed in the event queue.