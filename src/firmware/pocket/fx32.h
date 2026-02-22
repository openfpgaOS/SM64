#ifndef FX32_H
#define FX32_H

#include <stdint.h>

// Q16.16 fixed-point type and operations using VexRiscv custom instructions.
// Custom-0 opcode (0x0B), R-type encoding, funct7=0000000.

typedef int32_t fx32;

// Constants
#define FX32_ONE       0x00010000   // 1.0
#define FX32_HALF      0x00008000   // 0.5
#define FX32_NEG_ONE   0xFFFF0000   // -1.0
#define FX32_PI        0x0003243F   // pi
#define FX32_2PI       0x0006487E   // 2*pi
#define FX32_HALF_PI   0x0001921F   // pi/2

// Conversions
#define FX32_FROM_INT(i)   ((fx32)((i) << 16))
#define FX32_FROM_FLOAT(f) ((fx32)((f) * 65536.0f))
#define FX32_TO_INT(f)     ((int32_t)((f) >> 16))
#define FX32_TO_FLOAT(f)   ((float)(f) / 65536.0f)

// Basic arithmetic (no custom instruction needed)
#define fx32_add(a, b) ((fx32)((a) + (b)))
#define fx32_sub(a, b) ((fx32)((a) - (b)))
#define fx32_neg(a)    ((fx32)(-(a)))

// ============================================
// Custom instruction intrinsics
// ============================================
// Encoding: .insn r opcode, funct3, funct7, rd, rs1, rs2

// FXMUL rd, rs1, rs2 (funct3=0, funct7=0, opcode=0x0B)
// rd = (rs1 * rs2) >> 16
static inline fx32 fx32_mul(fx32 a, fx32 b) {
    fx32 result;
    __asm__ volatile (
        ".insn r 0x0B, 0, 0, %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

// FXMACS rs1, rs2 (funct3=1, funct7=0, opcode=0x0B, rd=x0)
// acc += (rs1 * rs2) >> 16
static inline void fx32_mac(fx32 a, fx32 b) {
    __asm__ volatile (
        ".insn r 0x0B, 1, 0, x0, %0, %1"
        :
        : "r"(a), "r"(b)
    );
}

// FXMACR rd (funct3=2, funct7=0, opcode=0x0B, rs1=x0, rs2=x0)
// rd = acc; acc = 0
static inline fx32 fx32_mac_read(void) {
    fx32 result;
    __asm__ volatile (
        ".insn r 0x0B, 2, 0, %0, x0, x0"
        : "=r"(result)
    );
    return result;
}

// ============================================
// Dot products using FXMACS/FXMACR
// ============================================

static inline fx32 fx32_dot3(const fx32 *a, const fx32 *b) {
    fx32_mac(a[0], b[0]);
    fx32_mac(a[1], b[1]);
    fx32_mac(a[2], b[2]);
    return fx32_mac_read();
}

static inline fx32 fx32_dot4(const fx32 *a, const fx32 *b) {
    fx32_mac(a[0], b[0]);
    fx32_mac(a[1], b[1]);
    fx32_mac(a[2], b[2]);
    fx32_mac(a[3], b[3]);
    return fx32_mac_read();
}

// ============================================
// Hardware reciprocal
// ============================================

// FXRCP rd, rs1 (funct3=3, funct7=0, opcode=0x0B, rs2=x0)
// rd = (1 << 32) / rs1   (Q16.16 reciprocal via LUT + Newton-Raphson)
// Division by zero returns 0x7FFFFFFF. Overflow saturates.
static inline fx32 fx32_rcp(fx32 x) {
    fx32 result;
    __asm__ volatile (
        ".insn r 0x0B, 3, 0, %0, %1, x0"
        : "=r"(result)
        : "r"(x)
    );
    return result;
}

// ============================================
// Hardware clamp
// ============================================

// FXCLAMP rd, rs1, rs2 (funct3=4, funct7=0, opcode=0x0B)
// rd = max(0, min(rs1, rs2))  (signed clamp to [0, rs2])
static inline fx32 fx32_clamp(fx32 val, fx32 max_val) {
    fx32 result;
    __asm__ volatile (
        ".insn r 0x0B, 4, 0, %0, %1, %2"
        : "=r"(result)
        : "r"(val), "r"(max_val)
    );
    return result;
}

// ============================================
// Hardware inverse square root
// ============================================

// FXRSQRT rd, rs1 (funct3=5, funct7=0, opcode=0x0B, rs2=x0)
// rd = 1/sqrt(rs1)  (Q16.16 inverse square root via 512-entry LUT)
// Zero/negative input returns 0x7FFFFFFF.
static inline fx32 fx32_rsqrt(fx32 x) {
    fx32 result;
    __asm__ volatile (
        ".insn r 0x0B, 5, 0, %0, %1, x0"
        : "=r"(result)
        : "r"(x)
    );
    return result;
}

// ============================================
// Hardware division
// ============================================

// FXDIV rd, rs1, rs2 (funct3=6, funct7=0, opcode=0x0B)
// rd = ((int64_t)rs1 << 16) / rs2   (exact Q16.16 division, 34-cycle latency)
// Division by zero returns 0x7FFFFFFF/0x80000001. Overflow saturates.
static inline fx32 fx32_div(fx32 a, fx32 b) {
    fx32 result;
    __asm__ volatile (
        ".insn r 0x0B, 6, 0, %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

// Software fallback (for reference/testing)
static inline fx32 fx32_div_sw(fx32 a, fx32 b) {
    return (fx32)(((int64_t)a << 16) / b);
}

// ============================================
// Fast division via hardware rcp + Newton-Raphson
// ============================================

// Refined reciprocal: FXRCP (~16 bits) + one NR step → ~32 bits precision.
// Cost: ~9 cycles (1 rcp + 3 mul + 1 sub) vs ~50+ for software 64-bit div.
static inline fx32 fx32_rcp_nr(fx32 b) {
    fx32 y0 = fx32_rcp(b);                         // ~16 bits, 3 cycles
    fx32 by0 = fx32_mul(b, y0);                     // b*y0 ≈ 1.0
    return fx32_mul(y0, FX32_FROM_INT(2) - by0);    // y0*(2 - b*y0)
}

// Fast a/b: refined rcp + multiply. Full Q16.16 precision, ~12 cycles.
static inline fx32 fx32_div_fast(fx32 a, fx32 b) {
    return fx32_mul(a, fx32_rcp_nr(b));
}

#endif // FX32_H
