/* VI: the host loop calls sbk_vi_retrace() once per emulated vertical
 * retrace; osViSetEvent's message goes out every `retraceCount` retraces,
 * exactly as the VI manager thread did. Buffer swaps are recorded so the
 * renderer can present the right frame. */
#include "ultra.h"
#include <string.h>
#include "sbk_os.h"

static OSMesgQueue *sbk_vi_mq;
static OSMesg sbk_vi_msg;
static u32 sbk_vi_retrace_count = 1;
static u32 sbk_vi_retrace_left = 1;
static void *sbk_vi_current_fb;
void *sbk_vi_next_fb;
unsigned sbk_vi_swap_serial;
static u8 sbk_vi_black;
static u32 sbk_vi_features;

void osCreateViManager(OSPri pri) {
    (void)pri;
}

void osViSetMode(OSViMode *mode) {
    (void)mode; /* always 320x240 NTSC LAN1 in this game */
}

void osViSetEvent(OSMesgQueue *mq, OSMesg msg, u32 retraceCount) {
    sbk_vi_mq = mq;
    sbk_vi_msg = msg;
    sbk_vi_retrace_count = retraceCount != 0 ? retraceCount : 1;
    sbk_vi_retrace_left = sbk_vi_retrace_count;
}

void osViBlack(u8 active) {
    sbk_vi_black = active;
}

void osViSetSpecialFeatures(u32 func) {
    sbk_vi_features = func;
}

void osViSetXScale(f32 value) {
    (void)value;
}

void osViSetYScale(f32 value) {
    (void)value;
}

void osViSwapBuffer(void *frameBufPtr) {
    sbk_vi_next_fb = frameBufPtr;
    sbk_vi_swap_serial++;
}

void *osViGetCurrentFramebuffer(void) {
    return sbk_vi_current_fb;
}

void *osViGetNextFramebuffer(void) {
    return sbk_vi_next_fb;
}

u32 osViGetStatus(void) {
    return 0;
}

int sbk_vi_is_black(void) {
    return sbk_vi_black;
}

/* One vertical retrace: the swapped-in buffer becomes current, then the
 * retrace message is delivered on the game's schedule. */
void sbk_vi_retrace(void) {
    sbk_vi_current_fb = sbk_vi_next_fb;
    if (sbk_vi_mq != NULL && --sbk_vi_retrace_left == 0) {
        sbk_vi_retrace_left = sbk_vi_retrace_count;
        osSendMesg(sbk_vi_mq, sbk_vi_msg, OS_MESG_NOBLOCK);
    }
}
