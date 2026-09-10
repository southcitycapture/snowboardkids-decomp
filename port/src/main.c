/* Host entry point and the "hardware" loop.
 *
 * The game's own main() (src/engine/system_runtime.c, renamed to
 * sbk_game_main by the build) sets up libultra threads exactly as on the N64;
 * from then on this loop plays the part of the interrupt controller: it runs
 * the cooperative threads, delivers a vertical retrace 60 times a second,
 * presents frames the game swapped in, and feeds controller input. */
#include "ultra/ultra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <SDL2/SDL.h>
#include "ultra/sbk_os.h"
#include "ultra/sbk_pins.h"
#include "platform/input.h"
#include "platform/audio_out.h"
#include "gfx/gfx_pc.h"
#include "gfx/gfx_window_manager_api.h"
#include "gfx/gfx_rendering_api.h"

extern void sbk_game_main(void *arg);
extern int sbk_rom_load(const char *path);
extern int sbk_trace;
extern struct GfxWindowManagerAPI gfx_sdl_gl13_wapi;
extern struct GfxRenderingAPI gfx_gl13_rapi;

#define RETRACE_USEC (1000000.0 / 60.0)

static double now_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

static const char *find_rom(int argc, char **argv) {
    static char path[1024];
    int i;
    const char *candidates[] = { "snowboardkids.z64", "../Resources/snowboardkids.z64", NULL };
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            return argv[i];
        }
    }
    for (i = 0; candidates[i] != NULL; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            return candidates[i];
        }
    }
    /* next to the executable (inside the .app bundle: Contents/MacOS/../Resources) */
    if (argc > 0) {
        const char *slash = strrchr(argv[0], '/');
        if (slash != NULL) {
            size_t n = (size_t)(slash - argv[0]);
            if (n < sizeof(path) - 64) {
                memcpy(path, argv[0], n);
                strcpy(path + n, "/../Resources/snowboardkids.z64");
                return path;
            }
        }
    }
    return "snowboardkids.z64";
}

int main(int argc, char **argv) {
    const char *rom;
    setvbuf(stdout, NULL, _IONBF, 0); /* logs survive a crash */
    setvbuf(stderr, NULL, _IONBF, 0);
    rom = find_rom(argc, argv);
    int fullscreen = 0;
    double next_retrace;
    unsigned presented = 0;
    unsigned long retraces = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fullscreen") == 0 || strcmp(argv[i], "-f") == 0) {
            fullscreen = 1;
        } else if (strcmp(argv[i], "--trace") == 0) {
            sbk_trace = 1;
        }
    }

    if (sbk_rom_load(rom) != 0) {
        fprintf(stderr, "usage: %s [--fullscreen] [snowboardkids.z64]\n", argv[0]);
        return 1;
    }
    printf("sbk: ROM %s (%lu bytes)\n", rom, (unsigned long)sbk_rom_size);

    sbk_rdram_init();
    sbk_pin_init();
    sbk_os_init();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "sbk: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    gfx_init(&gfx_sdl_gl13_wapi, &gfx_gl13_rapi, "Snowboard Kids", fullscreen != 0);
    sbk_input_init();
    sbk_audio_out_init();

    /* Boot: the game creates its boot thread and starts it. */
    printf("sbk: booting game (image at %p, RDRAM at 0x%08x)\n", (void *)main, SBK_RDRAM_BASE);
    sbk_game_main(NULL);

    next_retrace = now_usec();
    while (!sbk_input_quit_requested()) {
        double t;

        sbk_sched_run();

        if (sbk_vi_swap_serial != presented) {
            presented = sbk_vi_swap_serial;
            gfx_present();
        }

        t = now_usec();
        if (t >= next_retrace) {
            gfx_handle_events();
            sbk_input_update();
            sbk_vi_retrace();
            retraces++;
            if (retraces % 120 == 0) {
                extern unsigned sbk_stat_dma, sbk_stat_cont, sbk_task_count, sbk_stat_present;
                printf("sbk: t=%lus retraces=%lu gfxtasks=%u presents=%u dma=%u contreads=%u swaps=%u pollfails=%u\n",
                       retraces / 60, retraces, sbk_task_count, sbk_stat_present, sbk_stat_dma, sbk_stat_cont,
                       sbk_vi_swap_serial, sbk_poll_fail_count);
            }
            next_retrace += RETRACE_USEC;
            if (t - next_retrace > 250000.0) {
                next_retrace = t; /* fell far behind (debugger, window drag): resync */
            }
            continue;
        }

        if (sbk_sched_has_runnable() && sbk_poll_fail_count < 64) {
            continue; /* real work pending */
        }

        /* Nothing to do until the next retrace. */
        {
            double wait = next_retrace - t;
            if (wait > 2000.0) {
                SDL_Delay((Uint32)((wait - 1000.0) / 1000.0));
            }
        }
    }

    printf("sbk: exiting after %lu retraces\n", retraces);
    sbk_audio_out_shutdown();
    SDL_Quit();
    return 0;
}
