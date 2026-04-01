// Memory response distributor. Operates within a cluster (16 PUs).
// Buffers at most one beat of response data (256 bits).
// Accepts from MemIf resp port, delivers to individual PU mem ports.
//
// Two modes:
//   Unicast (dst = specific PU ID): delivers to that one PU.
//   Broadcast (dst = 0xFFFF): delivers to ALL 16 PUs simultaneously.
//     Used for CSR-aware memory scatter. Each PU's BIU inspects the
//     256-bit payload and picks out entries addressed to it.
//
// PU ID range: [puStart, puStart + 16)

package koneko.bus

import chisel3._
import chisel3.util._

import koneko._

class MemDistributor(
  puStart: Int, // First PU ID in this cluster (1-based)
)(implicit val param: CoreParameters) extends Module {
  val in = IO(Flipped(Decoupled(new RingResp)))

  val out = IO(Vec(16, Valid(new MemResp)))

  // Single-entry buffer
  val bufDst  = Reg(UInt(16.W))
  val bufId   = Reg(UInt(16.W))
  val bufData = Reg(UInt(param.memBusWidth.W))
  val valid   = RegInit(false.B)

  // Accept when buffer is empty
  in.ready := !valid

  when(in.fire) {
    bufDst  := in.bits.dst
    bufId   := in.bits.id
    bufData := in.bits.data
    valid   := true.B
  }

  val isBroadcast = bufDst === 0xFFFF.U
  val puIdx = bufDst - puStart.U
  for (i <- 0 until 16) {
    out(i).valid    := valid && (isBroadcast || puIdx === i.U)
    out(i).bits.id  := bufId
    out(i).bits.data := bufData
  }

  // Clear buffer after one cycle of valid output
  when(valid) {
    valid := false.B
  }
}