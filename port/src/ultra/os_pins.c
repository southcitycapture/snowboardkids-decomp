/* Copy the initial values of pinned globals into the emulated RDRAM. */
#include <stdio.h>
#include <string.h>
#include "sbk_pins.h"
#include "sbk_os.h"

void sbk_pin_init(void) {
    const struct sbk_pin *p;
    unsigned n = 0, bytes = 0;
    for (p = sbk_pin_table; p->twin != NULL; p++) {
        if (p->addr < SBK_RDRAM_BASE || p->addr + p->size > SBK_RDRAM_BASE + SBK_RDRAM_SIZE) {
            continue;
        }
        memcpy((void *)(uintptr_t)p->addr, p->twin, p->size);
        n++;
        bytes += p->size;
    }
    printf("sbk: pinned data: %u objects, %u bytes copied into RDRAM\n", n, bytes);
}
