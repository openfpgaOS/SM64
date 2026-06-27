/*
 * audio_pocket.c -- openfpgaOS audio backend for SM64.
 *
 * Implements AudioAPI over of_audio.h.  SM64 generates 32 kHz 16-bit stereo
 * PCM.  We open the OS stream voice at 32 kHz (of_audio_stream_open) and push
 * the raw pairs straight to the ring: the HW mixer voice's per-voice rate
 * engine resamples 32k -> 48k in hardware (2-tap linear interp), so there is
 * NO CPU upsample and ~1/3 fewer uncached ring writes than feeding 48 kHz.
 * Falls back cleanly to the SW mixer when the HW mixer is absent (it does the
 * same rate conversion in its render).
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

static int s_capacity;     /* ring depth in stereo pairs (probed at init) */

static bool audio_of_init(void) {
    of_audio_init();
    /* Open the stream voice at SM64's native 32 kHz so the HW mixer's per-voice
     * rate engine does the 32->48 kHz resample instead of the CPU. */
    of_audio_stream_open(SM64_RATE);
    s_capacity = of_audio_free();   /* full capacity while the ring is empty */
    if (s_capacity <= 0) s_capacity = OF_AUDIO_FIFO;
    return true;
}

static int audio_of_buffered(void) {
    int free = of_audio_free();
    int q = s_capacity - free;
    return q < 0 ? 0 : q;
}

static int audio_of_get_desired_buffered(void) {
    /* Ring cushion the decoupled audio pump (produce_one_frame) refills to each
     * frame.  Must exceed the worst single render-frame drain or audio underruns
     * between refills: the os30 renderer hits ~50-70 ms frames in busy scenes.
     * The voice consumes at SM64_RATE (22.05 kHz; HW resamples to 48k), so these
     * pairs are 22.05 kHz units: 2000 pairs ~= 91 ms (covers a 70 ms frame with
     * margin).  Larger = fewer underruns but more latency. */
    return 2000;
}

/* Push `count` stereo pairs, honouring ring space (drop the tail if full). */
static void flush_pairs(const int16_t *pairs, int count) {
    int w = 0;
    while (w < count) {
        int room = of_audio_free();
        if (room <= 0) break;
        int c = count - w;
        if (c > room) c = room;
        of_audio_write(pairs + w * 2, c);
        w += c;
    }
}

static void audio_of_play(const uint8_t *buf, size_t len) {
    /* Raw 32 kHz stereo straight to the ring; the HW mixer voice resamples to
     * 48 kHz (rate set in audio_of_init).  No CPU upsample. */
    flush_pairs((const int16_t *)buf, (int)(len / 4));
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
