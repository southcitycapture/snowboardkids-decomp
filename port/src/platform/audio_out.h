#ifndef SBK_AUDIO_OUT_H
#define SBK_AUDIO_OUT_H

#include <stdint.h>

int sbk_audio_out_init(void);
void sbk_audio_out_set_rate(uint32_t hz);
/* Queue `bytes` of 16-bit stereo big-endian samples for playback. */
void sbk_audio_out_queue(const void *samples, uint32_t bytes);
/* Bytes queued but not yet played (what osAiGetLength reports). */
uint32_t sbk_audio_out_queued_bytes(void);
void sbk_audio_out_shutdown(void);

void sbk_audio_out_set_volume(int percent);
int sbk_audio_out_get_volume(void);

#endif
