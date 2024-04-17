package koneko;

import chisel3._
import chisel3.util._

import koneko.fetch._
import koneko.exec._

class Core(implicit val params: CoreParameters) extends Module {
  val mem = IO(new Bundle {
    val req = Decoupled(new MemReq)
    val resp = Flipped(Valid(new MemResp))
  })

  val ext = IO(new Bundle {
    val out = Decoupled(new Bundle {
      val dst = UInt(16.W)
      val data = UInt(32.W)
      val tag = UInt(16.W)
    })

    val in = Flipped(Decoupled(new Bundle {
      val src = UInt(16.W)
      val data = UInt(32.W)
      val tag = UInt(16.W)
    }))

    val idlings = Output(UInt(params.SMEP.W))
    val working = Output(Bool())
  })

  val cfg = IO(Input(new Bundle {
    val hartid = UInt(32.W)
  }))

  //////////////////////////
  // Cfg CSRS
  //////////////////////////

  val fetch = Module(new Fetch)
  val exec = Module(new Exec)

  exec.cfg <> cfg
  exec.ext <> ext
  fetch.mem <> mem
  fetch.decoded <> exec.dec
  fetch.busy <> exec.busy
  fetch.ctrl.br <> exec.brs
}