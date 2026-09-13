/* Enhanced-mode distance haze.
 *
 * --drawdistance N multiplies the race camera's far plane (patches.txt), and
 * the extra range is honest geometry: the N64 never drew it, so nobody ever
 * made it look like anything.  At N=4 the top of the screen grows a hard band
 * of far terrain standing in front of the sky, a mountain outline behind the
 * chairlift, clouds against hillside.  The fix is the one every draw-distance
 * mod ends up at: fade the new range into the air.
 *
 * The technique is the N64's own fog, not a post-process and not a shader.
 * F3DEX already has per-vertex fog -- gfx_pc.c computes a fog factor for
 * G_FOG geometry and gfx_gl13.c hands it to GL_FOG as a per-vertex fog
 * coordinate (GL_EXT_fog_coord, GL_LINEAR, start 0 end 1).  The Radeon 9000
 * does that in fixed function for nothing.  So the haze is not new machinery:
 * it is the same vertex slot and the same GL_FOG, filled in by the port for
 * race geometry the game did not fog far enough out.  Where the game has its
 * own fog factor for a vertex, the thicker of the two wins -- the haze can
 * only ever add, never take the game's fog away.
 *
 * The distance is exact rather than estimated.  configureViewport ->
 * guPerspective(..., scale = 0.5f), and the projection is then MUL'd with the
 * view rotation and translation into the same G_MTX_PROJECTION slot, so a
 * vertex's clip-space w is its eye distance times that scale.  gfx_pc takes
 * the length of the matrix's w column to recover the scale, which is why the
 * first game's 0.5 and the sequel's 1.0 need no special case.
 *
 * The colour is the game's own.  The first game has no per-viewport fog and no
 * LevelConfig colour table -- what it has is gFadeColorRed/Green/Blue, set once
 * per course by setBootFadeColor / setTitleFadeColor at the end of each case of
 * initRaceCourseSceneTasks (src/race/scene/race_scene_setup.c), and fed
 * straight to gDPSetFogColor by appendFadeOverlayDisplayList.  It is the
 * colour the course fades to, which is to say the colour of that course's air,
 * authored per course and per time of day: 80 C0 FF for Big Snowman's sky,
 * FF 80 00 for Sunset Rock, 00 00 32 and 00 00 40 for the two night courses,
 * F0 E6 BE for Quicksand Valley, 20 40 50 for Rookie Mountain's dusk.  So the
 * haze needs no table of its own and no guessing at the backdrop.
 *
 * The ramp is anchored to the far plane the *unmodified* game clipped at, not
 * to the extended one.  The geometry --drawdistance adds is not spread evenly
 * over the new range: it is a band sitting just past where the N64 clipped, so
 * a ramp that only reached full haze at the new far plane would be a few per
 * cent thick exactly where it is needed.  See the sequel's copy of this file
 * for the measurement that settled it.
 *
 * Off in Original mode, off in a scripted or golden run, and off whenever
 * --drawdistance is 1: at scale 1 there is nothing extra on screen to hide.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "haze.h"
#include "../ultra/ultra.h"
#include "game/race/player/race_player_input.h"
#include "game/race/race_state.h"

/* src/engine/system_runtime.c: the colour the current course fades to, which
 * is also the colour it hands gDPSetFogColor. */
extern u8 gFadeColorRed;
extern u8 gFadeColorGreen;
extern u8 gFadeColorBlue;

extern float sbk_far_scale;

int sbk_haze_enabled;
int sbk_haze_debug;

int sbk_haze_on;
int sbk_race_proj_on;
int sbk_fadein_enabled;
int sbk_fadein_debug;
int sbk_fadein_on;
float sbk_fadein_start;
float sbk_fadein_end;
float sbk_fadein_inv_span;
unsigned sbk_fadein_dbg_draws, sbk_fadein_dbg_faded;
float sbk_fadein_dbg_min = 1.0f;
float sbk_haze_start;
float sbk_haze_end;
float sbk_haze_inv_span;
float sbk_haze_color[3];
int sbk_haze_persp_norm;
unsigned long sbk_haze_retrace;

/* START: the fade begins slightly inside the old clip distance, so it starts
 * before the new geometry appears rather than exactly on it.  6% haze at the
 * N64's own horizon is a whisper and nothing the N64 drew moves by more.
 * END: one and a half times the old clip distance, capped at the extended far
 * plane.  1.5 and not 2 because the haze has to *beat* the fog the sequel
 * already applies to a race, which at --drawdistance 4 runs about 28% at the
 * old horizon and 58% at 6000 units: a gentler ramp than that changes not one
 * pixel, because the two fogs are the same colour and the thicker one wins.
 * At 1.5 the haze passes the game's own at around 4000 units and is flat
 * course-coloured air by 5700. */
#define HAZE_START_FRAC 0.85f
#define HAZE_END_MULT   1.50f

static float haze_far;      /* the race far plane, already scaled */
static float cull_range;    /* the camera-distance cull, in world units */

float sbk_haze_note_far(float f) {
    haze_far = f;
    return f;
}

/* The game compares 16.16 fixed-point world coordinates against this, so the
 * distance it stands for is range / 65536 in the units the far plane and the
 * haze are written in.  patches.txt already multiplied it by sbk_far_scale,
 * exactly as it does the far plane, so what arrives here is the range this
 * run actually culls at. */
int sbk_fadein_note_cull(int range) {
    cull_range = (float)range / 65536.0f;
    return range;
}

/* The last 14% of the cull range.  Shorter than that and an object still
 * arrives visibly; longer and props are half-transparent while they are
 * plainly in view.  At --drawdistance 1 that band is 2560..2976 units, which
 * is where the first game's own fog is already most of the way to total, so
 * Original loses nothing by not having it. */
#define FADEIN_FRAC 0.86f

/* --- which matrices belong to a cullable object -------------------------
 *
 * The first version of the fade keyed on the object's origin distance alone
 * (MP[3][3]), guarded by "all three vertices are past the start of the band".
 * That is not enough, and the sequel said so loudly: at --drawdistance 4 it
 * faded 20,000 of 40,000 triangles a second down to alpha 0, because the
 * course's own terrain is drawn in chunks with their own far-away origins and
 * every chunk past 3,504 units looked exactly like a distant prop.
 *
 * So the port stops guessing and lets the game say which matrices belong to
 * the objects the cull applies to.  Each game has one function that builds an
 * object's transform matrix -- allocFixedTransformMatrix in the first game,
 * setupDisplayListMatrix in the sequel -- and patches.txt has it hand the
 * pointer over.  gfx_pc then fades a draw only when the modelview it loaded
 * is one of those.  Terrain, the sky and the HUD never appear in the table.
 *
 * The table is cleared after every frame's display list is walked, because
 * the matrices come out of a per-frame scratch allocator: a pointer from the
 * last frame means nothing.  A collision loses one object's fade, never
 * anything else, so eviction is a shrug rather than a problem. */
#define FADEIN_TABLE 1024
#define FADEIN_PROBE 4
static const void *fadein_mtx[FADEIN_TABLE];
static int fadein_mtx_used;

void sbk_fadein_note_object(const void *mtx) {
    unsigned long h;
    int i;
    if (!sbk_fadein_on || mtx == NULL) return;
    h = ((unsigned long)mtx >> 6) ^ ((unsigned long)mtx >> 3);
    for (i = 0; i < FADEIN_PROBE; i++) {
        unsigned k = (unsigned)((h + (unsigned long)i) & (FADEIN_TABLE - 1));
        if (fadein_mtx[k] == mtx) return;
        if (fadein_mtx[k] == NULL) { fadein_mtx[k] = mtx; fadein_mtx_used = 1; return; }
    }
    fadein_mtx[(unsigned)(h & (FADEIN_TABLE - 1))] = mtx;   /* evict: one lost fade */
    fadein_mtx_used = 1;
}

int sbk_fadein_is_object(const void *mtx) {
    unsigned long h;
    int i;
    if (!fadein_mtx_used || mtx == NULL) return 0;
    h = ((unsigned long)mtx >> 6) ^ ((unsigned long)mtx >> 3);
    for (i = 0; i < FADEIN_PROBE; i++) {
        unsigned k = (unsigned)((h + (unsigned long)i) & (FADEIN_TABLE - 1));
        if (fadein_mtx[k] == mtx) return 1;
        if (fadein_mtx[k] == NULL) return 0;
    }
    return 0;
}

void sbk_fadein_frame_end(void) {
    if (!fadein_mtx_used) return;
    memset(fadein_mtx, 0, sizeof(fadein_mtx));
    fadein_mtx_used = 0;
}

void sbk_haze_frame(void) {
    static int last_course = -1;
    static unsigned long ticks;

    sbk_haze_retrace++;
    sbk_haze_on = 0;
    sbk_fadein_on = 0;
    sbk_race_proj_on = 0;
    if (haze_far <= 1.0f) return;          /* no race viewport has been built */

    /* The first game has no render-context flag to key on the way the sequel
     * does; gRacePlayers[0].isActive is what every other port-side race hook
     * uses.  It is set in the menus too, which is why it is not the only gate:
     * the perspNorm test below is what actually keeps the haze inside the race
     * camera's own viewport. */
    if (!gRacePlayers[0].isActive) return;

    sbk_haze_color[0] = gFadeColorRed / 255.0f;
    sbk_haze_color[1] = gFadeColorGreen / 255.0f;
    sbk_haze_color[2] = gFadeColorBlue / 255.0f;

    /* guPerspective's perspNorm names the far plane on its own; see the note
     * below.  It is computed whether or not either effect is switched on,
     * because widescreen needs the same answer -- "is this draw the race
     * camera's?" -- to leave the menu passes in their 4:3 box. */
    sbk_haze_persp_norm = (int)(131072.0f / haze_far);
    sbk_race_proj_on = 1;

    if (sbk_fadein_enabled && cull_range > 1.0f) {
        sbk_fadein_end = cull_range;
        sbk_fadein_start = cull_range * FADEIN_FRAC;
        sbk_fadein_inv_span = 1.0f / (sbk_fadein_end - sbk_fadein_start);
        sbk_fadein_on = 1;
    }

    if (sbk_fadein_debug && (sbk_haze_retrace % 60 == 0)) {
        printf("sbk-fade: r%lu cull %.0f fade %.0f..%.0f draws %u faded %u minalpha %.2f\n",
               sbk_haze_retrace, cull_range, sbk_fadein_start, sbk_fadein_end,
               sbk_fadein_dbg_draws, sbk_fadein_dbg_faded, sbk_fadein_dbg_min);
        sbk_fadein_dbg_draws = sbk_fadein_dbg_faded = 0;
        sbk_fadein_dbg_min = 1.0f;
    }

    if (!sbk_haze_enabled) return;
    if (sbk_far_scale <= 1.001f) return;   /* nothing extra was drawn to hide */

    {
        float orig_far = haze_far / sbk_far_scale;   /* what the N64 clipped at */
        sbk_haze_start = orig_far * HAZE_START_FRAC;
        sbk_haze_end = orig_far * HAZE_END_MULT;
        if (sbk_haze_end > haze_far) sbk_haze_end = haze_far;
    }
    if (sbk_haze_end - sbk_haze_start < 1.0f) return;
    sbk_haze_inv_span = 1.0f / (sbk_haze_end - sbk_haze_start);

    /* guPerspective's perspNorm is 2*65536/(near+far), and near (10) is a
     * fraction of a percent of far here, so the integer the race camera
     * carries is exactly (int)(131072/far).  Every other viewport in the
     * frame has a far plane --drawdistance does not scale -- the overlay
     * passes are a flat 15000, the menu viewport 10000, and Silver Mountain's
     * own race viewport 1000 -- so each carries a different integer and
     * gfx_pc can tell them apart without a heuristic. */
    sbk_haze_on = 1;

    if (sbk_haze_debug && (ticks++ % 60 == 0 || gRaceCourseIndex.signedValue != last_course)) {
        int i;
        last_course = gRaceCourseIndex.signedValue;
        printf("sbk-haze: r%lu course %d rgb %02x%02x%02x range %.0f..%.0f perspnorm %d "
               "projtris %u hazed %u maxf %.2f maxdist %.0f wscale %.4f\n",
               sbk_haze_retrace, last_course,
               gFadeColorRed, gFadeColorGreen, gFadeColorBlue,
               sbk_haze_start, sbk_haze_end, sbk_haze_persp_norm,
               sbk_haze_dbg_proj_tris, sbk_haze_dbg_tris, sbk_haze_dbg_max,
               sbk_haze_dbg_maxdist, sbk_haze_dbg_scale);
        printf("sbk-haze: perspnorms");
        for (i = 0; i < 8 && sbk_haze_dbg_pn_tris[i] != 0; i++)
            printf(" %u=%u", (unsigned)sbk_haze_dbg_pn[i], sbk_haze_dbg_pn_tris[i]);
        printf("\n");
        for (i = 0; i < 8; i++) { sbk_haze_dbg_pn[i] = 0; sbk_haze_dbg_pn_tris[i] = 0; }
        sbk_haze_dbg_proj_tris = sbk_haze_dbg_tris = 0;
        sbk_haze_dbg_max = 0.0f;
        sbk_haze_dbg_maxdist = 0.0f;
    }

}
