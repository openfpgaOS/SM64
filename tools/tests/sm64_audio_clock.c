/* The virtual ring must advance 60 sequence ticks per second. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint32_t now;
unsigned of_time_us(void) { return now; }
void of_voice_init(void) {}
void of_audio_init(void) {}
#include "pocket/audio_pocket.c"
int32_t gSamplesPerFrameTarget=368;
static void run(uint32_t start, unsigned interval, unsigned duration) {
    now=start;
    audio_of_init();
    unsigned produced=0,elapsed=0;
    while(elapsed<duration) {
        unsigned step=interval+(elapsed%113);
        if(step>duration-elapsed) step=duration-elapsed;
        elapsed+=step; now=start+elapsed;
        while(audio_of_buffered()<audio_of_get_desired_buffered()) {
            audio_of_play(NULL,gSamplesPerFrameTarget*4);
            produced++;
        }
    }
    uint64_t drained=(uint64_t)(produced*gSamplesPerFrameTarget-s_queued);
    /* First polling instant starts with an empty ring; that debt is dropped. */
    uint64_t expected=(uint64_t)(duration-interval)*gSamplesPerFrameTarget*60/1000000;
    assert(drained==expected || drained==expected+1);
    assert(s_queued>=2000 && s_queued<2000+gSamplesPerFrameTarget);
}
int main(void) {
    run(0,16666,10000000);
    run(UINT32_MAX-500000,33333,10000000);
    gSamplesPerFrameTarget=384;
    run(12345,50000,10000000);
    puts("audio: exact 60 Hz pacing, fractional carry and timer wrap passed");
}
