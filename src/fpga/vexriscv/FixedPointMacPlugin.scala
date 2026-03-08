package vexiiriscv.execute

import spinal.core._
import spinal.lib._
import spinal.lib.misc.pipeline._
import vexiiriscv.riscv._
import vexiiriscv.riscv.Riscv._

// Q16.16 fixed-point multiply, multiply-accumulate, reciprocal, rsqrt, clamp, and divide.
// Uses RISC-V custom-0 opcode space (0x0B), R-type encoding.
//
// FXMUL    rd, rs1, rs2  (funct3=000): rd = (rs1 * rs2) >> 16
// FXMACS   rs1, rs2      (funct3=001): acc += (rs1 * rs2) >> 16
// FXMACR   rd            (funct3=010): rd = acc[31:0]; acc = 0
// FXRCP    rd, rs1       (funct3=011): rd = (1 << 32) / rs1  (Q16.16 reciprocal)
// FXCLAMP  rd, rs1, rs2  (funct3=100): rd = max(0, min(rs1, rs2))
// FXRSQRT  rd, rs1       (funct3=101): rd = 1/sqrt(rs1)  (Q16.16 inverse square root)
// FXDIV    rd, rs1, rs2  (funct3=110): rd = ((int64_t)rs1 << 16) / rs2
//
// Pipeline: multiply decomposed into 17x17 partial products at mulAt stage,
// combined at writebackAt stage.  FXRCP uses 3-cycle freeze in execute.
// FXDIV uses 34-cycle freeze (restoring division).

object FixedPointMacPlugin extends AreaObject {
  // Instruction type flags
  val IS_FXMUL   = Payload(Bool())
  val IS_FXMACS  = Payload(Bool())
  val IS_FXMACR  = Payload(Bool())
  val IS_FXRCP   = Payload(Bool())
  val IS_FXCLAMP = Payload(Bool())
  val IS_FXRSQRT = Payload(Bool())
  val IS_FXDIV   = Payload(Bool())

  // Pipelined partial products (execute -> writeback)
  val FX_MUL_LL = Payload(Bits(32 bits))
  val FX_MUL_LH = Payload(Bits(34 bits))
  val FX_MUL_HL = Payload(Bits(34 bits))
  val FX_MUL_HH = Payload(Bits(34 bits))

  // FXRCP pipeline stageables
  val RCP_Y1    = Payload(UInt(32 bits))
  val RCP_SHIFT = Payload(UInt(5 bits))
  val RCP_SIGN  = Payload(Bool())
  val RCP_ZERO  = Payload(Bool())

  // FXRSQRT pipeline stageables
  val RSQRT_Y0      = Payload(UInt(16 bits))
  val RSQRT_HALFCLZ = Payload(UInt(4 bits))
  val RSQRT_ZERO    = Payload(Bool())

  // FXDIV pipeline stageables
  val DIV_QUOTIENT = Payload(UInt(32 bits))
  val DIV_SIGN     = Payload(Bool())
  val DIV_ZERO     = Payload(Bool())
  val DIV_OVERFLOW = Payload(Bool())
}

// Custom-0 instruction encodings (opcode 0x0B = 0001011)
object Fx extends AreaObject {
  import IntRegFile._

  // R-type: funct7=0000000, funct3=NNN, opcode=0001011
  // TypeR reads RS1, RS2, writes RD
  val FXMUL   = TypeR(M"0000000----------000-----0001011")
  val FXCLAMP = TypeR(M"0000000----------100-----0001011")
  val FXDIV   = TypeR(M"0000000----------110-----0001011")

  // FXMACS: reads RS1+RS2, no RD write
  val FXMACS = SingleDecoding(
    M"0000000----------001-----0001011",
    List(RS1, RS2).map(IntRegFile -> _)
  )

  // FXMACR: writes RD only, no RS1/RS2
  val FXMACR = SingleDecoding(
    M"0000000----------010-----0001011",
    List(RD).map(IntRegFile -> _)
  )

  // FXRCP: reads RS1, writes RD
  val FXRCP = TypeI(M"0000000----------011-----0001011")

  // FXRSQRT: reads RS1, writes RD
  val FXRSQRT = TypeI(M"0000000----------101-----0001011")
}

class FixedPointMacPlugin(val layer: LaneLayer,
                          var executeAt: Int = 0,
                          var writebackAt: Int = 1) extends ExecutionUnitElementSimple(layer) {
  import FixedPointMacPlugin._

  val logic = during setup new Logic {
    awaitBuild()

    // FXMACS must be added BEFORE newWriteback() so it doesn't get
    // registered with the IntFormatPlugin (it has no RD).
    add(Fx.FXMACS).decode(IS_FXMACS -> True, IS_FXMUL -> False, IS_FXMACR -> False,
      IS_FXRCP -> False, IS_FXCLAMP -> False, IS_FXRSQRT -> False, IS_FXDIV -> False)
    for (op <- List(Fx.FXMACS); spec = layer(op)) {
      spec.addRsSpec(RS1, executeAt)
      spec.addRsSpec(RS2, executeAt)
      spec.setCompletion(executeAt)
    }

    val formatBus = newWriteback(ifp, writebackAt)

    // Register instructions with decoder
    // FXMUL: R-type, RS1+RS2, writes RD
    add(Fx.FXMUL).decode(IS_FXMUL -> True, IS_FXMACS -> False, IS_FXMACR -> False,
      IS_FXRCP -> False, IS_FXCLAMP -> False, IS_FXRSQRT -> False, IS_FXDIV -> False)
    for (op <- List(Fx.FXMUL); spec = layer(op)) {
      spec.addRsSpec(RS1, executeAt)
      spec.addRsSpec(RS2, executeAt)
    }

    // FXMACR: writes RD, no RS1/RS2
    add(Fx.FXMACR).decode(IS_FXMACR -> True, IS_FXMUL -> False, IS_FXMACS -> False,
      IS_FXRCP -> False, IS_FXCLAMP -> False, IS_FXRSQRT -> False, IS_FXDIV -> False)

    // FXRCP: RS1, writes RD
    add(Fx.FXRCP).decode(IS_FXRCP -> True, IS_FXMUL -> False, IS_FXMACS -> False,
      IS_FXMACR -> False, IS_FXCLAMP -> False, IS_FXRSQRT -> False, IS_FXDIV -> False)
    for (op <- List(Fx.FXRCP); spec = layer(op)) {
      spec.addRsSpec(RS1, executeAt)
    }

    // FXCLAMP: RS1+RS2, writes RD
    add(Fx.FXCLAMP).decode(IS_FXCLAMP -> True, IS_FXMUL -> False, IS_FXMACS -> False,
      IS_FXMACR -> False, IS_FXRCP -> False, IS_FXRSQRT -> False, IS_FXDIV -> False)
    for (op <- List(Fx.FXCLAMP); spec = layer(op)) {
      spec.addRsSpec(RS1, executeAt)
      spec.addRsSpec(RS2, executeAt)
    }

    // FXRSQRT: RS1, writes RD
    add(Fx.FXRSQRT).decode(IS_FXRSQRT -> True, IS_FXMUL -> False, IS_FXMACS -> False,
      IS_FXMACR -> False, IS_FXRCP -> False, IS_FXCLAMP -> False, IS_FXDIV -> False)
    for (op <- List(Fx.FXRSQRT); spec = layer(op)) {
      spec.addRsSpec(RS1, executeAt)
    }

    // FXDIV: RS1+RS2, writes RD
    add(Fx.FXDIV).decode(IS_FXDIV -> True, IS_FXMUL -> False, IS_FXMACS -> False,
      IS_FXMACR -> False, IS_FXRCP -> False, IS_FXCLAMP -> False, IS_FXRSQRT -> False)
    for (op <- List(Fx.FXDIV); spec = layer(op)) {
      spec.addRsSpec(RS1, executeAt)
      spec.addRsSpec(RS2, executeAt)
    }

    uopRetainer.release()

    // 48-bit accumulator for multiply-accumulate
    val accumulator = Reg(SInt(48 bits)) init (0)

    // Reciprocal LUT: 256 entries of Q0.16
    val rcpLut = Mem(UInt(16 bits), 256)
    rcpLut.initialContent = (0 until 256).map { i =>
      val x_real = 1.0 + (i + 0.5) / 256.0
      val rcp = scala.math.round(65536.0 / x_real).toInt
      BigInt(if (rcp > 0xFFFF) 0xFFFF else rcp)
    }.toArray

    // Inverse sqrt LUT: 512 entries of Q0.16
    val rsqrtLut = Mem(UInt(16 bits), 512)
    rsqrtLut.initialContent = (0 until 512).map { i =>
      if (i < 128) BigInt(0)
      else {
        val nval = (2.0 * i + 1.0) * (1 << 22).toDouble
        val rsqrt = scala.math.round(scala.math.pow(2, 30) / scala.math.sqrt(nval)).toInt
        BigInt(if (rsqrt > 0xFFFF) 0xFFFF else rsqrt)
      }
    }.toArray

    // ========== Execute stage ==========
    val exe = new el.Execute(executeAt) {
      val rs1 = up(el(IntRegFile, RS1))
      val rs2 = up(el(IntRegFile, RS2))
      val a = rs1.asSInt
      val b = rs2.asSInt

      // --- FXMUL/FXMACS: 17x17 partial products (pipelined to writeback) ---
      val aULow = a(15 downto 0).asUInt
      val bULow = b(15 downto 0).asUInt
      val aSLow = (False ## aULow).asSInt
      val bSLow = (False ## bULow).asSInt
      val aHigh = (a(31) ## a(31 downto 16)).asSInt
      val bHigh = (b(31) ## b(31 downto 16)).asSInt

      FX_MUL_LL := (aULow * bULow).asBits
      FX_MUL_LH := (aSLow * bHigh).asBits
      FX_MUL_HL := (aHigh * bSLow).asBits
      FX_MUL_HH := (aHigh * bHigh).asBits

      // --- FXRCP: 3-cycle execute ---
      val rcpPhase = Reg(UInt(2 bits)) init (0)
      val rcpAbsReg  = Reg(UInt(32 bits))
      val rcpClzReg  = Reg(UInt(5 bits))
      val rcpSignReg = Reg(Bool())
      val rcpZeroReg = Reg(Bool())
      val rcpY0Reg   = Reg(UInt(16 bits))
      val rcpXYReg   = Reg(UInt(32 bits))

      // CLZ and abs (combinational, used by RCP and RSQRT)
      val isNeg = a.msb
      val isZero = (a === 0)
      val absVal = Mux(isNeg, (-a).asUInt, a.asUInt)
      val clz = UInt(5 bits)
      clz := 0
      for (bit <- 0 until 32) {
        when(absVal(bit)) { clz := U(31 - bit, 5 bits) }
      }

      // Cycle 1 combinational (from cycle 0 registers)
      val normalized = rcpAbsReg |<< rcpClzReg
      val xNorm = normalized(31 downto 16)
      val lutIdx = xNorm(14 downto 7)
      val y0 = rcpLut.readAsync(lutIdx)
      val xy = xNorm * y0

      // Cycle 2 combinational (from cycle 1 registers)
      val correction = (~rcpXYReg) + U(1)
      val corrHi = correction(31 downto 16)
      val y1_full = rcpY0Reg * corrHi

      // Default stageable values
      RCP_Y1    := U(0, 32 bits)
      RCP_SHIFT := U(0, 5 bits)
      RCP_SIGN  := False
      RCP_ZERO  := False
      RSQRT_Y0      := U(0, 16 bits)
      RSQRT_HALFCLZ := U(0, 4 bits)
      RSQRT_ZERO    := False
      DIV_QUOTIENT := U(0, 32 bits)
      DIV_SIGN     := False
      DIV_ZERO     := False
      DIV_OVERFLOW := False

      // Freeze signals: must be proper signals, not constant True literals.
      // freezeWhen(True) would add a permanently-asserted freeze source.
      val rcpFreeze = isValid && SEL && IS_FXRCP && (rcpPhase =/= 2)
      el.freezeWhen(rcpFreeze)

      when(isValid && SEL && IS_FXRCP) {
        when(rcpPhase === 0) {
          rcpAbsReg  := absVal
          rcpClzReg  := clz
          rcpSignReg := isNeg
          rcpZeroReg := isZero
          rcpPhase   := 1
        } elsewhen (rcpPhase === 1) {
          rcpY0Reg := y0
          rcpXYReg := xy
          rcpPhase := 2
        } otherwise {
          RCP_Y1    := y1_full
          RCP_SHIFT := rcpClzReg
          RCP_SIGN  := rcpSignReg
          RCP_ZERO  := rcpZeroReg
        }
      }
      when(isReady || isCancel) { rcpPhase := 0 }

      // --- FXRSQRT: 2-cycle execute ---
      val rsqrtPhase = Reg(Bool()) init (False)
      val rsqrtClzReg  = Reg(UInt(5 bits))
      val rsqrtAbsReg  = Reg(UInt(32 bits))
      val rsqrtZeroReg = Reg(Bool())

      val rsqrtEvenClz = rsqrtClzReg & U(0x1E, 5 bits)
      val rsqrtNormalized = rsqrtAbsReg |<< rsqrtEvenClz
      val rsqrtLutIdx = rsqrtNormalized(31 downto 23)
      val rsqrtY0 = rsqrtLut.readAsync(rsqrtLutIdx)
      val rsqrtHalfClz = (rsqrtEvenClz >> 1).resize(4)

      val rsqrtFreeze = isValid && SEL && IS_FXRSQRT && !rsqrtPhase
      el.freezeWhen(rsqrtFreeze)

      when(isValid && SEL && IS_FXRSQRT) {
        when(!rsqrtPhase) {
          rsqrtAbsReg  := absVal
          rsqrtClzReg  := clz
          rsqrtZeroReg := isZero || isNeg
          rsqrtPhase   := True
        } otherwise {
          RSQRT_Y0      := rsqrtY0
          RSQRT_HALFCLZ := rsqrtHalfClz
          RSQRT_ZERO    := rsqrtZeroReg
        }
      }
      when(isReady || isCancel) { rsqrtPhase := False }

      // --- FXDIV: 34-cycle execute ---
      val divCounter    = Reg(UInt(6 bits)) init (0)
      val divRemainder  = Reg(UInt(33 bits))
      val divDividend   = Reg(UInt(32 bits))
      val divQuotient   = Reg(UInt(32 bits))
      val divDivisor    = Reg(UInt(32 bits))
      val divSignReg    = Reg(Bool())
      val divZeroReg    = Reg(Bool())
      val divOverflowReg = Reg(Bool())

      val divFreeze = isValid && SEL && IS_FXDIV && (divCounter =/= 33)
      el.freezeWhen(divFreeze)

      when(isValid && SEL && IS_FXDIV) {
        when(divCounter === 0) {
          val aSign = a.msb
          val bSign = b.msb
          val aAbs = Mux(aSign, (-a).asUInt, a.asUInt)
          val bAbs = Mux(bSign, (-b).asUInt, b.asUInt)
          divRemainder   := aAbs(31 downto 16).resize(33)
          divDividend    := (aAbs(15 downto 0) ## U(0, 16 bits)).asUInt
          divQuotient    := U(0)
          divDivisor     := bAbs
          divSignReg     := aSign ^ bSign
          divZeroReg     := (b === 0)
          divOverflowReg := (aAbs(31 downto 16).resize(32) >= bAbs) && (b =/= 0)
          divCounter     := 1
        } elsewhen (divCounter <= 32) {
          val shiftedRem = (divRemainder(31 downto 0) ## divDividend.msb).asUInt
          val diff = shiftedRem - divDivisor.resize(33)
          when(!diff.msb) {
            divRemainder := diff
            divQuotient  := (divQuotient(30 downto 0) ## True).asUInt
          } otherwise {
            divRemainder := shiftedRem
            divQuotient  := (divQuotient(30 downto 0) ## False).asUInt
          }
          divDividend := (divDividend |<< 1).resize(32)
          divCounter  := divCounter + 1
        } elsewhen (divCounter === 33) {
          DIV_QUOTIENT := divQuotient
          DIV_SIGN     := divSignReg
          DIV_ZERO     := divZeroReg
          DIV_OVERFLOW := divOverflowReg
        }
      }
      when(isReady || isCancel) { divCounter := 0 }
    }

    // ========== Writeback stage ==========
    val wb = new el.Execute(writebackAt) {
      val result = Bits(32 bits)
      result := B(0, 32 bits)

      // --- Combine FXMUL/FXMACS partial products ---
      val ll = FX_MUL_LL.asUInt
      val lh = FX_MUL_LH.asSInt
      val hl = FX_MUL_HL.asSInt
      val hh = FX_MUL_HH.asSInt

      val fxSum = (False ## ll(31 downto 16)).asSInt.resize(48) +
                  lh.resize(48) +
                  hl.resize(48) +
                  (hh.resize(32) |<< 16)
      val fxProduct = fxSum(31 downto 0)

      when(SEL && IS_FXMUL) {
        result := fxProduct.asBits
      }

      // FXMACS: accumulate (no register writeback)
      when(isValid && SEL && IS_FXMACS && !isCancel) {
        accumulator := accumulator + fxProduct.resize(48)
      }

      // FXMACR: read accumulator, reset
      when(SEL && IS_FXMACR) {
        result := accumulator(31 downto 0).asBits
      }
      when(isValid && SEL && IS_FXMACR && !isCancel) {
        accumulator := 0
      }

      // --- FXCLAMP (combinational, single cycle result) ---
      when(SEL && IS_FXCLAMP) {
        // Read RS1/RS2 at this stage too (they pipeline through)
        val rs1 = up(el(IntRegFile, RS1))
        val rs2 = up(el(IntRegFile, RS2))
        val val_s = rs1.asSInt
        val max_s = rs2.asSInt
        val clamped = Mux(val_s < S(0, 32 bits), S(0, 32 bits),
                      Mux(val_s > max_s, max_s, val_s))
        result := clamped.asBits
      }

      // --- FXRCP memory stage ---
      when(SEL && IS_FXRCP) {
        val y1   = RCP_Y1
        val clzV = RCP_SHIFT
        val sign = RCP_SIGN
        val zero = RCP_ZERO

        val needLeftShift = (clzV > U(30))
        val shiftRight = U(30, 5 bits) - clzV
        val resultRaw = Mux(needLeftShift,
          y1 |<< U(1),
          y1 |>> shiftRight
        )
        val overflow = resultRaw(31)
        val resultClamped = Mux(overflow, U(0x7FFFFFFFL, 32 bits), resultRaw)

        val signedResult = Mux(zero,
          S(0x7FFFFFFFL, 32 bits),
          Mux(sign, -resultClamped.asSInt, resultClamped.asSInt)
        )
        result := signedResult.asBits
      }

      // --- FXRSQRT memory stage ---
      when(SEL && IS_FXRSQRT) {
        val y0V     = RSQRT_Y0
        val halfClz = RSQRT_HALFCLZ
        val zero    = RSQRT_ZERO

        val shiftLeft = (halfClz >= U(6))
        val shiftAmt = Mux(shiftLeft,
          (halfClz - U(6, 4 bits)),
          (U(6, 4 bits) - halfClz)
        )
        val resultRaw = Mux(shiftLeft,
          y0V.resize(32) |<< shiftAmt,
          y0V.resize(32) |>> shiftAmt
        )
        result := Mux(zero, B(0x7FFFFFFF, 32 bits), resultRaw.asBits)
      }

      // --- FXDIV memory stage ---
      when(SEL && IS_FXDIV) {
        val quotient = DIV_QUOTIENT
        val sign     = DIV_SIGN
        val zero     = DIV_ZERO
        val overflow = DIV_OVERFLOW

        val satNeg = S(-0x7FFFFFFFL, 32 bits)  // 0x80000001 = Int.MinValue + 1
        val satPos = S(0x7FFFFFFFL, 32 bits)
        val signedResult = SInt(32 bits)
        when(zero || overflow) {
          signedResult := Mux(sign, satNeg, satPos)
        } otherwise {
          signedResult := Mux(sign, -quotient.asSInt, quotient.asSInt)
        }
        result := signedResult.asBits
      }

      // Write result to register file
      formatBus.valid := SEL
      formatBus.payload := result
    }
  }
}
