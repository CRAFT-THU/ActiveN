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

  // Latch last sent SMSel.
  val sentSmsel = RegInit(0.U(params.pipeCnt.W))
  // sentSmsel may be reset during killing, since that specific line may require line fill
  // so the killed ICache cannot accept new fetch at that specific cycle.
  val maskedSentSmsel = sentSmsel & ~VecInit(ctrl.br.map(_.valid)).asUInt
  sentSmsel := Mux(step, sel, maskedSentSmsel)

  // Fetch via ICache
  val icache = Module(new ICache)
  icache.mem <> mem
  icache.input.bits := selpc
  icache.input.valid := fetchable.orR
  icache.kill := VecInit(ctrl.br.zip(sentSmsel.asBools).map({ case (b, s) => b.valid && s })).asUInt.orR
  // We're using icache.input.ready as stepping signal
  // So even there is no active thread in the frontend, we can still process
  // fetched instructions.
  step := icache.input.ready

  // Decode
  val decode = Module(new Decode)
  decode.pc := RegEnable(selpc, step)
  decode.smsel := RegEnable(sel, step)
  decode.instr := icache.output

  val holding = for(_ <- 0 until params.pipeCnt) yield Reg(new uOp)
  val held = for(_ <- 0 until params.pipeCnt) yield RegInit(false.B)

  for(((d, dec), idx) <- held.zip(decoded).zipWithIndex) {
    d := MuxCase(d, Seq(
      ctrl.br(idx).valid -> false.B,
      dec.ready -> false.B,
      // dec is not ready, write into holding register
      (sentSmsel(idx) && step) -> true.B,
    ))
    dec.valid := d || (sentSmsel(idx) && step && !VecInit(ctrl.br.zip(sentSmsel.asBools).map({ case (b, s) => b.valid && s })).asUInt.orR)
    dec.bits := Mux(d, holding(idx), decode.decoded)
  }
  for((d, h) <- held.zip(holding)) {
    when(!d) {
      h := decode.decoded
    }
  }

  assert(!((sentSmsel & VecInit(held).asUInt).orR), "Thread that holds a instruction must not have sent a fetch")

  // Fetchable threads do not:
  // 1. branch at the same cycle
  // 2. it's holding register is being filled this step
  // 3. have a held instruction and it's not being vacated this cycle
  // Since if the thread already holds a instruction, the last cycle must not have
  // fetched from that thread
  // 2 + 3 = there is a pending fetched instruction that's not consumed by downstream
  fetchable := ~VecInit.tabulate(params.pipeCnt)(i =>
    ctrl.br(i).valid
    || ((sentSmsel(i) || held(i)) && !decoded(i).ready)
  ).asUInt
}
