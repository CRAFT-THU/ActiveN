package koneko.bus

// Priority-aware arbiter. Otherwise work in a round-robin fashion

import chisel3._
import chisel3.util._

class FlitArb[F <: Routable](flit: F, n: Int) extends Module {
  val io = IO(new ArbiterIO[F](flit.cloneType, n))

  // Priority comparison matrix:
  // prioMatrix(i)(j) = true if i has higher prio than j (i.prio <= j.prio)
  val prioMatrix: Seq[Seq[Bool]] = Seq.tabulate(n, n) { (i, j) =>
    io.in(i).bits.prio <= io.in(j).bits.prio
  }
  // Whether this entry might be a valid candidate for arbitration
  val candidates: Seq[Bool] = Seq.tabulate(n) { i => {
    // A candidate must be valid and have highest priority among all *VALID* enq
    (0 until n).map(j => {
      if (i == j) true.B
      else !io.in(j).valid || prioMatrix(i)(j)
    }).reduce(_ && _) && io.in(i).valid
  }}
  val hasCandidate = candidates.reduce(_ || _)
  assert(io.in.map(_.valid).reduce(_ || _) === hasCandidate, "valid ingress but no candidates")
  val lastGrant = RegEnable(io.chosen, 0.U, io.out.fire)
  val filteredCandidates = Seq.tabulate(n) { i => i.U > lastGrant && candidates(i) }
  val doubleSel = PriorityEncoderOH(filteredCandidates ++ candidates)
  val sel = (0 until n).map(i => doubleSel(i) || doubleSel(i + n))

  io.chosen := OHToUInt(sel)
  io.out.valid := hasCandidate
  io.out.bits := Mux1H(sel, io.in.map(_.bits))
  for (i <- 0 until n) {
    io.in(i).ready := sel(i) && io.out.ready
  }
}