/// Encoder: converts MemReq/MemResp to/from event flit format for the NoC.
/// Requests are encoded as multi-flit events and sent through `out`.
/// Responses arrive on the wide `resp` bus and are forwarded to the crossbar.

package koneko.bus

import chisel3._
import chisel3.util._

import koneko._

class Encoder(implicit val param: CoreParameters) extends Module {
  // Memory-side port: connects to Crossbar downstream
  val mem = IO(new Bundle {
    val req = Flipped(Decoupled(new MemReq))
    val resp = Valid(new MemResp)
  })

  // Event output: arbitrated externally with BIU ext.out
  val out = IO(Decoupled(new Bundle {
    val dst = UInt(16.W)
    val data = UInt(32.W)
    val tag = UInt(16.W)
  }))

  // Response input: from Core.mem
  val resp = IO(Flipped(Valid(new MemResp)))

  // --- Response path: forward directly to crossbar ---
  mdm.resp := resp

  // --- Request encoding ---
  // Compute DRAM controller destination from address
  // memCtrlSizes defines the size of each controller.
  // Global memory starts at 0x80000000; address within global space
  // maps to controllers sequentially.
  val memCtrlSizes = param.memCtrlSizes
  val memDstBase = 0x8000 // Memory controllers: dst = 0x8000 + X

  // For hardware lookup: build cumulative boundaries
  // Controller i handles [cumSize(i), cumSize(i+1))
  val cumSizes = memCtrlSizes.scanLeft(BigInt(0))(_ + _) // [0, size0, size0+size1, ...]
  val numCtrls = memCtrlSizes.length

  val globalAddr = mdm.req.bits.addr - 0x80000000L.U
  val ctrlDst = Wire(UInt(16.W))
  ctrlDst := (memDstBase + numCtrls - 1).U(16.W) // default: last controller
  for (i <- 0 until numCtrls) {
    when(globalAddr < cumSizes(i + 1).U) {
      ctrlDst := (memDstBase + i).U(16.W)
    }
  }
  // Address within the controller's address space
  val ctrlLocalAddr = Wire(UInt(32.W))
  ctrlLocalAddr := globalAddr
  for (i <- 0 until numCtrls) {
    when(globalAddr < cumSizes(i + 1).U) {
      ctrlLocalAddr := globalAddr - cumSizes(i).U
    }
  }

  // FSM: encode MemReq as 2 or 3 flits
  val sIdle :: sSendAddr :: sSendMeta :: sSendWdata :: Nil = Enum(4)
  val state = RegInit(sIdle)

  // Latch request fields when accepted
  val reqAddr = Reg(UInt(32.W))
  val reqId = Reg(UInt(16.W))
  val reqSize = Reg(UInt(2.W))
  val reqWdata = Reg(UInt(32.W))
  val reqWrite = Reg(Bool())
  val reqDst = Reg(UInt(16.W))

  // Accept new request only when idle
  mdm.req.ready := state === sIdle

  when(mdm.req.fire) {
    reqAddr := ctrlLocalAddr
    reqId := mdm.req.bits.id
    reqSize := mdm.req.bits.size
    reqWdata := mdm.req.bits.wdata
    reqWrite := mdm.req.bits.write
    reqDst := ctrlDst
    state := sSendAddr
  }

  // Output flit
  out.valid := state =/= sIdle
  out.bits.dst := reqDst
  out.bits.tag := Mux(reqWrite, 0xFF01.U, 0xFF00.U)
  out.bits.data := MuxLookup(state, 0.U)(Seq(
    sSendAddr -> reqAddr,
    sSendMeta -> Mux(reqWrite, 0.U(14.W) ## reqSize ## reqId, 0.U(16.W) ## reqId),
    sSendWdata -> reqWdata,
  ))

  when(out.fire) {
    switch(state) {
      is(sSendAddr) { state := sSendMeta }
      is(sSendMeta) { state := Mux(reqWrite, sSendWdata, sIdle) }
      is(sSendWdata) { state := sIdle }
    }
  }
}
