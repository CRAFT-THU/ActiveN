package koneko.exec

import chisel3._
import chisel3.util._

class FPUDataIO extends Bundle {
  val a = Input(UInt(32.W))
  val b = Input(UInt(32.W))
  val r = Output(UInt(32.W))
  val funct7 = Input(UInt(7.W))
  val funct3 = Input(UInt(3.W))
  val rs2b0 = Input(Bool())
  val valid = Input(Bool())
}

/** All implementations have one internal register boundary. Exec already
  * supplies the operand register and the following writeback edge, giving the
  * same two-cycle issue-to-writeback contract as the original FPU wrapper.
  */
class SelectedFPU(implementation: String, pipelined: Boolean) extends Module {
  val io = IO(new FPUDataIO)
  if (implementation == "cvfpu") {
    val unit = Module(new FPU(pipelined))
    unit.io.clock := clock
    unit.io.reset := reset
    unit.io.a := io.a
    unit.io.b := io.b
    unit.io.funct7 := io.funct7
    unit.io.funct3 := io.funct3
    unit.io.rs2b0 := io.rs2b0
    unit.io.valid := io.valid
    io.r := unit.io.r
  } else {
    val unit = if (implementation == "fudian") Module(new FudianFPU)
      else Module(new HardFloatFPU(implementation == "hardfloat-normal"))
    unit.io <> io
  }
}

abstract class ExperimentalFPU extends Module {
  val io = IO(new FPUDataIO)
  protected val rm = Mux(io.funct3 === 7.U, 0.U, io.funct3)
  protected val isAdd = io.funct7 === 0.U || io.funct7 === 4.U
  protected val isMul = io.funct7 === 8.U
  protected val isF2I = io.funct7 === 0x60.U
  protected val isI2F = io.funct7 === 0x68.U
  protected val addEnable = io.valid && isAdd
  protected val mulEnable = io.valid && isMul
  protected val f2iEnable = io.valid && isF2I
  protected val i2fEnable = io.valid && isI2F

  // These operations act directly on IEEE bits and are identical in all three
  // experiments. Preserve signed zeros and RISC-V canonical-NaN min/max rules.
  private val aNaN = io.a(30, 23).andR && io.a(22, 0).orR
  private val bNaN = io.b(30, 23).andR && io.b(22, 0).orR
  private val bothZero = !io.a(30, 0).orR && !io.b(30, 0).orR
  private val eq = (io.a === io.b || bothZero) && !aNaN && !bNaN
  private val lt = !aNaN && !bNaN && !bothZero &&
    Mux(io.a(31) =/= io.b(31), io.a(31),
      Mux(io.a(31), io.a(30, 0) > io.b(30, 0), io.a(30, 0) < io.b(30, 0)))
  private val max = io.funct3(0)
  private val minmax = Mux(aNaN && bNaN, "h7fc00000".U,
    Mux(aNaN, io.b, Mux(bNaN, io.a,
      Mux(bothZero, Cat(Mux(max, io.a(31) && io.b(31), io.a(31) || io.b(31)), 0.U(31.W)),
        Mux(lt ^ max, io.a, io.b)))))
  private val sign = MuxLookup(io.funct3, io.b(31))(Seq(
    0.U -> io.b(31), 1.U -> !io.b(31), 2.U -> (io.a(31) ^ io.b(31))))
  private val cmp = MuxLookup(io.funct3, false.B)(Seq(0.U -> (lt || eq), 1.U -> lt, 2.U -> eq))
  private val simple = MuxLookup(io.funct7, 0.U(32.W))(Seq(
    0x10.U -> Cat(sign, io.a(30, 0)), 0x14.U -> minmax, 0x50.U -> cmp.asUInt))
  protected val simpleResult = RegEnable(simple, io.valid && !(isAdd || isMul || isF2I || isI2F))
  protected val operation = RegEnable(io.funct7, 0.U, io.valid)
  protected def select(add: UInt, mul: UInt, f2i: UInt, i2f: UInt): Unit = {
    io.r := MuxLookup(operation, simpleResult)(Seq(
      0.U -> add, 4.U -> add, 8.U -> mul, 0x60.U -> f2i, 0x68.U -> i2f))
  }
}

class HardFloatFPU(normalOnly: Boolean) extends ExperimentalFPU {
  override def desiredName = if (normalOnly) "HardFloatNormalFPU" else "HardFloatFPU"

  // Standard recFN uses E+129 for normal FP32, so even without subnormal
  // normalization this is not a pure bit permutation. Zeros/Inf/NaN remain
  // supported. Subnormal inputs/results are outside the normal-only contract.
  private def encode(x: UInt): UInt = {
    if (!normalOnly) hardfloat.recFNFromFN(8, 24, x)
    else {
      val e = x(30, 23)
      val f = x(22, 0)
      val re = Mux(e === 0.U, 0.U(9.W),
        Mux(e.andR, Mux(f.orR, 448.U(9.W), 384.U(9.W)), e +& 129.U(8.W)))
      Cat(x(31), re, Mux(e === 0.U, 0.U(23.W), f))
    }
  }
  private def decode(x: UInt): UInt = {
    if (!normalOnly) hardfloat.fNFromRecFN(8, 24, x)
    else {
      val tag = x(31, 29)
      val zero = tag === 0.U
      val inf = tag === 6.U
      val special = tag(2, 1).andR
      val e = Mux(zero, 0.U(8.W), Mux(special, 255.U(8.W), (x(31, 23) - 129.U)(7, 0)))
      Cat(x(32), e, Mux(zero || inf, 0.U(23.W), x(22, 0)))
    }
  }
  private val a = encode(io.a)
  private val b = encode(io.b)
  private val rawA = hardfloat.rawFloatFromRecFN(8, 24, a)
  private val rawB = hardfloat.rawFloatFromRecFN(8, 24, b)
  private val add = Module(new hardfloat.AddRawFN(8, 24))
  add.io.a := rawA
  add.io.b := rawB
  add.io.subOp := io.funct7 === 4.U
  add.io.roundingMode := rm
  private val mul = Module(new hardfloat.MulRawFN(8, 24))
  mul.io.a := rawA
  mul.io.b := rawB

  private def rounded(raw: hardfloat.RawFloat, invalid: Bool, enable: Bool): UInt = {
    val round = Module(new hardfloat.RoundRawFNToRecFN(8, 24, 0))
    round.io.in := RegEnable(raw, enable)
    round.io.invalidExc := RegEnable(invalid, enable)
    round.io.infiniteExc := false.B
    round.io.roundingMode := RegEnable(rm, enable)
    round.io.detectTininess := 1.U
    decode(round.io.out)
  }
  private val addResult = rounded(add.io.rawOut, add.io.invalidExc, addEnable)
  private val mulResult = rounded(mul.io.rawOut, mul.io.invalidExc, mulEnable)
  private val f2i = Module(new hardfloat.RecFNToIN(8, 24, 32))
  f2i.io.in := a
  f2i.io.signedOut := !io.rs2b0
  f2i.io.roundingMode := rm
  private val f2iResult = RegEnable(f2i.io.out, f2iEnable)
  private val i2f = Module(new hardfloat.INToRecFN(32, 8, 24))
  i2f.io.in := io.a
  i2f.io.signedIn := !io.rs2b0
  i2f.io.roundingMode := rm
  i2f.io.detectTininess := 1.U
  private val i2fResult = decode(RegEnable(i2f.io.out, i2fEnable))
  select(addResult, mulResult, f2iResult, i2fResult)
}

class FudianFPU extends ExperimentalFPU {
  private val add1 = Module(new fudian.FCMA_ADD_s1(8, 24, 24))
  private val add2 = Module(new fudian.FCMA_ADD_s2(8, 24, 24))
  add1.io.a := io.a
  add1.io.b := Cat(io.b(31) ^ (io.funct7 === 4.U), io.b(30, 0))
  add1.io.rm := rm
  add1.io.b_inter_valid := false.B
  add1.io.b_inter_flags := 0.U.asTypeOf(add1.io.b_inter_flags)
  add2.io.in := RegEnable(add1.io.out, addEnable)

  private val mul1 = Module(new fudian.FMUL_s1(8, 24))
  private val mul2 = Module(new fudian.FMUL_s2(8, 24))
  private val mul3 = Module(new fudian.FMUL_s3(8, 24))
  private val multiplier = Module(new fudian.utils.Multiplier(25, pipeAt = Seq()))
  multiplier.io.a := fudian.RawFloat.fromUInt(io.a, 8, 24).sig
  multiplier.io.b := fudian.RawFloat.fromUInt(io.b, 8, 24).sig
  multiplier.io.regEnables.foreach(_ := true.B)
  mul1.io.a := io.a
  mul1.io.b := io.b
  mul1.io.rm := rm
  mul2.io.in := RegEnable(mul1.io.out, mulEnable)
  mul2.io.prod := RegEnable(multiplier.io.result, mulEnable)
  mul3.io.in := mul2.io.out

  private val f2i1 = Module(new fudian.FPToInt_s1(8, 24))
  private val f2i2 = Module(new fudian.FPToInt_s2(8, 24))
  f2i1.io.a := io.a
  f2i1.io.rm := rm
  f2i1.io.op := Cat(false.B, !io.rs2b0)
  f2i2.io.s1 := RegEnable(f2i1.io.to_s2, f2iEnable)

  private val i2f1 = Module(new fudian.IntToFP_prenorm)
  private val i2f2 = Module(new fudian.IntToFP_postnorm(8, 24))
  i2f1.io.in.int := Cat(0.U(32.W), io.a)
  i2f1.io.in.sign := !io.rs2b0
  i2f1.io.in.long := false.B
  i2f2.io.in := RegEnable(i2f1.io.out, i2fEnable)
  i2f2.io.rm := RegEnable(rm, i2fEnable)
  select(add2.io.result, mul3.io.result, f2i2.io.result(31, 0), i2f2.io.result)
}

/** Small standalone arithmetic/latency harness; no System simulator required. */
class FPUExperimentCheck extends Module {
  val io = IO(new Bundle {
    val a = Input(UInt(32.W))
    val b = Input(UInt(32.W))
    val funct7 = Input(UInt(7.W))
    val funct3 = Input(UInt(3.W))
    val rs2b0 = Input(Bool())
    val valid = Input(Bool())
    val results = Output(Vec(3, UInt(32.W)))
  })
  for ((name, i) <- Seq("hardfloat", "hardfloat-normal", "fudian").zipWithIndex) {
    val unit = Module(new SelectedFPU(name, true))
    unit.io.a := io.a
    unit.io.b := io.b
    unit.io.funct7 := io.funct7
    unit.io.funct3 := io.funct3
    unit.io.rs2b0 := io.rs2b0
    unit.io.valid := io.valid
    io.results(i) := unit.io.r
  }
}

object FPUExperimentCheckMain extends App {
  _root_.circt.stage.ChiselStage.emitSystemVerilogFile(new FPUExperimentCheck,
    Array("--target-dir", "generated/fpu-check"), Array("--split-verilog"))
}
