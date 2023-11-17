import circt.stage.ChiselStage;

object Main extends App {
  val param = CoreParameters();
  ChiselStage.emitSystemVerilogFile(new Core(param))
}
