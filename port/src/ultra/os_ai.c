/* AI (audio interface): the game hands over 16-bit stereo buffers at the rate
 * it set with osAiSetFrequency and uses osAiGetLength to size the next one.
 * The platform layer keeps a ring buffer that the SDL callback drains. */
#include "ultra.h"
#include "sbk_os.h"
#include "../platform/audio_out.h"

static u32 sbk_ai_freq = 22050;

s32 osAiSetFrequency(u32 frequency) {
    sbk_ai_freq = frequency;
    sbk_audio_out_set_rate(frequency);
    return (s32)frequency;
}

/* Until the aspMain interpreter exists (milestone 3) the game's output
 * buffers hold whatever was in memory: queue silence of the same length so
 * the AI timing (osAiGetLength) still behaves. */
int sbk_audio_task_implemented = 0;

s32 osAiSetNextBuffer(void *bufPtr, u32 size) {
    void *host = sbk_phys_to_host((u32)(uintptr_t)bufPtr); /* the game passes a physical address */
    size &= 0x3FFF8; /* AI_LEN_REG is 18 bits; the game's first request is uninitialised */
    if (!sbk_audio_task_implemented) {
        static u8 silence[8192];
        if (size > sizeof(silence)) {
            size = sizeof(silence);
        }
        sbk_audio_out_queue(silence, size);
        return 0;
    }
    sbk_audio_out_queue(host, size);
    return 0;
}

u32 osAiGetLength(void) {
    return sbk_audio_out_queued_bytes();
}

u32 osAiGetStatus(void) {
    return 0;
}
