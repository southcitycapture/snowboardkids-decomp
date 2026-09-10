#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdint.h>
#include <stdbool.h>
#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct GfxDimensions {
    uint32_t width, height;
    float aspect_ratio;
};

extern struct GfxDimensions gfx_current_dimensions;

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen);
struct GfxRenderingAPI *gfx_get_current_rendering_api(void);

/* Interpret one display list (one RSP graphics task) into the back buffer. */
void gfx_run(Gfx *commands);
/* The game swapped a framebuffer in: show what has been drawn. */
void gfx_present(void);
/* Pump window events (called by the host loop once per retrace). */
void gfx_handle_events(void);

#endif
