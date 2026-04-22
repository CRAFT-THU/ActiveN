package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

/**
 * Purely combinatory 32-bit AMOALU implementation
 */
class AMOALU32(implicit val param: CoreParameters) extends Module {
  val orig = IO(Input(UInt(32.W)))
  val input = IO(Input(UInt(32.W)))
  val funct5 = IO(Input(UInt(5.W)))
  val written = IO(Output(UInt(32.W)))

  val scmp = orig.asSInt > input.asSInt
  val ucmp = orig > input

  written := Mux1H(Seq(
    (funct5 === "b00001".U) -> input, // AMOSWAP
    (funct5 === "b00000".U) -> (orig + input), // AMOADD
    (funct5 === "b00100".U) -> (orig ^ input), // AMOXOR
    (funct5 === "b01100".U) -> (orig & input), // AMOAND
    (funct5 === "b01000".U) -> (orig | input), // AMOOR
    (funct5 === "b10000".U) -> Mux(scmp, input, orig), // AMOMIN
    (funct5 === "b10100".U) -> Mux(scmp, orig, input), // AMOMAX
    (funct5 === "b11000".U) -> Mux(ucmp, input, orig), // AMOMINU
    (funct5 === "b11100".U) -> Mux(ucmp, orig, input), // AMOMAXU
  ))
}