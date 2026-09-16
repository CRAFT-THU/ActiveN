package koneko;

import chisel3._
import chisel3.util._

case class CoreParameters(
  val initVec: BigInt = BigInt("60000000", 16),
  val i$Lines: Int = 1,
  val i$BlockSize: Int = 64,
  val i$Assoc: Int = 1,
  val memBusWidth: Int = 512,
  val memCtrlSizes: List[BigInt] = List(BigInt("100000000", 16)), // 4 GiB default-addressable mem
  val scratchpadSize: Int = 16384,
  val spmSram: Boolean = false,
  val eventSram: Boolean = false,
  val spmClockGating: Boolean = false,
  val eventClockGating: Boolean = false,
  val fpuPipeline: Boolean = false,
  val useFPU: Boolean = true,
  val pipeCnt: Int = 2,

  // Depth of the send queue
  val sendQueueDepth: Int = 32,
  // Depth of the shared message ingestion queue
  val msgQueueDepth: Int = 4,
  // Depth of each event queue
  val evQueueDepth: Int = 16,
  // Depth of the broadcast queue in front of the evQueue
  val bcastQueueDepth: Int = 8,
) {
  require(scratchpadSize % 4 == 0)
  require(!spmSram || (scratchpadSize == 16384 && memBusWidth == 512),
    "The ASAP7 scratchpad macro experiment requires 16 KiB and 512-bit memory lines")
  require(!eventSram || evQueueDepth == 16,
    "The event SRAM experiment preserves the existing 16-entry handler queues")
  require(!spmClockGating || spmSram, "Scratchpad clock gating requires SRAM banks")
  require(!eventClockGating || eventSram, "Event clock gating requires SRAM queues")
  require(memBusWidth >= 64 && isPow2(memBusWidth), "memBusWidth must be a power of 2 and >= 64")
  require(i$BlockSize >= memBusWidth / 8, "I-cache block must contain at least one memory line")
  require(i$BlockSize % (memBusWidth / 8) == 0, "I-cache block size must be a multiple of the memory line size")
  require(sendQueueDepth >= 4 && isPow2(sendQueueDepth), "sendQueueDepth must be a power of 2 and >= 4")
  require(evQueueDepth >= 2, "evQueueDepth must be at least 2 to avoid deadlock when waiting for a response")
  require(bcastQueueDepth >= 2, "bcastQueueDepth must be at least 2 to avoid deadlock when waiting for a response")
  def i$Sets = i$Lines / i$Assoc
  def i$OffsetLen = log2Up(i$BlockSize)
  def i$InstrOffsetLen = log2Up(i$BlockSize / 4)
  def i$IndexLen = log2Ceil(i$Sets)
  def i$TagLen = 32 - i$OffsetLen - i$IndexLen
}
