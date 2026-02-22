package vexriscv.plugin

import vexriscv._
import vexriscv.plugin._
import spinal.core._

// Q16.16 fixed-point multiply, multiply-accumulate, reciprocal, and clamp instructions.
// Uses RISC-V custom-0 opcode space (0x0B), R-type encoding.
//
// FXMUL   rd, rs1, rs2  (funct3=000): rd = (rs1 * rs2) >> 16
// FXMACS  rs1, rs2      (funct3=001): acc += (rs1 * rs2) >> 16
// FXMACR  rd            (funct3=010): rd = acc[31:0]; acc = 0
// FXRCP   rd, rs1       (funct3=011): rd = (1 << 32) / rs1  (Q16.16 reciprocal)
// FXCLAMP rd, rs1, rs2  (funct3=100): rd = max(0, min(rs1, rs2))  (signed clamp to [0, rs2])
//
// Pipeline: multiply DECOMPOSED into 17x17 partial products in execute stage,
// combined in memory stage.  Matches VexRiscv MulPlugin decomposition pattern
// so each partial product maps to one 18x18 DSP block on Cyclone V.
// FXMUL/FXMACR/FXRCP results are bypassable from memory stage.
//
// FXRCP implementation (5-cycle latency: 3 execute + memory + writeback):
//   Execute cycle 0: absolute value, CLZ -> local registers (~6ns)
//   Execute cycle 1: normalize (barrel shift), LUT lookup (256 entries),
//                     compute x_norm * y0 via DSP (16x16 -> 32) -> local registers (~8ns)
//   Execute cycle 2: Newton-Raphson correction: y1 = y0 * (2 - x_norm * y0)
//                     via DSP (16x16 -> 32) -> pipeline stageables (~5ns)
//   Memory stage:    de-normalize by barrel shift, saturate, apply sign (~4ns)

class FixedPointMacPlugin extends Plugin[VexRiscv] {

  // Instruction flags (set by decoder, flow through pipeline)
  object IS_FXMUL   extends Stageable(Bool)
  object IS_FXMACS  extends Stageable(Bool)
  object IS_FXMACR  extends Stageable(Bool)
  object IS_FXRCP   extends Stageable(Bool)
  object IS_FXCLAMP extends Stageable(Bool)

  // Pipelined partial products: execute -> memory
  // Decomposition: a = aHigh * 2^16 + aULow, b = bHigh * 2^16 + bULow
  // product = aULow*bULow + (aSLow*bHigh + aHigh*bSLow)*2^16 + aHigh*bHigh*2^32
  object FX_MUL_LL extends Stageable(Bits(32 bits))   // aULow * bULow (unsigned 16x16)
  object FX_MUL_LH extends Stageable(Bits(34 bits))   // aSLow * bHigh (signed 17x17)
  object FX_MUL_HL extends Stageable(Bits(34 bits))   // aHigh * bSLow (signed 17x17)
  object FX_MUL_HH extends Stageable(Bits(34 bits))   // aHigh * bHigh (signed 17x17)

  // FXRCP pipeline stageables: execute -> memory
  object RCP_Y1       extends Stageable(UInt(32 bits))  // Newton-Raphson refined y1 (Q1.31)
  object RCP_SHIFT    extends Stageable(UInt(5 bits))   // CLZ of |input|
  object RCP_SIGN     extends Stageable(Bool)           // sign of input
  object RCP_ZERO     extends Stageable(Bool)           // input was zero

  override def setup(pipeline: VexRiscv): Unit = {
    import pipeline.config._

    val decoderService = pipeline.service(classOf[DecoderService])

    decoderService.addDefault(IS_FXMUL, False)
    decoderService.addDefault(IS_FXMACS, False)
    decoderService.addDefault(IS_FXMACR, False)
    decoderService.addDefault(IS_FXRCP, False)
    decoderService.addDefault(IS_FXCLAMP, False)

    // FXMUL rd, rs1, rs2
    // funct7=0000000, funct3=000, opcode=0001011
    decoderService.add(
      M"0000000----------000-----0001011",
      List(
        IS_FXMUL             -> True,
        REGFILE_WRITE_VALID  -> True,
        BYPASSABLE_EXECUTE_STAGE -> False,
        BYPASSABLE_MEMORY_STAGE  -> True,
        RS1_USE -> True,
        RS2_USE -> True
      )
    )

    // FXMACS rs1, rs2 (rd ignored, should be x0)
    // funct7=0000000, funct3=001, opcode=0001011
    decoderService.add(
      M"0000000----------001-----0001011",
      List(
        IS_FXMACS            -> True,
        REGFILE_WRITE_VALID  -> False,
        RS1_USE -> True,
        RS2_USE -> True
      )
    )

    // FXMACR rd (rs1/rs2 ignored, should be x0)
    // funct7=0000000, funct3=010, opcode=0001011
    decoderService.add(
      M"0000000----------010-----0001011",
      List(
        IS_FXMACR            -> True,
        REGFILE_WRITE_VALID  -> True,
        BYPASSABLE_EXECUTE_STAGE -> False,
        BYPASSABLE_MEMORY_STAGE  -> True,
        RS1_USE -> False,
        RS2_USE -> False
      )
    )

    // FXRCP rd, rs1 (rs2 ignored, should be x0)
    // funct7=0000000, funct3=011, opcode=0001011
    // rd = (1 << 32) / rs1   (Q16.16 reciprocal of Q16.16 input)
    decoderService.add(
      M"0000000----------011-----0001011",
      List(
        IS_FXRCP             -> True,
        REGFILE_WRITE_VALID  -> True,
        BYPASSABLE_EXECUTE_STAGE -> False,
        BYPASSABLE_MEMORY_STAGE  -> True,
        RS1_USE -> True,
        RS2_USE -> False
      )
    )

    // FXCLAMP rd, rs1, rs2
    // funct7=0000000, funct3=100, opcode=0001011
    // rd = max(0, min(rs1, rs2))  (signed clamp to [0, rs2])
    decoderService.add(
      M"0000000----------100-----0001011",
      List(
        IS_FXCLAMP           -> True,
        REGFILE_WRITE_VALID  -> True,
        BYPASSABLE_EXECUTE_STAGE -> True,
        BYPASSABLE_MEMORY_STAGE  -> True,
        RS1_USE -> True,
        RS2_USE -> True
      )
    )
  }

  override def build(pipeline: VexRiscv): Unit = {
    import pipeline._
    import pipeline.config._

    // 48-bit accumulator for multiply-accumulate dot products.
    val accumulator = Reg(SInt(48 bits)) init(0)

    // Reciprocal LUT: 256 entries of Q0.16 values.
    // After normalization, input x_norm is in [1.0, 2.0) (Q1.31, bit 31 always set).
    // Index = top 8 fractional bits after the implicit leading 1.
    // LUT[i] = round(65536.0 / (1.0 + (i + 0.5) / 256.0))
    // Values range from ~65408 (1/1.002) to ~32800 (1/1.998), all fit in 16 bits.
    val rcpLut = Mem(UInt(16 bits), 256)
    rcpLut.initialContent = (0 until 256).map { i =>
      val x_real = 1.0 + (i + 0.5) / 256.0
      val rcp = math.round(65536.0 / x_real).toInt
      BigInt(if (rcp > 0xFFFF) 0xFFFF else rcp)
    }.toArray

    // Execute stage: compute partial products for FXMUL/FXMACS (pipelined to memory),
    // and FXRCP (3-cycle: abs/CLZ, normalize/LUT/DSP, Newton-Raphson/DSP)
    execute plug new Area {
      import execute._

      // --- FXMUL/FXMACS pipelined multiply ---
      // Decompose 32x32 signed multiply into four 17x17 partial products.
      // Each fits in one 18x18 DSP block on Cyclone V.
      // Combination happens in memory stage (next cycle).
      val a = input(RS1).asSInt
      val b = input(RS2).asSInt

      // Split into unsigned low (16-bit) and signed high (17-bit) halves.
      // Zero-extend low halves to 17-bit signed for mixed-sign DSP multiply.
      val aULow = a(15 downto 0).asUInt                      // 16-bit unsigned
      val bULow = b(15 downto 0).asUInt                      // 16-bit unsigned
      val aSLow = (False ## aULow).asSInt                     // 17-bit signed (zero-extended)
      val bSLow = (False ## bULow).asSInt                     // 17-bit signed (zero-extended)
      val aHigh = (a(31) ## a(31 downto 16)).asSInt           // 17-bit signed (sign-extended)
      val bHigh = (b(31) ## b(31 downto 16)).asSInt           // 17-bit signed (sign-extended)

      // Four partial products — each 17x17 maps to one 18x18 DSP block
      insert(FX_MUL_LL) := (aULow * bULow).asBits            // 32-bit unsigned
      insert(FX_MUL_LH) := (aSLow * bHigh).asBits            // 34-bit signed
      insert(FX_MUL_HL) := (aHigh * bSLow).asBits            // 34-bit signed
      insert(FX_MUL_HH) := (aHigh * bHigh).asBits            // 34-bit signed

      // --- FXRCP 3-cycle execute stage ---
      // Split across 3 execute cycles to meet timing at 100MHz:
      //   Cycle 0: abs + CLZ (~6ns) -> local registers
      //   Cycle 1: normalize + LUT + DSP(xNorm*y0) (~8ns) -> local registers
      //   Cycle 2: Newton-Raphson correction + DSP(y0*corrHi) (~5ns) -> stageables
      val rcpPhase = Reg(UInt(2 bits)) init(0)

      // Cycle 0 -> Cycle 1 registers
      val rcpAbsReg  = Reg(UInt(32 bits))
      val rcpClzReg  = Reg(UInt(5 bits))
      val rcpSignReg = Reg(Bool)
      val rcpZeroReg = Reg(Bool)

      // Cycle 1 -> Cycle 2 registers
      val rcpY0Reg   = Reg(UInt(16 bits))
      val rcpXYReg   = Reg(UInt(32 bits))

      // Cycle 0 combinational: abs value, CLZ (from RS1)
      val isNeg = a.msb
      val isZero = (a === 0)
      val absVal = Mux(isNeg, (-a).asUInt, a.asUInt)

      val clz = UInt(5 bits)
      clz := 0
      for (bit <- 0 until 32) {
        when(absVal(bit)) {
          clz := U(31 - bit, 5 bits)
        }
      }

      // Cycle 1 combinational: normalize + LUT + DSP (from cycle 0 registers)
      val normalized = rcpAbsReg |<< rcpClzReg
      val xNorm = normalized(31 downto 16)  // Q1.15, range [0x8000, 0xFFFF]
      val lutIdx = xNorm(14 downto 7)
      val y0 = rcpLut.readAsync(lutIdx)     // Q0.16
      val xy = xNorm * y0                   // 32-bit unsigned, Q1.31

      // Cycle 2 combinational: Newton-Raphson correction (from cycle 1 registers)
      // correction = 2^32 - xy = ~xy + 1 = (2.0 - x*y0) in Q1.31
      val correction = (~rcpXYReg) + U(1)
      val corrHi = correction(31 downto 16)   // Q1.15
      val y1_full = rcpY0Reg * corrHi         // Q0.16 x Q1.15 = Q1.31 (32-bit unsigned)

      // Default values for pipeline stageables
      insert(RCP_Y1)    := U(0, 32 bits)
      insert(RCP_SHIFT) := U(0, 5 bits)
      insert(RCP_SIGN)  := False
      insert(RCP_ZERO)  := False

      when(arbitration.isValid && input(IS_FXRCP)) {
        when(rcpPhase === 0) {
          // Cycle 0: abs + CLZ -> registers, stall
          rcpAbsReg  := absVal
          rcpClzReg  := clz
          rcpSignReg := isNeg
          rcpZeroReg := isZero
          rcpPhase   := 1
          arbitration.haltItself := True
        } elsewhen(rcpPhase === 1) {
          // Cycle 1: normalize + LUT + DSP -> registers, stall
          rcpY0Reg  := y0
          rcpXYReg  := xy
          rcpPhase  := 2
          arbitration.haltItself := True
        } otherwise {
          // Cycle 2: Newton-Raphson + DSP -> pipeline stageables, no halt
          insert(RCP_Y1)    := y1_full
          insert(RCP_SHIFT) := rcpClzReg
          insert(RCP_SIGN)  := rcpSignReg
          insert(RCP_ZERO)  := rcpZeroReg
        }
      }

      // Reset state when instruction advances or is flushed
      when(!arbitration.isStuck || arbitration.removeIt) {
        rcpPhase := 0
      }

      // --- FXCLAMP execute stage (pure combinational, no DSP) ---
      when(input(IS_FXCLAMP)) {
        val val_s = input(RS1).asSInt
        val max_s = input(RS2).asSInt
        val clamped = Mux(val_s < S(0, 32 bits), S(0, 32 bits),
                      Mux(val_s > max_s, max_s, val_s))
        output(REGFILE_WRITE_DATA) := clamped.asBits
      }
    }

    // Memory stage: combine partial products, write results, update accumulator.
    // FXRCP final step: de-normalize + saturate + sign.
    memory plug new Area {
      import memory._

      // --- Combine partial products for FXMUL/FXMACS ---
      // product = LL + (LH + HL) * 2^16 + HH * 2^32
      // We need (product >> 16)[31:0] = LL[31:16] + LH + HL + (HH << 16)
      // Computed in 48-bit signed arithmetic to handle carries correctly.
      val ll = input(FX_MUL_LL).asUInt    // 32-bit unsigned
      val lh = input(FX_MUL_LH).asSInt   // 34-bit signed
      val hl = input(FX_MUL_HL).asSInt   // 34-bit signed
      val hh = input(FX_MUL_HH).asSInt   // 34-bit signed

      val fxSum = (False ## ll(31 downto 16)).asSInt.resize(48) +  // LL >> 16, unsigned -> signed
                  lh.resize(48) +                                   // sign-extend to 48
                  hl.resize(48) +                                   // sign-extend to 48
                  (hh.resize(32) |<< 16)                            // HH << 16 -> 48-bit

      val fxProduct = fxSum(31 downto 0)   // Q16.16 result (SInt slice of SInt)

      when(input(IS_FXMUL)) {
        output(REGFILE_WRITE_DATA) := fxProduct.asBits
      }

      when(input(IS_FXMACS) && arbitration.isFiring) {
        accumulator := accumulator + fxProduct.resize(48)
      }

      when(input(IS_FXMACR)) {
        output(REGFILE_WRITE_DATA) := accumulator(31 downto 0).asBits
      }
      when(input(IS_FXMACR) && arbitration.isFiring) {
        accumulator := 0
      }

      // --- FXRCP memory stage: de-normalize + saturate + sign ---
      // Newton-Raphson was completed in execute cycle 2.
      // Only barrel shift, overflow check, and sign application remain.
      when(input(IS_FXRCP)) {
        val y1   = input(RCP_Y1)       // Newton-Raphson result (Q1.31)
        val clz  = input(RCP_SHIFT)
        val sign = input(RCP_SIGN)
        val zero = input(RCP_ZERO)

        // De-normalize: convert from reciprocal of normalized value to Q16.16.
        //   result = y1 * 2^(clz - 30)
        //          = y1 >> (30 - clz)  when clz <= 30
        //          = y1 << (clz - 30)  when clz > 30 (only clz=31)
        val needLeftShift = (clz > U(30))
        val shiftRight = U(30, 5 bits) - clz     // used when clz <= 30
        val resultRaw = Mux(needLeftShift,
          y1 |<< U(1),                            // clz=31: shift left by 1
          y1 |>> shiftRight                        // clz<=30: shift right
        )

        // Saturate on overflow: if bit 31 is set, the unsigned result
        // exceeds the signed positive range. Clamp to 0x7FFFFFFF.
        val overflow = resultRaw(31)
        val resultClamped = Mux(overflow, U(0x7FFFFFFF, 32 bits), resultRaw)

        // Apply sign and handle division by zero (return 0x7FFFFFFF)
        val signedResult = SInt(32 bits)
        signedResult := Mux(zero,
          S(0x7FFFFFFF, 32 bits),
          Mux(sign, -resultClamped.asSInt, resultClamped.asSInt)
        )

        output(REGFILE_WRITE_DATA) := signedResult.asBits
      }
    }
  }
}
