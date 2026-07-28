package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class SPM(implicit val param: CoreParameters) extends Module {
  val io = IO(new Bundle {
    val addr = Input(UInt(32.W))
    val wdata = Input(UInt(param.memBusWidth.W))
    val we = Input(UInt((param.memBusWidth / 8).W))

    val data = Output(UInt(param.memBusWidth.W))
  })

  val LineType = Vec(param.memBusWidth / 8, UInt(8.W))
  val scratchpad = SyncReadMem(param.scratchpadSize / (param.memBusWidth / 8), LineType)
  val wordAddr = io.addr(log2Ceil(param.scratchpadSize) - 1, log2Ceil(param.memBusWidth / 8))

  // Synchronous read (1-cycle latency)
  io.data := scratchpad.read(wordAddr).asUInt

  // Write with byte enables
  when(io.we.orR) {
    scratchpad.write(wordAddr, io.wdata.asTypeOf(LineType), io.we.asBools)
  }
}
