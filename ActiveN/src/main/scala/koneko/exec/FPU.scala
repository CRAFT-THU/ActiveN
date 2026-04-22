package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class FPU extends BlackBox() {
  val io = IO(new Bundle {
    val clock = Input(Clock())
    val reset = Input(Reset())

    val a = Input(UInt(32.W))
    val b = Input(UInt(32.W))
    val r = Output(UInt(32.W))

    val funct7 = Input(UInt(7.W))
    val funct3 = Input(UInt(3.W))
    val rs2b0 = Input(Bool())

    val valid = Input(Bool())
  })
}