package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class Exec(implicit val param: CoreParameters) extends Module {
  val dec = IO(Flipped(
      Vec(param.SMEP, Decoupled(new uOp))
  ))
  val busy = IO(Output(UInt(param.SMEP.W)))
  val brs = IO(Output(Vec(param.SMEP, Valid(UInt(32.W)))))
  val ext = IO(new Bundle {
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

    val idlings = Output(UInt(param.SMEP.W))
    val working = Output(Bool())
  })

  val cfg = IO(Input(new Bundle {
    val hartid = UInt(32.W)
  }))

  //////////////////////////
  // Cfg CSRS
  //////////////////////////

  val handlers = Reg(Vec(16, UInt(32.W)))
  val argcnts = Reg(Vec(16, UInt(5.W)))

  //////////////////////////
  // Actual configuration
  //////////////////////////

  val s0uops = dec.map(_.bits)
  val busyMap = RegInit(0.U(param.SMEP.W))
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

  val regfiles = for(i <- 0 until param.SMEP) yield Module(new RegFile(i))
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
  val delayed = RegEnable(s0delayed, s0step)

  ext.working := valid

  // rs1 + rs2, used for AUIPC, Load/Store addresses, and OP +
  val adder1val = Mux(uop.adder1pc, uop.pc, rs1val)
  val adder2val = Mux(uop.adder2imm, uop.imm, rs2val)
  val added = adder1val + adder2val

  // Rest of ALU
  val alu2 = Mux(uop.alu2imm, uop.imm, rs2val)
  val isMul = !uop.alu2imm && uop.funct7(1)

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
  lsu.hartid := cfg.hartid

  lsu.req.bits.addr := added
  lsu.req.bits.len.b := uop.funct3(1, 0) === 0.U
  lsu.req.bits.len.h := uop.funct3(1, 0) === 1.U
  lsu.req.bits.len.w := uop.funct3(1, 0) === 2.U
  lsu.req.bits.rsext := !uop.funct3(2)
  lsu.req.bits.wdata := rs2val
  lsu.req.bits.write := uop.memIsWrite
  lsu.req.valid := valid && uop.isMem

  // BIU, target at rs2
  val biu = Module(new BIU)
  for((b, (h, a)) <- biu.cfg.zip(handlers.zip(argcnts))) {
    b.handler := h
    b.argcnt := a
  }

  val isYield = dontTouch(uop.isAM && uop.funct7(0))
  val yieldTag = rs2val >> 16
  val yieldTarget = handlers(yieldTag.asUInt)
  val yieldAcked = biu.br.valid

  biu.ext.in <> ext.in
  biu.ext.out <> ext.out
  biu.msg.bits.reg(0) := rs1val
  biu.msg.bits.reg(1) := rs2val
  biu.msg.bits.enq := uop.funct3(1, 0)
  biu.msg.bits.send := uop.funct3(2)
  biu.msg.bits.target := rs2val
  biu.msg.bits.tag := rs2val >> 16
  biu.msg.valid := valid && uop.isAM && (!isYield || yieldAcked) // TODO: funct7 arbitration?
  assert(!(valid && uop.isAM && biu.msg.fire) || s0step)

  val isWFI = uop.isSystem && uop.funct3 === 0.U && uop.rs2 === 5.U
  val idlings = RegInit(0.U(param.SMEP.W))
  val idlingsMasked: UInt = idlings | Mux(valid && isWFI, uop.smsel, 0.U)

  biu.br.ready := s0step && (idlingsMasked.orR || valid && isYield)
  val biuSel = Mux(valid && isYield, uop.smsel, PriorityEncoderOH(idlingsMasked)) // Yielding guy takes precidence
  idlings := idlingsMasked & (~Mux(biu.br.fire, biuSel, 0.U)).asUInt // Yielding guy never transitions to idling
  ext.idlings := idlings

  val biuBrs = for(i <- 0 until param.SMEP) yield {
    val biuBr = Wire(Valid(UInt(32.W)))
    biuBr.valid := (idlingsMasked | Mux(valid && isYield, uop.smsel, 0.U))(i)
    biuBr.bits := MuxCase(param.initVec.U, Seq(
      biu.br.valid -> biu.br.bits.target,
      (valid && isYield) -> yieldTarget,
    ))
    for ((r, active) <- regfiles.zip(biuSel.asBools)) {
      r.bankedWrite.en := biu.br.fire && active
      r.bankedWrite.regs := biu.br.bits.regs
      r.bankedWrite.offset := 0.U // TODO: allow regs > 4
      r.bankedWrite.cnt := biu.br.bits.argcnt
    }
    biuBr
  }

  // CSR
  val csrmapping = Seq(
    0xF14 -> cfg.hartid,
  ) ++ handlers.zipWithIndex.map({
    case (h, i) => ((0x700 + i) -> h)
  }) ++ argcnts.zipWithIndex.map({
    case (h, i) => ((0x710 + i) -> h)
  })
  val csrwmapping = csrmapping.filter({ e => (e._1 >> 10) != 3 })
  val isCSR = uop.isSystem && uop.funct3(1, 0) =/= 0.U
  val csrUimmExt = Wire(UInt(32.W))
  val csrIdx = uop.imm(11, 0)
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
   * - AUIPC, OP[-IMM]: aluval
   * - LUI: uop.imm
   */
  val rdsrc = Seq(
    // uop.rdalu -> aluval,
    uop.rdalu -> ealuval,
    uop.rdpclink -> pclink,
    uop.rdimm -> (uop.imm >> 12) ## 0.U(12.W),
    uop.isMem -> lsu.resp,
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
    uop.isAM -> (!biu.msg.valid || biu.msg.ready),
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
    br.bits := Mux(biuBr.valid, biuBr.bits, (added >> 1) ## 0.U(1.W))
    br.valid := biuBr.valid || uop.smsel(idx) && (valid && ((uop.isBr && brfire) || uop.isJump))
  }

  // Scheduling
  s0step := !valid || s1done
}