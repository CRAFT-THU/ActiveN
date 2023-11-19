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

  val fetch = Module(new Fetch)
  val exec = Module(new Exec)

  fetch.mem <> mem
  fetch.decoded <> exec.dec
  fetch.ctrl.br <> exec.br
}