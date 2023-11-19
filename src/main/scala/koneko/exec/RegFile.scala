package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class RegFile extends Module {
  val read = IO(Vec(2, new Bundle {
    val num = Input(UInt(5.W))
    val value = Output(UInt(5.W))
  }))

  val write = IO(new Bundle {
    val en = Input(Bool())
    val num = Input(UInt(5.W))
    val value = Input(UInt(5.W))
  })

  val regs = dontTouch(Reg(Vec(32, UInt(32.W)))) // Disable DCE for RegFile

  when(write.en) {
    regs(write.num) := write.value // Actually may write regs(0), but we mask them at the read part
  }

  for(port <- read) {
    // Bypass
    port.value := MuxCase(regs(port.num), Seq(
      (port.num === 0.U) -> 0.U,
      (write.en && port.num === write.num) -> write.value
    ))
  }
}
