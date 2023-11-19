package koneko;

import chisel3._

class MemReq extends Bundle {
  val addr = UInt(32.W)
  val burst = UInt(3.W) // 1, 2, 4, 8, 16, 32, 64, 128
  val wdata = UInt(32.W)
  val wbe = UInt(4.W)
}

class MemResp extends Bundle {
  val data = UInt(32.W)
}

class uOp extends Bundle {
  val adder1pc = Bool()
  val adder2imm = Bool()
  val alu2imm = Bool()
  val rdalu = Bool()
  val rdpclink = Bool()
  val rdimm = Bool()
  val rdignore = Bool()

  val rs1 = UInt(5.W)
  val rs2 = UInt(5.W)
  val rd = UInt(5.W)

  val pc = UInt(32.W)
  val imm = UInt(32.W)

  // Actually embedded inside imm
  val funct7 = UInt(7.W)
  val funct3 = UInt(3.W)
}