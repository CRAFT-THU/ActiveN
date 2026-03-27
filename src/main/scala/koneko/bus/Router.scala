/// A NoC router, supporting single-flit messages

package koneko.bus

import chisel3._
import chisel3.util._
import chisel3.util.experimental.decode.{TruthTable, decoder}

trait Routable extends Data {
  // The destination of this message
  def dst: UInt

  // The priority of this message
  // This is the hardware priority, not the protocol priority
  // Will be mapped to VCs
  def prio: UInt
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
  val numVc: Int,
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

  // --- Input side: per-input-port VC buffers ---
  val vcBufs: Seq[Seq[Queue[D]]] = Seq.tabulate(numInputs) { i =>
    val bufs = Seq.tabulate(numVc) { v =>
      val q = Module(new Queue(data.cloneType, buffer))
      q.io.enq.bits := inputPorts(i).bits
      q.io.enq.valid := inputPorts(i).valid && inputPorts(i).bits.prio === v.U
      q
    }
    // Ready when the VC queue targeted by prio can accept
    inputPorts(i).ready := MuxLookup(inputPorts(i).bits.prio, false.B)(
      bufs.zipWithIndex.map { case (q, v) => v.U -> q.io.enq.ready }
    )
    bufs
  }

  private val allBufs: Seq[Queue[D]] = vcBufs.flatten

  // Decode each buffer head's target output port via truth tables
  // Result: combined 1-hot UInt over numOutputs (bits 0..numEgress-1 = egress, rest = eject locals)
  private val bufTargets: Seq[UInt] = allBufs.map { buf =>
    val dstBits = buf.io.deq.bits.dst(idWidth - 1, 0)
    val fwd = fwdTT.map(tt => decoder(dstBits, tt)).getOrElse(0.U(0.W))
    val lcl = localTT.map(tt => decoder(dstBits, tt)).getOrElse(0.U(0.W))
    Cat(lcl, fwd)
  }

  // --- Output side: per-output-port two-level arbitration ---
  val deqGrant = Wire(Vec(allBufs.length, Bool()))
  deqGrant := VecInit(Seq.fill(allBufs.length)(false.B))

  for (j <- 0 until numOutputs) {
    // Per-VC round-robin arbiter across input ports
    val vcArbs = Seq.tabulate(numVc) { v =>
      val arb = Module(new RRArbiter(data.cloneType, numInputs))
      for (i <- 0 until numInputs) {
        val flat = i * numVc + v
        arb.io.in(i).valid := allBufs(flat).io.deq.valid && bufTargets(flat)(j)
        arb.io.in(i).bits := allBufs(flat).io.deq.bits
      }
      arb
    }

    // Strict VC priority: highest VC number wins
    val vcValid = VecInit(vcArbs.map(_.io.out.valid))
    val hasReq = vcValid.asUInt.orR
    val winVc = Wire(UInt(log2Ceil(numVc max 2).W))
    winVc := 0.U
    for (v <- 0 until numVc) {
      when(vcValid(v)) { winVc := v.U }
    }

    // Drive output port
    outputPorts(j).valid := hasReq
    outputPorts(j).bits := MuxLookup(winVc, vcArbs(0).io.out.bits)(
      vcArbs.zipWithIndex.map { case (arb, v) => v.U -> arb.io.out.bits }
    )

    // Back-propagate ready only to the winning VC arbiter
    for (v <- 0 until numVc) {
      vcArbs(v).io.out.ready := outputPorts(j).ready && winVc === v.U && hasReq
    }

    // Record grants so we can dequeue the correct buffer
    for (v <- 0 until numVc) {
      when(vcArbs(v).io.out.fire) {
        for (i <- 0 until numInputs) {
          when(vcArbs(v).io.chosen === i.U) {
            deqGrant(i * numVc + v) := true.B
          }
        }
      }
    }
  }

  // Connect dequeue ready to grants
  for (k <- allBufs.indices) {
    allBufs(k).io.deq.ready := deqGrant(k)
  }
}