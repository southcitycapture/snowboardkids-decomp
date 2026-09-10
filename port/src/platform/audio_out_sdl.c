/* Audio output: a ring buffer of big-endian 16-bit stereo samples that the
 * SDL audio callback drains. SDL 2.0.3 (the Tiger backport) has no
 * SDL_QueueAudio, hence the hand-rolled ring. */
#include <string.h>
#include <SDL2/SDL.h>
#include "audio_out.h"

#define RING_BYTES (16 * 1024) /* ~93 ms at 22050 Hz stereo 16-bit */

static Uint8 sbk_ring[RING_BYTES];
static volatile Uint32 sbk_ring_read, sbk_ring_write; /* byte offsets, monotonically increasing */
static SDL_AudioDeviceID sbk_dev;
static Uint32 sbk_rate = 22050;

static void sbk_audio_cb(void *userdata, Uint8 *stream, int len) {
    Uint32 avail = sbk_ring_write - sbk_ring_read;
    int n = (int)(avail < (Uint32)len ? avail : (Uint32)len);
    int i;
    (void)userdata;
    for (i = 0; i < n; i++) {
        stream[i] = sbk_ring[(sbk_ring_read + (Uint32)i) % RING_BYTES];
    }
    sbk_ring_read += (Uint32)n;
    if (n < len) {
        memset(stream + n, 0, (size_t)(len - n));
    }
}

static int sbk_audio_open(void) {
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = (int)sbk_rate;
    want.format = AUDIO_S16MSB; /* the N64 (and the G4) are big-endian */
    want.channels = 2;
    want.samples = 512;
    want.callback = sbk_audio_cb;
    if (sbk_dev != 0) {
        SDL_CloseAudioDevice(sbk_dev);
        sbk_dev = 0;
    }
    sbk_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (sbk_dev == 0) {
        SDL_Log("audio: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(sbk_dev, 0);
    return 0;
}

int sbk_audio_out_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_Log("audio: SDL_INIT_AUDIO failed: %s", SDL_GetError());
        return -1;
    }
    return sbk_audio_open();
}

void sbk_audio_out_set_rate(uint32_t hz) {
    if (hz != sbk_rate) {
        sbk_rate = hz;
        if (sbk_dev != 0) {
            sbk_audio_open();
        }
    }
}

void sbk_audio_out_queue(const void *samples, uint32_t bytes) {
    const Uint8 *src = (const Uint8 *)samples;
    Uint32 i;
    if (sbk_dev == 0) {
        return;
    }
    SDL_LockAudioDevice(sbk_dev);
    if (sbk_ring_write - sbk_ring_read + bytes > RING_BYTES) {
        bytes = RING_BYTES - (sbk_ring_write - sbk_ring_read); /* overrun: drop the tail */
    }
    for (i = 0; i < bytes; i++) {
        sbk_ring[(sbk_ring_write + i) % RING_BYTES] = src[i];
    }
    sbk_ring_write += bytes;
    SDL_UnlockAudioDevice(sbk_dev);
}

uint32_t sbk_audio_out_queued_bytes(void) {
    Uint32 n;
    if (sbk_dev == 0) {
        return 0;
    }
    SDL_LockAudioDevice(sbk_dev);
    n = sbk_ring_write - sbk_ring_read;
    SDL_UnlockAudioDevice(sbk_dev);
    return n;
}

void sbk_audio_out_shutdown(void) {
    if (sbk_dev != 0) {
        SDL_CloseAudioDevice(sbk_dev);
        sbk_dev = 0;
    }
}
