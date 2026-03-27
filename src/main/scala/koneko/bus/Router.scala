/// A NoC router, supporting single-flit messages

package koneko.bus

import chisel3._
import chisel3.util._

trait Routable extends Data {
  // The destination of this message
  // 
  def dst: UInt

  // The priority of this message
  // This is the hardware priority, not the protocol priority
  // Will be mapped to VCs
  def prio: UInt
}

class Link[D <: Routable](
  proto: D,
) extends Bundle {
  val egress = Decoupled(proto.cloneType)
  val ingress = Flipped(Decoupled(proto.cloneType))
}

class Router[D <: Routable](
  data: D,
  val local: Int, // Local ID, used to determine if a message is destined for this router
  val numLinks: Int,
  val numVc: Int,
  val buffer: Int, // Buffer size per VC
  val table: Map[Int, Int], // Routing table: dst -> egress link
) extends Module {
  val links = IO(Vec(numLinks, new Link(data)))
  val eject = IO(Decoupled(data.cloneType))
  val inject = IO(Flipped(Decoupled(data.cloneType)))

  private val numInputs = numLinks + 1   // link ingresses + local inject
  private val numOutputs = numLinks + 1  // link egresses + local eject
  private val ejectPort = numLinks       // output port index for eject

  // Route lookup: destination ID -> 1-hot output port selection (combinational)
  // Groups destinations by output port for a compact OR-based decoder.
  private def routeTo(dst: UInt): Vec[Bool] = {
    val oh = Wire(Vec(numOutputs, Bool()))
    // Group table entries by output port: port -> Set[dst]
    val portToDsts = table.groupMap(_._2)(_._1)
    for (j <- 0 until numOutputs) {
      if (j == ejectPort) {
        // Eject port: local destination + any dst not in routing table
        val inTable = table.keys.map(d => dst === d.U).foldLeft(false.B)(_ || _)
        oh(j) := dst === local.U || (!inTable && dst =/= local.U).suggestName(s"eject_fallback")
      } else {
        val dsts = portToDsts.getOrElse(j, Set.empty)
        if (dsts.isEmpty) {
          oh(j) := false.B
        } else {
          oh(j) := dsts.map(d => dst === d.U).foldLeft(false.B)(_ || _)
        }
      }
    }
    oh
  }

  // --- Input side: per-input-port VC buffers ---
  val inputPorts: Seq[DecoupledIO[D]] = links.map(_.ingress).toSeq :+ inject

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

  // Flatten: allBufs(i * numVc + v) = vcBufs(i)(v)
  val allBufs: Seq[Queue[D]] = vcBufs.flatten

  // Pre-compute each buffer head's target output port (1-hot)
  val bufOutPort: Seq[Vec[Bool]] = allBufs.map(buf => routeTo(buf.io.deq.bits.dst))

  // --- Output side: per-output-port arbitration ---
  val outputPorts: Seq[DecoupledIO[D]] = links.map(_.egress).toSeq :+ eject

  // Dequeue grant signals (one per buffer, at most one true per cycle per buffer)
  val deqGrant = Wire(Vec(allBufs.length, Bool()))
  deqGrant := VecInit(Seq.fill(allBufs.length)(false.B))

  for (j <- 0 until numOutputs) {
    // Per-VC round-robin arbiter across input ports
    val vcArbs = Seq.tabulate(numVc) { v =>
      val arb = Module(new RRArbiter(data.cloneType, numInputs))
      for (i <- 0 until numInputs) {
        val flatIdx = i * numVc + v
        arb.io.in(i).valid := allBufs(flatIdx).io.deq.valid && bufOutPort(flatIdx)(j)
        arb.io.in(i).bits := allBufs(flatIdx).io.deq.bits
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