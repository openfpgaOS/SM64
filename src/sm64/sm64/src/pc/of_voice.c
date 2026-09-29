/* of_voice.c -- HW-mixer per-note audio backend for SM64 on openfpgaOS.
 *
 * One os30 HW-mixer voice per active Note. Each audio sub-update, process_notes
 * (CPU) decides every note's pitch (note->frequency), stereo volume
 * (note->targetVolLeft/Right, with velocity*ADSR^2*pan already folded in) and
 * loop (note->sound->sample->loop); of_voice_sync() pushes those to HW voices,
 * which do the resample + volume-ramp + mix. This replaces the CPU software
 * synthesis (decode/resample/env-mix), the ~17-85 ms/frame cost.
 *
 * Sample PCM comes from the VADPCM pre-decode (audio_predecode_get, pc/mixer.c):
 * VADPCM -> S16 in SDRAM, 1:1 sample mapping (loop points stay in sample units).
 *
 * Uses the NON-handle (voice-index) of_mixer API on purpose: those calls are
 * always present in any mixer-capable os.bin (the _h variants are guarded by
 * OF_SVC_HAS_FIELD and would no-op to silence on an older os.bin). At 12 notes
 * over 32 voices the firmware never needs to steal our voices, and we gate every
 * sustain on of_mixer_voice_active(), so the voice-index ABA risk is negligible. */
#ifdef TARGET_OPENFPGA

#include <stdint.h>

#include "of.h"
#include "of_mixer.h"

#include "audio/internal.h"   /* struct Note, AudioBank*, Adpcm*, NOTE_PRIORITY_* */
#include "audio/load.h"       /* gNotes, gMaxSimultaneousNotes */

#include "of_voice.h"

extern s32 gAiFrequency;
extern const int16_t *audio_predecode_get(const uint8_t *sampleAddr,
        const int16_t *book, int order, int npred, uint32_t sampleSize,
        uint32_t loopStart, uint32_t loopCount, const int16_t *loopState);

/* gMaxSimultaneousNotes is the preset's stock value (16, or 20 for preset 7) now
 * that the heap.c clamp-to-12 is gone; 32 = HW voice max (we have headroom). */
#define OF_VOICE_MAX 32

/* HW volume-ramp step (click-free volume glide between 60 Hz target updates).
 * EAR-TUNE: higher = snappier attacks, lower = smoother but mushier. */
#define OF_VOICE_RAMP 8

static int s_voice[OF_VOICE_MAX];   /* HW voice index per note slot, -1 = none */

void of_voice_init(void) {
    int i;
    of_mixer_init(OF_MIXER_MAX_VOICES, OF_MIXER_OUTPUT_RATE);
    for (i = 0; i < OF_VOICE_MAX; i++) {
        s_voice[i] = -1;
    }
}

static inline int clamp255(int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); }

void of_voice_sync(void) {
    int n = gMaxSimultaneousNotes;
    int i;
    uint32_t ended;
    float K;

    if (gNotes == NULL || gAiFrequency <= 0) {
        return;
    }
    if (n > OF_VOICE_MAX) {
        n = OF_VOICE_MAX;
    }

    /* rate_fp16 = note->frequency * K.  Real-time consumption equivalence with
     * the old SW->stream path: SW resampled at note->frequency, output at
     * gAiFrequency; HW out = 48000.  So K = gAiFrequency * 65536 / 48000.
     * *** PITCH KNOB: if HW audio is uniformly sharp/flat, this is the constant
     *     to check (gAiFrequency interaction); a 32000-based K=43691 is the
     *     alternative if note->frequency is NOT pre-scaled by 32000/gAiFrequency. */
    K = (float)gAiFrequency * (65536.0f / 48000.0f);

    for (i = 0; i < n; i++) {
        struct Note *note = &gNotes[i];
        int v = s_voice[i];

        /* A: inactive -> stop the voice, drop the mapping. */
        if (note->priority == NOTE_PRIORITY_DISABLED) {
            if (v >= 0) {
                of_mixer_stop(v);
                s_voice[i] = -1;
            }
            continue;
        }

        /* B: (re)start -- no voice yet, or the Note slot was re-initialised. */
        if (v < 0 || note->needsInit) {
            struct AudioBankSound *snd = note->sound;
            struct AudioBankSample *smp;
            struct AdpcmLoop *loop;
            struct AdpcmBook *book;
            const int16_t *pcm;
            uint32_t nSamples, loopStart, loopEnd, loopCount, rate;
            int vl, vr;

            /* Retire the old voice FIRST, before any early-out below -- otherwise
             * a looped voice plays forever ("stuck note") when this slot is reused
             * by a synthetic-wave (sound==NULL) or undecodable note. */
            if (v >= 0) {
                of_mixer_stop(v);
                s_voice[i] = -1;
                v = -1;
            }

            if (snd == NULL) {
                /* Synthetic wave (sawtooth/triangle/sine/square): SM64 built the
                 * 64-sample cycle in note->synthesisBuffers->samples (sub-sampled +
                 * repeated per note->sampleCount, cache-flushed) in
                 * note_init_for_layer.  Play it looped, same pitch/vol mapping as a
                 * sample (note->frequency already carries the wave freqScale). */
                if (note->instOrWave < 0x80 || note->synthesisBuffers == NULL) {
                    continue;   /* not a valid wave -> silent */
                }
                pcm       = (const int16_t *) note->synthesisBuffers->samples;
                nSamples  = 0x40u;   /* 64-sample wave buffer */
                loopStart = 0u;
                loopEnd   = 0x40u;
                loopCount = 1u;      /* loop the cycle forever */
            } else {
                smp = snd->sample;
                if (smp == NULL) {
                    continue;
                }
                loop = smp->loop;
                book = smp->book;
                loopStart = loop ? loop->start : 0u;
                loopEnd   = loop ? loop->end   : 0u;
                loopCount = loop ? loop->count : 0u;

                pcm = audio_predecode_get(smp->sampleAddr,
                                          book ? book->book : NULL,
                                          book ? (int)book->order : 0,
                                          book ? (int)book->npredictors : 0,
                                          smp->sampleSize, loopStart, loopCount,
                                          loop ? loop->state : NULL);
                if (pcm == NULL) {
                    s_voice[i] = -1;
                    continue;       /* un-decodable sample -> silent (no crash) */
                }
                nSamples = (smp->sampleSize / 9u) * 16u;
            }
            /* (old voice already retired at the top of this block) */

            rate = (uint32_t)(note->frequency * K);
            vl = clamp255((int)note->targetVolLeft  >> 8);
            vr = clamp255((int)note->targetVolRight >> 8);

            /* Start silent, then ramp to target -> click-free attack. */
            v = of_mixer_play((const uint8_t *)pcm, nSamples,
                              (uint32_t)gAiFrequency, note->priority, 0);
            s_voice[i] = v;
            if (v < 0) {
                continue;
            }
            of_mixer_set_group(v, OF_MIXER_GROUP_MUSIC);      /* TODO: SFX split */
            of_mixer_set_volume_ramp(v, OF_VOICE_RAMP);
            if (loopCount != 0u) {
                of_mixer_set_loop(v, (int)loopStart, (int)loopEnd);
            }
            of_mixer_set_voice_raw(v, rate, vl, vr);
            note->needsInit = 0;
            continue;
        }

        /* C: sustain -- 1 call/note/frame; HW resamples + ramps + mixes.
         * D (release) is handled here too: process_notes keeps ADSR-ing a
         * STOPPING note so targetVol falls to 0 and the HW ramp fades it. */
        if (!of_mixer_voice_active(v)) {
            s_voice[i] = -1;
            continue;
        }
        {
            uint32_t rate = (uint32_t)(note->frequency * K);
            int vl = clamp255((int)note->targetVolLeft  >> 8);
            int vr = clamp255((int)note->targetVolRight >> 8);
            /* Release finished (faded to silent but not yet DISABLED): stop now so a
             * looped voice can't linger.  Only when STOPPING/lower -- a PLAYING note
             * momentarily at vol 0 (tremolo) must keep its voice. */
            if (vl == 0 && vr == 0 && note->priority <= NOTE_PRIORITY_STOPPING) {
                of_mixer_stop(v);
                s_voice[i] = -1;
                continue;
            }
            of_mixer_set_voice_raw(v, rate, vl, vr);
        }
    }

    /* Retire voices that ended in HW (one-shot SFX/drums): tell SM64 the note
     * finished so process_notes frees it through its existing path. */
    ended = of_mixer_poll_ended();
    if (ended != 0u) {
        for (i = 0; i < n; i++) {
            int v = s_voice[i];
            if (v >= 0 && (ended & (1u << v)) != 0u) {
                s_voice[i] = -1;
                gNotes[i].finished = 1;
            }
        }
    }

    /* GC stuck/orphaned voices: of_voice owns ALL HW voices (the stream voice is
     * retired), so any voice still PLAYING but no longer owned by a live note slot
     * has leaked -- a looped voice whose stop we missed -> stop it.  Robust
     * catch-all for "notes stay on" regardless of cause. */
    {
        uint32_t owned = 0;
        for (i = 0; i < n; i++) {
            if (s_voice[i] >= 0) {
                owned |= 1u << s_voice[i];
            }
        }
        for (int vi = 0; vi < OF_MIXER_MAX_VOICES; vi++) {
            /* Owned voices were checked in the sustain path already. */
            if (!(owned & (1u << vi)) && of_mixer_voice_active(vi))
                of_mixer_stop(vi);
        }
    }
}

#endif /* TARGET_OPENFPGA */
