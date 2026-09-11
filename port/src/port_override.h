/* Force-included into every game translation unit (see port/Makefile).
 * Keep this tiny: it exists for the few IDO/N64-isms that cannot be handled
 * in the platform layer. */
#ifndef PORT_OVERRIDE_H
#define PORT_OVERRIDE_H

/* IDO accepts `long long` constants and `-Xcpluscomm` comments in gnu89; GCC
 * with -std=gnu89 does too, so nothing to do there. */

extern float sbk_far_scale; /* --drawdistance: multiplies the far plane and the cull range (patches.txt) */

#endif
