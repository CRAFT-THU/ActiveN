package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

// LSU: routes accesses to SPM (local scratchpad) or global memory
class LSU(implicit val param: CoreParameters) extends Module {
  val req = IO(Flipped(Irrevocable(new Bundle {
    val addr = UInt(32.W)

    val len = new Bundle {
      val w = Bool()
      val h = Bool()
      val b = Bool()
    }

    val wdata = UInt(32.W)
    val write = Bool()
    val rsext = Bool()
  })))

  val resp = IO(Output(UInt(32.W)))

  val mem = IO(new Bundle {
    val req = Decoupled(new MemReq)
    val resp = Flipped(Valid(new MemResp))
  })

  val spm = Module(new SPM)

  val wmapped = Mux1H(Seq(
    req.bits.len.w -> req.bits.wdata,
    req.bits.len.h -> req.bits.wdata(15, 0) ## req.bits.wdata(15, 0),
    req.bits.len.b -> req.bits.wdata(7, 0) ## req.bits.wdata(7, 0) ## req.bits.wdata(7, 0) ## req.bits.wdata(7, 0),
  ))
  val wbe = Mux1H(Seq(
    req.bits.len.w -> 0xF.U(4.W),
    req.bits.len.h -> (0x3.U(4.W) << (req.bits.addr(1, 1) * 2 .U)),
    req.bits.len.b -> (0x1.U(4.W) << req.bits.addr(1, 0)),
  ))

  // Address space routing: SPM for low addresses, global memory otherwise
  val isSPM = req.bits.addr < param.scratchpadSize.U

  val alignedAddr = (req.bits.addr >> 2) ## 0.U(2.W)

  // --- SPM path (2-cycle reads, 1-cycle writes) ---
  val spmReadPending = RegInit(false.B)
  spmReadPending := req.valid && isSPM && !req.bits.write && !spmReadPending
  val spmReady = Mux(req.bits.write, true.B, spmReadPending)

  spm.io.addr := alignedAddr
  spm.io.we := Mux(req.fire && isSPM && req.bits.write, wbe, 0.U)
  spm.io.wdata := wmapped

  // --- Global memory path ---
  val memSent = RegInit(false.B)
  val memGotResp = RegInit(false.B)
  val memRdata = Reg(UInt(32.W))

  mem.req.valid := req.valid && !isSPM && !memSent
  mem.req.bits.addr := alignedAddr
  mem.req.bits.burst := 0.U
  mem.req.bits.wdata := wmapped
  mem.req.bits.wbe := Mux(req.bits.write, wbe, 0.U)
  mem.req.bits.write := req.bits.write

  val memReqFired = mem.req.fire
  when(memReqFired) { memSent := true.B }

  // Response may arrive on same cycle as the request (combinational path through crossbar/driver)
  val memRespCapture = mem.resp.valid && (memSent || memReqFired)
  when(memRespCapture) {
    memGotResp := true.B
    memRdata := mem.resp.bits.data
  }

  val globalDone = memGotResp || memRespCapture
  val globalRdata = Mux(memRespCapture && !memGotResp, mem.resp.bits.data, memRdata)

  when(req.fire && !isSPM) {
    memSent := false.B
    memGotResp := false.B
  }

  // --- Ready and response mux ---
  req.ready := Mux(isSPM, spmReady, globalDone)

  val rdata = Mux(isSPM, spm.io.data, globalRdata)

  val rhalf = rdata.asTypeOf(Vec(2, UInt(16.W)))(req.bits.addr(1, 1))
  val rbyte = rdata.asTypeOf(Vec(4, UInt(8.W)))(req.bits.addr(1, 0))
  val rmapped = Mux1H(Seq(
    req.bits.len.w -> rdata,
    req.bits.len.h -> VecInit(Seq.fill(16)(req.bits.rsext && rhalf(15))).asUInt ## rhalf,
    req.bits.len.b -> VecInit(Seq.fill(24)(req.bits.rsext && rbyte(7))).asUInt ## rbyte,
  ))
  resp := rmapped
}
