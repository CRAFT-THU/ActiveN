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

  // val cnt = RegInit(0.U(log2Up(MUL_DELAY).W))
  // val done = Wire(Bool())
  // cnt := Mux(input.valid && !done, cnt + 1.U, 0.U)
  // done := cnt === (MUL_DELAY - 1).U

  // FIXME: Pipeline
  // input.ready := RegNext(input.valid) // Count cycles

  val rs1t = input.rs1(31, 16)
  val rs1b = input.rs1(15, 0)
  val rs2t = input.rs2(31, 16)
  val rs2b = input.rs2(15, 0)

  val bb = RegNext(rs1b * rs2b)
  val tb = RegNext(rs1t * rs2b)
  val bt = RegNext(rs1b * rs2t)
  val tt = RegNext(rs1t * rs2t)

  val rs1neg = RegNext(Mux(input.rs1signed && input.rs1(31), input.rs1, 0.U))
  val rs2neg = RegNext(Mux(input.rs2signed && input.rs2(31), input.rs2, 0.U))

  val s1low = RegNext(input.low)
  val s1high = RegNext(input.high)
  val s1mid = RegNext(input.mid)

  val result = (
    (tt << 32).asUInt + (tb << 16).asUInt + (bt << 16).asUInt + bb
    - rs1neg - rs2neg
  )

  output := Mux1H(Seq(
    s1low -> result(31, 0),
    s1high -> result(63, 31),
    s1mid -> result(47, 16),
  ))
}