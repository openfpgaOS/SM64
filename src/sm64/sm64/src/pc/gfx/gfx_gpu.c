/*
 * gfx_gpu.c -- openfpgaOS (os30) hardware GPU rendering backend for SM64.
 *
 * Replaces the software rasterizer with the os30 hardware vertex-triangle
 * path.  The CPU only transforms/projects vertices (already done by
 * gfx_pc.c into buf_vbo); this backend converts each vertex to the GPU's
 * fixed-point format and streams whole triangles via of_gpu_draw_vert_tri.
 * The GPU edge-walks, perspective-divides, samples a CI8 texture, shades
 * through the palookup colormap, and depth-tests against an SDRAM z-buffer.
 *
 * Two rendering models, chosen at boot by of_has_feature(OF_HW_GPU_VCOLOR):
 *
 *   TRUECOLOR (os30 with INCLUDE_DIRECT_COLOR — the preferred path):
 *     - framebuffer: 16-bit RGB565 direct colour
 *     - textures:    RGBA32 -> RGB565; a<0x80 -> 0x0000 (transparent)
 *     - shading:     RGB565 texel * per-vertex brightness (interpolated light,
 *                    OF_GPU_SPAN_TRUECOLOR); untextured -> 1x1 white texel
 *                    (vertex hue currently collapses to brightness — full
 *                    per-vertex RGB Gouraud would need the derive RGB planes)
 *
 *   PALETTIZED (fallback on cores without OF_HW_GPU_VCOLOR):
 *     - framebuffer: 8-bit indexed, palette == RGB332 (index encodes colour)
 *     - textures:    RGBA32 -> CI8 (nearest RGB332 index); a<0x80 -> index 0
 *     - shading:     per-vertex luminance -> palookup shade row (0..63)
 *     - untextured:  256x1 identity ramp carries the vertex colour via s
 *
 * of_gpu.h holds static ring state and MUST be included from exactly one
 * TU — this is that TU.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "of.h"
#include "of_caps.h"
#include "of_cache.h"
#include "of_gpu.h"
#include "of_video.h"

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_gpu.h"

#define SCR_W       320
#define SCR_H       240
#define FB_STRIDE   SCR_W
#define CMAP_ROWS   64
#define ZBUF_ELEM   2            /* 16-bit z entries */
#define RAMP_W      256          /* identity ramp for untextured tris */

/* The HW vert-tri derive forms s*zi in Q16.16, fits an affine plane per
 * attribute (du = sat32((num*rdet) >>> 44), Q16.16 gradient), and per-pixel
 * perspective-divides texel = (s*zi)/zi.  The plane gradient carries a FIXED
 * 1/65536 ULP, but rdet's reciprocal scale (2^44) is magnitude-independent, so
 * the number of SIGNIFICANT gradient bits that survive the >>>44 grows with the
 * magnitude of zi/szi.  SM64's large world units make raw 1/w land at only
 * ~3..655 in Q16.16 — a starved magnitude, so each of two edge-adjacent
 * triangles re-derives its own coarse plane with its own rounding/anchor and
 * the two reconstructions disagree by the residual along the shared diagonal:
 * the faint perspective seam.
 *
 * Fix: scale zi up so the derived planes use the full Q16.16 window.  The scale
 * CANCELS in (s*zi)/zi (texel = interp(u/w)/interp(1/w)), so it is free to vary
 * PER TRIANGLE — adjacent triangles with different scales still reconstruct the
 * identical shared-edge texel, each just nearer the true value.  Rather than one
 * global constant (which underflows on far surfaces and overflows on near/tiled
 * ones), gpu_draw_triangles picks a per-triangle zscale that lands the largest
 * derived attribute (szi/tzi/zi) at ZI_DERIVE_TARGET.  Depth is decoupled
 * (depth[] = 1/w*2^30, GPU reads sp_depth_value not zi), so this never touches
 * the z-buffer.  ZI_SCALE is the fallback for degenerate (zero-magnitude) tris. */
#define ZI_SCALE    64

/* Peak |value| (NOT raw Q16.16) the per-triangle scale targets for the largest
 * of szi/tzi/zi.  szi value = (u/w)*zscale; with all three attrs held <= 8192
 * the raw Q16.16 stays ~2^29 — comfortably inside signed int32 AND below the
 * derive's int32 saturation of the per-attribute difference da = a_k - a0
 * (deriv_sat33 in gpu_core.v, which needs the value range < 16384).  8192 keeps
 * ~1 bit of headroom; pushing toward 16384 buys only one more bit. */
#define ZI_DERIVE_TARGET    8192.0f

/* Decal (G_ZMODE_DEC) depth bias toward the camera, as a multiplier on the
 * decoupled depth df = (1/w)*2^30.  The GPU float-compresses df to a 16-bit code
 * (5-bit exp + 11-bit mantissa, ~0.05% per code), so this multiplier is a
 * ~constant code offset of ~(BIAS-1)*2048 codes regardless of distance.
 *
 * Shadows/signs/paintings are decals meant to sit ON a surface and win its
 * z-test.  SM64's tree shadows are 9-vertex TERRAIN-FOLLOWING discs overlaid on
 * the coarser ground mesh, so a flat shadow triangle dips below a curved ground
 * triangle by a depth that varies with camera distance/angle — and since the
 * offset is near-uniform across the (near-coplanar) shadow, the WHOLE shadow
 * flips visible/hidden ("pops").  1.003 (~6 codes / ~0.3%) was too weak to cover
 * those dips.  TUNE on hardware: raise if shadows still pop; lower if a decal
 * pokes through geometry just in front of it (e.g. the shadow over Mario's
 * feet).  The principled fix is an RTL N64-style decal tolerance band, but this
 * constant is app-only (fast: make + make copy + cold boot, no bitstream). */
#define DECAL_Z_BIAS        1.012f

/* Slope-scaled decal bias (glPolygonOffset "factor" / N64 dz term): push the
 * decal toward the camera by this many pixels' worth of the per-pixel depth
 * GRADIENT, on top of the flat DECAL_Z_BIAS floor.  A flat-on decal (a sign, a
 * painting frame, Mario's shadow under an overhead camera) has a tiny gradient
 * so the floor dominates and it's stable.  A ground decal viewed at a GRAZING
 * angle (tree/object shadows on sloped or distant terrain) has a large per-pixel
 * depth change, so the bias auto-scales up to match — which a flat constant
 * (any DECAL_Z_BIAS) cannot, and is why bumping the constant didn't stop the
 * shadows popping.  TUNE on hardware: raise if shadows still pop at grazing
 * angles; lower if a decal lifts off / pokes through at steep angles. */
#define DECAL_SLOPE_PIXELS  4.0f

#define MAX_TEX     4096
#define MAX_SHADERS 64

/* ================================================================
 * RGB332 helpers (palette index == colour)
 * ================================================================ */

static inline uint8_t rgb332(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint8_t)((r & 0xE0) | ((g >> 3) & 0x1C) | (b >> 6));
}

static inline void rgb332_expand(uint8_t i, int *r, int *g, int *b) {
    int r3 = (i >> 5) & 7, g3 = (i >> 2) & 7, b2 = i & 3;
    *r = (r3 << 5) | (r3 << 2) | (r3 >> 1);
    *g = (g3 << 5) | (g3 << 2) | (g3 >> 1);
    *b = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2;
}

static inline uint16_t rgba_to_565(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ================================================================
 * State
 * ================================================================ */

struct GTex {
    uint8_t  *ci8;          /* current pixel buffer = slot[cur]; NULL until first upload */
    uint8_t  *slot[2];      /* double-buffered store: a re-upload lands in the OTHER slot */
    uint32_t  slotcap[2];   /* allocated bytes per slot */
    uint8_t   cur;          /* which slot ci8 / addr currently point at */
    uint16_t  w, h;
    uint16_t  wmask, hmask;
    uint32_t  addr;         /* CPU/AXI byte address the GPU samples from */
    uint8_t   cms, cmt;     /* N64 wrap mode (G_TX_MIRROR=1 / G_TX_CLAMP=2) per axis */
};

struct ShaderProgram {
    uint32_t shader_id;
    struct CCFeatures cc;
    int stride;             /* floats per vertex in buf_vbo */
    int coff;               /* float offset of first colour input (RGB) */
    int used;
};

static struct GTex   g_tex[MAX_TEX];
static int           g_tex_count;
static struct GTex  *g_cur_tex[2];
static int           g_cur_tmu;

static struct ShaderProgram g_shaders[MAX_SHADERS];
static int           g_shader_count;
static struct ShaderProgram *g_cur_shader;

static uint8_t       g_colormap[CMAP_ROWS * 256];
static uint8_t      *g_ramp;            /* RAMP_W x1 identity texture */
static uint32_t      g_ramp_addr;
static uint16_t     *g_zbuf;

static int           g_draw_idx;
static uint32_t      g_draw_fb;
static int           g_fb_active;
static int           g_has_gpu;
static int           g_truecolor;       /* RGB565 direct-color path (OF_HW_GPU_VCOLOR) */
static int           g_fb_bpp = 1;      /* framebuffer bytes/pixel (1 CI8, 2 RGB565) */
static uint16_t      g_white_tex;       /* 1x1 white RGB565 for untextured truecolor */
static uint32_t      g_white_addr;

/* viewport (NDC -> screen), set by set_viewport — mirrors gfx_soft */
static float g_cx = SCR_W * 0.5f, g_cy = SCR_H * 0.5f;
static float g_hw = SCR_W * 0.5f, g_hh = SCR_H * 0.5f;

/* scissor / depth state */
static int16_t g_clip_x0, g_clip_y0, g_clip_x1 = SCR_W, g_clip_y1 = SCR_H;
static int g_z_test, g_z_write;

/* sticky-state dedup: skip re-emitting an identical SET_TRI_STATE. */
static of_gpu_tri_state_t g_st_cache;
static int g_st_cache_valid;

/* ================================================================
 * Boot: palette, colormap, ramp, z-buffer, first draw buffer
 * ================================================================ */

static void build_palette(void) {
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        rgb332_expand((uint8_t)i, &r, &g, &b);
        of_video_palette((uint8_t)i, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    }
}

/* row 0 = brightest (identity), row 63 = darkest. */
static void build_colormap(void) {
    for (int row = 0; row < CMAP_ROWS; row++) {
        float s = (float)(CMAP_ROWS - 1 - row) / (float)(CMAP_ROWS - 1);
        for (int c = 0; c < 256; c++) {
            int r, g, b;
            rgb332_expand((uint8_t)c, &r, &g, &b);
            g_colormap[row * 256 + c] =
                rgb332((int)(r * s), (int)(g * s), (int)(b * s));
        }
    }
}

void gfx_gpu_boot(void) {
    const struct of_capabilities *caps = of_get_caps();
    g_has_gpu = caps && caps->gpu_base &&
                (caps->hw_features & OF_HW_GPU_SPAN) &&
                of_has_feature(OF_HW_GPU_VERT_TRI);
    /* Truecolor (RGB565 direct color) when the os30 GPU advertises it. */
    g_truecolor = g_has_gpu && of_has_feature(OF_HW_GPU_VCOLOR);
    /* Always set the RGB332 palette: harmless under RGB565 scanout (ignored),
     * and the safety net if the RGB565 mode switch is rejected below. */
    build_palette();

    if (g_has_gpu) {
        of_gpu_init();

        if (g_truecolor) {
            /* Direct color needs the scanout in RGB565.  Request it AND verify
             * it took effect — if the kernel rejects it (older os.bin), fall
             * back to the palettized path instead of writing 16-bit pixels into
             * an 8-bit framebuffer (which would render as black/garbage). */
            of_video_mode_t want, norm, cur;
            memset(&want, 0, sizeof(want));
            want.width = SCR_W; want.height = SCR_H;
            want.color_mode = OF_VIDEO_MODE_RGB565;
            if (of_video_check_mode(&want, &norm) == 0)
                of_video_set_mode(&norm);
            of_video_get_mode(&cur);
            if (cur.color_mode == OF_VIDEO_MODE_RGB565) {
                g_fb_bpp = 2;
                g_white_tex = 0xFFFF;   /* 1x1 white texel for untextured tris */
                of_cache_flush_range(&g_white_tex, sizeof(g_white_tex));
                g_white_addr = (uint32_t)(uintptr_t)&g_white_tex;
            } else {
                g_truecolor = 0;        /* RGB565 unavailable → palettized */
            }
        }

        if (!g_truecolor) {
            build_colormap();
            of_gpu_palookup_upload(0, g_colormap, sizeof(g_colormap));

            /* identity ramp: texel[i] == i, so an untextured tri can carry a
             * per-vertex RGB332 colour index through its s coordinate. */
            g_ramp = malloc(RAMP_W);
            for (int i = 0; i < RAMP_W; i++) g_ramp[i] = (uint8_t)i;
            of_cache_flush_range(g_ramp, RAMP_W);
            g_ramp_addr = (uint32_t)(uintptr_t)g_ramp;
        }

        g_zbuf = malloc((size_t)SCR_W * SCR_H * ZBUF_ELEM);
        memset(g_zbuf, 0, (size_t)SCR_W * SCR_H * ZBUF_ELEM);
        of_cache_flush_range(g_zbuf, (uint32_t)((size_t)SCR_W * SCR_H * ZBUF_ELEM));

        g_draw_idx = of_video_acquire_next(-1, 0);
        g_draw_fb = (uint32_t)(uintptr_t)of_video_buffer_addr(g_draw_idx);
    }
}

void gfx_gpu_present(void) {
    if (!g_has_gpu) {
        of_video_flip();
        return;
    }
    uint32_t token = of_gpu_flip_to(g_draw_idx);
    of_gpu_kick();
    g_draw_idx = of_video_acquire_next(g_draw_idx, token);
    g_draw_fb = (uint32_t)(uintptr_t)of_video_buffer_addr(g_draw_idx);
    if (!g_fb_active) {
        g_fb_active = 1;
        of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
    }
}

/* ================================================================
 * Shaders
 * ================================================================ */

static int shader_stride(const struct CCFeatures *cc) {
    int s = 4;                                  /* x,y,z,1/w */
    if (cc->used_textures[0]) s += 2;           /* u,v */
    if (cc->opt_fog) s += 1;                    /* fog intensity */
    s += cc->num_inputs * (cc->opt_alpha ? 4 : 3);
    return s;
}

static struct ShaderProgram *gpu_create_and_load_new_shader(uint32_t shader_id) {
    struct ShaderProgram *p = &g_shaders[g_shader_count++ % MAX_SHADERS];
    memset(p, 0, sizeof(*p));
    p->shader_id = shader_id;
    gfx_cc_get_features(shader_id, &p->cc);
    p->stride = shader_stride(&p->cc);
    p->coff = 4 + (p->cc.used_textures[0] ? 2 : 0) + (p->cc.opt_fog ? 1 : 0);
    p->used = 1;
    g_cur_shader = p;
    return p;
}

static struct ShaderProgram *gpu_lookup_shader(uint32_t shader_id) {
    for (int i = 0; i < g_shader_count && i < MAX_SHADERS; i++)
        if (g_shaders[i].used && g_shaders[i].shader_id == shader_id)
            return &g_shaders[i];
    return NULL;
}

static void gpu_load_shader(struct ShaderProgram *prg)  { g_cur_shader = prg; }
static void gpu_unload_shader(struct ShaderProgram *old) { (void)old; g_cur_shader = NULL; }

static void gpu_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs,
                                bool used_textures[2]) {
    *num_inputs = (uint8_t)prg->cc.num_inputs;
    used_textures[0] = prg->cc.used_textures[0];
    used_textures[1] = prg->cc.used_textures[1];
}

static bool gpu_z_is_from_0_to_1(void) { return true; }

/* ================================================================
 * Textures (RGBA32 -> CI8)
 * ================================================================ */

static uint32_t gpu_new_texture(void) {
    int id = g_tex_count++;
    if (id >= MAX_TEX) id = MAX_TEX - 1;
    return (uint32_t)id;
}

static void gpu_select_texture(int tile, uint32_t texture_id) {
    if (texture_id >= MAX_TEX) texture_id = MAX_TEX - 1;
    g_cur_tmu = tile & 1;
    g_cur_tex[g_cur_tmu] = &g_tex[texture_id];
}

static void gpu_upload_texture(const uint8_t *rgba32, int width, int height) {
    struct GTex *t = g_cur_tex[g_cur_tmu];
    uint32_t n = (uint32_t)width * (uint32_t)height;
    uint32_t bytes = n * (uint32_t)g_fb_bpp;   /* 1 byte CI8 / 2 bytes RGB565 */
    if (n == 0) return;
    /* Double-buffer the pixel store to kill an ABA texture-content race: the os30
     * GPU reads texels from t->addr (SDRAM) ASYNCHRONOUSLY with NO render-path
     * fence, so re-uploading a texture-cache-recycled slot IN PLACE would
     * overwrite bytes that in-flight triangles of THIS frame are still sampling
     * -> wrong texture (worst on animated characters, which churn many small
     * part-textures and wrap the gfx_pc texture cache mid-frame).  Writing the new
     * pixels into the OTHER slot keeps the address the GPU is still reading valid;
     * later triangles re-point to the fresh slot via the new t->addr (emit_tri_state
     * re-emits because tex_addr changed).  Caps are retained, so steady state does
     * zero extra allocs and adds no GPU stall. */
    uint8_t slotc = t->cur ^ 1u;
    if (t->slot[slotc] == NULL || t->slotcap[slotc] < bytes) {
        free(t->slot[slotc]);
        t->slot[slotc] = malloc(bytes);
        t->slotcap[slotc] = bytes;
    }
    t->cur = slotc;
    t->ci8 = t->slot[slotc];
    if (g_truecolor) {
        uint16_t *px = (uint16_t *)t->ci8;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t r = rgba32[i * 4 + 0], g = rgba32[i * 4 + 1];
            uint8_t b = rgba32[i * 4 + 2], a = rgba32[i * 4 + 3];
            /* a<0x80 -> 0x0000 transparent (SKIP_ZERO).  Avoid a true black
             * opaque texel colliding with transparent by nudging to 0x0008. */
            uint16_t c = (a < 0x80) ? 0x0000 : rgba_to_565(r, g, b);
            if (a >= 0x80 && c == 0x0000) c = 0x0008;
            px[i] = c;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            uint8_t r = rgba32[i * 4 + 0], g = rgba32[i * 4 + 1];
            uint8_t b = rgba32[i * 4 + 2], a = rgba32[i * 4 + 3];
            t->ci8[i] = (a < 0x80) ? 0 : rgb332(r, g, b);
        }
    }
    t->w = (uint16_t)width;
    t->h = (uint16_t)height;
    /* Power-of-two dims use the GPU's bitmask wrap; non-power-of-two dims set a
     * no-op 0xFFFF mask and engage the GPU's real clamp unit (in emit_tri_state)
     * instead.  The bitmask `s & (width-1)` corrupts non-pow2 widths (e.g. the
     * 80x20 title/ending/game-over backgrounds: s & 0x4F drops bits 16/32),
     * collapsing the image into a garbled band — the "texture zoomed/cropped
     * inside the rect" symptom.  SM64's non-pow2 textures are all G_TX_CLAMP. */
    t->wmask = ((width  & (width  - 1)) == 0) ? (uint16_t)(width  - 1) : 0xFFFF;
    t->hmask = ((height & (height - 1)) == 0) ? (uint16_t)(height - 1) : 0xFFFF;
    t->addr = (uint32_t)(uintptr_t)t->ci8;
    of_cache_flush_range(t->ci8, bytes);
}

static void gpu_set_sampler_parameters(int tile, bool linear, uint32_t cms,
                                       uint32_t cmt) {
    (void)linear;   /* nearest sampling only (no bilinear) */
    /* Capture the N64 wrap mode so emit_tri_state can drive the GPU's clamp
     * unit (non-pow2 / G_TX_CLAMP) and mirror addressing (G_TX_MIRROR). */
    struct GTex *t = g_cur_tex[tile & 1];
    if (t) { t->cms = (uint8_t)cms; t->cmt = (uint8_t)cmt; }
}

static void gpu_set_depth_test(bool depth_test) { g_z_test = depth_test; }
static void gpu_set_depth_mask(bool z_upd)       { g_z_write = z_upd; }
static int g_decal;
static void gpu_set_zmode_decal(bool decal)      { g_decal = decal; }

static void gpu_set_viewport(int x, int y, int width, int height) {
    g_hw = (float)(width >> 1);
    g_hh = (float)(height >> 1);
    g_cx = (float)x + g_hw;
    g_cy = (float)y + g_hh;
}

static void gpu_set_scissor(int x, int y, int width, int height) {
    int x1 = x + width, y1 = y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    g_clip_x0 = (int16_t)x;  g_clip_y0 = (int16_t)y;
    g_clip_x1 = (int16_t)x1; g_clip_y1 = (int16_t)y1;
}

static int     g_use_alpha;
static uint8_t g_surf_alpha = 255;   /* per-surface src alpha for blending */
static void gpu_set_use_alpha(bool use_alpha) { g_use_alpha = use_alpha; }
static void gpu_set_fog_color(const uint8_t *rgb) { (void)rgb; }

/* ================================================================
 * Triangle emission
 * ================================================================ */

static uint8_t shade_row(float r, float g, float b) {
    int lum = (int)(0.30f * r + 0.59f * g + 0.11f * b);
    if (lum < 0) lum = 0; else if (lum > 255) lum = 255;
    /* (255-lum) >> 2 maps bright->row 0, dark->row 63 with no divide. */
    return (uint8_t)((255 - lum) >> 2);
}

/* Truecolor brightness: luminance >> 2 -> 0..63 (brighter = higher), the
 * sense the RTL's rgb565_modulate expects (texel * (light+1) >> 6). */
static uint8_t brightness_row(float r, float g, float b) {
    int lum = (int)(0.30f * r + 0.59f * g + 0.11f * b);
    if (lum < 0) lum = 0; else if (lum > 255) lum = 255;
    return (uint8_t)(lum >> 2);
}

static int gpu_z_mode(void) {
    if (g_z_test)  return g_z_write ? OF_GPU_PARAM_Z_TEST_WRITE : OF_GPU_PARAM_Z_TEST_ZI;
    if (g_z_write) return OF_GPU_PARAM_Z_WRITE_ZI;
    return OF_GPU_PARAM_Z_NONE;
}

/* ----------------------------------------------------------------------------
 * Full combiner emulation: clamp(texel*C + D).
 *
 * The GPU computes, per pixel, clamp(texel*C + D) with C and D per-vertex RGB.
 * We reduce the RGB color cycle (a-b)*c+d to that canonical form, treating
 * TEXEL0 as the free (per-pixel) variable.  We only engage it for the HILITE
 * class — c == TEXEL0 with a real subtraction or additive term — which the
 * legacy texel*C path (unsigned C, no add) cannot represent.  Everything else
 * (MODULATE/DECAL/SHADE/world geometry) stays on the byte-exact legacy path.
 *
 * Combiner inputs resolve straight out of buf_vbo by INDEX: cc.c[0][*] holds
 * SHADER_INPUT_n tokens, and SHADER_INPUT_n's resolved color already lives at
 * buf_vbo input slot (n-1).  So no PRIM/SHADE/ENV identity (shader_input_mapping)
 * is needed — the equation is symbolic in the input slots.
 *
 * C-field encoding (RTL reads per-vertex RGB565 words 12-14 as BIASED-UNSIGNED,
 * then subtracts the bias before the signed texel*C product):
 *   R/B 5-bit: field = clamp(round(C*32),-16,15) + 16  (0..31, bias 16).
 *   G   6-bit: field = clamp(round(C*64),-32,31) + 32  (0..63, bias 32).
 * Biased so C interpolates monotonically (unsigned) across the triangle; a raw
 * two's-comp field would wrap at C's zero-crossings and facet the head.
 * So |C| saturates near 0.5 (R/B) — the accepted soft-highlight limit for now.
 * D is the unsigned per-vertex color (R/B 0..31, G 0..63) on words 19-21.
 * ---------------------------------------------------------------------------- */

/* Map a combiner token to a buf_vbo color-input index. idx=-1 means constant 0.
 * Returns 0 (ineligible) for TEXEL0/TEXEL1/alpha tokens. */
static int cc_input_idx(uint8_t tok, int *idx) {
    if (tok == SHADER_0) { *idx = -1; return 1; }
    if (tok >= SHADER_INPUT_1 && tok <= SHADER_INPUT_4) { *idx = (int)(tok - SHADER_INPUT_1); return 1; }
    return 0;
}

/* Classify the RGB color cycle.  On the HILITE form (c==TEXEL0) fills the input
 * indices for a/b/d and returns 1; otherwise returns 0 (use legacy). */
static int classify_combine_cd(const struct CCFeatures *cc,
                               int *a_in, int *b_in, int *d_in) {
    uint8_t a = cc->c[0][0], b = cc->c[0][1], c = cc->c[0][2], d = cc->c[0][3];
    int ai, bi, di;
    if (c != SHADER_TEXEL0) return 0;                 /* need TEXEL0 as multiplicand */
    if (!cc_input_idx(a, &ai)) return 0;
    if (!cc_input_idx(b, &bi)) return 0;
    if (!cc_input_idx(d, &di)) return 0;
    if (b == SHADER_0 && d == SHADER_0) return 0;     /* pure texel*C -> legacy */
    *a_in = ai; *b_in = bi; *d_in = di;
    return 1;
}

/* Resolve a buf_vbo color input (or constant 0) at one vertex into 0..1 floats.
 * f = vertex float base; colors are premultiplied by 1/w, so multiply by w.
 * GFX_DONT_SCALE_COLORS (TARGET_OPENFPGA) stores colors 0..255, so also divide
 * by 255 — the enc_C/enc_D encoders below expect 0..1 (without this the C and D
 * fields saturate and the HILITE head blows out to white). */
static void cc_val_rgb(const float *f, int coff, int istride, int idx, float w, float out[3]) {
    if (idx < 0) { out[0] = out[1] = out[2] = 0.0f; return; }
    const float s = w * (1.0f / 255.0f);
    out[0] = f[coff + idx * istride + 0] * s;
    out[1] = f[coff + idx * istride + 1] * s;
    out[2] = f[coff + idx * istride + 2] * s;
}

/* C is BIASED-UNSIGNED (not two's-complement): signed -16..+15 -> field 0..31
 * (bias 16); signed -32..+31 -> field 0..63 (bias 32).  The RTL interpolates the
 * field as unsigned (correct, monotonic) and subtracts the bias before the signed
 * texel*C product.  Two's-comp here would interpolate-as-unsigned and wrap at C's
 * zero-crossings, faceting the Goddard head. */
static inline uint8_t enc_C5(float c) {   /* biased-unsigned 5-bit (bias 16) */
    int v = (int)lrintf(c * 32.0f);
    if (v < -16) v = -16; else if (v > 15) v = 15;
    return (uint8_t)(v + 16);
}
static inline uint8_t enc_C6(float c) {   /* biased-unsigned 6-bit (bias 32) */
    int v = (int)lrintf(c * 64.0f);
    if (v < -32) v = -32; else if (v > 31) v = 31;
    return (uint8_t)(v + 32);
}
static inline uint8_t enc_D5(float d) {   /* unsigned 5-bit 0..31 */
    int v = (int)lrintf(d * 31.0f);
    if (v < 0) v = 0; else if (v > 31) v = 31;
    return (uint8_t)v;
}
static inline uint8_t enc_D6(float d) {   /* unsigned 6-bit 0..63 */
    int v = (int)lrintf(d * 63.0f);
    if (v < 0) v = 0; else if (v > 63) v = 63;
    return (uint8_t)v;
}

static unsigned g_cd_fallbacks;      /* combiners that didn't fit texel*C+D */
static int      g_cd_active;         /* set by gpu_draw_triangles for emit_tri_state */
static int      g_subpix_tri;        /* 1 = 3D vert-tri sends Q12.4 subpixel Y (control bit 31) */

/* Emit the sticky surface state for the current batch. */
static void emit_tri_state(int textured) {
    struct GTex *t = textured ? g_cur_tex[0] : NULL;
    of_gpu_tri_state_t st;
    memset(&st, 0, sizeof(st));
    st.fb_base       = g_draw_fb;
    st.fb_major_step = FB_STRIDE * g_fb_bpp;   /* bytes per scanline */
    st.fb_minor_step = g_fb_bpp;               /* bytes per pixel */
    int skip_zero = g_cur_shader && (g_cur_shader->cc.opt_alpha ||
                                     g_cur_shader->cc.opt_texture_edge);
    if (g_truecolor) {
        if (textured && t && t->ci8) {
            st.tex_addr  = t->addr;
            st.tex_width = t->w;
            st.tex_w_mask = t->wmask;
            st.tex_h_mask = t->hmask;
        } else {
            st.tex_addr  = g_white_addr;   /* 1x1 white RGB565 */
            st.tex_width = 1;
            st.tex_w_mask = 0;
            st.tex_h_mask = 0;
        }
        st.flags = OF_GPU_SPAN_TRUECOLOR;
        if (skip_zero) st.flags |= OF_GPU_SPAN_SKIP_ZERO;
        st.colormap_id = 0;
    } else {
        if (textured && t && t->ci8) {
            st.tex_addr  = t->addr;
            st.tex_width = t->w;
            st.tex_w_mask = t->wmask;
            st.tex_h_mask = t->hmask;
        } else {
            st.tex_addr  = g_ramp_addr;
            st.tex_width = RAMP_W;
            st.tex_w_mask = RAMP_W - 1;
            st.tex_h_mask = 0;
        }
        st.flags = OF_GPU_SPAN_COLORMAP;
        if (skip_zero) st.flags |= OF_GPU_SPAN_SKIP_ZERO;
        st.colormap_id = 0;
    }
    /* Non-power-of-two textures: engage the GPU's real clamp unit (clamp_max in
     * Q16.16 texels) so the post-clamp `& mask` (mask = 0xFFFF, no-op) passes
     * the clamped 0..dim-1 index through.  Matches gfx_soft's G_TX_CLAMP. */
    if (textured && t && t->ci8) {
        if (t->wmask == 0xFFFF) st.clamp_max[0] = (uint32_t)(t->w - 1) << 16;
        if (t->hmask == 0xFFFF) st.clamp_max[1] = (uint32_t)(t->h - 1) << 16;
        /* G_TX_MIRROR (bit 0 of the N64 wrap mode): the GPU reflects the texel
         * index every W texels instead of wrapping — needed for the symmetric
         * star/circle stage transitions and Mario's shadow (quarter texture
         * mirrored to a full shape).  Mirror axes are power-of-two, so they
         * never collide with the non-pow2 clamp above. */
        if (t->cms & 0x1) st.mirror_s = 1;
        if (t->cmt & 0x1) st.mirror_t = 1;
    }

    /* Src-over alpha blend (truecolor): translucent surfaces (water, lava,
     * screen fades, Boos / Mario vanish) blend against the framebuffer by the
     * per-surface constant alpha instead of writing opaque.  SKIP_ZERO stays on
     * so a fully-transparent texel still drops out.  Untextured translucent
     * surfaces (white-texel fades) blend too, hence outside the textured gate. */
    /* Const-alpha src-over blend ONLY for genuinely TRANSLUCENT (XLU) surfaces.
     * Opaque/AA geometry also reports opt_alpha (coverage alpha) and can carry a
     * material alpha < 255, but it writes depth and must stay opaque — blending it
     * sank the title Mario head and the Peach letter into the dark background.
     * XLU surfaces (water, lava, screen fades, Boos) conventionally disable
     * z-write, so gate on !g_z_write to separate them from opaque AA geometry. */
    /* a==0 must NOT blend: opaque surfaces (e.g. the intro goddard Mario head,
     * AA-no-z-write textured geometry) report opt_alpha but carry a spurious
     * 0 surface alpha — blending them src*0+dst makes them fully TRANSPARENT, so
     * they vanish and show the geometry behind = "wrong texture / missing head".
     * Only blend genuinely-translucent surfaces (0 < a < 255); a==0 renders
     * opaque (visible).  Real translucency (water/lava/fades/dialog box ~150)
     * is unaffected. */
    if (g_truecolor && g_cur_shader && g_cur_shader->cc.opt_alpha
        && g_surf_alpha > 0 && g_surf_alpha < 255 && !g_z_write) {
        st.flags |= OF_GPU_SPAN_BLEND;
        st.const_alpha = g_surf_alpha;
    }

    /* Full combiner emulation (texel*C+D): enabled per-draw by gpu_draw_triangles
     * (g_cd_active) when the combiner is the HILITE class.  Truecolor only. */
    if (g_truecolor && g_cd_active)
        st.cd_combine = 1;

    /* Subpixel-Y (control bit 31): set for the 3D vert-tri path (which sends
     * Q12.4 Y), cleared for 2D rects (pixel-aligned integer Y). */
    st.subpix_y = (uint8_t)g_subpix_tri;

    st.z_mode = (uint8_t)gpu_z_mode();
    if (st.z_mode != OF_GPU_PARAM_Z_NONE) {
        st.z_base = (uint32_t)(uintptr_t)g_zbuf;
        st.z_major_step = SCR_W * ZBUF_ELEM;
        st.z_minor_step = ZBUF_ELEM;
    }
    st.clip_x0 = g_clip_x0; st.clip_x1 = g_clip_x1;
    st.clip_y0 = g_clip_y0; st.clip_y1 = g_clip_y1;

    /* Skip redundant sticky-state commands (st is fully memset, so the
     * padding is zero and the compare is well-defined). */
    if (g_st_cache_valid && memcmp(&st, &g_st_cache, sizeof(st)) == 0)
        return;
    g_st_cache = st;
    g_st_cache_valid = 1;
    of_gpu_set_tri_state(&st);
}

/* Per-triangle Q29 zi-precision override (0x4E word 15: {q29_en[5], shift[4:0]}).
 * SM64's large world units leave 1/w with few significant bits on near/far
 * surfaces, so the perspective derive snaps ("texture jump").  A per-tri shift
 * rescales zi into the derive's precision window.  Formula wired once the RTL
 * w15 semantics are confirmed; 0 = legacy precision (no change). */
static inline uint32_t gpu_q29_word(const int32_t zi[3]) {
    (void)zi;
    return 0u;
}

static void gpu_draw_triangles(float buf_vbo[], size_t buf_vbo_len,
                               size_t buf_vbo_num_tris) {
    if (!g_has_gpu || !g_cur_shader || buf_vbo_num_tris == 0)
        return;

    const struct ShaderProgram *sh = g_cur_shader;
    const int stride = (int)(buf_vbo_len / (3 * buf_vbo_num_tris));
    const int textured = sh->cc.used_textures[0] && g_cur_tex[0] &&
                         g_cur_tex[0]->ci8;
    const int has_color = sh->cc.num_inputs > 0;
    /* buf_vbo carries RAW tile-relative texel coords (gfx_pc.c, TARGET_OPENFPGA):
     * the recovered per-vertex value IS the texel, so the only scale here is
     * texel->Q16.16 (65536).  Do NOT multiply by the uploaded texture width — the
     * GPU already strides the texel fetch by st.tex_width (= t->w); folding it in
     * here too double-counted the tile/upload size ratio and stretched textures. */
    const float tw_q = 65536.0f;
    const float th_q = 65536.0f;
    const int coff = sh->coff;
    /* When a combiner has 2 colour inputs, the 2nd is the shade (the 1st is the
     * specular PRIM); pick it so e.g. the Mario head isn't black. */
    const int csel = (sh->cc.num_inputs > 1) ? (sh->cc.opt_alpha ? 4 : 3) : 0;

    /* Full combiner emulation: detect the HILITE class (texel*C+D) so the GPU
     * resolves the specular highlight (e.g. the title Mario head) instead of the
     * legacy single-input texel*C.  Only on truecolor + textured surfaces. */
    const int istride = sh->cc.opt_alpha ? 4 : 3;
    int cd_a = -1, cd_b = -1, cd_d = -1;
    int cd_on = (g_truecolor && textured)
                ? classify_combine_cd(&sh->cc, &cd_a, &cd_b, &cd_d) : 0;
    if (g_truecolor && textured && !cd_on) g_cd_fallbacks++;
    g_cd_active = cd_on;
    g_subpix_tri = g_truecolor;   /* 3D vert-tri: Q12.4 subpixel Y (truecolor only) */

    /* Per-surface src alpha for blending (Stage 0: constant).  When the
     * combiner blends (opt_alpha), each colour input carries a 4th alpha float
     * (premultiplied by 1/w like the colour); take vertex 0's as the surface
     * constant — water/lava/fades/Boos use a uniform alpha across the surface. */
    g_surf_alpha = 255;
    if (sh->cc.opt_alpha && has_color) {
        const float *f0 = buf_vbo;
        float w0 = (f0[3] != 0.0f) ? 1.0f / f0[3] : 0.0f;
        int a = (int)lrintf(f0[coff + csel + 3] * w0);
        if (a < 0) a = 0; else if (a > 255) a = 255;
        g_surf_alpha = (uint8_t)a;
    }

    emit_tri_state(textured);

    /* buf_vbo holds premultiplied NDC (GFX_W_PREMULT on): f[0],f[1]=NDC x,y;
     * f[3]=1/w; f[4],f[5]=u/w,v/w; colours premultiplied by 1/w.  Project to
     * screen, recover the perspective divide, and emit screen-space 0x4E/0x4B. */
    for (size_t tri = 0; tri < buf_vbo_num_tris; tri++) {
        int16_t x[3], y[3];
        int32_t s[3], t[3], zi[3];
        uint8_t light[3];
        uint16_t rgb[3];
        uint16_t rgb_d[3];      /* combine: per-vertex additive D (RGB565) */
        int32_t depth[3];

        /* Per-triangle perspective scale (see ZI_SCALE / ZI_DERIVE_TARGET).
         * The derived planes are szi=(u/w)*K, tzi=(v/w)*K, zi=(1/w)*K, so the
         * largest of |1/w|,|u/w|,|v/w| over the 3 verts bounds all three.  Pick
         * K so the peak lands at ZI_DERIVE_TARGET — maximising the significant
         * gradient bits the HW derive retains, without overflowing the Q16.16
         * window.  u/w,v/w are the premultiplied f[4],f[5]; 1/w is f[3].  Scale
         * cancels in szi/zi, so each triangle may use its own K and the shared
         * edge stays consistent.
         *
         * ONLY the textured path: there s carries the raw texel u, so szi=(u/w)*K
         * really is bounded by f[4].  The untextured palettized path instead packs
         * the colour index (0..255) into s, so szi=index*(1/w)*K — a large K would
         * overflow it; that (precision-insensitive) ramp path keeps the fixed
         * ZI_SCALE.  Truecolor-untextured sends s=t=0 (no texcoord to sharpen). */
        float zscale = (float)ZI_SCALE;
        if (textured) {
            float m = 0.0f;
            for (int k = 0; k < 3; k++) {
                const float *f = buf_vbo + (tri * 3 + k) * stride;
                float a3 = fabsf(f[3]);            /* |1/w| (bounds zi)        */
                float a4 = fabsf(f[4]);            /* |u/w| (bounds szi)       */
                float a5 = fabsf(f[5]);            /* |v/w| (bounds tzi)       */
                if (a3 > m) m = a3;
                if (a4 > m) m = a4;
                if (a5 > m) m = a5;
            }
            if (m > 0.0f) zscale = ZI_DERIVE_TARGET / m;
            if (zscale < 1.0f) zscale = 1.0f;      /* never shrink below 1:1   */
        }

        for (int k = 0; k < 3; k++) {
            const float *f = buf_vbo + (tri * 3 + k) * stride;
            float wi = f[3];                       /* 1/w */
            float w  = (wi != 0.0f) ? 1.0f / wi : 0.0f;

            float sx = f[0] * g_hw + g_cx;
            float sy = g_cy - f[1] * g_hh;          /* flip NDC y-up -> screen y-down */
            x[k] = (int16_t)lrintf(sx * 16.0f);
            if (g_truecolor) {
                /* Q12.4 subpixel Y (control bit 31) — 1/16-scanline edge
                 * precision so triangle vertices aren't snapped to whole
                 * scanlines (fixes the slightly-displaced character polygons).
                 * Clamp to +/-2047 px so sy*16 fits int16; verts past that are
                 * far outside the viewport and the walker clips them anyway. */
                float syc = (sy < -2047.0f) ? -2047.0f : (sy > 2047.0f ? 2047.0f : sy);
                y[k] = (int16_t)lrintf(syc * 16.0f);
            } else {
                y[k] = (int16_t)lrintf(sy);          /* integer scanline (palettized) */
            }

            int z = (int)lrintf(wi * (65536.0f * zscale));  /* per-tri scaled Q16.16 of 1/w */
            if (z < 1) z = 1;
            zi[k] = z;

            /* Decoupled high-precision depth (0x4E w16-18): 1/w at a large fixed
             * scale, GPU float-compresses it for the z-buffer (independent of the
             * perspective-capped zi above) -> ~10x better far-depth precision. */
            float df = wi * 1073741824.0f;      /* 1/w * 2^30 */
            /* Decal (G_ZMODE_DEC: shadows, signs, painting frames) sits on a
             * coplanar surface — bias it toward the camera (larger 1/w = nearer)
             * so it reliably wins the z-test instead of z-fighting/popping.
             * See DECAL_Z_BIAS (tune on hardware). */
            if (g_decal) df *= DECAL_Z_BIAS;
            if (df < 1.0f) df = 1.0f;
            else if (df > 2147483520.0f) df = 2147483520.0f;
            depth[k] = (int32_t)lrintf(df);

            float cr = 255.0f, cg = 255.0f, cb = 255.0f;
            if (has_color) {
                cr = f[coff + csel + 0] * w;
                cg = f[coff + csel + 1] * w;
                cb = f[coff + csel + 2] * w;
            }

            if (textured) {
                s[k] = (int32_t)lrintf((f[4] * w) * tw_q);  /* raw texel s, Q16.16 */
                t[k] = (int32_t)lrintf((f[5] * w) * th_q);
            } else if (g_truecolor) {
                s[k] = 0;
                t[k] = 0;
            } else {
                s[k] = (int32_t)rgb332((int)cr, (int)cg, (int)cb) << 16;
                t[k] = 0;
            }

            if (g_truecolor) {
                if (cd_on) {
                    /* texel*C+D: C = val(a)-val(b) (signed), D = val(d).  Encode
                     * C into the signed RGB565 words; D into the rgb_d triple. */
                    float va[3], vb[3], vd[3];
                    cc_val_rgb(f, coff, istride, cd_a, w, va);
                    cc_val_rgb(f, coff, istride, cd_b, w, vb);
                    cc_val_rgb(f, coff, istride, cd_d, w, vd);
                    rgb[k]   = ((uint16_t)enc_C5(va[0] - vb[0]) << 11)
                             | ((uint16_t)enc_C6(va[1] - vb[1]) << 5)
                             |  (uint16_t)enc_C5(va[2] - vb[2]);
                    rgb_d[k] = ((uint16_t)enc_D5(vd[0]) << 11)
                             | ((uint16_t)enc_D6(vd[1]) << 5)
                             |  (uint16_t)enc_D5(vd[2]);
                } else {
                    rgb[k]   = has_color ? rgba_to_565((int)cr, (int)cg, (int)cb) : 0xFFFF;
                    rgb_d[k] = 0;
                }
            } else {
                light[k] = (textured && has_color) ? shade_row(cr, cg, cb) : 0;
            }
        }

        /* Slope-scaled decal offset (see DECAL_SLOPE_PIXELS).  Fit the depth
         * plane z(x,y) to the 3 screen verts, take the per-pixel gradient, and
         * push the whole decal toward the camera (larger depth = nearer) by
         * DECAL_SLOPE_PIXELS * |gradient| — so grazing-angle ground shadows win
         * the z-test where the flat DECAL_Z_BIAS floor is dwarfed.  The constant
         * floor (applied per-vertex above) handles the flat-on case; this adds
         * the angle-dependent term on top. */
        if (g_decal) {
            float p0x = x[0] * (1.0f / 16.0f);     /* Q12.4 subpixel -> pixels */
            float p1x = x[1] * (1.0f / 16.0f);
            float p2x = x[2] * (1.0f / 16.0f);
            float e1x = p1x - p0x, e1y = (float)(y[1] - y[0]), e1z = (float)(depth[1] - depth[0]);
            float e2x = p2x - p0x, e2y = (float)(y[2] - y[0]), e2z = (float)(depth[2] - depth[0]);
            float area = e1x * e2y - e2x * e1y;
            if (fabsf(area) > 0.5f) {              /* skip degenerate/edge-on tris */
                float inv  = 1.0f / area;
                float dzdx = (e1z * e2y - e2z * e1y) * inv;
                float dzdy = (e1x * e2z - e2x * e1z) * inv;
                float slope = fabsf(dzdx);
                float ady   = fabsf(dzdy);
                if (ady > slope) slope = ady;
                float bias = DECAL_SLOPE_PIXELS * slope;
                for (int k = 0; k < 3; k++) {
                    float d = (float)depth[k] + bias;
                    if (d > 2147483520.0f) d = 2147483520.0f;
                    depth[k] = (int32_t)d;
                }
            }
        }

        if (g_truecolor)
            of_gpu_draw_vert_tri_rgb(x, y, s, t, zi, rgb, gpu_q29_word(zi), depth,
                                     cd_on ? rgb_d : NULL);
        else
            of_gpu_draw_vert_tri(x, y, s, t, zi, light);
    }
    /* Publish this batch so the GPU rasterizes it while the CPU builds the next. */
    of_gpu_kick();
}

/* ================================================================
 * 2D rects
 * ================================================================ */

static void gpu_fill_rect(int x0, int y0, int x1, int y1, const uint8_t *rgba) {
    if (!g_has_gpu) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCR_W) x1 = SCR_W;
    if (y1 > SCR_H) y1 = SCR_H;
    if (x1 <= x0 || y1 <= y0) return;
    /* clear_rect replicates one byte, so truecolor can only fill a
     * byte-uniform RGB565 — use black (covers SM64's letterbox/border fills;
     * rare coloured fills degrade to black). */
    uint8_t c = g_truecolor ? 0 : rgb332(rgba[0], rgba[1], rgba[2]);
    uint32_t addr = g_draw_fb +
                    ((uint32_t)y0 * FB_STRIDE + (uint32_t)x0) * (uint32_t)g_fb_bpp;
    of_gpu_clear_rect_strided(addr, (uint16_t)((x1 - x0) * g_fb_bpp),
                              (uint16_t)(y1 - y0),
                              (uint16_t)(FB_STRIDE * g_fb_bpp), c);
}

static void gpu_tex_rect(int x0, int y0, int x1, int y1, float u0, float v0,
                         float dudx, float dvdy, const uint8_t *rgba) {
    if (!g_has_gpu || !g_cur_tex[0] || !g_cur_tex[0]->ci8) return;
    if (x1 <= x0 || y1 <= y0) return;

    int modulate = g_cur_shader && g_cur_shader->cc.num_inputs;
    uint8_t row = 0;          /* palettized: shade row */
    uint16_t vc = 0xFFFF;     /* truecolor: per-vertex RGB565 = prim colour */
    if (g_truecolor)
        vc = modulate ? rgba_to_565(rgba[0], rgba[1], rgba[2]) : 0xFFFF;
    else
        row = modulate ? shade_row(rgba[0], rgba[1], rgba[2]) : 0;

    /* 2D rect blend: the prim/env colour's alpha is the surface alpha. */
    g_surf_alpha = modulate ? rgba[3] : 255;

    g_cd_active = 0;   /* 2D blit: never the texel*C+D combine path */
    g_subpix_tri = 0;  /* 2D rects: integer pixel-aligned Y, not Q12.4 */
    emit_tri_state(1);

    /* corner texel coords (texel units, like gfx_soft tex_rect) */
    float uL = u0,                  uR = u0 + dudx * (x1 - x0);
    float vT = v0,                  vB = v0 + dvdy * (y1 - y0);
    const int32_t zi1 = 1 << 16;

    int16_t X[4] = { (int16_t)(x0 * 16), (int16_t)(x1 * 16),
                     (int16_t)(x1 * 16), (int16_t)(x0 * 16) };
    int16_t Y[4] = { (int16_t)y0, (int16_t)y0, (int16_t)y1, (int16_t)y1 };
    int32_t S[4] = { (int32_t)lrintf(uL * 65536.0f), (int32_t)lrintf(uR * 65536.0f),
                     (int32_t)lrintf(uR * 65536.0f), (int32_t)lrintf(uL * 65536.0f) };
    int32_t T[4] = { (int32_t)lrintf(vT * 65536.0f), (int32_t)lrintf(vT * 65536.0f),
                     (int32_t)lrintf(vB * 65536.0f), (int32_t)lrintf(vB * 65536.0f) };

    int16_t tx[3]; int16_t ty[3]; int32_t ts[3], tt[3], tzi[3];
    uint8_t tl[3]; uint16_t trgb[3]; int32_t tdepth[3];
    const int idx[2][3] = { {0,1,2}, {0,2,3} };
    for (int q = 0; q < 2; q++) {
        for (int k = 0; k < 3; k++) {
            int i = idx[q][k];
            tx[k] = X[i]; ty[k] = Y[i];
            ts[k] = S[i]; tt[k] = T[i]; tzi[k] = zi1;
            tl[k] = row; trgb[k] = vc;
            tdepth[k] = 0x40000000;   /* constant near depth (2D overlay) */
        }
        if (g_truecolor)
            of_gpu_draw_vert_tri_rgb(tx, ty, ts, tt, tzi, trgb, 0u, tdepth, NULL);
        else
            of_gpu_draw_vert_tri(tx, ty, ts, tt, tzi, tl);
    }
}

/* ================================================================
 * Frame hooks
 * ================================================================ */

static void gpu_init(void)        { /* boot happens in gfx_gpu_boot via the WM */ }
static void gpu_on_resize(void)   { }

static void gpu_start_frame(void) {
    if (!g_has_gpu) return;
    g_st_cache_valid = 0;   /* draw buffer rotated; force a fresh tri-state */
    of_gpu_clear_rect_strided(g_draw_fb, (uint16_t)(SCR_W * g_fb_bpp), SCR_H,
                              (uint16_t)(FB_STRIDE * g_fb_bpp), 0);
    of_gpu_clear_rect_strided((uint32_t)(uintptr_t)g_zbuf,
                              SCR_W * ZBUF_ELEM, SCR_H, SCR_W * ZBUF_ELEM, 0);
}

static void gpu_end_frame(void)    { if (g_has_gpu) of_gpu_kick(); }
static void gpu_finish_render(void) { }
static void gpu_shutdown(void)      { if (g_has_gpu) of_gpu_shutdown(); }

struct GfxRenderingAPI gfx_gpu_api = {
    gpu_z_is_from_0_to_1,
    gpu_unload_shader,
    gpu_load_shader,
    gpu_create_and_load_new_shader,
    gpu_lookup_shader,
    gpu_shader_get_info,
    gpu_new_texture,
    gpu_select_texture,
    gpu_upload_texture,
    gpu_set_sampler_parameters,
    gpu_set_depth_test,
    gpu_set_depth_mask,
    gpu_set_zmode_decal,
    gpu_set_viewport,
    gpu_set_scissor,
    gpu_set_use_alpha,
    gpu_draw_triangles,
    gpu_init,
    gpu_on_resize,
    gpu_start_frame,
    gpu_end_frame,
    gpu_finish_render,
    gpu_fill_rect,
    /* tex_rect = NULL: gpu_tex_rect feeds 2D glyph rects through the os30
     * vert-tri plane-derive, which transposes/flips axis-aligned rects (menu
     * text came out upside-down/stretched; a magnitude fix had no effect ->
     * not a precision bug).  With this NULL, gfx_dp_texture_rectangle takes its
     * fallback (gfx_pc.c:1615) -> gfx_draw_rectangle -> two gfx_sp_tri1 ->
     * gpu_draw_triangles: the SAME proven triangle pipeline that renders the
     * SCORE/COPY/ERASE/STEREO labels (and that the SW/OG renderer is equivalent
     * to).  Costs a vertex-transform per texrect vs the direct blit; text/HUD
     * are few quads so the impact is negligible. */
    NULL,
    gpu_set_fog_color,
    gpu_shutdown,
};
