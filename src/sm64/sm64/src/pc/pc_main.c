#include <stdlib.h>
#ifdef TARGET_WEB
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include "sm64.h"

#include "game/memory.h"
#include "audio/external.h"

#include "gfx/gfx_pc.h"
#include "gfx/gfx_opengl.h"
#include "gfx/gfx_soft.h"
#include "gfx/gfx_direct3d11.h"
#include "gfx/gfx_direct3d12.h"
#include "gfx/gfx_dos_api.h"
#include "gfx/gfx_dxgi.h"
#include "gfx/gfx_glx.h"
#include "gfx/gfx_sdl.h"

#include "audio/audio_api.h"
#ifdef TARGET_OPENFPGA
#include "pocket/audio_pocket.h"
#include "pocket/wm_pocket.h"
#include "gfx/gfx_gpu.h"
#else
#include "audio/audio_sb16.h"
#include "audio/audio_wasapi.h"
#include "audio/audio_pulse.h"
#include "audio/audio_alsa.h"
#include "audio/audio_sdl.h"
#include "audio/audio_null.h"
#endif

#ifndef TARGET_OPENFPGA
#include "controller/controller_keyboard.h"
#endif

#include "configfile.h"

#include "compat.h"

#define CONFIG_FILE "sm64config.txt"

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;

s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

static struct AudioAPI *audio_api;
static struct GfxWindowManagerAPI *wm_api;
static struct GfxRenderingAPI *rendering_api;

extern void gfx_run(Gfx *commands);
extern void thread5_game_loop(void *arg);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {
}

void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) {
}

static uint8_t inited = 0;

#include "game/game_init.h" // for gGlobalTimer
void send_display_list(struct SPTask *spTask) {
    if (!inited) {
        return;
    }
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#define printf

#ifdef VERSION_EU
#define SAMPLES_HIGH 656
#define SAMPLES_LOW 640
#else
#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528
#endif

#ifdef TARGET_OPENFPGA
/* Decoupled-audio cap: the most audio sub-frames produce_one_frame may
 * synthesize in one render frame while catching the HW-mixer ring up to its
 * target.  8 * ~17 ms ~= 136 ms of audio -- enough to cover any single slow
 * render frame, while bounding the worst-case CPU spent on audio per frame so
 * a long frame can't spiral. */
#define AUDIO_MAX_SUBFRAMES 8
/* Free-running hardware cycle counter.  The desktop build also defines
 * TARGET_OPENFPGA but has no such MMIO, so stub it there. */
#ifdef OF_PC
#define AUDIO_PROF_TMR() 0u
#else
#define AUDIO_PROF_TMR() (*(volatile uint32_t *)0x40000004)
#endif
#endif

/* SM64 #defines printf to nothing (above) and term_printf is TARGET_POCKET-only
 * (unlinked in this build), so re-enable real musl printf for the OTHER line —
 * same source it reaches from gfx_pc.c's PERF line.  Nothing else in this file
 * uses printf, so the undef is safe for the remainder. */
#undef printf

void produce_one_frame(void) {
    gfx_start_frame();
    game_loop_one_iteration();

    if (configEnableSound) {
#ifdef TARGET_OPENFPGA
        /* Time the pump and ACCUMULATE (dropped catch-up frames pump too, and
         * their cost lands in the next rendered frame's PERF `other`).  This is
         * the one number that tells cap-idle apart from real work in `other`:
         * a light frame idling at the 30 Hz sim cap shows audio ~0, whereas a
         * first-touch VADPCM predecode shows up here directly. */
#if SM64_PROFILE
        extern unsigned g_prof_audio;
        uint32_t _au0 = AUDIO_PROF_TMR();
#endif
        /* Decouple audio from render fps.  The HW mixer drains the ring at a
         * constant 48 kHz, so producing until the ring holds `target` pairs
         * yields ~60 audio updates/sec (correct music tempo) no matter how
         * slow the renderer runs, and keeps the ring from underrunning across
         * a long frame.  Bounded by AUDIO_MAX_SUBFRAMES so a very slow frame
         * can't spiral.  At a steady 30 fps this settles to ~2 sub-frames per
         * frame -- the same cadence as the original fixed loop. */
        s16 audio_buffer[SAMPLES_HIGH * 2];
        /* Produce the engine's exact per-1/60s-frame sample count (gAiFrequency/60)
         * so each call advances the sequence one audio frame and the music tempo
         * stays correct at whatever synthesis rate is configured (it auto-tracks
         * the cheap-knob rate change).  Clamped to the buffer. */
        extern s32 gSamplesPerFrameTarget;
        int frame_samples = (int)gSamplesPerFrameTarget;
        if (frame_samples <= 0 || frame_samples > SAMPLES_HIGH) frame_samples = SAMPLES_HIGH;
        int target = audio_api->get_desired_buffered();
        for (int i = 0; i < AUDIO_MAX_SUBFRAMES; i++) {
            if (audio_api->buffered() >= target) break;
            create_next_audio_buffer(audio_buffer, frame_samples);
            audio_api->play((u8 *)audio_buffer, frame_samples * 4);
        }
#if SM64_PROFILE
        g_prof_audio += AUDIO_PROF_TMR() - _au0;
#endif
#else
        int samples_left = audio_api->buffered();
        u32 num_audio_samples = samples_left < audio_api->get_desired_buffered() ? SAMPLES_HIGH : SAMPLES_LOW;
        s16 audio_buffer[SAMPLES_HIGH * 2 * 2];
        for (int i = 0; i < 2; i++) {
            create_next_audio_buffer(audio_buffer + i * (num_audio_samples * 2), num_audio_samples);
        }
        audio_api->play((u8 *)audio_buffer, 2 * num_audio_samples * 4);
#endif
    }

    gfx_end_frame();
}

#ifdef TARGET_WEB
static void em_main_loop(void) {
}

static void request_anim_frame(void (*func)(double time)) {
    EM_ASM(requestAnimationFrame(function(time) {
        dynCall("vd", $0, [time]);
    }), func);
}

static void on_anim_frame(double time) {
    static double target_time;

    time *= 0.03; // milliseconds to frame count (33.333 ms -> 1)

    if (time >= target_time + 10.0) {
        // We are lagging 10 frames behind, probably due to coming back after inactivity,
        // so reset, with a small margin to avoid potential jitter later.
        target_time = time - 0.010;
    }

    for (int i = 0; i < 2; i++) {
        // If refresh rate is 15 Hz or something we might need to generate two frames
        if (time >= target_time) {
            produce_one_frame();
            target_time = target_time + 1.0;
        }
    }

    request_anim_frame(on_anim_frame);
}
#endif

static void save_config(void) {
    configfile_save(CONFIG_FILE);
}

static void on_fullscreen_changed(bool is_now_fullscreen) {
    configFullscreen = is_now_fullscreen;
}

void game_exit(void) {
    if (audio_api && audio_api->shutdown) audio_api->shutdown();
    gfx_shutdown();
    exit(0);
}

void main_func(void) {
    static u64 pool[0x165000/8 / 4 * sizeof(void *)];

#ifdef TARGET_POCKET
    /* Initialize the heap allocator before anything that uses malloc/calloc.
     * Use a region in SDRAM after assets and BSS. */
    extern void heap_init(void *start, size_t size);
    extern char _heap_start[], _heap_end[];
    heap_init(_heap_start, (size_t)(_heap_end - _heap_start));
#endif

    main_pool_init(pool, pool + sizeof(pool) / sizeof(pool[0]));
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

#ifndef TARGET_OPENFPGA
    configfile_load(CONFIG_FILE);
    atexit(save_config);
#else
    /* Pocket: use hardcoded defaults, no config file */
    configScreenWidth = 320;
    configScreenHeight = 240;
    configEnableSound = true;   /* Sound banks bundled via sm64/sound/sound_data.c */
    configFullscreen = false;
#ifdef OF_DBG_SHADE
    /* SHADE probe: host desktop build has no N64 ROM-DMA path for the sound
     * banks (osPiStartDma faults); the title-screen head renders without audio,
     * so silence it while capturing the per-vertex SHADE diagnostic. */
    configEnableSound = false;
#endif
#endif

#ifdef TARGET_WEB
    emscripten_set_main_loop(em_main_loop, 0, 0);
    request_anim_frame(on_anim_frame);
#endif

#if defined(TARGET_OPENFPGA)
    rendering_api = &gfx_gpu_api;
    wm_api = &gfx_pocket_wm_api;
#elif defined(ENABLE_DX12)
    rendering_api = &gfx_direct3d12_api;
    wm_api = &gfx_dxgi_api;
#elif defined(ENABLE_DX11)
    rendering_api = &gfx_direct3d11_api;
    wm_api = &gfx_dxgi_api;
#elif defined(ENABLE_OPENGL) || defined(ENABLE_OPENGL_LEGACY)
    rendering_api = &gfx_opengl_api;
    #if defined(__linux__) || defined(__BSD__)
        wm_api = &gfx_glx;
    #elif defined(TARGET_DOS)
        wm_api = &gfx_dos_api;
    #else
        wm_api = &gfx_sdl;
    #endif
#elif defined(ENABLE_SOFTRAST)
    rendering_api = &gfx_soft_api;
    #if defined(TARGET_DOS)
        wm_api = &gfx_dos_api;
    #else
        wm_api = &gfx_sdl;
    #endif
#else
    #error Could not pick rendering API!
#endif

#ifdef TARGET_POCKET
    { extern void term_printf(const char *fmt, ...);
      term_printf("[SM64] gfx_init...\n"); }
#endif
    gfx_init(wm_api, rendering_api, "Super Mario 64 PC-Port", configFullscreen);
#ifdef TARGET_POCKET
    { extern void term_printf(const char *fmt, ...);
      term_printf("[SM64] gfx_init done\n"); }
#endif

    if (configEnableSound) {
#if defined(TARGET_OPENFPGA)
        if (audio_api == NULL && audio_pocket.init()) {
            audio_api = &audio_pocket;
        }
#elif HAVE_WASAPI
        if (audio_api == NULL && audio_wasapi.init()) {
            audio_api = &audio_wasapi;
        }
#endif
#if HAVE_PULSE_AUDIO
        if (audio_api == NULL && audio_pulse.init()) {
            audio_api = &audio_pulse;
        }
#endif
#if HAVE_ALSA
        if (audio_api == NULL && audio_alsa.init()) {
            audio_api = &audio_alsa;
        }
#endif
#ifdef TARGET_WEB
        if (audio_api == NULL && audio_sdl.init()) {
            audio_api = &audio_sdl;
        }
#endif
#ifdef TARGET_DOS
        if (audio_api == NULL && audio_sb.init()) {
            audio_api = &audio_sb;
        }
#endif
    }

#ifdef TARGET_OPENFPGA
    if (audio_api == NULL) {
        configEnableSound = false;
    }
#else
    if (audio_api == NULL) {
        audio_api = &audio_null;
    }
#endif

    wm_api->set_fullscreen_changed_callback(on_fullscreen_changed);
#ifndef TARGET_OPENFPGA
    wm_api->set_keyboard_callbacks(keyboard_on_key_down, keyboard_on_key_up, keyboard_on_all_keys_up);
#endif

#ifdef TARGET_POCKET
    { extern void term_printf(const char *fmt, ...);
      term_printf("[SM64] audio_init...\n"); }
#endif
    audio_init();
    sound_init();

#ifdef TARGET_POCKET
    { extern void term_printf(const char *fmt, ...);
      term_printf("[SM64] game_loop init...\n"); }
#endif
    thread5_game_loop(NULL);
#ifdef TARGET_POCKET
    { extern void term_printf(const char *fmt, ...);
      term_printf("[SM64] entering main loop\n"); }
#endif
#ifdef TARGET_WEB
    /*for (int i = 0; i < atoi(argv[1]); i++) {
        game_loop_one_iteration();
    }*/
    inited = 1;
#else
    inited = 1;
    while (1) {
        wm_api->main_loop(produce_one_frame);
    }
#endif
}

#if defined(TARGET_OPENFPGA) && !defined(OF_PC)
/* Quick smoke test for FixedPointMacPlugin custom instructions.
 * Uses term_printf directly since SM64 #defines printf away.
 * RISC-V custom .insn — excluded from the host (OF_PC) desktop build, which
 * provides its own main() via pocket/main_of.c. */
static void test_fx_instructions(void) {
    extern void term_printf(const char *fmt, ...);
    int result;
    int pass = 0, fail = 0;

    term_printf("=== FX INSTR TEST ===\n");

    /* FXMUL 1.0 * 1.0 = 1.0 */
    __asm__ volatile(".insn r 0x0B, 0, 0, %0, %1, %2"
        : "=r"(result) : "r"(0x10000), "r"(0x10000));
    term_printf("MUL 1*1=%x %s\n", result, result == 0x10000 ? "OK" : "FAIL");
    if (result == 0x10000) pass++; else fail++;

    /* FXMUL 2.0 * 3.0 = 6.0 */
    __asm__ volatile(".insn r 0x0B, 0, 0, %0, %1, %2"
        : "=r"(result) : "r"(0x20000), "r"(0x30000));
    term_printf("MUL 2*3=%x %s\n", result, result == 0x60000 ? "OK" : "FAIL");
    if (result == 0x60000) pass++; else fail++;

    /* FXMUL (-1) * 2 = -2 */
    __asm__ volatile(".insn r 0x0B, 0, 0, %0, %1, %2"
        : "=r"(result) : "r"((int)0xFFFF0000), "r"(0x20000));
    term_printf("MUL -1*2=%x %s\n", result, result == (int)0xFFFE0000 ? "OK" : "FAIL");
    if (result == (int)0xFFFE0000) pass++; else fail++;

    /* FXMACS/FXMACR: 1*2 + 3*4 = 14.0 */
    __asm__ volatile(".insn r 0x0B, 2, 0, %0, x0, x0" : "=r"(result)); /* clear acc */
    __asm__ volatile(".insn r 0x0B, 1, 0, x0, %0, %1" : : "r"(0x10000), "r"(0x20000));
    __asm__ volatile(".insn r 0x0B, 1, 0, x0, %0, %1" : : "r"(0x30000), "r"(0x40000));
    __asm__ volatile(".insn r 0x0B, 2, 0, %0, x0, x0" : "=r"(result));
    term_printf("MAC 1*2+3*4=%x %s\n", result, result == 0xE0000 ? "OK" : "FAIL");
    if (result == 0xE0000) pass++; else fail++;

    term_printf("FX: %d/%d pass\n\n", pass, pass + fail);
}

/* Pocket entry point: called by bootloader after PSRAM copy */
void sm64_main(void) {
    test_fx_instructions();
    main_func();
}
#elif defined(OF_PC)
/* Host desktop build provides main() via pocket/main_of.c. */
#elif defined(_WIN32) || defined(_WIN64)
#include <windows.h>
int WINAPI WinMain(UNUSED HINSTANCE hInstance, UNUSED HINSTANCE hPrevInstance, UNUSED LPSTR pCmdLine, UNUSED int nCmdShow) {
    main_func();
    return 0;
}
#else
int main(UNUSED int argc, UNUSED char *argv[]) {
    main_func();
    return 0;
}
#endif
