package koneko.fetch

import chisel3._
import chisel3.util._
import chisel3.util.experimental.decode._

import koneko._

object InstrType extends Enumeration {
  val R, I, S, B, U, J = Value
}

object JumpType extends Enumeration {
  val Jump, Branch = Value
}

case class InstrPattern(
  val name: String,
  val opcode: String,
  val ty: InstrType.Value,
  val isOPImm: Option[Boolean] = None,
  val jumpType: Option[JumpType.Value] = None,
  val memIsWrite: Option[Boolean] = None,
) extends DecodePattern {
  override def bitPat: BitPat = BitPat("b" + opcode)
}

object InstrAdder1PC extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode Adder 1 is PC"
  override def genTable(op: InstrPattern): BitPat = (op.ty match {
    case InstrType.U | InstrType.J | InstrType.B => y
    case _ => n
  })
}

object InstrAdder2Imm extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode Adder 2 is Imm"
  override def genTable(op: InstrPattern): BitPat = (op.ty match {
    case InstrType.I | InstrType.U | InstrType.J | InstrType.B | InstrType.S => y
    case _ => n
  })
}

object InstrALU2Imm extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode ALU 2 is Imm"
  override def genTable(op: InstrPattern): BitPat = (op.isOPImm match {
    case Some(true) => y
    case Some(false) => n
    case None => dc
  })
}

object InstrRDALU extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode RD ALU"
  override def genTable(op: InstrPattern): BitPat = (op.name match {
    case "OP" | "OP-IMM" | "AUIPC" => y
    case _ => n
  })
}

object InstrRDImm extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode RD Imm"
  override def genTable(op: InstrPattern): BitPat = (op.name match {
    case "LUI" => y
    case _ => n
  })
}

object InstrRDPCLink extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode RD PCLink"
  override def genTable(op: InstrPattern): BitPat = (op.name match {
    case "JAL" | "JALR" => y
    case _ => n
  })
}

object InstrRDIgnore extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode RD Ignored"
  override def genTable(op: InstrPattern): BitPat = (op.ty match {
    case InstrType.S | InstrType.B => y
    case _ => n
  })
}

object InstrIsJump extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode is jump"
  override def genTable(op: InstrPattern): BitPat = (op.jumpType match {
    case Some(JumpType.Jump) => y
    case _ => n
  })
}

object InstrIsBranch extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode is br"
  override def genTable(op: InstrPattern): BitPat = (op.jumpType match {
    case Some(JumpType.Branch) => y
    case _ => n
  })
}

object InstrIsMem extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode is mem"
  override def genTable(op: InstrPattern): BitPat = (op.memIsWrite match {
    case Some(_) => y
    case _ => n
  })
}

object InstrIsSystem extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode is system"
  override def genTable(op: InstrPattern): BitPat = (op.name match {
    case "SYSTEM" => y
    case _ => n
  })
}

object InstrMemIsWrite extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode mem op is write"
  override def genTable(op: InstrPattern): BitPat = (op.memIsWrite match {
    case Some(true) => y
    case Some(false) => n
    case _ => dc
  })
}

object InstrIsAM extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode is AM"
  override def genTable(op: InstrPattern): BitPat = (op.name match {
    case "AM" => y
    case _ => n
  })
}

object InstrIsFP extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode is FP"
  override def genTable(op: InstrPattern): BitPat = (op.name match {
    case "OP-FP" => y
    case _ => n
  })
}

/*
 * Imm extraction tables:
 *
 * Low = [0, 4]
 * [5, 10] is consistent
 * 11
 * Mid = [12, 19] is consistent (Or extended)
 * High = [20, 31]
 */

object ImmLowFormat extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode ImmLow = [0, 4] Format (instr[20] = y, instr[7] = n)"
  override def genTable(op: InstrPattern): BitPat = (op.ty match {
    case InstrType.I | InstrType.J => y // J-type should ignore that 0-bit
    case InstrType.S | InstrType.B => n // B-type should ignore that 0-bit
    case InstrType.U | InstrType.R => dc // U and R doesn't care about low bits
  })
}

object Imm11Format extends DecodeField[InstrPattern, UInt] {
  override def name: String = "Decode Imm[11] Format (instr[31], instr[20], instr[7])"
  override def genTable(op: InstrPattern): BitPat = (op.ty match {
    case InstrType.I | InstrType.S => BitPat("b100")
    case InstrType.J => BitPat("b010")
    case InstrType.B => BitPat("b001")
    case InstrType.U | InstrType.R => BitPat("b???") // U and R doesn't care about low bits
  })
  override def chiselType: UInt = UInt(3.W)
}

object ImmMidFormat extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode ImmMid = [12, 19] Format (normal = y, extended instr[31] = n)"
  override def genTable(op: InstrPattern): BitPat = (op.ty match {
    case InstrType.J | InstrType.U => y
    case InstrType.B | InstrType.I | InstrType.S => n
    case InstrType.R => dc // R doesn't care about imm
  })
}

object ImmHighFormat extends BoolDecodeField[InstrPattern] {
  override def name: String = "Decode ImmHigh = [20, 31] Format (normal = y, extended instr[31] = n)"
  override def genTable(op: InstrPattern): BitPat = (op.ty match {
    case InstrType.U => y
    case InstrType.J | InstrType.B | InstrType.I | InstrType.S => n
    case InstrType.R => dc // R doesn't care about imm
  })
}

class Decode extends Module {
  val instr = IO(Input(UInt(32.W)))
  val pc = IO(Input(UInt(32.W)))
  val decoded = IO(Output(new uOp))

  // Static field extractions
  decoded.rd := instr(11, 7)
  decoded.rs1 := instr(19, 15)
  decoded.rs2 := instr(24, 20)
  decoded.funct7 := instr(31, 25)
  decoded.funct3 := instr(14, 12)
  decoded.pc := pc

  // Control signals
  // TODO: add CSR, add WFI
  val opcodes = Seq(
    InstrPattern("LUI", "01101", InstrType.U),
    InstrPattern("AUIPC", "00101",  InstrType.U),
    InstrPattern("JAL", "11011", InstrType.J, jumpType = Some(JumpType.Jump)),
    InstrPattern("JALR", "11001", InstrType.I, jumpType = Some(JumpType.Jump)),

    InstrPattern("BRANCH", "11000", InstrType.B, isOPImm = Some(false), jumpType = Some(JumpType.Branch)),
    InstrPattern("LOAD", "00000", InstrType.I, memIsWrite = Some(false)),
    InstrPattern("STORE", "01000", InstrType.S, memIsWrite = Some(true)),
    InstrPattern("OP-IMM", "00100", InstrType.I, isOPImm = Some(true)),
    InstrPattern("OP", "01100", InstrType.R, isOPImm = Some(false)),
    InstrPattern("AM", "00010", InstrType.R, isOPImm = Some(false)), // Custom-0
    InstrPattern("SYSTEM", "11100", InstrType.I), // Doesn't use adder result
    InstrPattern("OP-FP", "10100", InstrType.R),
  )

  val dectraits = Seq(
    InstrAdder1PC, InstrAdder2Imm, InstrALU2Imm,
    InstrRDALU, InstrRDImm, InstrRDPCLink, InstrRDIgnore,
    InstrIsJump, InstrIsBranch,
    InstrIsMem, InstrMemIsWrite,
    InstrIsSystem,
    InstrIsAM,
    InstrIsFP,
    ImmLowFormat, Imm11Format, ImmMidFormat, ImmHighFormat
  )
  val dectbl = new DecodeTable(opcodes, dectraits);
  val decout = dectbl.decode(instr(6, 2))

  // TODO: illegal instr if instr(0, 1) != 0x3 (C instructions)
  decoded.adder1pc := decout(InstrAdder1PC)
  decoded.adder2imm := decout(InstrAdder2Imm)
  decoded.alu2imm := decout(InstrALU2Imm)
  decoded.rdignore := decout(InstrRDIgnore)
  decoded.rdalu := decout(InstrRDALU)
  decoded.rdpclink := decout(InstrRDPCLink)
  decoded.rdimm := decout(InstrRDImm)
  decoded.isJump := decout(InstrIsJump)
  decoded.isBr := decout(InstrIsBranch)
  decoded.isMem := decout(InstrIsMem)
  decoded.memIsWrite := decout(InstrMemIsWrite)
  decoded.isAM := decout(InstrIsAM)
  decoded.isSystem := decout(InstrIsSystem)
  decoded.isFP := decout(InstrIsFP)

  // Imm extraction
  val imm_0_4 = Mux(decout(ImmLowFormat), instr(24, 20), instr(11, 7))
  val imm_5_10 = instr(30, 25)
  val imm_11 = Mux1H(decout(Imm11Format).asBools.zip(Seq(
    instr(7),
    instr(20),
    instr(31),
  )))
  val imm_12_19 = Mux(decout(ImmMidFormat), instr(19, 12), VecInit(Seq.fill(8)(instr(31))).asUInt)
  val imm_20_31 = Mux(decout(ImmHighFormat), instr(31, 20), VecInit(Seq.fill(12)(instr(31))).asUInt)

  decoded.imm := imm_20_31 ## imm_12_19 ## imm_11 ## imm_5_10 ## imm_0_4
}