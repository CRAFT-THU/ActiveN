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

  // Burst tracking state
  val idle = RegInit(true.B)
  val owner = Reg(UInt(log2Ceil(n).W))
  val remaining = Reg(UInt(8.W))

  // Arbiter selects among requesting upstream ports
  val arb = Module(new Arbiter(new MemReq, n))
  for ((a, u) <- arb.io.in.zip(upstream.map(_.req))) {
    a <> u
  }

  // Only allow new requests when idle
  arb.io.out.ready := idle && downstream.req.ready
  downstream.req.valid := idle && arb.io.out.valid
  downstream.req.bits := arb.io.out.bits

  // Handle request fire and response on the same cycle:
  // When a request fires, the response may already be valid on that same cycle
  // (e.g., the simulation driver responds immediately). We must route it correctly.
  val reqJustFired = downstream.req.fire
  val activeOwner = Mux(reqJustFired, arb.io.chosen, owner)
  val busActive = !idle || reqJustFired

  // Route responses to the owning upstream port
  for ((u, i) <- upstream.zipWithIndex) {
    u.resp.valid := downstream.resp.valid && busActive && (activeOwner === i.U)
    u.resp.bits := downstream.resp.bits
  }

  when(downstream.req.fire) {
    val totalBurst = (1.U << arb.io.out.bits.burst)
    owner := arb.io.chosen
    when(downstream.resp.valid) {
      // Response arrived on the same cycle as the request
      remaining := totalBurst - 1.U
      idle := totalBurst === 1.U
    } .otherwise {
      remaining := totalBurst
      idle := false.B
    }
  } .elsewhen(downstream.resp.valid && !idle) {
    remaining := remaining - 1.U
    when(remaining === 1.U) {
      idle := true.B
    }
  }
}
