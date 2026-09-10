/* Startup copy table for globals pinned at their N64 addresses (gen_pins.py). */
#ifndef SBK_PINS_H
#define SBK_PINS_H

#include <stdint.h>

struct sbk_pin {
    const char *twin;   /* native copy holding the initial value */
    uint32_t addr;      /* N64 address inside the emulated RDRAM */
    uint32_t size;               /* cap: bytes up to the next pinned symbol */
    const unsigned long *twin_size; /* sizeof the native twin, or NULL (asm twins: size is exact) */
};

extern const struct sbk_pin sbk_pin_table[];

/* Copy every initialised global into RDRAM. Call after sbk_rdram_init(). */
void sbk_pin_init(void);

#endif
