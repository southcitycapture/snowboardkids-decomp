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
void sbk_input_play_set_cmdfile(const char *path);
void sbk_input_play_poll(void);
void sbk_input_play_add(const char *line);
union SDL_Event;
void sbk_input_joy_event(const union SDL_Event *ev);
int sbk_input_rumble_supported(void);

/* Raw navigation state for the launcher and the options overlay, read
 * straight from the keyboard and the pad so that it works before the game
 * boots and while the game's own input is suppressed. Levels, not edges: the
 * UI does its own repeat. */
struct SbkUiRaw {
    int up, down, left, right;
    int accept;   /* Enter / Space / pad A */
    int cancel;   /* Esc / pad B */
    int menu;     /* F1 / pad View(Back) -- opens and closes the overlay */
};
void sbk_ui_input_raw(struct SbkUiRaw *r);
/* While set, sbk_input_update() hands the game an idle controller. */
extern int sbk_ui_owns_input;
void sbk_input_rumble(int on);

#endif
