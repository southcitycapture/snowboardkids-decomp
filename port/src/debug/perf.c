/* --perf: where the frame time goes.
 *
 * The host loop and the task dispatch stamp the phases of every frame; once
 * a second the averages and worst cases are printed and, if the window is
 * up, put in its title so they can be read off the G4's own screen:
 *
 *   sbk-perf: 60.0 fps cpu 71% | game 2.9 gfx 6.1 aud 0.8 gl 3.2 idle 3.5 ms | max 19.4 | 1240 tris 38 draws 12 tex 61KB
 *
 * game = the game's own threads (everything sbk_sched_run runs except tasks),
 * gfx = display-list interpretation and GL calls issued from it, aud = the
 * audio command list, gl = present (swap, including any vsync wait), idle =
 * sleeping for the next retrace. cpu = this process, from getrusage. */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include "perf.h"

int sbk_perf_enabled;

static double acc[SBK_PERF_PHASES];
static double peak[SBK_PERF_PHASES]; /* longest single stamp per phase this second */
static double frame_max;
static double frame_start;
static unsigned frames;
static unsigned long stat_tris, stat_draws, stat_tex, stat_tex_bytes;
static double last_report, last_cpu;

unsigned long sbk_perf_tris, sbk_perf_draws, sbk_perf_tex, sbk_perf_tex_bytes; /* bumped by gfx_pc */

double sbk_perf_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

void sbk_perf_add(int phase, double usec) {
    if (phase >= 0 && phase < SBK_PERF_PHASES) {
        acc[phase] += usec;
        if (usec > peak[phase]) peak[phase] = usec;
    }
}

/* Called once per retrace: closes the frame's accounting. */
void sbk_perf_frame(void) {
    double now = sbk_perf_now();
    if (frame_start != 0.0) {
        double len = now - frame_start;
        if (len > frame_max) frame_max = len;
    }
    frame_start = now;
    frames++;
    stat_tris += sbk_perf_tris; stat_draws += sbk_perf_draws;
    stat_tex += sbk_perf_tex; stat_tex_bytes += sbk_perf_tex_bytes;
    sbk_perf_tris = sbk_perf_draws = sbk_perf_tex = sbk_perf_tex_bytes = 0;
}

static double cpu_usec(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec * 1e6 + ru.ru_utime.tv_usec + (double)ru.ru_stime.tv_sec * 1e6 + ru.ru_stime.tv_usec;
}

extern void gfx_set_window_title(const char *title); /* gfx_pc.c */

void sbk_perf_report(void) {
    double now = sbk_perf_now(), cpu = cpu_usec();
    double wall = now - last_report;
    char line[256];
    if (!sbk_perf_enabled) return;
    if (last_report == 0.0 || wall < 1000000.0) {
        if (last_report == 0.0) { last_report = now; last_cpu = cpu; }
        return;
    }
    if (frames == 0) frames = 1;
    acc[SBK_PERF_GAME] -= acc[SBK_PERF_GFX] + acc[SBK_PERF_AUDIO]; /* tasks run inside the scheduler */
    if (acc[SBK_PERF_GAME] < 0) acc[SBK_PERF_GAME] = 0;
    snprintf(line, sizeof(line),
             "%.1f Hz cpu %.0f%% | game %.1f gfx %.1f aud %.1f gl %.1f idle %.1f ms | frame max %.1f (game %.1f gfx %.1f aud %.1f gl %.1f: end %.1f finish %.1f swap %.1f) | %lu tris %lu draws %lu tex %luKB",
             frames * 1000000.0 / wall, 100.0 * (cpu - last_cpu) / wall,
             acc[SBK_PERF_GAME] / frames / 1000.0, acc[SBK_PERF_GFX] / frames / 1000.0,
             acc[SBK_PERF_AUDIO] / frames / 1000.0, acc[SBK_PERF_PRESENT] / frames / 1000.0,
             acc[SBK_PERF_IDLE] / frames / 1000.0, frame_max / 1000.0,
             peak[SBK_PERF_GAME] / 1000.0, peak[SBK_PERF_GFX] / 1000.0, peak[SBK_PERF_AUDIO] / 1000.0, peak[SBK_PERF_PRESENT] / 1000.0,
             peak[SBK_PERF_ENDFRAME] / 1000.0, peak[SBK_PERF_FINISH] / 1000.0, peak[SBK_PERF_SWAP] / 1000.0,
             stat_tris / frames, stat_draws / frames, stat_tex / frames, stat_tex_bytes / frames / 1024);
    printf("sbk-perf: %s\n", line);
    gfx_set_window_title(line);
    memset(acc, 0, sizeof(acc));
    memset(peak, 0, sizeof(peak));
    frame_max = 0.0; frames = 0;
    stat_tris = stat_draws = stat_tex = stat_tex_bytes = 0;
    last_report = now; last_cpu = cpu;
}
