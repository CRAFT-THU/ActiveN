package koneko.exec

import chisel3._
import chisel3.util._

import koneko._

class SPM(implicit val param: CoreParameters) extends Module {
  val io = IO(new Bundle {
    val addr = Input(UInt(32.W))
    val wdata = Input(UInt(param.memBusWidth.W))
    val we = Input(UInt((param.memBusWidth / 8).W))
    val re = if (param.spmClockGating) Some(Input(UInt((param.memBusWidth / 8).W))) else None

    val data = Output(UInt(param.memBusWidth.W))
  })

  val LineType = Vec(param.memBusWidth / 8, UInt(8.W))
  val wordAddr = io.addr(log2Ceil(param.scratchpadSize) - 1, log2Ceil(param.memBusWidth / 8))

  if (param.spmSram) {
    // The available 256x16 1RW macro has no byte mask. One bank per byte
    // preserves every write mask and the existing synchronous read latency.
    // The unused upper byte makes physical capacity twice useful capacity.
    val bytes = Wire(LineType)
    for (lane <- 0 until param.memBusWidth / 8) {
      val bank = Module(new ScratchpadSramBank)
      val read = io.re.map(_(lane)).getOrElse(!io.we(lane)) && !io.we(lane)
      val enable = read || io.we(lane)
      if (param.spmClockGating) {
        val gate = Module(new ScratchpadClockGate)
        gate.io.CLK := clock
        gate.io.ENA := enable
        gate.io.SE := false.B
        bank.io.clk := gate.io.GCLK
      } else {
        bank.io.clk := clock
      }
      bank.io.ADDRESS := wordAddr
      bank.io.wd := Cat(0.U(8.W), io.wdata(8 * lane + 7, 8 * lane))
      bank.io.banksel := enable
      bank.io.read := read
      bank.io.write := io.we(lane)
      bytes(lane) := bank.io.dataout(7, 0)
    }
    io.data := bytes.asUInt
  } else {
    val scratchpad = SyncReadMem(param.scratchpadSize / (param.memBusWidth / 8), LineType)
    io.data := scratchpad.read(wordAddr).asUInt
    when(io.we.orR) {
      scratchpad.write(wordAddr, io.wdata.asTypeOf(LineType), io.we.asBools)
    }
  }
}

/** Standard-cell ICG functional view: latch the enable while clock is low. */
class ScratchpadClockGate extends BlackBox with HasBlackBoxInline {
  override def desiredName = "ICGx1_ASAP7_75t_R"
  val io = IO(new Bundle {
    val CLK = Input(Clock())
    val ENA = Input(Bool())
    val SE = Input(Bool())
    val GCLK = Output(Clock())
  })
  setInline("ICGx1_ASAP7_75t_R.sv", """
module ICGx1_ASAP7_75t_R (input CLK, ENA, SE, output GCLK);
  reg enabled;
  always_latch if (!CLK) enabled <= ENA | SE;
  assign GCLK = CLK & enabled;
endmodule
""")
}

/** Functional view only; ASIC synthesis must link the external Liberty cell
  * and exclude this inline view from its analysis source list.
  */
class ScratchpadSramBank extends BlackBox with HasBlackBoxInline {
  override def desiredName = "srambank_64x4x16_6t122"
  val io = IO(new Bundle {
    val clk = Input(Clock())
    val ADDRESS = Input(UInt(8.W))
    val wd = Input(UInt(16.W))
    val banksel = Input(Bool())
    val read = Input(Bool())
    val write = Input(Bool())
    val dataout = Output(UInt(16.W))
  })
  setInline("srambank_64x4x16_6t122.sv", """
module srambank_64x4x16_6t122 (
  input clk, input [7:0] ADDRESS, input [15:0] wd,
  input banksel, input read, input write, output reg [15:0] dataout
);
  reg [15:0] mem [0:255];
  always @(posedge clk) begin
    if (banksel && write) mem[ADDRESS] <= wd;
    else if (banksel && read) dataout <= mem[ADDRESS];
  end
endmodule
""")
}
