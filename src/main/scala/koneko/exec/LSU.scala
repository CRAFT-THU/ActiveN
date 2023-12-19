package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

// Two cycle LSU
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
  val hartid = IO(Input(UInt(32.W)))

  /*
  val mem = IO(new Bundle {
    val req = Decoupled(new MemReq)
    val resp = Flipped(Valid(new MemResp))
  })
  */

  // val sent = Reg(Bool())
  // val valid = RegInit(false.B)

  // TODO: use SRAM
  class SPMElab extends Module {
    val io = IO(new Bundle {
      val hartid = Input(UInt(32.W))
      val addr = Input(UInt(32.W))
      val wdata = Input(UInt(32.W))
      val we = Input(UInt(4.W))

      val data = Output(UInt(32.W))
    })

    val scratchpad = Mem(param.scratchpadSize / 4, Vec(4, UInt(8.W)))
    io.data := scratchpad(io.addr).asUInt
    scratchpad.write(io.addr, io.wdata.asTypeOf(Vec(4, UInt(8.W))), io.we.asBools)
  }

  val spm = Module(new SPM)
  spm.io.clock := clock
  spm.io.hartid := hartid

  val wmapped = Mux1H(Seq(
    req.bits.len.w -> req.bits.wdata,
    req.bits.len.h -> req.bits.wdata(15, 0) ## req.bits.wdata(15, 0),
    req.bits.len.b -> req.bits.wdata(7, 0) ## req.bits.wdata(7, 0) ## req.bits.wdata(7, 0) ## req.bits.wdata(7, 0),
  ))
  val wbe = Mux1H(Seq(
    req.bits.len.w -> 0xF.U(4.W),
    req.bits.len.h -> (0x3.U(4.W) << (req.bits.addr(2, 1) * 16.U)),
    req.bits.len.b -> (0x1.U(4.W) << (req.bits.addr(2, 0) * 8.U)),
  ))

  /*
  mem.req.bits.addr := req.bits.addr
  mem.req.bits.burst := 0.U
  mem.req.bits.wdata := wmapped
  mem.req.bits.wbe := wbe
  mem.req.bits.write := req.bits.write
  mem.req.valid := valid && !sent

  sent := MuxCase(sent, Seq(
    req.fire -> false.B,
    mem.req.fire -> true.B,
  ))
  */

  req.ready := true.B

  // req.ready := mem.resp.valid
  // val rdata = mem.resp.bits

  val alignedAddr = (req.bits.addr >> 2) ## 0.U(2.W)
  // spm.io.clock := clock
  spm.io.addr := alignedAddr
  spm.io.we := Mux(req.fire && req.bits.write, wbe, 0.U)
  spm.io.wdata := wmapped

  // when(req.fire && req.bits.write) {
  //  scratchpad.write(alignedAddr, wmapped.asTypeOf(Vec(4, UInt(8.W))), wbe.asTypeOf(Vec(4, Bool())))
  //}
  // val rdata = scratchpad.read(alignedAddr).asUInt
  val rdata = spm.io.data

  val rhalf = rdata.asTypeOf(Vec(2, UInt(16.W)))(req.bits.addr(2, 1))
  val rbyte = rdata.asTypeOf(Vec(4, UInt(8.W)))(req.bits.addr(2, 0))
  val rmapped = Mux1H(Seq(
    req.bits.len.w -> rdata,
    req.bits.len.h -> VecInit(Seq.fill(16)(req.bits.rsext && rhalf(15))).asUInt ## rhalf,
    req.bits.len.b -> VecInit(Seq.fill(24)(req.bits.rsext && rbyte(7))).asUInt ## rbyte,
  ))
  resp := rmapped
}
