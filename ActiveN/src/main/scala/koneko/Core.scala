package koneko;

import chisel3._
import chisel3.util._
import chisel3.experimental.hierarchy.{instantiable, public}

import koneko.fetch._
import koneko.exec._
import koneko.bus._

@instantiable
class Core(implicit val params: CoreParameters) extends Module {
  @public val mem = IO(new Bundle {
    val unicast = Flipped(Valid(new MemResp))
    val broadcast = Flipped(Decoupled(new BcastLine))
  })

  @public val ext = IO(new Bundle {
    val out = Decoupled(new OutgoingFlit)
    val in = Flipped(Decoupled(new IncomingFlit))

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
  val msgQueue = Module(new FlitQueue(ext.in.bits.cloneType, params.msgQueueDepth, 4))

  exec.cfg <> cfg

  // FIXME: deadlock proof
  msgQueue.enq <> ext.in
  exec.ext.in <> msgQueue.deq
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
  crossbar.downstream.req <> encoder.req

  // Memory response from external -> encoder -> crossbar
  // Broadcast responses (tag=0xFFFF from MemDistributor) go to BIU instead
  crossbar.downstream.resp := mem.unicast
  exec.bcast               <> mem.broadcast

  fetch.decoded <> exec.dec
  fetch.busy := exec.busy
  fetch.ctrl.br <> exec.brs
}