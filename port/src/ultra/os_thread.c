/* libultra threads, message queues and events, re-implemented as cooperative
 * coroutines on one host thread.
 *
 * Semantics follow libultra: a priority-ordered run queue, FIFO among equal
 * priorities, a thread runs until it blocks or a higher-priority thread is
 * woken. What libultra did from interrupt handlers (VI retrace, SP done, ...)
 * the host loop does from sbk_os_event() while no game thread is running.
 *
 * Two port-specific rules make this work without touching game code:
 *   1. A thread that polls an empty queue with OS_MESG_NOBLOCK yields to the
 *      host ("poll yield"); on the N64 that spin loop was only ever interrupted
 *      by hardware events, which is exactly what the host loop then delivers.
 *   2. A thread that lowers its own priority to OS_PRIORITY_IDLE is parked for
 *      good: the game's boot thread does this and then spins forever, and the
 *      host loop is the idle thread here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ultra.h"
#include <sys/mman.h>
#include "sbk_ctx.h"
#include "sbk_os.h"

#define SBK_THREAD_STACK_SIZE (1024 * 1024)
#define SBK_GUARD_SIZE 4096

struct sbk_thread {
    sbk_ctx ctx;
    void (*entry)(void *);
    void *arg;
    void *stack;
    int parked;
};

/* The bookkeeping hides inside the OSThread's (unused) MIPS register context. */
typedef char sbk_thread_fits_in_context[sizeof(struct sbk_thread) <= sizeof(__OSThreadContext) ? 1 : -1];
#define ST(t) ((struct sbk_thread *)&(t)->context)

static OSThread *sbk_run_queue;   /* highest priority first */
static OSThread *sbk_current;     /* NULL while the host loop runs */
static OSThread *sbk_all_threads; /* tlnext chain */
static sbk_ctx sbk_host_ctx;
static int sbk_poll_yield_pending;
unsigned sbk_poll_fail_count;

static void (*sbk_hc_fn)(void *);
static void *sbk_hc_arg;
static OSThread *sbk_hc_thread;

static OSMesgQueue *sbk_event_mq[OS_NUM_EVENTS];
static OSMesg sbk_event_msg[OS_NUM_EVENTS];

int sbk_trace; /* set by --trace: log thread events (first few hundred) */
static unsigned sbk_trace_left = 400;
#define TRACE(...) do { if (sbk_trace && sbk_trace_left > 0) { sbk_trace_left--; printf("sbk-thr: " __VA_ARGS__); } } while (0)
static int sbk_tid(OSThread *t) { return t != NULL ? (int)t->id : -1; }

static void sbk_fatal(const char *what) {
    fprintf(stderr, "sbk os: %s\n", what);
    abort();
}

/* ---- thread queues (same shape as libultra's __osEnqueueThread etc.) ---- */

static void sbk_enqueue(OSThread **queue, OSThread *t) {
    OSThread *prev = NULL;
    OSThread *cur = *queue;
    while (cur != NULL && cur->priority >= t->priority) {
        prev = cur;
        cur = cur->next;
    }
    t->next = cur;
    t->queue = queue;
    if (prev != NULL) {
        prev->next = t;
    } else {
        *queue = t;
    }
}

static OSThread *sbk_pop(OSThread **queue) {
    OSThread *t = *queue;
    if (t != NULL) {
        *queue = t->next;
        t->next = NULL;
    }
    return t;
}

static void sbk_dequeue(OSThread **queue, OSThread *t) {
    OSThread **pp = queue;
    while (*pp != NULL && *pp != t) {
        pp = &(*pp)->next;
    }
    if (*pp == t) {
        *pp = t->next;
    }
    t->next = NULL;
}

/* ---- context switching ------------------------------------------------ */

/* Switch from whoever is running (a thread, or the host) to t; t == NULL
 * means back to the host loop. */
static void sbk_switch_to(OSThread *t) {
    OSThread *prev = sbk_current;
    sbk_ctx *from = prev != NULL ? &ST(prev)->ctx : &sbk_host_ctx;
    sbk_ctx *to = t != NULL ? &ST(t)->ctx : &sbk_host_ctx;
    TRACE("switch %d -> %d (ctx %p sp=0x%x lr=0x%x)\n", sbk_tid(prev), sbk_tid(t), (void *)to,
          t != NULL ? ST(t)->ctx.r1 : sbk_host_ctx.r1, t != NULL ? ST(t)->ctx.lr : sbk_host_ctx.lr);
    sbk_current = t;
    if (t != NULL) {
        t->state = OS_STATE_RUNNING;
    }
    if (from != to) {
        sbk_ctx_switch(from, to);
    }
}

/* The current thread has already been queued/blocked/stopped: run the best
 * runnable thread, or return to the host if there is none. */
static void sbk_dispatch(void) {
    sbk_switch_to(sbk_pop(&sbk_run_queue));
}

/* If a runnable thread outranks the current one, hand over to it now. */
static void sbk_maybe_preempt(void) {
    if (sbk_current != NULL && sbk_run_queue != NULL && sbk_run_queue->priority > sbk_current->priority) {
        OSThread *me = sbk_current;
        me->state = OS_STATE_RUNNABLE;
        sbk_enqueue(&sbk_run_queue, me);
        sbk_dispatch();
    }
}

static void sbk_poll_yield(void) {
    OSThread *me = sbk_current;
    sbk_poll_fail_count++;
    if (me == NULL) {
        return;
    }
    me->state = OS_STATE_RUNNABLE;
    sbk_enqueue(&sbk_run_queue, me);
    sbk_poll_yield_pending = 1;
    sbk_switch_to(NULL);
}

void sbk_sched_run(void) {
    for (;;) {
        if (sbk_hc_fn != NULL) {
            void (*fn)(void *) = sbk_hc_fn;
            OSThread *t = sbk_hc_thread;
            sbk_hc_fn = NULL;
            sbk_hc_thread = NULL;
            fn(sbk_hc_arg);
            sbk_switch_to(t);
            continue;
        }
        if (sbk_poll_yield_pending) {
            sbk_poll_yield_pending = 0;
            return;
        }
        if (sbk_run_queue == NULL) {
            return;
        }
        sbk_dispatch();
    }
}

int sbk_sched_has_runnable(void) {
    return sbk_run_queue != NULL;
}

void sbk_host_call(void (*fn)(void *), void *arg) {
    if (sbk_current == NULL) {
        fn(arg);
        return;
    }
    sbk_hc_fn = fn;
    sbk_hc_arg = arg;
    sbk_hc_thread = sbk_current;
    sbk_switch_to(NULL);
}

/* ---- thread lifecycle -------------------------------------------------- */

static void sbk_thread_main(void *arg) {
    OSThread *t = (OSThread *)arg;
    ST(t)->entry(ST(t)->arg);
}

void sbk_thread_exit(void) {
    OSThread *t = sbk_current;
    TRACE("exit id=%d\n", sbk_tid(t));
    if (t == NULL) {
        sbk_fatal("sbk_thread_exit from host");
    }
    t->state = OS_STATE_STOPPED;
    sbk_dispatch();
    sbk_fatal("resumed a finished thread");
}

void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri) {
    struct sbk_thread *st;
    (void)sp; /* the game's N64 stacks are far too small for native code */

    memset(t, 0, sizeof(*t));
    t->id = id;
    t->priority = pri;
    t->state = OS_STATE_STOPPED;
    t->next = NULL;
    t->queue = NULL;

    st = ST(t);
    st->entry = entry;
    st->arg = arg;
    st->parked = 0;
    /* The executable is linked with a 256 MB __PAGEZERO, so any mapping the
     * kernel hands out sits above the N64 address range and stack data
     * referenced from display lists is never mistaken for an N64 address.
     * (Never MAP_FIXED here: Leopard loads CoreAudio's components at 0x70000000.) */
    st->stack = mmap(NULL, SBK_THREAD_STACK_SIZE + SBK_GUARD_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (st->stack == MAP_FAILED) {
        sbk_fatal("cannot allocate a thread stack");
    }
    mprotect(st->stack, SBK_GUARD_SIZE, PROT_NONE);
    sbk_ctx_init(&st->ctx, (char *)st->stack + SBK_GUARD_SIZE, SBK_THREAD_STACK_SIZE, sbk_thread_main, t);

    t->tlnext = sbk_all_threads;
    sbk_all_threads = t;
    TRACE("create id=%d pri=%d thread=%p entry=%p stack=%p sizeof(OSThread)=%u\n", (int)id, (int)pri, (void *)t, (void *)entry, st->stack, (unsigned)sizeof(OSThread));
}

void osDestroyThread(OSThread *t) {
    if (t == NULL) {
        t = sbk_current;
    }
    if (t == sbk_current) {
        t->state = OS_STATE_STOPPED;
        sbk_dispatch();
        sbk_fatal("resumed a destroyed thread");
    }
    if (t->state == OS_STATE_RUNNABLE || t->state == OS_STATE_WAITING) {
        sbk_dequeue(t->queue, t);
    }
    t->state = OS_STATE_STOPPED;
    t->queue = NULL;
}

void osStartThread(OSThread *t) {
    TRACE("start id=%d state=%d from %d\n", sbk_tid(t), (int)t->state, sbk_tid(sbk_current));
    if (ST(t)->parked) {
        return;
    }
    switch (t->state) {
        case OS_STATE_STOPPED:
            if (t->queue == NULL || t->queue == &sbk_run_queue) {
                t->state = OS_STATE_RUNNABLE;
                sbk_enqueue(&sbk_run_queue, t);
            } else {
                /* was stopped while blocked on a message queue: resume waiting */
                t->state = OS_STATE_WAITING;
                sbk_enqueue(t->queue, t);
            }
            break;
        default:
            break;
    }
    sbk_maybe_preempt();
}

void osStopThread(OSThread *t) {
    if (t == NULL) {
        t = sbk_current;
    }
    if (t == NULL) {
        return;
    }
    TRACE("stop id=%d state=%d from %d\n", sbk_tid(t), (int)t->state, sbk_tid(sbk_current));
    switch (t->state) {
        case OS_STATE_RUNNING:
            t->state = OS_STATE_STOPPED;
            t->queue = NULL;
            sbk_dispatch();
            break; /* resumed later by osStartThread */
        case OS_STATE_RUNNABLE:
        case OS_STATE_WAITING:
            sbk_dequeue(t->queue, t); /* keep t->queue so osStartThread can restore the wait */
            t->state = OS_STATE_STOPPED;
            break;
        default:
            break;
    }
}

void osYieldThread(void) {
    OSThread *me = sbk_current;
    if (me == NULL) {
        return;
    }
    me->state = OS_STATE_RUNNABLE;
    sbk_enqueue(&sbk_run_queue, me);
    sbk_dispatch();
}

void osSetThreadPri(OSThread *t, OSPri pri) {
    if (t == NULL) {
        t = sbk_current;
    }
    if (t == NULL) {
        return;
    }
    TRACE("setpri id=%d pri=%d\n", sbk_tid(t), (int)pri);
    if (pri == OS_PRIORITY_IDLE && t == sbk_current) {
        /* The boot thread becoming the idle spinner: park it forever. */
        ST(t)->parked = 1;
        t->priority = pri;
        t->state = OS_STATE_STOPPED;
        sbk_dispatch();
        sbk_fatal("resumed the parked idle thread");
    }
    t->priority = pri;
    if (t->state == OS_STATE_RUNNABLE || t->state == OS_STATE_WAITING) {
        OSThread **q = t->queue;
        sbk_dequeue(q, t);
        sbk_enqueue(q, t);
    }
    sbk_maybe_preempt();
}

OSPri osGetThreadPri(OSThread *t) {
    if (t == NULL) {
        t = sbk_current;
    }
    return t != NULL ? t->priority : OS_PRIORITY_IDLE;
}

OSId osGetThreadId(OSThread *t) {
    if (t == NULL) {
        t = sbk_current;
    }
    return t != NULL ? t->id : 0;
}

/* ---- message queues ----------------------------------------------------- */

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count) {
    mq->mtqueue = NULL;
    mq->fullqueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

static void sbk_wake_one(OSThread **queue) {
    OSThread *t = sbk_pop(queue);
    if (t != NULL) {
        t->state = OS_STATE_RUNNABLE;
        sbk_enqueue(&sbk_run_queue, t);
        sbk_maybe_preempt();
    }
}

s32 osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) {
    s32 last;
    while (mq->validCount >= mq->msgCount) {
        OSThread *me = sbk_current;
        if (flag != OS_MESG_BLOCK || me == NULL) {
            return -1;
        }
        me->state = OS_STATE_WAITING;
        sbk_enqueue(&mq->fullqueue, me);
        sbk_dispatch();
    }
    last = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[last] = msg;
    mq->validCount++;
    sbk_poll_fail_count = 0;
    sbk_wake_one(&mq->mtqueue);
    return 0;
}

s32 osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) {
    while (mq->validCount >= mq->msgCount) {
        OSThread *me = sbk_current;
        if (flag != OS_MESG_BLOCK || me == NULL) {
            return -1;
        }
        me->state = OS_STATE_WAITING;
        sbk_enqueue(&mq->fullqueue, me);
        sbk_dispatch();
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    mq->msg[mq->first] = msg;
    mq->validCount++;
    sbk_poll_fail_count = 0;
    sbk_wake_one(&mq->mtqueue);
    return 0;
}

s32 osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag) {
    if (mq->validCount == 0 && flag == OS_MESG_NOBLOCK) {
        sbk_poll_yield();
        if (mq->validCount == 0) {
            return -1;
        }
    }
    while (mq->validCount == 0) {
        OSThread *me = sbk_current;
        if (me == NULL) {
            sbk_fatal("host blocked in osRecvMesg");
        }
        me->state = OS_STATE_WAITING;
        sbk_enqueue(&mq->mtqueue, me);
        sbk_dispatch();
    }
    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    sbk_wake_one(&mq->fullqueue);
    return 0;
}

/* ---- events --------------------------------------------------------------- */

void osSetEventMesg(OSEvent event, OSMesgQueue *mq, OSMesg msg) {
    if (event < OS_NUM_EVENTS) {
        sbk_event_mq[event] = mq;
        sbk_event_msg[event] = msg;
    }
}

void sbk_os_event(OSEvent event) {
    if (event < OS_NUM_EVENTS && sbk_event_mq[event] != NULL) {
        osSendMesg(sbk_event_mq[event], sbk_event_msg[event], OS_MESG_NOBLOCK);
    }
}

void sbk_os_init(void) {
    memset(sbk_event_mq, 0, sizeof(sbk_event_mq));
    memset(sbk_event_msg, 0, sizeof(sbk_event_msg));
    sbk_run_queue = NULL;
    sbk_current = NULL;
    sbk_poll_fail_count = 0;
}
