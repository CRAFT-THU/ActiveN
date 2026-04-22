package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class SPM(implicit val param: CoreParameters) extends Module {
  val io = IO(new Bundle {
    val addr = Input(UInt(32.W))
    val wdata = Input(UInt(32.W))
    val we = Input(UInt(4.W))

    val data = Output(UInt(32.W))
  })

  val scratchpad = SyncReadMem(param.scratchpadSize / 4, Vec(4, UInt(8.W)))

  val wordAddr = io.addr(log2Ceil(param.scratchpadSize) - 1, 2)

  // Synchronous read (1-cycle latency)
  io.data := scratchpad.read(wordAddr).asUInt

  // Write with byte enables
  when(io.we.orR) {
    scratchpad.write(wordAddr, io.wdata.asTypeOf(Vec(4, UInt(8.W))), io.we.asBools)
  }
}
