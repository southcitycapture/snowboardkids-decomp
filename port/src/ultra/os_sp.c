/* RSP tasks. A graphics task is interpreted by gfx_pc on the spot; an audio
 * task by the aspMain command-list interpreter. Both complete synchronously,
 * so by the time the game asks whether a task yielded, it has already
 * finished and the SP/DP events are already in the queues. */
#include "ultra.h"
#include <stdio.h>
#include "sbk_os.h"
#include "../debug/perf.h"

extern void sbk_gfx_task(OSTask *task);   /* port/src/gfx/gfx_task.c   */
extern void sbk_audio_task(OSTask *task); /* port/src/audio/audio_task.c */

static OSTask *sbk_sp_loaded;
unsigned sbk_task_count;

extern int sbk_headless;

static void sbk_run_gfx(void *arg) {
    if (sbk_headless) return;
    SBK_PERF_TIMED(SBK_PERF_GFX, sbk_gfx_task((OSTask *)arg));
}

static void sbk_run_audio(void *arg) {
    SBK_PERF_TIMED(SBK_PERF_AUDIO, sbk_audio_task((OSTask *)arg));
}

void osSpTaskLoad(OSTask *tp) {
    sbk_sp_loaded = tp;
    tp->t.flags &= ~OS_TASK_YIELDED;
}

void osSpTaskStartGo(OSTask *tp) {
    if (tp == NULL) {
        tp = sbk_sp_loaded;
    }
    switch (tp->t.type) {
        case M_GFXTASK:
            if (sbk_task_count++ < 4) {
                printf("sbk: gfx task %u: dl=%p size=%u\n", sbk_task_count, (void *)tp->t.data_ptr, (unsigned)tp->t.data_size);
            }
            sbk_host_call(sbk_run_gfx, tp);
            sbk_os_event(OS_EVENT_SP);
            sbk_os_event(OS_EVENT_DP);
            break;
        case M_AUDTASK:
            sbk_host_call(sbk_run_audio, tp);
            sbk_os_event(OS_EVENT_SP);
            break;
        default:
            fprintf(stderr, "sbk: unknown SP task type %u\n", (unsigned)tp->t.type);
            sbk_os_event(OS_EVENT_SP);
            break;
    }
}

void osSpTaskYield(void) {
    /* Nothing is running on the RSP between tasks: nothing to yield. */
}

OSYieldResult osSpTaskYielded(OSTask *tp) {
    (void)tp;
    return 0; /* the task completed instead of yielding */
}
