/* Bridge from an M_GFXTASK OSTask to the gfx_pc display-list interpreter. */
#include "../ultra/ultra.h"
#include <PR/gbi.h>
#include "../ultra/sbk_os.h"
#include "gfx_pc.h"

extern int sbk_trace;
int sbk_dump_task = -1; /* --dumpdl N: print the whole display list of gfx task N */

extern void *gfx_debug_seg_addr(uint32_t w1);
extern void gfx_debug_set_segment(uint32_t seg, uint32_t base);

static void dump_dl(const Gfx *dl, unsigned max, int depth) {
    unsigned i;
    for (i = 0; i < max; i++) {
        unsigned op = dl[i].words.w0 >> 24;
        printf("sbk-dl:%*s %08x %08x\n", depth * 2, "", (unsigned)dl[i].words.w0, (unsigned)dl[i].words.w1);
        if (op == (uint8_t)G_MOVEWORD && (dl[i].words.w0 & 0xFF) == G_MW_SEGMENT) {
            gfx_debug_set_segment(((dl[i].words.w0 >> 8) & 0xFFFF) / 4, dl[i].words.w1); /* keep the dump's view current */
        }
        if (op == (uint8_t)G_DL && depth < 4) {
            unsigned kind = (dl[i].words.w0 >> 16) & 0xFF;
            uint32_t target = (uint32_t)dl[i].words.w1;
            if (target >= 0x10000000u || (target >> 24) <= 0xF) {
                dump_dl((const Gfx *)gfx_debug_seg_addr(target), 2048, depth + 1);
            }
            if (kind == G_DL_NOPUSH) break; /* branch, not call */
        }
        if (op == (uint8_t)G_ENDDL) break;
    }
}

void sbk_gfx_task(OSTask *task) {
    Gfx *dl = (Gfx *)sbk_phys_to_host((uint32_t)(uintptr_t)task->t.data_ptr);
    static int count;
    count++;
    if (count == sbk_dump_task) {
        extern int sbk_tex_dump_left;
        extern void gfx_debug_flush_texture_cache(void);
        extern int sbk_frame_dump_left;
        gfx_debug_flush_texture_cache();
        sbk_tex_dump_left = 120;
        sbk_frame_dump_left = 3;
    }
    if ((sbk_trace && count <= 6) || count == sbk_dump_task) {
        unsigned n = task->t.data_size / sizeof(Gfx);
        printf("sbk-dl: task %d, %u commands at %p\n", count, n, (void *)dl);
        dump_dl(dl, count == sbk_dump_task ? n : 96, 0);
    }
    gfx_run(dl);
}
