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

  // Parse arguments
  var system: Boolean = false
  var core: Boolean = false
  var pu: Option[Int] = None
  var mc: Option[Int] = None
  var output: String = "./generated"
  var spliter = args.indexOf("--")
  val (mainArgs, otherArgs) = if (spliter >= 0) {
    this.args.splitAt(spliter)
  } else {
    (args, Array.empty[String])
  }

  for (arg <- mainArgs) {
    arg match {
      case "--help" | "-h" => {
        println("Usage: [options] [-- <chisel options>]")
        println("Options:")
        println("  --core           Emit Core with default config")
        println("  --system         Emit System with default config")
        println("  --pu=<N>         Emit System with N PUs")
        println("  --mc=<N>         Emit System with N MCs")
        println("  --output=<path>  Set output path for generated files. Default: ./generated")
        sys.exit(0)
      }
      case "--core" => core = true
      case "--system" => system = true
      case a if a.startsWith("--pu=") => {
        pu = a.stripPrefix("--pu=").toIntOption
        if (pu.isEmpty) {
          println(s"Invalid PU count: ${a.stripPrefix("--pu=")}")
          sys.exit(1)
        }
      }
      case a if a.startsWith("--mc=") => {
        mc = a.stripPrefix("--mc=").toIntOption
        if (mc.isEmpty) {
          println(s"Invalid MC count: ${a.stripPrefix("--mc=")}")
          sys.exit(1)
        }
      }
      case a if a.startsWith("--output=") => {
        output = a.stripPrefix("--output=")
        if (output.isEmpty) {
          println(s"Warning: ignoring empty output path, using './generated'")
          output = "./generated"
        }
      }
      case _ => {
        println(s"Unknown argument: $arg")
        sys.exit(1)
      }
    }
  }

  if (!system && !core) {
    println("No target specified. Use --system or --core.")
    sys.exit(1)
  }

  var chiselArgs = Array("--target-dir", output) ++ otherArgs

  if (system) {
    if (pu.isEmpty || mc.isEmpty) {
      println("For system generation, both --pu and --mc must be specified.")
      sys.exit(1)
    }
    implicit val sysParam = SystemParameters(mc.get, pu.get, param)
    ChiselStage.emitSystemVerilogFile(new System, chiselArgs)
    writeJson(s"$output/System.config.json", systemConfigJson(sysParam))
  } else {
    ChiselStage.emitSystemVerilogFile(new Core()(param), chiselArgs)
    writeJson(s"$output/Core.config.json", coreConfigJson(param))
  }
}
