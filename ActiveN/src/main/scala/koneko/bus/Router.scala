/// A single-flit NoC router with shared priority queues and
/// priority-aware arbitration per output port.
/// Uses FlitQueue for admission control (deadlock avoidance via priority
/// reservation) and FlitArb for priority-aware round-robin output selection.

package koneko.bus

import chisel3._
import chisel3.util._
import chisel3.util.experimental.decode.{TruthTable, decoder}

trait Prio extends Data {
  // The priority of this message
  // This is the hardware priority, not the protocol priority
  // Lower value = higher priority (0 is highest)
  def prio: UInt
}

trait Routable extends Data with Prio {
  // The destination of this message
  def dst: UInt

  // Whether this is a tail flit
  def isTail(isHead: Bool): Bool

  // Packet identifier, same pkt => same pkg id, but not necessarily vice versa
  // Only used for debugging and assertions
  def pktId: UInt
}

object Routable {
  /** Assert that a Decoupled[Routable] stream presents each multi-flit packet
    * on consecutive cycles with no bubbles and consistent pktId.
    * Safe to use on any port; the assertion is only active mid-packet. */
  def assertConsecutive[D <: Routable](port: DecoupledIO[D], name: String = "port"): Unit = {
    val locked  = RegInit(false.B)
    val prevId  = Reg(chiselTypeOf(port.bits.pktId))

    val isHead = !locked
    val isTail = port.bits.isTail(isHead)

    when(port.fire) {
      when(isHead && !isTail) {
        // Head of multi-flit packet: start tracking
        locked  := true.B
        prevId  := port.bits.pktId
      }.elsewhen(isTail) {
        locked := false.B
      }
    }

    // Mid-packet: must see valid every cycle (no bubble)
    when(locked) {
      assert(port.valid,
        cf"$name: bubble in multi-flit packet (expected consecutive flits)")
      assert(port.bits.pktId === prevId,
        cf"$name: pktId changed mid-packet")
    }
  }
}

case class Local(
  id: Int,
  inject: Boolean,
  eject: Boolean,
)

class Router[D <: Routable](
  data: D,
  val locals: Seq[Local],
  val numIngress: Int,
  val numEgress: Int,
  val numPrio: Int,
  val buffer: Int,
  val table: Map[Int, Int], // Routing table: dst -> egress link
) extends Module {
  private val ejectIds = locals.filter(_.eject).map(_.id).toSet
  require(!table.keys.exists(ejectIds.contains), "forwarding table must not contain local eject IDs")
  require(table.values.forall(e => e >= 0 && e < numEgress), "egress indices out of range")

  private val injectLocals = locals.zipWithIndex.filter(_._1.inject)
  private val ejectLocals = locals.zipWithIndex.filter(_._1.eject)
  private val numInputs = numIngress + injectLocals.length
  private val numOutputs = numEgress + ejectLocals.length

  // --- Routing truth tables ---
  // All IDs that appear in forwarding table or as eject-local destinations
  private val allIds = (table.keys ++ ejectLocals.map(_._1.id)).toSeq.distinct
  private val idWidth = if (allIds.isEmpty) 1 else allIds.map(k => log2Ceil(k.max(1) + 1)).max.max(1)

  // Left-padded binary string for an ID
  private def idBits(id: Int): String = {
    val s = id.toBinaryString
    "0" * (idWidth - s.length) + s
  }

  // 1-hot bit string of width w with bit j set (bit 0 = LSB = rightmost character)
  private def oneHot(w: Int, j: Int): String =
    "0" * (w - 1 - j) + "1" + "0" * j

  // Forward truth table: dst -> 1-hot egress selection (numEgress bits)
  // Table entries map to their egress port; eject-local IDs map to all-zero (not forwarded)
  private val fwdTT = if (numEgress > 0) {
    val entries = table.toSeq.map { case (dst, eIdx) =>
      BitPat("b" + idBits(dst)) -> BitPat("b" + oneHot(numEgress, eIdx))
    } ++ ejectLocals.map { case (l, _) =>
      BitPat("b" + idBits(l.id)) -> BitPat("b" + "0" * numEgress)
    }
    Some(TruthTable(entries, BitPat("b" + "0" * numEgress)))
  } else None

  // Local truth table: dst -> 1-hot eject-local selection (ejectLocals.length bits)
  // Table entries map to all-zero (forwarded); eject-local IDs map to their 1-hot position
  private val localTT = if (ejectLocals.nonEmpty) {
    val entries = table.toSeq.map { case (dst, _) =>
      BitPat("b" + idBits(dst)) -> BitPat("b" + "0" * ejectLocals.length)
    } ++ ejectLocals.zipWithIndex.map { case ((l, _), k) =>
      BitPat("b" + idBits(l.id)) -> BitPat("b" + oneHot(ejectLocals.length, k))
    }
    Some(TruthTable(entries, BitPat("b" + "0" * ejectLocals.length)))
  } else None

  // --- IO ---
  val ingress = IO(Vec(numIngress, Flipped(Decoupled(data.cloneType))))
  val egress = IO(Vec(numEgress, Decoupled(data.cloneType)))

  // Conditional inject/eject ports per local
  val injects: Seq[Option[DecoupledIO[D]]] = locals.map { l =>
    if (l.inject) Some(IO(Flipped(Decoupled(data.cloneType)))) else None
  }
  val ejects: Seq[Option[DecoupledIO[D]]] = locals.map { l =>
    if (l.eject) Some(IO(Decoupled(data.cloneType))) else None
  }

  // Ordered input ports: all ingresses, then inject-enabled locals
  private val inputPorts: Seq[DecoupledIO[D]] =
    ingress.toSeq ++ injectLocals.map { case (_, idx) => injects(idx).get }

  // Ordered output ports: all egresses, then eject-enabled locals
  private val outputPorts: Seq[DecoupledIO[D]] =
    egress.toSeq ++ ejectLocals.map { case (_, idx) => ejects(idx).get }

  // Assert consecutive-flit invariant on all ingress and inject ports
  for (i <- 0 until numIngress) {
    Routable.assertConsecutive(ingress(i), s"ingress($i)")
  }
  for ((l, idx) <- locals.zipWithIndex if l.inject) {
    Routable.assertConsecutive(injects(idx).get, s"inject($idx)")
  }

  // --- Input side: per-input-port shared FlitQueue ---
  val inputQueues: Seq[FlitQueue[D]] = Seq.tabulate(numInputs) { i =>
    val q = Module(new FlitQueue(data.cloneType, buffer, numPrio))
    q.enq <> inputPorts(i)
    q
  }

  // --- Output side: per-output-port priority-aware arbitration ---
  //
  // For each output port, we gather all input queues whose head flit targets
  // that output, then use FlitArb for priority-aware round-robin selection.

  val deqGrant = Wire(Vec(numInputs, Bool()))
  deqGrant := VecInit(Seq.fill(numInputs)(false.B))

  for (j <- 0 until numOutputs) {
    val arb = Module(new FlitArb(data.cloneType, numInputs))

    for (i <- 0 until numInputs) {
      val q = inputQueues(i)
      val dstBits = q.deq.bits.dst(idWidth - 1, 0)
      val fwd = fwdTT.map(tt => decoder(dstBits, tt)).getOrElse(0.U(0.W))
      val lcl = localTT.map(tt => decoder(dstBits, tt)).getOrElse(0.U(0.W))
      val target = Cat(lcl, fwd)

      arb.io.in(i).valid := q.deq.valid && target(j)
      arb.io.in(i).bits := q.deq.bits
    }

    outputPorts(j).valid := arb.io.out.valid
    outputPorts(j).bits := arb.io.out.bits
    arb.io.out.ready := outputPorts(j).ready

    // Grant the winning input queue
    for (i <- 0 until numInputs) {
      when(arb.io.in(i).ready && arb.io.in(i).valid) {
        deqGrant(i) := true.B
      }
    }
  }

  // Connect dequeue ready to grants
  for (i <- 0 until numInputs) {
    inputQueues(i).deq.ready := deqGrant(i)
  }
}