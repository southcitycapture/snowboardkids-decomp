/* Enhanced-mode distance haze.  See haze.c for the whole story. */
#ifndef SBK_HAZE_H
#define SBK_HAZE_H

#ifdef __cplusplus
extern "C" {
#endif

/* The setting (settings.txt `haze`, --haze, the Options row). */
extern int sbk_haze_enabled;
/* --hazedbg: one line a second naming the course, the colour and the range. */
extern int sbk_haze_debug;

/* --- far-object fade-in (the same file, the same per-frame hook) ---------
 * Props, riders, item panels and effects are drawn only while they are inside
 * a square camera-distance cull box (isPositionNearCurrentRaceViewportCamera,
 * src/math/spatial_math.c), so at the edge of it they appear from nothing.
 * With `fadein` on they are faded in across the last stretch of that range
 * instead.  The range is not a constant here: patches.txt hands the port the
 * game's own number, already multiplied by --drawdistance. */
extern int sbk_fadein_enabled;
extern int sbk_fadein_debug;     /* --fadedbg */
extern int sbk_fadein_on;        /* 1 when this frame can fade anything */
extern float sbk_fadein_start;   /* eye distance where the fade-in begins */
extern float sbk_fadein_end;     /* the cull distance itself */
extern float sbk_fadein_inv_span;

/* Called from the patched cull range in patches.txt, like sbk_haze_note_far:
 * the range in the game's own 16.16 fixed point.  Returns its argument. */
int sbk_fadein_note_cull(int range);

/* 1 once this frame's race viewport is known, whether or not either effect is
 * switched on: it is what lets gfx_pc tell the race camera's projection from
 * the menu and overlay ones. */
extern int sbk_race_proj_on;

/* --fadedbg counters. */
extern unsigned sbk_fadein_dbg_draws, sbk_fadein_dbg_faded;
extern float sbk_fadein_dbg_min;

/* Recomputed once a frame by sbk_haze_frame(); read by gfx_pc.c.
 * sbk_haze_on is 0 for every frame that is not a race frame, and gfx_pc then
 * does not touch a single vertex. */
extern int sbk_haze_on;
extern float sbk_haze_start;      /* eye distance where the fade begins */
extern float sbk_haze_end;        /* eye distance where it is all haze */
extern float sbk_haze_inv_span;   /* 1 / (end - start) */
extern float sbk_haze_color[3];   /* 0..1 RGB */
/* The gSPPerspNormalize value the race camera's projection carries, so that
 * the sky viewport (a different far plane, and not one --drawdistance scales)
 * is left alone.  0 = the game never sent one; fall back to distance alone. */
extern int sbk_haze_persp_norm;

/* Called from the patched far-plane expressions in patches.txt: the race
 * viewport's far plane, already multiplied by sbk_far_scale.  Returns f so it
 * can wrap the expression in place. */
float sbk_haze_note_far(float f);

/* --hazedbg only: triangles seen under the race projection, triangles the
 * haze actually tinted, and the largest haze factor any vertex reached. */
extern unsigned sbk_haze_dbg_proj_tris, sbk_haze_dbg_tris;
extern float sbk_haze_dbg_max, sbk_haze_dbg_maxdist, sbk_haze_dbg_scale;
extern unsigned long sbk_haze_retrace;
extern unsigned short sbk_haze_dbg_pn[8];
extern unsigned sbk_haze_dbg_pn_tris[8];

/* Once a frame, before the display list is walked. */
void sbk_haze_frame(void);

#ifdef __cplusplus
}
#endif

#endif
