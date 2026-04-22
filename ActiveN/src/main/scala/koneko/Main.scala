package koneko;

import circt.stage.ChiselStage;
import java.io.{File, PrintWriter}

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

  def coreConfigJson(p: CoreParameters): String = {
    val sizes = p.memCtrlSizes.map(s => s""""0x${s.toString(16)}"""").mkString(", ")
    s"""{
       |  "initVec": "0x${p.initVec.toString(16)}",
       |  "iCacheLines": ${p.i$Lines},
       |  "iCacheBlockSize": ${p.i$BlockSize},
       |  "iCacheAssoc": ${p.i$Assoc},
       |  "memDst": ${p.memDst},
       |  "memTagBase": ${p.memTagBase},
       |  "memBusWidth": ${p.memBusWidth},
       |  "memCtrlSizes": [${sizes}],
       |  "scratchpadSize": ${p.scratchpadSize},
       |  "useFPU": ${p.useFPU},
       |  "pipeCnt": ${p.pipeCnt}
       |}""".stripMargin
  }

  def systemConfigJson(p: SystemParameters): String = {
    s"""{
       |  "numMC": ${p.numMC},
       |  "numPU": ${p.numPU},
       |  "core": ${coreConfigJson(p.coreParams).split("\n").mkString("\n  ")}
       |}""".stripMargin
  }

  def writeJson(filename: String, content: String): Unit = {
    val pw = new PrintWriter(new File(filename))
    try pw.write(content) finally pw.close()
  }

  val emitSystem = sys.env.get("AN_SYSTEM").nonEmpty
  if (emitSystem) {
    val numPU = sys.env.get("AN_NUM_PU").flatMap(_.toIntOption).getOrElse(16)
    val numMC = sys.env.get("AN_NUM_MC").flatMap(_.toIntOption).getOrElse(1)
    implicit val sysParam = SystemParameters(numMC, numPU, param)
    ChiselStage.emitSystemVerilogFile(new System, args)
    writeJson("System.config.json", systemConfigJson(sysParam))
  } else {
    ChiselStage.emitSystemVerilogFile(new Core()(param), args)
    writeJson("Core.config.json", coreConfigJson(param))
  }
}
