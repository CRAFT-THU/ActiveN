package koneko.fetch

import chisel3._
import chisel3.util._

import koneko._

class Metadata(implicit val params: CoreParameters) extends Bundle {
  val valid = Bool()
  val tag = UInt(params.i$TagLen.W)
}

class ICache(implicit val params: CoreParameters) extends Module {
  val input = IO(Flipped(DecoupledIO(UInt(32.W))))
  val kill = IO(Input(Bool()))
  val mem = IO(new Bundle {
    val req = Decoupled(new MemReq)
    val resp = Flipped(Valid(new MemResp))
  })
  val output = IO(DecoupledIO(UInt(32.W)))

  /*(valid && isWFI)
   * Storages
   */
  // TODO: use SRAM API
  val metadata = Mem(params.i$Sets, Vec(params.i$Assoc, new Metadata))
  val data = Mem(params.i$Sets * params.i$BlockSize / 4, Vec(params.i$Assoc, UInt(32.W)))

  val s0pc = input.bits
  val s0pcidx = dontTouch((s0pc >> params.i$OffsetLen)(params.i$IndexLen - 1, 0))
  val s0pcdataidx = s0pc(params.i$IndexLen + params.i$OffsetLen - 1, 2)
  val s0step = Wire(Bool())

  val s1pc = RegEnable(s0pc, s0step)
  val s1pcidx = (s1pc >> params.i$OffsetLen)(params.i$IndexLen - 1, 0)

  val s1pctag = dontTouch(s1pc >> (params.i$OffsetLen + params.i$IndexLen))
  val s1metadata = RegEnable(metadata(s0pcidx), s0step)
  val s1data = RegEnable(data(s0pcdataidx), s0step)
  val s1valid = RegEnable(input.valid, false.B, s0step)
  val s1sent = Reg(Bool())

  val s1hitmap = VecInit(s1metadata.map(e => e.valid && e.tag === s1pctag))
  val s1hit = s1hitmap.asUInt.orR
  val s1datamux = Mux1H(s1hitmap.zip(s1data))

  val s1reset = RegInit(true.B)
  val s1rstCnt = RegInit(0.U(params.i$IndexLen.W))
  s1reset := Mux(s1rstCnt.andR, false.B, s1reset)
  s1rstCnt := Mux(s1reset, s1rstCnt + 1.U, s1rstCnt)

  val s1refillCnt = RegInit(0.U((params.i$OffsetLen - 2).W))
  val s1refillComplete = Wire(Bool())
  val s1victimAssoc = 0.U // TODO: impl prng assoc
  val s1victimMap = VecInit(Seq.tabulate(params.i$Assoc)(s1victimAssoc === _.U))
  // TODO: merge into s1datamux
  val s1refilledCapture = RegEnable(
    mem.resp.bits.data,
    mem.resp.valid && s1refillCnt === (s1pc(params.i$OffsetLen - 1, 0) >> 2)
  )
  output.bits := Mux(s1hit, s1datamux, s1refilledCapture)

  // Refiller
  mem.req.bits.addr := s1pctag ## s1pcidx ## 0.U(params.i$OffsetLen.W)
  mem.req.bits.burst := log2Up(params.i$BlockSize / 4).U
  mem.req.bits.wbe := 0.U
  mem.req.bits.write := false.B
  mem.req.bits.wdata := DontCare
  mem.req.valid := s1valid && !s1sent && !s1hit
  s1sent := MuxCase(s1sent, Seq(
    s0step -> false.B,
    mem.req.fire -> true.B
  ))
  val s1refillCompleted = Reg(Bool())
  s1refillCompleted := MuxCase(s1refillCompleted, Seq(
    s0step -> false.B,
    s1refillComplete -> true.B
  ))
  val s1blocked = !s1hit && !s1refillCompleted

  // Killing
  val killed = Reg(Bool())
  killed := MuxCase(killed, Seq(
    s0step -> false.B,
    kill -> true.B
  ))

  // Writing
  val dataWriteIdx = s1pcidx ## s1refillCnt
  val dataWriteVal = mem.resp.bits.data
  val dataWriteEnable = mem.resp.valid
  when(dataWriteEnable) {
    data.write(dataWriteIdx, VecInit(Seq.fill(params.i$Assoc)(dataWriteVal)), s1victimMap)
  }
  s1refillCnt := Mux(dataWriteEnable, s1refillCnt + 1.U, s1refillCnt)
  s1refillComplete := dataWriteEnable && s1refillCnt.andR

  val metadataWriteIdx = Mux(s1reset, s1rstCnt, s1pcidx)
  val metadataWriteMask = Mux(s1reset, VecInit(Seq.fill(params.i$Assoc)(true.B)), s1victimMap)
  val metadataWriteVal = Wire(new Metadata)
  metadataWriteVal.valid := !s1reset
  metadataWriteVal.tag := s1pctag
  val metadataWriteEnable = s1reset || s1refillComplete
  when(metadataWriteEnable) {
    metadata.write(metadataWriteIdx, VecInit(Seq.fill(params.i$Assoc)(metadataWriteVal)), metadataWriteMask)
  }

  // Scheduler
  s0step := !s1reset && (!s1valid || ((output.ready || kill || killed) && !s1blocked))
  input.ready := s0step
  output.valid := s1valid && !kill && !killed && !s1blocked
}
