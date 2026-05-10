/*
 * Span Rasterizer — hardware pixel pipeline driver with command FIFO
 *
 * The span rasterizer handles the per-pixel inner loop in hardware:
 *   z-test → 1/w reciprocal → perspective correction → texture →
 *   color combine → framebuffer + z-buffer update
 *
 * CPU pushes CONFIG and SCANLINE commands into a 1024-word FIFO.
 * Hardware pops and processes them asynchronously, allowing the CPU
 * to overlap display list processing with rasterization.
 *
 * Command format (first word tag bits [31:30]):
 *   CONFIG   (tag=01, 14 words): mode/flags, dp_z/w/u/v/r/g/b, tex config
 *   SCANLINE (tag=10, 10 words): fb_idx, zb_idx, p_z/w/u/v/r/g/b
 *
 * FIFO registers:
 *   0x80: FIFO_PUSH   — write a 32-bit word into command FIFO
 *   0x84: FIFO_STATUS — [25:16]=free slots, [9:0]=used slots
 *   0x6C: STATUS      — bit 0 = busy (FIFO not empty or processing)
 *
 * Texture BRAM (CPU-loaded, up to 32x32 RGBA):
 *   0x1000-0x1FFF: 1024 x 32-bit RGBA texels
 */
#ifndef SPAN_RASTERIZER_H
#define SPAN_RASTERIZER_H

#include <stdint.h>

#define SPAN_BASE       0x45000000

/* FIFO interface */
#define SPAN_FIFO_PUSH   (*(volatile uint32_t *)(SPAN_BASE + 0x80))
#define SPAN_FIFO_STATUS (*(volatile uint32_t *)(SPAN_BASE + 0x84))
#define SPAN_STATUS      (*(volatile uint32_t *)(SPAN_BASE + 0x6C))

/* Texture BRAM base */
#define SPAN_TEX_BRAM   ((volatile uint32_t *)(SPAN_BASE + 0x1000))

/* Diagnostic counters */
#define SPAN_ZTEST_PASS   (*(volatile uint32_t *)(SPAN_BASE + 0x88))
#define SPAN_ZTEST_REJECT (*(volatile uint32_t *)(SPAN_BASE + 0x8C))

/* Command tags */
#define CMD_TAG_CONFIG   (1u << 30)
#define CMD_TAG_SCANLINE (2u << 30)

/* Mode field encoding */
#define SPAN_MODE_TEXRGB     0x0  /* tex * vertex color */
#define SPAN_MODE_TEX        0x1  /* texture only */
#define SPAN_MODE_RGB        0x2  /* vertex color only */
#define SPAN_FLAG_Z_TEST     (1 << 2)
#define SPAN_FLAG_Z_WRITE    (1 << 3)

/* FIFO free slot count (bits [25:16]) */
static inline int span_fifo_free(void) {
    return (SPAN_FIFO_STATUS >> 16) & 0x3FF;
}

/* Wait until FIFO has at least 'words' free slots */
static inline void span_fifo_wait_space(int words) {
    int timeout = 1000000;
    while (span_fifo_free() < words && --timeout > 0)
        ;
}

/* Push a CONFIG command (14 words): per-triangle setup */
static inline void span_push_config(uint32_t mode,
    int32_t dp_z, int32_t dp_w, int32_t dp_u, int32_t dp_v,
    int32_t dp_r, int32_t dp_g, int32_t dp_b,
    int32_t tex_fw, int32_t tex_fh,
    uint32_t wrap_w, uint32_t wrap_h,
    uint32_t log2_w, int32_t z_offset)
{
    span_fifo_wait_space(14);
    SPAN_FIFO_PUSH = CMD_TAG_CONFIG | mode;
    SPAN_FIFO_PUSH = dp_z;
    SPAN_FIFO_PUSH = dp_w;
    SPAN_FIFO_PUSH = dp_u;
    SPAN_FIFO_PUSH = dp_v;
    SPAN_FIFO_PUSH = dp_r;
    SPAN_FIFO_PUSH = dp_g;
    SPAN_FIFO_PUSH = dp_b;
    SPAN_FIFO_PUSH = tex_fw;
    SPAN_FIFO_PUSH = tex_fh;
    SPAN_FIFO_PUSH = wrap_w;
    SPAN_FIFO_PUSH = wrap_h;
    SPAN_FIFO_PUSH = log2_w;
    SPAN_FIFO_PUSH = z_offset;
}

/* Push a SCANLINE command (10 words): per-scanline span */
static inline void span_push_scanline(uint32_t fb_idx, uint32_t zb_idx,
    uint32_t count, int32_t p_z, int32_t p_w,
    int32_t p_u, int32_t p_v, int32_t p_r, int32_t p_g, int32_t p_b)
{
    span_fifo_wait_space(10);
    SPAN_FIFO_PUSH = CMD_TAG_SCANLINE | (count & 0x1FF);
    SPAN_FIFO_PUSH = fb_idx;
    SPAN_FIFO_PUSH = zb_idx;
    SPAN_FIFO_PUSH = p_z;
    SPAN_FIFO_PUSH = p_w;
    SPAN_FIFO_PUSH = p_u;
    SPAN_FIFO_PUSH = p_v;
    SPAN_FIFO_PUSH = p_r;
    SPAN_FIFO_PUSH = p_g;
    SPAN_FIFO_PUSH = p_b;
}

/* Drain FIFO: wait until all commands are processed */
static inline void span_drain(void) {
    int timeout = 1000000;
    while ((SPAN_STATUS & 1) && --timeout > 0)
        ;
}

/* Upload RGBA32 texture to hardware BRAM (up to 1024 texels).
 * Must drain FIFO first to avoid texture BRAM contention. */
static inline void span_upload_texture(const uint32_t *src, int count) {
    span_drain();
    volatile uint32_t *dst = SPAN_TEX_BRAM;
    for (int i = 0; i < count; i++)
        dst[i] = src[i];
}

#endif /* SPAN_RASTERIZER_H */
