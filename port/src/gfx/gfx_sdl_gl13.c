/* Window + legacy OpenGL context through SDL 2.0.3 (the Tiger backport).
 * Frame pacing is the host loop's business; this layer only swaps. */
#include <stdio.h>
#include <sys/time.h>
#include <SDL2/SDL.h>
#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"
#include "../platform/input.h"

extern void sbk_input_request_quit(void);

static SDL_Window *wnd;
static SDL_GLContext ctx;
static int win_w = DESIRED_SCREEN_WIDTH, win_h = DESIRED_SCREEN_HEIGHT;

static void gfx_sdl_init(const char *window_title, bool start_in_fullscreen) {
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    if (start_in_fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    wnd = SDL_CreateWindow(window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, flags);
    if (wnd == NULL) {
        fprintf(stderr, "sbk: SDL_CreateWindow: %s\n", SDL_GetError());
        exit(1);
    }
    ctx = SDL_GL_CreateContext(wnd);
    if (ctx == NULL) {
        fprintf(stderr, "sbk: SDL_GL_CreateContext: %s\n", SDL_GetError());
        exit(1);
    }
    SDL_GL_MakeCurrent(wnd, ctx);
    if (SDL_GL_SetSwapInterval(1) != 0) {
        SDL_GL_SetSwapInterval(0);
    }
    SDL_GetWindowSize(wnd, &win_w, &win_h);
}

static void gfx_sdl_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = (uint32_t)win_w;
    *height = (uint32_t)win_h;
}

static void gfx_sdl_handle_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                sbk_input_request_quit();
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    SDL_GetWindowSize(wnd, &win_w, &win_h);
                }
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)) {
                    Uint32 f = SDL_GetWindowFlags(wnd) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(wnd, f ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    SDL_GetWindowSize(wnd, &win_w, &win_h);
                }
                break;
            default:
                break;
        }
    }
}

static void gfx_sdl_swap_buffers(void) {
    SDL_GL_SwapWindow(wnd);
}

static double gfx_sdl_get_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static void gfx_sdl_shutdown(void) {
    if (ctx != NULL) {
        SDL_GL_DeleteContext(ctx);
    }
    if (wnd != NULL) {
        SDL_DestroyWindow(wnd);
    }
}

struct GfxWindowManagerAPI gfx_sdl_gl13_wapi = {
    gfx_sdl_init,
    gfx_sdl_get_dimensions,
    gfx_sdl_handle_events,
    gfx_sdl_swap_buffers,
    gfx_sdl_get_time,
    gfx_sdl_shutdown
};
