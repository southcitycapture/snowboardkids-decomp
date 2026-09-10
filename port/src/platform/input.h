#ifndef SBK_INPUT_H
#define SBK_INPUT_H

#include <stdint.h>

void sbk_input_init(void);
/* Number of controllers the game should see plugged in (>= 1). */
int sbk_input_controller_count(void);
/* Snapshot of controller `port` in N64 terms: CONT_* button bits, stick -80..80. */
void sbk_input_read_pad(int port, uint16_t *buttons, int8_t *stick_x, int8_t *stick_y);
/* Called by the host loop after pumping SDL events. */
void sbk_input_update(void);
int sbk_input_quit_requested(void);

/* Scripted input (input_play.c): a text script or an .m64 movie, and recording. */
int sbk_input_play_load(const char *path);
int sbk_input_record_start(const char *path);
void sbk_input_play_step(uint16_t *buttons, int8_t *x, int8_t *y);
void sbk_input_play_shutdown(void);

#endif
