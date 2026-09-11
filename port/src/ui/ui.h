/* The launcher and the in-game options overlay. */
#ifndef SBK_UI_H
#define SBK_UI_H

/* Runs the launcher on the already-created window and returns 1 to start the
 * game, 0 if the user quit. A no-op returning 1 when the launcher is off. */
int sbk_launcher_run(void);

/* Called from the GL backend at the end of every presented frame: draws the
 * options overlay when it is open. Cheap (a few hundred quads) and does
 * nothing at all when closed. */
void sbk_ui_overlay_draw(int win_w, int win_h, int out_x, int out_y, int out_w, int out_h);

/* Called once per retrace from the host loop: opens/closes the overlay on F1
 * or the pad's View button and steps its navigation. Returns 1 while the
 * overlay owns the input (the game's pad then reads as all-zero). */
int sbk_ui_overlay_tick(void);

int sbk_ui_overlay_open(void);

/* --uiscript SEQ: drive the launcher and the overlay from a token string
 * (u d l r a b m w .) so they can be tested and screenshotted over SSH. */
void sbk_ui_script_set(const char *seq);

#endif
