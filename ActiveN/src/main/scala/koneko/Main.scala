package koneko;

import circt.stage.ChiselStage;
import java.io.{File, PrintWriter}

object Main extends App {
  val param = CoreParameters()

  def coreConfigJson(p: CoreParameters): ujson.Value = ujson.Obj(
    "initVec" -> s"0x${p.initVec.toString(16)}",
    "iCacheLines" -> p.i$Lines,
    "iCacheBlockSize" -> p.i$BlockSize,
    "iCacheAssoc" -> p.i$Assoc,
    "memBusWidth" -> p.memBusWidth,
    "memCtrlSizes" -> p.memCtrlSizes.map(s => ujson.Str(s"0x${s.toString(16)}")),
    "scratchpadSize" -> p.scratchpadSize,
    "useFPU" -> p.useFPU,
    "pipeCnt" -> p.pipeCnt,
    "sendQueueDepth" -> p.sendQueueDepth,
    "msgQueueDepth" -> p.msgQueueDepth,
    "evQueueDepth" -> p.evQueueDepth,
    "bcastQueueDepth" -> p.bcastQueueDepth,
  )

  def systemConfigJson(p: SystemParameters): ujson.Value = ujson.Obj(
    "numMC" -> p.numMC,
    "numPU" -> p.numPU,
    "core" -> coreConfigJson(p.coreParams),
  )

  def writeJson(filename: String, content: ujson.Value): Unit = {
    val pw = new PrintWriter(new File(filename))
    try pw.write(ujson.write(content, indent = 2)) finally pw.close()
  }

  // Parse arguments
  var system: Boolean = false
  var core: Boolean = false
  var pu: Option[Int] = None
  var mc: Option[Int] = None
  var output: Option[String] = None
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
        println("  --output=<path>  Set output path for generated files. Default: ./generated/core or ./generated/system")
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
        val stripped = a.stripPrefix("--output=")
        if (stripped.isEmpty) {
          println(s"Warning: ignoring empty output path")
        } else {
          output = Some(stripped)
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

  if (system && core) {
    println("Both --system and --core specified. Please specify only one.")
    sys.exit(1)
  }

  val subdir = if (system) "system" else "core"
  val outputUnwrapped = if (output.isDefined) {
    output.get
  } else {
    println(s"No output path specified. Using default ./generated/$subdir")
    s"./generated/$subdir"
  }

  var chiselArgs = Array("--target-dir", outputUnwrapped) ++ otherArgs

  if (system) {
    if (pu.isEmpty || mc.isEmpty) {
      println("For system generation, both --pu and --mc must be specified.")
      sys.exit(1)
    }
    implicit val sysParam = SystemParameters(mc.get, pu.get, param.copy(
      memCtrlSizes = List.fill(mc.get)(param.memCtrlSizes.head)
    ))
    ChiselStage.emitSystemVerilogFile(new System, chiselArgs)
    writeJson(s"$outputUnwrapped/System.config.json", systemConfigJson(sysParam))
  } else {
    ChiselStage.emitSystemVerilogFile(new Core()(param), chiselArgs)
    writeJson(s"$outputUnwrapped/Core.config.json", coreConfigJson(param))
  }
}
