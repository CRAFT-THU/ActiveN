package koneko;

import chisel3._
import chisel3.util._

case class CoreParameters(
  val initVec: BigInt,
  val i$Lines: Int,
  val i$BlockSize: Int,
  val i$Assoc: Int,
  val memDst: Int,
  val memTagBase: Int,
  val scratchpadSize: Int,
  val useFPU: Boolean,
) {
  require(scratchpadSize % 4 == 0)
  def i$Sets = i$Lines / i$Assoc
  def i$OffsetLen = log2Up(i$BlockSize)
  def i$InstrOffsetLen = log2Up(i$BlockSize / 4)
  def i$IndexLen = log2Up(i$Sets)
  def i$TagLen = 32 - i$OffsetLen - i$IndexLen
}