package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class RegFile(val smtid: Int) extends Module {
  val read = IO(Vec(2, new Bundle {
    val num = Input(UInt(5.W))
    val value = Output(UInt(32.W))
  }))

  val write = IO(new Bundle {
    val en = Input(Bool())
    val num = Input(UInt(5.W))
    val value = Input(UInt(32.W))
  })

  val bankedWrite = IO(Input(new Bundle {
    val en = Bool()
    val regs = Vec(4, UInt(32.W))
    val offset = UInt(2.W)
    val cnt = UInt(2.W) // 0, 1, 2, 3
  }))

  // Disable DCE for RegFile
  val regs = dontTouch(RegInit({
    val init = Wire(Vec(32, UInt(32.W)))
    init := DontCare
    init(10) := smtid.U
    init
  }))

  val bankedWriteResult = Wire(regs.cloneType)
  for(i <- 0 until 32) {
    if(i < 10) {
      bankedWriteResult(i) := regs(i)
    } else {
      val diff = i - 10
      val doffset = diff >> 2
      val didx = diff % 4
      bankedWriteResult(i) := Mux(doffset.U === bankedWrite.offset && didx.U <= bankedWrite.cnt, bankedWrite.regs(didx), regs(i))
    }
  }

  when(bankedWrite.en) {
    regs := bankedWriteResult
  }

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
