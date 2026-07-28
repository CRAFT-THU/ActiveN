// Memory response distributor. Operates within a cluster (16 PUs).
// Buffers at most one memory line of response data.
// Accepts from MemIf resp port, delivers to individual PU mem ports.
//
// Two modes:
//   Unicast (dst = specific PU ID): delivers to that one PU.
//   Broadcast (dst = 0xFFFF): delivers to ALL 16 PUs simultaneously.
//     Used for CSR-aware memory scatter. Each PU's BIU inspects the
//     memory-line payload and picks out entries addressed to it.
//
// PU ID range: [puStart, puStart + 16)

package koneko.bus

import chisel3._
import chisel3.util._

import koneko._

class DistributorOutput(implicit val param: CoreParameters) extends Bundle {
  val unicast = Output(new Bundle {
    val resp = new MemResp
    val valids = UInt(16.W)
  })
  val broadcast = new Bundle {
    val resp = Output(new BcastLine)
    val valids = Output(UInt(16.W))
    val readies = Input(UInt(16.W))
  }
}

class Distributor(
  puStart: Int, // First PU ID in this cluster (1-based)
)(implicit val param: CoreParameters) extends Module {
  val in = IO(new Bundle {
    val unicast = Flipped(Valid(new RingResp))
    val broadcast = Flipped(Decoupled(new BcastLine))
  })

  val out = IO(new DistributorOutput)

  // Unicast
  val unicast = Common.pipeValid(in.unicast)
  out.unicast.resp.id := unicast.bits.id
  out.unicast.resp.data := unicast.bits.data
  out.unicast.resp.ident := unicast.bits.ident
  out.unicast.valids := Fill(16, unicast.valid) & UIntToOH(unicast.bits.dst - puStart.U, 16)

  // Single slot queue for broadcasts
  val bcstQueue = Module(new Queue(new BcastLine, 2))
  bcstQueue.io.enq <> in.broadcast

  // Broadcast
  // Whether this PU already accepted this packet
  val bcstAccepted = RegInit(0.U(16.W))
  // Whether each PU is hit with this specific broadcast
  val bcstValids = bcstQueue.io.deq.bits.line.map(e => {
    val inRange = e.pu -% puStart.U < 16.U
    Fill(16, inRange) & UIntToOH(e.pu - puStart.U, 16)
  }).reduce(_ | _)
  // Block if there is a PU that: is valid, do not accept, not accepted yet
  val bcstWait = (~(bcstAccepted | out.broadcast.readies) & bcstValids).orR
  bcstQueue.io.deq.ready := !bcstWait
  out.broadcast.valids := Fill(16, bcstQueue.io.deq.valid) & bcstValids & ~bcstAccepted
  out.broadcast.resp := bcstQueue.io.deq.bits
  bcstAccepted := Mux(
    bcstQueue.io.deq.fire,
    0.U,
    bcstAccepted | (out.broadcast.valids & out.broadcast.readies)
  )
}
