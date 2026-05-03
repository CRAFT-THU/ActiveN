package koneko

import chisel3._
import chisel3.util._

import koneko._
import koneko.bus._
import koneko.Main.pipeCnt
import koneko.Common.DecoupledOps

class BIU(implicit val param: CoreParameters) extends Module {
  // Messaging interface
  // If ready = false, it means that the send fails
  // For nonblocking instructions, the request will be canceled next cycle
  // Local sends is handled outside
  val msg = IO(Flipped(Decoupled(new OutgoingFlit)))

  // Local push
  val push = IO(Flipped(Decoupled(new Bundle {
    val handler = UInt(4.W)
    val regs = Vec(4, UInt(32.W))
  })))

  val liveQuota = IO(Input(UInt(log2Ceil(param.sendQueueDepth + 1).W)))

  val ext = IO(new Bundle {
    val out = Decoupled(new OutgoingFlit)

    val in = Flipped(Decoupled(new Bundle {
      val src = UInt(16.W)
      val data = Vec(4, UInt(32.W))
      val tag = UInt(12.W)
    }))
  })

  // Bitmask for enabled handlers
  val enabled = IO(Input(Vec(16, Bool())))

  // Send out: current schedulable events, or the pushed event at the same cycle (at lower priority)
  val sched = IO(Decoupled(new Bundle {
    val handler = UInt(4.W) // We have 16 handlers
    val regs = Vec(4, UInt(32.W))
  }))

  val margins = IO(Input(
    Vec(16, UInt(log2Ceil(param.sendQueueDepth + 1).W))
  ))

  val bcast = IO(Flipped(Decoupled(new BcastLine)))
  val hartid = IO(Input(UInt(16.W)))

  //////////////////////////
  // Sending
  //////////////////////////

  val sendQueue = Module(new FlitQueue(new OutgoingFlit, param.sendQueueDepth, 4))
  sendQueue.enq <> msg
  ext.out <> sendQueue.deq

  //////////////////////////
  // Receiving
  //////////////////////////

  val evQueues = for (i <- 0 until 16) yield (
    Module(new Queue(Vec(4, UInt(32.W)), 16)).suggestName(s"evq_$i")
  )
  val bcastQueue = Module(new Queue(new BcastLine, param.bcastQueueDepth))

  bcastQueue.io.enq <> bcast

  // Braodcast reinterpret state machine
  val BcastBeats = 256 / (32 + 32) // 32 bits of data, 32 bits of index
  val bcastBeats = bcastQueue.io.deq.bits.line
  val bcastValids: UInt = VecInit(bcastBeats.map(_.pu === hartid)).asUInt
  val bcastSent = RegInit(0.U(BcastBeats.W))
  val bcastAvail = bcastValids & ~bcastSent
  val bcastSel = PriorityEncoderOH(bcastAvail)
  val bcastValid = bcastAvail.orR
  val bcastData = Mux1H(bcastSel, bcastBeats.map(_.data))
  val bcastIdx = Mux1H(bcastSel, bcastBeats.map(_.idx))
  val bcastHandler = bcastQueue.io.deq.bits.tag(3, 0)
  val bcastMapped = Wire(Decoupled(Vec(4, UInt(32.W))))
  bcastMapped.bits(0) := bcastIdx
  bcastMapped.bits(1) := bcastData
  bcastMapped.bits(2) := bcastQueue.io.deq.bits.carried(0)
  bcastMapped.bits(3) := bcastQueue.io.deq.bits.carried(1)
  bcastMapped.valid := bcastValid && bcastQueue.io.deq.valid
  bcastQueue.io.deq.ready := !(bcastAvail.orR) // Everything is sent
  when(bcastQueue.io.deq.fire) {
    bcastSent := 0.U
  }.elsewhen(bcastMapped.fire) {
    bcastSent := bcastSent | bcastSel
  }

  // Arbiter priority:
  // 1. Local push
  // 2. Incoming unicast
  // 3. Incoming broadcast
  val splittedLocal = push.split(16, b => UIntToOH(b.handler(3, 0))).map(_.map(_.regs))
  val splittedUnicast = ext.in.split(16, d => UIntToOH(d.tag(3, 0))).map(_.map(_.data))
  val splittedBcast = bcastMapped.split(16, b => UIntToOH(bcastHandler(3, 0)))
  for (i <- 0 until 16) {
    val arb = Module(new Arbiter(Vec(4, UInt(32.W)), 3)).suggestName(s"ingressArb_$i")
    arb.io.in(0) <> splittedLocal(i)
    arb.io.in(1) <> splittedUnicast(i)
    arb.io.in(2) <> splittedBcast(i)
    evQueues(i).io.enq <> arb.io.out
  }

  //////////////////////////
  // Scheduling
  //////////////////////////

  val scheduleArb = Module(new Arbiter(Vec(4, UInt(32.W)), 16)).suggestName("scheduleArb")
  for (i <- 0 until 16) {
    val schedulable = enabled(i) && (sendQueue.count + margins(i) + liveQuota <= param.sendQueueDepth.U)
    val gated = evQueues(i).io.deq.gatedBy(schedulable).suggestName(s"gated_$i")
    scheduleArb.io.in(i) <> gated
  }
  val scheduled = scheduleArb.io.chosen
  sched.valid := scheduleArb.io.out.valid
  scheduleArb.io.out.ready := sched.ready
  sched.bits.handler := scheduleArb.io.chosen
  sched.bits.regs := scheduleArb.io.out.bits
}