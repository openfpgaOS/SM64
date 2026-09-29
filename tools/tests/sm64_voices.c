/* Compare note/voice ownership with a deterministic hardware-mixer model. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "of_mixer.h"
static uint32_t active_mask, ended_mask, stops, checks;
static uint64_t digest=1469598103934665603ULL;
static void hash(uint32_t x) { digest^=x; digest*=1099511628211ULL; }
static int test_voice_active(int v) { checks++; return (active_mask>>v)&1; }
static void stop(int v) { active_mask&=~(1u<<v); stops|=1u<<v; }
static uint32_t test_voice_ended(void) { active_mask&=~ended_mask; return ended_mask; }
static void raw(int v,uint32_t rate,int left,int right) {
    hash(v); hash(rate); hash(left); hash(right);
}
#define of_mixer_voice_active test_voice_active
#define of_mixer_stop stop
#define of_mixer_poll_ended test_voice_ended
#define of_mixer_set_voice_raw raw
#include "of_voice.c"
s32 gAiFrequency=22050;
s32 gMaxSimultaneousNotes;
struct Note *gNotes;
const int16_t *audio_predecode_get(const uint8_t *a,const int16_t *b,int c,int d,
 uint32_t e,uint32_t f,uint32_t g,const int16_t *h) { return NULL; }
static uint32_t seed=12356789;
static uint32_t next(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
int main(void) {
    struct Note notes[32]; gNotes=notes;
    for(unsigned trial=0;trial<10000;trial++) {
        memset(notes,0,sizeof(notes));
        gMaxSimultaneousNotes=next()%33;
        active_mask=next(); ended_mask=next(); stops=0;
        for(int i=0;i<32;i++) {
            s_voice[i]=i<gMaxSimultaneousNotes ? (i*7)%32 : -1;
            notes[i].priority=(next()%4) ? 20 : NOTE_PRIORITY_DISABLED;
            notes[i].frequency=0.25f+i/32.0f;
            notes[i].targetVolLeft=next()&0x7fff;
            notes[i].targetVolRight=next()&0x7fff;
        }
        of_voice_sync();
        uint32_t owned=0;
        for(int i=0;i<gMaxSimultaneousNotes;i++) {
            hash(s_voice[i]); hash(notes[i].finished);
            if(s_voice[i]>=0) owned|=1u<<s_voice[i];
        }
        assert(!(active_mask&~owned));
        hash(stops); hash(active_mask);
    }
    printf("voices %016llx active_queries %u\n",(unsigned long long)digest,checks);
}
