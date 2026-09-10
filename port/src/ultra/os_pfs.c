/* Controller Pak on port 1, backed by a 32 KB image file.
 *
 * libultra's own pak file system (src/ultra/io/pfs*.c, contpfs.c) is compiled
 * into the port unchanged; it sees the pak through 32-byte block reads and
 * writes that on hardware go over the PIF. This file replaces that bottom
 * layer: __osContRamRead/__osContRamWrite hit the image (write-through), the
 * status probes report the pak present, and the SI access lock is a no-op.
 *
 * The image is the raw .mpk layout emulators use, so a save can move between
 * the port and an emulator. A missing file starts as zeros: libultra's own
 * repair path then writes a fresh ID block and osPfsChecker rebuilds the
 * inode tables, exactly as it would for a corrupt pak on hardware. */
#include "ultra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <PRinternal/controller.h>

#define PAK_BYTES 32768
#define PAK_BLOCKS (PAK_BYTES / BLOCKSIZE)

static u8 pak_image[PAK_BYTES];
static FILE *pak_file;
static int pak_present;
static char pak_path_buf[1024];

/* __osPfsPifRam and __osMaxControllers are pinned N64 globals (pins.o); only the
 * replaced PIF-level code used them. */

void __osSiGetAccess(void) {}
void __osSiRelAccess(void) {}

int sbk_pak_present(void) {
    return pak_present;
}

static const char *default_pak_path(void) {
    const char *home = getenv("HOME");
    if (home == NULL) home = ".";
    snprintf(pak_path_buf, sizeof(pak_path_buf), "%s/Library/Application Support/SnowboardKids", home);
    mkdir(pak_path_buf, 0755); /* the parent exists on every Mac; ignore failure */
    snprintf(pak_path_buf, sizeof(pak_path_buf), "%s/Library/Application Support/SnowboardKids/controller-pak-1.mpk", home);
    return pak_path_buf;
}

int sbk_pak_open(const char *path) {
    size_t n;
    if (path == NULL) path = default_pak_path();
    pak_file = fopen(path, "r+b");
    if (pak_file == NULL) {
        pak_file = fopen(path, "w+b");
        if (pak_file == NULL) {
            fprintf(stderr, "sbk: cannot create Controller Pak image %s; no pak\n", path);
            return -1;
        }
        memset(pak_image, 0, sizeof(pak_image));
        fwrite(pak_image, 1, sizeof(pak_image), pak_file);
        fflush(pak_file);
        pak_present = 1;
        /* Format it like a factory-fresh pak with libultra's own code: the ID
         * repair writes the four ID blocks, the checker builds empty inode
         * tables. Without this the game reports "DATA MAY BE ERASED". */
        {
            OSPfs tmp;
            s32 r1, r2;
            memset(&tmp, 0, sizeof(tmp));
            r1 = osPfsRepairId(&tmp);
            r2 = osPfsChecker(&tmp);
            printf("sbk: new Controller Pak image %s (format: repair %d, checker %d)\n", path, (int)r1, (int)r2);
        }
    } else {
        n = fread(pak_image, 1, sizeof(pak_image), pak_file);
        if (n < sizeof(pak_image)) {
            memset(pak_image + n, 0, sizeof(pak_image) - n);
        }
        printf("sbk: Controller Pak image %s\n", path);
    }
    pak_present = 1;
    return 0;
}

/* address is in 32-byte blocks. 0x400 and up are the bank-select / rumble
 * detect area: not backed by memory, reads return the block written last. */
static u8 detect_block[BLOCKSIZE];

s32 __osContRamRead(OSMesgQueue *mq, int channel, u16 address, u8 *buffer) {
    (void)mq;
    if (channel != 0 || !pak_present) return PFS_ERR_NOPACK;
    if (address >= PAK_BLOCKS) {
        memcpy(buffer, detect_block, BLOCKSIZE);
        return 0;
    }
    memcpy(buffer, pak_image + (size_t)address * BLOCKSIZE, BLOCKSIZE);
    return 0;
}

s32 __osContRamWrite(OSMesgQueue *mq, int channel, u16 address, u8 *buffer, int force) {
    (void)mq; (void)force;
    if (channel != 0 || !pak_present) return PFS_ERR_NOPACK;
    if (address >= PAK_BLOCKS) {
        memcpy(detect_block, buffer, BLOCKSIZE);
        return 0;
    }
    memcpy(pak_image + (size_t)address * BLOCKSIZE, buffer, BLOCKSIZE);
    if (pak_file != NULL) {
        fseek(pak_file, (long)address * BLOCKSIZE, SEEK_SET);
        fwrite(buffer, 1, BLOCKSIZE, pak_file);
        fflush(pak_file);
    }
    return 0;
}

s32 __osPfsGetStatus(OSMesgQueue *queue, int channel) {
    (void)queue;
    return (channel == 0 && pak_present) ? 0 : PFS_ERR_NOPACK;
}

s32 osPfsIsPlug(OSMesgQueue *mq, u8 *pattern) {
    (void)mq;
    *pattern = pak_present ? 1 : 0;
    return 0;
}
