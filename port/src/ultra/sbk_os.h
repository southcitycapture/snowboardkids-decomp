/* Internal interface of the libultra replacement layer (the "port OS"). */
#ifndef SBK_OS_H
#define SBK_OS_H

#include <stdint.h>
#include "ultra.h"

/* ---- boot ------------------------------------------------------------- */

/* The game's ROM image (8 MB, big-endian, exactly as on the cartridge). */
extern const uint8_t *sbk_rom;
extern unsigned long sbk_rom_size;

/* Emulated RDRAM: 4 MB mapped at 0x80000000 so that the handful of absolute
 * addresses baked into game data (title-demo replay pointers, the +0x80000000
 * physical-address trick in the menu renderer) keep working. */
#define SBK_RDRAM_BASE 0x80000000u
#define SBK_RDRAM_SIZE 0x00400000u

/* Turn an address found in game data into a host pointer. Three kinds show up:
 *  - N64 "physical" RDRAM addresses (< 0x10000000, e.g. after the game's
 *    `ptr + 0x80000000` wraparound): map into the RDRAM window;
 *  - KSEG0 RDRAM pointers (0x80xxxxxx): already valid, RDRAM is mapped 1:1;
 *  - native pointers into the executable's data (>= 0x10000000 thanks to the
 *    256 MB __PAGEZERO the port links with): already valid. */
static inline void *sbk_phys_to_host(uint32_t addr) {
    if (addr < 0x10000000u) {
        return (void *)(uintptr_t)(addr | SBK_RDRAM_BASE);
    }
    return (void *)(uintptr_t)addr;
}

void sbk_os_init(void);            /* threads, queues, event table */
void sbk_rdram_init(void);         /* map RDRAM at its fixed address */

/* ---- scheduler ---------------------------------------------------------- */

/* Run game threads until nothing is runnable or a thread polled an empty
 * queue with OS_MESG_NOBLOCK (a "poll yield"). Returns to the host loop. */
void sbk_sched_run(void);

/* True when at least one game thread is runnable right now. */
int sbk_sched_has_runnable(void);

/* Number of consecutive empty NOBLOCK polls since the last delivered message;
 * the host loop uses it to decide when the game is merely spinning. */
extern unsigned sbk_poll_fail_count;

/* Run fn(arg) on the host's own (big) stack, then resume the calling thread.
 * Used for anything that goes deep into SDL/OpenGL/libc. Safe from the host. */
void sbk_host_call(void (*fn)(void *), void *arg);

/* Deliver a hardware event (OS_EVENT_VI, OS_EVENT_SP, ...) to whichever queue
 * the game registered with osSetEventMesg. Callable from host or thread. */
void sbk_os_event(OSEvent event);

/* ---- devices ---------------------------------------------------------- */

void sbk_vi_retrace(void);         /* host: one vertical retrace happened */
extern void *sbk_vi_next_fb;       /* last osViSwapBuffer argument */
extern unsigned sbk_vi_swap_serial;/* incremented by every osViSwapBuffer */

void sbk_cont_update(void);        /* host: refresh controller state from SDL */

#endif
