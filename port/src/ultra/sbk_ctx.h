/* Minimal stackful coroutine contexts for the libultra thread emulation.
 *
 * On Mac OS X / PowerPC the switch is 40 lines of assembly (sbk_ctx_ppc_darwin.s)
 * so it needs nothing from the OS. Other hosts fall back to <ucontext.h>. */
#ifndef SBK_CTX_H
#define SBK_CTX_H

#include <stddef.h>

#if defined(__APPLE__) && defined(__ppc__)
/* Layout is shared with sbk_ctx_ppc_darwin.s: keep the offsets in sync. */
typedef struct sbk_ctx {
    unsigned int r1;        /*  0: stack pointer            */
    unsigned int lr;        /*  4: return address           */
    unsigned int cr;        /*  8: condition register       */
    unsigned int gpr[19];   /* 12: r13..r31                 */
    double fpr[18];         /* 88: f14..f31 (8-byte aligned) */
} sbk_ctx;                  /* 232 bytes                    */
#else
#include <ucontext.h>
typedef struct sbk_ctx {
    ucontext_t uc;
} sbk_ctx;
#endif

/* Prepare `ctx` so that the first switch into it calls entry(arg) on the given
 * stack. When entry returns, sbk_thread_exit() is called (defined by the
 * scheduler) and must never return. */
void sbk_ctx_init(sbk_ctx *ctx, void *stack_base, size_t stack_size, void (*entry)(void *), void *arg);

/* Save the current context into `from` and resume `to`. Returns when someone
 * switches back to `from`. */
void sbk_ctx_switch(sbk_ctx *from, sbk_ctx *to);

/* Provided by the scheduler; called when a thread's entry function returns. */
void sbk_thread_exit(void);

#endif
