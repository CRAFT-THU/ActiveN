package koneko.fetch

import chisel3._
import chisel3.util._

import koneko._

class Metadata(implicit val params: CoreParameters) extends Bundle {
  val valid = Bool()
  val tag = UInt(params.i$TagLen.W)
}

class ICache(implicit val params: CoreParameters) extends Module {
  val input = IO(Flipped(Decoupled(UInt(32.W))))
  val kill = IO(Input(Bool()))
  val mem = IO(new Bundle {
    val req = Decoupled(new MemReq)
    val resp = Flipped(Valid(new MemResp))
  })
  val output = IO(Output(UInt(32.W)))

  /*
   * Storages
   */
  val metadata = Mem(params.i$Sets, Vec(params.i$Assoc, new Metadata))
  val data = Mem(params.i$Sets * params.i$BlockSize / 4, Vec(params.i$Assoc, UInt(32.W)))

  // PLRU state: (assoc-1) bits per set, tree-based pseudo-LRU
  val plruBits = if(params.i$Assoc > 1) params.i$Assoc - 1 else 0
  val plruState = if(plruBits > 0) Some(Mem(params.i$Sets, UInt(plruBits.W))) else None

  // PLRU helper: given tree state, find victim way index
  def plruVictim(state: UInt): UInt = {
    if(params.i$Assoc == 1) {
      0.U
    } else if(params.i$Assoc == 2) {
      state(0)
    } else {
      // Tree-based PLRU for power-of-2 associativity
      val depth = log2Up(params.i$Assoc)
      val wayBits = Wire(Vec(depth, Bool()))
      var node = 0.U(log2Up(plruBits).W)
      for(level <- 0 until depth) {
        if(level == 0) {
          wayBits(level) := state(0)
          node = 0.U
        } else {
          // Node index = 2*parent + 1 + direction
          val nodeIdx = Wire(UInt(log2Up(plruBits).W))
          nodeIdx := node * 2.U + 1.U + wayBits(level - 1).asUInt
          // Read the state bit at this node
          val nodeBits = VecInit((0 until plruBits).map(i => state(i)))
          wayBits(level) := nodeBits(nodeIdx)
          node = nodeIdx
        }
      }
      wayBits.asUInt
    }
  }

  // PLRU helper: update tree state after accessing a specific way
  def plruUpdate(oldState: UInt, way: UInt): UInt = {
    if(params.i$Assoc <= 1) {
      0.U
    } else if(params.i$Assoc == 2) {
      // Point away from accessed way
      ~way(0)
    } else {
      val depth = log2Up(params.i$Assoc)
      val newState = Wire(Vec(plruBits, Bool()))
      for(i <- 0 until plruBits) newState(i) := oldState(i)
      // Walk from root to the leaf corresponding to 'way', flipping bits to point away
      var nodeIdx = 0
      for(level <- 0 until depth) {
        val nodesAtLevel = 1 << level
        for(n <- 0 until nodesAtLevel) {
          val treeIdx = n + nodesAtLevel - 1
          if(treeIdx < plruBits) {
            // This node corresponds to ways that start at n * (assoc / nodesAtLevel)
            val waysPerNode = params.i$Assoc / (nodesAtLevel * 2)
            val leftStart = n * 2 * waysPerNode
            val rightStart = leftStart + waysPerNode
            // Check if 'way' is in left subtree (point right) or right subtree (point left)
            val wayIdx = way
            val inLeft = wayIdx >= leftStart.U && wayIdx < rightStart.U
            val inRight = wayIdx >= rightStart.U && wayIdx < (rightStart + waysPerNode).U
            when(inLeft) {
              newState(treeIdx) := true.B  // Point right (away from accessed)
            }.elsewhen(inRight) {
              newState(treeIdx) := false.B // Point left (away from accessed)
            }
          }
        }
      }
      newState.asUInt
    }
  }

  val s0pc = input.bits
  val s0pcidx = dontTouch((s0pc >> params.i$OffsetLen)(params.i$IndexLen - 1, 0))
  val s0pcdataidx = s0pc(params.i$IndexLen + params.i$OffsetLen - 1, 2)
  val s0step = Wire(Bool())

  val s1pc = RegEnable(s0pc, s0step)
  val s1pcidx = (s1pc >> params.i$OffsetLen)(params.i$IndexLen - 1, 0)

  val s1pctag = dontTouch(s1pc >> (params.i$OffsetLen + params.i$IndexLen))
  val s1metadata = RegEnable(metadata(s0pcidx), s0step)
  val s1data = RegEnable(data(s0pcdataidx), s0step)
  val s1plru = if(plruBits > 0) Some(RegEnable(plruState.get(s0pcidx), s0step)) else None
  val s1valid = RegEnable(input.valid, false.B, s0step)

  val s1hitmap = VecInit(s1metadata.map(e => e.valid && e.tag === s1pctag))
  val s1hit = s1hitmap.asUInt.orR
  val s1datamux = Mux1H(s1hitmap.zip(s1data))

  // Find the hit way index (for PLRU update on hit)
  val s1hitWay = OHToUInt(s1hitmap)

  val s1reset = RegInit(true.B)
  val s1rstCnt = RegInit(0.U(params.i$IndexLen.W))
  s1reset := Mux(s1rstCnt.andR, false.B, s1reset)
  s1rstCnt := Mux(s1reset, s1rstCnt + 1.U, s1rstCnt)

  val s1refillCnt = RegInit(0.U((params.i$OffsetLen - 2).W))

  // PLRU victim selection: first try to find an invalid way, then use PLRU
  val s1invalidWay = PriorityEncoder(s1metadata.map(!_.valid))
  val s1allValid = VecInit(s1metadata.map(_.valid)).asUInt.andR
  val s1plruVictimWay = if(plruBits > 0) plruVictim(s1plru.get) else 0.U
  val s1victimAssoc = Mux(s1allValid, s1plruVictimWay, s1invalidWay)
  val s1victimMap = VecInit(Seq.tabulate(params.i$Assoc)(s1victimAssoc === _.U))

  val s1refilledCapture = Reg(UInt(32.W))

  // Refiller: sends multiple requests per cache line (one per memory bus beat)
  // TODO: send out multiple beats together
  val wordsPerBeat = params.memBusWidth / 32
  val beatsPerLine = params.i$BlockSize / (params.memBusWidth / 8)
  val wordBits = log2Ceil(wordsPerBeat)

  val s1refillBeatData = Reg(UInt(params.memBusWidth.W))
  val s1refillWriting = RegInit(false.B) // serializing writes from a received beat
  val s1reqSentForBeat = Reg(Bool())
  val s1refillCompleted = RegInit(false.B)

  // Request: word-aligned address for current beat
  val refillBaseAddr = s1pctag ## s1pcidx ## 0.U(params.i$OffsetLen.W)
  val curBeat = s1refillCnt >> wordBits
  val curWordInBeat = s1refillCnt(wordBits - 1, 0)
  val beatAddr = refillBaseAddr + (curBeat << log2Ceil(params.memBusWidth / 8))

  val needRefill = s1valid && !s1hit && !s1refillCompleted && !s1refillWriting

  mem.req.bits.addr := beatAddr
  mem.req.bits.size := log2Ceil(params.memBusWidth / 8).U
  mem.req.bits.id := curBeat
  mem.req.bits.wbe := 0.U
  mem.req.bits.write := false.B
  mem.req.bits.wdata := DontCare
  mem.req.valid := needRefill && !s1reqSentForBeat
  // Asserted that a response implies needRefill
  assert(!mem.resp.valid || needRefill, "Unexpected ICache memory response")

  s1reqSentForBeat := MuxCase(s1reqSentForBeat, Seq(
    s0step -> false.B,
    mem.req.fire -> true.B,
  ))

  // Receive response: latch wide data
  when(mem.resp.fire) {
    s1refillBeatData := mem.resp.bits.data
    s1refillWriting := true.B
  }

  // Serialize writes from the latched beat
  val beatWords = VecInit(Seq.tabulate(wordsPerBeat)(i =>
    s1refillBeatData((i + 1) * 32 - 1, i * 32)
  ))
  val dataWriteIdx = s1pcidx ## s1refillCnt
  val dataWriteVal = beatWords(curWordInBeat)
  val dataWriteEnable = s1refillWriting

  when(dataWriteEnable) {
    data.write(dataWriteIdx, VecInit(Seq.fill(params.i$Assoc)(dataWriteVal)), s1victimMap)
    s1refillCnt := s1refillCnt + 1.U
    when(curWordInBeat === (wordsPerBeat - 1).U) {
      s1refillWriting := false.B
      s1reqSentForBeat := false.B
    }
  }

  // Capture the word corresponding to the requested PC during refill
  val pcWordInLine = s1pc(params.i$OffsetLen - 1, 2)
  when(dataWriteEnable && s1refillCnt === pcWordInLine) {
    s1refilledCapture := dataWriteVal
  }

  val s1refillComplete = dataWriteEnable && s1refillCnt.andR
  s1refillCompleted := MuxCase(s1refillCompleted, Seq(
    s0step -> false.B,
    s1refillComplete -> true.B,
  ))
  val s1blocked = !s1hit && !s1refillCompleted

  // Killing
  val killed = Reg(Bool())
  killed := MuxCase(killed, Seq(
    s0step -> false.B,
    kill -> true.B,
  ))

  output := Mux(s1hit, s1datamux, s1refilledCapture)

  val metadataWriteIdx = Mux(s1reset, s1rstCnt, s1pcidx)
  val metadataWriteMask = Mux(s1reset, VecInit(Seq.fill(params.i$Assoc)(true.B)), s1victimMap)
  val metadataWriteVal = Wire(new Metadata)
  metadataWriteVal.valid := !s1reset
  metadataWriteVal.tag := s1pctag
  val metadataWriteEnable = s1reset || s1refillComplete
  when(metadataWriteEnable) {
    metadata.write(metadataWriteIdx, VecInit(Seq.fill(params.i$Assoc)(metadataWriteVal)), metadataWriteMask)
  }

  // PLRU state update: on hit, update to point away from hit way; on refill, update for victim way
  if(plruBits > 0) {
    val plru = plruState.get
    val curPlru = s1plru.get
    when(s1valid && s1hit && !kill && !killed) {
      plru.write(s1pcidx, plruUpdate(curPlru, s1hitWay))
    }.elsewhen(s1refillComplete) {
      plru.write(s1pcidx, plruUpdate(curPlru, s1victimAssoc))
    }.elsewhen(s1reset) {
      plru.write(s1rstCnt, 0.U)
    }
  }

  // Scheduler
  s0step := !s1reset && (!s1valid || !s1blocked)
  input.ready := s0step
}
