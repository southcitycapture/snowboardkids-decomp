/* The launcher's backdrop: a Sunny Mountain sky with drifting clouds and
 * light snowfall, and the two games as N64 boxes standing on a snow shelf.
 *
 * Everything here is generated in code -- the sky is a gradient, the clouds
 * and the snowflakes are one soft blob texture, the boxes are six quads at the
 * proportions of a real N64 box (1 : 1.4 : 0.15).  The only thing that comes
 * from outside is the picture on the front of each box: the game's own title
 * logo pulled out of its ROM (ui_rom_art.c), or a PNG the user dropped into
 * port/resources (tools/gen_box.py).
 *
 * Called between sbk_ui_begin() and sbk_ui_end(), which have already saved the
 * GL state the game left behind; the perspective pass puts the ortho
 * projection back itself so the 2D drawing after it needs no special case. */
#ifndef SBK_UI_SCENE_H
#define SBK_UI_SCENE_H

/* Advance the clouds and the snow. dt is in seconds. */
void sbk_ui_scene_step(float dt);

/* The sky gradient, in the ortho projection: drawn first, under everything. */
void sbk_ui_scene_sky(int win_w, int win_h);

/* Its clouds and snowfall, drawn after the boxes. */
void sbk_ui_scene_weather(int win_w, int win_h);

/* Paint the sky gradient back over the boxes at `a` (0..1): how the screens
 * with a panel on them cross-fade the boxes away. */
void sbk_ui_scene_sky_veil(int win_w, int win_h, float a);

/* Where a box is drawn on screen, so the 2D pass can put a label under it. */
struct SbkBoxPlace { int cx, cy, w, h; };

/* The shelf and the boxes.  `count` boxes, `focus` is the selected one,
 * `present[i]` is 0 for a game whose ROM is missing (grey and unlit).
 * `blend` is 0..1: 1 = the boxes are the whole picture (the Pick screen),
 * smaller pushes them back and up for the screens that put a panel over them.
 * Fills `place[i]` with the screen rectangle each box came out at. */
void sbk_ui_scene_boxes(int win_w, int win_h, int count, int focus,
                        const int *present, float blend,
                        struct SbkBoxPlace *place);

/* The RGBA front picture for game `i`, built once and uploaded.  Called by
 * the scene; exposed so the launcher can force it after a ROM appears. */
void sbk_ui_scene_invalidate(void);

#endif
