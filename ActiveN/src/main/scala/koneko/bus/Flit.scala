package koneko.bus

import chisel3._

object Flit {
  def Content = Vec(4, UInt(32.W))
}

class Flit extends Bundle with Routable {
  val src  = UInt(16.W)
  val dst  = UInt(16.W)
  val data = Flit.Content
  val tag  = UInt(12.W)

  // Single VC — XY routing on mesh is deadlock-free for single-flit packets
  def prio = tag(11, 10)
  def isTail(head: Bool) = true.B
  def pktId = this.asUInt
}

class OutgoingFlit extends Bundle with Prio {
  val dst = UInt(16.W)
  val data = Vec(4, UInt(32.W))
  val tag = UInt(12.W)

  def prio = tag(11, 10)
}

class IncomingFlit extends Bundle with Prio {
  val src = UInt(16.W)
  val data = Vec(4, UInt(32.W))
  val tag = UInt(12.W)

  def prio = tag(11, 10)
}