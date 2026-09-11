/* SI devices: controllers, Controller Pak, Rumble Pak.
 *
 * One standard controller is always plugged into port 1, driven from the
 * host's keyboard/gamepad state (platform/input.c). Reads complete at once
 * and post the SI event, like the real hardware would a few hundred
 * microseconds later. The Controller Pak on port 1 is a
 * 32 KB image file (os_pfs.c); the Rumble Pak reports "not plugged". */
#include "ultra.h"
#include <string.h>
#include "sbk_os.h"
#include "../platform/input.h"

int sbk_pak_present(void); /* os_pfs.c */

static OSContStatus sbk_cont_status[MAXCONTROLLERS];
static OSContPad sbk_cont_pad[MAXCONTROLLERS];
static OSMesgQueue *sbk_si_mq;

s32 osContInit(OSMesgQueue *mq, u8 *bitpattern, OSContStatus *data) {
    int i;
    sbk_si_mq = mq;
    memset(sbk_cont_status, 0, sizeof(sbk_cont_status));
    for (i = 0; i < MAXCONTROLLERS; i++) {
        data[i].type = 0;
        data[i].status = 0;
        data[i].errno = CONT_NO_RESPONSE_ERROR;
    }
    for (i = 0; i < sbk_input_controller_count(); i++) {
        data[i].type = CONT_TYPE_NORMAL;
        data[i].status = (i == 0 && sbk_pak_present()) ? CONT_CARD_ON : 0;
        data[i].errno = 0;
    }
    memcpy(sbk_cont_status, data, sizeof(sbk_cont_status));
    *bitpattern = (u8)((1u << sbk_input_controller_count()) - 1u);
    return 0;
}

s32 osContReset(OSMesgQueue *mq, OSContStatus *data) {
    u8 pattern;
    return osContInit(mq, &pattern, data);
}

s32 osContSetCh(u8 ch) {
    (void)ch;
    return 0;
}

s32 osContStartQuery(OSMesgQueue *mq) {
    osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetQuery(OSContStatus *data) {
    memcpy(data, sbk_cont_status, sizeof(sbk_cont_status));
}

unsigned sbk_stat_cont;

s32 osContStartReadData(OSMesgQueue *mq) {
    int i;
    sbk_stat_cont++;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        if (sbk_cont_status[i].errno == 0) {
            sbk_input_read_pad(i, &sbk_cont_pad[i].button, &sbk_cont_pad[i].stick_x, &sbk_cont_pad[i].stick_y);
            if (i == 0) {
                sbk_input_play_step(&sbk_cont_pad[i].button, &sbk_cont_pad[i].stick_x, &sbk_cont_pad[i].stick_y);
            }
            sbk_cont_pad[i].errno = 0;
        } else {
            sbk_cont_pad[i].button = 0;
            sbk_cont_pad[i].stick_x = 0;
            sbk_cont_pad[i].stick_y = 0;
            sbk_cont_pad[i].errno = CONT_NO_RESPONSE_ERROR;
        }
    }
    osSendMesg(mq, NULL, OS_MESG_NOBLOCK);
    return 0;
}

void osContGetReadData(OSContPad *data) {
    memcpy(data, sbk_cont_pad, sizeof(sbk_cont_pad));
}

/* ---- Rumble Pak: the gamepad's rumble, on port 1 -------------------------
 * A real N64 cannot hold a Rumble Pak and a Controller Pak in one controller,
 * which is why the game asks to swap them; here both are always present, so
 * every prompt passes at once and saving still works. */

static unsigned motor_log;

s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int channel) {
    (void)mq;
    memset(pfs, 0, sizeof(*pfs));
    pfs->channel = channel;
    if (channel != 0 || !sbk_input_rumble_supported()) {
        return PFS_ERR_NOPACK;
    }
    pfs->status = PFS_MOTOR_INITIALIZED;
    return 0;
}

s32 osMotorStart(OSPfs *pfs) {
    if (pfs->channel != 0 || !sbk_input_rumble_supported()) return PFS_ERR_NOPACK;
    if (motor_log++ < 4) printf("sbk: rumble on\n");
    sbk_input_rumble(1);
    return 0;
}

s32 osMotorStop(OSPfs *pfs) {
    if (pfs->channel != 0 || !sbk_input_rumble_supported()) return PFS_ERR_NOPACK;
    sbk_input_rumble(0);
    return 0;
}

/* Controller Pak: see os_pfs.c (libultra's own pfs code over a file-backed image). */
