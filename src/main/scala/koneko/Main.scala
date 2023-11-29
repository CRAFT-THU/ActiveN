package koneko;

import circt.stage.ChiselStage;

object Main extends App {
  val param = CoreParameters(
    initVec = BigInt("80000000", 16),
    i$Lines = 16,
    i$Assoc = 1,
    i$BlockSize = 64,
    memDst = 0,
    memTagBase = 0x0,
    scratchpadSize = 16384,
    useFPU = true,
  )
  ChiselStage.emitSystemVerilogFile(new Core()(param), args)
}