package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class SPM extends BlackBox() {
  val io = IO(new Bundle {
    val clock = Input(Clock())

    val hartid = Input(UInt(32.W))
    val addr = Input(UInt(32.W))
    val wdata = Input(UInt(32.W))
    val we = Input(UInt(4.W))

    val data = Output(UInt(32.W))
  })
}
