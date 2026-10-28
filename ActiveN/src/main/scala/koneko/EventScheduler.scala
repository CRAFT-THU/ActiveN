package koneko

import chisel3._
import chisel3.util._

/** Combinational event selection. Late thread availability selects a precomputed
  * decision; no queue, wakeup, or instruction boundary moves to another cycle.
  */
class EventScheduler(pipeCnt: Int) extends Module {
  val io = IO(new Bundle {
    val accepting = Input(UInt(pipeCnt.W))
    val enmasks = Input(Vec(16, UInt(pipeCnt.W)))
    val eligible = Input(UInt(16.W))
    val available = Input(UInt(16.W))
    val data = Input(Vec(16, Vec(4, UInt(32.W))))
    val handlers = Input(Vec(16, UInt(32.W)))
    val ready = Output(UInt(16.W))
    val handler = Output(UInt(4.W))
    val regs = Output(Vec(4, UInt(32.W)))
    val pc = Output(UInt(32.W))
    val wakeup = Output(UInt(pipeCnt.W))
  })

  class Decision extends Bundle {
    val ready = UInt(16.W)
    val handler = UInt(4.W)
    val regs = Vec(4, UInt(32.W))
    val pc = UInt(32.W)
    val wakeup = UInt(pipeCnt.W)
  }

  def decide(accepting: UInt): Decision = {
    val enabled = VecInit(io.enmasks.map(m => (m & accepting).orR)).asUInt & io.eligible
    val candidates = enabled & io.available
    val grant = VecInit((0 until 16).map(i =>
      if (i == 0) true.B else !candidates(i - 1, 0).orR)).asUInt
    // Chisel Arbiter selects input 15 when none is valid. Preserve that value,
    // including its payload, rather than introducing new invalid-cycle values.
    val winner = (grant & candidates) | Mux(candidates.orR, 0.U(16.W), (1 << 15).U(16.W))
    val d = Wire(new Decision)
    d.ready := grant & enabled
    d.handler := OHToUInt(winner)
    d.regs := Mux1H(winner, io.data)
    d.pc := Mux1H(winner, io.handlers)
    val mask = Mux1H(winner, io.enmasks) & accepting
    d.wakeup := Mux(candidates.orR, PriorityEncoderOH(mask), 0.U)
    d
  }

  // Compute a decision for every accepting-thread mask in parallel. The actual
  // accepting mask selects the result without entering the arbitration logic.
  val choices = (0 until (1 << pipeCnt)).map(m => decide(m.U(pipeCnt.W)))
  val result = Mux1H(UIntToOH(io.accepting), choices)
  io.ready := result.ready
  io.handler := result.handler
  io.regs := result.regs
  io.pc := result.pc
  io.wakeup := result.wakeup
}
