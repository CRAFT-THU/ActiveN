package koneko.bus

import chisel3._
import chisel3.util._

import koneko._

// Memory interface: consumes MC eject flits from the NoC, issues memory ops,
// and routes responses back to PUs via cluster resp ports or inter-MemIf ring bus.
//
// Flit collection: multi-flit requests (2 for loads, 3 for stores) are assembled
// using a CAM on src with remainingFlits > 0.
//
// Ring bus: responses for PUs outside this MemIf's zone are forwarded via a
// 2-deep ring buffer. Deadlock avoidance: only push internal→ring when ring
// buffer is empty; ring input for local PUs delivered directly.

class MemPending extends Bundle {
  val src     = UInt(16.W)
  val id      = UInt(16.W)
  val address = UInt(32.W)
  val write   = Bool()
  val size    = UInt(2.W)
  val wdata   = UInt(32.W)
}

class GlobalMemReq extends Bundle {
  val id    = UInt(16.W)
  val addr  = UInt(32.W)
  val wdata = UInt(256.W)
  val size  = UInt(8.W)
  val write = Bool()
}

class GlobalMemResp extends Bundle {
  val id    = UInt(16.W)
  val rdata = UInt(256.W)
}

class RingResp extends Bundle {
  val dst  = UInt(16.W)
  val id   = UInt(16.W)
  val data = UInt(256.W)
}

class MemIf(
  mcIdx: Int,
  puStart: Int,     // First PU ID in zone (1-based, inclusive)
  puEnd: Int,       // Last PU ID in zone (1-based, inclusive)
  numClusters: Int, // Clusters in this zone
  maxInflight: Int,
  // Optional config ROM: maps 32-byte-aligned addresses to 256-bit values.
  // When a read falls in this range, it is answered locally without issuing to memory.
  configROM: Map[BigInt, BigInt] = Map.empty,
) extends Module {
  require(numClusters >= 1)
  require(puEnd >= puStart)

  private val slotIdxWidth = log2Ceil(maxInflight max 2)

  private def bump(idx: UInt): UInt =
    Mux(idx === (maxInflight - 1).U, 0.U(slotIdxWidth.W), idx + 1.U)

  private def rrPick(candidates: Seq[Bool], start: UInt): (Bool, UInt) = {
    val candBits = VecInit(candidates).asUInt
    val doubled = Cat(candBits, candBits)
    val shifted = (doubled >> start)(maxInflight - 1, 0)
    val hasAny = shifted.orR
    val relIdx = PriorityEncoder(shifted)
    val sum = relIdx +& start
    val slot = Wire(UInt(slotIdxWidth.W))
    slot := Mux(sum >= maxInflight.U, sum - maxInflight.U, sum)(slotIdxWidth - 1, 0)
    (hasAny, slot)
  }

  // --- IO ---
  val req = IO(Vec(numClusters, Flipped(Decoupled(new Bundle {
    val src  = UInt(16.W)
    val dst  = UInt(16.W)
    val data = UInt(32.W)
    val tag  = UInt(16.W)
  }))))

  val resp = IO(Vec(numClusters, Decoupled(new RingResp)))

  val ringIn  = IO(Flipped(Decoupled(new RingResp)))
  val ringOut = IO(Decoupled(new RingResp))

  val mem = IO(new Bundle {
    val req  = Decoupled(new GlobalMemReq)
    val resp = Flipped(Valid(new GlobalMemResp))
  })

  // --- State ---
  val allocated      = RegInit(VecInit.fill(maxInflight)(false.B))
  val pending        = Reg(Vec(maxInflight, new MemPending))
  val remainingFlits = Reg(Vec(maxInflight, UInt(2.W)))
  val ready          = RegInit(VecInit.fill(maxInflight)(false.B))
  val issued         = RegInit(VecInit.fill(maxInflight)(false.B))
  val completed      = RegInit(VecInit.fill(maxInflight)(false.B))
  val data           = Reg(Vec(maxInflight, UInt(256.W)))
  val issueStart     = RegInit(0.U(slotIdxWidth.W))
  val localRespStart = RegInit(0.U(slotIdxWidth.W))
  val remoteRespStart = RegInit(0.U(slotIdxWidth.W))
  val ringEnqPreferIncoming = RegInit(true.B)

  def isLocal(puId: UInt): Bool = puId >= puStart.U && puId <= puEnd.U
  def clusterOf(puId: UInt): UInt = (puId - puStart.U) >> 4

  // --- Scatter (CSR-aware broadcast) FSM ---
  // Collects 2-flit scatter commands (tag=0xFF02: base, end),
  // issues sequential DRAM reads, broadcasts each beat to all clusters.
  val sScatIdle :: sScatCollect :: sScatIssue :: sScatWait :: sScatBcast :: Nil = Enum(5)
  val scatState   = RegInit(sScatIdle)
  val scatBase    = Reg(UInt(32.W))
  val scatEnd     = Reg(UInt(32.W))
  val scatAddr    = Reg(UInt(32.W))  // current read address (absolute)
  val scatData    = Reg(UInt(256.W))
  val scatCluster = RegInit(0.U(log2Ceil(math.max(numClusters, 2)).W))
  val scatSrc     = Reg(UInt(16.W))  // source PU (for CAM matching of 2nd flit)
  val scatSlotId  = maxInflight.U(16.W)  // out-of-band id for scatter reads

  // --- 1. Flit collection ---
  // Use a single RRArbiter to select one flit per cycle, avoiding
  // multi-port allocation conflicts entirely.
  val flitArb = Module(new RRArbiter(req(0).bits.cloneType, numClusters))
  for (ci <- 0 until numClusters) flitArb.io.in(ci) <> req(ci)

  val flit = flitArb.io.out
  val src  = flit.bits.src
  val tag  = flit.bits.tag
  val fd   = flit.bits.data

  val isMem = tag === 0xFF00.U || tag === 0xFF01.U
  val isScatCmd = tag === 0xFF02.U
  val scatCanAccept = isScatCmd && (scatState === sScatIdle || (scatState === sScatCollect && src === scatSrc))

  val camHits = VecInit((0 until maxInflight).map { s =>
    allocated(s) && !ready(s) && pending(s).src === src && remainingFlits(s) > 0.U
  })
  val camHit  = camHits.asUInt.orR
  val camSlot = PriorityEncoder(camHits.asUInt)

  val freeSlots = VecInit((0 until maxInflight).map(s => !allocated(s)))
  val hasFree   = freeSlots.asUInt.orR
  val freeSlot  = PriorityEncoder(freeSlots.asUInt)

  // Non-memory flits: accept and drop (except scatter commands)
  // Memory flits: CAM hit or allocate new slot
  // Scatter flits: accepted by scatter FSM
  flit.ready := camHit || (isMem && hasFree) || (!isMem && !isScatCmd) || scatCanAccept

  when(flit.fire && isScatCmd) {
    when(scatState === sScatIdle) {
      // First scatter flit: base address
      scatBase := fd
      scatAddr := fd
      scatSrc  := src
      scatState := sScatCollect
    }.elsewhen(scatState === sScatCollect) {
      // Second scatter flit: end address
      scatEnd  := fd
      scatState := sScatIssue
    }
  }

  when(flit.fire && !isScatCmd && (camHit || isMem)) {
    when(camHit) {
      val s = camSlot
      val totalFlits = Mux(pending(s).write, 3.U, 2.U)
      val flitIdx    = totalFlits - remainingFlits(s)
      when(flitIdx === 1.U) {
        pending(s).id := fd(15, 0)
        when(pending(s).write) { pending(s).size := fd(17, 16) }
      }
      when(flitIdx === 2.U) {
        pending(s).wdata := fd
      }
      remainingFlits(s) := remainingFlits(s) - 1.U
      when(remainingFlits(s) === 1.U) { ready(s) := true.B }
    }.otherwise {
      val s = freeSlot
      allocated(s)      := true.B
      pending(s).src     := src
      pending(s).address := fd
      pending(s).write   := tag === 0xFF01.U
      pending(s).id      := 0.U
      pending(s).size    := 0.U
      pending(s).wdata   := 0.U
      ready(s)           := false.B
      issued(s)          := false.B
      completed(s)       := false.B
      remainingFlits(s)  := Mux(tag === 0xFF01.U, 2.U, 1.U)
    }
  }

  // --- 2. Issue to external memory ---
  // Config ROM: handle reads locally without issuing to memory
  val configRomEntries = configROM.toSeq.sortBy(_._1)
  val hasConfigROM = configRomEntries.nonEmpty

  val readyToIssueSeq = (0 until maxInflight).map(s =>
    allocated(s) && ready(s) && !issued(s)
  )
  val (issueValid, issueSlot) = rrPick(readyToIssueSeq, issueStart)
  val issuePend  = pending(issueSlot)

  // Config ROM hit: intercept reads to config addresses
  val configHit  = WireDefault(false.B)
  val configData = WireDefault(0.U(256.W))
  if (hasConfigROM) {
    val alignedAddr = issuePend.address & ~(31.U(32.W))
    for ((addr, value) <- configRomEntries) {
      when(alignedAddr === addr.U && !issuePend.write) {
        configHit  := true.B
        configData := value.U(256.W)
      }
    }
  }

  // Handle config ROM hits immediately (mark completed without issuing to memory)
  when(issueValid && configHit) {
    data(issueSlot)      := configData
    issued(issueSlot)    := true.B
    completed(issueSlot) := true.B
    issueStart           := bump(issueSlot)
  }

  val scatWantsIssue = scatState === sScatIssue

  // Normal has priority over scatter; skip config ROM hits
  val normalIssue = issueValid && !configHit
  mem.req.valid     := normalIssue || scatWantsIssue
  mem.req.bits.id   := Mux(normalIssue, issueSlot, scatSlotId)
  mem.req.bits.addr := Mux(normalIssue, issuePend.address, scatAddr)
  mem.req.bits.write := Mux(normalIssue, issuePend.write, false.B)
  // Replicate 32-bit wdata across all words; byte-enables select correct bytes
  mem.req.bits.wdata := Mux(normalIssue, Fill(8, issuePend.wdata), 0.U)
  val byteOff   = issuePend.address(4, 0)
  val byteCount = (1.U << issuePend.size)(3, 0)
  val baseMask  = (1.U << byteCount)(4, 0) - 1.U
  mem.req.bits.wbe := Mux(normalIssue && issuePend.write, baseMask << byteOff, 0.U)

  when(mem.req.fire) {
    when(normalIssue) {
      issued(issueSlot) := true.B
      when(issuePend.write) { completed(issueSlot) := true.B }
      issueStart := bump(issueSlot)
    }.otherwise {
      scatState := sScatWait
    }
  }

  // --- 3. Receive memory response ---
  val respId = mem.resp.bits.id
  val isScatResp = mem.resp.valid && respId === scatSlotId && scatState === sScatWait

  // Normal response: guard with allocated check and scatter exclusion
  when(mem.resp.valid && !isScatResp && respId < maxInflight.U && allocated(respId)) {
    data(respId)      := mem.resp.bits.rdata
    completed(respId) := true.B
  }

  // Scatter response: capture data and start broadcast
  when(isScatResp) {
    scatData    := mem.resp.bits.rdata
    scatState   := sScatBcast
    scatCluster := 0.U
  }

  // --- 4. Response delivery ---
  // Default: deassert all resp ports
  val respBusy = Wire(Vec(numClusters, Bool()))
  for (ci <- 0 until numClusters) {
    resp(ci).valid := false.B
    resp(ci).bits  := 0.U.asTypeOf(new RingResp)
    respBusy(ci) := false.B
  }

  // Ring output buffer (2-deep)
  val ringBuf = Module(new Queue(new RingResp, 2))
  ringBuf.io.enq.valid := false.B
  ringBuf.io.enq.bits  := 0.U.asTypeOf(new RingResp)
  ringOut <> ringBuf.io.deq

  // Find first completed local entry
  val localCandidatesSeq = (0 until maxInflight).map(s =>
    allocated(s) && completed(s) && isLocal(pending(s).src)
  )
  val (hasLocal, localSlot) = rrPick(localCandidatesSeq, localRespStart)

  // Scatter broadcast: deliver to each cluster one at a time.
  // While broadcasting, normal local delivery is paused.
  when(scatState === sScatBcast) {
    val ci = scatCluster
    resp(ci).valid     := true.B
    resp(ci).bits.dst  := 0xFFFF.U
    resp(ci).bits.id   := 0xFFFF.U
    resp(ci).bits.data := scatData
    respBusy(ci)       := true.B
    when(resp(ci).ready) {
      when(scatCluster === (numClusters - 1).U) {
        scatCluster := 0.U
        // Advance to next beat or finish
        val nextAddr = scatAddr + 32.U
        when(nextAddr >= scatEnd) {
          scatState := sScatIdle
        }.otherwise {
          scatAddr  := nextAddr
          scatState := sScatIssue
        }
      }.otherwise {
        scatCluster := scatCluster + 1.U
      }
    }
  }.otherwise {
    when(hasLocal) {
      val s    = localSlot
      val puId = pending(s).src
      val ci   = clusterOf(puId)
      resp(ci).valid     := true.B
      resp(ci).bits.dst  := puId
      resp(ci).bits.id   := pending(s).id
      resp(ci).bits.data := data(s)
      respBusy(ci)       := true.B
      when(resp(ci).ready) {
        allocated(s) := false.B
        completed(s) := false.B
        ready(s)     := false.B
        issued(s)    := false.B
        localRespStart := bump(localSlot)
      }
    }
  }

  // Find first completed remote entry
  val remoteCandidatesSeq = (0 until maxInflight).map(s =>
    allocated(s) && completed(s) && !isLocal(pending(s).src)
  )
  val (hasRemote, remoteSlot) = rrPick(remoteCandidatesSeq, remoteRespStart)

  // --- 5. Ring input ---
  // Local PUs: deliver directly (overrides local completion on same cluster).
  // Remote: forward to ring output.
  val ringLocal = isLocal(ringIn.bits.dst)
  val ringCi    = clusterOf(ringIn.bits.dst)
  val ringForwardValid = ringIn.valid && !ringLocal
  val chooseIncomingRing = WireDefault(false.B)
  val chooseRemoteRing = WireDefault(false.B)

  when(ringBuf.io.enq.ready) {
    when(ringForwardValid && hasRemote) {
      chooseIncomingRing := ringEnqPreferIncoming
      chooseRemoteRing := !ringEnqPreferIncoming
    }.elsewhen(ringForwardValid) {
      chooseIncomingRing := true.B
    }.elsewhen(hasRemote) {
      chooseRemoteRing := true.B
    }
  }

  when(chooseRemoteRing) {
    val s = remoteSlot
    ringBuf.io.enq.valid     := true.B
    ringBuf.io.enq.bits.dst  := pending(s).src
    ringBuf.io.enq.bits.id   := pending(s).id
    ringBuf.io.enq.bits.data := data(s)
    allocated(s) := false.B
    completed(s) := false.B
    ready(s)     := false.B
    issued(s)    := false.B
    remoteRespStart := bump(remoteSlot)
  }

  ringIn.ready := false.B

  when(ringIn.valid) {
    when(ringLocal) {
      when(!respBusy(ringCi)) {
        resp(ringCi).valid := true.B
        resp(ringCi).bits  := ringIn.bits
        ringIn.ready := resp(ringCi).ready
      }
    }.otherwise {
      when(chooseIncomingRing) {
        ringIn.ready             := true.B
        ringBuf.io.enq.valid     := true.B
        ringBuf.io.enq.bits      := ringIn.bits
      }
    }
  }

  when(ringBuf.io.enq.fire && ringForwardValid && hasRemote) {
    ringEnqPreferIncoming := !ringEnqPreferIncoming
  }
}