package koneko.exec

import chisel3._
import chisel3.util._

import koneko._
import koneko.bus._

class Exec(implicit val param: CoreParameters) extends Module {
  // Decoded instructions
  // dec(i).valid ONLY represents that whether ICache or the holding register contain a instruction this cycle
  // it does not gate against branches.
  // Exec is expected to gate against branches itself.
  val dec = IO(Flipped(
      Vec(param.pipeCnt, Decoupled(new uOp))
  ))
  val busy = IO(Output(UInt(param.pipeCnt.W)))
  val brs = IO(Output(Vec(param.pipeCnt, Valid(UInt(32.W)))))
  val ext = IO(new Bundle {
    val out = Decoupled(new Bundle {
      val dst = UInt(16.W)
      val data = Vec(4, UInt(32.W))
      val tag = UInt(12.W)
    })

    val in = Flipped(Decoupled(new Bundle {
      val src = UInt(16.W)
      val data = Vec(4, UInt(32.W))
      val tag = UInt(12.W)
    }))

    val idlings = Output(UInt(param.pipeCnt.W))
    val working = Output(Bool())
  })

  val cfg = IO(Input(new Bundle {
    val hartid = UInt(32.W)
  }))

  val lsuMem = IO(new Bundle {
    val req = Decoupled(new MemReq)
    val resp = Flipped(Valid(new MemResp))
  })

  // CSR broadcast input from MemDistributor (via Core)
  val bcast = IO(Flipped(Decoupled(new BcastLine)))

  //////////////////////////
  // Cfg CSRS
  //////////////////////////

  val handlers = RegInit(VecInit(Seq.fill(16)(0.U(32.W))))
  val argcnts = RegInit(VecInit(Seq.fill(16)(0.U(3.W))))
  val enmasks = RegInit(VecInit(Seq.fill(16)(0.U(param.pipeCnt.W))))
  val margins = RegInit(VecInit(Seq.fill(16)(0.U(log2Ceil(param.sendQueueDepth + 1).W))))
  val quotas = RegInit(VecInit(Seq.fill(16)(0.U(log2Ceil(param.sendQueueDepth + 1).W))))
  require(quotas(0).getWidth <= 16) // Can fit two counters into a CSR

  //////////////////////////
  // Actual configuration
  //////////////////////////

  val s0uops = dec.map(_.bits)
  // TODO: remove isWFI is we can be sure that s1 always either count us in eidlings or branches
  // it currently does not hold because of CSR
  val issuable = for (i <- 0 until param.pipeCnt) yield dec(i).valid && !dec(i).bits.isWFI && !busy(i) && !brs(i).valid
  val issueSel = PriorityEncoderOH(issuable)

  // We know that whenever a branch happens, the next instruction must be a bubble for that thread
  for (i <- 0 until param.pipeCnt) assert(!RegNext(brs(i).valid) || !dec(i).valid, "Thread branched next cycle must not have a valid instruction")

  // Output consumed
  val s0step = Wire(Bool())

  val s0uop = Mux1H(issueSel, s0uops)
  val s0delayed = s0uop.isMul || s0uop.isFP

  for((d, i) <- dec.zip(issueSel)) d.ready := i && s0step

  val regfiles = for(i <- 0 until param.pipeCnt) yield Module(new RegFile(i))
  for((r, u) <- regfiles.zip(s0uops)) {
    r.read(0).num := u.rs1
    r.read(1).num := u.rs2
  }

  // --- Stage ---
  // TODO: investigate about moving br forward one cycle

  val valid = RegEnable(VecInit(issueSel).asUInt.orR, false.B, s0step)
  val uop = RegEnable(s0uop, s0step)
  val rs1val = RegEnable(Mux1H(issueSel, regfiles.map(_.read(0).value)), s0step)
  val rs2val = RegEnable(Mux1H(issueSel, regfiles.map(_.read(1).value)), s0step)
  val directval = RegEnable(Mux1H(issueSel, regfiles.map(r => VecInit(r.msgDirect.map(_.rdata)))), s0step)
  // We also ask s1 about instructions that's decided to be delayed during execution
  val dynDelayed = Wire(Bool())
  val delayed = RegEnable(s0delayed, s0step) || dynDelayed
  val s1done = Wire(Bool())

  ext.working := valid

  // rs1 + rs2, used for Branch addresses, Load/Store addresses, and OP +
  val adder1val = Mux(uop.adder1pc, uop.pc, rs1val)
  val adder2val = Mux(uop.adder2imm, uop.immExt, rs2val)
  val added = adder1val + adder2val

  // Rest of ALU
  val alu2 = Mux(uop.alu2imm, uop.immExt, rs2val)
  val isMul = !uop.alu2imm && uop.funct7(0)

  // Comparisions, used for branches and
  val lt = rs1val.asSInt < alu2.asSInt
  val ltu = rs1val < alu2
  val eq = rs1val === alu2

  // Sub
  val subbed = rs1val - alu2

  // Boolean
  val or = rs1val | alu2
  val xor = rs1val ^ alu2
  val and = rs1val & alu2

  // Shifts
  val sll = rs1val << alu2(4, 0)
  val srl = rs1val >> alu2(4, 0)
  val sra = (rs1val.asSInt >> alu2(4, 0)).asUInt

  // ALU result
  val ealuval = Mux1H(Seq(
    (uop.funct3 === "b000".U) -> Mux((!uop.alu2imm) && uop.funct7(5), subbed, added), // We don't have SUBI!
    (uop.funct3 === "b001".U) -> sll,
    (uop.funct3 === "b010".U) -> lt.asUInt,
    (uop.funct3 === "b011".U) -> ltu.asUInt,
    (uop.funct3 === "b100".U) -> xor,
    (uop.funct3 === "b101".U) -> Mux(uop.funct7(5), sra, srl),
    (uop.funct3 === "b110".U) -> or,
    (uop.funct3 === "b111".U) -> and,
  ))
  val mul = Module(new Mul)
  mul.input.rs1 := rs1val
  mul.input.rs2 := rs2val
  mul.input.rs1signed := uop.funct3(1, 0) =/= 3.U
  mul.input.rs2signed := !uop.funct3(1)
  mul.input.low := uop.funct3 === 0.U
  mul.input.high := uop.funct3(1, 0) =/= 0.U && !uop.funct3(2)
  mul.input.mid := uop.funct3(2)
  val mulval = mul.output
  // val aluval = Mux(isMul, mulval, ealuval)

  // FPU, remove if
  val fpu = if(param.useFPU) {
    val fpu = Module(new FPU)
    fpu.io.clock := clock
    fpu.io.reset := reset
    fpu.io.a := rs1val
    fpu.io.b := rs2val
    fpu.io.funct7 := uop.funct7
    fpu.io.funct3 := uop.funct3
    fpu.io.rs2b0 := uop.rs2(0)
    fpu.io.valid := valid && uop.isFP

    Some(fpu)
  } else None

  // Non-biu branches: used to gate against WFI
  val brfire = Mux1H(Seq(
    (uop.funct3 === "b000".U) -> eq,
    (uop.funct3 === "b001".U) -> !eq,
    (uop.funct3 === "b100".U) -> lt,
    (uop.funct3 === "b101".U) -> !lt,
    (uop.funct3 === "b110".U) -> ltu,
    (uop.funct3 === "b111".U) -> !ltu,
  ))
  val rvBrs = Wire(Vec(param.pipeCnt, new Valid(UInt(32.W))))
  for(i <- 0 until param.pipeCnt) {
    rvBrs(i).bits := (added >> 1) ## 0.U(1.W)
    rvBrs(i).valid := uop.smsel(i) && (valid && ((uop.isBr && brfire) || uop.isJump))
  }

  // LSU
  val lsu = Module(new LSU)
  lsu.mem <> lsuMem

  lsu.req.bits.addr := Mux(uop.memIsAtomic, rs1val, added)
  lsu.req.bits.len.b := uop.funct3(1, 0) === 0.U
  lsu.req.bits.len.h := uop.funct3(1, 0) === 1.U
  lsu.req.bits.len.w := uop.funct3(1, 0) === 2.U
  lsu.req.bits.rsext := !uop.funct3(2)
  lsu.req.bits.wdata := rs2val
  lsu.req.bits.write := uop.memIsWrite || uop.isSC
  lsu.req.bits.amo := uop.memIsAtomic && !uop.isLR && !uop.isSC
  lsu.req.bits.funct5 := uop.funct5
  lsu.req.bits.lrsc := uop.isLR || uop.isSC
  lsu.req.bits.smsel := uop.smsel
  lsu.req.valid := valid && uop.isMem

  val acGBase = RegInit(0.U(32.W))
  val acSBase = RegInit(0.U(32.W))
  val acLen = RegInit(0.U(32.W))
  lsu.ac.gBase := acGBase
  lsu.ac.sBase := acSBase
  lsu.ac.len := acLen
  // Default connection
  lsu.ac.start.valid := false.B
  lsu.ac.start.bits := DontCare

  // BIU & related AM connections
  val biu = Module(new BIU)
  biu.hartid := cfg.hartid(15, 0)

  biu.ext.in <> ext.in
  biu.ext.out <> ext.out
  biu.bcast <> bcast

  // Message port connections
  biu.msg.bits.dst := rs1val
  biu.msg.bits.data := directval
  biu.msg.bits.tag := alu2(11, 0) // Reuse alu2imm for decode
  val msgIsLocal = rs1val === 0.U
  biu.push.bits.handler := alu2(3, 0)
  biu.push.bits.regs := directval

  // FIXME: quota check. See targetQuotaSufficient below
  biu.msg.valid := valid && uop.isAM && !msgIsLocal
  biu.push.valid := valid && uop.isAM && msgIsLocal

  // We use fire here because there is a chance of insufficient quota
  // so valid is not necessarily true
  val biuAccepted = Mux(msgIsLocal, biu.push.fire, biu.msg.fire)

  // TODO: optimize local send + yield
  // If biu.sched does not need to be consumed by the current thread, i.e.
  // - biu.sched.valid === false.B
  // - There is already another idling thread
  // Then we can skip sending anything, just immediately complete this send,
  // jump to the handler without even writing registers.

  // TODO: what's this?
  // assert(!(valid && uop.isAM && biu.msg.fire) || s0step)

  // Idling management
  // Next cycle idlings
  val idlings = RegInit(0.U(param.pipeCnt.W))
  ext.idlings := idlings
  assert(!valid || ((idlings.asUInt & uop.smsel) === 0.U)) // Active instruction must come from active thread

  val liveQuotas = RegInit(VecInit(Seq.fill(param.pipeCnt)(0.U(log2Ceil(param.sendQueueDepth + 1).W))))
  val liveQuotaSum = liveQuotas.zip(idlings.asBools).map({ case (quota, idle) => Mux(idle, 0.U, quota) }).reduce(_ +& _)
  assert(liveQuotaSum <= param.sendQueueDepth.U)
  biu.liveQuota := liveQuotaSum

  biu.margins := margins

  // Assert that WFI never reaches exec stage
  assert(!valid || !uop.isWFI, "WFI should be handled in decode stage")
  val isYield = valid && uop.isAM && uop.funct3(0)
  // If the next instruction is fetched and is WFI
  //
  // Ignores:
  // 1. Ignore WFI that's being branched, excluding biuBrs to avoid combinatorial loop
  // 2. Ignore WFI if we're blocked
  // 3. Ignore WFI if we're a CSR instruction, because they can change BIU-related CSRs
  // TODO: later: CSR should be a hard branch, so this will no longer to depend on isCSR
  //
  // Additionally, we need to be careful around writebacks: RegFile should guarantee that
  // handler writes takes precedence over delayed writebacks
  val nextWFIs = VecInit(dec.zipWithIndex.map({ case (d, i) => (
    d.valid && d.bits.isWFI
      && !rvBrs(i).valid
      && !(!s0step && uop.smsel(i))
      && !(valid && isCSR && uop.smsel(i))
  )})).asUInt
  // Effectively schedulable threads **RIGHT NOW**
  // failed nonblocking yield does not constitute a candidate
  val eyield = isYield && biuAccepted
  val eidlings = idlings | (Fill(param.pipeCnt, s0step && eyield) & uop.smsel) | nextWFIs
  assert(!eyield || s0step, "effectivelyYield -> s0step")

  biu.enmasks := enmasks
  biu.accepting := eidlings
  // scheduled mask is a subset of accepting threads
  assert((biu.sched.wakeup & biu.accepting) === biu.sched.wakeup)
  // scheduled mask has at most 1 bit set
  assert(PopCount(biu.sched.wakeup) <= 1.U)

  // Branches caused by AM
  val biuBrs = Wire(Vec(param.pipeCnt, Valid(UInt(4.W))))
  // FIXME: handles blocking
  // We handle idlings by continously branching to an address
  // FIXME: nonlocal yield may become idle. Also need to branch. Also: WFI is the same
  for (i <- 0 until param.pipeCnt) {
    biuBrs(i).valid := biu.sched.wakeup(i)
    biuBrs(i).bits := biu.sched.handler
  }

  // Sending quota decrement
  // BE CAREFUL WHEN MOVING THIS BLOCK!
  // lievQuotas(i) might be updated at the same cycle
  // if we're yielding.
  // See the block below for setting live quotas for scheduled events
  val targetQuota = Mux1H(uop.smsel, liveQuotas)
  val targetQuotaSufficient = targetQuota > 0.U
  val targetQuotaUpdate = Mux(targetQuotaSufficient, targetQuota - 1.U, targetQuota)
  for(i <- 0 until param.pipeCnt) {
    when(valid && uop.isAM && uop.smsel(i) && biuAccepted) {
      liveQuotas(i) := targetQuotaUpdate
    }
  }

  // State transition during actual schedule: regfile, quotas, idlings
  // only clear wakeup thread when there is something to schedule
  //
  // Explicitly using the last-connected-wins semantics of Chisel
  //
  // We delay the writing of registers by one cycle, because it necessarily branched this cycle
  // And we want to be sure that the writes take effect after the instruction that may be in s1 right now
  // and is delayed
  idlings := eidlings & ~biu.sched.wakeup
  for (i <- 0 until param.pipeCnt) {
    val scheduled = biu.sched.wakeup(i)
    for (j <- 0 until 4) {
      // Local sends skips writing registers, only handles remote sends here
      regfiles(i).msgDirect(j).wdata := RegNext(biu.sched.regs(j))
      regfiles(i).msgDirect(j).wen := RegNext(scheduled && argcnts(biu.sched.handler) > j.U)
    }

    when(scheduled) {
      liveQuotas(i) := quotas(biu.sched.handler)
    }
  }

  sealed trait CSR {
    def read: UInt
    def write(data: UInt): Unit
  }

  // Automatically cast a UInt into a CSRWriter
  case class CSRUInt(data: UInt) extends CSR {
    def read = data
    def write(data: UInt) = this.data := data
  }
  case class CSRHandlerCfg(i: Int) extends CSR {
    def read = enmasks(i) ## argcnts(i)(2, 0)
    def write(data: UInt) = {
      val argcntNew = data(2, 0)
      argcnts(i) := Mux(argcntNew > 4.U, 4.U, argcntNew)
      enmasks(i) := data(3 + param.pipeCnt - 1, 3)
    }
  }
  case class CSRQuotaMargin(i: Int) extends CSR {
    def read = quotas(i) ## margins(i)
    def write(data: UInt) = {
      val quotaNew = data(31, 16)
      val marginNew = data(15, 0)
      quotas(i) := Mux(quotaNew > param.sendQueueDepth.U, param.sendQueueDepth.U, quotaNew)
      margins(i) := Mux(marginNew > param.sendQueueDepth.U, param.sendQueueDepth.U, marginNew)
    }
  }
  case class CSRGatedUInt(data: UInt, gate: Bool) extends CSR {
    def read = data
    def write(data: UInt) = when(gate) { this.data := data }
  }
  case class CSRAcCtrl(start: Valid[ACOps.Type], remaining: UInt) extends CSR {
    def read = remaining
    def write(data: UInt) = {
      start.valid := true.B
      // If we later introduced other AC ops, decode here
      start.bits := ACOps.Load
    }
  }

  // CSR
  val csrmapping: Seq[(Int, CSR)] = Seq(
    0xF14 -> CSRUInt(cfg.hartid),
    0x730 -> CSRAcCtrl(lsu.ac.start, lsu.ac.remaining),
    0x731 -> CSRGatedUInt(acGBase, lsu.ac.remaining === 0.U),
    0x732 -> CSRGatedUInt(acSBase, lsu.ac.remaining === 0.U),
    0x733 -> CSRGatedUInt(acLen, lsu.ac.remaining === 0.U),
  ) ++ (
    for (i <- 0 until 16) yield (0x700 + i) -> CSRUInt(handlers(i))
  ) ++ (
    for (i <- 0 until 16) yield (0x710 + i) -> CSRHandlerCfg(i)
  ) ++ (
    for (i <- 0 until 16) yield (0x720 + i) -> CSRQuotaMargin(i)
  )
  val csrwmapping = csrmapping.filter({ e => (e._1 >> 10) != 3 })
  val isCSR = uop.isSystem && uop.funct3(1, 0) =/= 0.U
  val csrUimmExt = Wire(UInt(32.W))
  val csrIdx = uop.cimm(11, 0)
  csrUimmExt := uop.rs1
  val csrWraw = Mux(uop.funct3(2), csrUimmExt, rs1val)
  // CSRR{S,C}[I]: immediate / rs1 = 0 skips write
  val csrSkipWrite = (uop.funct3(1, 0) === 2.U || uop.funct3(1, 0) === 3.U) && uop.rs1 === 0.U
  for((i, c) <- csrwmapping) {
    val csrWdata = Mux1H(Seq(
      (uop.funct3(1, 0) === 1.U) -> csrWraw,
      (uop.funct3(1, 0) === 2.U) -> (csrWraw | c.read),
      (uop.funct3(1, 0) === 3.U) -> (c.read & (~csrWraw).asUInt),
    ))
    when(valid && isCSR && i.U === csrIdx && !csrSkipWrite) {
      c.write(csrWdata)
    }
  }
  val csrRdata = Mux1H(csrmapping.map({ case (i, c) => (i.U === csrIdx, c.read) }))

  // PC + 4, used for JAL / JALR
  val pclink = uop.pc + 4.U

  /*
   * RD arbitration
   * - JAL[R]: pclink
   * - OP[-IMM]: aluval
   * - LUI: uop.cimm
   * - AUIPC: uop.cimm + pc
   */
  val rdsrc = Seq(
    uop.rdalu -> ealuval,
    uop.rdpclink -> pclink,
    uop.rdlui -> uop.immU,
    uop.rdauipc -> (uop.immU + uop.pc),
    uop.isMem -> Mux(uop.isSC, lsu.scFail, lsu.resp),
    uop.isAM -> Mux(biuAccepted, 1.U, 0.U),
    uop.isSystem -> csrRdata, // Only CSR here
  )

  // Delayed instruction writebacks
  val delayedUop = RegNext(uop)
  val delayedIsMul = RegNext(uop.rdalu)
  val delayedIsFP = RegNext(uop.isFP)
  val delayedIsLd = RegNext(uop.isMem && lsu.delayed)
  var delayedRdsrc = Seq(
    delayedIsMul -> mulval,
    delayedIsLd -> lsu.resp,
  )

  if(param.useFPU) {
    delayedRdsrc = Seq(
      delayedIsFP -> fpu.get.io.r,
    ) ++delayedRdsrc
  }

  val rdval = Mux1H(rdsrc)
  val delayedRdval = Mux1H(delayedRdsrc)

  val delayedSent = RegNext(delayed && valid && s1done)

  assert(!delayedSent || !delayedUop.rdignore) // Delayed sent -> uop have meaningful rd
  assert(!(delayedSent && valid) || delayedUop.smsel =/= uop.smsel) // When delayed sent, rd cannot be the same

  for((r, i) <- regfiles.zipWithIndex) {
    val matchDelayed = delayedSent && delayedUop.smsel(i)
    val matchCur = uop.smsel(i) && !delayed
    r.write.en := matchDelayed || (matchCur && valid && !uop.rdignore)
    r.write.num := Mux(matchDelayed, delayedUop.rd, uop.rd)
    r.write.value := Mux(matchDelayed, delayedRdval, rdval)
  }

  var s1donesrc = Seq(
    uop.isMem -> lsu.req.ready,
    uop.isAM -> (biuAccepted && !uop.funct3(1)),
  )

  s1done := MuxCase(true.B, s1donesrc)

  for(i <- 0 until param.pipeCnt) {
    brs(i).bits := Mux(biuBrs(i).valid, handlers(biuBrs(i).bits), rvBrs(i).bits)
    brs(i).valid := biuBrs(i).valid || rvBrs(i).valid
  }

  // Scheduling
  dynDelayed := uop.isMem && lsu.delayed
  s0step := !valid || s1done
  busy := eidlings | (Fill(param.pipeCnt, valid && (!s1done || delayed)) & uop.smsel)
}
