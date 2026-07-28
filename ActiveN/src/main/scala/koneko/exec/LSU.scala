package koneko.exec

import chisel3._
import chisel3.util._

import koneko._
import koneko.bus.OutgoingFlit
import koneko.bus.Encoder

// Async copy operation
object ACOps extends ChiselEnum {
  val Load = Value
}

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

  // Async-copy to/from global memory configurations
  val ac = IO(new Bundle {
    val gBase = Input(UInt(32.W))
    val sBase = Input(UInt(32.W))
    val len = Input(UInt(32.W))
    val start = Flipped(Valid(ACOps()))
    val remaining = Output(UInt(16.W))
  })

  // Asserts that write and AMO cannot both be asserted
  assert(!req.valid || !req.bits.write || !req.bits.amo, "AMO and write cannot both be true")
  // LR/SC only supports 32-bit
  assert(!req.valid || !req.bits.lrsc || req.bits.len.w)
  // Widths are mutually exclusive
  assert(!req.valid || (req.bits.len.w.asUInt + req.bits.len.h.asUInt + req.bits.len.b.asUInt) === 1.U)

  val resp = IO(Output(UInt(32.W)))
  val scFail = IO(Output(Bool()))

  val mem = IO(new Bundle {
    val req = Decoupled(new MemReq)
    val resp = Flipped(Valid(new MemResp))
  })

  val spm = Module(new SPM)

  /* Response validness */
  val respAc = mem.resp.valid && mem.resp.bits.id === 1.U
  val respG = mem.resp.valid && mem.resp.bits.id === 0.U

  /* Async-copy state machine */
  val acOp = Reg(ACOps()) // This is unused for now.
  val acPending = RegInit(false.B) // Whether the async-copy request is not sent yet
  val acRemaining = RegInit(0.U(16.W))

  ac.remaining := acRemaining

  // TODO: avoid accessing peripheral with a bulk load
  // TODO: fail on >= 2^16 lines
  val acmem = Wire(Decoupled(new MemReq))
  acmem.bits.id := 1.U
  acmem.bits.addr := ac.gBase
  acmem.bits.bulk := true.B
  acmem.bits.bulkSize := ac.len >> log2Ceil(param.memBusWidth / 8)
  acmem.bits.write := false.B
  acmem.bits.wdata := DontCare
  acmem.bits.wbe := DontCare
  acmem.bits.size := DontCare // Not used in bulk requests
  acmem.valid := acPending
  val acNew = ac.start.valid && acRemaining === 0.U // Ignore new request if there is an ongoing one
  when (acNew) {
    acOp := ac.start.bits
  }
  acPending := MuxCase(acPending, Seq(
    acNew -> true.B,
    acmem.fire -> false.B,
  ))
  acRemaining := MuxCase(acRemaining, Seq(
    acNew -> (ac.len >> log2Ceil(param.memBusWidth / 8)),
    respAc -> (acRemaining - 1.U)
  ))

  val wmapped = Mux1H(Seq(
    req.bits.len.w -> req.bits.wdata,
    req.bits.len.h -> Fill(2, req.bits.wdata(15, 0)),
    req.bits.len.b -> Fill(4, req.bits.wdata(7, 0)),
  ))
  val wbe = Mux1H(Seq(
    req.bits.len.w -> 0xF.U(4.W),
    req.bits.len.h -> (0x3.U(4.W) << (req.bits.addr(1, 1) * 2 .U)),
    req.bits.len.b -> (0x1.U(4.W) << req.bits.addr(1, 0)),
  ))


  val alignedAddr = (req.bits.addr >> 2) ## 0.U(2.W)

  // Extract word from wide response based on address within the beat
  val wordsPerBeat = param.memBusWidth / 32
  val beatAlignBits = log2Ceil(param.memBusWidth / 8)
  val wordInBeat = alignedAddr(beatAlignBits - 1, 2) // word offset within the beat
  def takeWord(resp: UInt): UInt = {
    val respWords = Wire(Vec(wordsPerBeat, UInt(32.W)))
    for (i <- 0 until wordsPerBeat) {
      respWords(i) := resp((i + 1) * 32 - 1, i * 32)
    }
    respWords(wordInBeat)
  }
  val gword = takeWord(mem.resp.bits.data)
  val sword = takeWord(spm.io.data)

  // Address space routing:
  //   0x20000000-0x3FFFFFFF: SPM (scratchpad)
  //   0x40000000+:           Exterior (peripheral or global memory)
  //   Below 0x20000000:      Invalid (assert)
  val isSPM = req.bits.addr >= 0x20000000.U && req.bits.addr < 0x40000000.U
  assert(!req.valid || req.bits.addr >= 0x20000000.U, "Access to address below 0x20000000")

  // TODO: detect misaligned access

  val OFFSET_WIDTH = log2Ceil(param.memBusWidth / 8)
  val spmAddr = req.bits.addr - 0x20000000.U
  val spmAlignedAddr = (spmAddr >> OFFSET_WIDTH) ## 0.U(OFFSET_WIDTH.W)
  val spmWordOffset = spmAddr(OFFSET_WIDTH - 1, 2) ## 0.U(2.W)

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
  // assert(!req.valid || !isSPM || !req.bits.amo || req.ready, "SPM requests must finish within one cycle")

  // --- SPM path (2-cycle reads / AMO, 1-cycle writes) ---
  val spmamoalu = Module(new AMOALU32)
  spmamoalu.orig := sword
  spmamoalu.input := req.bits.wdata
  spmamoalu.funct5 := req.bits.funct5

  // SPM might be busy, in the following conditions:
  // 1. Bulk load response takes precedence if this is a write, a new load, or an AMO
  // 2. A new request, not write (AMO or read), needs a response, so is busy this cycle
  //   We also take into account the fact that the request may not be sent last cycle
  //   RegNext(req.ready || !req.valid || spmBulkWrite) is the "new request or blocked" condition.
  //
  // This scheme should guarantee a retry of AMO if the second cycle is blocked by a bulk load.
  // A read will not be retried because we did not block its second cycle
  //
  // Actual busy signal should also consider req.valid and isSPM
  // But for generating req.ready, this is sufficient
  //
  // The semantics of LRSC + async copy is UB for now.
  // TODO: fix this
  //
  // It can also be busy if there is a bulk load response hitting us
  val spmBusy = (
    RegNext(req.ready || !req.valid || respAc) && !req.bits.write
    || respAc && (req.bits.write || req.bits.amo)
  )

  val spmShortWe = MuxCase(0.U(4.W), Seq(
    (req.valid && isSPM && req.bits.write && (!req.bits.lrsc || reserved)) -> wbe,
    RegNext(req.valid && !req.ready && !respAc && isSPM && req.bits.amo) -> "b1111".U(4.W)
  ))
  val spmShortWdata = Mux(req.bits.amo, spmamoalu.written, wmapped)
  val spmBulkWriteAddr = (mem.resp.bits.ident << log2Ceil(param.memBusWidth / 8)) + (ac.sBase - 0x20000000.U)
  spm.io.addr := Mux(respAc, spmBulkWriteAddr, spmAlignedAddr)
  spm.io.we := Mux(respAc, Fill(param.memBusWidth / 8, 1.U(1.W)), spmShortWe << spmWordOffset)
  // Note: AMO necessarily takes two cycles, so if req.bits.atomic causes timing hazard,
  // we can RegNext here.
  spm.io.wdata := Mux(respAc, mem.resp.bits.data, Fill(param.memBusWidth / 32, spmShortWdata))

  // --- Global memory path ---
  val memSent = RegInit(false.B)

  val gmem = Wire(Decoupled(new MemReq))
  gmem.valid := req.valid && !isSPM && !memSent
  gmem.bits.addr := req.bits.addr // full address (byte offset needed for sub-word stores)
  gmem.bits.size := Mux1H(Seq(
    req.bits.len.b -> 0.U(2.W),
    req.bits.len.h -> 1.U(2.W),
    req.bits.len.w -> 2.U(2.W),
  ))
  gmem.bits.id := 0.U
  gmem.bits.wdata := wmapped
  gmem.bits.wbe := Mux(req.bits.write, wbe, 0.U)
  gmem.bits.write := req.bits.write
  gmem.bits.bulkSize := DontCare
  gmem.bits.bulk := false.B

  memSent := MuxCase(memSent, Seq(
    gmem.fire -> true.B,
    (req.fire && !isSPM) -> false.B
  ))
  // Mem resp valid implies memSent
  // Response now never arrives on same cycle as the request, because we have a 1 cycle buffer in the Distributor unicast pathway
  assert(!respG || memSent, "Unexpected LSU memory response")

  val reqArb = Module(new Arbiter(new MemReq, 2))
  reqArb.io.in(0) <> gmem
  reqArb.io.in(1) <> acmem
  mem.req <> reqArb.io.out

  // --- Ready and response mux ---
  req.ready := Mux(isSPM, !spmBusy, respG)

  val rdata = Mux(isSPM, sword, gword)

  val rhalf = rdata.asTypeOf(Vec(2, UInt(16.W)))(req.bits.addr(1, 1))
  val rbyte = rdata.asTypeOf(Vec(4, UInt(8.W)))(req.bits.addr(1, 0))
  val rmapped = Mux1H(Seq(
    req.bits.len.w -> rdata,
    req.bits.len.h -> VecInit(Seq.fill(16)(req.bits.rsext && rhalf(15))).asUInt ## rhalf,
    req.bits.len.b -> VecInit(Seq.fill(24)(req.bits.rsext && rbyte(7))).asUInt ## rbyte,
  ))
  resp := rmapped
}
