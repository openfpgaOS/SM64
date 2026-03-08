package vexiiriscv

import spinal.core._
import spinal.lib.system.tag.PmaRegion
import vexiiriscv.execute.{FixedPointMacPlugin, SrcPlugin}

import scala.collection.mutable.ArrayBuffer

// Generates VexiiRiscv with FixedPointMacPlugin for PocketSM64.
// Uses the same ParamSimple command-line flags as standard Generate,
// then appends Q16.16 fixed-point custom instructions on the lane0 pipeline.
//
// Usage:
//   cd VexiiRiscv
//   sbt "Test/runMain vexiiriscv.GenPocketSM64 <flags>"
object GenPocketSM64 extends App {
  val param = new ParamSimple()
  val sc = SpinalConfig()
  val regions = ArrayBuffer[PmaRegion]()

  assert(new scopt.OptionParser[Unit]("VexiiRiscv") {
    help("help").text("prints this usage text")
    param.addOptions(this)
    ParamSimple.addOptionRegion(this, regions)
  }.parse(args, ()).nonEmpty)

  if (regions.isEmpty) regions ++= ParamSimple.defaultPma

  val report = sc.generateVerilog {
    val plugins = param.plugins()

    // Find the early0 lane layer (shared by MulPlugin, IntAluPlugin, etc.)
    val early0 = plugins.collectFirst { case p: SrcPlugin if p.layer.name == "early0" => p.layer }.get
    plugins += new FixedPointMacPlugin(early0, executeAt = 0, writebackAt = 1)

    ParamSimple.setPma(plugins, regions)
    VexiiRiscv(plugins)
  }
}
