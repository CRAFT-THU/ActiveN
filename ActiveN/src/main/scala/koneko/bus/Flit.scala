package koneko.bus

import chisel3._

object Flit {
  def Content = Vec(4, UInt(32.W))
}

class Flit extends Bundle with Routable {
  val src  = UInt(16.W)
  val dst  = UInt(16.W)
  val data = Flit.Content
  val tag  = UInt(16.W)

  // Single VC — XY routing on mesh is deadlock-free for single-flit packets
  def prio = 0.U
  def isTail(head: Bool) = true.B
  def pktId = this.asUInt
}