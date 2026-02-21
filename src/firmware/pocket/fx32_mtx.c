#include <ultra64.h>
#include "pocket/fx32_mtx.h"

// ============================================
// Basic operations
// ============================================

void mtx4_identity(Mtx4 dest) {
    register s32 i;
    register fx32 *d;
    for (d = (fx32 *)dest + 1, i = 0; i < 14; d++, i++) *d = 0;
    for (d = (fx32 *)dest, i = 0; i < 4; d += 5, i++) *d = FX32_ONE;
}

void mtx4_copy(Mtx4 dest, Mtx4 src) {
    register s32 i;
    register u32 *d = (u32 *)dest;
    register u32 *s = (u32 *)src;
    for (i = 0; i < 16; i++) *d++ = *s++;
}

// ============================================
// Matrix multiply: dest = a * b
// Assumes bottom row of both a and b is [0,0,0,1]
// Uses FXMACS/FXMACR for dot products
// ============================================

void mtx4_mul(Mtx4 dest, Mtx4 a, Mtx4 b) {
    Mtx4 temp;

    // Rows 0-2: 3x3 rotation block
    // temp[row][col] = a[row][0]*b[0][col] + a[row][1]*b[1][col] + a[row][2]*b[2][col]
    register s32 row, col;
    for (row = 0; row < 3; row++) {
        for (col = 0; col < 3; col++) {
            fx32_mac(a[row][0], b[0][col]);
            fx32_mac(a[row][1], b[1][col]);
            fx32_mac(a[row][2], b[2][col]);
            temp[row][col] = fx32_mac_read();
        }
    }

    // Row 3: translation = a[3] * b_rotation + b[3]
    for (col = 0; col < 3; col++) {
        fx32_mac(a[3][0], b[0][col]);
        fx32_mac(a[3][1], b[1][col]);
        fx32_mac(a[3][2], b[2][col]);
        temp[3][col] = fx32_mac_read() + b[3][col];
    }

    // Bottom row constants
    temp[0][3] = temp[1][3] = temp[2][3] = 0;
    temp[3][3] = FX32_ONE;

    mtx4_copy(dest, temp);
}

// ============================================
// Build rotation matrix (Z-X-Y order) with translation
// Matches mtxf_rotate_zxy_and_translate exactly
// ============================================

void mtx4_rotate_zxy_and_translate(Mtx4 dest, Vec3fx translate, s16 *rotate) {
    fx32 sx = fx32_sins(rotate[0]);
    fx32 cx = fx32_coss(rotate[0]);
    fx32 sy = fx32_sins(rotate[1]);
    fx32 cy = fx32_coss(rotate[1]);
    fx32 sz = fx32_sins(rotate[2]);
    fx32 cz = fx32_coss(rotate[2]);

    fx32 sx_sy = fx32_mul(sx, sy);
    fx32 sx_cy = fx32_mul(sx, cy);

    dest[0][0] = fx32_mul(cy, cz) + fx32_mul(sx_sy, sz);
    dest[1][0] = fx32_mul(fx32_neg(cy), sz) + fx32_mul(sx_sy, cz);
    dest[2][0] = fx32_mul(cx, sy);
    dest[3][0] = translate[0];

    dest[0][1] = fx32_mul(cx, sz);
    dest[1][1] = fx32_mul(cx, cz);
    dest[2][1] = fx32_neg(sx);
    dest[3][1] = translate[1];

    dest[0][2] = fx32_mul(fx32_neg(sy), cz) + fx32_mul(sx_cy, sz);
    dest[1][2] = fx32_mul(sy, sz) + fx32_mul(sx_cy, cz);
    dest[2][2] = fx32_mul(cx, cy);
    dest[3][2] = translate[2];

    dest[0][3] = dest[1][3] = dest[2][3] = 0;
    dest[3][3] = FX32_ONE;
}

// ============================================
// Build rotation matrix (X-Y-Z order) with translation
// Matches mtxf_rotate_xyz_and_translate exactly
// ============================================

void mtx4_rotate_xyz_and_translate(Mtx4 dest, Vec3fx translate, s16 *rotate) {
    fx32 sx = fx32_sins(rotate[0]);
    fx32 cx = fx32_coss(rotate[0]);
    fx32 sy = fx32_sins(rotate[1]);
    fx32 cy = fx32_coss(rotate[1]);
    fx32 sz = fx32_sins(rotate[2]);
    fx32 cz = fx32_coss(rotate[2]);

    fx32 sx_sy = fx32_mul(sx, sy);
    fx32 cx_sy = fx32_mul(cx, sy);

    dest[0][0] = fx32_mul(cy, cz);
    dest[0][1] = fx32_mul(cy, sz);
    dest[0][2] = fx32_neg(sy);
    dest[0][3] = 0;

    dest[1][0] = fx32_mul(sx_sy, cz) - fx32_mul(cx, sz);
    dest[1][1] = fx32_mul(sx_sy, sz) + fx32_mul(cx, cz);
    dest[1][2] = fx32_mul(sx, cy);
    dest[1][3] = 0;

    dest[2][0] = fx32_mul(cx_sy, cz) + fx32_mul(sx, sz);
    dest[2][1] = fx32_mul(cx_sy, sz) - fx32_mul(sx, cz);
    dest[2][2] = fx32_mul(cx, cy);
    dest[2][3] = 0;

    dest[3][0] = translate[0];
    dest[3][1] = translate[1];
    dest[3][2] = translate[2];
    dest[3][3] = FX32_ONE;
}

// ============================================
// Billboard matrix — faces camera with optional rotation
// Matches mtxf_billboard exactly
// ============================================

void mtx4_billboard(Mtx4 dest, Mtx4 mtx, Vec3fx position, s16 angle) {
    fx32 ca = fx32_coss(angle);
    fx32 sa = fx32_sins(angle);

    dest[0][0] = ca;
    dest[0][1] = sa;
    dest[0][2] = 0;
    dest[0][3] = 0;

    dest[1][0] = fx32_neg(sa);
    dest[1][1] = ca;
    dest[1][2] = 0;
    dest[1][3] = 0;

    dest[2][0] = 0;
    dest[2][1] = 0;
    dest[2][2] = FX32_ONE;
    dest[2][3] = 0;

    // Translation: position transformed by mtx rotation + mtx translation
    fx32_mac(mtx[0][0], position[0]);
    fx32_mac(mtx[1][0], position[1]);
    fx32_mac(mtx[2][0], position[2]);
    dest[3][0] = fx32_mac_read() + mtx[3][0];

    fx32_mac(mtx[0][1], position[0]);
    fx32_mac(mtx[1][1], position[1]);
    fx32_mac(mtx[2][1], position[2]);
    dest[3][1] = fx32_mac_read() + mtx[3][1];

    fx32_mac(mtx[0][2], position[0]);
    fx32_mac(mtx[1][2], position[1]);
    fx32_mac(mtx[2][2], position[2]);
    dest[3][2] = fx32_mac_read() + mtx[3][2];

    dest[3][3] = FX32_ONE;
}

// ============================================
// Scale matrix columns
// Matches mtxf_scale_vec3f exactly
// ============================================

void mtx4_scale_vec3f(Mtx4 dest, Mtx4 mtx, Vec3fx s) {
    register s32 i;
    for (i = 0; i < 4; i++) {
        dest[0][i] = fx32_mul(mtx[0][i], s[0]);
        dest[1][i] = fx32_mul(mtx[1][i], s[1]);
        dest[2][i] = fx32_mul(mtx[2][i], s[2]);
        dest[3][i] = mtx[3][i];
    }
}

// ============================================
// Translation matrix
// ============================================

void mtx4_translate(Mtx4 dest, Vec3fx b) {
    mtx4_identity(dest);
    dest[3][0] = b[0];
    dest[3][1] = b[1];
    dest[3][2] = b[2];
}

// ============================================
// Convert Mtx4 (Q16.16) -> N64 Mtx (split s16 integer/fraction)
// Pure bit manipulation — no float math at all
// Reference: guMtxF2L in lib/src/guMtxF2L.c
// ============================================

void mtx4_to_mtx(Mtx *dest, Mtx4 src) {
#ifdef GBI_FLOATS
    // Pack raw Q16.16 values into Mtx storage (reinterpret float[4][4] as fx32[4][4]).
    // gfx_sp_matrix reads these as fx32 directly — zero conversion overhead.
    __builtin_memcpy(dest->m, src, 64);
#else
    // N64 split integer/fraction format
    register s32 r, c;
    s32 *m1 = &dest->m[0][0];
    s32 *m2 = &dest->m[2][0];
    fx32 *s = (fx32 *)src;

    for (r = 0; r < 4; r++) {
        for (c = 0; c < 2; c++) {
            s32 val0 = s[r * 4 + 2 * c];
            s32 val1 = s[r * 4 + 2 * c + 1];
            *m1++ = (val0 & 0xFFFF0000) | ((val1 >> 16) & 0xFFFF);
            *m2++ = ((val0 << 16) & 0xFFFF0000) | (val1 & 0xFFFF);
        }
    }
#endif
}

// ============================================
// Convert Mtx4 (Q16.16) -> float Mat4 (for callbacks)
// ============================================

void mtx4_to_mat4(f32 dest[4][4], Mtx4 src) {
    register s32 i;
    f32 *d = (f32 *)dest;
    fx32 *s = (fx32 *)src;
    for (i = 0; i < 16; i++) {
        *d++ = FX32_TO_FLOAT(*s++);
    }
}

// ============================================
// Convert float Mat4 -> Mtx4 (Q16.16)
// ============================================

void mat4_to_mtx4(Mtx4 dest, f32 src[4][4]) {
    register s32 i;
    fx32 *d = (fx32 *)dest;
    f32 *s = (f32 *)src;
    for (i = 0; i < 16; i++) {
        *d++ = FX32_FROM_FLOAT(*s++);
    }
}
