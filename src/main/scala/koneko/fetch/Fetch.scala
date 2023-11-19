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
    val br = Flipped(Valid(UInt(32.W)))
  })
  val decoded = IO(Decoupled(new uOp))

  // PC Control
  val npc = Wire(UInt(32.W))
  val step = WireDefault(false.B)

  val pc = RegEnable(npc, params.initVec.U(32.W), step)
  val fpc = Mux(ctrl.br.valid, ctrl.br.bits, pc)
  npc := fpc + 4.U

  // Fetch
  val icache = Module(new ICache)
  icache.mem <> mem
  icache.input.bits := fpc
  icache.input.valid := true.B
  step := icache.input.ready

  // Decode
  val sentFpc = RegEnable(fpc, step)
  val decode = Module(new Decode)
  decode.pc := sentFpc
  decode.instr := icache.output.bits
  decoded.valid := icache.output.valid

  decoded.bits := decode.decoded

  // Scheduling
  step := icache.input.fire
  icache.output.ready := decoded.ready
}