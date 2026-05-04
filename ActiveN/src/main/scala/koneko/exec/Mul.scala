package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class Mul extends Module {
  val MUL_DELAY = 2

  val input = IO(Input(new Bundle {
    val rs1 = UInt(32.W)
    val rs2 = UInt(32.W)

    val rs1signed = Bool()
    val rs2signed = Bool()

    val low = Bool()
    val mid = Bool()
    val high = Bool()
  }))
  val output = IO(Output(UInt(32.W)))

  val rs1ext = (input.rs1signed && input.rs1(31)) ## input.rs1
  val rs2ext = (input.rs2signed && input.rs2(31)) ## input.rs2

  val result = (rs1ext * rs2ext)(63, 0)

  // Retimer go brrrrr
  output := RegNext(Mux1H(Seq(
    input.low -> result(31, 0),
    input.high -> result(63, 32),
    input.mid -> result(47, 16),
  )))
}