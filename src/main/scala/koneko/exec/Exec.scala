package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class Exec extends Module {
  val dec = IO(Flipped(Decoupled(new uOp)))
  val br = IO(Output(Valid(UInt(32.W))))
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
  val aluval = Mux1H(Seq(
    (uop.funct3 === "b000".U) -> Mux(uop.funct7(5), subbed, added),
    (uop.funct3 === "b001".U) -> sll,
    (uop.funct3 === "b010".U) -> lt.asUInt,
    (uop.funct3 === "b011".U) -> ltu.asUInt,
    (uop.funct3 === "b100".U) -> xor,
    (uop.funct3 === "b101".U) -> Mux(uop.funct7(5), sra, srl),
    (uop.funct3 === "b110".U) -> or,
    (uop.funct3 === "b111".U) -> and,
  ))

  // PC + 4, used for JAL / JALR
  val pclink = uop.pc + 4.U

  /*
   * RD arbitration
   * - JAL[R]: pclink
   * - AUIPC, OP[-IMM]: aluval
   * - LUI: uop.imm
   */
  val rdval = Mux1H(Seq(
    uop.rdalu -> aluval,
    uop.rdpclink -> pclink,
    uop.rdimm -> uop.imm,
  ))
  regfile.write.en := valid && !uop.rdignore
  regfile.write.num := uop.rd
  regfile.write.value := rdval

  // TODO: multicycle
  val s1done = true.B

  // Branching
  br.bits := (added >> 1) ## 0.U(1.W)
  val brfire = Mux1H(Seq(
    (uop.funct3 === "b000".U) -> eq,
    (uop.funct3 === "b001".U) -> !eq,
    (uop.funct3 === "b100".U) -> lt,
    (uop.funct3 === "b101".U) -> !lt,
    (uop.funct3 === "b110".U) -> ltu,
    (uop.funct3 === "b111".U) -> !ltu,
  ))
  br.valid := valid && ((uop.isBr && brfire) || uop.isJump)

  // Scheduling
  s0step := !valid || s1done
  dec.ready := s0step
}