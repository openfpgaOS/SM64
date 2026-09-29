#ifndef GFX_GPU_H
#define GFX_GPU_H

/*
 * gfx_gpu.h -- openfpgaOS hardware (os30) GPU rendering backend for SM64.
 *
 * Implements GfxRenderingAPI by emitting of_gpu vertex-triangle commands
 * (CMD_SET_TRI_STATE 0x4A + CMD_DRAW_VERT_TRI 0x4B).  Output is an 8-bit
 * indexed (RGB332) framebuffer shaded through a 64-row palookup colormap.
 *
 * gfx_gpu owns all of_gpu.h state (that header must be included from a
 * single TU), so the window-manager backend drives boot/present through
 * the small surface below rather than touching of_gpu directly.
 */

#include "gfx_rendering_api.h"

extern struct GfxRenderingAPI gfx_gpu_api;

/* Called once after of_video_init(): of_gpu_init, palette + colormap
 * upload, z-buffer alloc, first draw-buffer acquire. */
void gfx_gpu_boot(void);

/* Present the frame just drawn: GPU-triggered flip + acquire next draw
 * buffer (triple-buffered).  Called from the WM swap_buffers hook. */
void gfx_gpu_present(void);

/* ---- GPU vertex-cache fast path (0x56 LOAD_VERT_CLIP + 0x54 DRAW_INDEXED_TRI)
 *
 * gfx_pc.c owns vertex IDENTITY (the G_VTX slot index dies before buf_vbo), so
 * it drives this path; the backend owns MATERIAL policy (truecolor/HILITE/
 * decal classification, rgb565 encoding, the sticky 0x50 viewport + 0x4A
 * surface state).  Protocol, per candidate triangle:
 *
 *   1. gfx_gpu_vtx_cache_begin() — per-batch eligibility (memoized until any
 *      material state changes; truecolor only, no HILITE/cd, no decal, no
 *      blend/XLU).  Returns 0 -> use the flattened 0x4E path.
 *      Nonzero return carries GFX_VC_TEXTURED (fill u/v with the final
 *      tile-relative texel coords, exactly as the 0x4E path would emit them).
 *      *rgb_input   = which color input (0-based) is the per-vertex shade,
 *                     -1 = texel rides unmodulated (backend sends 0xFFFF).
 *      *alpha_input = which color input carries the surface alpha (resolved
 *                     from the ALPHA-cycle mapping), -1 = opaque 255.
 *   2. gfx_gpu_vtx_cache_tri(v, slot, dirty_mask) — re-derives the sticky
 *      0x50/0x4A per tri, dedup'd against the SHARED last-emitted memo that
 *      the legacy path also writes (single source of truth: no path-local
 *      emission edge), uploads the dirty slots (0x56, clip x/y/w + s/t
 *      Q16.16 + rgb565 + explicit (1/w)*2^30 depth) and draws the 1-word
 *      0x54.
 *
 * Caller guarantees: slots < 32, 0 < w < 32767 (Q16.16 fit), and that any
 * buffered 0x4E triangles were flushed first (ring draw order). */
#define GFX_VC_TEXTURED  2

struct gfx_vc_vtx {
    float cx, cy, cw;     /* clip-space x,y,w (LoadedVertex x/y/w) */
    float w_inv;          /* reciprocal already computed by the vertex loader */
    float u, v;           /* final tile-relative texels (0 when untextured) */
    uint8_t r, g, b;      /* resolved shade input, 0..255 (unused when white) */
    uint8_t a;            /* resolved surface alpha (v[0] only is consumed) */
};

/* Preserve clip coordinates for every fallback triangle. Shared vertices must
 * project identically across material changes, clipping and cached draws. */
#define GFX_VC_MAX_BUFFERED 256   /* must equal gfx_pc.c's MAX_BUFFERED */
struct gfx_vc_true_xyw {
    float x[3], y[3], w[3];
    uint8_t valid;
};
extern struct gfx_vc_true_xyw gfx_vc_true[GFX_VC_MAX_BUFFERED];

int  gfx_gpu_vtx_cache_begin(int *rgb_input, int *alpha_input);
void gfx_gpu_vtx_cache_tri(const struct gfx_vc_vtx v[3], const uint8_t slot[3],
                           unsigned dirty_mask);

/* Per-frame wire-cost counters (reset on read, like gpu_ring_prof_get):
 * words = ring words emitted by the draw path (0x4A/0x50/0x4E/0x4B/0x56/0x54),
 * ctris/ltris = triangles drawn via the cache (0x54) vs flattened (0x4E/0x4B),
 * loads = 0x56 vertex uploads. */
void gpu_vc_prof_get(unsigned *words, unsigned *ctris, unsigned *ltris,
                     unsigned *loads);

#endif /* GFX_GPU_H */
