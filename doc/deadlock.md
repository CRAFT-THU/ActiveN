# Deadlock avoidance

## Fabric
The mesh fabric itself is routing deadlock free, in the sense that:
- Each packet has at most one flit, so there is no wormhole-style locking and related resource allocation problem.
- We use axis-directed routing, so there are no cyclic channel allocation dependencies.
- There are four different priority levels. Although we use a shared queue, lower priority flits cannot fill up a queue and block higher priority flits from passing through.

The highest 2 bits of the tag determines the priority. 00 is the highest priority.

So a message with tag 0b01... will not be able to ingress into a router if the remaining space of the queue is less than 2. Router always selects the highest priority candidate message (in the queue) to forward, so the higher priority message will be able to pass through.

## Protocol

User-defined protocol should be careful about deadlock. We did not place any restriction on the handler priority for each type of messages, but it's recommended to assign higher priority handlers to higher priority messages.

Since we uses nonpreemptive scheduling, we provide a auxillary mechanism for avoiding filling up: each handler have a **queue margin** CSR, which can be set to a value in [0, queue depth). When the space in the egress queue is less than the margin, the handler will not schedule that handler, instead it will schedule other handlers that might have lower priority. Note that there might be no handler that's scheduable at a specific time.

Logically, you should make sure that the message dependency graph is acyclic, and assign the priorities accordingly.

A rule of thumb in designing a deadlock-free protocol:
1. For a handler triggered by a message of priority P, it should only send messages of priority higher than P. Note that sending messages of the same priority is not safe: some other source may inject messages of this priority, and the NoC may fill up.
2. Assign the queue margin based on the following criteria:
  - If this handler does not send any message whatsoever, set the margin to 0.
  - Else, make sure that the margin is the number of messages that this handler can send, **plus** $sum_{p \in "(strictly) higher priority"} max_{h \in "handlers of messages of priority p"} sent(h)$, where $sent$ stands for the maximum number of messages that handler $h$ can send during one execution. In this way, higher priority messages can always be consumed after this handler is scheduled once.

TODO: make sure that when the margin check works with SMT. Assume a running thread already consumes it's share of margin.
TODO: Now every module should no longer assume irrevocable Decoupled interface. Distributor should consume and buffer one beat of **each type of response** (so that broadcast won't block unicasts). Don't rely on incoming Decoupled without accepting them. They might get retracted.
TODO: can we make sure that unicast memory responses always gets consumed in a single cycle?

## Memory requests / responses

Memory responses is not delivered through NoC, rather through the MemIf Ringbus / Distributors, and enters the PU queue from a different path.

You need to make sure that the assigned event queue for these kind of messages will always have space for the incoming messages, and the handler can always consume them (i.e. have high enough priority). We recommend assign a dedicated handler / event queue for scatter memory responses.

Logically, they are depended upon by the memory requests (because in theory they can backpressure the memory controllers, and then backpressure the highest priority messages in the NoC). So if your handler for memory scatters send messages, please make sure the original memory requests has the correct priority: the high bits of the memory request tag is ignored by MemIf, and should be assigned based on wanted priority.

Broadcast memory responses always has lower priority than unicast memory responses in the MemIf response ringbus. This is to make sure that ICache refill is not blocked by any other memory responses.

For local PU ingress, memory responses, if translated as events (i.e. scatter responses), it's arbitrated with NoC's ejection into this PU according to their priority.

## Bounded event relaxation

If you have a type of event that:

> It's number is bounded globally, and it's sending pattern guarantees that the receiving end can always buffer them into the event queue (so not blocking the NoC).

Then we can relax the handler's sending restriction to allow sending messages of the same priority. Think of these kind of events as "tokens" that cannot be generated infinitely.

A specific useful example of this is the local sending pattern to try to emulate preemptive scheduling, where a handler sends an event to the PU itself to re-trigger the handler.

User should be much more careful with these kind of assumptions, because if it actually locks the NoC, it can potentially cause a deadlock.

## SMT Quota

Since each core can have multiple concurrent threads, we need to make sure that SMT threads will not contend for the same queue space. Each handler will have a **quota** CSR that contains the actual number of messages that this handler wants to send during this execution. This value should always be less than or equal to its margin.

When a handler is scheduled onto a thread, it's quota will be copied into a thread-specific **quota counter**. The scheduability of the handlers will take into account the quota counters of all live threads.

When a thread actually sends a message, its quota counter will decrement. Currently we don't hard error on quota underflow, because we don't have instruction exception.

For handlers that may generate unbounded number of messages, we have dedicated instructions for querying the remaining quota, so that the software can try to avoid sending too much message.