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
#include "settings.h"
#include "ui/ui.h"

extern void sbk_game_main(void *arg);
extern int sbk_rom_load(const char *path);
extern int sbk_trace;
extern int sbk_dump_task;
extern int sbk_dump_frames;
extern int sbk_dump_tris;
extern int sbk_audio_disabled;
extern int sbk_race_debug_enabled;
int sbk_pak_open(const char *path);
extern float sbk_far_scale;
#include "debug/perf.h"
int sbk_peek_add(const char *spec);
extern int sbk_autoplay, sbk_soak, sbk_nightmare, sbk_dumpon;
int sbk_trial_parse(const char *spec);
int sbk_turbo, sbk_headless;
static int quit_now;
void sbk_request_quit_now(void) { quit_now = 1; }
void sbk_autoplay_tick(unsigned long retraces);
void sbk_race_debug(unsigned long retraces);
extern struct GfxWindowManagerAPI gfx_sdl_gl13_wapi;
extern struct GfxRenderingAPI gfx_gl13_rapi;

#define RETRACE_USEC (1000000.0 / 60.0)
/* The game thread polls three queues per loop pass; this many consecutive
 * empty polls with nothing else runnable means the game is idle for the frame. */
#define SBK_IDLE_POLLS 3

extern void sbk_ai_retrace(void);
extern int sbk_hash_frames;
extern unsigned sbk_last_frame_hash;
extern unsigned sbk_stat_dma;
extern int sbk_audio_muted;
int sbk_ai_dump_start(const char *path);
void sbk_ai_dump_finish(void);
extern unsigned sbk_audio_peak;

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
        if (strcmp(argv[i], "--play") == 0 || strcmp(argv[i], "--record") == 0 || strcmp(argv[i], "--drawdistance") == 0 || strcmp(argv[i], "--dumpdl") == 0 || strcmp(argv[i], "--frames") == 0 || strcmp(argv[i], "--wav") == 0 || strcmp(argv[i], "--dumpframes") == 0 || strcmp(argv[i], "--pak") == 0 || strcmp(argv[i], "--bigtri") == 0 || strcmp(argv[i], "--peek") == 0 || strcmp(argv[i], "--cmds") == 0 || strcmp(argv[i], "--trial") == 0 || strcmp(argv[i], "--plan") == 0 || strcmp(argv[i], "--saveevery") == 0 || strcmp(argv[i], "--uiscript") == 0) {
            i++; /* option value */
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

/* --play / --headless / --nolauncher: a scripted run. It must not read the
 * settings file, must not show the launcher and must not apply any filter, so
 * that golden replays stay bit-identical whatever the user last chose. */
static int scripted_run(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--play") == 0 || strcmp(argv[i], "--headless") == 0 ||
            strcmp(argv[i], "--nolauncher") == 0 || strcmp(argv[i], "--record") == 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *rom;
    setvbuf(stdout, NULL, _IONBF, 0); /* logs survive a crash */
    setvbuf(stderr, NULL, _IONBF, 0);
    rom = find_rom(argc, argv);
    sbk_settings_scripted = scripted_run(argc, argv);
    sbk_settings_load();
    int fullscreen = 0; /* 1 = yes, -1 = --windowed, 0 = default (fullscreen when SBK_FULLSCREEN=1 or launched from the Finder) */
    extern int sbk_wide_output;
    const char *pak_path = NULL;
    int nopak = 0;
    const char *play = NULL, *record = NULL;
    unsigned long max_frames = 0;
    unsigned last_dma = 0;
    double next_retrace;
    unsigned presented = 0;
    unsigned long retraces = 0;
    int i;

    if (!sbk_settings_scripted) {
        /* the file's values are this run's starting point; every flag below
         * overrides its own key for this run only */
        sbk_far_scale = (float)sbk_settings.draw_distance;
        sbk_wide_output = sbk_settings.widescreen;
        if (sbk_settings.fullscreen) fullscreen = 1;
        sbk_perf_enabled = sbk_settings.perf;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fullscreen") == 0 || strcmp(argv[i], "-f") == 0) {
            fullscreen = 1;
        } else if (strncmp(argv[i], "--fullscreen=", 13) == 0) {
            extern int sbk_fullscreen_w, sbk_fullscreen_h;
            fullscreen = 1;
            sscanf(argv[i] + 13, "%dx%d", &sbk_fullscreen_w, &sbk_fullscreen_h);
        } else if (strcmp(argv[i], "--fullscreen-desktop") == 0) {
            extern int sbk_fullscreen_desktop;
            fullscreen = 1;
            sbk_fullscreen_desktop = 1;
        } else if (strcmp(argv[i], "--novsync") == 0) {
            extern int sbk_novsync;
            sbk_novsync = 1;
        } else if (strcmp(argv[i], "--windowed") == 0) {
            fullscreen = -1;
            sbk_settings.fullscreen = 0;
        } else if (strcmp(argv[i], "--wide") == 0) {
            sbk_wide_output = 1;
            sbk_settings.widescreen = 1;
        } else if (strcmp(argv[i], "--nolauncher") == 0) {
            sbk_settings.launcher = 0;
        } else if (strcmp(argv[i], "--uiscript") == 0 && i + 1 < argc) {
            sbk_ui_script_set(argv[++i]);
        } else if (strcmp(argv[i], "--launcher") == 0) {
            sbk_settings.launcher = 1;
        } else if (strncmp(argv[i], "--volume=", 9) == 0) {
            sbk_settings.volume = atoi(argv[i] + 9);
        } else if (strncmp(argv[i], "--resolution=", 13) == 0) {
            const char *v = argv[i] + 13;
            sbk_settings.resolution = strcmp(v, "n64") == 0 ? SBK_RES_N64 : (strcmp(v, "2x") == 0 ? SBK_RES_2X : SBK_RES_NATIVE);
            sbk_settings_forced = 1;
        } else if (strncmp(argv[i], "--filter=", 9) == 0) {
            const char *v = argv[i] + 9;
            sbk_settings.filter = strcmp(v, "scanlines") == 0 ? SBK_FILTER_SCANLINES :
                                  strcmp(v, "grille") == 0 ? SBK_FILTER_GRILLE :
                                  strcmp(v, "smooth") == 0 ? SBK_FILTER_SMOOTH : SBK_FILTER_NONE;
            sbk_settings_forced = 1;
        } else if (strncmp(argv[i], "--mode=", 7) == 0) {
            sbk_settings_apply_mode(strcmp(argv[i] + 7, "enhanced") == 0 ? SBK_MODE_ENHANCED : SBK_MODE_ORIGINAL);
            sbk_settings_forced = 1;
            sbk_far_scale = (float)sbk_settings.draw_distance;
            sbk_wide_output = sbk_settings.widescreen;
        } else if (strncmp(argv[i], "-psn_", 5) == 0) {
            if (fullscreen == 0) fullscreen = 1; /* launched from the Finder: go fullscreen */
        } else if (strcmp(argv[i], "--trace") == 0) {
            sbk_trace = 1;
        } else if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) {
            play = argv[++i];
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record = argv[++i];
        } else if (strcmp(argv[i], "--dumpdl") == 0 && i + 1 < argc) {
            sbk_dump_task = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtoul(argv[++i], NULL, 10); /* quit after N retraces */
        } else if (strcmp(argv[i], "--dumpframes") == 0 && i + 1 < argc) {
            sbk_dump_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hashframe") == 0) {
            sbk_hash_frames = 1; /* fingerprint every presented frame */
        } else if (strcmp(argv[i], "--mute") == 0) {
            sbk_audio_muted = 1;
        } else if (strcmp(argv[i], "--noaudio") == 0) {
            sbk_audio_muted = 1;
            sbk_audio_disabled = 1; /* skip the command-list interpreter entirely */
        } else if (strcmp(argv[i], "--dumptris") == 0) {
            sbk_dump_tris = 1;
        } else if (strcmp(argv[i], "--bigtri") == 0 && i + 1 < argc) {
            extern int sbk_bigtri_area, sbk_bigtri_left;
            sbk_bigtri_area = atoi(argv[++i]);
            sbk_bigtri_left = 400000;
        } else if (strcmp(argv[i], "--pak") == 0 && i + 1 < argc) {
            pak_path = argv[++i];
        } else if (strcmp(argv[i], "--autonav") == 0) {
            extern int sbk_autonav;
            sbk_autonav = 1;    /* drive the menus by name: race, save, shop */
            sbk_settings.launcher = 0;
        } else if (strcmp(argv[i], "--shop") == 0) {
            extern int sbk_autonav_shop;
            sbk_autonav_shop = 1;
        } else if (strcmp(argv[i], "--saveevery") == 0 && i + 1 < argc) {
            extern int sbk_autonav_every;
            sbk_autonav_every = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--menutrace") == 0) {
            extern int sbk_menutrace;
            sbk_menutrace = 1;
        } else if (strcmp(argv[i], "--nopad") == 0) {
            extern int sbk_nopad;
            sbk_nopad = 1;  /* no gamepad: no Rumble Pak, no stray stick input */
        } else if (strcmp(argv[i], "--nopak") == 0) {
            nopak = 1;      /* no Controller Pak at all: nothing is opened or written */
        } else if (strcmp(argv[i], "--peek") == 0 && i + 1 < argc) {
            sbk_peek_add(argv[++i]);
        } else if (strcmp(argv[i], "--cmds") == 0 && i + 1 < argc) {
            sbk_input_play_set_cmdfile(argv[++i]);
        } else if (strcmp(argv[i], "--autoplay") == 0) {
            sbk_autoplay = 1;
            sbk_settings.launcher = 0;  /* unattended: never sit on a menu */
        } else if (strcmp(argv[i], "--trial") == 0 && i + 1 < argc) {
            sbk_trial_parse(argv[++i]);
        } else if (strcmp(argv[i], "--plan") == 0 && i + 1 < argc) {
            extern int sbk_plan_parse(const char *);
            sbk_plan_parse(argv[++i]);
        } else if (strcmp(argv[i], "--turbo") == 0) {
            sbk_turbo = 1;      /* no pacing: a retrace as soon as the game is idle */
        } else if (strcmp(argv[i], "--headless") == 0) {
            sbk_headless = 1;   /* skip display lists and presents; implies --turbo and --mute */
            sbk_turbo = 1;
            sbk_audio_muted = 1;
        } else if (strcmp(argv[i], "--status") == 0) {
            extern int sbk_status;
            sbk_status = 1;
        } else if (strcmp(argv[i], "--coursetrace") == 0) {
            extern int sbk_course_trace;
            sbk_course_trace = 1;
        } else if (strcmp(argv[i], "--dumpon") == 0) {
            sbk_dumpon = 1;
        } else if (strcmp(argv[i], "--nightmare") == 0) {
            sbk_nightmare = 1;
        } else if (strcmp(argv[i], "--soak") == 0) {
            sbk_settings.launcher = 0;
            sbk_autoplay = sbk_soak = 1;
        } else if (strcmp(argv[i], "--drawdistance") == 0 && i + 1 < argc) {
            sbk_far_scale = (float)atof(argv[++i]);
            if (sbk_far_scale < 0.25f) sbk_far_scale = 0.25f;
            sbk_settings.draw_distance = (int)sbk_far_scale;
            if (sbk_settings.draw_distance < 1) sbk_settings.draw_distance = 1;
            if (sbk_settings.draw_distance > 4) sbk_settings.draw_distance = 4;
        } else if (strcmp(argv[i], "--perf") == 0) {
            sbk_perf_enabled = 1;
            sbk_settings.perf = 1;
        } else if (strcmp(argv[i], "--racedbg") == 0) {
            sbk_race_debug_enabled = 1;
        } else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            sbk_ai_dump_start(argv[++i]);
        }
    }

    if (sbk_rom_load(rom) != 0) {
        fprintf(stderr, "usage: %s [--fullscreen[=WxH]|--fullscreen-desktop|--windowed] [--wide] [--novsync] [--trace] [--play SCRIPT|MOVIE.m64] [--record MOVIE.m64] [--frames N] [--hashframe] [--perf] [--autoplay] [--soak] [--nightmare] [--trial SPEC] [--plan C:CH:B:BO,..] [--pak FILE|--nopak] [--nopad] [--status] [--coursetrace] [--turbo] [--headless] [--mute] [--wav OUT.wav] [snowboardkids.z64]\n", argv[0]);
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
    if (fullscreen == 0 && getenv("SBK_FULLSCREEN") != NULL && getenv("SBK_FULLSCREEN")[0] == '1') fullscreen = 1;
    gfx_init(&gfx_sdl_gl13_wapi, &gfx_gl13_rapi, "Snowboard Kids", fullscreen > 0);
    sbk_settings.fullscreen = fullscreen > 0;
    sbk_settings_apply();
    sbk_input_init();
    if (!nopak) {
        sbk_pak_open(pak_path);
    } else {
        printf("sbk: no Controller Pak (--nopak)\n");
    }
    if (play != NULL && sbk_input_play_load(play) != 0) {
        return 1;
    }
    if (record != NULL && sbk_input_record_start(record) != 0) {
        return 1;
    }
    sbk_audio_out_init();

    if (!sbk_launcher_run()) {
        printf("sbk: quit from the launcher\n");
        sbk_audio_out_shutdown();
        SDL_Quit();
        return 0;
    }

    /* Boot: the game creates its boot thread and starts it. */
    printf("sbk: booting game (image at %p, RDRAM at 0x%08x)\n", (void *)main, SBK_RDRAM_BASE);
    sbk_game_main(NULL);

    /* Determinism: a retrace is delivered only once the game has gone idle
     * for the frame (nothing runnable but the polling game thread, which has
     * come up empty SBK_IDLE_POLLS times). The wall clock only paces delivery;
     * it never lands a retrace early or piles several onto a slow frame. Given
     * the same inputs, every run then sees each retrace at the same point in
     * its logic, which is what a movie needs. */
    next_retrace = now_usec();
    while (!sbk_input_quit_requested()) {
        double t;
        int idle;

        {
            /* game = everything the scheduler runs minus the tasks it dispatches */
            double t0 = sbk_perf_now();
            sbk_sched_run();
            sbk_perf_add(SBK_PERF_GAME, sbk_perf_now() - t0);
        }

        if (sbk_vi_swap_serial != presented) {
            presented = sbk_vi_swap_serial;
            SBK_PERF_TIMED(SBK_PERF_PRESENT, gfx_present());
        }

        idle = !sbk_sched_has_runnable() || sbk_poll_fail_count >= SBK_IDLE_POLLS;
        if (!idle) {
            continue; /* the game still has work for this frame */
        }

        t = now_usec();
        if (quit_now) break;
        if (!sbk_turbo && t < next_retrace) {
            double wait = next_retrace - t;
            if (wait > 2000.0) {
                SDL_Delay((Uint32)((wait - 1000.0) / 1000.0));
                sbk_perf_add(SBK_PERF_IDLE, sbk_perf_now() - t);
            }
            continue;
        }

        gfx_handle_events();
        sbk_ui_overlay_tick();
        sbk_input_play_poll();
        sbk_input_update();
        sbk_vi_retrace();
        sbk_ai_retrace();
        retraces++;
        sbk_perf_frame();
        sbk_autoplay_tick(retraces);
        if (sbk_perf_enabled && retraces % 60 == 0) {
            sbk_perf_report();
        }
        if (sbk_race_debug_enabled && retraces % 60 == 0) {
            sbk_race_debug(retraces);
        }
        if (retraces % 120 == 0) {
            extern unsigned sbk_stat_cont, sbk_task_count, sbk_stat_present;
            printf("sbk: t=%lus retraces=%lu gfxtasks=%u presents=%u dma=%u contreads=%u swaps=%u audiopeak=%u\n",
                   retraces / 60, retraces, sbk_task_count, sbk_stat_present, sbk_stat_dma, sbk_stat_cont,
                   sbk_vi_swap_serial, sbk_audio_peak);
        }
        next_retrace += RETRACE_USEC;
        if (t - next_retrace > 250000.0) {
            next_retrace = t; /* fell far behind (slow frame, window drag): resync the pacing */
        }
        if (sbk_hash_frames && sbk_stat_dma != last_dma) {
            last_dma = sbk_stat_dma;
            printf("sbk: retrace %lu: dma=%u\n", retraces, last_dma); /* run-to-run fingerprint (--hashframe) */
        }
        if (max_frames != 0 && retraces >= max_frames) {
            break;
        }
    }

    printf("sbk: exiting after %lu retraces\n", retraces);
    if (sbk_hash_frames) {
        printf("sbk: last frame hash %08x (swap %u)\n", sbk_last_frame_hash, sbk_vi_swap_serial);
    }
    sbk_input_play_shutdown();
    sbk_ai_dump_finish();
    sbk_audio_out_shutdown();
    SDL_Quit();
    return 0;
}
