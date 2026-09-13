/* See rom_scan.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "rom_scan.h"
#include "settings.h"

/* --- SHA-1 (RFC 3174), small and self-contained ------------------------- */

struct Sha1 {
    unsigned int h[5];
    unsigned long len;
    unsigned char buf[64];
    unsigned int n;
};

static unsigned int rol(unsigned int v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_block(struct Sha1 *s, const unsigned char *p) {
    unsigned int w[80], a, b, c, d, e, f, k, t;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
    }
    for (i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(struct Sha1 *s) {
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
    s->len = 0; s->n = 0;
}

static void sha1_update(struct Sha1 *s, const unsigned char *p, unsigned long n) {
    s->len += n;
    while (n > 0) {
        unsigned int take = 64 - s->n;
        if ((unsigned long)take > n) take = (unsigned int)n;
        memcpy(s->buf + s->n, p, take);
        s->n += take;
        p += take;
        n -= take;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}

static void sha1_final(struct Sha1 *s, char out[41]) {
    /* 64 bits of length, then the 0x80 and the zeroes, written straight into
     * the block rather than back through sha1_update() -- padding is not part
     * of the message length and must not be counted into it. */
    unsigned long bits = s->len * 8;
    static const char hex[] = "0123456789abcdef";
    int i;
    s->buf[s->n++] = 0x80;
    if (s->n > 56) {
        memset(s->buf + s->n, 0, 64 - s->n);
        sha1_block(s, s->buf);
        s->n = 0;
    }
    memset(s->buf + s->n, 0, 56 - s->n);
    for (i = 0; i < 8; i++) s->buf[63 - i] = (unsigned char)((bits >> (i * 8)) & 0xFF);
    sha1_block(s, s->buf);
    for (i = 0; i < 20; i++) {
        unsigned char v = (unsigned char)((s->h[i / 4] >> ((3 - (i % 4)) * 8)) & 0xFF);
        out[i * 2] = hex[v >> 4];
        out[i * 2 + 1] = hex[v & 15];
    }
    out[40] = '\0';
}

void sbk_sha1_hex(const unsigned char *data, unsigned long len, char out[41]) {
    struct Sha1 s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    sha1_final(&s, out);
}

/* --- byte order ---------------------------------------------------------- */

enum { ORD_Z64 = 0, ORD_V64 = 1, ORD_N64 = 2, ORD_UNKNOWN = -1 };

/* The first word of every N64 ROM is 0x80371240.  Which permutation of those
 * four bytes a file starts with names its byte order outright, so nothing here
 * is guessed from the extension: a .z64 that is really a .v64 still loads. */
static int detect_order(const unsigned char *b) {
    if (b[0] == 0x80 && b[1] == 0x37 && b[2] == 0x12 && b[3] == 0x40) return ORD_Z64;
    if (b[0] == 0x37 && b[1] == 0x80 && b[2] == 0x40 && b[3] == 0x12) return ORD_V64;
    if (b[0] == 0x40 && b[1] == 0x12 && b[2] == 0x37 && b[3] == 0x80) return ORD_N64;
    return ORD_UNKNOWN;
}

static void to_big_endian(unsigned char *b, unsigned long n, int order) {
    unsigned long i;
    if (order == ORD_V64) {
        for (i = 0; i + 1 < n; i += 2) {
            unsigned char t = b[i]; b[i] = b[i + 1]; b[i + 1] = t;
        }
    } else if (order == ORD_N64) {
        for (i = 0; i + 3 < n; i += 4) {
            unsigned char t0 = b[i], t1 = b[i + 1];
            b[i] = b[i + 3]; b[i + 1] = b[i + 2];
            b[i + 2] = t1;   b[i + 3] = t0;
        }
    }
}

static const char *order_name(int o) {
    return o == ORD_V64 ? "v64 (byte-swapped)" : (o == ORD_N64 ? "n64 (word-swapped)" : "z64");
}

/* --- what the ROM header says it is -------------------------------------- */

/* The two-letter cartridge id at 0x3C and the country code at 0x3E, read out
 * of a header that has already been put in big-endian order. */
struct RomId { char cart[3]; char country; };

static void read_id(const unsigned char *b, struct RomId *id) {
    id->cart[0] = (char)b[0x3C];
    id->cart[1] = (char)b[0x3D];
    id->cart[2] = '\0';
    id->country = (char)b[0x3E];
}

static const char *country_name(char c) {
    switch (c) {
        case 'E': return "USA";
        case 'J': return "Japanese";
        case 'P': return "European";
        case 'A': return "Japan/USA";
        case 'D': return "German";
        case 'F': return "French";
        case 'S': return "Spanish";
        case 'I': return "Italian";
        default:  return "unknown-region";
    }
}

/* --- the known-good dumps ------------------------------------------------ */

/* SHA-1 of the USA cartridge dump of each game, which is also exactly what
 * each decompilation's own matching build produces -- these were computed from
 * the repositories' build/snowboardkids.z64 and build/snowboardkids2.z64, so
 * "the ROM the port was built against" and "the retail USA cartridge" are the
 * same 8 and 16 MB of bytes. */
static const struct {
    const char *id;       /* sbk_game_at()->id */
    const char *cart;     /* the ROM header's own cartridge id */
    const char *sha1;
    unsigned long size;
} known[] = {
    { "sbk1", "SK", "1583bacc9046a360df8ea4d536942155247e154c", 8u << 20 },
    { "sbk2", "K2", "5ce896fd64276948bc2b8cccd8cd51c25a9f32aa", 16u << 20 },
};
#define KNOWN_N ((int)(sizeof(known) / sizeof(known[0])))

/* --- paths --------------------------------------------------------------- */

const char *sbk_rom_dir(void) {
    static char dir[1200];
    if (dir[0] == '\0') snprintf(dir, sizeof(dir), "%s/ROMs", sbk_settings_dir());
    return dir;
}

static void ensure_rom_dir(void) {
    struct stat st;
    if (stat(sbk_settings_dir(), &st) != 0) mkdir(sbk_settings_dir(), 0755);
    if (stat(sbk_rom_dir(), &st) != 0) mkdir(sbk_rom_dir(), 0755);
}

void sbk_rom_open_folder(void) {
    char cmd[1400];
    ensure_rom_dir();
    /* Leopard's open(1); the Finder brings the window up.  The path is one the
     * port built itself out of $HOME, not anything a file said. */
    snprintf(cmd, sizeof(cmd), "/usr/bin/open \"%s\" >/dev/null 2>&1 &", sbk_rom_dir());
    if (system(cmd) != 0) fprintf(stderr, "sbk: could not open %s\n", sbk_rom_dir());
}

/* --- the hash cache ------------------------------------------------------ */

/* An 8 MB SHA-1 is about a fifth of a second on the 1 GHz G4 and a 16 MB one
 * twice that, which is a visible pause before the launcher appears every
 * single time.  One line per file of "size mtime sha1 path" in the folder
 * turns that into a stat(). */
#define CACHE_MAX 32
static struct CacheRow {
    unsigned long size;
    long mtime;
    char sha1[41];
    char path[1024];
} cache[CACHE_MAX];
static int cache_n;
static int cache_dirty;

static const char *cache_path(void) {
    static char p[1300];
    if (p[0] == '\0') snprintf(p, sizeof(p), "%s/.rom-hashes", sbk_rom_dir());
    return p;
}

static void cache_load(void) {
    FILE *f = fopen(cache_path(), "r");
    char line[1200];
    cache_n = 0;
    cache_dirty = 0;
    if (f == NULL) return;
    while (cache_n < CACHE_MAX && fgets(line, sizeof(line), f) != NULL) {
        struct CacheRow *r = &cache[cache_n];
        if (sscanf(line, "%lu %ld %40s %1023[^\n]", &r->size, &r->mtime, r->sha1, r->path) == 4) cache_n++;
    }
    fclose(f);
}

static void cache_save(void) {
    FILE *f;
    int i;
    if (!cache_dirty) return;
    f = fopen(cache_path(), "w");
    if (f == NULL) return;
    for (i = 0; i < cache_n; i++) {
        fprintf(f, "%lu %ld %s %s\n", cache[i].size, cache[i].mtime, cache[i].sha1, cache[i].path);
    }
    fclose(f);
}

static const char *cache_get(const char *path, unsigned long size, long mtime) {
    int i;
    for (i = 0; i < cache_n; i++) {
        if (cache[i].size == size && cache[i].mtime == mtime && strcmp(cache[i].path, path) == 0) {
            return cache[i].sha1;
        }
    }
    return NULL;
}

static void cache_put(const char *path, unsigned long size, long mtime, const char *sha1) {
    struct CacheRow *r;
    int i;
    for (i = 0; i < cache_n; i++) {
        if (strcmp(cache[i].path, path) == 0) { r = &cache[i]; goto set; }
    }
    if (cache_n >= CACHE_MAX) return;
    r = &cache[cache_n++];
set:
    r->size = size;
    r->mtime = mtime;
    snprintf(r->sha1, sizeof(r->sha1), "%s", sha1);
    snprintf(r->path, sizeof(r->path), "%s", path);
    cache_dirty = 1;
}

/* --- the scan ------------------------------------------------------------ */

static struct SbkRomSlot slots[4];

const struct SbkRomSlot *sbk_rom_slot(int i) {
    if (i < 0 || i >= (int)(sizeof(slots) / sizeof(slots[0]))) return NULL;
    return &slots[i];
}

/* Read a file, put it in big-endian order, hash it and say which game it is.
 * Returns the index into `known`, or -1 if it is not either game at all. */
static int examine(const char *path, char *detail, size_t detail_n, int *order_out, int *ok_out) {
    FILE *f;
    struct stat st;
    unsigned char head[64];
    struct RomId id;
    int order, k;
    char sha1[41];
    const char *cached;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    if (st.st_size <= 0x1000 || st.st_size > (64L << 20)) return -1;
    f = fopen(path, "rb");
    if (f == NULL) return -1;
    if (fread(head, 1, sizeof(head), f) != sizeof(head)) { fclose(f); return -1; }
    order = detect_order(head);
    if (order == ORD_UNKNOWN) { fclose(f); return -1; }
    to_big_endian(head, sizeof(head), order);
    read_id(head, &id);

    for (k = 0; k < KNOWN_N; k++) if (strcmp(id.cart, known[k].cart) == 0) break;
    if (k == KNOWN_N) { fclose(f); return -1; }   /* some other N64 ROM */
    *order_out = order;

    if (id.country != 'E') {
        snprintf(detail, detail_n, "%s", country_name(id.country));
        *ok_out = 0;
        fclose(f);
        return k;
    }

    cached = cache_get(path, (unsigned long)st.st_size, (long)st.st_mtime);
    if (cached != NULL) {
        snprintf(sha1, sizeof(sha1), "%s", cached);
    } else {
        unsigned char *buf = malloc((size_t)st.st_size);
        if (buf == NULL) { fclose(f); return -1; }
        fseek(f, 0, SEEK_SET);
        if (fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
            free(buf);
            fclose(f);
            return -1;
        }
        to_big_endian(buf, (unsigned long)st.st_size, order);
        sbk_sha1_hex(buf, (unsigned long)st.st_size, sha1);
        free(buf);
        cache_put(path, (unsigned long)st.st_size, (long)st.st_mtime, sha1);
    }
    fclose(f);

    if (strcmp(sha1, known[k].sha1) == 0) {
        *ok_out = 1;
        if (order == ORD_Z64) snprintf(detail, detail_n, "USA");
        else snprintf(detail, detail_n, "USA, %s", order_name(order));
    } else {
        *ok_out = 0;
        snprintf(detail, detail_n, "damaged");
    }
    return k;
}

/* Better news replaces worse: a good dump beats a wrong region beats a damaged
 * file beats nothing, so one bad file in the folder never hides a good one. */
static int rank(int status) {
    switch (status) {
        case SBK_ROM_OK:           return 3;
        case SBK_ROM_WRONG_REGION: return 2;
        case SBK_ROM_DAMAGED:      return 1;
        default:                   return 0;
    }
}

static void offer(const char *path, int slot_i, int status, const char *detail, int order) {
    struct SbkRomSlot *s;
    if (slot_i < 0 || slot_i >= (int)(sizeof(slots) / sizeof(slots[0]))) return;
    s = &slots[slot_i];
    if (rank(status) <= rank(s->status)) return;
    s->status = status;
    s->byte_order = order;
    snprintf(s->path, sizeof(s->path), "%s", path);
    snprintf(s->detail, sizeof(s->detail), "%s", detail);
}

static void consider(const char *path) {
    char detail[128];
    int order = ORD_Z64, ok = 0;
    int k = examine(path, detail, sizeof(detail), &order, &ok);
    int gi;
    if (k < 0) return;
    gi = sbk_game_index(known[k].id);
    offer(path, gi, ok ? SBK_ROM_OK
                       : (strcmp(detail, "damaged") == 0 ? SBK_ROM_DAMAGED : SBK_ROM_WRONG_REGION),
          detail, order);
}

static int has_rom_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    if (dot == NULL) return 0;
    return strcmp(dot, ".z64") == 0 || strcmp(dot, ".n64") == 0 || strcmp(dot, ".v64") == 0 ||
           strcmp(dot, ".Z64") == 0 || strcmp(dot, ".N64") == 0 || strcmp(dot, ".V64") == 0;
}

/* .../Foo.app/Contents/MacOS/isle -> .../Foo.app/Contents/Resources */
static int bundle_resources(const char *argv0, char *out, size_t n) {
    char buf[1024];
    char *slash;
    int i;
    if (argv0 == NULL || argv0[0] == '\0') return 0;
    snprintf(buf, sizeof(buf), "%s", argv0);
    for (i = 0; i < 2; i++) {           /* isle, MacOS */
        slash = strrchr(buf, '/');
        if (slash == NULL) return 0;
        *slash = '\0';
    }
    snprintf(out, n, "%s/Resources", buf);
    return 1;
}

static char scan_argv0[1024];

void sbk_rom_rescan(void) { sbk_rom_scan(scan_argv0[0] != '\0' ? scan_argv0 : NULL); }

void sbk_rom_scan(const char *argv0) {
    DIR *d;
    struct dirent *e;
    char res[1100];

    if (argv0 != NULL && argv0 != scan_argv0) snprintf(scan_argv0, sizeof(scan_argv0), "%s", argv0);
    memset(slots, 0, sizeof(slots));
    ensure_rom_dir();
    cache_load();

    d = opendir(sbk_rom_dir());
    if (d != NULL) {
        while ((e = readdir(d)) != NULL) {
            char path[1300];
            if (e->d_name[0] == '.') continue;
            if (!has_rom_ext(e->d_name)) continue;
            snprintf(path, sizeof(path), "%s/%s", sbk_rom_dir(), e->d_name);
            consider(path);
        }
        closedir(d);
    }

    /* Last, so that anything the player put in the shared folder wins over the
     * copy the bundle was built with. */
    if (bundle_resources(argv0, res, sizeof(res))) {
        DIR *rd = opendir(res);
        if (rd != NULL) {
            while ((e = readdir(rd)) != NULL) {
                char path[1400];
                if (e->d_name[0] == '.' || !has_rom_ext(e->d_name)) continue;
                snprintf(path, sizeof(path), "%s/%s", res, e->d_name);
                consider(path);
            }
            closedir(rd);
        }
    }

    cache_save();

    {
        int i;
        for (i = 0; i < sbk_game_count(); i++) {
            const struct SbkGameEntry *g = sbk_game_at(i);
            printf("sbk: rom %s: %s%s%s\n", g->id, sbk_rom_status_text(i),
                   slots[i].status != SBK_ROM_MISSING ? " -- " : "",
                   slots[i].status != SBK_ROM_MISSING ? slots[i].path : "");
        }
    }
}

int sbk_rom_read_at(const struct SbkRomSlot *slot, unsigned long off,
                    unsigned long len, void *dst) {
    FILE *f;
    unsigned long align, start, span;
    unsigned char *tmp;
    if (slot == NULL || slot->status == SBK_ROM_MISSING || len == 0) return -1;
    /* A .v64 is swapped within 2-byte pairs and a .n64 within 4-byte words, so
     * the read has to start and end on that boundary before it is unswapped. */
    align = slot->byte_order == ORD_N64 ? 4 : (slot->byte_order == ORD_V64 ? 2 : 1);
    start = off - (off % align);
    span = len + (off - start);
    span += (align - (span % align)) % align;
    tmp = malloc(span);
    if (tmp == NULL) return -1;
    f = fopen(slot->path, "rb");
    if (f == NULL) { free(tmp); return -1; }
    if (fseek(f, (long)start, SEEK_SET) != 0 || fread(tmp, 1, span, f) != span) {
        fclose(f);
        free(tmp);
        return -1;
    }
    fclose(f);
    to_big_endian(tmp, span, slot->byte_order);
    memcpy(dst, tmp + (off - start), len);
    free(tmp);
    return 0;
}

const char *sbk_rom_status_text(int i) {
    const struct SbkRomSlot *s = sbk_rom_slot(i);
    static char line[256];
    if (s == NULL) return "put your Snowboard Kids cartridge dump in this folder";
    switch (s->status) {
        case SBK_ROM_OK:
            snprintf(line, sizeof(line), "ready (%s)", s->detail);
            return line;
        case SBK_ROM_WRONG_REGION:
            snprintf(line, sizeof(line), "this is the %s cartridge; the port needs the USA one", s->detail);
            return line;
        case SBK_ROM_DAMAGED:
            return "this file is damaged; the port needs a clean USA dump";
        default:
            return "put your Snowboard Kids cartridge dump in this folder";
    }
}
