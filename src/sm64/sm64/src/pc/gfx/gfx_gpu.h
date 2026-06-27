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

#endif /* GFX_GPU_H */
