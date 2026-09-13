/* See ui_rom_art.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <OpenGL/gl.h>
#include "ui_rom_art.h"
#include "ui_gl.h"
#include "ui_font.h"
#include "rom_codec.h"
#include "ui_box.h"
#include "../rom_scan.h"
#include "../settings.h"

/* --- where each game keeps the two things the launcher wants -------------
 *
 * These are properties of the USA cartridge, and the launcher only ever reads
 * them out of a file whose SHA-1 it has already matched against that dump
 * (rom_scan.c), so there is no version of this table that can be pointed at
 * the wrong bytes.  Both were read off the games' own build maps:
 *
 *   sbk1  _2427D0   the ASCII font sheet   src/menu/renderer/menu_render_utils.c
 *         _5DCBE0   the title logo         src/menu/main_menu/main_menu_title_ui.c
 *   sbk2  FONT_DATA_TABLE                  src/text/font_assets.c
 *         titleLogo                        src/ui/title_ui_elements.c
 *
 * Both fonts are the same shape: a 64x64 CI4 sheet of 8x8 cells indexed by
 * `ascii - 0x20`, sixteen-colour palettes, palette 0 white.  Both logos are
 * the same shape too: a 10x8 grid of 32x32 CI8 tiles with one 256-colour
 * palette, differing only in which rows carry the picture -- which is why the
 * decoder below crops to what is actually drawn rather than being told. */

enum { CODEC_HUFFMAN = 0, CODEC_SNO = 1 };

struct GameArt {
    const char *id;
    int codec;
    unsigned long font_start, font_end, font_size;
    unsigned long font_tlut, font_tex;     /* offsets inside the decompressed blob */
    unsigned long logo_start, logo_end, logo_size;
};

static const struct GameArt art[] = {
    { "sbk1", CODEC_HUFFMAN,
      0x2427D0, 0x243270, 0x2218, 0x58, 0x198,
      0x5DCBE0, 0x5DFDD0, 0x5F14 },
    { "sbk2", CODEC_SNO,
      0x215D70, 0x216290, 0x0918, 0x18, 0x118,
      0x414CF0, 0x418520, 0x7B50 },
};
#define ART_N ((int)(sizeof(art) / sizeof(art[0])))

static const struct GameArt *art_for(int game_index) {
    const struct SbkGameEntry *g = sbk_game_at(game_index);
    int i;
    if (g == NULL) return NULL;
    for (i = 0; i < ART_N; i++) if (strcmp(art[i].id, g->id) == 0) return &art[i];
    return NULL;
}

/* Read one compressed asset out of the game's ROM file and decompress it.
 * Returns a malloc'd blob of exactly `size` bytes, or NULL. */
static unsigned char *load_asset(int game_index, const struct GameArt *a,
                                 unsigned long start, unsigned long end, unsigned long size) {
    const struct SbkRomSlot *slot = sbk_rom_slot(game_index);
    unsigned char *packed, *out;
    unsigned long got;
    if (slot == NULL || slot->status != SBK_ROM_OK || end <= start) return NULL;
    packed = malloc(end - start);
    if (packed == NULL) return NULL;
    if (sbk_rom_read_at(slot, start, end - start, packed) != 0) { free(packed); return NULL; }
    out = malloc(size);
    if (out == NULL) { free(packed); return NULL; }
    if (a->codec == CODEC_HUFFMAN) got = sbk_decompress_huffman(packed, end - start, out, size);
    else                           got = sbk_decompress_sno(packed, end - start, out, size);
    free(packed);
    if (got != size) {
        fprintf(stderr, "sbk: %s asset at 0x%06lx decompressed to %lu, expected %lu\n",
                a->id, start, got, size);
        free(out);
        return NULL;
    }
    return out;
}

/* --- N64 pixel formats --------------------------------------------------- */

static unsigned short be16(const unsigned char *p) {
    return (unsigned short)(((unsigned)p[0] << 8) | p[1]);
}

/* RGBA5551, which is what both games' palettes are. */
static void rgba16_to_rgba8(unsigned short v, unsigned char *out) {
    out[0] = (unsigned char)((((v >> 11) & 31) * 255) / 31);
    out[1] = (unsigned char)((((v >> 6) & 31) * 255) / 31);
    out[2] = (unsigned char)((((v >> 1) & 31) * 255) / 31);
    out[3] = (v & 1) ? 255 : 0;
}

/* --- the font ------------------------------------------------------------ */

static int font_installed;

/* 64x64 CI4 with the sixteen-colour palette `pal`, expanded to RGBA8.  The
 * palette is kept rather than flattened to a mask: these glyphs are white with
 * a black outline, and multiplying the whole thing by the UI's colour tints
 * the body while leaving the outline dark -- which is exactly how the games'
 * own menus look. */
static void font_expand(const unsigned char *blob, const struct GameArt *a,
                        int palette, unsigned char *rgba) {
    const unsigned char *tex = blob + a->font_tex;
    const unsigned char *tl = blob + a->font_tlut + (unsigned long)palette * 32;
    unsigned char lut[16][4];
    int i, y, x;
    for (i = 0; i < 16; i++) rgba16_to_rgba8(be16(tl + i * 2), lut[i]);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            unsigned char b = tex[(y * 64 + x) / 2];
            int idx = (x & 1) ? (b & 15) : (b >> 4);
            memcpy(rgba + (y * 64 + x) * 4, lut[idx], 4);
        }
    }
}

/* The decoded sheet is kept so the box fronts can write with the same
 * letterforms straight into their own pixel buffers. */
static unsigned char font_rgba[64 * 64 * 4];
static int font_have_pixels;

void sbk_ui_rom_art_init(void) {
    int order[4], n = 0, i;
    if (font_installed) return;
    /* the game this executable is first, then anything else present: whichever
     * cartridge the player owns is the one the launcher writes with */
    order[n++] = sbk_game_self_index();
    for (i = 0; i < sbk_game_count() && n < 4; i++) if (i != order[0]) order[n++] = i;

    for (i = 0; i < n; i++) {
        const struct GameArt *a = art_for(order[i]);
        unsigned char *blob;
        if (a == NULL) continue;
        blob = load_asset(order[i], a, a->font_start, a->font_end, a->font_size);
        if (blob == NULL) continue;
        font_expand(blob, a, 0, font_rgba);
        free(blob);
        font_have_pixels = 1;
        sbk_ui_font_set_rom(font_rgba, 64, 64, 8, 8, 8, 0x20, 1);
        font_installed = 1;
        printf("sbk: launcher font from %s's own sprite sheet\n", sbk_game_at(order[i])->title);
        return;
    }
    printf("sbk: no ROM yet; launcher uses the port's own font\n");
}

/* --- the title logo ------------------------------------------------------ */

/* A 10x8 grid of 32x32 CI8 tiles.  Both games use the same header, four bytes
 * per tile descriptor and a 1-based texture index, with 0 meaning "nothing is
 * drawn in this cell" -- so the composite is decoded whole and then cropped to
 * whatever the game actually put in it. */
static unsigned char *logo_decode(const unsigned char *blob, unsigned long size,
                                  int *out_w, int *out_h) {
    unsigned gw = be16(blob + 0x00), gh = be16(blob + 0x02);
    unsigned tmap = be16(blob + 0x0A), pal = be16(blob + 0x0C), img = be16(blob + 0x0E);
    const unsigned char *tl;
    unsigned char lut[256][4];
    unsigned char *full;
    int W, H, i, row, col;
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1;
    unsigned char *crop;
    int cw, ch, y;

    if (gw == 0 || gh == 0 || gw > 32 || gh > 32) return NULL;
    if (pal + 512 > size || tmap + gw * gh * 2 > size || img >= size) return NULL;
    W = (int)gw * 32;
    H = (int)gh * 32;
    tl = blob + pal;
    for (i = 0; i < 256; i++) rgba16_to_rgba8(be16(tl + i * 2), lut[i]);
    full = calloc((size_t)W * H, 4);
    if (full == NULL) return NULL;

    for (row = 0; row < (int)gh; row++) {
        for (col = 0; col < (int)gw; col++) {
            unsigned cell = be16(blob + tmap + (col + row * (int)gw) * 2);
            unsigned ti, base;
            int ty, tx;
            if (cell == 0) continue;
            ti = be16(blob + 0x10 + cell * 4);
            if (ti == 0) continue;
            base = img + (ti - 1) * 32 * 32;
            if (base + 32 * 32 > size) continue;
            for (ty = 0; ty < 32; ty++) {
                for (tx = 0; tx < 32; tx++) {
                    int px = col * 32 + tx, py = row * 32 + ty;
                    memcpy(full + ((size_t)py * W + px) * 4, lut[blob[base + ty * 32 + tx]], 4);
                }
            }
        }
    }

    for (y = 0; y < H; y++) {
        int x;
        for (x = 0; x < W; x++) {
            if (full[((size_t)y * W + x) * 4 + 3] != 0) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < x0 || y1 < y0) { free(full); return NULL; }
    cw = x1 - x0 + 1;
    ch = y1 - y0 + 1;
    crop = malloc((size_t)cw * ch * 4);
    if (crop == NULL) { free(full); return NULL; }
    for (y = 0; y < ch; y++) {
        memcpy(crop + (size_t)y * cw * 4, full + ((size_t)(y + y0) * W + x0) * 4, (size_t)cw * 4);
    }
    free(full);
    *out_w = cw;
    *out_h = ch;
    return crop;
}

/* --- building a box front ------------------------------------------------ */

static void put(unsigned char *dst, int w, int x, int y, int h,
                int r, int g, int b, int a) {
    unsigned char *p;
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    p = dst + ((size_t)y * w + x) * 4;
    if (a >= 255) { p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b; p[3] = 255; return; }
    p[0] = (unsigned char)((p[0] * (255 - a) + r * a) / 255);
    p[1] = (unsigned char)((p[1] * (255 - a) + g * a) / 255);
    p[2] = (unsigned char)((p[2] * (255 - a) + b * a) / 255);
    if (p[3] < a) p[3] = (unsigned char)a;
}

static void fill_rect(unsigned char *dst, int w, int h, int x, int y, int rw, int rh,
                      int r, int g, int b, int a) {
    int j, i;
    for (j = y; j < y + rh; j++) for (i = x; i < x + rw; i++) put(dst, w, i, j, h, r, g, b, a);
}

/* A box filter: the logos come in at 250-290 pixels wide and land at about
 * 200, so the samples are averaged rather than dropped. */
static void blit_scaled(unsigned char *dst, int dw, int dh, int dx, int dy, int tw, int th,
                        const unsigned char *src, int sw, int sh) {
    int j, i;
    for (j = 0; j < th; j++) {
        int sy0 = j * sh / th, sy1 = (j + 1) * sh / th;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (i = 0; i < tw; i++) {
            int sx0 = i * sw / tw, sx1 = (i + 1) * sw / tw;
            long ar = 0, ag = 0, ab = 0, aa = 0, n = 0;
            int y, x;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (y = sy0; y < sy1 && y < sh; y++) {
                for (x = sx0; x < sx1 && x < sw; x++) {
                    const unsigned char *s = src + ((size_t)y * sw + x) * 4;
                    ar += s[0] * s[3]; ag += s[1] * s[3]; ab += s[2] * s[3];
                    aa += s[3];
                    n++;
                }
            }
            if (n == 0 || aa == 0) continue;
            put(dst, dw, dx + i, dy + j, dh,
                (int)(ar / aa), (int)(ag / aa), (int)(ab / aa), (int)(aa / n));
        }
    }
}

/* Text into the buffer, with the game's own letterforms when a ROM gave us
 * some and the port's 5x7 font when it did not. */
static int art_text_w(const char *s, int scale) {
    return (int)strlen(s) * (font_have_pixels ? 8 : SBK_FONT_ADVANCE) * scale;
}

static void art_text(unsigned char *dst, int dw, int dh, int x, int y, int scale,
                     const char *s, int r, int g, int b) {
    const unsigned char *p = (const unsigned char *)s;
    int pen = x;
    for (; *p != '\0'; p++) {
        int ch = *p;
        int row, col;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (font_have_pixels) {
            int idx = ch - 0x20;
            if (idx >= 0 && idx < 64) {
                int cx = (idx & 7) * 8, cy = (idx >> 3) * 8;
                for (row = 0; row < 8; row++) {
                    for (col = 0; col < 8; col++) {
                        const unsigned char *sp = font_rgba + (((cy + row) * 64) + cx + col) * 4;
                        int sy, sx;
                        if (sp[3] == 0) continue;
                        for (sy = 0; sy < scale; sy++) for (sx = 0; sx < scale; sx++) {
                            put(dst, dw, pen + col * scale + sx, y + row * scale + sy, dh,
                                sp[0] * r / 255, sp[1] * g / 255, sp[2] * b / 255, sp[3]);
                        }
                    }
                }
            }
            pen += 8 * scale;
        } else {
            for (row = 0; row < 8; row++) {
                unsigned char bits = sbk_ui_font[ch & 0x7F][row];
                for (col = 0; col < 8; col++) {
                    int sy, sx;
                    if (!(bits & (0x80 >> col))) continue;
                    for (sy = 0; sy < scale; sy++) for (sx = 0; sx < scale; sx++) {
                        put(dst, dw, pen + col * scale + sx, y + row * scale + sy, dh, r, g, b, 255);
                    }
                }
            }
            pen += SBK_FONT_ADVANCE * scale;
        }
    }
}

/* The game's colour, matching ui_scene.c's plain faces -- except that a game
 * with no cartridge behind it gets a grey front to go with its grey box, so
 * "there is nothing here yet" is visible from across the room. */
static void front_colours(int game, int *top, int *bot) {
    const struct SbkRomSlot *slot = sbk_rom_slot(game);
    if (slot == NULL || slot->status != SBK_ROM_OK) {
        top[0] = top[1] = top[2] = 0x7E;
        bot[0] = bot[1] = bot[2] = 0x3C;
        return;
    }
    if (game == 1) { top[0] = 0xE0; top[1] = 0x78; top[2] = 0x1A;
                     bot[0] = 0x7A; bot[1] = 0x25; bot[2] = 0x06; }
    else           { top[0] = 0x2E; top[1] = 0x76; top[2] = 0xC8;
                     bot[0] = 0x0C; bot[1] = 0x25; bot[2] = 0x5A; }
}

static unsigned int upload_front(const unsigned char *rgba) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SBK_BOX_FRONT_TEX_W, SBK_BOX_FRONT_TEX_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

unsigned int sbk_ui_rom_art_box_front(int game) {
    const int W = SBK_BOX_FRONT_TEX_W, H = SBK_BOX_FRONT_TEX_H;
    const int CW = SBK_BOX_FRONT_W, CH = SBK_BOX_FRONT_H;
    const struct GameArt *a = art_for(game);
    unsigned char *px;
    unsigned char *logo = NULL;
    int lw = 0, lh = 0;
    int top[3], bot[3];
    int band_h = CH / 8;
    unsigned int tex;
    int y;

    px = calloc((size_t)W * H, 4);
    if (px == NULL) return 0;
    front_colours(game, top, bot);

    /* The sky the box art sits on: the same gradient idea as the launcher's
     * own sky, in the game's colour. */
    for (y = 0; y < CH; y++) {
        int t = y * 255 / (CH - 1);
        int r = top[0] + (bot[0] - top[0]) * t / 255;
        int g = top[1] + (bot[1] - top[1]) * t / 255;
        int b = top[2] + (bot[2] - top[2]) * t / 255;
        fill_rect(px, W, H, 0, y, CW, 1, r, g, b, 255);
    }
    /* a snowfield across the lower third, so the front is not a flat wash */
    for (y = CH * 74 / 100; y < CH - band_h; y++) {
        int t = (y - CH * 74 / 100) * 255 / (CH - band_h - CH * 74 / 100 + 1);
        fill_rect(px, W, H, 0, y, CW, 1, 255, 255, 255, 25 + t * 160 / 255);
    }

    /* A picture the user drew wins over the game's own logo: it is already
     * the size of the front, so it is the front (tools/gen_box.py). */
    {
        const unsigned char *drawn = NULL;
        const struct SbkGameEntry *g = sbk_game_at(game);
#ifdef SBK_BOX_HAVE_SBK1
        if (g != NULL && strcmp(g->id, "sbk1") == 0) drawn = sbk_box_sbk1_rgba;
#endif
#ifdef SBK_BOX_HAVE_SBK2
        if (g != NULL && strcmp(g->id, "sbk2") == 0) drawn = sbk_box_sbk2_rgba;
#endif
        if (drawn != NULL) {
            int j;
            for (j = 0; j < CH; j++) {
                memcpy(px + (size_t)j * W * 4, drawn + (size_t)j * CW * 4, (size_t)CW * 4);
            }
            tex = upload_front(px);
            free(px);
            return tex;
        }
        (void)g;
    }

    if (a != NULL) {
        unsigned char *blob = load_asset(game, a, a->logo_start, a->logo_end, a->logo_size);
        if (blob != NULL) {
            logo = logo_decode(blob, a->logo_size, &lw, &lh);
            free(blob);
        }
    }

    if (logo != NULL) {
        int tw = CW - 28;
        int th = lh * tw / lw;
        int max_h = CH / 2;
        if (th > max_h) { th = max_h; tw = lw * th / lh; }
        blit_scaled(px, W, H, (CW - tw) / 2, CH / 6, tw, th, logo, lw, lh);
        free(logo);
    } else {
        /* No ROM: the front says so rather than pretending to be art. */
        const struct SbkGameEntry *g = sbk_game_at(game);
        const char *name = g != NULL ? g->title : "SNOWBOARD KIDS";
        int s = 3;
        while (s > 1 && art_text_w(name, s) > CW - 20) s--;
        art_text(px, W, H, (CW - art_text_w(name, s)) / 2, CH / 3, s, name, 235, 240, 250);
        s = 2;
        art_text(px, W, H, (CW - art_text_w("NO CARTRIDGE", s)) / 2, CH / 3 + 40, s,
                 "NO CARTRIDGE", 200, 205, 215);
    }

    /* the band along the bottom, the one thing on the box that is the port's */
    fill_rect(px, W, H, 0, CH - band_h, CW, band_h, 12, 16, 26, 235);
    fill_rect(px, W, H, 0, CH - band_h, CW, 2, 240, 200, 60, 255);
    {
        int s = 2;
        while (s > 1 && art_text_w("POWERPC EDITION", s) > CW - 16) s--;
        art_text(px, W, H, (CW - art_text_w("POWERPC EDITION", s)) / 2,
                 CH - band_h + (band_h - 8 * s) / 2, s, "POWERPC EDITION", 245, 225, 150);
    }

    /* a printed edge so the front reads as a card, not as a screen */
    fill_rect(px, W, H, 0, 0, CW, 2, 255, 255, 255, 90);
    fill_rect(px, W, H, 0, 0, 2, CH, 255, 255, 255, 70);
    fill_rect(px, W, H, CW - 2, 0, 2, CH, 0, 0, 0, 70);
    fill_rect(px, W, H, 0, CH - 2, CW, 2, 0, 0, 0, 90);

    tex = upload_front(px);
    free(px);
    return tex;
}
