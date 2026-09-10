#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "sbk_ctx.h"

#if defined(__APPLE__) && defined(__ppc__)

extern void sbk_ctx_trampoline(void);

void sbk_ctx_init(sbk_ctx *ctx, void *stack_base, size_t stack_size, void (*entry)(void *), void *arg) {
    /* Darwin PPC32: r1 must stay 16-byte aligned; the callee's prologue stores
     * LR at 8(r1) and the back chain at 0(r1), so leave a zeroed 64-byte
     * linkage + parameter area at the top of the stack. */
    unsigned int top = ((unsigned int)stack_base + (unsigned int)stack_size) & ~15u;
    unsigned int sp = top - 64;

    memset(ctx, 0, sizeof(*ctx));
    memset((void *)sp, 0, 64);
    ctx->r1 = sp;
    ctx->lr = (unsigned int)sbk_ctx_trampoline;
    ctx->gpr[14 - 13] = (unsigned int)entry;
    ctx->gpr[15 - 13] = (unsigned int)arg;
}

#else /* generic ucontext fallback (Linux PPC in the be-test rig, host dry runs) */

static void sbk_ctx_start(unsigned int hi, unsigned int lo, unsigned int ahi, unsigned int alo) {
    void (*entry)(void *) = (void (*)(void *))(((unsigned long)hi << 32) | lo);
    void *arg = (void *)(((unsigned long)ahi << 32) | alo);
    entry(arg);
    sbk_thread_exit();
}

void sbk_ctx_init(sbk_ctx *ctx, void *stack_base, size_t stack_size, void (*entry)(void *), void *arg) {
    unsigned long e = (unsigned long)entry, a = (unsigned long)arg;
    if (getcontext(&ctx->uc) != 0) {
        perror("getcontext");
        abort();
    }
    ctx->uc.uc_stack.ss_sp = stack_base;
    ctx->uc.uc_stack.ss_size = stack_size;
    ctx->uc.uc_link = NULL;
    makecontext(&ctx->uc, (void (*)(void))sbk_ctx_start, 4,
                (unsigned int)(e >> 32), (unsigned int)e, (unsigned int)(a >> 32), (unsigned int)a);
}

void sbk_ctx_switch(sbk_ctx *from, sbk_ctx *to) {
    if (swapcontext(&from->uc, &to->uc) != 0) {
        perror("swapcontext");
        abort();
    }
}

#endif
