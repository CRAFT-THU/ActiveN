package koneko.fetch

import chisel3._
import chisel3.util._

import koneko._

class Fetch(implicit val params: CoreParameters) extends Module {
  val INSTR_MEM_SIZE = 512

  val mem = IO(new Bundle {
    val req = Decoupled(new MemReq)
    val resp = Flipped(Valid(new MemResp))
  })
  val ctrl = IO(new Bundle {
    val br = Vec(params.pipeCnt, Flipped(Valid(UInt(32.W))))
  })
  val decoded = IO(Vec(params.pipeCnt, Decoupled(new uOp)))
  val busy = IO(Input(UInt(params.pipeCnt.W)))

  // PC Control
  val step = WireDefault(false.B)

  val pcs = RegInit(VecInit(Seq.fill(params.pipeCnt)(params.initVec.U(32.W))))
  val fetchable = Wire(UInt(params.pipeCnt.W))
  val sel = PriorityEncoderOH(fetchable)
  for(((pc, b), fetch) <- (pcs.zip(ctrl.br).zip(sel.asBools))) {
    pc := Mux(b.valid, b.bits, pc + Mux(fetch && step, 4.U, 0.U))
  }
  val selpc = Mux1H(sel.asBools, pcs)

  val refilling = RegInit(true.B)

  /*
  // Latch
  val sentFpc = RegEnable(selectedFpc, step)
  val sentSmsel = RegEnable(sel, step)

  // Fetch
  val icache = Module(new ICache)
  icache.mem <> mem
  icache.input.bits := selectedFpc
  icache.input.valid := true.B // FIXME: is this actually OK?
  icache.kill := VecInit(ctrl.br.zip(sentSmsel.asBools).map({ case (b, s) => b.valid && s })).asUInt.orR
  step := icache.input.fire
  */
  val instrMem = Mem(INSTR_MEM_SIZE / 4, UInt(32.W))
  val instrReadout = instrMem.read((selpc >> 2).asUInt)
  val instrValid = fetchable.orR && !VecInit(sel.asBools.zip(ctrl.br).map({ case (s, b) => s && b.valid })).asUInt.orR
  step := fetchable.orR && !refilling

  // Decode
  val decode = Module(new Decode)
  decode.pc := selpc
  decode.smsel := sel
  decode.instr := instrReadout

  val decodedHolding = for(_ <- 0 until params.pipeCnt) yield Reg(new uOp)
  val decodedHoldingValid = for(_ <- 0 until params.pipeCnt) yield RegInit(false.B)
  for(((d, dec), idx) <- decodedHoldingValid.zip(decoded).zipWithIndex) {
    d := MuxCase(d, Seq(
      (refilling || dec.ready) -> false.B,

      // dec is not ready
      sel(idx) -> true.B,
    ))
    dec.valid := !refilling && (d || (sel(idx) && !ctrl.br(idx).valid))
    dec.bits := Mux(d, decodedHolding(idx), decode.decoded)
  }
  for((d, h) <- decodedHoldingValid.zip(decodedHolding)) {
    h := Mux(d, h, decode.decoded)
  }
  fetchable := ~VecInit(decodedHoldingValid.zip(ctrl.br).map({ case (d, b) => d || b.valid })).asUInt & ~busy
  assert(!sel.orR || !ctrl.br(OHToUInt(sel)).valid)

  // Scheduling
  // step := icache.input.fire
  // icache.output.ready := decoded.ready

  // Refilling
  val (refillReqCnt, refillReqDone) = Counter(mem.req.fire, INSTR_MEM_SIZE / 4)
  val (refillRespCnt, refillRespDone) = Counter(mem.resp.valid, INSTR_MEM_SIZE / 4)
  val refillReq = RegInit(true.B)

  mem.req.valid := refillReq
  mem.req.bits.burst := 0.U
  mem.req.bits.addr := params.initVec.U + (refillReqCnt << 2)
  mem.req.bits.wbe := DontCare
  mem.req.bits.wdata := DontCare
  mem.req.bits.write := false.B

  assert(!mem.resp.valid || refilling)
  when(mem.resp.valid) {
    instrMem.write(refillRespCnt, mem.resp.bits.data)
  }

  when(refillReqDone) {
    refillReq := false.B
  }

  when(refillRespDone) {
    refilling := false.B
  }
}
