//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * audio_pocket.c -- openfpgaOS audio backend for SM64.
 *
 * SM64 now drives the os30 HW mixer DIRECTLY, one voice per note (resample +
 * envelope + mix in hardware): see pc/of_voice.c, called from synthesis_execute.
 * This file therefore no longer renders or streams PCM -- it only provides the
 * AudioAPI the engine's pump expects, as a VIRTUAL ring that paces the sequence
 * player in real time (play() counts produced samples; buffered() drains them by
 * wall-clock at SM64_RATE), so the seq advances ~60x/s regardless of render fps
 * with no PCM output (the HW voices are the sound).  of_voice_init() is started
 * from audio_of_init().
 */

#ifdef TARGET_OPENFPGA

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "audio/audio_api.h"

#include "of.h"
#include "of_audio.h"

/* MUST match the engine's gAiFrequency (audio/heap.c cheap-knob rate cap): the
 * HW mixer voice resamples this -> 48 kHz, so a mismatch pitch-shifts audio. */
#define SM64_RATE   22050

/* HW-VOICE BACKEND (pc/of_voice.c) is now the sound source: SM64 drives one
 * os30 HW-mixer voice per note (resample+envelope+mix in HW).  This file no
 * longer opens a stream voice or pushes PCM -- that would DOUBLE the audio.
 * But the sequence player must still advance in REAL TIME (not tied to render
 * fps), and the decoupled pump in produce_one_frame paces off buffered()/
 * get_desired_buffered().  So we keep a VIRTUAL ring: play() only COUNTS the
 * samples "produced"; buffered() drains that count by wall-clock at SM64_RATE.
 * The pump then calls create_next_audio_buffer() ~60x/s regardless of fps (each
 * call advances the seq 1/60 s and runs of_voice_sync()), with no PCM output. */
#include "of_timer.h"
#define TMR()       ((uint32_t)of_time_us())
#define TMR_PER_SEC 1000000u

extern void of_voice_init(void);

static int      s_queued;    /* virtual pairs "in flight" (real-time pacer) */
static uint32_t s_lastTick;
static uint32_t s_rem;       /* fractional-tick carry for the drain */

static bool audio_of_init(void) {
    of_audio_init();
    of_voice_init();         /* HW-mixer per-note backend owns playback */
    s_queued = 0;
    s_lastTick = TMR();
    s_rem = 0;
    return true;
}

static int audio_of_buffered(void) {
    uint32_t now = TMR();
    uint32_t dt = now - s_lastTick;     /* unsigned wrap-safe microseconds */
    /* Each synthesized buffer advances one 60 Hz sequence tick. The engine
     * rounds its sample count to 16, so draining at gAiFrequency would still
     * slow the sequence down. Match the actual buffer count, at every clock. */
    extern int32_t gSamplesPerFrameTarget;
    uint32_t rate = gSamplesPerFrameTarget > 0
                  ? (uint32_t)gSamplesPerFrameTarget * 60u : SM64_RATE;
    uint64_t num = (uint64_t) dt * rate + s_rem;
    uint32_t drained = (uint32_t)(num / TMR_PER_SEC);
    s_lastTick = now;
    s_rem = (uint32_t)(num % TMR_PER_SEC);
    s_queued -= (int) drained;
    if (s_queued < 0) s_queued = 0;
    return s_queued;
}

static int audio_of_get_desired_buffered(void) {
    /* Virtual-ring cushion the pump refills to (SM64_RATE pairs/s): 2000 pairs
     * ~= 91 ms, so the seq stays ahead across a slow render frame. */
    return 2000;
}

static void audio_of_play(const uint8_t *buf, size_t len) {
    /* Count-only: the HW voices make the sound; this just advances the virtual
     * ring so the pump paces the sequence player in real time. */
    (void) buf;
    s_queued += (int)(len / 4);
}

struct AudioAPI audio_pocket = {
    audio_of_init,
    audio_of_buffered,
    audio_of_get_desired_buffered,
    audio_of_play,
    NULL, /* shutdown */
};

/*
 * Sound banks (gSoundDataADSR/Raw, gMusicData, gBankSetsData) are bundled into
 * the ELF via sm64/sound/sound_data.c, which #includes the generated .inc.c
 * blobs (assembled by the Makefile sound rules with --endian little
 * --bitwidth 32 to match the rv32 target).  No stubs here any more.
 */

#endif /* TARGET_OPENFPGA */
