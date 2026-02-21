#ifndef FX32_MTX_H
#define FX32_MTX_H

#include "fx32.h"
#include <PR/ultratypes.h>
#include <PR/gbi.h>

// Forward-declare trig tables (defined in math_util via trig_tables.inc.c)
extern f32 gSineTable[];
#ifdef AVOID_UB
#define gCosineTable (gSineTable + 0x400)
#else
extern f32 gCosineTable[];
#endif

// Q16.16 4x4 matrix and 3D vector types
typedef fx32 Mtx4[4][4];
typedef fx32 Vec3fx[3];

// Float matrix type (same as SM64's Mat4)
typedef f32 Mat4[4][4];

// ============================================
// Trig helpers — lookup float table, convert to Q16.16
// ============================================

static inline fx32 fx32_sins(s16 angle) {
    return FX32_FROM_FLOAT(gSineTable[(u16)(angle) >> 4]);
}

static inline fx32 fx32_coss(s16 angle) {
    return FX32_FROM_FLOAT(gCosineTable[(u16)(angle) >> 4]);
}

// ============================================
// Vector conversion helpers
// ============================================

static inline void vec3s_to_vec3fx(Vec3fx dest, s16 *src) {
    dest[0] = FX32_FROM_INT(src[0]);
    dest[1] = FX32_FROM_INT(src[1]);
    dest[2] = FX32_FROM_INT(src[2]);
}

static inline void vec3f_to_vec3fx(Vec3fx dest, f32 *src) {
    dest[0] = FX32_FROM_FLOAT(src[0]);
    dest[1] = FX32_FROM_FLOAT(src[1]);
    dest[2] = FX32_FROM_FLOAT(src[2]);
}

// ============================================
// Matrix operations
// ============================================

void mtx4_identity(Mtx4 dest);
void mtx4_copy(Mtx4 dest, Mtx4 src);
void mtx4_mul(Mtx4 dest, Mtx4 a, Mtx4 b);
void mtx4_rotate_zxy_and_translate(Mtx4 dest, Vec3fx translate, s16 *rotate);
void mtx4_rotate_xyz_and_translate(Mtx4 dest, Vec3fx translate, s16 *rotate);
void mtx4_billboard(Mtx4 dest, Mtx4 mtx, Vec3fx position, s16 angle);
void mtx4_scale_vec3f(Mtx4 dest, Mtx4 mtx, Vec3fx s);
void mtx4_translate(Mtx4 dest, Vec3fx b);

// ============================================
// Conversions
// ============================================

// Q16.16 Mtx4 -> N64 split-format Mtx (pure bit manipulation, no float)
void mtx4_to_mtx(Mtx *dest, Mtx4 src);

// Q16.16 Mtx4 -> float Mat4 (for callbacks that need float)
void mtx4_to_mat4(f32 dest[4][4], Mtx4 src);

// float Mat4 -> Q16.16 Mtx4 (for camera lookat boundary)
void mat4_to_mtx4(Mtx4 dest, f32 src[4][4]);

#endif // FX32_MTX_H
