/* of_voice.h -- HW-mixer per-note audio backend for SM64 on openfpgaOS.
 *
 * Drives one os30 HW-mixer voice per active Note: pitch (resample), volume
 * (envelope ramp), loop and mix all run in hardware (of_mixer.h), replacing the
 * CPU software synthesis render. The CPU keeps the sequence player + ADSR (the
 * "decide" half, process_notes); HW does the "render" half. Sample data is
 * pre-decoded VADPCM->S16 PCM in SDRAM (audio_predecode_get, pc/mixer.c). */
#ifndef OF_VOICE_H
#define OF_VOICE_H

/* Init the HW mixer + clear the note<->voice map. Call once at audio init. */
void of_voice_init(void);

/* Push the current per-note state (pitch/volume/loop) to HW voices, start/stop
 * voices as notes begin/end, and retire finished one-shots. Call once per audio
 * sub-update after process_sequences (from synthesis_execute). */
void of_voice_sync(void);

#endif /* OF_VOICE_H */
