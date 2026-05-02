package koneko

import chisel3._
import chisel3.util._

object Common {
  implicit class DecoupledOps[D <: Data](val input: DecoupledIO[D]) {
    def map[E <: Data](f: D => E): DecoupledIO[E] = {
      val mapped = f(input.bits)
      val output = Wire(Decoupled(mapped.cloneType))
      output.bits := mapped
      output.valid := input.valid
      input.ready := output.ready

      output
    }

    def gatedBy(gate: Bool): DecoupledIO[D] = {
      val output = Wire(Decoupled(input.bits.cloneType))
      output.bits := input.bits
      output.valid := input.valid && gate
      input.ready := output.ready && gate

      output
    }

    def split(cnt: Int, steering: D => UInt): Seq[DecoupledIO[D]] = {
      val steer = steering(input.bits)
      require(steer.getWidth == cnt, cf"Steering signal width ${steer.getWidth} does not match count $cnt")
      assert(!input.valid || PopCount(steer) === 1.U)

      val outputs = for (i <- 0 until cnt) yield {
        val out = Wire(Decoupled(input.bits.cloneType))
        out.valid := input.valid && steer(i)
        out.bits := input.bits
        out
      }
      input.ready := (steer & VecInit(outputs.map(_.ready)).asUInt).orR
      outputs
    }

    def split(toRight: D => Bool): (DecoupledIO[D], DecoupledIO[D]) = {
      val steering = (d: D) => Mux(toRight(d), 2.U, 1.U)
      val Seq(left, right) = input.split(2, steering)
      (left, right)
    }
  }
}

