import chisel3._
import chisel3.util._

class Core(val params: CoreParameters) extends Module {
  val mem = IO(new Bundle {})
}
