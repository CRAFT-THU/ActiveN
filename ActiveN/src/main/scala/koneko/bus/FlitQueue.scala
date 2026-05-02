package koneko.bus

import chisel3._
import chisel3.util._

class FlitQueue[F <: Prio](
  flit: F,
  depth: Int,
  numPrio: Int,
  preserveOrder: Boolean = false,
) extends Module {
  require(isPow2(depth), "Depth must be a power of 2 for easy full/empty checks")
  require(depth >= numPrio, "Depth must be at least numPrio for deadlock avoidance algorithm")
  val buffer = Reg(Vec(depth, flit.cloneType))
  val valid = RegInit(VecInit.fill(depth)(false.B))
  val prio = RegInit(VecInit.fill(depth)(0.U(numPrio.W))) // Stored as bitmask
  val cnt = RegInit(0.U((log2Ceil(depth) + 1).W))

  for (i <- 0 until depth) {
    assert(!(valid(i) && prio(i) === 0.U), s"Valid flit in slot $i must have nonzero prio")
    assert(!(!valid(i) && prio(i) =/= 0.U), s"Invalid flit in slot $i must have zero prio")
    assert(PopCount(prio(i)) <= 1.U, cf"Flit in slot $i has prio ${prio(i)}, which is not a one-hot bitmask")
  }
  assert(PopCount(valid) === cnt)
  if (preserveOrder) {
    assert(valid.asUInt === ((1.U << cnt) - 1.U), cf"Valid bits ${valid.asUInt} do not form a contiguous sequence for preserveOrder")
  }

  val enq = IO(Flipped(Decoupled(flit.cloneType)))
  val deq = IO(Decoupled(flit.cloneType))
  val count = IO(Output(UInt((log2Ceil(depth) + 1).W)))
  count := cnt

  // Dequeue
  val prioDeqs = (0 until numPrio) map { p => VecInit(prio.map(_(p))) }
  val prioValids = prioDeqs.map(_.asUInt.orR)
  val prioSel = PriorityEncoderOH(prioValids) // 0 is highest prio
  val deqCandidates = Mux1H(prioSel, prioDeqs)
  val deqMask = PriorityEncoderOH(deqCandidates)
  val deqData = Mux1H(deqMask, buffer)
  deq.bits := deqData
  deq.valid := VecInit(prioValids).asUInt.orR

  if (preserveOrder) {
    // TODO: impl
    // For dequeue, collapse queue, prefix-or on deqMask (= prefix-or on deqCandidates), shift left one slot
    // For alloc, should just alloc to cnt - deq.valid
    throw new NotImplementedError("preserveOrder=true is not implemented yet")
  }

  // Enqueue
  val allocCandidates = valid.map(!_)
  val enqPrio = enq.bits.prio
  assert(!enq.valid || enqPrio < numPrio.U, cf"Enqueued flit has prio ${enqPrio}, which exceeds numPrio $numPrio")
  // enqPrio + 1 is the required space, so we just add it onto cnt and see if the high bit is set
  val canAlloc = !((cnt + enqPrio)(log2Ceil(depth)))
  val allocMask = PriorityEncoderOH(allocCandidates)
  enq.ready := VecInit(allocCandidates).asUInt.orR && canAlloc

  // State transition
  cnt := cnt + enq.fire - deq.fire
  for (i <- 0 until depth) {
    val enqHere = enq.fire && allocMask(i)
    val deqHere = deq.fire && deqMask(i)
    valid(i) := valid(i) && !deqHere || enqHere
    when (enqHere) {
      prio(i) := UIntToOH(enqPrio, numPrio)
      buffer(i) := enq.bits
    }.elsewhen(deqHere) {
      prio(i) := 0.U
    }
  }
}