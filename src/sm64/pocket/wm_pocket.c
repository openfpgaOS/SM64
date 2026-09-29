//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * wm_pocket.c -- openfpgaOS window-manager backend for SM64.
 *
 * Implements GfxWindowManagerAPI over of_video.h.  Boot and frame
 * presentation are delegated to the GPU renderer (gfx_gpu.c), which owns
 * the of_gpu ring state and the triple-buffered draw/flip cycle.
 * (Filename/symbol kept for minimal churn.)
 */

#ifdef TARGET_OPENFPGA

#include <stdint.h>
#include <stdbool.h>

#include "macros.h"
#include "gfx/gfx_window_manager_api.h"
#include "gfx/gfx_gpu.h"

#include "of.h"
#include "of_video.h"
#include "of_timer.h"

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

static void gfx_of_init(UNUSED const char *game_name, UNUSED bool start_in_fullscreen) {
    of_video_init();
    gfx_gpu_boot();
}

static void gfx_of_set_keyboard_callbacks(
    UNUSED bool (*on_key_down)(int scancode),
    UNUSED bool (*on_key_up)(int scancode),
    UNUSED void (*on_all_keys_up)(void)) {
}

static void gfx_of_set_fullscreen_changed_callback(
    UNUSED void (*on_fullscreen_changed)(bool is_now_fullscreen)) {
}

static void gfx_of_set_fullscreen(UNUSED bool enable) {
}

/* SM64 is a FIXED 30 Hz timestep: all speeds/physics/timers are per-frame with
 * no delta-time, and the game advances exactly one step per rendered frame.  So
 * if a frame renders slower than 1/30 s, the whole game runs in slow motion.
 *
 * Decouple the sim from rendering: advance the game by REAL elapsed time (one
 * 1/30 s step per due interval) and render only the newest step.  A slow GPU
 * then drops *visual* frames instead of slowing the game down — it stays at
 * real-time speed (choppier, not slower).  Catch-up is capped so a very slow
 * scene can't spiral; below that floor the game still slows (nothing to do but
 * render faster).  Frameskip is the only safe fix here — delta-time scaling
 * would break SM64's frame-counted collision/animation/cutscene logic. */
#define SIM_DT_US    33333u   /* 1/30 s */
#define MAX_CATCHUP  3        /* render-cycle advances at most this many steps  */

static bool g_drop_frame = false;

static void gfx_of_main_loop(void (*run_one_game_iter)(void)) {
    static unsigned sim_due = 0;
    static bool     init    = false;
    unsigned now = of_time_us();
    if (!init) { sim_due = now; init = true; }

    /* How many fixed steps are due by now? (capped) */
    int steps = 0;
    while ((int)(now - sim_due) >= 0 && steps < MAX_CATCHUP) {
        steps++;
        sim_due += SIM_DT_US;
    }
    /* Hopelessly behind (resume from pause, or sustained <10 fps): resync the
     * clock instead of accumulating an unpayable debt. */
    if ((int)(now - sim_due) > (int)(SIM_DT_US * 8u))
        sim_due = now + SIM_DT_US;

    if (steps == 0)
        return;   /* ahead of schedule: hold 30 Hz, don't run the game fast */

    for (int i = 0; i < steps; i++) {
        g_drop_frame = (i < steps - 1);   /* render only the final (newest) step */
        run_one_game_iter();
    }
    g_drop_frame = false;
}

static void gfx_of_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = SCREEN_WIDTH;
    *height = SCREEN_HEIGHT;
}

static void gfx_of_handle_events(void) {
}

static bool gfx_of_start_frame(void) {
    /* false -> gfx_run() drops this frame: skips the vertex transform, draws
     * and the present/flip (the slow GPU work), but the game logic for this
     * 1/30 s step already ran.  This is the catch-up frameskip. */
    return !g_drop_frame;
}

static void gfx_of_swap_buffers_begin(void) {
    gfx_gpu_present();
}

static void gfx_of_swap_buffers_end(void) {
}

static double gfx_of_get_time(void) {
    return (double)of_time_us() * 1e-6;
}

static void gfx_of_shutdown(void) {
}

struct GfxWindowManagerAPI gfx_pocket_wm_api = {
    gfx_of_init,
    gfx_of_set_keyboard_callbacks,
    gfx_of_set_fullscreen_changed_callback,
    gfx_of_set_fullscreen,
    gfx_of_main_loop,
    gfx_of_get_dimensions,
    gfx_of_handle_events,
    gfx_of_start_frame,
    gfx_of_swap_buffers_begin,
    gfx_of_swap_buffers_end,
    gfx_of_get_time,
    gfx_of_shutdown,
};

#endif /* TARGET_OPENFPGA */
