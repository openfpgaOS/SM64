/*
 * wm_pocket.c -- Analogue Pocket window manager for SM64
 *
 * Implements GfxWindowManagerAPI for the Pocket hardware.
 * The software rasterizer outputs RGBA32 to gfx_output.
 * We convert RGBA32 -> RGB332 (8-bit) and write to the SDRAM framebuffer,
 * then use the hardware palette for display.
 */

#ifdef TARGET_POCKET

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "macros.h"
#include "gfx/gfx_window_manager_api.h"
#include "gfx/gfx_soft.h"
#include "font8x8.h"

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

/* SDRAM uncached alias (bypasses D-cache for framebuffer writes) */
#define SDRAM_UC_BASE       0x50000000u

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

/* Get CPU byte address of the current draw framebuffer */
static uint8_t *fb_draw_buffer(void) {
    uint32_t draw_word_addr = SYS_FB_DRAW & 0x01FFFFFFu;
    return (uint8_t *)(SDRAM_UC_BASE + (draw_word_addr << 1));
}

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

/* Convert RGBA32 framebuffer to RGB332 indexed framebuffer.
 * gfx_output is 320x240 RGBA32, we write 320x240 8-bit to SDRAM. */
static void convert_rgba32_to_rgb332(void) {
    uint8_t *dst = fb_draw_buffer();
    const uint32_t *src = gfx_output;
    int count = SCREEN_WIDTH * SCREEN_HEIGHT;

    for (int i = 0; i < count; i++) {
        uint32_t rgba = src[i];
        /* RGBA32: byte order is R, G, B, A (little-endian: A<<24 | B<<16 | G<<8 | R) */
        uint8_t r = rgba & 0xFF;
        uint8_t g = (rgba >> 8) & 0xFF;
        uint8_t b = (rgba >> 16) & 0xFF;
        /* RGB332: RRRGGGBB */
        dst[i] = (r & 0xE0) | ((g >> 3) & 0x1C) | (b >> 6);
    }
}

static void gfx_pocket_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen) {
    /* Set up the RGB332 palette in FPGA hardware */
    setup_rgb332_palette();

    /* Switch to framebuffer display mode */
    SYS_DISPLAY_MODE = 1;
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

/* Debug overlay: draw text into RGB332 framebuffer */
extern char dbg_overlay_line[80];

static void dbg_draw_char(uint8_t *fb, int px, int py, char c) {
    if (c < 32 || c > 127) return;
    const uint8_t *glyph = font8x8[c - 32];
    for (int row = 0; row < 8; row++) {
        int y = py + row;
        if ((unsigned)y >= SCREEN_HEIGHT) continue;
        uint8_t *dst = fb + y * SCREEN_WIDTH + px;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int x = px + col;
            if ((unsigned)x >= SCREEN_WIDTH) continue;
            dst[col] = (bits & (0x80 >> col)) ? 0xFF : 0x00;
        }
    }
}

static void dbg_draw_overlay(uint8_t *fb) {
    int x = 1, y = 1;
    for (const char *s = dbg_overlay_line; *s; s++) {
        if (*s == '\n') { x = 1; y += 9; continue; }
        dbg_draw_char(fb, x, y, *s);
        x += 8;
    }
}

static void gfx_pocket_swap_buffers_begin(void) {
    /* Convert the software rasterizer output to the hardware framebuffer */
    if (gfx_output != NULL) {
        convert_rgba32_to_rgb332();
    }

    /* Draw debug overlay on top of converted framebuffer */
    if (dbg_overlay_line[0]) {
        dbg_draw_overlay(fb_draw_buffer());
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
