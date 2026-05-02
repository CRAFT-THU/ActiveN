package koneko.exec

import chisel3._
import chisel3.util._

import koneko._
import koneko.bus._

class Exec(implicit val param: CoreParameters) extends Module {
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
  val margins = RegInit(VecInit(Seq.fill(16)(0.U(log2Ceil(param.sendQueueDepth + 1).W))))
  val quotas = RegInit(VecInit(Seq.fill(16)(0.U(log2Ceil(param.sendQueueDepth + 1).W))))

  //////////////////////////
  // Actual configuration
  //////////////////////////

  val s0uops = dec.map(_.bits)
  val busyMap = RegInit(0.U(param.pipeCnt.W))
  val issuable = dec.map(_.valid).zip(busyMap.asBools).map({ case (v, b) => v && !b })
  val issueSel = PriorityEncoderOH(issuable)

  // Output consumed
  val s0step = Wire(Bool())

  // Set busyMap based on delay
  // Remember to check that issueSel != 0!
  val s0uop = Mux1H(issueSel, s0uops)
  val s0delayed = s0uop.isMul || s0uop.isFP
  // busymap always unsets (because we have max 2-cycle instrs)
  busyMap := Mux(s0delayed, VecInit(issueSel).asUInt, 0.U)

  for((d, i) <- dec.zip(issueSel)) d.ready := i && s0step

  val regfiles = for(i <- 0 until param.pipeCnt) yield Module(new RegFile(i))
  for((r, u) <- regfiles.zip(s0uops)) {
    r.read(0).num := u.rs1
    r.read(1).num := u.rs2
  }

  busy := busyMap

  // --- Stage ---
  // TODO: investigate about moving br forward one cycle

  val valid = RegEnable(VecInit(issueSel).asUInt.orR, false.B, s0step)
  val uop = RegEnable(Mux1H(issueSel, s0uops), s0step)
  val rs1val = RegEnable(Mux1H(issueSel, regfiles.map(_.read(0).value)), s0step)
  val rs2val = RegEnable(Mux1H(issueSel, regfiles.map(_.read(1).value)), s0step)
  val directval = RegEnable(Mux1H(issueSel, regfiles.map(r => VecInit(r.msgDirect.map(_.rdata)))), s0step)
  val delayed = RegEnable(s0delayed, s0step)

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

  // LSU
  val lsu = Module(new LSU)
  lsu.mem <> lsuMem

  lsu.req.bits.addr := Mux(uop.memIsAtomic, rs1val, added)
  lsu.req.bits.len.b := uop.funct3(1, 0) === 0.U
  lsu.req.bits.len.h := uop.funct3(1, 0) === 1.U
  lsu.req.bits.len.w := uop.funct3(1, 0) === 2.U
  lsu.req.bits.rsext := !uop.funct3(2)
  lsu.req.bits.wdata := rs2val
  lsu.req.bits.write := uop.memIsWrite
  lsu.req.bits.atomic := uop.memIsAtomic
  lsu.req.bits.funct5 := uop.funct7(6, 2) // AMO funct5 is funct7[6:2]
  lsu.req.valid := valid && uop.isMem

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

  val isYield = valid && uop.isAM && uop.funct3(0)
  val isWFI = valid && uop.isSystem && uop.funct3 === 0.U && uop.rs2 === 5.U
  // Effectively schedulable threads **RIGHT NOW**
  // failed nonblocking yield does not constitute a candidate
  val effectivelyYield = isYield && biuAccepted || isWFI
  val eidlings = idlings | (Fill(param.pipeCnt, s0step && (isYield && biuAccepted || isWFI)) & uop.smsel)
  val wakeup = PriorityEncoderOH(eidlings)
  assert(!effectivelyYield || s0step, "effectivelyYield -> s0step")
  assert(s0step || eidlings === idlings, "eidlings can only differ from idlings when s0step")

  // Branches caused by AM
  val biuBrs = Wire(Vec(param.pipeCnt, Valid(UInt(4.W))))
  // FIXME: handles blocking
  // We handle idlings by continously branching to an address
  // FIXME: nonlocal yield may become idle. Also need to branch. Also: WFI is the same
  for (i <- 0 until param.pipeCnt) {
    biuBrs(i).valid := eidlings(i)
    biuBrs(i).bits := biu.sched.bits.handler
  }

  // BIU sched is accepted when there is an effectively idling thread
  biu.sched.ready := eidlings.orR

  // Sending quota decrement
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
  idlings := eidlings & (~wakeup | Fill(param.pipeCnt, !biu.sched.valid))
  for (i <- 0 until param.pipeCnt) {
    val scheduled = biu.sched.valid && wakeup(i)
    for (j <- 0 until 4) {
      // Local sends skips writing registers, only handles remote sends here
      regfiles(i).msgDirect(j).wdata := biu.sched.bits.regs(j)
      regfiles(i).msgDirect(j).wen := scheduled && argcnts(biu.sched.bits.handler) > j.U
    }

    when(scheduled) {
      liveQuotas(i) := quotas(biu.sched.bits.handler)
    }
  }

  // CSR
  val csrmapping = Seq(
    0xF14 -> cfg.hartid,
  ) ++ handlers.zipWithIndex.map({
    case (h, i) => ((0x700 + i) -> h)
  }) ++ argcnts.zipWithIndex.map({
    case (h, i) => ((0x710 + i) -> h)
  }) ++ margins.zipWithIndex.map({
    case (h, i) => ((0x720 + i) -> h)
  }) ++ quotas.zipWithIndex.map({
    case (h, i) => ((0x730 + i) -> h)
  })
  val csrwmapping = csrmapping.filter({ e => (e._1 >> 10) != 3 })
  val isCSR = uop.isSystem && uop.funct3(1, 0) =/= 0.U
  val csrUimmExt = Wire(UInt(32.W))
  val csrIdx = uop.cimm(11, 0)
  csrUimmExt := uop.rs1
  val csrWraw = Mux(uop.funct3(2), csrUimmExt, rs1val)
  for((i, c) <- csrwmapping) {
    val csrWdata = Mux1H(Seq(
      (uop.funct3(1, 0) === 1.U) -> csrWraw,
      (uop.funct3(1, 0) === 2.U) -> (csrWraw | c),
      (uop.funct3(1, 0) === 3.U) -> (c & (~csrWraw).asUInt),
    ))
    when(valid && isCSR && i.U === csrIdx) {
      c := csrWdata
    }
  }
  val csrRdata = Mux1H(csrmapping.map({ case (i, c) => (i.U === csrIdx, c) }))

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
    uop.isMem -> lsu.resp,
    uop.isAM -> Mux(biuAccepted, 1.U, 0.U),
    uop.isSystem -> csrRdata, // Only CSR here
  )

  val delayedUop = RegNext(uop)
  val delayedIsMul = RegNext(uop.rdalu)
  val delayedIsFP = RegNext(uop.isFP)
  var delayedRdsrc = Seq(
    delayedIsMul -> mulval,
  )

  if(param.useFPU) {
    delayedRdsrc = Seq(
      delayedIsFP -> fpu.get.io.r,
    ) ++delayedRdsrc
  }

  val rdval = Mux1H(rdsrc)
  val delayedRdval = Mux1H(delayedRdsrc)

  val delayedSent = RegNext(delayed && valid)

  assert(!delayed || s0step) // Delayed -> s0step
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

  val s1done = MuxCase(true.B, s1donesrc)

  // Branching
  val brfire = Mux1H(Seq(
    (uop.funct3 === "b000".U) -> eq,
    (uop.funct3 === "b001".U) -> !eq,
    (uop.funct3 === "b100".U) -> lt,
    (uop.funct3 === "b101".U) -> !lt,
    (uop.funct3 === "b110".U) -> ltu,
    (uop.funct3 === "b111".U) -> !ltu,
  ))

  for(((br, biuBr), idx) <- brs.zip(biuBrs).zipWithIndex) {
    br.bits := Mux(biuBr.valid, handlers(biuBr.bits), (added >> 1) ## 0.U(1.W))
    br.valid := biuBr.valid || uop.smsel(idx) && (valid && ((uop.isBr && brfire) || uop.isJump))
  }

  // Scheduling
  s0step := !valid || s1done
}