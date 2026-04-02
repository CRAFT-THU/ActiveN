package koneko

import chisel3._
import chisel3.util._

import koneko._

class EvQueue(implicit val param: CoreParameters) extends Module {
  val alignment = IO(Input(UInt(5.W)))
  val enq = IO(Flipped(Decoupled(UInt(32.W))))
  val deq = IO(Decoupled(Vec(4, UInt(32.W))))
  // TODO: arbitrary alignment

  // TODO: configurable queue length
  val ram = Mem(32, Vec(4, UInt(32.W))) // TODO: calculate power as 16 lines
  val cnt = RegInit(0.U(5.W))

  val head = RegInit(0.U(6.W)) // 1 + log2up(32)
  val tail = RegInit(0.U(6.W)) // 1 + log2up(32)

  val empty = head === tail
  val full = head(4, 0) === tail(4, 0) && head(5) =/= tail(5)

  enq.ready := !full
  // TODO: support alignment > 4
  val last = cnt === alignment - 1.U
  when(enq.fire) {
    ram.write(tail(4, 0), VecInit(Seq.fill(4)(enq.bits)), UIntToOH(cnt(1, 0)).asBools)
    cnt := Mux(last, 0.U, cnt + 1.U)
    tail := Mux(last, tail + 1.U, tail)
  }

  when(deq.fire) {
    head := head + 1.U
  }
  deq.valid := !empty
  deq.bits := VecInit(ram.read(head).zipWithIndex.map({
    case (o, i) => Mux(
      head(4, 0) === tail(4,0) && i.U === cnt && enq.fire,
      enq.bits, o
    )
  }))
}

class BIU(implicit val param: CoreParameters) extends Module {
  val msg = IO(Flipped(Decoupled(new Bundle {
    val reg = Vec(2, UInt(32.W))
    val enq = UInt(2.W) // 0, 1, 2
    val send = Bool()
    val target = UInt(16.W)
    val tag = UInt(16.W)
  })))

  val ext = IO(new Bundle {
    // TODO: last
    val out = Decoupled(new Bundle {
      val dst = UInt(16.W)
      val data = UInt(32.W)
      val tag = UInt(16.W)
    })

    val in = Flipped(Decoupled(new Bundle {
      val src = UInt(16.W)
      val data = UInt(32.W)
      val tag = UInt(16.W)
    }))
  })

  val br = IO(Decoupled(new Bundle {
    val regs = Vec(4, UInt(32.W))
    val target = UInt(32.W)
    val argcnt = UInt(5.W)
  }))

  val cfg = IO(Input(Vec(16, new Bundle {
    val handler = UInt(32.W)
    val argcnt = UInt(5.W)
  })))

  // CSR broadcast input: 256-bit data from MemDistributor broadcast
  // Contains up to 4 CSR entries (64 bits each):
  //   entry[i] = data[64*i+63 : 64*i]
  //   entry[i][31:16] = dest_core, entry[i][15:0] = neuron_index
  //   entry[i][63:32] = weight
  val bcast = IO(new Bundle {
    val valid = Input(Bool())
    val data  = Input(UInt(param.memBusWidth.W))
  })
  val hartid = IO(Input(UInt(16.W)))

  //////////////////////////
  // Sending
  //////////////////////////

  val sendingMsg = RegInit(false.B)
  val sendingTarget = RegEnable(msg.bits.target, msg.fire) // Actually the gate cond here can be optimized
  val sendingTag = RegEnable(msg.bits.tag, msg.fire)

  /*
  val sendingMem = RegInit(false.B)
  val sendingMemAddr = RegEnable(ifmem.req.bits.addr, ifmem.req.fire)
  val sendingMemTagSent = Reg(Bool())
  val sendingMemAddrSent = Reg(Bool())
   */

  // Register queues
  val queueEnqs = Seq.fill(2)(Wire(Decoupled(UInt(32.W))))
  val queues = queueEnqs.map(Queue(_, 16))
  val head = RegInit(0.U(1.W))
  val tail = RegInit(0.U(1.W))

  // Enq
  head := head + Mux(msg.fire, msg.bits.enq, 0.U)
  for((q, i) <- queueEnqs.zipWithIndex) {
    q.bits := Mux(i.U === head, msg.bits.reg(0), msg.bits.reg(1))
    q.valid := Mux(i.U === head, msg.bits.enq =/= 0.U, msg.bits.enq === 2.U) && msg.fire && !sendingMsg
    assert(q.ready) // TODO: actually handle queue full
  }

  val enqHasSpace = VecInit(queueEnqs.zipWithIndex.map({ case (q, i) => q.ready && i.U === head })).asUInt.orR

  // Deq
  val regDeq = Wire(Decoupled(UInt(32.W)))
  tail := tail + Mux(regDeq.fire, 1.U, 0.U)
  regDeq.bits := Mux1H(queues.zipWithIndex.map({ case (value, i) => (i.U === tail) -> value.bits }))
  regDeq.valid := sendingMsg && Mux1H(queues.zipWithIndex.map({ case (value, i) => (i.U === tail) -> value.valid }))
  for((q, i) <- queues.zipWithIndex) {
    q.ready := regDeq.ready && i.U === tail && sendingMsg
  }
  val regDrained = VecInit(queues.map(!_.valid)).asUInt.andR

  sendingMsg := MuxCase(sendingMsg, Seq(
    (sendingMsg && regDrained) -> false.B,
    (!sendingMsg && msg.fire && msg.bits.send) -> true.B
  ))
  // msg.ready := !sendingMsg && !sendingMem
  msg.ready := !sendingMsg && enqHasSpace

  // val memMapped = Decoupled(UInt(32.W))
  // memMapped.bits := Mux(sendingMem, sendingMemAddr, param.memTagBase.U)

  // Local send: dst=0 means push to own EvQueue instead of ext.out
  val isLocalSend = sendingMsg && sendingTarget === 0.U

  ext.out.valid    := regDeq.valid && !isLocalSend
  ext.out.bits.dst := sendingTarget
  ext.out.bits.tag := sendingTag
  ext.out.bits.data := regDeq.bits

  //////////////////////////
  // Recv & queues
  //////////////////////////
  val evqueues = Seq.fill(16)(Module(new EvQueue))
  for((e, i) <- evqueues.zipWithIndex) {
    e.alignment := cfg(i).argcnt
  }

  // Forward-declare: true when broadcast drain is writing to evqueues(bcastTag)
  val bcastDraining = Wire(Bool())

  for ((e, i) <- evqueues.zipWithIndex) {
    e.enq.bits := ext.in.bits.data
    e.enq.valid := ext.in.valid && ext.in.bits.tag === i.U
  }

  // Local send: override EvQueue enq for the target tag
  val localSendReady = VecInit(evqueues.zipWithIndex.map { case (e, i) =>
    sendingTag === i.U && e.enq.ready
  }).asUInt.orR

  regDeq.ready := Mux(isLocalSend, localSendReady, ext.out.ready)

  when(isLocalSend && regDeq.valid) {
    for ((e, i) <- evqueues.zipWithIndex) {
      when(sendingTag === i.U) {
        e.enq.valid := true.B
        e.enq.bits  := regDeq.bits
      }
    }
  }

  // Backpressure ext.in when broadcast drain or local send targets the same tag
  ext.in.ready := VecInit(evqueues.zipWithIndex.map({ case (e, i) =>
    val base = e.enq.ready && ext.in.bits.tag === i.U
    val bcastBlock = if (i == 1) bcastDraining else false.B
    val localBlock = isLocalSend && sendingTag === i.U
    base && !bcastBlock && !localBlock
  })).asUInt.orR

  val evdeq = Module(new Arbiter(br.bits.cloneType, 16))
  val evdeqsMapped = evqueues.zipWithIndex.map({ case (e, i) => e.deq.map({ k => {
    val w = Wire(br.bits.cloneType)
    w.regs := k
    w.target := cfg(i).handler
    w.argcnt := cfg(i).argcnt
    w
  }})})

  for((i, j) <- evdeq.io.in.zip(evdeqsMapped)) i <> j
  br <> evdeq.io.out

  //////////////////////////
  // CSR broadcast processing
  //////////////////////////
  // Each 256-bit beat has 4 entries of 64 bits each.
  //   entry[i] = data[64*i+63 : 64*i]
  //   word0 = entry[i][31:0]  -> [31:16] = dest_core, [15:0] = neuron_index
  //   word1 = entry[i][63:32] -> weight (f32)
  // Matching entries (dest_core == hartid) are buffered in a small FIFO
  // and drained into the EvQueue for SPIKE_TAG (tag 1) as 2-word events
  // (word 0 = neuron_index, word 1 = weight).

  val bcastTag = 1 // SPIKE_TAG
  val numEntries = param.memBusWidth / 64 // 4 for 256-bit bus

  // The MemDistributor broadcast path is valid-only, so buffer raw beats here
  // before iterating over per-core matches.
  val bcastBeatQ = Module(new Queue(UInt(param.memBusWidth.W), 256))
  bcastBeatQ.io.enq.valid := bcast.valid
  bcastBeatQ.io.enq.bits  := bcast.data
  bcastBeatQ.io.deq.ready := false.B
  assert(!bcast.valid || bcastBeatQ.io.enq.ready, "BIU broadcast beat queue overflow")

  // Buffer matching entries: each is (neuron_index: UInt(32), weight: UInt(32))
  val bcastBuf = Module(new Queue(Vec(2, UInt(32.W)), 16))
  bcastBuf.io.enq.valid := false.B
  bcastBuf.io.enq.bits  := VecInit(0.U(32.W), 0.U(32.W))

  // On broadcast valid, scan all entries and enqueue matches one at a time
  // using a small FSM to iterate through entries across cycles.
  val bcastPending = RegInit(VecInit(Seq.fill(numEntries)(false.B)))
  val bcastWords   = Reg(Vec(numEntries, Vec(2, UInt(32.W))))
  val bcastActive  = RegInit(false.B)

  when(!bcastActive && bcastBeatQ.io.deq.valid) {
    // Latch matching entries
    bcastBeatQ.io.deq.ready := true.B
    bcastActive := false.B
    for (i <- 0 until numEntries) {
      val word0 = bcastBeatQ.io.deq.bits(64 * i + 31, 64 * i)
      val word1 = bcastBeatQ.io.deq.bits(64 * i + 63, 64 * i + 32)
      val dstCore = word0(31, 16)
      val neuron  = word0(15, 0)
      val matches = dstCore === hartid
      bcastPending(i) := matches
      bcastWords(i)(0) := neuron
      bcastWords(i)(1) := word1
      when(matches) { bcastActive := true.B }
    }
  }

  when(bcastActive) {
    // Find first pending entry
    val which = PriorityEncoder(bcastPending.asUInt)
    bcastBuf.io.enq.valid := true.B
    bcastBuf.io.enq.bits  := bcastWords(which)
    when(bcastBuf.io.enq.fire) {
      bcastPending(which) := false.B
      when(PopCount(bcastPending.asUInt) === 1.U) {
        bcastActive := false.B
      }
    }
  }

  // Drain broadcast buffer into EvQueue for bcastTag, 2 words at a time
  // The EvQueue expects individual word enqueues with alignment=2,
  // so we need to send word0 then word1 sequentially.
  val bcastDrainState = RegInit(false.B) // false = word0, true = word1
  val bcastDrainEntry = Reg(Vec(2, UInt(32.W)))

  // Set bcastDraining: true when broadcast drain wants the EvQueue
  bcastDraining := (!bcastDrainState && bcastBuf.io.deq.valid) || bcastDrainState

  bcastBuf.io.deq.ready := false.B

  when(!bcastDrainState && bcastBuf.io.deq.valid) {
    // word0: enqueue neuron_index
    val eq = evqueues(bcastTag)
    when(eq.enq.ready) {
      // Latch the entry and send word0
      bcastDrainEntry := bcastBuf.io.deq.bits
      bcastBuf.io.deq.ready := true.B
      bcastDrainState := true.B
    }
  }

  // Override EvQueue enq for bcastTag when draining broadcast data
  when(!bcastDrainState && bcastBuf.io.deq.valid && evqueues(bcastTag).enq.ready) {
    evqueues(bcastTag).enq.valid := true.B
    evqueues(bcastTag).enq.bits  := bcastBuf.io.deq.bits(0) // neuron_index
  }
  when(bcastDrainState) {
    evqueues(bcastTag).enq.valid := true.B
    evqueues(bcastTag).enq.bits  := bcastDrainEntry(1) // weight
    when(evqueues(bcastTag).enq.fire) {
      bcastDrainState := false.B
    }
  }
}

// TODO: handles local send