package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class RegDirect extends Bundle {
  val rdata = Output(UInt(32.W))
  val wdata = Input(UInt(32.W))
  val wen = Input(Bool())
}

class RegFile(val smtid: Int) extends Module {
  val read = IO(Vec(2, new Bundle {
    val num = Input(UInt(5.W))
    val value = Output(UInt(32.W))
  }))

  val write = IO(new Bundle {
    val en = Input(Bool())
    val num = Input(UInt(5.W))
    val value = Input(UInt(32.W))
  })

  // AM specific specific ports
  val msgDirect = IO(Vec(4, new RegDirect))

  // Disable DCE for RegFile
  val regs = Seq(null) ++ (
    for (i <- 1 until 32) yield {
      val reg = if (i == 10) RegInit(smtid.U(32.W)) else Reg(UInt(32.W))
      reg.suggestName(s"x$i")
      dontTouch(reg)
    }
  )

  // Read ports
  val rdata = VecInit(regs.map(r => if (r == null) 0.U else r))
  for (r <- read) {
    r.value := rdata(r.num)
  }

  // Write ports
  for ((r, i) <- regs.zipWithIndex) if (r != null) {
    when(write.en && write.num === i.U) {
      r := write.value
    }
  }

  // Direct connections
  for ((d, i) <- msgDirect.zipWithIndex) {
    val target = regs(i + 10)
    d.rdata := target
    when(d.wen) {
      target := d.wdata
    }
  }
}
