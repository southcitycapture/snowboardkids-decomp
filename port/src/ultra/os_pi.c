/* PI (cartridge) DMA: the ROM image lives in host memory, so a DMA is a
 * memcpy followed by the completion message the PI manager would have sent. */
#include "ultra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sbk_os.h"

const uint8_t *sbk_rom;
unsigned long sbk_rom_size;

static OSPiHandle sbk_cart_handle;

int sbk_rom_load(const char *path) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (f == NULL) {
        fprintf(stderr, "sbk: cannot open ROM %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0x1000 || n > (64 << 20)) {
        fprintf(stderr, "sbk: %s is not a ROM image\n", path);
        fclose(f);
        return -1;
    }
    buf = malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "sbk: short read on %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    /* Any of the three byte orders a dump gets saved in.  The first word of
     * every N64 ROM is 0x80371240, so which permutation of those four bytes
     * the file starts with names the order outright -- the extension is never
     * consulted, and a .z64 that is really a .v64 still loads. */
    if (buf[0] == 0x37 && buf[1] == 0x80 && buf[2] == 0x40 && buf[3] == 0x12) {
        long i;
        for (i = 0; i + 1 < n; i += 2) { uint8_t t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t; }
        printf("sbk: %s was byte-swapped (.v64); converted to big-endian\n", path);
    } else if (buf[0] == 0x40 && buf[1] == 0x12 && buf[2] == 0x37 && buf[3] == 0x80) {
        long i;
        for (i = 0; i + 3 < n; i += 4) {
            uint8_t t0 = buf[i], t1 = buf[i + 1];
            buf[i] = buf[i + 3]; buf[i + 1] = buf[i + 2];
            buf[i + 2] = t1;     buf[i + 3] = t0;
        }
        printf("sbk: %s was word-swapped (.n64); converted to big-endian\n", path);
    } else if (!(buf[0] == 0x80 && buf[1] == 0x37 && buf[2] == 0x12 && buf[3] == 0x40)) {
        fprintf(stderr, "sbk: %s is not an N64 ROM image in any byte order\n", path);
        return -1;
    }
    sbk_rom = buf;
    sbk_rom_size = (unsigned long)n;
    return 0;
}

void osCreatePiManager(OSPri pri, OSMesgQueue *cmdQ, OSMesg *cmdBuf, s32 cmdMsgCnt) {
    (void)pri;
    osCreateMesgQueue(cmdQ, cmdBuf, cmdMsgCnt);
}

OSPiHandle *osCartRomInit(void) {
    memset(&sbk_cart_handle, 0, sizeof(sbk_cart_handle));
    sbk_cart_handle.type = 0; /* cartridge */
    sbk_cart_handle.baseAddress = 0x10000000;
    return &sbk_cart_handle;
}

static s32 sbk_pi_copy(s32 direction, u32 devAddr, void *dramAddr, u32 size) {
    u32 off = devAddr & 0x0FFFFFFFu; /* strip the 0x10000000 cart base if present */
    if (direction != OS_READ) {
        return -1; /* writes to the cartridge are not a thing */
    }
    if (sbk_rom == NULL || off + size > sbk_rom_size) {
        fprintf(stderr, "sbk: PI DMA out of ROM range: 0x%08x + 0x%x\n", devAddr, size);
        memset(dramAddr, 0, size);
        return -1;
    }
    memcpy(dramAddr, sbk_rom + off, size);
    return 0;
}

extern int sbk_trace;   /* --trace: the first ROM DMAs */
unsigned sbk_stat_dma;

s32 osPiStartDma(OSIoMesg *mb, s32 priority, s32 direction, u32 devAddr, void *dramAddr, u32 size, OSMesgQueue *mq) {
    (void)priority;
    if (sbk_stat_dma++ < 12 && sbk_trace) {
        printf("sbk: dma rom 0x%06x -> %p (%u bytes)\n", devAddr, dramAddr, size);
    }
    sbk_pi_copy(direction, devAddr, dramAddr, size);
    if (mb != NULL) {
        mb->hdr.type = OS_MESG_TYPE_DMAREAD;
        mb->hdr.pri = (u8)priority;
        mb->hdr.retQueue = mq;
        mb->dramAddr = dramAddr;
        mb->devAddr = devAddr;
        mb->size = size;
        mb->piHandle = &sbk_cart_handle;
    }
    if (mq != NULL) {
        osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

s32 osEPiStartDma(OSPiHandle *pihandle, OSIoMesg *mb, s32 direction) {
    (void)pihandle;
    sbk_pi_copy(direction, mb->devAddr, mb->dramAddr, mb->size);
    if (mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

u32 osPiGetStatus(void) {
    return 0;
}

s32 osPiReadIo(u32 devAddr, u32 *data) {
    u32 v = 0;
    if (sbk_pi_copy(OS_READ, devAddr & ~3u, &v, 4) != 0) {
        return -1;
    }
    *data = v;
    return 0;
}

s32 osPiWriteIo(u32 devAddr, u32 data) {
    (void)devAddr;
    (void)data;
    return -1;
}
