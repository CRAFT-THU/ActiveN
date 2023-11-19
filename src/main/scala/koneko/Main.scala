package koneko;

import circt.stage.ChiselStage;

object Main extends App {
  val param = CoreParameters(
    initVec = BigInt("80000000", 16),
    i$Lines = 64,
    i$Assoc = 2,
    i$BlockSize = 32,
  )
  ChiselStage.emitSystemVerilogFile(new Core()(param), args)
}