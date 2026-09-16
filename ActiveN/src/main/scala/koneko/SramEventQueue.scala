package koneko

import chisel3._
import chisel3.util._
import koneko.exec.ScratchpadClockGate

/** A non-flow, non-pipe 16-entry event queue with a synchronous SRAM payload.
  * Each handler retains independent read/write ports and a visible head.
  */
class SramEventQueue(clockGating: Boolean = false) extends Module {
  val io = IO(new QueueIO(Vec(4, UInt(32.W)), 16))
  val head = RegInit(0.U(4.W))
  val tail = RegInit(0.U(4.W))
  val maybeFull = RegInit(false.B)
  val same = head === tail
  io.enq.ready := !(same && maybeFull)
  io.deq.valid := !(same && !maybeFull)
  io.count := Mux(same, Mux(maybeFull, 16.U, 0.U), (tail - head)(3, 0))
  // Stable transfer names are also used by the simulator's queue profiler.
  val doEnq = dontTouch(WireDefault(io.enq.fire))
  val doDeq = dontTouch(WireDefault(io.deq.fire))

  val nextHead = (head + 1.U)(3, 0)
  val readAddr = Mux(doDeq, nextHead, head)
  when(doEnq) { tail := tail + 1.U }
  when(doDeq) { head := nextHead }
  when(doEnq =/= doDeq) { maybeFull := doEnq }

  val bypass = RegNext(doEnq && tail === readAddr, false.B)
  val bypassData = RegEnable(io.enq.bits.asUInt, doEnq)

  val bank = Module(new EventSramBank)
  if (clockGating) {
    val readGate = Module(new ScratchpadClockGate)
    val writeGate = Module(new ScratchpadClockGate)
    readGate.io.CLK := clock
    // Refresh after a collision before dropping the registered bypass. A
    // stalled, already fetched head needs no further SRAM reads.
    readGate.io.ENA := doDeq || bypass
    readGate.io.SE := false.B
    writeGate.io.CLK := clock
    writeGate.io.ENA := doEnq
    writeGate.io.SE := false.B
    bank.io.clk_a := readGate.io.GCLK
    bank.io.clk_b := writeGate.io.GCLK
  } else {
    bank.io.clk_a := clock
    bank.io.clk_b := clock
  }
  bank.io.we_a := false.B
  bank.io.addr_a := readAddr
  bank.io.din_a := 0.U
  bank.io.we_b := doEnq
  bank.io.addr_b := tail
  bank.io.din_b := io.enq.bits.asUInt

  // A same-address read/write may be undefined inside the macro. Forward the
  // accepted write after the edge, including empty enqueue and replacement of
  // the only entry during simultaneous dequeue/enqueue. Output never depends
  // combinationally on downstream ready.
  io.deq.bits := Mux(bypass, bypassData, bank.io.dout_a).asTypeOf(io.deq.bits)
}

/** Functional view for the budgetary 2RW model, not a characterized macro.
  * ASIC synthesis links its external Liberty view instead of this model.
  * The queue uses port A only for reads and port B only for writes.
  */
class EventSramBank extends BlackBox with HasBlackBoxInline {
  override def desiredName = "ActiveNEventSram2RW16x128"
  val io = IO(new Bundle {
    val clk_a = Input(Clock())
    val we_a = Input(Bool())
    val addr_a = Input(UInt(4.W))
    val din_a = Input(UInt(128.W))
    val dout_a = Output(UInt(128.W))
    val clk_b = Input(Clock())
    val we_b = Input(Bool())
    val addr_b = Input(UInt(4.W))
    val din_b = Input(UInt(128.W))
    val dout_b = Output(UInt(128.W))
  })
  setInline("ActiveNEventSram2RW16x128.sv", """
module ActiveNEventSram2RW16x128 (
  input clk_a, input we_a, input [3:0] addr_a, input [127:0] din_a,
  output reg [127:0] dout_a,
  input clk_b, input we_b, input [3:0] addr_b, input [127:0] din_b,
  output reg [127:0] dout_b
);
  reg [127:0] mem [0:15];
  always @(posedge clk_a) begin
    if (we_a) mem[addr_a] <= din_a;
    else dout_a <= mem[addr_a];
  end
  always @(posedge clk_b) begin
    if (we_b) mem[addr_b] <= din_b;
    else dout_b <= mem[addr_b];
  end
endmodule
""")
}
