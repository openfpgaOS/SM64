#ifndef GFX_SOFT_H
#define GFX_SOFT_H

#include "gfx_rendering_api.h"

extern struct GfxRenderingAPI gfx_soft_api;
extern uint32_t *gfx_output;
#ifdef TARGET_POCKET
extern uint8_t *fb_cache;
#endif

#endif
