package koneko;

import circt.stage.ChiselStage;

object Main extends App {
  val pipeCnt = sys.env.get("AN_PIPE_CNT").flatMap(_.toIntOption).getOrElse(2)
  val param = CoreParameters(
    initVec = BigInt("80000000", 16),
    i$Lines = 16,
    i$Assoc = 1,
    i$BlockSize = 64,
    memDst = 0,
    memTagBase = 0x0,
    scratchpadSize = 16384,
    useFPU = true,
    pipeCnt = pipeCnt,
  )
  ChiselStage.emitSystemVerilogFile(new Core()(param), args)
}
