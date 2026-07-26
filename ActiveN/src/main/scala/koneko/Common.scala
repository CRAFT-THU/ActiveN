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

  def pipeValid[D <: Data](input: ValidIO[D]): ValidIO[D] = {
    val data = RegEnable(input.bits, input.valid)
    val valid = RegNext(input.valid, false.B)
    val output = Wire(Valid(data.cloneType))
    output.bits := data
    output.valid := valid
    output
  }

  def invalid[D <: Data](gen: D): ValidIO[D] = {
    val ret = Wire(Valid(gen.cloneType))
    ret.bits := DontCare
    ret.valid := false.B
    ret
  }

  def rr(input: UInt, take: Bool, name: String): UInt = {
    val width = input.getWidth
    val output = Wire(UInt(log2Ceil(width).W)).suggestName(name)

    val last = RegEnable(output, 0.U, take).suggestName(s"${name}_last")
    val filtered = Seq.tabulate(width) { i => i.U > last && input(i) }
    val doubleSel = PriorityEncoderOH(filtered ++ input.asBools)
    val sel = (0 until width).map(i => doubleSel(i) || doubleSel(i + width))
    output := OHToUInt(VecInit(sel))
    output
  }
}
