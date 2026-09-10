/* libultra odds and ends: initialization, cache maintenance, interrupt masks,
 * address conversion, time. The host is cache coherent and single threaded,
 * so almost everything here is a no-op with the right return value. */
#include "ultra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include "sbk_os.h"

/* libultra globals the game reads (linker_scripts/libultra_syms.ld). */
s32 osTvType = OS_TV_NTSC;
void *osRomBase = (void *)0x10000000;
s32 osResetType = 0;
u32 osMemSize = SBK_RDRAM_SIZE;
s32 osAppNMIBuffer[16];

void osInitialize(void) {
}

void osInvalDCache(void *vaddr, s32 nbytes) {
    (void)vaddr;
    (void)nbytes;
}

void osInvalICache(void *vaddr, s32 nbytes) {
    (void)vaddr;
    (void)nbytes;
}

void osWritebackDCache(void *vaddr, s32 nbytes) {
    (void)vaddr;
    (void)nbytes;
}

void osWritebackDCacheAll(void) {
}

static OSIntMask sbk_int_mask = OS_IM_ALL;

OSIntMask osSetIntMask(OSIntMask mask) {
    OSIntMask prev = sbk_int_mask;
    sbk_int_mask = mask;
    return prev;
}

OSIntMask osGetIntMask(void) {
    return sbk_int_mask;
}

/* The game only ever feeds RDRAM pointers through here and only uses the
 * result as an RSP/RDP address (display lists, audio buffers), which the port
 * maps back with sbk_phys_to_host(). */
u32 osVirtualToPhysical(void *vaddr) {
    u32 a = (u32)(uintptr_t)vaddr;
    if (a >= SBK_RDRAM_BASE && a < SBK_RDRAM_BASE + SBK_RDRAM_SIZE) {
        return a & 0x1FFFFFFFu;
    }
    return a; /* native pointer: leave it alone */
}

void *osPhysicalToVirtual(u32 paddr) {
    return sbk_phys_to_host(paddr);
}

/* 46.875 MHz counter, as on the real CPU. Nothing in the game uses it, but
 * libultra's own audio code may. */
static u64 sbk_now_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (u64)tv.tv_sec * 1000000u + (u64)tv.tv_usec;
}

u32 osGetCount(void) {
    return (u32)(sbk_now_usec() * 46875u / 1000u);
}

OSTime osGetTime(void) {
    return sbk_now_usec() * 46875u / 1000u;
}

void osSetTime(OSTime t) {
    (void)t;
}

/* ---- emulated RDRAM ---------------------------------------------------- */

void sbk_rdram_init(void) {
    void *p = mmap((void *)(uintptr_t)SBK_RDRAM_BASE, SBK_RDRAM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED || (uintptr_t)p != SBK_RDRAM_BASE) {
        fprintf(stderr, "sbk: cannot map RDRAM at 0x%08x\n", SBK_RDRAM_BASE);
        exit(1);
    }
    memset(p, 0, SBK_RDRAM_SIZE);
}
