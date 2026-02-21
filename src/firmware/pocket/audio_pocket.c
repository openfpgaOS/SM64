/*
 * audio_pocket.c -- Analogue Pocket audio backend for SM64
 *
 * Implements AudioAPI for the FPGA I2S audio FIFO.
 * SM64 generates 32kHz 16-bit stereo PCM; we upsample to 48kHz
 * using Bresenham resampling and push to the FPGA FIFO.
 */

#ifdef TARGET_POCKET

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "audio/audio_api.h"

/* Audio MMIO registers (FPGA audio_output module) */
#define AUDIO_SAMPLE    (*(volatile uint32_t *)0x4C000000)  /* Write: push {L16, R16} */
#define AUDIO_STATUS    (*(volatile uint32_t *)0x4C000004)  /* Read: [11:0]=fifo level, [12]=full */

#define AUDIO_FIFO_SIZE 4096
#define SM64_SAMPLE_RATE 32000
#define OUTPUT_SAMPLE_RATE 48000

/* Upsampling state */
static int upsample_frac;

static bool audio_pocket_init(void) {
    upsample_frac = 0;
    return true;
}

static int audio_pocket_buffered(void) {
    /* Return number of samples in FIFO */
    return AUDIO_STATUS & 0xFFF;
}

static int audio_pocket_get_desired_buffered(void) {
    /* Keep ~1100 samples buffered (~23ms at 48kHz) */
    return 1100;
}

static void audio_pocket_play(const uint8_t *buf, size_t len) {
    const int16_t *samples = (const int16_t *)buf;
    int num_stereo_pairs = len / 4; /* len is in bytes, each stereo pair is 4 bytes */
    int fifo_level, fifo_space;

    for (int i = 0; i < num_stereo_pairs; i++) {
        int16_t left = samples[i * 2];
        int16_t right = samples[i * 2 + 1];

        /* Upsample from SM64_SAMPLE_RATE to OUTPUT_SAMPLE_RATE using Bresenham.
         * For each source sample, output ceil(OUTPUT/SM64) samples on average. */
        upsample_frac += OUTPUT_SAMPLE_RATE;
        while (upsample_frac >= SM64_SAMPLE_RATE) {
            upsample_frac -= SM64_SAMPLE_RATE;

            /* Check FIFO space before pushing */
            fifo_level = AUDIO_STATUS & 0xFFF;
            fifo_space = AUDIO_FIFO_SIZE - fifo_level;
            if (fifo_space <= 1)
                return; /* FIFO full, drop remaining samples */

            /* Write stereo pair: {left[15:0], right[15:0]} */
            AUDIO_SAMPLE = ((uint32_t)(uint16_t)left << 16) | (uint16_t)right;
        }
    }
}

struct AudioAPI audio_pocket = {
    audio_pocket_init,
    audio_pocket_buffered,
    audio_pocket_get_desired_buffered,
    audio_pocket_play,
    NULL, /* shutdown */
};

/*
 * Sound data stubs - on N64 these are linked from assembled ROM data.
 * On Pocket they will eventually be loaded from the data slot.
 * For now, provide empty arrays so the linker is happy (sound is disabled).
 */
uint8_t gSoundDataADSR[1] __attribute__((aligned(16))) = {0};
uint8_t gSoundDataRaw[1]  __attribute__((aligned(16))) = {0};
uint8_t gMusicData[1]     __attribute__((aligned(16))) = {0};
uint8_t gBankSetsData[1]  __attribute__((aligned(16))) = {0};

#endif /* TARGET_POCKET */
