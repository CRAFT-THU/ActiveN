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

object ScalarType extends ChiselEnum {
  val Load = Value
  val Store = Value
  // TODO: AMO

  def fromTag(tag: UInt): (Bool, ScalarType.Type) = {
    // TODO: impl by decoder
    (
      tag === 0xFF00.U || tag === 0xFF01.U,
      Mux(tag === 0xFF00.U, ScalarType.Load, ScalarType.Store)
    )
  }
}

class ScalarPending extends Bundle {
  val ty        = ScalarType()
  val src       = UInt(16.W)
  val id        = UInt(16.W)
  val address   = UInt(32.W)
  val size      = UInt(4.W) // FIXME: ICache request size!
}
object ScalarPending {
  // Check mem.md for encoding
  def fromFlit(flit: Flit): (Bool, ScalarPending, UInt) = {
    val p = Wire(new ScalarPending)
    val (isScalar, ty) = ScalarType.fromTag(flit.tag)
    p.ty := ty
    p.src := flit.src

    // Operand 0
    p.address := flit.data(0)

    // Operand 1
    p.size := flit.data(1)(31, 16)
    p.id := flit.data(1)(15, 0)

    // Operand 2
    val data = Fill(8, flit.data(2))

    (isScalar, p, data)
  }
}

object BulkType extends ChiselEnum {
  val Scatter = Value
  val BulkLoad = Value
  // TODO: copy write

  def fromTag(tag: UInt): (Bool, BulkType.Type) = {
    (
      tag === 0xFF10.U || tag === 0xFF11.U,
      Mux(tag === 0xFF10.U, BulkType.Scatter, BulkType.BulkLoad)
    )
  }

  def isBroadcast(ty: BulkType.Type): Bool = ty === Scatter
}

class BulkPending extends Bundle {
  val ty = BulkType()
  val base = UInt(32.W) // Aligned, inclusive
  val end = UInt(32.W) // Aligned, exclusive
  val src = UInt(16.W)
  val tag = UInt(16.W)

  val issueCnt = UInt(16.W) // Counting beats
  val completedCnt = UInt(16.W)

  // FIXME: parametric beat size
  def isFullyIssued: Bool = issueCnt === ((end >> 5) - (base >> 5))
  def isCompletedNext: Bool = (completedCnt + 1.U) === ((end >> 5) - (base >> 5))
  def canIssue(maxInflight: Int): Bool = completedCnt - issueCnt < maxInflight.U
  def issueAddr = base + (issueCnt << 5)
}

object BulkPending {
  def initFromFlit(flit: Flit): (Bool, BulkPending) = {
    val p = Wire(new BulkPending)
    p.src := flit.src

    val (isBulk, ty) = BulkType.fromTag(flit.tag)
    p.ty := ty
    p.base := flit.data(0)
    p.end := flit.data(0) + ((flit.data(1)(31, 16)) << 5) // length in beats
    p.tag := flit.data(1).apply(15, 0)

    p.issueCnt := 0.U
    p.completedCnt := 0.U

    (isBulk, p)
  }
}

// TODO: parametric id / data width for global memory interface
class GlobalMemReq extends Bundle {
  val id    = UInt(8.W)
  val addr  = UInt(32.W)
  val wdata = UInt(256.W)
  val wbe   = UInt(32.W)
  val size  = UInt(3.W)
  val write = Bool()
}

class GlobalMemResp extends Bundle {
  val id    = UInt(8.W)
  val rdata = UInt(256.W)
}

class RingResp extends Bundle {
  val dst  = UInt(16.W)
  val id   = UInt(16.W)
  val data = UInt(256.W)
}

// Base class for DRAMIf and MMIOIf, the former implements scatter & bulk access & local response egress, the latter implements config ROM.
// scalarInflight is the maximum number or ordinary (non-bulk) memory requests
abstract class MemIf(
  mcIdx: Int,
  scalarInflight: Int,
  numReq: Int,
) extends Module {
  assert(scalarInflight <= 32768)

  // Common IO
  val req = IO(Vec(numReq, Flipped(Decoupled(new Flit))))

  val ringIn  = IO(Flipped(Decoupled(new RingResp)))
  val ringOut = IO(Decoupled(new RingResp))

  val mem = IO(new Bundle {
    val req  = Decoupled(new GlobalMemReq)
    val resp = Flipped(Valid(new GlobalMemResp))
  })

  protected val flitArb = Module(new RRArbiter(req(0).bits.cloneType, numReq))
  for (ci <- 0 until numReq) flitArb.io.in(ci) <> req(ci)
  protected val flit = flitArb.io.out

  // Common scalar l/s states
  protected val scalarAllocated = RegInit(VecInit.fill(scalarInflight)(false.B))
  protected val scalarPendings  = Reg(Vec(scalarInflight, new ScalarPending))
  protected val scalarIssued    = RegInit(VecInit.fill(scalarInflight)(false.B))
  protected val scalarCompleted = RegInit(VecInit.fill(scalarInflight)(false.B))
  protected val scalarBuffer    = Reg(Vec(scalarInflight, UInt(256.W)))
  // Buffers either write data, or response data, or both (for AMO)
  // We cannot reduce this to 32 bit: ICache fetches whole lines, and AMOs

  // -- Scalar bus: request allocation & deallocation --
  // This flit is accepted by the scalar request allcation logic
  protected val scalarAcceptFlit = Wire(Bool())

  // Scalar allocation
  private val (isScalar, parsedScalar, parsedData) = ScalarPending.fromFlit(flit.bits)

  private val scalarAlloc = PriorityEncoderOH(scalarAllocated.map(a => !a))
  private val scalarAllocValid = scalarAlloc.reduce(_ || _)
  scalarAcceptFlit := isScalar && scalarAllocValid

  when(flit.fire && isScalar) {
    for(s <- 0 until scalarInflight) {
      when(scalarAlloc(s)) {
        scalarAllocated(s) := true.B
        scalarPendings(s) := parsedScalar
        scalarBuffer(s) := parsedData
        scalarIssued(s) := false.B
        scalarCompleted(s) := false.B
      }
    }
  }

  // Scalar request deallocation happens on response being consumed by either ring or local resp port
  // Used by subclasses
  val scalarEject = Wire(Decoupled(new RingResp))
  val scalarEjectSel = PriorityEncoderOH((scalarAllocated zip scalarCompleted).map { case (a, c) => a && c })
  scalarEject.valid := VecInit(scalarEjectSel).asUInt.orR
  scalarEject.bits.dst := Mux1H(scalarEjectSel, scalarPendings.map(_.src))
  scalarEject.bits.id := Mux1H(scalarEjectSel, scalarPendings.map(_.id))
  scalarEject.bits.data := Mux1H(scalarEjectSel, scalarBuffer)
  when(scalarEject.fire) {
    for(s <- 0 until scalarInflight) {
      when(scalarEjectSel(s)) {
        scalarAllocated(s) := false.B
      }
    }
  }

  // Scalar request pathway
  private val scalarReqArb = Module(new RRArbiter(new ScalarPending, scalarInflight))
  private val scalarReqs = for (s <- 0 until scalarInflight) yield {
    val r = scalarReqArb.io.in(s)
    r.valid := scalarAllocated(s) && !scalarIssued(s)
    r.bits := scalarPendings(s)
    when(r.fire) {
      scalarIssued(s) := true.B
    }
  }
  private val scalarReqData = Mux1H(scalarReqArb.io.in.map(_.fire), scalarBuffer)

  protected val scalarReq = Wire(Decoupled(new GlobalMemReq))
  scalarReq.valid := scalarReqArb.io.out.valid
  scalarReqArb.io.out.ready := scalarReq.ready
  scalarReq.bits.id := scalarReqArb.io.chosen // Zero extended slot idx, highest bit == 0 => scalar req
  scalarReq.bits.addr := scalarReqArb.io.out.bits.address
  scalarReq.bits.wdata := Fill(8, scalarReqData)
  scalarReq.bits.wbe := Fill(8, "b1111".U(4.W)) // TDOO: parametric bus width, width check
  scalarReq.bits.size := scalarReqArb.io.out.bits.size
  assert(!scalarReqArb.io.out.valid || scalarReqArb.io.out.bits.size <= 5.U, "Unsupported scalar access size")
  scalarReq.bits.write := scalarReqArb.io.out.bits.ty === ScalarType.Store // TODO: handle RMW for subline writes, or let's have a LLDC

  // Scalar response pathway
  // Instantiated by subclasses
  val scalarResp: ValidIO[GlobalMemResp]
  when(scalarResp.valid) {
    // TODO: explicitly slice, and assert that the extension is zero
    scalarCompleted(scalarResp.bits.id) := true.B
    // May overwrites wdata, but we're fine with that.
    // When introducing AMO, this has to be fixed.
    scalarBuffer(scalarResp.bits.id) := scalarResp.bits.rdata

    assert(scalarAllocated(scalarResp.bits.id), "Received response for non-allocated slot")
    assert(!scalarCompleted(scalarResp.bits.id), "Received response for already completed slot")
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

  val ringSend = Wire(Decoupled(new RingResp))
  val ringFwdQueue = Module(new Queue(new RingResp, 2))
  val ringSendGated = Wire(Decoupled(new RingResp))
  ringSendGated.bits := ringSend.bits
  ringSendGated.valid := ringSend.valid && ringFwdQueue.io.count === 0.U
  ringSend.ready := ringSendGated.ready && ringFwdQueue.io.count === 0.U

  val (ringRecv, ringFwd) = splitLocal(ringIn)
  val (scalarRecv, scalarSend) = splitLocal(scalarEject)

  val ringPushArb = Module(new Arbiter(new RingResp, 2))
  ringPushArb.io.in(0) <> ringFwd
  ringPushArb.io.in(1) <> ringSendGated
  ringFwdQueue.io.enq <> ringPushArb.io.out
  ringFwdQueue.io.deq <> ringOut
}

class DRAMIf(
  mcIdx: Int,
  puStart: Int,     // First PU ID in zone (1-based, inclusive)
  puEnd: Int,       // Last PU ID in zone (1-based, inclusive)
  numClusters: Int, // Clusters in this zone
  scalarInflight: Int,
  bulkInflight: Int, // Number of inflight sub-line for the ONLY pending bulk request
) extends MemIf(mcIdx, scalarInflight, numClusters) {
  require(numClusters >= 1)
  require(puEnd >= puStart)
  require(puEnd - puStart + 1 == numClusters * 16, "PU IDs must map cleanly to clusters")
  require(puStart == mcIdx * numClusters * 16 + 1, s"mcIdx ($mcIdx) must align with puStart ($puStart), expected ${mcIdx * numClusters * 16 + 1}")
  require(isPow2(bulkInflight), "bulkInflight must be a power of 2")

  val bulkSubIdWidth = log2Ceil(bulkInflight)
  require(bulkSubIdWidth + 1 <= 8, "bulk request ID must fit in 8 bits")

  // DRAMIf has response egress ports into local clusters
  val resp = IO(Vec(numClusters, Decoupled(new RingResp)))

  // DRAMIf-specific state for scatter/broadcast
  // Scatter: multi-read command (tag=0xFF02) followed by multiple beats of response data,
  // broadcast to all clusters one beat at a time. During broadcast, normal local responses are paused.

  def isLocal(puId: UInt): Bool = puId >= puStart.U && puId <= puEnd.U
  def clusterOf(puId: UInt): UInt = (puId - puStart.U) >> 4

  val bulkAllocated = RegInit(false.B)
  val bulkPending = Reg(new BulkPending)
  val bulkBuffer = Reg(Vec(bulkInflight, UInt(256.W))) // Buffer for the different bits of the ONLY pending bulk request
  val bulkCompleted = RegInit(VecInit.fill(bulkInflight)(false.B))

  // -- Bulk bus handling: allocation & dealloc --
  // Also handles real bus connection

  // Alloc
  val (isBulk, parsedBulk) = BulkPending.initFromFlit(flit.bits)
  val bulkAcceptFlit = isBulk && !bulkAllocated
  when(flit.fire && isBulk) {
    assert(!bulkAllocated, "Received new bulk request while previous one is still pending")
    assert(!bulkCompleted.asUInt.orR, "Bulk completed bits should be cleared when no bulk is pending")
    bulkAllocated := true.B
    bulkPending := parsedBulk
  }

  flit.ready := scalarAcceptFlit || bulkAcceptFlit

  // Bulk requests: scatter and bulk copy
  // Highest bit == 1 => bulk req, rest is beat id
  def bulkId(beat: UInt) = {
    1.U(1.W) ## 0.U((7 - bulkSubIdWidth).W) ## beat(bulkSubIdWidth - 1, 0)
  }

  // FIXME: assert alignment
  val bulkReq = Wire(Decoupled(new GlobalMemReq))
  bulkReq.valid := bulkAllocated && !bulkPending.isFullyIssued && bulkPending.canIssue(bulkInflight)
  bulkReq.bits.id := bulkId(bulkPending.issueCnt)
  bulkReq.bits.addr := bulkPending.issueAddr
  bulkReq.bits.wdata := DontCare // Right now we only do bulk loads
  bulkReq.bits.wbe := 0.U
  bulkReq.bits.size := 5.U // 32 bytes per beat
  bulkReq.bits.write := false.B
  when(bulkReq.fire) {
    bulkPending.issueCnt := bulkPending.issueCnt + 1.U
    bulkCompleted(bulkPending.issueCnt) := false.B
  }

  // For DRAMIf, bulk req takes unconditional priority over scalar reqs, so we're using a plain Arbiter here
  // TODO: do we also block scalarReq if bulkInflight is saturated?

  val reqArb = Module(new Arbiter(new GlobalMemReq, 2))
  reqArb.io.in(0) <> bulkReq
  reqArb.io.in(1) <> scalarReq
  mem.req <> reqArb.io.out

  // Response handling
  override lazy val scalarResp: ValidIO[GlobalMemResp] = Wire(Valid(new GlobalMemResp))
  scalarResp.valid := mem.resp.valid && mem.resp.bits.id(7) === 0.U
  scalarResp.bits := mem.resp.bits
  val respIsBulk = mem.resp.valid && mem.resp.bits.id(7) === 1.U
  val respBulkBeatId = mem.resp.bits.id(bulkSubIdWidth - 1, 0)
  when(respIsBulk) {
    bulkBuffer(respBulkBeatId) := mem.resp.bits.rdata
    bulkCompleted(respBulkBeatId) := true.B
  }

  // Bulk completion state machine
  // Each resp port has a decoupled state. The state machine steps if all resp port has accepted the current beat
  val bulkRespValid = bulkAllocated && bulkCompleted(bulkPending.completedCnt)
  val bulkStep = Wire(Bool())
  when(bulkStep) {
    val bulkCompletedCntNext = bulkPending.completedCnt + 1.U
    bulkPending.completedCnt := bulkCompletedCntNext
    when(bulkPending.isCompletedNext) {
      bulkAllocated := false.B
    }
  }

  // Bulk unicast
  val bulkUcstValid = bulkRespValid && !BulkType.isBroadcast(bulkPending.ty)
  val bulkUcstEject = Wire(Decoupled(new RingResp))
  bulkUcstEject.valid := bulkUcstValid
  bulkUcstEject.bits.dst := bulkPending.src
  bulkUcstEject.bits.id := bulkPending.tag
  bulkUcstEject.bits.data := bulkBuffer(bulkPending.completedCnt)
  val bulkUcstStep = bulkUcstEject.fire

  // Bulk Broadcast
  val bcstValid = bulkRespValid && BulkType.isBroadcast(bulkPending.ty)
  val bcstAccepted = RegInit(0.U(numClusters.W))
  val bcstAccept = Wire(Vec(numClusters, Bool()))
  val bcstAcceptedUpdated = bcstAccepted | bcstAccept.asUInt
  val bcstStep = bcstAcceptedUpdated.andR
  bcstAccepted := Mux(bcstStep, 0.U(numClusters.W), bcstAcceptedUpdated)
  val bcstDists = for (ci <- 0 until numClusters) yield {
    val bcstDist = Wire(Decoupled(new RingResp))
    bcstDist.suggestName(s"bcstDist_$ci")
    bcstDist.valid := bcstValid && !bcstAccepted(ci)
    bcstDist.bits.dst := 0xFFFF.U
    bcstDist.bits.id := bulkPending.tag
    bcstDist.bits.data := bulkBuffer(bulkPending.completedCnt)
    bcstAccept(ci) := bcstDist.fire
    bcstDist
  }

  bulkStep := bcstStep || bulkUcstStep

  val (bulkUcstRecv, bulkUcstSend) = splitLocal(bulkUcstEject)

  // Arbitration for ring send:
  // 1. scalarSend
  // 2. bulkUcstSend
  val scalarSendArb = Module(new Arbiter(new RingResp, 2))
  scalarSendArb.io.in(0) <> scalarSend
  scalarSendArb.io.in(1) <> bulkUcstSend
  ringSend <> scalarSendArb.io.out

  // Arbitration for each resp port & ring egress:
  // 1. ringRecv (only for local resp)
  // 2. scalarEject
  // 3. bulkUcstEject
  // 4. bcstEject (only local resp)
  def localDist(in: DecoupledIO[RingResp], name: String): Seq[DecoupledIO[RingResp]] = {
    val clusterMask = for (ci <- 0 until numClusters) yield {
      clusterOf(in.bits.dst) === ci.U
    }

    when(in.valid) {
      assert(isLocal(in.bits.dst), cf"$name: Received non-local response with dst ${in.bits.dst}")
      assert(PopCount(clusterMask) === 1.U, cf"$name: Received response with dst ${in.bits.dst} that matches none / multiple clusters")
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
  val scalarDist = localDist(scalarRecv, "scalarDist")
  val bulkUcstDist = localDist(bulkUcstRecv, "bulkUcstDist")

  for (ci <- 0 until numClusters) {
    val arb = Module(new Arbiter(new RingResp, 4)).suggestName(s"distArb_$ci")
    arb.io.in(0) <> ringDist(ci)
    arb.io.in(1) <> scalarDist(ci)
    arb.io.in(2) <> bulkUcstDist(ci)
    arb.io.in(3) <> bcstDists(ci)

    resp(ci) <> arb.io.out
  }
}

// No distributor, no scatter / bulk, can have configROM
class PeripheralIf(
  externalInflight: Int,
  configROM: Map[BigInt, BigInt] = Map.empty,
) extends MemIf(0, externalInflight, 1) {
  override lazy val scalarResp: ValidIO[GlobalMemResp] = Wire(Valid(new GlobalMemResp))

  val configHits = configROM.map({ case (addr, data) => ((scalarReq.bits.addr(31, 5) << 5) === addr.U, data.U(256.W)) }).toSeq
  val configHit = VecInit(configHits.map(_._1)).asUInt.orR
  val configReadout = Mux1H(configHits)
  val configGrant = !mem.resp.valid
  val configResp = Wire(Valid(new GlobalMemResp))
  configResp.valid := scalarReq.valid && configHit && configGrant
  configResp.bits.id := scalarReq.bits.id
  configResp.bits.rdata := configReadout

  mem.req.valid := scalarReq.valid && !configHit
  mem.req.bits := scalarReq.bits
  scalarReq.ready := Mux(configHit, configGrant, mem.req.ready)

  // Response arbitration
  scalarResp.bits := Mux(mem.resp.valid, mem.resp.bits, configResp.bits)
  scalarResp.valid := mem.resp.valid || configResp.valid

  // We don't have any local distributor
  def isLocal(puId: UInt): Bool = false.B
  ringRecv.ready := DontCare
  assert(!ringRecv.valid)
  scalarRecv.ready := DontCare
  assert(!scalarRecv.valid)

  flit.ready := scalarAcceptFlit

  ringSend <> scalarSend
}