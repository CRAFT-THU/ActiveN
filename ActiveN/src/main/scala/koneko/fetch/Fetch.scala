package koneko.fetch

import chisel3._
import chisel3.util._

import koneko._

class Fetch(implicit val params: CoreParameters) extends Module {
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

  // Latch
  val sentSmsel = RegEnable(sel, step)

  // Fetch via ICache
  val icache = Module(new ICache)
  icache.mem <> mem
  icache.input.bits := selpc
  icache.input.valid := fetchable.orR
  icache.kill := VecInit(ctrl.br.zip(sentSmsel.asBools).map({ case (b, s) => b.valid && s })).asUInt.orR
  step := icache.input.fire

  // Decode
  val decode = Module(new Decode)
  decode.pc := RegEnable(selpc, step)
  decode.smsel := RegEnable(sel, step)
  decode.instr := icache.output.bits

  val decodedHolding = for(_ <- 0 until params.pipeCnt) yield Reg(new uOp)
  val decodedHoldingValid = for(_ <- 0 until params.pipeCnt) yield RegInit(false.B)
  val icacheOutputConsumed = Wire(Bool())
  icacheOutputConsumed := false.B

  // Track when the ICache output gets captured into the holding register
  // so we can acknowledge it and let the ICache advance
  val holdingCapture = Wire(Bool())
  holdingCapture := false.B

  for(((d, dec), idx) <- decodedHoldingValid.zip(decoded).zipWithIndex) {
    d := MuxCase(d, Seq(
      ctrl.br(idx).valid -> false.B,
      dec.ready -> false.B,
      // dec is not ready
      (sentSmsel(idx) && icache.output.valid) -> true.B,
    ))
    dec.valid := d || (sentSmsel(idx) && icache.output.valid && !VecInit(ctrl.br.zip(sentSmsel.asBools).map({ case (b, s) => b.valid && s })).asUInt.orR)
    dec.bits := Mux(d, decodedHolding(idx), decode.decoded)
    when(dec.valid && dec.ready && sentSmsel(idx) && !d) {
      icacheOutputConsumed := true.B
    }
    // Detect when we're capturing ICache output into holding register
    when(!d && !dec.ready && sentSmsel(idx) && icache.output.valid) {
      holdingCapture := true.B
    }
  }
  for((d, h) <- decodedHoldingValid.zip(decodedHolding)) {
    h := Mux(d, h, decode.decoded)
  }
  fetchable := ~VecInit(decodedHoldingValid.zip(ctrl.br).map({ case (d, b) => d || b.valid })).asUInt & ~busy

  icache.output.ready := icacheOutputConsumed || holdingCapture || VecInit(ctrl.br.zip(sentSmsel.asBools).map({ case (b, s) => b.valid && s })).asUInt.orR
}
