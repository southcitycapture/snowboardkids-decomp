/* Immediate-mode GL 1.3 drawing for the launcher and the options overlay.
 *
 * Everything is drawn in window pixels with the origin at the top left, out of
 * one 128x128 GL_ALPHA font texture and untextured quads. The whole UI is a
 * few hundred quads per frame, well inside the 2 ms budget on the G4.
 *
 * sbk_ui_begin() saves the GL state the game left behind (glPushAttrib) and
 * sets up an orthographic pixel projection; sbk_ui_end() puts it all back, so
 * gfx_pc's own state cache stays valid. */
#ifndef SBK_UI_GL_H
#define SBK_UI_GL_H

struct SbkColor { float r, g, b, a; };

void sbk_ui_begin(int win_w, int win_h);
void sbk_ui_end(void);

/* Pixel size of one font pixel. 1 at 320x240, 4 at 1680x1050. */
int sbk_ui_pick_scale(int win_h);

void sbk_ui_rect(int x, int y, int w, int h, struct SbkColor c);
void sbk_ui_border(int x, int y, int w, int h, int t, struct SbkColor c);
void sbk_ui_text(int x, int y, int scale, const char *s, struct SbkColor c);
/* Same, with a one-pixel*scale drop shadow: readable over any frame. */
void sbk_ui_text_shadow(int x, int y, int scale, const char *s, struct SbkColor c);
void sbk_ui_logo(int x, int y, int w, int h); /* the launcher logo, aspect 16:9 */
int sbk_ui_text_w(const char *s, int scale);
int sbk_ui_text_h(int scale);

extern const struct SbkColor SBK_UI_FG, SBK_UI_DIM, SBK_UI_SEL, SBK_UI_ACCENT,
                             SBK_UI_PANEL, SBK_UI_SHADOW, SBK_UI_LINE;

#endif
