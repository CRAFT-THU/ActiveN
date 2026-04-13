// import Mill dependency
import mill._
import scalalib._

object Koneko extends SbtModule { m =>
  override def moduleDir = os.pwd
  override def scalaVersion = "2.13.8"
  override def scalacOptions = Seq(
    "-language:reflectiveCalls",
    "-deprecation",
    "-feature",
    "-Xcheckinit",
    "-Ymacro-annotations",
    "-P:chiselplugin:genBundleElements"
  )
  override def mvnDeps = Seq(
    mvn"org.chipsalliance::chisel:6.0.0",
  )
  override def scalacPluginMvnDeps = Seq(
    mvn"org.chipsalliance:::chisel-plugin:6.0.0",
  )
}
