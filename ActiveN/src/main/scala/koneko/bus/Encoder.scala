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

class Encoder(implicit val param: CoreParameters) extends Module {
  // Memory-side port: connects to Crossbar downstream
  val req = IO(Flipped(Decoupled(new MemReq)))

  // Single-flit output: arbitrated externally with BIU ext.out
  val out = IO(Decoupled(new Bundle {
    val dst = UInt(16.W)
    val data = Vec(4, UInt(32.W))
    val tag = UInt(12.W)
  }))

  // --- Destination computation ---
  val memCtrlSizes = param.memCtrlSizes
  val memDstBase = 0x8001
  val periphDst = 0x8000

  val isPeripheral = !req.bits.addr(31) && req.bits.addr(30)
  val periphAddr = req.bits.addr - 0x40000000L.U

  val cumSizes = memCtrlSizes.scanLeft(BigInt(0))(_ + _)
  val numCtrls = memCtrlSizes.length

  val globalAddr = req.bits.addr - 0x80000000L.U
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

  // --- Single-flit output (combinational) ---
  out.valid := req.valid
  req.ready := out.ready

  out.bits.dst := finalDst
  out.bits.tag := Mux(req.bits.write, 0x001.U(12.W), 0x000.U(12.W))
  out.bits.data(0) := finalAddr
  out.bits.data(1) := 0.U(12.W) ## req.bits.size ## req.bits.id
  out.bits.data(2) := req.bits.wdata
  out.bits.data(3) := 0.U
}
