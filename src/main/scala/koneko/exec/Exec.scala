package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class Exec(implicit val param: CoreParameters) extends Module {
  val dec = IO(Flipped(Decoupled(new uOp)))
  val br = IO(Output(Valid(UInt(32.W))))
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

  val s0uop = dec.bits

  // Output consumed
  val s0step = Wire(Bool())

  val regfile = Module(new RegFile)
  regfile.read(0).num := s0uop.rs1
  regfile.read(1).num := s0uop.rs2

  // --- Stage ---
  // TODO: investigate about moving br forward one cycle

  val valid = RegEnable(dec.valid, false.B, s0step)
  val uop = RegEnable(s0uop, s0step)
  val rs1val = RegEnable(regfile.read(0).value, s0step)
  val rs2val = RegEnable(regfile.read(1).value, s0step)

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
  val sll = rs1val >> alu2(4, 0)
  val srl = rs1val << alu2(4, 0)
  val sra = (rs1val.asSInt << alu2(4, 0)).asUInt

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
  mul.input.bits.rs1 := rs1val
  mul.input.bits.rs2 := rs2val
  mul.input.valid := isMul
  mul.input.bits.rs1signed := uop.funct3(1, 0) =/= 3.U
  mul.input.bits.rs2signed := !uop.funct3(1)
  mul.input.bits.low := uop.funct3 === 0.U
  mul.input.bits.high := uop.funct3(1, 0) =/= 0.U && !uop.funct3(2)
  mul.input.bits.mid := uop.funct3(2)
  val mulval = mul.output
  val aluval = Mux(isMul, mulval, ealuval)

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

  biu.ext <> ext
  biu.msg.bits.reg(0) := rs1val
  biu.msg.bits.reg(1) := rs2val
  biu.msg.bits.enq := uop.funct3(1, 0)
  biu.msg.bits.send := uop.funct3(2)
  biu.msg.bits.target := rs2val
  biu.msg.bits.tag := rs2val >> 16
  biu.msg.valid := valid && uop.isAM // TODO: funct7 arbitration?

  val idling = RegInit(false.B)
  val isWFI = dontTouch(uop.isSystem && uop.funct3 === 0.U && uop.rs2 === 5.U)
  idling := MuxCase(idling, Seq(
    biu.br.fire -> false.B,
    (valid && isWFI) -> true.B,
  ))
  biu.br.ready := idling || (valid && isWFI)
  val biuBr = Wire(Valid(UInt(32.W)))
  biuBr.valid := idling || (valid && isWFI)
  biuBr.bits := biu.br.bits.target // If idling, Spin at some random address
  regfile.bankedWrite.en := biu.br.fire
  regfile.bankedWrite.regs := biu.br.bits.regs
  regfile.bankedWrite.offset := 0.U // TODO: allow regs > 4
  regfile.bankedWrite.cnt := biu.br.bits.argcnt

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
  var rdsrc = Seq(
    uop.rdalu -> aluval,
    uop.rdpclink -> pclink,
    uop.rdimm -> (uop.imm >> 12) ## 0.U(12.W),
    uop.isMem -> lsu.resp,
    uop.isSystem -> csrRdata, // Only CSR here
  )

  if(param.useFPU) {
    rdsrc = Seq(
      uop.isFP -> fpu.get.io.r,
    ) ++ rdsrc
  }

  val rdval = Mux1H(rdsrc)
  regfile.write.en := valid && !uop.rdignore
  regfile.write.num := uop.rd
  regfile.write.value := rdval

  var s1donesrc = Seq(
    uop.isMem -> lsu.req.ready,
    uop.isAM -> biu.msg.ready,
    (uop.rdalu && isMul) -> mul.input.ready,
  )
  if(param.useFPU) {
    s1donesrc = Seq(
      uop.isFP -> fpu.get.io.ready,
    ) ++ s1donesrc
  }

  val s1done = MuxCase(true.B, s1donesrc)

    // Branching
  br.bits := Mux(biuBr.valid, biuBr.bits, (added >> 1) ## 0.U(1.W))
  val brfire = Mux1H(Seq(
    (uop.funct3 === "b000".U) -> eq,
    (uop.funct3 === "b001".U) -> !eq,
    (uop.funct3 === "b100".U) -> lt,
    (uop.funct3 === "b101".U) -> !lt,
    (uop.funct3 === "b110".U) -> ltu,
    (uop.funct3 === "b111".U) -> !ltu,
  ))
  br.valid := biuBr.valid || (valid && ((uop.isBr && brfire) || uop.isJump))

  // Scheduling
  s0step := !valid || s1done
  dec.ready := s0step
}