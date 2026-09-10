/* Bridge from an M_GFXTASK OSTask to the gfx_pc display-list interpreter. */
#include "../ultra/ultra.h"
#include <PR/gbi.h>
#include "../ultra/sbk_os.h"
#include "gfx_pc.h"

extern int sbk_trace;

void sbk_gfx_task(OSTask *task) {
    Gfx *dl = (Gfx *)sbk_phys_to_host((uint32_t)(uintptr_t)task->t.data_ptr);
    static int dumped;
    if (sbk_trace && dumped < 6) {
        unsigned n = task->t.data_size / sizeof(Gfx), i;
        dumped++;
        printf("sbk-dl: task %d, %u commands at %p\n", dumped, n, (void *)dl);
        for (i = 0; i < n && i < 96; i++) {
            printf("sbk-dl:   %08x %08x\n", (unsigned)dl[i].words.w0, (unsigned)dl[i].words.w1);
            if ((dl[i].words.w0 >> 24) == G_ENDDL) break;
        }
    }
    gfx_run(dl);
}
