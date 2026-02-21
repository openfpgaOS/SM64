package vexriscv.plugin

import vexriscv._
import vexriscv.plugin._
import spinal.core._

// Q16.16 fixed-point multiply, multiply-accumulate, and reciprocal instructions.
// Uses RISC-V custom-0 opcode space (0x0B), R-type encoding.
//
// FXMUL  rd, rs1, rs2  (funct3=000): rd = (rs1 * rs2) >> 16
// FXMACS rs1, rs2      (funct3=001): acc += (rs1 * rs2) >> 16
// FXMACR rd            (funct3=010): rd = acc[31:0]; acc = 0
// FXRCP  rd, rs1       (funct3=011): rd = (1 << 32) / rs1  (Q16.16 reciprocal)
//
// Pipeline: multiply computed in execute stage (DSP blocks),
// result extracted and accumulator updated in memory stage.
// FXMUL/FXMACR/FXRCP results are bypassable from memory stage.
//
// FXRCP implementation:
//   Execute stage: absolute value, CLZ, normalize to [1.0, 2.0) in Q1.31,
//                  LUT lookup (256 entries, Q0.16 initial estimate y0),
//                  compute x_norm * y0 via DSP (16×16 → 32).
//   Memory stage:  Newton-Raphson correction: y1 = y0 * (2 - x_norm * y0),
//                  de-normalize by barrel shift, saturate, apply sign.

class FixedPointMacPlugin extends Plugin[VexRiscv] {

  // Instruction flags (set by decoder, flow through pipeline)
  object IS_FXMUL  extends Stageable(Bool)
  object IS_FXMACS extends Stageable(Bool)
  object IS_FXMACR extends Stageable(Bool)
  object IS_FXRCP  extends Stageable(Bool)

  // Q16.16 product: execute → memory pipeline register
  object FX_PRODUCT extends Stageable(Bits(32 bits))

  // FXRCP pipeline registers: execute → memory
  object RCP_Y0       extends Stageable(UInt(16 bits))  // LUT estimate y0 (Q0.16)
  object RCP_XY       extends Stageable(UInt(32 bits))  // x_norm * y0 (Q1.31)
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

    // Execute stage: compute 32x32 signed multiply (for FXMUL/FXMACS)
    // and FXRCP first half (normalize, LUT lookup, x_norm * y0)
    execute plug new Area {
      import execute._

      // --- FXMUL/FXMACS multiply ---
      val isFx = input(IS_FXMUL) || input(IS_FXMACS)
      val a = input(RS1).asSInt
      val b = input(RS2).asSInt
      val product = a * b   // 64-bit signed
      insert(FX_PRODUCT) := Mux(isFx, product(47 downto 16).asBits, B(0, 32 bits))

      // --- FXRCP execute stage ---
      val rs1 = input(RS1).asSInt
      val isNeg = rs1.msb
      val isZero = (rs1 === 0)
      val absVal = Mux(isNeg, (-rs1).asUInt, rs1.asUInt)

      // CLZ via priority encoder: later `when` blocks override earlier ones,
      // so the highest set bit (last matching iteration) wins.
      val clz = UInt(5 bits)
      clz := 0
      for (bit <- 0 until 32) {
        when(absVal(bit)) {
          clz := U(31 - bit, 5 bits)
        }
      }

      // Normalize: shift left by clz so MSB lands at bit 31.
      // Result is in [1.0, 2.0) when interpreted as Q1.31.
      val normalized = absVal |<< clz
      val xNorm = normalized(31 downto 16)  // Q1.15, range [0x8000, 0xFFFF]

      // LUT lookup: index is the 8 fractional bits after the implicit leading 1
      val lutIdx = xNorm(14 downto 7)
      val y0 = rcpLut.readAsync(lutIdx)  // Q0.16, range (~0x8000, ~0xFF80)

      // DSP multiply: xNorm * y0 → Q1.15 × Q0.16 = Q1.31
      // Product ≈ 1.0 in Q1.31 (≈ 0x80000000) when estimate is accurate
      val xy = xNorm * y0  // 32-bit unsigned

      insert(RCP_Y0)    := y0
      insert(RCP_XY)    := xy
      insert(RCP_SHIFT) := clz
      insert(RCP_SIGN)  := isNeg
      insert(RCP_ZERO)  := isZero
    }

    // Memory stage: write results and update accumulator.
    // FXRCP second half: Newton-Raphson refinement + de-normalize + sign.
    memory plug new Area {
      import memory._

      val fxProduct = input(FX_PRODUCT).asSInt

      when(input(IS_FXMUL)) {
        output(REGFILE_WRITE_DATA) := input(FX_PRODUCT)
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

      // --- FXRCP memory stage ---
      when(input(IS_FXRCP)) {
        val y0   = input(RCP_Y0)
        val xy   = input(RCP_XY)       // x_norm * y0 in Q1.31
        val clz  = input(RCP_SHIFT)
        val sign = input(RCP_SIGN)
        val zero = input(RCP_ZERO)

        // Newton-Raphson: y1 = y0 * (2 - x_norm * y0)
        // xy is Q1.31, ideally ≈ 1.0 (0x80000000).
        // 2.0 in Q1.31 = 0x1_0000_0000 (33 bits).
        // correction = 2^32 - xy = ~xy + 1, which equals (2.0 - xy) in Q1.31.
        val correction = (~xy) + U(1)  // Q1.31, ≈ 1.0 when estimate is good

        // y1 = y0 * correction[31:16]  (Q0.16 × Q1.15 → Q1.31)
        val corrHi = correction(31 downto 16)
        val y1_full = y0 * corrHi      // 32-bit unsigned, Q1.31

        // De-normalize: convert from reciprocal of normalized value to Q16.16.
        //   result = 2^32 / absVal = 2^(32+clz) / normalized
        //   y1_full (Q1.31) ≈ 2^62 / normalized
        //   result = y1_full * 2^(clz - 30)
        //          = y1_full >> (30 - clz)  when clz <= 30
        //          = y1_full << (clz - 30)  when clz > 30 (only clz=31)
        val needLeftShift = (clz > U(30))
        val shiftRight = U(30, 5 bits) - clz     // used when clz <= 30
        val resultRaw = Mux(needLeftShift,
          y1_full |<< U(1),                       // clz=31: shift left by 1
          y1_full |>> shiftRight                   // clz<=30: shift right
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
