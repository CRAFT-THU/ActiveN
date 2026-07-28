package koneko.bus

import chisel3._
import chisel3.util._

import koneko._

// Memory interface: consumes MC eject flits from the NoC, issues memory ops,
// and routes responses back to PUs via cluster resp ports or inter-MemIf ring bus.
//
// We now use single-flit messages
//
// Ring bus: responses for PUs outside this MemIf's zone are forwarded via a
// 2-deep ring buffer. Deadlock avoidance: only push internal→ring when ring
// buffer is empty; ring input for local PUs delivered directly.

case class MemBusParameters(
  core: CoreParameters,
  extIdWidth: Int = 8,
  extAddrWidth: Int = 32,
) {
  def replicateScalar(input: UInt): UInt = {
    require(core.memBusWidth % 32 == 0, "memBusWidth must be a multiple of 32")
    Fill(core.memBusWidth / 32, input)
  }

  def lineAddrShift: Int = log2Ceil(core.memBusWidth / 8)
}

object MemOp extends ChiselEnum {
  val Load = Value
  val Store = Value
  // TODO: AMO

  def fromTag(tag: UInt): (Bool, MemOp.Type) = {
    // TODO: impl by decoder
    (
      tag(7, 0) === 0x00.U || tag(7, 0) === 0x01.U,
      Mux(tag(7, 0) === 0x00.U, MemOp.Load, MemOp.Store)
    )
  }
}

class MSHR extends Bundle {
  val ty        = MemOp()

  // Return status
  val src       = UInt(16.W) // 0 = broadcast. TODO: this is bad for constant propagation & DCE
  val id        = UInt(16.W) // Or return tag

  val address   = UInt(32.W)
  val size      = UInt(4.W)

  // TODO: move these to a separate broadcast carried buffer
  val carried = Vec(2, UInt(32.W)) // Extra data carried by the request, e.g. for broadcast

  def isBroadcast: Bool = src === 0.U
}

object MSHR {
  // Check mem.md for encoding
  def fromFlitAsScalar(flit: Flit): (Bool, MSHR, UInt) = {
    val p = Wire(new MSHR)
    val (isScalar, ty) = MemOp.fromTag(flit.tag)
    p.ty := ty
    p.src := flit.src

    // Operand 0
    p.address := flit.data(0)

    // Operand 1
    p.size := flit.data(1)(31, 16)
    p.id := flit.data(1)(15, 0)

    p.carried := DontCare

    // Operand 2
    val data = flit.data(2)

    (isScalar, p, data)
  }
}

object BulkType extends ChiselEnum {
  val Scatter = Value
  val BulkLoad = Value
  // TODO: copy write

  def fromTag(tag: UInt): (Bool, BulkType.Type) = {
    (
      tag(7, 0) === 0x10.U || tag(7, 0) === 0x11.U,
      Mux(tag(7, 0) === 0x10.U, BulkType.Scatter, BulkType.BulkLoad)
    )
  }

  def isBroadcast(ty: BulkType.Type): Bool = ty === Scatter
}

class BulkDispatcher extends Bundle {
  val ty = BulkType()
  val base = UInt(32.W) // Aligned, inclusive
  val end = UInt(32.W) // Aligned, exclusive
  val src = UInt(16.W)
  val tag = UInt(16.W)

  val carried = Vec(2, UInt(32.W))

  def isAllocated(cnt: UInt)(implicit busParams: MemBusParameters): Bool = cnt === ((end >> busParams.lineAddrShift) - (base >> busParams.lineAddrShift))
  def isAllocatedNext(cnt: UInt)(implicit busParams: MemBusParameters): Bool = (cnt + 1.U) === ((end >> busParams.lineAddrShift) - (base >> busParams.lineAddrShift))
  def issueAddr(cnt: UInt)(implicit busParams: MemBusParameters) = base + (cnt << busParams.lineAddrShift)
  def isEmpty: Bool = base === end

  def dispatch(cnt: UInt)(implicit busParams: MemBusParameters): MSHR = {
    val mshr = Wire(new MSHR)
    mshr.ty := MemOp.Load
    mshr.src := Mux(BulkType.isBroadcast(ty), 0.U, src)
    mshr.id := tag
    mshr.address := issueAddr(cnt)
    mshr.size := busParams.lineAddrShift.U
    mshr.carried := carried
    mshr
  }

  def isBroadcast: Bool = BulkType.isBroadcast(ty)
}

object BulkDispatcher {
  def fromFlit(flit: Flit)(implicit params: MemBusParameters): (Bool, BulkDispatcher) = {
    val p = Wire(new BulkDispatcher)
    p.src := flit.src

    val (isBulk, ty) = BulkType.fromTag(flit.tag)
    p.ty := ty
    p.base := flit.data(0)
    val sizeOffset = log2Ceil(params.core.memBusWidth / 8)
    p.end := flit.data(0) + ((flit.data(1)(31, 16)) << sizeOffset) // length in beats
    p.tag := flit.data(1).apply(15, 0)

    p.carried(0) := flit.data(2)
    p.carried(1) := flit.data(3)

    (isBulk, p)
  }
}

// TODO: parametric id / data width for global memory interface
class GlobalMemReq(implicit params: MemBusParameters) extends Bundle {
  val id    = UInt(params.extIdWidth.W)
  val addr  = UInt(params.extAddrWidth.W)
  val wdata = UInt(params.core.memBusWidth.W)
  val wbe   = UInt((params.core.memBusWidth / 8).W)
  val size  = UInt(3.W)
  val write = Bool()
}

class GlobalMemResp(implicit params: MemBusParameters) extends Bundle {
  val id    = UInt(params.extIdWidth.W)
  val rdata = UInt(params.core.memBusWidth.W)
}

class RingResp(implicit params: CoreParameters) extends Bundle {
  val dst  = UInt(16.W)
  val id   = UInt(16.W)
  val data = UInt(params.memBusWidth.W)
}

// Base class for DRAMIf and MMIOIf, the former implements scatter & bulk access & local response egress, the latter implements config ROM.
// inflight is the maximum number of inflight memory requests, including scalar and bulk sub-requests.
abstract class MemIf(
  mcIdx: Int,
  inflight: Int,
  numReq: Int,
  hasExtraAlloc: Boolean,
)(implicit busParams: MemBusParameters) extends Module {
  require(inflight >= 2, "inflight must be at least 2")
  assert(inflight <= (1 << busParams.extIdWidth), "inflight must be <= 2^extIdWidth")
  final implicit val coreParams: CoreParameters = busParams.core

  // Common IO
  final val nocIn = IO(Vec(numReq, Flipped(Decoupled(new Flit))))
  final val nocOut = IO(Decoupled(new Flit))

  final val ringIn  = IO(Flipped(Decoupled(new RingResp)))
  final val ringOut = IO(Decoupled(new RingResp))

  final val mem = IO(new Bundle {
    val req  = Decoupled(new GlobalMemReq)
    val resp = Flipped(Valid(new GlobalMemResp))
  })

  private val flitArb = Module(new FlitArb(nocIn(0).bits.cloneType, numReq))
  for (ci <- 0 until numReq) flitArb.io.in(ci) <> nocIn(ci)
  final protected val flit = flitArb.io.out

  // Common MSHR states
  final protected val allocated = RegInit(VecInit(Seq.fill(inflight)(false.B)))
  private val mshrs     = Reg(Vec(inflight, new MSHR))
  // Whether this request is issued
  private val issued    = Reg(Vec(inflight, Bool()))
  // Whether this request is fulfilled
  private val fulfilled = Reg(Vec(inflight, Bool()))
  // Buffers either write data, or response data, or both (for AMO)
  // We cannot reduce this to 32 bit: ICache fetches whole lines, and AMOs
  private val buffer    = Reg(Vec(inflight, UInt(coreParams.memBusWidth.W)))

  // Allocation inputs
  protected class Alloc extends Bundle {
    val mshr = new MSHR
    val data = UInt(coreParams.memBusWidth.W)
  }
  private val alloc = Wire(Decoupled(new Alloc))
  final protected val allocExtra: Option[DecoupledIO[Alloc]] =
    if (hasExtraAlloc) Some(Wire(Decoupled(new Alloc))) else None
  final protected val bcstEject = Wire(Decoupled(new BcastLine))
  // Unicast request deallocation happens on response being consumed by either ring or local resp port
  // Only used in MemIf
  private val ucstEject = Wire(Decoupled(new RingResp))

  alloc.ready := !allocated.asUInt.andR // Not full
  val allocIdx = PriorityEncoder(allocated.map(!_))
  when(alloc.fire) {
    mshrs(allocIdx) := alloc.bits.mshr
    buffer(allocIdx) := alloc.bits.data
    allocated(allocIdx) := true.B
    fulfilled(allocIdx) := false.B
    issued(allocIdx) := false.B
  }

  val ucstMap = allocated.asUInt & fulfilled.asUInt & VecInit(mshrs.map(!_.isBroadcast)).asUInt
  val ucstIdx = Common.rr(ucstMap, ucstEject.fire, "ucstIdx")
  ucstEject.bits.dst := mshrs(ucstIdx).src
  ucstEject.bits.id := mshrs(ucstIdx).id
  ucstEject.bits.data := buffer(ucstIdx)
  ucstEject.valid := ucstMap.orR
  when(ucstEject.fire) {
    allocated(ucstIdx) := false.B
  }

  val bcstMap = allocated.asUInt & fulfilled.asUInt & VecInit(mshrs.map(_.isBroadcast)).asUInt
  val bcstIdx = Common.rr(bcstMap, bcstEject.fire, "bcstIdx")
  bcstEject.bits.tag := mshrs(bcstIdx).id
  bcstEject.bits.line := buffer(bcstIdx).asTypeOf((new BcastLine).line)
  bcstEject.bits.carried := mshrs(bcstIdx).carried
  bcstEject.valid := bcstMap.orR
  when(bcstEject.fire) {
    allocated(bcstIdx) := false.B
  }

  // Scalar parsing and allocation
  private val (isScalar, parsedScalar, parsedData) = MSHR.fromFlitAsScalar(flit.bits)
  private val scalarAlloc = Wire(new Alloc)
  scalarAlloc.mshr := parsedScalar
  scalarAlloc.data := busParams.replicateScalar(parsedData)
  final protected val scalarAcceptFlit = isScalar && alloc.ready

  // Subclasses can provide bulk-to-scalar allocation through allocExtra.
  allocExtra match {
    case Some(extra) =>
      alloc.bits := Mux(isScalar && flit.valid, scalarAlloc, extra.bits)
      alloc.valid := (isScalar && flit.valid) || extra.valid
      extra.ready := alloc.ready && !(isScalar && flit.valid)
    case None =>
      alloc.bits := scalarAlloc
      alloc.valid := isScalar && flit.valid
  }

  // Outgoing request
  final protected val req = Wire(Decoupled(new GlobalMemReq))
  val issueMap = allocated.asUInt & ~issued.asUInt
  val issueIdx = Common.rr(issueMap, req.fire, "issueIdx")
  req.valid := issueMap.orR
  req.bits.id := issueIdx
  req.bits.addr := mshrs(issueIdx).address
  req.bits.wdata := buffer(issueIdx)
  req.bits.wbe := busParams.replicateScalar("b1111".U(4.W))
  req.bits.size := mshrs(issueIdx).size
  assert(!req.valid || req.bits.size <= busParams.lineAddrShift.U, "Unsupported scalar access size")
  req.bits.write := mshrs(issueIdx).ty === MemOp.Store // TODO: handle RMW for subline writes, or let's have a LLDC
  when(req.fire) {
    issued(issueIdx) := true.B
  }

  // Incoming response
  final protected val resp = Wire(Valid(new GlobalMemResp))
  val respInRange = resp.bits.id < inflight.U
  val respIdx = resp.bits.id(log2Ceil(inflight) - 1, 0)
  when(resp.valid) {
    assert(respInRange, "Received response ID outside the MSHR range")
    when(respInRange) {
      val issuedNow = req.fire && req.bits.id === resp.bits.id
      assert(allocated(respIdx), "Received response for an unallocated MSHR")
      assert(issued(respIdx) || issuedNow, "Received response for an unissued MSHR")
      assert(!fulfilled(respIdx), "Received duplicate response for an MSHR")
      fulfilled(respIdx) := true.B
      // May overwrite wdata, but stores do not consume it after completion.
      buffer(respIdx) := resp.bits.rdata
    }
  }

  // Ring implementation
  def isLocal(puId: UInt): Bool
  def splitLocal(in: DecoupledIO[RingResp]) : (DecoupledIO[RingResp], DecoupledIO[RingResp]) = {
    val local = Wire(Decoupled(new RingResp))
    val remote = Wire(Decoupled(new RingResp))
    local.bits := in.bits
    remote.bits := in.bits
    local.valid := in.valid && isLocal(in.bits.dst)
    remote.valid := in.valid && !isLocal(in.bits.dst)
    in.ready := Mux(isLocal(in.bits.dst), local.ready, remote.ready)
    (local, remote)
  }

  final protected val (ringRecv, ringFwd) = splitLocal(ringIn)
  final protected val (ucstLocal, ringSend) = splitLocal(ucstEject)

  private val ringFwdQueue = Module(new Queue(new RingResp, 2))
  private val ringSendGated = Wire(Decoupled(new RingResp))
  ringSendGated.bits := ringSend.bits
  ringSendGated.valid := ringSend.valid && ringFwdQueue.io.count === 0.U
  ringSend.ready := ringSendGated.ready && ringFwdQueue.io.count === 0.U

  private val ringPushArb = Module(new Arbiter(new RingResp, 2))
  ringPushArb.io.in(0) <> ringFwd
  ringPushArb.io.in(1) <> ringSendGated
  ringFwdQueue.io.enq <> ringPushArb.io.out
  ringFwdQueue.io.deq <> ringOut

  // Idle notifier
  // TODO: rejection & unregister buffer
  class IdleNotifier extends Bundle {
    val tgt = UInt(16.W)
    val tag = UInt(16.W)
    val cycles = UInt(16.W)
  }
  private val idleNotifier = RegInit(Common.invalid(new IdleNotifier))
  private val idleCycles = RegInit(0.U(16.W))
  final protected val idleCurrently = Wire(Bool())
  idleCycles := MuxCase(idleCycles + 1.U, Seq(
    !idleCurrently -> 0.U,
    idleCycles.andR -> idleCycles
  ))
  class IdleReturn extends Bundle {
    val tgt = UInt(16.W)
    val tag = UInt(16.W)
    val data = UInt(32.W)
  }
  private val idleReturn = RegInit(Common.invalid(new IdleReturn))
  private val isIdle = Seq(0xF0, 0xF1).map(_.U === flit.bits.tag(7, 0)).reduce(_ || _)
  // Idle return buffer have to be empty: all requests can generate it
  final protected val idleAcceptFlit = isIdle && !idleReturn.valid

  // Fire idle notifier into idle return buffer
  when (idleNotifier.valid && !idleReturn.valid && idleCycles > idleNotifier.bits.cycles && !(flit.valid && isIdle)) {
    assert(!idleReturn.valid)
    idleReturn.valid := true.B
    idleReturn.bits.tgt := idleNotifier.bits.tgt
    idleReturn.bits.tag := idleNotifier.bits.tag
    idleReturn.bits.data := 0.U
    idleNotifier.valid := false.B
  }
  // Request takes precedence over idle notifier firing to occupy the idle return buffer
  when(flit.fire && flit.bits.tag(7, 0) === 0xF0.U) { // Allocation
    idleNotifier.valid := true.B
    when(!idleNotifier.valid) {
      idleNotifier.bits.tgt := flit.bits.src
      idleNotifier.bits.tag := flit.bits.data(0)(15, 0)
      idleNotifier.bits.cycles := flit.bits.data(0)(31, 16)
    }.otherwise {
      assert(!idleReturn.valid)
      idleReturn.valid := true.B
      idleReturn.bits.tgt := flit.bits.src
      idleReturn.bits.tag := flit.bits.data(0)(15, 0)
      idleReturn.bits.data := "hFFFFFFFF".U // -1
    }
  }
  when(flit.fire && flit.bits.tag(7, 0) === 0xF1.U) { // Deallocation
    idleNotifier.valid := false.B
    assert(!idleReturn.valid)
    idleReturn.valid := true.B
    idleReturn.bits.tgt := flit.bits.src
    idleReturn.bits.tag := flit.bits.data(0)(15, 0)
    idleReturn.bits.data := Mux(idleNotifier.valid, 0.U, "hFFFFFFFF".U)
  }

  private val idleFlit = Wire(Decoupled(new Flit))
  idleFlit.bits.src := (0x8000 + mcIdx).U
  idleFlit.bits.dst := idleReturn.bits.tgt
  idleFlit.bits.tag := idleReturn.bits.tag
  idleFlit.bits.data(0) := idleReturn.bits.data
  idleFlit.bits.data(1) := 0.U
  idleFlit.bits.data(2) := 0.U
  idleFlit.bits.data(3) := 0.U
  idleFlit.valid := idleReturn.valid
  when(idleFlit.fire) {
    idleReturn.valid := false.B
  }

  nocOut <> idleFlit
}

class DRAMIf(
  mcIdx: Int,
  puStart: Int,     // First PU ID in zone (1-based, inclusive)
  puEnd: Int,       // Last PU ID in zone (1-based, inclusive)
  numClusters: Int, // Clusters in this zone
  inflight: Int,
)(implicit busParams: MemBusParameters) extends MemIf(mcIdx, inflight, numClusters, hasExtraAlloc = true) {
  require(mcIdx >= 1, "mcIdx must be >= 1 for DRAMIf")
  require(numClusters >= 1)
  require(puEnd >= puStart)
  require(puEnd - puStart + 1 == numClusters * 16, "PU IDs must map cleanly to clusters")
  require(puStart == (mcIdx - 1) * numClusters * 16 + 1,
    s"mcIdx ($mcIdx) must align with puStart ($puStart), expected ${(mcIdx - 1) * numClusters * 16 + 1}")

  // DRAMIf has response egress ports into local clusters
  val unicast = IO(Vec(numClusters, Valid(new RingResp)))
  val broadcast = IO(Vec(numClusters, Decoupled(new BcastLine)))

  mem.req.valid := req.valid
  mem.req.bits := req.bits
  req.ready := mem.req.ready
  resp.valid := mem.resp.valid
  resp.bits := mem.resp.bits

  // DRAMIf-specific state for scatter/broadcast
  // Scatter: multi-read command (tag=0xFF02) followed by multiple beats of response data,
  // broadcast to all clusters one beat at a time. During broadcast, normal local responses are paused.

  def isLocal(puId: UInt): Bool = puId >= puStart.U && puId <= puEnd.U
  def clusterOf(puId: UInt): UInt = (puId - puStart.U) >> 4

  // TODO: make this queue length configurable
  val bulkQueue = Module(new Queue(new BulkDispatcher, 32))

  // Alloc
  val (isBulk, parsedBulk) = BulkDispatcher.fromFlit(flit.bits)
  bulkQueue.io.enq.valid := flit.valid && isBulk && !parsedBulk.isEmpty
  bulkQueue.io.enq.bits := parsedBulk
  val bulkAcceptFlit = isBulk && (bulkQueue.io.enq.ready || parsedBulk.isEmpty)

  flit.ready := scalarAcceptFlit || bulkAcceptFlit || idleAcceptFlit

  val bulkAlloc = allocExtra.get
  val bulkCnt = RegInit(0.U((32 - busParams.lineAddrShift).W))

  // Guarantee that there is space for unicast requests, so we don't deadlock the system
  val bcstInflight = RegInit(0.U(log2Ceil(inflight).W))
  require(inflight >= 64, "inflight must be at least 64 for DRAMIf to avoid deadlock")
  val BCST_BOUND = inflight - 32
  val bcstBlocked = bcstInflight === BCST_BOUND.U && bulkQueue.io.deq.bits.isBroadcast
  assert(bcstInflight <= BCST_BOUND.U, "bcstInflight exceeded BCST_BOUND")
  bulkAlloc.valid := bulkQueue.io.deq.valid && !bcstBlocked
  bulkAlloc.bits.mshr := bulkQueue.io.deq.bits.dispatch(bulkCnt)
  bulkAlloc.bits.data := DontCare
  val bulkCurDone = bulkQueue.io.deq.bits.isAllocatedNext(bulkCnt)
  bulkQueue.io.deq.ready := bulkAlloc.ready && !bcstBlocked && bulkCurDone
  bulkCnt := MuxCase(bulkCnt, Seq(
    (bulkAlloc.fire && bulkCurDone) -> 0.U,
    bulkAlloc.fire -> (bulkCnt + 1.U),
  ))

  // Bulk Broadcast
  val bcstQueue = Module(new Queue(new BcastLine, 2))
  // The queue holds a broadcast response stable while clusters accept it independently.
  bcstQueue.io.enq <> bcstEject
  val bcstAccepted = RegInit(0.U(numClusters.W))
  val bcstAccept = Wire(Vec(numClusters, Bool()))
  val bcstAcceptedUpdated = bcstAccepted | bcstAccept.asUInt
  val bcstStep = bcstAcceptedUpdated.andR
  bcstQueue.io.deq.ready := bcstStep
  bcstAccepted := Mux(bcstStep, 0.U(numClusters.W), bcstAcceptedUpdated)
  val bcstDists = for (ci <- 0 until numClusters) yield {
    val bcstDist = Wire(Decoupled(new BcastLine))
    bcstDist.suggestName(s"bcstDist_$ci")
    // TODO: gate by if any data lands in that distributor
    bcstDist.valid := bcstQueue.io.deq.valid && !bcstAccepted(ci)
    bcstDist.bits := bcstQueue.io.deq.bits
    bcstAccept(ci) := bcstDist.fire
    bcstDist
  }
  val bcstAlloc = bulkAlloc.fire && bulkQueue.io.deq.bits.isBroadcast
  val bcstFree = bcstEject.fire
  when(bcstAlloc && !bcstFree) {
    assert(bcstInflight < BCST_BOUND.U, "Broadcast MSHR counter overflow")
    bcstInflight := bcstInflight + 1.U
  }.elsewhen(!bcstAlloc && bcstFree) {
    assert(bcstInflight =/= 0.U, "Broadcast MSHR counter underflow")
    bcstInflight := bcstInflight - 1.U
  }

  // Arbitration for each resp port & ring egress:
  // 1. ringRecv (only for local resp)
  // 2. scalarEject
  // Broadcast now has a dedicated port
  def localDist(in: DecoupledIO[RingResp], name: String): Seq[DecoupledIO[RingResp]] = {
    val clusterMask = for (ci <- 0 until numClusters) yield {
      clusterOf(in.bits.dst) === ci.U
    }

    when(in.valid) {
      assert(isLocal(in.bits.dst), cf"$name: Received non-local response with dst ${in.bits.dst}")
    }

    val outs = for (ci <- 0 until numClusters) yield {
      val out = Wire(Decoupled(new RingResp)).suggestName(s"${name}_$ci")
      out.bits := in.bits
      out.valid := in.valid && clusterMask(ci)
      out
    }

    val readies = outs.zip(clusterMask).map({ case (out, cm) => out.ready && cm })
    in.ready := VecInit(readies).asUInt.orR
    outs
  }

  val ringDist = localDist(ringRecv, "ringDist")
  val scalarDist = localDist(ucstLocal, "ucstLocal")

  for (ci <- 0 until numClusters) {
    val arb = Module(new Arbiter(new RingResp, 2)).suggestName(s"distArb_$ci")
    arb.io.in(0) <> ringDist(ci)
    arb.io.in(1) <> scalarDist(ci)

    unicast(ci) := arb.io.out
    arb.io.out.ready := true.B

    broadcast(ci) <> bcstDists(ci)
  }

  idleCurrently := !bulkQueue.io.deq.valid && !bcstQueue.io.deq.valid && !allocated.asUInt.orR
}

// No distributor, no scatter / bulk, can have configROM
class PeripheralIf(
  externalInflight: Int,
  configROM: Map[BigInt, BigInt] = Map.empty,
)(implicit busParams: MemBusParameters) extends MemIf(0, externalInflight, 1, hasExtraAlloc = false) {

  val addrMask = ~(busParams.core.memBusWidth / 8 - 1).U(32.W)
  val configHits = configROM.map({ case (addr, data) => ((req.bits.addr & addrMask) === addr.U, data.U(busParams.core.memBusWidth.W)) }).toSeq
  val configHit = VecInit(configHits.map(_._1)).asUInt.orR
  val configReadout = Mux1H(configHits)
  val configGrant = !mem.resp.valid
  val configResp = Wire(Valid(new GlobalMemResp))
  configResp.valid := req.valid && configHit && configGrant
  configResp.bits.id := req.bits.id
  configResp.bits.rdata := configReadout

  mem.req.valid := req.valid && !configHit
  mem.req.bits := req.bits
  req.ready := Mux(configHit, configGrant, mem.req.ready)

  // Response arbitration
  resp.bits := Mux(mem.resp.valid, mem.resp.bits, configResp.bits)
  resp.valid := mem.resp.valid || configResp.valid

  // We don't have any local distributor
  def isLocal(puId: UInt): Bool = false.B
  ringRecv.ready := DontCare
  assert(!ringRecv.valid)
  ucstLocal.ready := DontCare
  assert(!ucstLocal.valid)
  bcstEject.ready := DontCare
  assert(!bcstEject.valid)

  flit.ready := scalarAcceptFlit || idleAcceptFlit

  idleCurrently := !allocated.asUInt.orR
}
