/* Window + legacy OpenGL context through SDL 2.0.3 (the Tiger backport).
 * Frame pacing is the host loop's business; this layer only swaps. */
#include <stdio.h>
#include <sys/time.h>
#include <SDL2/SDL.h>
#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"
#include "../platform/input.h"
#include "../settings.h"

extern void sbk_input_request_quit(void);
extern int sbk_ui_launcher_active;
int sbk_ui_overlay_open(void);

static SDL_Window *wnd;
static SDL_GLContext ctx;
static int win_w = DESIRED_SCREEN_WIDTH, win_h = DESIRED_SCREEN_HEIGHT;
static int out_w, out_h;
int sbk_wide_output; /* SBK_WIDE_4_3 / SBK_WIDE_16_9: the aspect of the output rectangle */
int sbk_glinfo;      /* --glinfo: dump the GL strings and extension list at init */
int sbk_msaa_samples; /* what the driver actually gave us (0 = none) */

void gfx_gl13_set_output_rect(int x, int y, int w, int h, int win_w, int win_h);
void gfx_gl13_set_widescreen(int wide);

/* The frame is a box of the chosen aspect (4:3, or 16:9 under widescreen)
 * centred in the window; the bars around it are cleared black every frame.
 * Nothing is stretched either way -- 16:9 widens the race camera's field of
 * view (gfx_pc's aspect correction does that on its own once the render
 * target is wider) and leaves every 2D task in a centred 4:3 box. */
static void update_output_rect(void) {
    int w = win_w, h = win_h, x = 0, y = 0;
    int an = sbk_wide_output ? 16 : 4;      /* aspect numerator / denominator */
    int ad = sbk_wide_output ? 9 : 3;
    if (w * ad > h * an) {
        w = h * an / ad;
        x = (win_w - w) / 2;
    } else {
        h = w * ad / an;
        y = (win_h - h) / 2;
    }
    out_w = w; out_h = h;
    printf("sbk: window %dx%d, frame %dx%d at %d,%d (%d:%d)\n", win_w, win_h, w, h, x, y, an, ad);
    gfx_gl13_set_widescreen(sbk_wide_output);
    gfx_gl13_set_output_rect(x, y, w, h, win_w, win_h);
}

int sbk_novsync; /* --novsync */
int sbk_fullscreen_desktop; /* --fullscreen-desktop: borderless window at the desktop size instead of a mode switch */
int sbk_fullscreen_w, sbk_fullscreen_h; /* --fullscreen=WxH: exclusive mode of that size; 0 = the desktop's size */

/* Exclusive fullscreen (a real mode switch, vsync from the display) rather
 * than the composited desktop-sized window: on the Radeon 9000 the latter
 * costs ~8 ms per present at 1680x1050. */
static Uint32 fullscreen_flag(void) {
    return sbk_fullscreen_desktop ? SDL_WINDOW_FULLSCREEN_DESKTOP : SDL_WINDOW_FULLSCREEN;
}

static void apply_fullscreen_mode(void) {
    SDL_DisplayMode mode;
    if (sbk_fullscreen_w > 0 && sbk_fullscreen_h > 0 && SDL_GetWindowDisplayMode(wnd, &mode) == 0) {
        mode.w = sbk_fullscreen_w;
        mode.h = sbk_fullscreen_h;
        if (SDL_SetWindowDisplayMode(wnd, &mode) != 0) {
            fprintf(stderr, "sbk: fullscreen mode %dx%d: %s\n", mode.w, mode.h, SDL_GetError());
        }
    }
}

static void gfx_sdl_init(const char *window_title, bool start_in_fullscreen) {
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    if (start_in_fullscreen) {
        flags |= fullscreen_flag();
        if (sbk_fullscreen_w > 0) {
            win_w = sbk_fullscreen_w;
            win_h = sbk_fullscreen_h;
        } else {
            SDL_DisplayMode dm;
            if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
                win_w = dm.w;
                win_h = dm.h;
            }
        }
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    /* Anti-aliasing is a pixel-format attribute, so it is asked for here and
     * nowhere else.  If the driver has no multisample format of that size the
     * window creation fails outright (it does not silently downgrade), so the
     * second attempt is the plain format again and the run says so rather
     * than pretending. */
    if (sbk_settings.msaa > 0) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, sbk_settings.msaa);
    }
    wnd = SDL_CreateWindow(window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, flags);
    if (wnd == NULL && sbk_settings.msaa > 0) {
        fprintf(stderr, "sbk: no %dx multisample pixel format (%s); anti-aliasing off\n",
                sbk_settings.msaa, SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        sbk_settings.msaa = 0;
        wnd = SDL_CreateWindow(window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, flags);
    }
    if (wnd == NULL) {
        fprintf(stderr, "sbk: SDL_CreateWindow: %s\n", SDL_GetError());
        exit(1);
    }
    if (start_in_fullscreen) {
        apply_fullscreen_mode();
    }
    ctx = SDL_GL_CreateContext(wnd);
    if (ctx == NULL) {
        fprintf(stderr, "sbk: SDL_GL_CreateContext: %s\n", SDL_GetError());
        exit(1);
    }
    SDL_GL_MakeCurrent(wnd, ctx);
    {
        /* What the context really came back with.  SDL reports the attribute
         * it asked for; GL reports what the buffer has, which is the number
         * that counts. */
        int bufs = 0, smp = 0;
        SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &bufs);
        SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &smp);
        sbk_msaa_samples = 0;
        if (bufs > 0 && smp > 0) sbk_msaa_samples = smp;
        printf("sbk: multisample: asked %d, SDL buffers %d samples %d\n",
               sbk_settings.msaa, bufs, smp);
    }
    if (sbk_novsync || SDL_GL_SetSwapInterval(1) != 0) {
        SDL_GL_SetSwapInterval(0);
    }
    SDL_GetWindowSize(wnd, &win_w, &win_h);
    update_output_rect();
}

/* --- hooks for the launcher / overlay / resolution modes ---------------- */

int gfx_gl13_render_width(void);
int gfx_gl13_render_height(void);

/* gfx_pc asks how big the framebuffer it draws into is. In the n64 / 2x
 * resolution modes that is the small off-screen viewport, not the window. */
static void gfx_sdl_get_dimensions(uint32_t *width, uint32_t *height) {
    int rw = gfx_gl13_render_width();
    if (rw > 0) {
        *width = (uint32_t)rw;
        *height = (uint32_t)gfx_gl13_render_height();
        return;
    }
    *width = (uint32_t)out_w;
    *height = (uint32_t)out_h;
}

void sbk_gfx_window_size(int *w, int *h) { *w = win_w; *h = win_h; }

void sbk_gfx_refresh_output_rect(void) { update_output_rect(); }

void sbk_gfx_set_vsync(int on) {
    static int cur = -1;
    if (wnd == NULL || on == cur) return;
    cur = on;
    if (sbk_novsync) return;    /* --novsync wins for the whole run */
    SDL_GL_SetSwapInterval(on ? 1 : 0);
}

void sbk_gfx_swap(void) { if (wnd != NULL) SDL_GL_SwapWindow(wnd); }

int sbk_gfx_is_fullscreen(void) {
    return wnd != NULL && (SDL_GetWindowFlags(wnd) & SDL_WINDOW_FULLSCREEN) != 0;
}

static void set_fullscreen(int on) {
    if (wnd == NULL) return;
    if (on && !sbk_gfx_is_fullscreen()) {
        apply_fullscreen_mode();
        SDL_SetWindowFullscreen(wnd, fullscreen_flag());
    } else if (!on && sbk_gfx_is_fullscreen()) {
        SDL_SetWindowFullscreen(wnd, 0);
        SDL_SetWindowSize(wnd, DESIRED_SCREEN_WIDTH, DESIRED_SCREEN_HEIGHT);
    } else {
        return;
    }
    SDL_GetWindowSize(wnd, &win_w, &win_h);
    update_output_rect();
}

void sbk_gfx_set_fullscreen(int on) { set_fullscreen(on); }

static void gfx_sdl_handle_events(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                sbk_input_request_quit();
                break;
            case SDL_JOYDEVICEADDED:
            case SDL_JOYDEVICEREMOVED:
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYAXISMOTION:
            case SDL_JOYHATMOTION:
                sbk_input_joy_event(&ev);
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    SDL_GetWindowSize(wnd, &win_w, &win_h);
                    update_output_rect();
                }
                break;
            case SDL_KEYDOWN:
                /* Esc quits, the way Cmd+Q does -- but only from the game
                 * itself: in the launcher and in the options overlay Esc is
                 * already "back", and a key repeat must not quit either. */
                if (ev.key.keysym.sym == SDLK_ESCAPE && ev.key.repeat == 0 &&
                    !sbk_ui_launcher_active && !sbk_ui_overlay_open()) {
                    sbk_input_request_quit();
                    break;
                }
                /* fullscreen toggle: Cmd+Return, Cmd+F, Option+Return or F11 */
                if ((ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & (KMOD_ALT | KMOD_GUI))) ||
                    (ev.key.keysym.sym == SDLK_f && (ev.key.keysym.mod & KMOD_GUI)) ||
                    ev.key.keysym.sym == SDLK_F11) {
                    Uint32 f = SDL_GetWindowFlags(wnd) & SDL_WINDOW_FULLSCREEN;
                    if (!f) {
                        apply_fullscreen_mode();
                    }
                    SDL_SetWindowFullscreen(wnd, f ? 0 : fullscreen_flag());
                    if (f) {
                        SDL_SetWindowSize(wnd, DESIRED_SCREEN_WIDTH, DESIRED_SCREEN_HEIGHT);
                    }
                    SDL_GetWindowSize(wnd, &win_w, &win_h);
                    update_output_rect();
                    sbk_settings.fullscreen = !f;
                    sbk_settings_save();
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

static void gfx_sdl_set_title(const char *title) {
    if (wnd != NULL && !(SDL_GetWindowFlags(wnd) & SDL_WINDOW_FULLSCREEN)) { /* a title change stalls a fullscreen window ~100 ms */
        SDL_SetWindowTitle(wnd, title);
    }
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
    gfx_sdl_shutdown,
    gfx_sdl_set_title,
};
