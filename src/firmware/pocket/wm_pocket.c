/*
 * wm_pocket.c -- Analogue Pocket window manager for SM64
 *
 * Implements GfxWindowManagerAPI for the Pocket hardware.
 * The software rasterizer renders at 320x240 RGBA32 into cached SDRAM.
 * At swap time, we downsample 2:1 to 160x120 RGB332 and write to FB BRAM.
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
#define SYS_PAL_INDEX       (*(volatile uint32_t *)0x40000040)
#define SYS_PAL_DATA        (*(volatile uint32_t *)0x40000044)

#define SCREEN_WIDTH  160
#define SCREEN_HEIGHT 120

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

/* FB BRAM base address (mapped via axi_periph_slave at 0x47) */
#define FB_BRAM_BASE ((volatile uint32_t *)0x47000000)

/* Convert 160x120 RGBA32 (cached SDRAM) to 160x120 RGB332 in FB BRAM.
 * BRAM is dual-port: CPU writes port A, video scanout reads port B.
 * Writes 4 packed RGB332 pixels per 32-bit word (4800 word writes). */
static void convert_rgba32_to_fb_bram(void) {
    volatile uint32_t *dst = FB_BRAM_BASE;
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

static void gfx_pocket_swap_buffers_begin(void) {
    /* Convert RGBA32 from cached SDRAM to RGB332 in FB BRAM */
    if (gfx_output != NULL) {
        convert_rgba32_to_fb_bram();
    }

    /* Switch to FB mode on first frame */
    if (!fb_mode_active) {
        fb_mode_active = true;
        SYS_DISPLAY_MODE = 1;
    }
}

static void gfx_pocket_swap_buffers_end(void) {
    /* Single-buffered FB BRAM — no swap needed.
     * Video scanout reads port B concurrently (dual-port BRAM). */
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
