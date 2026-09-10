#ifndef GFX_WINDOW_MANAGER_API_H
#define GFX_WINDOW_MANAGER_API_H

#include <stdint.h>
#include <stdbool.h>

struct GfxWindowManagerAPI {
    void (*init)(const char *window_title, bool start_in_fullscreen);
    void (*get_dimensions)(uint32_t *width, uint32_t *height);
    void (*handle_events)(void);
    void (*swap_buffers)(void);
    double (*get_time)(void);
    void (*shutdown)(void);
    void (*set_title)(const char *title);
};

#endif
