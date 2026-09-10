/* SI devices: controllers, Controller Pak, Rumble Pak.
 *
 * One standard controller is always plugged into port 1, driven from the
 * host's keyboard/gamepad state (platform/input.c). Reads complete at once
 * and post the SI event, like the real hardware would a few hundred
 * microseconds later. Controller Pak and Rumble Pak report "not plugged"
 * for now; a file-backed pak comes with milestone 2. */
#include "ultra.h"
#include <string.h>
#include "sbk_os.h"
#include "../platform/input.h"

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
        data[i].status = 0;
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

/* ---- Rumble Pak: absent ------------------------------------------------- */

s32 osMotorInit(OSMesgQueue *mq, OSPfs *pfs, int channel) {
    (void)mq;
    memset(pfs, 0, sizeof(*pfs));
    pfs->channel = channel;
    return PFS_ERR_NOPACK;
}

s32 osMotorStart(OSPfs *pfs) {
    (void)pfs;
    return PFS_ERR_NOPACK;
}

s32 osMotorStop(OSPfs *pfs) {
    (void)pfs;
    return PFS_ERR_NOPACK;
}

/* ---- Controller Pak: absent (milestone 2 replaces this file's tail) ------- */

s32 osPfsInitPak(OSMesgQueue *mq, OSPfs *pfs, int channel) {
    (void)mq;
    memset(pfs, 0, sizeof(*pfs));
    pfs->channel = channel;
    return PFS_ERR_NOPACK;
}

s32 osPfsRepairId(OSPfs *pfs) {
    (void)pfs;
    return PFS_ERR_NOPACK;
}

s32 osPfsChecker(OSPfs *pfs) {
    (void)pfs;
    return PFS_ERR_NOPACK;
}

s32 osPfsAllocateFile(OSPfs *pfs, u16 company_code, u32 game_code, u8 *game_name, u8 *ext_name, int file_size_in_bytes, s32 *file_no) {
    (void)pfs; (void)company_code; (void)game_code; (void)game_name; (void)ext_name; (void)file_size_in_bytes;
    *file_no = -1;
    return PFS_ERR_NOPACK;
}

s32 osPfsFindFile(OSPfs *pfs, u16 company_code, u32 game_code, u8 *game_name, u8 *ext_name, s32 *file_no) {
    (void)pfs; (void)company_code; (void)game_code; (void)game_name; (void)ext_name;
    *file_no = -1;
    return PFS_ERR_NOPACK;
}

s32 osPfsDeleteFile(OSPfs *pfs, u16 company_code, u32 game_code, u8 *game_name, u8 *ext_name) {
    (void)pfs; (void)company_code; (void)game_code; (void)game_name; (void)ext_name;
    return PFS_ERR_NOPACK;
}

s32 osPfsReadWriteFile(OSPfs *pfs, s32 file_no, u8 flag, int offset, int size_in_bytes, u8 *data_buffer) {
    (void)pfs; (void)file_no; (void)flag; (void)offset; (void)size_in_bytes; (void)data_buffer;
    return PFS_ERR_NOPACK;
}

s32 osPfsFileState(OSPfs *pfs, s32 file_no, OSPfsState *state) {
    (void)pfs; (void)file_no;
    memset(state, 0, sizeof(*state));
    return PFS_ERR_NOPACK;
}

s32 osPfsFreeBlocks(OSPfs *pfs, s32 *bytes_not_used) {
    (void)pfs;
    *bytes_not_used = 0;
    return PFS_ERR_NOPACK;
}

s32 osPfsNumFiles(OSPfs *pfs, s32 *max_files, s32 *files_used) {
    (void)pfs;
    *max_files = 0;
    *files_used = 0;
    return PFS_ERR_NOPACK;
}

s32 osPfsIsPlug(OSMesgQueue *mq, u8 *pattern) {
    (void)mq;
    *pattern = 0;
    return 0;
}
