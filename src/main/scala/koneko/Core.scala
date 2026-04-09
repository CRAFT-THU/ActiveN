package koneko;

import chisel3._
import chisel3.util._
import chisel3.experimental.hierarchy.{instantiable, public}

import koneko.fetch._
import koneko.exec._
import koneko.bus._

@instantiable
class Core(implicit val params: CoreParameters) extends Module {
  @public val mem = IO(Flipped(Valid(new MemResp)))

  @public val ext = IO(new Bundle {
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

    val idlings = Output(UInt(params.pipeCnt.W))
    val working = Output(Bool())
  })

  @public val cfg = IO(Input(new Bundle {
    val hartid = UInt(32.W)
  }))

  //////////////////////////
  // Cfg CSRS
  //////////////////////////

  val fetch = Module(new Fetch)
  val exec = Module(new Exec)
  val crossbar = Module(new Crossbar(2))
  val encoder = Module(new Encoder)
  val extInQueue = Module(new Queue(ext.in.bits.cloneType, 4))
  val respQueue = Module(new Queue(new MemResp, 4))

  exec.cfg <> cfg

  // FIXME: deadlock proof
  extInQueue.io.enq <> ext.in
  exec.ext.in <> extInQueue.io.deq
  exec.ext.idlings <> ext.idlings
  exec.ext.working <> ext.working

  // Arbiter: Encoder out (memory requests) and BIU ext.out (AM messages)
  // BIU gets priority (port 0) since AM messages are typically latency-sensitive
  val extArb = Module(new Arbiter(ext.out.bits.cloneType, 2))
  extArb.io.in(0) <> exec.ext.out
  extArb.io.in(1) <> encoder.out
  ext.out <> extArb.io.out

  // Crossbar: upstream(0) = ICache, upstream(1) = LSU global memory
  crossbar.upstream(0) <> fetch.mem
  crossbar.upstream(1) <> exec.lsuMem
  crossbar.downstream <> encoder.mem

  // Memory response from external -> encoder -> crossbar
  // Broadcast responses (tag=0xFFFF from MemDistributor) go to BIU instead
  val isBroadcast = mem.valid && mem.bits.id === 0xFFFF.U
  respQueue.io.enq.valid := mem.valid && !isBroadcast
  respQueue.io.enq.bits := mem.bits
  assert(!respQueue.io.enq.valid || respQueue.io.enq.ready, "Core memory response queue overflow")
  encoder.resp := respQueue.io.deq
  respQueue.io.deq.ready := true.B
  exec.bcast.valid   := isBroadcast
  exec.bcast.data    := mem.bits.data

  fetch.decoded <> exec.dec
  fetch.busy := exec.busy | ext.idlings
  fetch.ctrl.br <> exec.brs
}