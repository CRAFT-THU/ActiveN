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

    val amo = Bool() // excluding LR/SC
    val funct5 = UInt(5.W) // for atomic operations

    val lrsc = Bool()
    val smsel = UInt(param.pipeCnt.W)
  })))

  // Asserts that write and AMO cannot both be asserted
  assert(!req.valid || !req.bits.write || !req.bits.amo, "AMO and write cannot both be true")
  // LR/SC only supports 32-bit
  assert(!req.valid || !req.bits.lrsc || req.bits.len.w)
  // Widthes are mutually exclusive
  assert(!req.valid || (req.bits.len.w.asUInt + req.bits.len.h.asUInt + req.bits.len.b.asUInt) === 1.U)

  val resp = IO(Output(UInt(32.W)))
  val scFail = IO(Output(Bool()))

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

  // Address space routing:
  //   0x20000000-0x3FFFFFFF: SPM (scratchpad)
  //   0x40000000+:           Exterior (peripheral or global memory)
  //   Below 0x20000000:      Invalid (assert)
  val isSPM = req.bits.addr >= 0x20000000.U && req.bits.addr < 0x40000000.U
  assert(!req.valid || req.bits.addr >= 0x20000000.U, "Access to address below 0x20000000")

  val spmAddr = req.bits.addr - 0x20000000.U
  val spmAlignedAddr = (spmAddr >> 2) ## 0.U(2.W)
  val alignedAddr = (req.bits.addr >> 2) ## 0.U(2.W)

  // Reserved (aligned) addresses & counter
  val spmReservedAddr = Reg(Vec(param.pipeCnt, UInt(32.W)))
  val spmReserved = RegInit(VecInit.fill(param.pipeCnt)(false.B))
  val reserved = (
    isSPM
    && Mux1H(req.bits.smsel, spmReservedAddr) === spmAlignedAddr
    && Mux1H(req.bits.smsel, spmReserved)
  )
  for (i <- 0 until param.pipeCnt) {
    val doingLR = req.valid && req.bits.smsel(i) && req.bits.lrsc && !req.bits.write
    val conflictingSC = spmReservedAddr(i) === spm.io.addr && spm.io.we.orR
    assert(!(doingLR && conflictingSC))
    // Transition:
    // Unset: someone stored on this exact address (no matter who)
    // Set: we LR on the line
    spmReserved(i) := spmReserved(i) && !conflictingSC || doingLR
    when(doingLR) {
      spmReservedAddr(i) := spmAlignedAddr
    }
  }
  scFail := !reserved
  assert(!req.valid || !isSPM || !req.bits.amo || req.ready, "SPM requests must finish within one cycle")

  // --- SPM path (2-cycle reads / AMO, 1-cycle writes) ---
  val spmamoalu = Module(new AMOALU32)
  spmamoalu.orig := spm.io.data
  spmamoalu.input := req.bits.wdata
  spmamoalu.funct5 := req.bits.funct5

  // SPM might be busy: new request, not write (AMO or read)
  //   RegNext(req.ready || !req.valid) is the "new request" condition.
  // Actual busy signal should also consider req.valid and isSPM
  // But for generating req.ready, this is sufficient
  val spmBusy = RegNext(req.ready || !req.valid) && !req.bits.write

  spm.io.addr := spmAlignedAddr
  spm.io.we := MuxCase(0.U(4.W), Seq(
    (req.valid && isSPM && req.bits.write && (!req.bits.lrsc || reserved)) -> wbe,
    RegNext(req.valid && !req.ready && isSPM && req.bits.amo) -> "b1111".U(4.W)
  ))
  // Note: AMO necessarily takes two cycles, so if req.bits.atomic causes timing hazard,
  // we can RegNext here.
  spm.io.wdata := Mux(req.bits.amo, spmamoalu.written, wmapped)

  // --- Global memory path ---
  val memSent = RegInit(false.B)
  val memGotResp = RegInit(false.B)
  val memRdata = Reg(UInt(32.W))

  mem.req.valid := req.valid && !isSPM && !memSent
  mem.req.bits.addr := req.bits.addr // full address (byte offset needed for sub-word stores)
  mem.req.bits.size := Mux1H(Seq(
    req.bits.len.b -> 0.U(2.W),
    req.bits.len.h -> 1.U(2.W),
    req.bits.len.w -> 2.U(2.W),
  ))
  mem.req.bits.id := 0.U
  mem.req.bits.wdata := wmapped
  mem.req.bits.wbe := Mux(req.bits.write, wbe, 0.U)
  mem.req.bits.write := req.bits.write

  val memReqFired = mem.req.fire
  when(memReqFired) { memSent := true.B }
  // Mem resp valid implies memSent && !memGotResp
  assert(!mem.resp.valid || (memSent && !memGotResp), "Unexpected LSU memory response")

  // Extract word from wide response based on address within the beat
  val wordsPerBeat = param.memBusWidth / 32
  val beatAlignBits = log2Ceil(param.memBusWidth / 8) // log2(32) = 5 for 256-bit
  val wordInBeat = alignedAddr(beatAlignBits - 1, 2) // word offset within the beat
  val respWords = Wire(Vec(wordsPerBeat, UInt(32.W)))
  for (i <- 0 until wordsPerBeat) {
    respWords(i) := mem.resp.bits.data((i + 1) * 32 - 1, i * 32)
  }
  val memRespWord = respWords(wordInBeat)

  // Response may arrive on same cycle as the request (combinational path through crossbar/driver)
  when(mem.resp.fire) {
    memGotResp := true.B
    memRdata := memRespWord
  }

  val globalDone = memGotResp || mem.resp.fire
  val globalRdata = Mux(mem.resp.fire && !memGotResp, memRespWord, memRdata)

  when(req.fire && !isSPM) {
    memSent := false.B
    memGotResp := false.B
  }

  // --- Ready and response mux ---
  req.ready := Mux(isSPM, !spmBusy, globalDone)

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
