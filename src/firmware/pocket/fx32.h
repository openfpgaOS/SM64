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
// Software operations (no custom instruction)
// ============================================

static inline fx32 fx32_div(fx32 a, fx32 b) {
    return (fx32)(((int64_t)a << 16) / b);
}

#endif // FX32_H
