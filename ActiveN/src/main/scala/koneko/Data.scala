package koneko;

import chisel3._

class MemReq extends Bundle {
  val addr = UInt(32.W)
  val size = UInt(4.W)
  val id = UInt(16.W)
  val wdata = UInt(32.W)
  val wbe = UInt(4.W)
  val write = Bool()

  val bulk = Bool()
  val bulkSize = UInt(16.W) // In memBusWidth
}

class MemResp(implicit val param: CoreParameters) extends Bundle {
  val id = UInt(16.W)
  val ident = UInt(16.W) // Used in bulk transfer to identify the request
  val data = UInt(param.memBusWidth.W)
}

class uOp(implicit val param: CoreParameters) extends Bundle {
  val adder1pc = Bool()
  val adder2imm = Bool()
  val alu2imm = Bool()
  val rdalu = Bool()
  val rdpclink = Bool()
  val rdlui = Bool()
  val rdauipc = Bool()
  val rdignore = Bool()

  val isJump = Bool()
  val isBr = Bool()

  val isMem = Bool()
  val memIsWrite = Bool()
  val memIsAtomic = Bool()

  val isAM = Bool()

  val isSystem = Bool()

  val isFP = Bool()

  val rs1 = UInt(5.W)
  val rs2 = UInt(5.W)
  val rd = UInt(5.W)

  val pc = UInt(32.W)
  // Compressed IMM, either imm[20:0] or imm [31:12]
  val cimm = UInt(21.W)

  // TODO: Actually embedded inside imm
  val funct7 = UInt(7.W)
  val funct3 = UInt(3.W)

  val smsel = UInt(param.pipeCnt.W)

  def isMul = rdalu && !alu2imm && funct7(0)

  def immExt = VecInit(Seq.fill(11)(cimm(20))).asUInt ## cimm
  def immU = cimm(19, 0) ## 0.U(12.W)
  def funct5 = funct7(6, 2)
  def isLR = memIsAtomic && funct5 === "b00010".U(5.W)
  def isSC = memIsAtomic && funct5 === "b00011".U(5.W)
  def isWFI = isSystem && funct3 === 0.U && rs2 === 5.U
  def isCSR = isSystem && funct3(1, 0) =/= 0.U
}
