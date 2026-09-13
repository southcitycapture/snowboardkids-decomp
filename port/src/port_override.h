/* Force-included into every game translation unit (see port/Makefile).
 * Keep this tiny: it exists for the few IDO/N64-isms that cannot be handled
 * in the platform layer. */
#ifndef PORT_OVERRIDE_H
#define PORT_OVERRIDE_H

/* IDO accepts `long long` constants and `-Xcpluscomm` comments in gnu89; GCC
 * with -std=gnu89 does too, so nothing to do there. */

extern float sbk_far_scale; /* --drawdistance: multiplies the far plane and the cull range (patches.txt) */
/* The Enhanced-mode distance haze needs to know where the race camera's far
 * plane ended up, in the game's own world units; the patched far-plane
 * expressions hand it over on the way past. Returns its argument. */
extern float sbk_haze_note_far(float f);

#endif
