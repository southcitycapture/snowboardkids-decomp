/* AI (audio interface): the game hands over 16-bit stereo buffers at the rate
 * it set with osAiSetFrequency and uses osAiGetLength to size the next one.
 * The platform layer keeps a ring buffer that the SDL callback drains. */
#include "ultra.h"
#include "sbk_os.h"
#include "../platform/audio_out.h"
#include <stdio.h>

static u32 sbk_ai_freq = 22050;

/* What the game sees through osAiGetLength must not depend on the real audio
 * device: simulate the AI draining exactly rate/60 samples per retrace. */
static u32 sbk_ai_sim_queued;
static u32 sbk_ai_sim_frac; /* 16.16 leftover samples */

void sbk_ai_retrace(void) {
    u32 samples = (sbk_ai_freq << 16) / 60 + sbk_ai_sim_frac;
    u32 whole = samples >> 16;
    u32 bytes = whole * 4; /* 16-bit stereo */
    sbk_ai_sim_frac = samples & 0xFFFF;
    sbk_ai_sim_queued = sbk_ai_sim_queued > bytes ? sbk_ai_sim_queued - bytes : 0;
}

s32 osAiSetFrequency(u32 frequency) {
    sbk_ai_freq = frequency;
    sbk_audio_out_set_rate(frequency);
    return (s32)frequency;
}

/* --mute keeps the AI timing but queues silence instead of the game's buffers. */
int sbk_audio_muted = 0;

/* --wav FILE captures everything handed to the AI as a WAV (big-endian
 * "RIFX"-less trick: samples are written as-is and byte-swapped at close). */
static FILE *sbk_ai_dump;
static u32 sbk_ai_dump_bytes;

static void put_le32(FILE *f, u32 v) {
    u8 b[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    fwrite(b, 1, 4, f);
}

int sbk_ai_dump_start(const char *path) {
    sbk_ai_dump = fopen(path, "wb");
    if (sbk_ai_dump == NULL) {
        fprintf(stderr, "sbk: cannot create %s\n", path);
        return -1;
    }
    fseek(sbk_ai_dump, 44, SEEK_SET); /* header written at close */
    printf("sbk: capturing audio to %s\n", path);
    return 0;
}

void sbk_ai_dump_finish(void) {
    FILE *f = sbk_ai_dump;
    if (f == NULL) return;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); put_le32(f, 36 + sbk_ai_dump_bytes);
    fwrite("WAVEfmt ", 1, 8, f); put_le32(f, 16);
    fwrite("\x01\x00\x02\x00", 1, 4, f);        /* PCM, 2 channels */
    put_le32(f, sbk_ai_freq); put_le32(f, sbk_ai_freq * 4);
    fwrite("\x04\x00\x10\x00", 1, 4, f);        /* block align 4, 16 bits */
    fwrite("data", 1, 4, f); put_le32(f, sbk_ai_dump_bytes);
    fclose(f);
    sbk_ai_dump = NULL;
    printf("sbk: audio capture: %u bytes at %u Hz\n", sbk_ai_dump_bytes, sbk_ai_freq);
}

s32 osAiSetNextBuffer(void *bufPtr, u32 size) {
    void *host = sbk_phys_to_host((u32)(uintptr_t)bufPtr); /* the game passes a physical address */
    size &= 0x3FFF8; /* AI_LEN_REG is 18 bits; the game's first request is uninitialised */
    sbk_ai_sim_queued += size;
    if (sbk_audio_muted) {
        static u8 silence[8192];
        if (size > sizeof(silence)) {
            size = sizeof(silence);
        }
        sbk_audio_out_queue(silence, size);
        return 0;
    }
    sbk_audio_out_queue(host, size);
    if (sbk_ai_dump != NULL) {
        /* WAV is little-endian: swap each 16-bit sample */
        const u8 *s = host;
        u8 buf[512];
        u32 i;
        for (i = 0; i + 1 < size; i += 2) {
            buf[i & 511] = s[i + 1];
            buf[(i & 511) + 1] = s[i];
            if ((i & 511) == 510 || i + 2 >= size) {
                fwrite(buf, 1, (i & 511) + 2, sbk_ai_dump);
            }
        }
        sbk_ai_dump_bytes += size & ~1u;
    }
    return 0;
}

u32 osAiGetLength(void) {
    return sbk_ai_sim_queued;
}

u32 osAiGetStatus(void) {
    return 0;
}
