/* Bring your own ROM.
 *
 * Both bundles share one folder for the player's cartridge dumps, next to the
 * Controller Pak and settings.txt:
 *
 *     ~/Library/Application Support/SnowboardKids/ROMs/
 *
 * Any filename, any of the three byte orders (.z64 big-endian, .v64
 * byte-swapped, .n64 little-endian word-swapped): the scan reads the first
 * four bytes to decide which, converts a copy to big-endian and identifies the
 * game by SHA-1 against the one known-good USA dump per game.  A file that is
 * the right game in the wrong region, or the right region damaged, is reported
 * as such in plain words rather than silently ignored.
 *
 * The bundle's own Contents/Resources ROM is still a valid location -- it is
 * scanned last, so a user's personal build in the shared folder wins. */
#ifndef SBK_ROM_SCAN_H
#define SBK_ROM_SCAN_H

enum {
    SBK_ROM_MISSING = 0,   /* no file for this game at all */
    SBK_ROM_OK,            /* SHA-1 matches the known-good USA dump */
    SBK_ROM_WRONG_REGION,  /* it is this game, from another region */
    SBK_ROM_DAMAGED        /* it says it is this game and the hash disagrees */
};

struct SbkRomSlot {
    int status;            /* SBK_ROM_* */
    char path[1024];       /* the file we would load */
    char detail[128];      /* region letter, or what is wrong, for the screen */
    int byte_order;        /* 0 = z64, 1 = v64, 2 = n64 (see rom_scan.c) */
};

/* The shared ROM folder, created on first use. */
const char *sbk_rom_dir(void);

/* Scan the folder (and, last, the bundle's Resources) and fill one slot per
 * registered game.  argv0 locates the bundle.  Cheap on a second run: results
 * are cached in the folder by path, size and mtime, so an 8 MB SHA-1 is paid
 * once per file rather than at every launch. */
void sbk_rom_scan(const char *argv0);

/* Scan again with the argv0 the first scan was given: the launcher does this
 * after the player has been to the ROM folder in the Finder. */
void sbk_rom_rescan(void);

/* The slot for a registered game index (see sbk_game_at). */
const struct SbkRomSlot *sbk_rom_slot(int game_index);

/* One line of plain English for a slot: what the launcher shows. */
const char *sbk_rom_status_text(int game_index);

/* Open the ROM folder in the Finder. */
void sbk_rom_open_folder(void);

/* Load a ROM image from `path` into a freshly malloc'd big-endian buffer,
 * converting from .v64 / .n64 if that is what it is.  Returns 0 on success and
 * sets sbk_rom / sbk_rom_size (os_pi.c). */
int sbk_rom_load(const char *path);

/* SHA-1 of a byte range, as 40 lower-case hex characters plus a NUL. */
void sbk_sha1_hex(const unsigned char *data, unsigned long len, char out[41]);

/* Read `len` bytes at ROM offset `off` out of the file in `slot`, converting
 * to big-endian on the way if the file is a .v64 or .n64.  This is how the
 * launcher gets at the *other* game's art without loading its whole 16 MB:
 * the offsets come from that game's own build map (see ui/ui_rom_art.c).
 * Returns 0 on success. */
int sbk_rom_read_at(const struct SbkRomSlot *slot, unsigned long off,
                    unsigned long len, void *dst);

#endif
