package koneko

import chisel3._
import chisel3.util._

import koneko._

class BIU(implicit val param: CoreParameters) extends Module {
  val msg = IO(Flipped(Decoupled(new Bundle {
    val reg = Vec(2, UInt(32.W))
    val enq = UInt(2.W) // 0, 1, 2
    val send = Bool()
    val target = UInt(16.W)
    val tag = UInt(16.W)
  })))

  val ext = IO(new Bundle {
    val out = Decoupled(new Bundle {
      val dst = UInt(16.W)
      val data = Vec(4, UInt(32.W))
      val tag = UInt(16.W)
    })

    val in = Flipped(Decoupled(new Bundle {
      val src = UInt(16.W)
      val data = Vec(4, UInt(32.W))
      val tag = UInt(16.W)
    }))
  })

  val br = IO(Decoupled(new Bundle {
    val regs = Vec(4, UInt(32.W))
    val target = UInt(32.W)
    val argcnt = UInt(2.W)
  }))

  val cfg = IO(Input(Vec(16, new Bundle {
    val handler = UInt(32.W)
    val argcnt = UInt(2.W)
  })))

  val bcast = IO(new Bundle {
    val valid = Input(Bool())
    val data  = Input(UInt(param.memBusWidth.W))
  })
  val hartid = IO(Input(UInt(16.W)))

  //////////////////////////
  // Sending
  //////////////////////////

  // Staging registers: accumulate up to 4 words across msg instructions
  val staged = Reg(Vec(4, UInt(32.W)))
  val stagedCnt = RegInit(0.U(3.W)) // 0..4
  val sending = RegInit(false.B)
  val sendTarget = Reg(UInt(16.W))
  val sendTag = Reg(UInt(16.W))

  // Accept msg when not in sending state
  msg.ready := !sending

  when(msg.fire) {
    // Enqueue registers into staging area
    when(msg.bits.enq >= 1.U) {
      staged(stagedCnt) := msg.bits.reg(0)
    }
    when(msg.bits.enq >= 2.U) {
      staged(stagedCnt + 1.U) := msg.bits.reg(1)
    }
    stagedCnt := stagedCnt + msg.bits.enq

    when(msg.bits.send) {
      sending := true.B
      sendTarget := msg.bits.target
      sendTag := msg.bits.tag
    }
  }

  // Local send: dst=0 means dispatch to own handler instead of ext.out
  val isLocalSend = sending && sendTarget === 0.U

  // Drive ext.out for remote sends
  ext.out.valid := sending && !isLocalSend
  ext.out.bits.dst := sendTarget
  ext.out.bits.tag := sendTag
  ext.out.bits.data := staged

  // Local send bypasses ext and goes directly to br arbitration
  // (handled below in the receive section)

  // sendDone is set when the flit is actually consumed:
  // - Remote send: ext.out.fire
  // - Local send: br.fire && localWins (deferred, connected after br arbiter)
  val localSendDone = Wire(Bool())
  localSendDone := false.B // default, overridden after arbiter
  val sendDone = Mux(isLocalSend, localSendDone, ext.out.fire)
  when(sendDone && sending) {
    sending := false.B
    stagedCnt := 0.U
  }

  //////////////////////////
  // Receiving: single-cycle dispatch
  //////////////////////////
  // With Vec(4, UInt(32.W)) data, we can write all 4 regs and schedule
  // the handler in one cycle. No EvQueue needed.

  // Arbitrate between: ext.in (remote), local send, and broadcast
  // All produce (regs, tag) -> look up handler + argcnt -> br output

  // Broadcast processing
  val bcastTag = 1.U // SPIKE_TAG
  val numEntries = param.memBusWidth / 64 // 4 for 256-bit bus

  // Buffer raw broadcast beats
  val bcastBeatQ = Module(new Queue(UInt(param.memBusWidth.W), 256))
  bcastBeatQ.io.enq.valid := bcast.valid
  bcastBeatQ.io.enq.bits  := bcast.data
  bcastBeatQ.io.deq.ready := false.B
  assert(!bcast.valid || bcastBeatQ.io.enq.ready, "BIU broadcast beat queue overflow")

  // Broadcast match FSM: extract matching entries from beat
  val bcastPending = RegInit(VecInit(Seq.fill(numEntries)(false.B)))
  val bcastRegs    = Reg(Vec(numEntries, Vec(4, UInt(32.W))))
  val bcastActive  = RegInit(false.B)

  when(!bcastActive && bcastBeatQ.io.deq.valid) {
    bcastBeatQ.io.deq.ready := true.B
    bcastActive := false.B
    for (i <- 0 until numEntries) {
      val word0 = bcastBeatQ.io.deq.bits(64 * i + 31, 64 * i)
      val word1 = bcastBeatQ.io.deq.bits(64 * i + 63, 64 * i + 32)
      val dstCore = word0(31, 16)
      val neuron  = word0(15, 0)
      val matches = dstCore === hartid
      bcastPending(i) := matches
      // Pack as 4-reg event: [neuron, weight, 0, 0]
      bcastRegs(i)(0) := neuron
      bcastRegs(i)(1) := word1
      bcastRegs(i)(2) := 0.U
      bcastRegs(i)(3) := 0.U
      when(matches) { bcastActive := true.B }
    }
  }

  // Pick first pending broadcast entry
  val bcastWhich = PriorityEncoder(bcastPending.asUInt)
  val bcastValid = bcastActive
  val bcastData  = bcastRegs(bcastWhich)

  // 3-way priority arbiter for br output:
  //   1. ext.in (remote message)
  //   2. local send
  //   3. broadcast
  val brValid = Wire(Bool())
  val brRegs  = Wire(Vec(4, UInt(32.W)))
  val brTag   = Wire(UInt(16.W))

  // Priority: ext.in > local send > broadcast
  val extInWins   = ext.in.valid
  val localWins   = !extInWins && isLocalSend
  val bcastWins   = !extInWins && !isLocalSend && bcastValid

  brValid := extInWins || localWins || bcastWins
  brTag   := MuxCase(0.U, Seq(
    extInWins  -> ext.in.bits.tag,
    localWins  -> sendTag,
    bcastWins  -> bcastTag,
  ))
  brRegs := MuxCase(VecInit(0.U, 0.U, 0.U, 0.U), Seq(
    extInWins  -> ext.in.bits.data,
    localWins  -> staged,
    bcastWins  -> bcastData,
  ))

  // Look up handler and argcnt from tag
  val brHandler = cfg(brTag).handler
  val brArgcnt  = cfg(brTag).argcnt

  br.valid       := brValid
  br.bits.regs   := brRegs
  br.bits.target := brHandler
  br.bits.argcnt := brArgcnt

  // Backpressure / consumption
  ext.in.ready := br.ready && extInWins

  // Local send completes only when br actually fires for the local send
  localSendDone := br.fire && localWins

  // Broadcast: consume pending entry when br fires from broadcast
  when(bcastWins && br.fire) {
    bcastPending(bcastWhich) := false.B
    when(PopCount(bcastPending.asUInt) === 1.U) {
      bcastActive := false.B
    }
  }
}

// TODO: handles local send