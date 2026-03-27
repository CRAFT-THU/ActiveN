package koneko;

import circt.stage.ChiselStage;

object Main extends App {
  val pipeCnt = sys.env.get("AN_PIPE_CNT").flatMap(_.toIntOption).getOrElse(2)
  val param = CoreParameters(
    initVec = BigInt("80000000", 16),
    i$Lines = 16,
    i$Assoc = 2,
    i$BlockSize = 64,
    memDst = 0,
    memTagBase = 0x0,
    memBusWidth = 256, // DDR4
    memCtrlSizes = List(BigInt("100000000", 16)), // 4 GiB
    scratchpadSize = 16384,
    useFPU = true,
    pipeCnt = pipeCnt,
  )

  val emitSystem = sys.env.get("AN_SYSTEM").nonEmpty
  if (emitSystem) {
    val numPU = sys.env.get("AN_NUM_PU").flatMap(_.toIntOption).getOrElse(16)
    val numMC = sys.env.get("AN_NUM_MC").flatMap(_.toIntOption).getOrElse(1)
    implicit val sysParam = SystemParameters(numMC, numPU, param)
    ChiselStage.emitSystemVerilogFile(new System, args)
  } else {
    ChiselStage.emitSystemVerilogFile(new Core()(param), args)
  }
}
