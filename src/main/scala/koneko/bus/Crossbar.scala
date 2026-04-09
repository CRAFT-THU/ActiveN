package koneko.bus

import chisel3._
import chisel3.util._

import koneko._

class Crossbar(n: Int)(implicit val param: CoreParameters) extends Module {
  val upstream = IO(Vec(n, new Bundle {
    val req = Flipped(Decoupled(new MemReq))
    val resp = Valid(new MemResp)
  }))

  val downstream = IO(new Bundle {
    val req = Decoupled(new MemReq)
    val resp = Flipped(Valid(new MemResp))
  })

  require(n <= (1 << 15), "Too many upstream ports for ID encoding")
  private val portBits = log2Ceil(n max 2)

  // Arbiter selects among requesting upstream ports
  val arb = Module(new Arbiter(new MemReq, n))
  for ((a, u) <- arb.io.in.zip(upstream.map(_.req))) {
    a <> u
  }

  // Forward request downstream, encoding upstream port in high bits of ID
  arb.io.out.ready := downstream.req.ready
  downstream.req.valid := arb.io.out.valid
  downstream.req.bits := arb.io.out.bits
  downstream.req.bits.id := arb.io.chosen ## arb.io.out.bits.id(15 - portBits, 0)

  // Route responses based on upstream port encoded in tag high bits
  val respPort = downstream.resp.bits.id(15, 16 - portBits)
  val respId = 0.U(portBits.W) ## downstream.resp.bits.id(15 - portBits, 0)

  for ((u, i) <- upstream.zipWithIndex) {
    val selected = respPort === i.U
    u.resp.valid := downstream.resp.valid && selected
    u.resp.bits.id := respId
    u.resp.bits.data := downstream.resp.bits.data
  }
}
