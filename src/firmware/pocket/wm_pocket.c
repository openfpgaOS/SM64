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
#include "pocket/span_rasterizer.h"
extern void term_printf(const char *fmt, ...);

/* System register MMIO */
#define SYS_STATUS          (*(volatile uint32_t *)0x40000000)
#define SYS_CYCLE_LO        (*(volatile uint32_t *)0x40000004)
#define SYS_CYCLE_HI        (*(volatile uint32_t *)0x40000008)
#define SYS_DISPLAY_MODE    (*(volatile uint32_t *)0x4000000C)
#define SYS_FB_PAGE_FLIP    (*(volatile uint32_t *)0x4000001C)
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
#define FB_BRAM_W ((volatile uint32_t *)0x47000000)

/* ZB BRAM for diagnostics */
#define ZB_BRAM_R ((volatile uint32_t *)0x46000000)

/* All pixels (HW and SW) are already in FB BRAM. Just drain and flip. */

int diag_frame = 0;

static void gfx_pocket_swap_buffers_begin(void) {
    /* Drain HW span FIFO to ensure all HW pixels are written */
    span_drain();

    /* Diagnostic: z-test pass/reject counters + z-buffer sampling */
    extern int batch_log_idx;
    batch_log_idx = 0;  /* Reset batch log for next frame */
    if (diag_frame < 10) {
        uint32_t zp = SPAN_ZTEST_PASS;
        uint32_t zr = SPAN_ZTEST_REJECT;
        /* Sample z-buffer at y=60, x=80 (center) */
        int idx = 160 * 59 + 80;
        int word = idx >> 1;
        uint32_t w = ZB_BRAM_R[word];
        uint16_t z = (idx & 1) ? (w >> 16) : (w & 0xFFFF);
        term_printf("F%d: pass=%u rej=%u z80=%04x\n",
                    diag_frame, zp, zr, z);
        diag_frame++;
    }

    /* Flip: toggle draw page. Video scanout reads the opposite page. */
    SYS_FB_PAGE_FLIP = 1;

    /* Switch to FB mode — delay to frame 15 so terminal diagnostics are visible */
    if (!fb_mode_active && diag_frame >= 15) {
        fb_mode_active = true;
        SYS_DISPLAY_MODE = 1;
    }
}

static void gfx_pocket_swap_buffers_end(void) {
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
