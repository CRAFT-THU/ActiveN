/// Encoder: converts MemReq into a single NoC flit.
///
/// Flit layout (Vec(4, UInt(32.W))):
///   data(0) = address (controller-local)
///   data(1) = 0(12) ## size(4) ## id(16)
///   data(2) = wdata (stores only, 0 for loads)
///   data(3) = reserved (0)
/// tag = 0x000 (load) or 0x001 (store)
/// Encoded requests uses highest priority (0x0??)

package koneko.bus

import chisel3._
import chisel3.util._

import koneko._

object Encoder {
  def decode(addr: UInt)(implicit param: CoreParameters): (Bool, UInt, UInt) = {
    val memCtrlSizes = param.memCtrlSizes
    val memDstBase = 0x8001
    val periphDst = 0x8000

    val isPeripheral = !addr(31) && addr(30)
    val periphAddr = addr - 0x40000000L.U

    val cumSizes = memCtrlSizes.scanLeft(BigInt(0))(_ + _)
    val numCtrls = memCtrlSizes.length

    val globalAddr = addr - 0x80000000L.U
    val ctrlDst = Wire(UInt(16.W))
    ctrlDst := (memDstBase + numCtrls - 1).U(16.W)
    for (i <- (0 until numCtrls).reverse) {
      when(globalAddr < cumSizes(i + 1).U) {
        ctrlDst := (memDstBase + i).U(16.W)
      }
    }
    val ctrlLocalAddr = Wire(UInt(32.W))
    ctrlLocalAddr := globalAddr
    for (i <- (0 until numCtrls).reverse) {
      when(globalAddr < cumSizes(i + 1).U) {
        ctrlLocalAddr := globalAddr - cumSizes(i).U
      }
    }

    val finalDst  = Mux(isPeripheral, periphDst.U(16.W), ctrlDst)
    val finalAddr = Mux(isPeripheral, periphAddr, ctrlLocalAddr)

    (isPeripheral, finalDst, finalAddr)
  }
}

class Encoder(implicit val param: CoreParameters) extends Module {
  // Memory-side port: connects to Crossbar downstream
  val req = IO(Flipped(Decoupled(new MemReq)))

  // Single-flit output: arbitrated externally with BIU ext.out
  val out = IO(Decoupled(new OutgoingFlit))

  // Address computation
  val (_, finalDst, finalAddr) = Encoder.decode(req.bits.addr)

  // --- Single-flit output (combinational) ---
  out.valid := req.valid
  req.ready := out.ready

  // FIXME: it does not seem to handle WBE?
  out.bits.dst := finalDst
  assert(!req.valid || !(req.bits.write && req.bits.bulk), "Bulk write is not supported yet")
  out.bits.tag := MuxCase(0x000.U(12.W), Seq(
    req.bits.write -> 0x001.U(12.W),
    req.bits.bulk -> 0x011.U(12.W),
  ))
  out.bits.data(0) := finalAddr
  val size = Mux(req.bits.bulk, req.bits.bulkSize, 0.U(12.W) ## req.bits.size)
  out.bits.data(1) := size ## req.bits.id
  out.bits.data(2) := req.bits.wdata
  out.bits.data(3) := 0.U
}
