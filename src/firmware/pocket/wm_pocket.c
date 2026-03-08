/*
 * wm_pocket.c -- Analogue Pocket window manager for SM64
 *
 * Implements GfxWindowManagerAPI for the Pocket hardware.
 * The software rasterizer outputs RGBA32 to gfx_output.
 * We convert RGBA32 -> RGB332 (8-bit) and write to a BRAM framebuffer (160x120).
 * The FPGA video scanout reads BRAM with 2x pixel/line replication to 320x240,
 * then uses the hardware palette for RGB888 display.
 */

#ifdef TARGET_POCKET

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "macros.h"
#include "gfx/gfx_window_manager_api.h"
#include "gfx/gfx_soft.h"
extern void term_printf(const char *fmt, ...);

/* System register MMIO */
#define SYS_STATUS          (*(volatile uint32_t *)0x40000000)
#define SYS_CYCLE_LO        (*(volatile uint32_t *)0x40000004)
#define SYS_CYCLE_HI        (*(volatile uint32_t *)0x40000008)
#define SYS_DISPLAY_MODE    (*(volatile uint32_t *)0x4000000C)
#define SYS_FB_DISPLAY      (*(volatile uint32_t *)0x40000010)
#define SYS_FB_DRAW         (*(volatile uint32_t *)0x40000014)
#define SYS_FB_SWAP         (*(volatile uint32_t *)0x40000018)
#define SYS_PAL_INDEX       (*(volatile uint32_t *)0x40000040)
#define SYS_PAL_DATA        (*(volatile uint32_t *)0x40000044)

/* Uncached SDRAM alias base (bypasses D-cache, visible to video DMA scanout) */
#define SDRAM_UC_BASE 0x50000000

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

/* Cycle counter for timing */
static inline uint64_t get_cycles(void) {
    uint32_t lo = SYS_CYCLE_LO;
    uint32_t hi = SYS_CYCLE_HI;
    return ((uint64_t)hi << 32) | lo;
}

/* CPU clock speed (110 MHz) */
#define CPU_HZ 110000000ULL

/* Set up RGB332 palette in the hardware palette LUT.
 * Maps all 256 possible RGB332 values to their RGB888 equivalents. */
static void setup_rgb332_palette(void) {
    SYS_PAL_INDEX = 0;
    for (int i = 0; i < 256; i++) {
        /* RGB332: RRRGGGBB */
        uint32_t r3 = (i >> 5) & 0x7;
        uint32_t g3 = (i >> 2) & 0x7;
        uint32_t b2 = i & 0x3;

        /* Expand to 8-bit: replicate top bits into lower bits */
        uint32_t r8 = (r3 << 5) | (r3 << 2) | (r3 >> 1);
        uint32_t g8 = (g3 << 5) | (g3 << 2) | (g3 >> 1);
        uint32_t b8 = (b2 << 6) | (b2 << 4) | (b2 << 2) | b2;

        SYS_PAL_DATA = b8 | (g8 << 8) | (r8 << 16);
    }
}

/* Convert RGBA32 framebuffer to RGB332 and write to uncached SDRAM draw buffer.
 * Hardware does 2x pixel/line replication in the FPGA video scanout.
 * Writes 4 pixels at a time as 32-bit words for efficiency (4,800 word writes).
 * Uses uncached SDRAM alias (0x50xxxxxx) so writes are immediately visible to
 * the video DMA scanout without requiring D-cache flush. */
static void convert_rgba32_to_rgb332(void) {
    /* Get current draw buffer (25-bit SDRAM 16-bit-word address) */
    uint32_t draw_word_addr = SYS_FB_DRAW;
    volatile uint32_t *dst = (volatile uint32_t *)(SDRAM_UC_BASE + (draw_word_addr << 1));
    const uint32_t *src = gfx_output;
    int total = SCREEN_WIDTH * SCREEN_HEIGHT;

    for (int i = 0; i < total; i += 4) {
        uint32_t r0 = src[i+0], r1 = src[i+1], r2 = src[i+2], r3 = src[i+3];
        uint8_t c0 = (r0 & 0xE0) | ((r0 >> 11) & 0x1C) | ((r0 >> 22) & 0x03);
        uint8_t c1 = (r1 & 0xE0) | ((r1 >> 11) & 0x1C) | ((r1 >> 22) & 0x03);
        uint8_t c2 = (r2 & 0xE0) | ((r2 >> 11) & 0x1C) | ((r2 >> 22) & 0x03);
        uint8_t c3 = (r3 & 0xE0) | ((r3 >> 11) & 0x1C) | ((r3 >> 22) & 0x03);
        dst[i >> 2] = c0 | ((uint32_t)c1 << 8) | ((uint32_t)c2 << 16) | ((uint32_t)c3 << 24);
    }
}

/* Defer display mode switch until first frame is ready,
 * so terminal output stays visible during init for diagnostics. */
static bool fb_mode_active = false;

static void gfx_pocket_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen) {
    /* Set up the RGB332 palette in FPGA hardware */
    setup_rgb332_palette();

    /* Don't switch to FB mode yet — wait until first frame is rendered */
}

static void gfx_pocket_set_keyboard_callbacks(
    UNUSED bool (*on_key_down)(int scancode),
    UNUSED bool (*on_key_up)(int scancode),
    UNUSED void (*on_all_keys_up)(void)) {
    /* No keyboard on Pocket */
}

static void gfx_pocket_set_fullscreen_changed_callback(
    UNUSED void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
}

static void gfx_pocket_set_fullscreen(UNUSED bool enable) {
}

static void gfx_pocket_main_loop(void (*run_one_game_iter)(void)) {
    run_one_game_iter();
}

static void gfx_pocket_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = SCREEN_WIDTH;
    *height = SCREEN_HEIGHT;
}

static void gfx_pocket_handle_events(void) {
    /* No events to handle on bare metal */
}

static bool gfx_pocket_start_frame(void) {
    return true;
}

static void gfx_pocket_swap_buffers_begin(void) {
    /* Convert the software rasterizer output to the SDRAM draw buffer */
    if (gfx_output != NULL) {
        convert_rgba32_to_rgb332();
    }

    /* Switch to FB mode on first frame */
    if (!fb_mode_active) {
        fb_mode_active = true;
        SYS_DISPLAY_MODE = 1;
    }

    /* Request buffer flip (happens on next vblank) */
    SYS_FB_SWAP = 1;
}

static void gfx_pocket_swap_buffers_end(void) {
    /* Wait for vsync */
    while (SYS_FB_SWAP)
        ;
}

static double gfx_pocket_get_time(void) {
    return (double)get_cycles() / (double)CPU_HZ;
}

static void gfx_pocket_shutdown(void) {
    SYS_DISPLAY_MODE = 0; /* Back to terminal */
}

struct GfxWindowManagerAPI gfx_pocket_wm_api = {
    gfx_pocket_init,
    gfx_pocket_set_keyboard_callbacks,
    gfx_pocket_set_fullscreen_changed_callback,
    gfx_pocket_set_fullscreen,
    gfx_pocket_main_loop,
    gfx_pocket_get_dimensions,
    gfx_pocket_handle_events,
    gfx_pocket_start_frame,
    gfx_pocket_swap_buffers_begin,
    gfx_pocket_swap_buffers_end,
    gfx_pocket_get_time,
    gfx_pocket_shutdown,
};

#endif /* TARGET_POCKET */
