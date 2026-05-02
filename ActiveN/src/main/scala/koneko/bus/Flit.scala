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

// Big endian
class BcastBeat extends Bundle {
  val pu = UInt(16.W)
  val idx = UInt(16.W)
  val data = UInt(32.W)
}

// Bcast ABI:
// Reinterpret each 64bits chunk of bcast line
// Into (16bit of PU index, 16bit of subindex, 32bit of data).
// a0: subindex (truncated)
// a1: data
// a2~a3: carried data
class BcastLine extends Bundle {
  val line = Vec(4, new BcastBeat)
  val tag = UInt(12.W)
  val carried = Vec(2, UInt(32.W))
}