#ifdef ENABLE_SOFTRAST

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_pc.h"
#include "gfx_soft.h"
#include "gfx_cc.h"
#include "macros.h"


typedef float rv_t;
#define RV_ONE       1.0f
#define RV_HALF      0.5f
#define RV_ZERO      0.0f
#define RV_MUL(a,b)  ((a)*(b))
#define RV_RCP(a)    (1.0f/(a))
#define RV_DIV(a,b)  ((a)/(b))
#define RV_TO_INT(a) ((int)(a))
#define RV_FROM_INT(a) ((float)(a))
#define RV_FROM_FLOAT(f) (f)
#define RV_LITERAL(f) (f)
#define RV_Z_TO_ZBUF(v) ((int)((v) * 65535.f))

#define ALIGN(x, a) (((x) + (a - 1)) & ~(a - 1))

#define MAX_TEXTURES 3072
#define TEXCACHE_STEP 0x10000

enum WrapType {
    WRAP_REPEAT = 0,
    WRAP_CLAMP  = 1,
    WRAP_MIRROR = 2,
};

enum DrawFlags {
    DRAW_ZWRITE = 1,
    DRAW_BLEND = 2,
    DRAW_BLEND_EDGE = 4,
};

enum MixType {
    SH_MT_NONE            = 0,
    SH_MT_COLOR           = 1 << 0,
    SH_MT_COLOR_COLOR     = 1 << 1,
    SH_MT_TEXTURE         = 1 << 2,
    SH_MT_TEXTURE_COLOR   = 1 << 3,
    SH_MT_TEXTURE_TEXTURE = 1 << 4,
};

typedef union Vector2 {
    struct { rv_t x, y; };
    struct { rv_t u, v; };
} Vector2;

typedef union Vector3 {
    struct { rv_t r, g, b; };
    struct { rv_t x, y, z; };
    Vector2 xy;
    rv_t v[3];
} Vector3;

typedef union Vector4 {
    struct { rv_t r, g, b, a; };
    struct { rv_t x, y, z, w; };
    Vector2 xy;
    Vector3 xyz;
    rv_t v[4];
} Vector4;

typedef union Color4 {
    struct { uint8_t r, g, b, a; };
    uint32_t c;
} Color4;

struct Tri {
    rv_t *v0;
    rv_t *v1;
    rv_t *v2;
};

struct Texture;

// texture sampling function: takes integer u,v and wraps/clamps it, samples texture, returns color
typedef Color4 (*sample_fn_t)(const struct Texture * const, const int, const int);
// pixel drawing function: does blending, zwriting, alpha edge checking or whatever else, then plots pixel
typedef void (*draw_fn_t)(const int idx, uint16_t uz, const Color4 src);
// color combiner: takes vertex properties and obtains final fragment color from them
typedef Color4 (*combine_fn_t)(const rv_t z, const rv_t *props);
// rasterizer: walks the triangle and interpolates a fixed amount of vertex properties
typedef void (*rast_fn_t)(const struct Tri tri);

struct ShaderProgram {
    uint32_t shader_id;
    struct CCFeatures cc;
    enum MixType mix;
    uint32_t draw_flags;
    int num_props;
    combine_fn_t combine;
    rast_fn_t rast;
};

struct Texture {
    int w, h;           // size
    rv_t fw, fh;        // rv_t size for sampling multiply
    int wrap_w, wrap_h; // size - 1 for wrapping
    bool filter;        // linear filter
    uint32_t addr;      // offset into texcache
    sample_fn_t sample; // sampling function (does wrapping/clamping)
};

struct Viewport {
    int x, y, w, h; // rect
    rv_t cx, cy;    // center
    rv_t hw, hh;    // half size
};

struct ClipRect {
    int x0, y0; // top left
    int x1, y1; // bottom right
};

uint32_t *gfx_output;

// this is set in the drawing functions
static draw_fn_t draw_fn;

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;
static struct ShaderProgram *cur_shader = NULL;

static struct Texture *cur_tex[2]; // currently selected textures for both tiles
static struct Texture tex_hdr[MAX_TEXTURES];
static uint32_t tex_num = 0; // amount of textures in cache
static int cur_tmu = 0; // select tile (used only for uploading)

// texture cache: linearly stores RGBA data of every cached texture
static uint8_t *texcache;
static uint32_t texcache_addr; // current offset into cache
static uint32_t texcache_size; // cache capacity

static bool do_blend; // fragment blending toggle
static bool do_clip;  // scissor toggle

static struct ClipRect r_clip;
static struct Viewport r_view;

static Color4 fog_color; // this is set by set_fog_color() calls from gfx_pc

static bool z_test;        // whether to perform depth testing
static bool z_write;       // whether to write into the Z buffer
static int32_t z_offset;   // offset for decal mode (z-buffer units)
static uint16_t *z_buffer;

static int scr_width;
static int scr_height;
static int scr_size; // scr_width * scr_height

#ifdef TARGET_POCKET
// Replace 64KB mult_tab + 131KB lerp_tab with inline multiplies.
// VexRiscv M-extension mul is 1 cycle — faster than cache-competing LUT loads.
#define MULT_U8(x, y)        ((uint8_t)(((unsigned)(x) * (unsigned)(y)) >> 8))
#define LERP_U8(c1, c2, t)   ((uint8_t)((c1) + (((int)(t) * ((int)(c2) - (int)(c1))) >> 8)))
#else
// color component interpolation table:
// lerp(x, y, t) = x + (y - x) * t
// the first index is x, the second is (y - x) + 256
static uint8_t lerp_tab[256][256 * 2 + 1];
// color component multiplication table: [x][y] = (x * y) / 256;
static uint8_t mult_tab[256][256];
#define MULT_U8(x, y)        mult_tab[x][y]
#define LERP_U8(c1, c2, t)   ((c1) + lerp_tab[t][0xFF + (c2) - (c1)])
#endif
// dither kernel for unreal texture filtering
static const Vector2 dither_tab[2][2] = {
    { {{ RV_LITERAL(0.25f), RV_LITERAL(0.00f) }}, {{ RV_LITERAL(0.50f), RV_LITERAL(0.75f) }} },
    { {{ RV_LITERAL(0.75f), RV_LITERAL(0.50f) }}, {{ RV_LITERAL(0.00f), RV_LITERAL(0.25f) }} },
};

/* math shit */

static inline rv_t fclamp01(const rv_t v) {
    return (v < RV_ZERO) ? RV_ZERO : (v > RV_ONE) ? RV_ONE : v;
}

static inline uint16_t u16clamp(const int v) {
    return (v < 0) ? (uint16_t)0 : (v > 0xFFFF) ? (uint16_t)0xFFFF : (uint16_t)v;
}

static inline int iwrap0w(const int x, const int wrap) {
    return x & wrap;
}

static inline int iclamp0w(const int x, const int wrap) {
    return (x < 0) ? 0 : (x > wrap) ? wrap : x;
}

static inline int imirror0w(const int x, const int wrap) {
    return iclamp0w(abs(x), wrap); // NOTE: this is not a universal solution
}

static inline rv_t flerp(const rv_t v0, const rv_t v1, const rv_t t) {
    return v0 + RV_MUL(t, v1 - v0);
}

static inline bool vec2_cmp(const Vector2 v1, const Vector2 v2) {
    return (v1.y == v2.y) ? (v1.x > v2.x) : (v1.y > v2.y);
}

static inline Vector4 vec4_sub(const Vector4 *v1, const Vector4 *v2) {
    return (Vector4) {{ v1->x - v2->x, v1->y - v2->y, v1->z - v2->z, RV_ONE }};
}

static inline Vector4 vec4_lerp(const Vector4 *v1, const Vector4 *v2, const rv_t t) {
    return (Vector4) {{
        flerp(v1->x, v2->x, t),
        flerp(v1->y, v2->y, t),
        flerp(v1->z, v2->z, t),
        flerp(v1->w, v2->w, t),
    }};
}

static inline Color4 rgba_modulate(const Color4 c1, const Color4 c2) {
    return (Color4) {{
        .r = MULT_U8(c1.r, c2.r),
        .g = MULT_U8(c1.g, c2.g),
        .b = MULT_U8(c1.b, c2.b),
        .a = MULT_U8(c1.a, c2.a),
    }};
}

static inline Color4 rgba_blend(const Color4 src, const Color4 dst, const uint8_t a) {
    const uint8_t ia = 0xFF - a;
    return (Color4) {{
        .r = MULT_U8(src.r, a) + MULT_U8(dst.r, ia),
        .g = MULT_U8(src.g, a) + MULT_U8(dst.g, ia),
        .b = MULT_U8(src.b, a) + MULT_U8(dst.b, ia),
        .a = dst.a,
    }};
}

static inline Color4 rgba_lerp(const Color4 c1, const Color4 c2, const uint8_t t) {
    return (Color4) {{
        .r = LERP_U8(c1.r, c2.r, t),
        .g = LERP_U8(c1.g, c2.g, t),
        .b = LERP_U8(c1.b, c2.b, t),
        .a = LERP_U8(c1.a, c2.a, t),
    }};
}

static inline int imin(const int a, const int b) {
    return (a < b) ? a : b;
}

static inline int imax(const int a, const int b) {
    return (a > b) ? a : b;
}

static inline void viewport_transform(Vector4 *v) {
    // gfx_pc.c with ENABLE_SOFTRAST defined will feed us with everything already pre-multiplied by inverse of w
    v->x = RV_MUL(v->x, r_view.hw) + r_view.cx + RV_HALF;
    v->y = RV_MUL(v->y, r_view.hh) + r_view.cy + RV_HALF;
    // v->w is also already 1/w
}

/* texture sampling functions */

static inline Color4 tex_get(const struct Texture * const tex, const int x, const int y) {
    return (Color4) { .c = ((const uint32_t *)(texcache + tex->addr))[y * tex->w + x] };
}

static Color4 tex_sample_nearest_rr(const struct Texture * const tex, const int x, const int y) {
    return tex_get(tex, iwrap0w(x, tex->wrap_w), iwrap0w(y, tex->wrap_h));
}

static Color4 tex_sample_nearest_rc(const struct Texture * const tex, const int x, const int y) {
    return tex_get(tex, iwrap0w(x, tex->wrap_w), iclamp0w(y, tex->wrap_h));
}

static Color4 tex_sample_nearest_rm(const struct Texture * const tex, const int x, const int y) {
    return tex_get(tex, iwrap0w(x, tex->wrap_w), imirror0w(y, tex->wrap_h));
}

static Color4 tex_sample_nearest_cc(const struct Texture * const tex, const int x, const int y) {
    return tex_get(tex, iclamp0w(x, tex->wrap_w), iclamp0w(y, tex->wrap_h));
}

static Color4 tex_sample_nearest_cr(const struct Texture * const tex, const int x, const int y) {
    return tex_get(tex, iclamp0w(x, tex->wrap_w), iwrap0w(y, tex->wrap_h));
}

static Color4 tex_sample_nearest_cm(const struct Texture * const tex, const int x, const int y) {
    return tex_get(tex, iclamp0w(x, tex->wrap_w), imirror0w(y, tex->wrap_h));
}

static Color4 tex_sample_nearest_mm(const struct Texture * const tex, const int x, const int y) {
    return tex_get(tex, imirror0w(x, tex->wrap_w), imirror0w(y, tex->wrap_h));
}

static Color4 tex_sample_nearest_mc(const struct Texture * const tex, const int x, const int y) {
    return tex_get(tex, imirror0w(x, tex->wrap_w), iclamp0w(y, tex->wrap_h));
}

static Color4 tex_sample_nearest_mr(const struct Texture * const tex, const int x, const int y) {
    return tex_get(tex, imirror0w(x, tex->wrap_w), iwrap0w(y, tex->wrap_h));
}

static inline Color4 tex_sample_linear(const struct Texture * const tex, const rv_t u, const rv_t v, const Vector2 d) {
    const int x = RV_TO_INT(d.u + RV_MUL(u, tex->fw));
    const int y = RV_TO_INT(d.v + RV_MUL(v, tex->fh));
    return tex->sample(tex, x, y);
}

static inline Color4 tex_sample_nearest(const struct Texture * const tex, const rv_t u, const rv_t v) {
    const int x = RV_TO_INT(RV_MUL(u, tex->fw));
    const int y = RV_TO_INT(RV_MUL(v, tex->fh));
#ifdef TARGET_POCKET
    /* Fast path: inline wrap-repeat (dominant mode) to avoid fn-ptr overhead.
     * Fall back to fn-ptr for clamp/mirror textures. */
    if (__builtin_expect(tex->sample == tex_sample_nearest_rr, 1))
        return tex_get(tex, x & tex->wrap_w, y & tex->wrap_h);
#endif
    return tex->sample(tex, x, y);
}

/* color combiners */

#define tex_sample tex_sample_nearest

// Helper: convert prop * z to uint8_t (perspective-correct un-premultiply)
// Clamp to [0,255] instead of truncating — fixed-point rounding can overshoot.
static inline uint8_t pz_clamp(rv_t prop, rv_t z) {
    int v = RV_TO_INT(RV_MUL(prop, z));
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}
#define PZ(prop, z) pz_clamp((prop), (z))

static Color4 combine_rgb(const rv_t z, const rv_t *props) {
    return (Color4) {{ .r = PZ(props[0],z), .g = PZ(props[1],z), .b = PZ(props[2],z), .a = 0xFF }};
}

static Color4 combine_rgba(const rv_t z, const rv_t *props) {
    return (Color4) {{ .r = PZ(props[0],z), .g = PZ(props[1],z), .b = PZ(props[2],z), .a = PZ(props[3],z) }};
}

static Color4 combine_fog_rgb(const rv_t z, const rv_t *props) {
    const uint8_t fog = PZ(props[0],z);
    const Color4 c = (Color4) {{ .r = PZ(props[1],z), .g = PZ(props[2],z), .b = PZ(props[3],z), .a = 0xFF }};
    return rgba_blend(fog_color, c, fog);
}

static Color4 combine_fog_rgba(const rv_t z, const rv_t *props) {
    const uint8_t fog = PZ(props[0],z);
    const Color4 c = (Color4) {{ .r = PZ(props[1],z), .g = PZ(props[2],z), .b = PZ(props[3],z), .a = PZ(props[4],z) }};
    return rgba_blend(fog_color, c, fog);
}

static Color4 combine_rgba_rgba(const rv_t z, const rv_t *props) {
    const Color4 ca = (Color4) {{ .r = PZ(props[0],z), .g = PZ(props[1],z), .b = PZ(props[2],z), .a = PZ(props[3],z) }};
    const Color4 cb = (Color4) {{ .r = PZ(props[4],z), .g = PZ(props[5],z), .b = PZ(props[6],z), .a = PZ(props[7],z) }};
    return rgba_modulate(ca, cb);
}

static Color4 combine_tex(const rv_t z, const rv_t *props) {
    return tex_sample(cur_tex[0], RV_MUL(props[0],z), RV_MUL(props[1],z));
}

static Color4 combine_tex_fog(const rv_t z, const rv_t *props) {
    const Color4 tc = tex_sample(cur_tex[0], RV_MUL(props[0],z), RV_MUL(props[1],z));
    const uint8_t fog = PZ(props[2],z);
    return rgba_blend(fog_color, tc, fog);
}

static Color4 combine_tex_rgb(const rv_t z, const rv_t *props) {
    const Color4 tc = tex_sample(cur_tex[0], RV_MUL(props[0],z), RV_MUL(props[1],z));
    const Color4 cc = (Color4) {{ .r = PZ(props[2],z), .g = PZ(props[3],z), .b = PZ(props[4],z), .a = 0xFF }};
    return rgba_modulate(tc, cc);
}

static Color4 combine_tex_fog_rgb(const rv_t z, const rv_t *props) {
    const Color4 tc = tex_sample(cur_tex[0], RV_MUL(props[0],z), RV_MUL(props[1],z));
    const uint8_t fog = PZ(props[2],z);
    const Color4 cc = (Color4) {{ .r = PZ(props[3],z), .g = PZ(props[4],z), .b = PZ(props[5],z), .a = 0xFF }};
    return rgba_blend(fog_color, rgba_modulate(tc, cc), fog);
}

static Color4 combine_tex_rgb_decal(const rv_t z, const rv_t *props) {
    const Color4 tc = tex_sample(cur_tex[0], RV_MUL(props[0],z), RV_MUL(props[1],z));
    const Color4 cc = (Color4) {{ .r = PZ(props[2],z), .g = PZ(props[3],z), .b = PZ(props[4],z), .a = 0xFF }};
    return rgba_blend(tc, cc, tc.a);
}

static Color4 combine_tex_rgba(const rv_t z, const rv_t *props) {
    const Color4 tc = tex_sample(cur_tex[0], RV_MUL(props[0],z), RV_MUL(props[1],z));
    const Color4 cc = (Color4) {{ .r = PZ(props[2],z), .g = PZ(props[3],z), .b = PZ(props[4],z), .a = PZ(props[5],z) }};
    return rgba_modulate(tc, cc);
}

static Color4 combine_tex_rgba_texa(const rv_t z, const rv_t *props) {
    const Color4 tc = tex_sample(cur_tex[0], RV_MUL(props[0],z), RV_MUL(props[1],z));
    const Color4 cc = (Color4) {{ .r = PZ(props[2],z), .g = PZ(props[3],z), .b = PZ(props[4],z), .a = 0xFF }};
    return rgba_modulate(tc, cc);
}

static Color4 combine_tex_fog_rgba(const rv_t z, const rv_t *props) {
    const Color4 tc = tex_sample(cur_tex[0], RV_MUL(props[0],z), RV_MUL(props[1],z));
    const uint8_t fog = PZ(props[2],z);
    const Color4 cc = (Color4) {{ .r = PZ(props[3],z), .g = PZ(props[4],z), .b = PZ(props[5],z), .a = PZ(props[6],z) }};
    return rgba_blend(fog_color, rgba_modulate(tc, cc), fog);
}

static Color4 combine_tex_rgba_decal(const rv_t z, const rv_t *props) {
    const Color4 tc = tex_sample(cur_tex[0], RV_MUL(props[0],z), RV_MUL(props[1],z));
    const Color4 cc = (Color4) {{ .r = PZ(props[2],z), .g = PZ(props[3],z), .b = PZ(props[4],z), .a = PZ(props[5],z) }};
    return rgba_blend(tc, cc, tc.a);
}

static Color4 combine_tex_rgb_rgb(const rv_t z, const rv_t *props) {
    const Color4 tc = tex_sample(cur_tex[0], RV_MUL(props[0],z), RV_MUL(props[1],z));
    const Color4 cc1 = (Color4) {{ .r = PZ(props[2],z), .g = PZ(props[3],z), .b = PZ(props[4],z), 0xFF }};
    const Color4 cc2 = (Color4) {{ .r = PZ(props[5],z), .g = PZ(props[6],z), .b = PZ(props[7],z), 0xFF }};
    return rgba_lerp(cc2, cc1, tc.r);
}

static Color4 combine_tex_tex_rgba(const rv_t z, const rv_t *props) {
    const rv_t u = RV_MUL(props[0],z);
    const rv_t v = RV_MUL(props[1],z);
    const Color4 tc1 = tex_sample(cur_tex[0], u, v);
    const Color4 tc2 = tex_sample(cur_tex[1], u, v);
    const uint8_t r = PZ(props[2],z);
    return rgba_lerp(tc1, tc2, r);
}

/* fragment plotters */

static void draw_pixel(const int idx, UNUSED const uint16_t z, Color4 src) {
    gfx_output[idx] = src.c;
}

static void draw_pixel_zwrite(const int idx, const uint16_t z, Color4 src) {
    gfx_output[idx] = src.c;
    z_buffer[idx] = z;
}

static void draw_pixel_blend(const int idx, UNUSED const uint16_t z, Color4 src) {
    const uint8_t a = src.a;
    const uint8_t ia = 255 - a;
    const Color4 dst = (Color4) { .c = gfx_output[idx] };
    src.r = MULT_U8(src.r, a) + MULT_U8(dst.r, ia);
    src.g = MULT_U8(src.g, a) + MULT_U8(dst.g, ia);
    src.b = MULT_U8(src.b, a) + MULT_U8(dst.b, ia);
    gfx_output[idx] = src.c;
}

static void draw_pixel_blend_zwrite(const int idx, const uint16_t z, Color4 src) {
    const uint8_t a = src.a;
    const uint8_t ia = 255 - a;
    const Color4 dst = (Color4) { .c = gfx_output[idx] };
    src.r = MULT_U8(src.r, a) + MULT_U8(dst.r, ia);
    src.g = MULT_U8(src.g, a) + MULT_U8(dst.g, ia);
    src.b = MULT_U8(src.b, a) + MULT_U8(dst.b, ia);
    gfx_output[idx] = src.c;
    z_buffer[idx] = z;
}

static void draw_pixel_blend_edge(const int idx, UNUSED const uint16_t z, Color4 src) {
    if (src.a > 0x80) {
        const uint8_t a = src.a;
        const uint8_t ia = 255 - a;
        const Color4 dst = (Color4) { .c = gfx_output[idx] };
        src.r = MULT_U8(src.r, a) + MULT_U8(dst.r, ia);
        src.g = MULT_U8(src.g, a) + MULT_U8(dst.g, ia);
        src.b = MULT_U8(src.b, a) + MULT_U8(dst.b, ia);
        gfx_output[idx] = src.c;
    }
}

static void draw_pixel_blend_edge_zwrite(const int idx, const uint16_t z, Color4 src) {
    if (src.a > 0x80) {
        const uint8_t a = src.a;
        const uint8_t ia = 255 - a;
        const Color4 dst = (Color4) { .c = gfx_output[idx] };
        src.r = MULT_U8(src.r, a) + MULT_U8(dst.r, ia);
        src.g = MULT_U8(src.g, a) + MULT_U8(dst.g, ia);
        src.b = MULT_U8(src.b, a) + MULT_U8(dst.b, ia);
        gfx_output[idx] = src.c;
        z_buffer[idx] = z;
    }
}

/* rasterizers */

#define rv_div_sat(a, b) RV_DIV(a, b)
#define rv_mul_wide(a, b) RV_MUL(a, b)
#define R_COMPUTE_DENOM_AND_DP(dp_x, dp_y, v0, v1, v2, ab, ac, nprops) \
    { const rv_t _denom = RV_RCP(RV_MUL(ac.x, ab.y) - RV_MUL(ab.x, ac.y)); \
    for (i = 2; i < nprops; ++i) { \
        dp_x[i] = RV_MUL(RV_MUL(v2[i] - v0[i], ab.y) - RV_MUL(v1[i] - v0[i], ac.y), _denom); \
        dp_y[i] = RV_MUL(RV_MUL(v1[i] - v0[i], ac.x) - RV_MUL(v2[i] - v0[i], ab.x), _denom); \
    } }

#define R_RASTERIZE_TRI_SEG(y_a, y_b, nprops) \
    register int y = y_a; \
    register int y_end = y_b; \
    register int x, x_end; \
    register int idx; \
    rv_t dx, w; \
    uint16_t uz; \
    /* draw triangle segment from y_a to y_b */ \
    while (y < y_end) { \
        /* do scissor clipping */ \
        x = imax(r_clip.x0, RV_TO_INT(x_a)); \
        x_end = imin(r_clip.x1, RV_TO_INT(x_b)); \
        /* do X subpixel prestepping */ \
        dx = RV_ONE - (x_a - RV_FROM_INT(x)); \
        for (i = 2; i < nprops; ++i) p[i] = p_a[i] + RV_MUL(dx, dp_x[i]); \
        idx = scr_width * (scr_height - y - 1) + x; \
        /* draw scanline from current x_a to current x_b */ \
        while (x++ < x_end) { \
            uz = u16clamp(RV_Z_TO_ZBUF(p[2]) + z_offset); \
            if (!z_test || uz <= z_buffer[idx]) { \
                w = RV_RCP(p[3]); \
                draw_fn(idx, uz, cur_shader->combine(w, p + 4)); \
            } \
            for (i = 2; i < nprops; ++i) p[i] += dp_x[i]; \
            ++idx; \
        } \
        /* advance scanline start and end and prop starts */ \
        x_a += dxdy_a; \
        x_b += dxdy_b; \
        for (i = 2; i < nprops; ++i) p_a[i] += dpdy_a[i]; \
        ++y; \
    }

/* R_RASTERIZE_IMPL: parameterized by scanline macro SEG for specialization */
#define R_RASTERIZE_IMPL(tri, nprops, SEG) \
    const rv_t *v0 = tri.v0; \
    const rv_t *v1 = tri.v1; \
    const rv_t *v2 = tri.v2; \
    const int y0i = imax(r_clip.y0, RV_TO_INT(v0[1])); \
    const int y1i = imax(y0i, RV_TO_INT(v1[1])); \
    const int y2i = imin(r_clip.y1, RV_TO_INT(v2[1])); \
    if ((y0i == y1i && y0i == y2i) || (RV_TO_INT(v0[0]) == RV_TO_INT(v1[0]) && RV_TO_INT(v0[0]) == RV_TO_INT(v2[0]))) \
        return; /* triangle has zero area */ \
    const Vector4 ab = (Vector4) {{ v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2], v1[3] - v0[3] }}; \
    const Vector4 ac = (Vector4) {{ v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2], v2[3] - v0[3] }}; \
    const Vector2 bc = (Vector2) {{ v2[0] - v1[0], v2[1] - v1[1] }}; \
    const rv_t dxdy_ab = rv_div_sat(ab.x, ab.y); /* x increment along ab */ \
    const rv_t dxdy_ac = rv_div_sat(ac.x, ac.y); /* x increment along ac */ \
    const rv_t dxdy_bc = rv_div_sat(bc.x, bc.y); /* x increment along bc */ \
    const bool side = dxdy_ac > dxdy_ab; /* which side the longer edge (AC) is on */ \
    const rv_t y_pre0 = RV_ONE - (v0[1] - RV_FROM_INT(y0i)); /* subpixel pre-step */ \
    rv_t dpdy_a[nprops]; /* vertex prop increments along left edge */ \
    rv_t p_a[nprops]; /* vertex leftmost points */ \
    rv_t p[nprops]; /* current vertex prop values */ \
    rv_t dp_x[nprops]; /* X increments for vertex props */ \
    rv_t dp_y[nprops]; /* Y increments for vertex props */ \
    register int i; \
    /* compute property derivatives */ \
    R_COMPUTE_DENOM_AND_DP(dp_x, dp_y, v0, v1, v2, ab, ac, nprops); \
    if (!side) { \
        /* longer edge is on the left */ \
        const rv_t dxdy_a = dxdy_ac; \
        /* first column of this scanline is on AC */ \
        rv_t x_a = v0[0] + RV_MUL(y_pre0, dxdy_a); \
        for (i = 2; i < nprops; ++i) { \
            dpdy_a[i] = rv_mul_wide(dxdy_ac, dp_x[i]) + dp_y[i]; \
            p_a[i] = v0[i] + RV_MUL(y_pre0, dpdy_a[i]); \
        } \
        if (y0i < y1i) { \
            /* left is AC, right is AB */ \
            const rv_t dxdy_b = dxdy_ab; \
            /* last column of this scanline */ \
            rv_t x_b = v0[0] + RV_MUL(y_pre0, dxdy_ab); \
            SEG(y0i, y1i, nprops); \
        } \
        if (y1i < y2i) { \
            /* left is AC, right is BC */ \
            const rv_t dxdy_b = dxdy_bc; \
            /* calculate prestep for vertex B */ \
            const rv_t y_pre1 = RV_ONE - (v1[1] - RV_FROM_INT(y1i)); \
            rv_t x_b = v1[0] + RV_MUL(y_pre1, dxdy_bc); \
            SEG(y1i, y2i, nprops); \
        } \
    } else { \
        /* longer edge is on the right */ \
        const rv_t dxdy_b = dxdy_ac; \
        /* last column of this scanline is on AC */ \
        rv_t x_b = v0[0] + RV_MUL(y_pre0, dxdy_ac); \
        if (y0i < y1i) { \
            /* right is AC, left is AB */ \
            const rv_t dxdy_a = dxdy_ab; \
            rv_t x_a = v0[0] + RV_MUL(y_pre0, dxdy_a); \
            for (i = 2; i < nprops; ++i) { \
                dpdy_a[i] = rv_mul_wide(dxdy_ab, dp_x[i]) + dp_y[i]; \
                p_a[i] = v0[i] + RV_MUL(y_pre0, dpdy_a[i]); \
            } \
            SEG(y0i, y1i, nprops); \
        } \
        if (y1i < y2i) { \
            /* right is AC, left is BC */ \
            const rv_t y_pre1 = RV_ONE - (v1[1] - RV_FROM_INT(y1i)); \
            const rv_t dxdy_a = dxdy_bc; \
            rv_t x_a = v1[0] + RV_MUL(y_pre1, dxdy_a); \
            for (i = 2; i < nprops; ++i) { \
                dpdy_a[i] = rv_mul_wide(dxdy_bc, dp_x[i]) + dp_y[i]; \
                p_a[i] = v1[i] + RV_MUL(y_pre1, dpdy_a[i]); \
            } \
            SEG(y1i, y2i, nprops); \
        } \
    }

#define R_RASTERIZE(tri, nprops) R_RASTERIZE_IMPL(tri, nprops, R_RASTERIZE_TRI_SEG)

/* Forward declarations for generic rast functions (used as fallback) */
#define DECLARE_RAST_FUNC(nprops) \
    static void rast_fn_ ## nprops (const struct Tri tri);
DECLARE_RAST_FUNC(6)
DECLARE_RAST_FUNC(7)
DECLARE_RAST_FUNC(8)
DECLARE_RAST_FUNC(9)
DECLARE_RAST_FUNC(10)
DECLARE_RAST_FUNC(11)
DECLARE_RAST_FUNC(12)
DECLARE_RAST_FUNC(13)
DECLARE_RAST_FUNC(14)
#undef DECLARE_RAST_FUNC
static inline void gfx_soft_pick_draw_func(void);

/* ============================================================
 * TARGET_POCKET: Specialized scanline macros for hot shader modes.
 * These inline all three function-pointer calls (combine, tex->sample, draw_fn)
 * to eliminate ~36 cycles of per-pixel overhead.
 * ============================================================ */
#ifdef TARGET_POCKET

/* --- Variant A: combine_tex_rgb + draw_pixel_zwrite ---
 * Textured + vertex color modulate, opaque with z-write.
 * nprops = 9: XYZW(0-3) + UV(4-5) + RGB(6-8)
 * Most common SM64 shader mode (~40-50% of pixels).
 */
#define R_SEG_TEXRGB_ZWRITE(y_a, y_b, nprops) \
    register int y = y_a; \
    register int y_end = y_b; \
    register int x, x_end; \
    register int idx; \
    rv_t dx, w; \
    uint16_t uz; \
    const struct Texture *_tex = cur_tex[0]; \
    while (y < y_end) { \
        x = imax(r_clip.x0, RV_TO_INT(x_a)); \
        x_end = imin(r_clip.x1, RV_TO_INT(x_b)); \
        dx = RV_ONE - (x_a - RV_FROM_INT(x)); \
        for (i = 2; i < nprops; ++i) p[i] = p_a[i] + RV_MUL(dx, dp_x[i]); \
        idx = scr_width * (scr_height - y - 1) + x; \
        while (x++ < x_end) { \
            const int32_t uz_raw = RV_Z_TO_ZBUF(p[2]) + z_offset; \
            uz = (uz_raw < 0) ? 0 : (uz_raw > 0xFFFF) ? 0xFFFF : (uint16_t)uz_raw; \
            if (!z_test || uz <= z_buffer[idx]) { \
                w = RV_RCP(p[3]); \
                const int tx = RV_TO_INT(RV_MUL(RV_MUL(p[4], w), _tex->fw)) & _tex->wrap_w; \
                const int ty = RV_TO_INT(RV_MUL(RV_MUL(p[5], w), _tex->fh)) & _tex->wrap_h; \
                const Color4 tc = (Color4){ .c = ((const uint32_t *)(texcache + _tex->addr))[ty * _tex->w + tx] }; \
                int vr = RV_TO_INT(RV_MUL(p[6], w)); if (vr < 0) vr = 0; else if (vr > 255) vr = 255; \
                int vg = RV_TO_INT(RV_MUL(p[7], w)); if (vg < 0) vg = 0; else if (vg > 255) vg = 255; \
                int vb = RV_TO_INT(RV_MUL(p[8], w)); if (vb < 0) vb = 0; else if (vb > 255) vb = 255; \
                gfx_output[idx] = (Color4){{ .r = MULT_U8(tc.r, (uint8_t)vr), .g = MULT_U8(tc.g, (uint8_t)vg), .b = MULT_U8(tc.b, (uint8_t)vb), .a = 0xFF }}.c; \
                if (z_write) z_buffer[idx] = uz; \
            } \
            for (i = 2; i < nprops; ++i) p[i] += dp_x[i]; \
            ++idx; \
        } \
        x_a += dxdy_a; \
        x_b += dxdy_b; \
        for (i = 2; i < nprops; ++i) p_a[i] += dpdy_a[i]; \
        ++y; \
    }

static void rast_fast_texrgb_zwrite(const struct Tri tri) {
    /* Fall back to generic rasterizer for non-repeat textures */
    if (__builtin_expect(cur_tex[0]->sample != tex_sample_nearest_rr, 0)) {
        gfx_soft_pick_draw_func();
        rast_fn_9(tri);
        return;
    }
    R_RASTERIZE_IMPL(tri, 9, R_SEG_TEXRGB_ZWRITE);
}

/* --- Variant B: combine_tex_rgba + draw_pixel_blend_edge_zwrite ---
 * Textured + vertex color with alpha, edge-blended with z-write.
 * nprops = 10: XYZW(0-3) + UV(4-5) + RGBA(6-9)
 * Alpha-tested foliage, objects (~15-20% of pixels).
 */
#define R_SEG_TEXRGBA_EDGE_ZWRITE(y_a, y_b, nprops) \
    register int y = y_a; \
    register int y_end = y_b; \
    register int x, x_end; \
    register int idx; \
    rv_t dx, w; \
    uint16_t uz; \
    const struct Texture *_tex = cur_tex[0]; \
    while (y < y_end) { \
        x = imax(r_clip.x0, RV_TO_INT(x_a)); \
        x_end = imin(r_clip.x1, RV_TO_INT(x_b)); \
        dx = RV_ONE - (x_a - RV_FROM_INT(x)); \
        for (i = 2; i < nprops; ++i) p[i] = p_a[i] + RV_MUL(dx, dp_x[i]); \
        idx = scr_width * (scr_height - y - 1) + x; \
        while (x++ < x_end) { \
            const int32_t uz_raw = RV_Z_TO_ZBUF(p[2]) + z_offset; \
            uz = (uz_raw < 0) ? 0 : (uz_raw > 0xFFFF) ? 0xFFFF : (uint16_t)uz_raw; \
            if (!z_test || uz <= z_buffer[idx]) { \
                w = RV_RCP(p[3]); \
                const int tx = RV_TO_INT(RV_MUL(RV_MUL(p[4], w), _tex->fw)) & _tex->wrap_w; \
                const int ty = RV_TO_INT(RV_MUL(RV_MUL(p[5], w), _tex->fh)) & _tex->wrap_h; \
                const Color4 tc = (Color4){ .c = ((const uint32_t *)(texcache + _tex->addr))[ty * _tex->w + tx] }; \
                int vr = RV_TO_INT(RV_MUL(p[6], w)); if (vr < 0) vr = 0; else if (vr > 255) vr = 255; \
                int vg = RV_TO_INT(RV_MUL(p[7], w)); if (vg < 0) vg = 0; else if (vg > 255) vg = 255; \
                int vb = RV_TO_INT(RV_MUL(p[8], w)); if (vb < 0) vb = 0; else if (vb > 255) vb = 255; \
                int va = RV_TO_INT(RV_MUL(p[9], w)); if (va < 0) va = 0; else if (va > 255) va = 255; \
                Color4 src; \
                src.r = MULT_U8(tc.r, (uint8_t)vr); \
                src.g = MULT_U8(tc.g, (uint8_t)vg); \
                src.b = MULT_U8(tc.b, (uint8_t)vb); \
                src.a = MULT_U8(tc.a, (uint8_t)va); \
                /* blend_edge: only draw if alpha > 50% */ \
                if (src.a > 0x80) { \
                    const uint8_t a = src.a; \
                    const uint8_t ia = 255 - a; \
                    const Color4 dst = (Color4){ .c = gfx_output[idx] }; \
                    src.r = MULT_U8(src.r, a) + MULT_U8(dst.r, ia); \
                    src.g = MULT_U8(src.g, a) + MULT_U8(dst.g, ia); \
                    src.b = MULT_U8(src.b, a) + MULT_U8(dst.b, ia); \
                    gfx_output[idx] = src.c; \
                    if (z_write) z_buffer[idx] = uz; \
                } \
            } \
            for (i = 2; i < nprops; ++i) p[i] += dp_x[i]; \
            ++idx; \
        } \
        x_a += dxdy_a; \
        x_b += dxdy_b; \
        for (i = 2; i < nprops; ++i) p_a[i] += dpdy_a[i]; \
        ++y; \
    }

static void rast_fast_texrgba_edge_zwrite(const struct Tri tri) {
    if (__builtin_expect(cur_tex[0]->sample != tex_sample_nearest_rr, 0)) {
        gfx_soft_pick_draw_func();
        rast_fn_10(tri);
        return;
    }
    R_RASTERIZE_IMPL(tri, 10, R_SEG_TEXRGBA_EDGE_ZWRITE);
}

/* --- Variant C: combine_rgb + draw_pixel_zwrite ---
 * Solid vertex color, opaque with z-write. No texture.
 * nprops = 7: XYZW(0-3) + RGB(4-6)
 * Untextured surfaces, sky, some UI (~10-15% of pixels).
 */
#define R_SEG_RGB_ZWRITE(y_a, y_b, nprops) \
    register int y = y_a; \
    register int y_end = y_b; \
    register int x, x_end; \
    register int idx; \
    rv_t dx, w; \
    uint16_t uz; \
    while (y < y_end) { \
        x = imax(r_clip.x0, RV_TO_INT(x_a)); \
        x_end = imin(r_clip.x1, RV_TO_INT(x_b)); \
        dx = RV_ONE - (x_a - RV_FROM_INT(x)); \
        for (i = 2; i < nprops; ++i) p[i] = p_a[i] + RV_MUL(dx, dp_x[i]); \
        idx = scr_width * (scr_height - y - 1) + x; \
        while (x++ < x_end) { \
            const int32_t uz_raw = RV_Z_TO_ZBUF(p[2]) + z_offset; \
            uz = (uz_raw < 0) ? 0 : (uz_raw > 0xFFFF) ? 0xFFFF : (uint16_t)uz_raw; \
            if (!z_test || uz <= z_buffer[idx]) { \
                w = RV_RCP(p[3]); \
                int vr = RV_TO_INT(RV_MUL(p[4], w)); if (vr < 0) vr = 0; else if (vr > 255) vr = 255; \
                int vg = RV_TO_INT(RV_MUL(p[5], w)); if (vg < 0) vg = 0; else if (vg > 255) vg = 255; \
                int vb = RV_TO_INT(RV_MUL(p[6], w)); if (vb < 0) vb = 0; else if (vb > 255) vb = 255; \
                gfx_output[idx] = (Color4){{ .r = (uint8_t)vr, .g = (uint8_t)vg, .b = (uint8_t)vb, .a = 0xFF }}.c; \
                if (z_write) z_buffer[idx] = uz; \
            } \
            for (i = 2; i < nprops; ++i) p[i] += dp_x[i]; \
            ++idx; \
        } \
        x_a += dxdy_a; \
        x_b += dxdy_b; \
        for (i = 2; i < nprops; ++i) p_a[i] += dpdy_a[i]; \
        ++y; \
    }

static void rast_fast_rgb_zwrite(const struct Tri tri) {
    R_RASTERIZE_IMPL(tri, 7, R_SEG_RGB_ZWRITE);
}

/* --- Variant D: combine_tex + draw_pixel_zwrite ---
 * Texture only (no vertex color modulation), opaque with z-write.
 * nprops = 6: XYZW(0-3) + UV(4-5)
 */
#define R_SEG_TEX_ZWRITE(y_a, y_b, nprops) \
    register int y = y_a; \
    register int y_end = y_b; \
    register int x, x_end; \
    register int idx; \
    rv_t dx, w; \
    uint16_t uz; \
    const struct Texture *_tex = cur_tex[0]; \
    while (y < y_end) { \
        x = imax(r_clip.x0, RV_TO_INT(x_a)); \
        x_end = imin(r_clip.x1, RV_TO_INT(x_b)); \
        dx = RV_ONE - (x_a - RV_FROM_INT(x)); \
        for (i = 2; i < nprops; ++i) p[i] = p_a[i] + RV_MUL(dx, dp_x[i]); \
        idx = scr_width * (scr_height - y - 1) + x; \
        while (x++ < x_end) { \
            const int32_t uz_raw = RV_Z_TO_ZBUF(p[2]) + z_offset; \
            uz = (uz_raw < 0) ? 0 : (uz_raw > 0xFFFF) ? 0xFFFF : (uint16_t)uz_raw; \
            if (!z_test || uz <= z_buffer[idx]) { \
                w = RV_RCP(p[3]); \
                const int tx = RV_TO_INT(RV_MUL(RV_MUL(p[4], w), _tex->fw)) & _tex->wrap_w; \
                const int ty = RV_TO_INT(RV_MUL(RV_MUL(p[5], w), _tex->fh)) & _tex->wrap_h; \
                gfx_output[idx] = ((const uint32_t *)(texcache + _tex->addr))[ty * _tex->w + tx]; \
                if (z_write) z_buffer[idx] = uz; \
            } \
            for (i = 2; i < nprops; ++i) p[i] += dp_x[i]; \
            ++idx; \
        } \
        x_a += dxdy_a; \
        x_b += dxdy_b; \
        for (i = 2; i < nprops; ++i) p_a[i] += dpdy_a[i]; \
        ++y; \
    }

static void rast_fast_tex_zwrite(const struct Tri tri) {
    if (__builtin_expect(cur_tex[0]->sample != tex_sample_nearest_rr, 0)) {
        gfx_soft_pick_draw_func();
        rast_fn_6(tri);
        return;
    }
    R_RASTERIZE_IMPL(tri, 6, R_SEG_TEX_ZWRITE);
}

#endif /* TARGET_POCKET */

// define a bunch of rasterizers/interpolators for known property counts
// nprops includes XYZW

#define DEFINE_RAST_FUNC(nprops) \
    static void rast_fn_ ## nprops (const struct Tri tri) { R_RASTERIZE(tri, nprops); }

#define GET_RAST_FUNC(nprops) rast_fn_ ## nprops

DEFINE_RAST_FUNC(6)
DEFINE_RAST_FUNC(7)
DEFINE_RAST_FUNC(8)
DEFINE_RAST_FUNC(9)
DEFINE_RAST_FUNC(10)
DEFINE_RAST_FUNC(11)
DEFINE_RAST_FUNC(12)
DEFINE_RAST_FUNC(13)
DEFINE_RAST_FUNC(14)

static inline void pop_triangle(rv_t *buf, const int stride) {
    Vector4 *v0 = (Vector4 *)buf;
    Vector4 *v1 = (Vector4 *)(buf + stride);
    Vector4 *v2 = (Vector4 *)(buf + (stride << 1));
    Vector4 *vt;

    // the vertices come to us in clip space, but already divided by w, still gotta transform
    viewport_transform(v0);
    viewport_transform(v1);
    viewport_transform(v2);

    // sort in Y order
    if (v0->y > v1->y) { vt = v0; v0 = v1; v1 = vt; }
    if (v0->y > v2->y) { vt = v0; v0 = v2; v2 = vt; }
    if (v1->y > v2->y) { vt = v1; v1 = v2; v2 = vt; }

    const struct Tri out = (struct Tri) { (rv_t *)v0, (rv_t *)v1, (rv_t *)v2 };
    cur_shader->rast(out);
}

static inline void depth_clear(void) {
    memset(z_buffer, 0xFF, scr_size << 1);
}

static inline void color_clear(void) {
    memset(gfx_output, 0x00, scr_size << 2);
}

/* FIXME: ztrick fucks with sky blending
static inline void depth_swap(void) {
    ++z_frame;
    if (z_frame & 1) {
        r_view.zn = 0.f;
        r_view.zf = 0.4999f;
        z_reverse = false;
    } else {
        r_view.zn = 1.f;
        r_view.zf = 0.5f;
        z_reverse = true;
    }
    r_view.hz = (r_view.zf - r_view.zn) * 0.5f;
    r_view.cz = (r_view.zn + r_view.zf) * 0.5f;
}
*/

/* interface */

static bool gfx_soft_z_is_from_0_to_1(void) {
    return true;
}

static void gfx_soft_unload_shader(struct ShaderProgram *old_prg) {
    if (cur_shader && (cur_shader == old_prg || !old_prg))
        cur_shader = NULL;
}

static void gfx_soft_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
}

static struct ShaderProgram *gfx_soft_create_and_load_new_shader(uint32_t shader_id) {
    static const rast_fn_t rast_funcs[] = {
        NULL,
        NULL,
        GET_RAST_FUNC(6),
        GET_RAST_FUNC(7),
        GET_RAST_FUNC(8),
        GET_RAST_FUNC(9),
        GET_RAST_FUNC(10),
        GET_RAST_FUNC(11),
        GET_RAST_FUNC(12),
        GET_RAST_FUNC(13),
        GET_RAST_FUNC(14),
    };

    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];

    prg->shader_id = shader_id;
    prg->cc = ccf;

    int num_props = 0;

    if (ccf.opt_fog) num_props++; // software renderer only gets fog intensity

    num_props += ccf.num_inputs * (ccf.opt_alpha ? 4 : 3);
    num_props += ccf.used_textures[0] * 2;

    if (ccf.used_textures[0] && ccf.used_textures[1]) {
        prg->mix = SH_MT_TEXTURE_TEXTURE;
        prg->combine = combine_tex_tex_rgba; // only one such known shader
    } else if (ccf.used_textures[0] && ccf.num_inputs) {
        prg->mix = SH_MT_TEXTURE_COLOR;
        if (ccf.num_inputs > 1)
            prg->combine = combine_tex_rgb_rgb; // only one such known shader
        else if (shader_id == 0x0000038D || shader_id == 0x01200A00 || shader_id == 0x01045A00 || shader_id == 0x0120038D)
            prg->combine = ccf.opt_alpha ? combine_tex_rgba_decal : combine_tex_rgb_decal;
        else if (ccf.opt_fog)
            prg->combine = ccf.opt_alpha ? combine_tex_fog_rgba : combine_tex_fog_rgb;
        else if (ccf.opt_alpha)
            prg->combine = shader_id == 0x01A00045 ? combine_tex_rgba_texa : combine_tex_rgba;
        else
            prg->combine = combine_tex_rgb;
    } else if (ccf.used_textures[0]) {
        prg->mix = SH_MT_TEXTURE;
        prg->combine = ccf.opt_fog ? combine_tex_fog : combine_tex;
    } else if (ccf.num_inputs > 1) {
        prg->mix = SH_MT_COLOR_COLOR;
        prg->combine = combine_rgba_rgba; // only one such known shader
    } else if (ccf.num_inputs) {
        prg->mix = SH_MT_COLOR;
        if (ccf.opt_fog)
            prg->combine = ccf.opt_alpha ? combine_fog_rgba : combine_fog_rgb;
        else
            prg->combine = ccf.opt_alpha ? combine_rgba : combine_rgb;
    }

    if (ccf.opt_alpha) {
        if (ccf.opt_texture_edge)
            prg->draw_flags = DRAW_BLEND_EDGE;
        else
            prg->draw_flags = DRAW_BLEND;
    } else {
        prg->draw_flags = 0;
    }

    prg->num_props = num_props;

    // pick rasterizer that interps the amount of float properties this shader requires
    prg->rast = rast_funcs[num_props];

#ifdef TARGET_POCKET
    // Specialized fast-path rasterizers: inline combine+sample+draw to eliminate
    // ~36 cycles of per-pixel function-pointer overhead.
    if (prg->draw_flags == 0) {
        // Opaque (no blend)
        if (prg->mix == SH_MT_TEXTURE_COLOR && !ccf.opt_fog && !ccf.opt_alpha && ccf.num_inputs == 1)
            prg->rast = rast_fast_texrgb_zwrite;      // tex+rgb, 9 props
        else if (prg->mix == SH_MT_TEXTURE && !ccf.opt_fog)
            prg->rast = rast_fast_tex_zwrite;          // tex only, 6 props
        else if (prg->mix == SH_MT_COLOR && !ccf.opt_fog && !ccf.opt_alpha)
            prg->rast = rast_fast_rgb_zwrite;          // rgb only, 7 props
    } else if (prg->draw_flags == DRAW_BLEND_EDGE) {
        if (prg->mix == SH_MT_TEXTURE_COLOR && !ccf.opt_fog && ccf.opt_alpha
            && ccf.num_inputs == 1 && prg->combine == combine_tex_rgba)
            prg->rast = rast_fast_texrgba_edge_zwrite; // tex+rgba+edge, 10 props
    }
#endif

    gfx_soft_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_soft_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++)
        if (shader_program_pool[i].shader_id == shader_id)
            return &shader_program_pool[i];
    return NULL;
}

static void gfx_soft_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->cc.num_inputs;
    used_textures[0] = prg->cc.used_textures[0];
    used_textures[1] = prg->cc.used_textures[1];
}

static uint32_t gfx_soft_new_texture(void) {
    const uint32_t id = tex_num++;

    if (tex_num > MAX_TEXTURES) {
        printf("gfx_soft: ran out of texture slots\n");
        abort();
    }

    tex_hdr[id].sample = tex_sample_nearest_rr;

    return id;
}

static void gfx_soft_select_texture(int tile, uint32_t texture_id) {
    cur_tex[tile] = tex_hdr + texture_id;
    cur_tmu = tile;
}

static uint32_t tex_cache_alloc(const uint32_t w, const uint32_t h) {
    const uint32_t size = w * h * 4;

    if (texcache_addr + size > texcache_size) {
        texcache_size += TEXCACHE_STEP + size;
        texcache_size = ALIGN(texcache_size, TEXCACHE_STEP);
        texcache = realloc(texcache, texcache_size);
        if (!texcache) {
            printf("gfx_soft: could not alloc %u bytes for texture cache\n", texcache_size);
            abort();
        }
    }

    uint32_t ret = texcache_addr;
    texcache_addr += size;
    return ret;
}

static void gfx_soft_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    uint32_t addr = tex_cache_alloc(width, height);
    memcpy(texcache + addr, rgba32_buf, width * height * 4);
    struct Texture *tex = cur_tex[cur_tmu];
    tex->addr = addr;
    tex->w = width;
    tex->h = height;
    tex->wrap_w = width - 1;
    tex->wrap_h = height - 1;
    tex->fw = RV_FROM_INT(tex->w);
    tex->fh = RV_FROM_INT(tex->h);
}

static inline int gfx_cm_to_local(uint32_t val) {
    if (val & G_TX_CLAMP) return WRAP_CLAMP;
    return (val & G_TX_MIRROR) ? WRAP_MIRROR : WRAP_REPEAT;
}

static void gfx_soft_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    static const sample_fn_t samplers[] = {
        tex_sample_nearest_rr, // 0000
        tex_sample_nearest_rc, // 0001
        tex_sample_nearest_rm, // 0010
        NULL,
        tex_sample_nearest_cr, // 0100
        tex_sample_nearest_cc, // 0101
        tex_sample_nearest_cm, // 0110
        NULL,
        tex_sample_nearest_mr, // 1000
        tex_sample_nearest_mc, // 1001
        tex_sample_nearest_mm, // 1010
    };

    cms = gfx_cm_to_local(cms) << 2;
    cmt = gfx_cm_to_local(cmt);

    cur_tex[tile]->filter = linear_filter;
    cur_tex[tile]->sample = samplers[cms | cmt];
}

static void gfx_soft_set_depth_test(bool depth_test) {
    z_test = depth_test;
}

static void gfx_soft_set_depth_mask(bool z_upd) {
    z_write = z_upd;
}

static void gfx_soft_set_zmode_decal(bool zmode_decal) {
    z_offset = zmode_decal ? -32 : 0;
}

static void gfx_soft_set_viewport(int x, int y, int width, int height) {
    r_view.x = x;
    r_view.y = y;
    r_view.w = width;
    r_view.h = height;
    r_view.hw = RV_FROM_INT(width >> 1);
    r_view.hh = RV_FROM_INT(height >> 1);
    r_view.cx = RV_FROM_INT(x) + r_view.hw;
    r_view.cy = RV_FROM_INT(y) + r_view.hh;
}

static void gfx_soft_set_scissor(int x, int y, int width, int height) {
    r_clip.x0 = x;
    r_clip.y0 = y;
    r_clip.x1 = x + width;
    r_clip.y1 = y + height;
}

static void gfx_soft_set_use_alpha(bool use_alpha) {
    do_blend = use_alpha;
}

static void gfx_soft_set_fog_color(const uint8_t *rgb) {
    fog_color.r = rgb[0];
    fog_color.g = rgb[1];
    fog_color.b = rgb[2];
    fog_color.a = 0xFF;
}

static inline void gfx_soft_pick_draw_func(void) {
    static const draw_fn_t draw_funcs[] = {
        draw_pixel,
        draw_pixel_zwrite,
        draw_pixel_blend,
        draw_pixel_blend_zwrite,
        draw_pixel_blend_edge,
        draw_pixel_blend_edge_zwrite,
    };
    draw_fn = draw_funcs[cur_shader->draw_flags | z_write];
}

static void gfx_soft_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    gfx_soft_pick_draw_func();
    const size_t num_verts = 3 * buf_vbo_num_tris;
    const size_t stride = buf_vbo_len / num_verts;
    for (size_t i = 0; i < num_verts * stride; i += 3 * stride)
        pop_triangle(buf_vbo + i, stride);
}

static void gfx_soft_fill_rect(int x0, int y0, int x1, int y1, const uint8_t *rgba) {
    // HACK: these are mainly used just to clear the screen and draw simple rects, so we ignore drawmode stuff and Z
    x0 = imax(0, x0);
    y0 = imax(0, y0);
    x1 = imin(scr_width, x1);
    y1 = imin(scr_height, y1);
    register const uint32_t color = *(uint32_t *)rgba;
    register uint32_t *base = gfx_output + y0 * scr_width + x0;
    register uint32_t *p;
    register int x, y;
    for (y = y0; y < y1; ++y, base += scr_width) {
        p = base;
        for (x = x0; x < x1; ++x, ++p)
            *p = color;
    }
}

static inline void gfx_soft_tex_rect_replace(int x0, int y0, int x1, int y1, const rv_t u0, const rv_t v0, const rv_t dudx, const rv_t dvdy) {
    register int base = y0 * scr_width + x0;
    register int idx;
    register int x, y;
    rv_t u;
    rv_t v = v0;
    for (y = y0; y < y1; ++y, base += scr_width, v += dvdy) {
        idx = base;
        u = u0;
        for (x = x0; x < x1; ++x, ++idx, u += dudx)
            draw_fn(idx, 0, cur_tex[0]->sample(cur_tex[0], RV_TO_INT(u), RV_TO_INT(v)));
    }
}

static inline void gfx_soft_tex_rect_modulate(int x0, int y0, int x1, int y1, const rv_t u0, const rv_t v0, const rv_t dudx, const rv_t dvdy, const Color4 rgba) {
    register int base = y0 * scr_width + x0;
    register int idx;
    register int x, y;
    rv_t u;
    rv_t v = v0;
    for (y = y0; y < y1; ++y, base += scr_width, v += dvdy) {
        idx = base;
        u = u0;
        for (x = x0; x < x1; ++x, ++idx, u += dudx)
            draw_fn(idx, 0, rgba_modulate(cur_tex[0]->sample(cur_tex[0], RV_TO_INT(u), RV_TO_INT(v)), rgba));
    }
}

static void gfx_soft_tex_rect(int x0, int y0, int x1, int y1, const float u0, const float v0, const float dudx, const float dvdy, const uint8_t *rgba) {
    x0 = imax(0, x0);
    y0 = imax(0, y0);
    x1 = imin(scr_width, x1);
    y1 = imin(scr_height, y1);
    gfx_soft_pick_draw_func();
    if (cur_shader->cc.num_inputs)
        gfx_soft_tex_rect_modulate(x0, y0, x1, y1, u0, v0, dudx, dvdy, *(Color4 *)rgba);
    else
        gfx_soft_tex_rect_replace(x0, y0, x1, y1, u0, v0, dudx, dvdy);
}

static void gfx_soft_prepare_tables(void) {
#ifndef TARGET_POCKET
    for (int t = 0; t < 0x100; ++t) {
        for (int i = 0, sum = 0; i < 0x100; ++i, sum += t) {
            lerp_tab[t][0xFF - i] = (uint8_t)(-sum >> 8);
            lerp_tab[t][0xFF + i] = (uint8_t)( sum >> 8);
        }
    }

    for (int x = 0; x < 0x100; ++x)
        for (int y = 0; y < 0x100; ++y)
            mult_tab[x][y] = (x * y) >> 8;
#endif
}

static void gfx_soft_set_resolution(const int width, const int height) {
    if (z_buffer) free(z_buffer);
    if (gfx_output) free(gfx_output);

    scr_width = width;
    scr_height = height;
    scr_size = scr_width * scr_height;

    z_buffer = calloc(scr_width * scr_height, sizeof(int16_t));
    if (!z_buffer) {
        printf("gfx_soft: could not alloc zbuffer for %dx%d\n", scr_width, scr_height);
        abort();
    }

    gfx_output = calloc(scr_width * scr_height, sizeof(uint32_t));
    if (!gfx_output) {
        printf("gfx_soft: could not alloc color buffer for %dx%d\n", scr_width, scr_height);
        abort();
    }

    depth_clear();
}

static void gfx_soft_init(void) {
    texcache = calloc(1, TEXCACHE_STEP); // this will be realloc'd as needed
    texcache_size = TEXCACHE_STEP;
    texcache_addr = 0;
    if (!texcache) {
        printf("gfx_soft: could not alloc %u bytes for texture cache\n", TEXCACHE_STEP);
        abort();
    }

    z_test = true;
    z_write = true;
    do_blend = false;
    do_clip = false;

    gfx_soft_prepare_tables();

    gfx_soft_set_resolution(gfx_current_dimensions.width, gfx_current_dimensions.height);
}

static void gfx_soft_start_frame(void) {
    // depth_swap(); // FIXME: ztrick
    color_clear();
    depth_clear();
}

static void gfx_soft_shutdown(void) {
    free(z_buffer);
    free(texcache);
}

static void gfx_soft_on_resize(void) {
}

static void gfx_soft_end_frame(void) {
}

static void gfx_soft_finish_render(void) {
}

struct GfxRenderingAPI gfx_soft_api = {
    gfx_soft_z_is_from_0_to_1,
    gfx_soft_unload_shader,
    gfx_soft_load_shader,
    gfx_soft_create_and_load_new_shader,
    gfx_soft_lookup_shader,
    gfx_soft_shader_get_info,
    gfx_soft_new_texture,
    gfx_soft_select_texture,
    gfx_soft_upload_texture,
    gfx_soft_set_sampler_parameters,
    gfx_soft_set_depth_test,
    gfx_soft_set_depth_mask,
    gfx_soft_set_zmode_decal,
    gfx_soft_set_viewport,
    gfx_soft_set_scissor,
    gfx_soft_set_use_alpha,
    gfx_soft_draw_triangles,
    gfx_soft_init,
    gfx_soft_on_resize,
    gfx_soft_start_frame,
    gfx_soft_end_frame,
    gfx_soft_finish_render,
    gfx_soft_fill_rect,
    gfx_soft_tex_rect,
    gfx_soft_set_fog_color,
    gfx_soft_shutdown,
};

#endif // ENABLE_OPENGL_LEGACY
