/* See ui_gl.h. */
#include <string.h>
#include <OpenGL/gl.h>
#include "ui_gl.h"
#include "ui_font.h"

const struct SbkColor SBK_UI_FG     = { 0.94f, 0.96f, 1.00f, 1.0f };
const struct SbkColor SBK_UI_DIM    = { 0.45f, 0.48f, 0.55f, 1.0f };
const struct SbkColor SBK_UI_SEL    = { 1.00f, 0.85f, 0.25f, 1.0f };
const struct SbkColor SBK_UI_ACCENT = { 0.35f, 0.70f, 1.00f, 1.0f };
const struct SbkColor SBK_UI_PANEL  = { 0.04f, 0.06f, 0.11f, 0.88f };
const struct SbkColor SBK_UI_SHADOW = { 0.00f, 0.00f, 0.00f, 0.70f };
const struct SbkColor SBK_UI_LINE   = { 0.20f, 0.28f, 0.42f, 1.0f };

static GLuint font_tex;
static GLuint logo_tex;
#include "ui_logo.h"
static int in_ui;

/* The launcher writes with the game's own sprite font when a ROM is present
 * (ui_rom_art.c pulls the sheet out of the cartridge dump and installs it
 * here) and with the port's generated 5x7 font when none is.  Both are the
 * same shape as far as the drawing goes -- a grid of fixed cells in one
 * texture -- so one descriptor covers them and sbk_ui_text() does not branch.
 *
 * The ROM font has no glyphs below 0x20, and the UI's three little arrow and
 * bullet pictures live at 1, 2 and 3, so those always come from the built-in
 * texture: a string that mixes the two swaps the binding mid-draw. */
struct UiFont {
    GLuint tex;
    int tex_w, tex_h;
    int cols;          /* cells across the texture */
    int cell;          /* cell size in texels (square) */
    int advance;       /* pen movement in texels */
    int first;         /* character code of cell 0 */
    int fold_lower;    /* 1 = 'a' draws 'A' (the games have no lowercase) */
    int tinted;        /* 1 = RGBA glyphs to modulate, 0 = an alpha mask */
};

static struct UiFont font_builtin = { 0, 128, 128, 16, 8, SBK_FONT_ADVANCE, 0, 0, 0 };
static int force_builtin;
static int force_builtin_fwd(void) { return force_builtin; }
static struct UiFont font_rom;
static int font_rom_ready;

static int force_builtin_fwd(void);

static struct UiFont *font_for(int code) {
    if (force_builtin_fwd()) return &font_builtin;
    if (font_rom_ready && code >= font_rom.first &&
        code - font_rom.first < font_rom.cols * (font_rom.tex_h / font_rom.cell)) {
        return &font_rom;
    }
    return &font_builtin;
}

static struct UiFont *font_body(void) { return font_rom_ready ? &font_rom : &font_builtin; }

static void font_upload(void) {
    /* 16 x 8 cells of 8x8 = 128x64, padded to a 128x128 POT texture (the
     * Radeon 9000 has no NPOT support). */
    static unsigned char px[128 * 128];
    int code, row, col;
    memset(px, 0, sizeof(px));
    for (code = 0; code < 128; code++) {
        int cx = (code % 16) * 8, cy = (code / 16) * 8;
        for (row = 0; row < 8; row++) {
            unsigned char bits = sbk_ui_font[code][row];
            for (col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) px[(cy + row) * 128 + cx + col] = 255;
            }
        }
    }
    glGenTextures(1, &font_tex);
    glBindTexture(GL_TEXTURE_2D, font_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 128, 128, 0, GL_ALPHA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    font_builtin.tex = font_tex;
}

void sbk_ui_font_set_rom(const unsigned char *rgba, int w, int h, int cols,
                         int cell, int advance, int first, int fold_lower) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* GL_NEAREST: these are 8x8 pixel glyphs and they are meant to look it */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    font_rom.tex = t;
    font_rom.tex_w = w;
    font_rom.tex_h = h;
    font_rom.cols = cols;
    font_rom.cell = cell;
    font_rom.advance = advance;
    font_rom.first = first;
    font_rom.fold_lower = fold_lower;
    font_rom.tinted = 1;
    font_rom_ready = 1;
}

int sbk_ui_font_is_rom(void) { return font_rom_ready; }

/* The launcher logo: uploaded once into a 512x512 texture (the 288-row image
 * in the top rows, the rest untouched), drawn with linear filtering. */
void sbk_ui_logo(int x, int y, int w, int h) {
    float v1 = (float)SBK_UI_LOGO_H / 512.0f;
    if (logo_tex == 0) {
        glGenTextures(1, &logo_tex);
        glBindTexture(GL_TEXTURE_2D, logo_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, SBK_UI_LOGO_W, SBK_UI_LOGO_H, GL_RGBA, GL_UNSIGNED_BYTE, sbk_ui_logo_rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, logo_tex);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2i(x, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2i(x + w, y);
    glTexCoord2f(1.0f, v1);   glVertex2i(x + w, y + h);
    glTexCoord2f(0.0f, v1);   glVertex2i(x, y + h);
    glEnd();
}

int sbk_ui_pick_scale(int win_h) {
    int s = win_h / 240;
    if (s < 1) s = 1;
    if (s > 6) s = 6;
    return s;
}

void sbk_ui_begin(int win_w, int win_h) {
    int u;
    if (in_ui) return;
    in_ui = 1;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    /* The game leaves up to six texture units enabled with a combiner chain
     * on them; the UI wants exactly one, modulating the font's alpha. */
    for (u = 5; u >= 0; u--) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    if (font_tex == 0) font_upload();
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, win_w, win_h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (GLdouble)win_w, (GLdouble)win_h, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
}

void sbk_ui_end(void) {
    if (!in_ui) return;
    in_ui = 0;
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopAttrib();
}

void sbk_ui_rect(int x, int y, int w, int h, struct SbkColor c) {
    if (w <= 0 || h <= 0) return;
    glDisable(GL_TEXTURE_2D);
    glColor4f(c.r, c.g, c.b, c.a);
    glBegin(GL_QUADS);
    glVertex2i(x, y);
    glVertex2i(x + w, y);
    glVertex2i(x + w, y + h);
    glVertex2i(x, y + h);
    glEnd();
}

void sbk_ui_border(int x, int y, int w, int h, int t, struct SbkColor c) {
    sbk_ui_rect(x, y, w, t, c);
    sbk_ui_rect(x, y + h - t, w, t, c);
    sbk_ui_rect(x, y + t, t, h - 2 * t, c);
    sbk_ui_rect(x + w - t, y + t, t, h - 2 * t, c);
}

int sbk_ui_text_w(const char *s, int scale) {
    return (int)strlen(s) * font_body()->advance * scale;
}

int sbk_ui_text_h(int scale) {
    return font_body()->cell * scale;
}

/* Force the port's own font for one string.  Used for file paths: the games'
 * sheets have no lowercase, and a path shouted in capitals reads like a
 * different path from the one the Finder shows. */
void sbk_ui_text_ascii(int x, int y, int scale, const char *s, struct SbkColor c) {
    force_builtin = 1;
    sbk_ui_text(x, y, scale, s, c);
    force_builtin = 0;
}

int sbk_ui_text_ascii_w(const char *s, int scale) {
    return (int)strlen(s) * font_builtin.advance * scale;
}

void sbk_ui_text(int x, int y, int scale, const char *s, struct SbkColor c) {
    const unsigned char *p = (const unsigned char *)s;
    int pen = x;
    struct UiFont *bound = NULL;
    if (font_tex == 0) font_upload();
    glEnable(GL_TEXTURE_2D);
    for (; *p != '\0'; p++) {
        int code = *p & 0x7F;
        struct UiFont *f;
        int idx, cx, cy, size;
        float u0, v0, u1, v1;
        if (code == ' ') { pen += (force_builtin ? font_builtin.advance : font_body()->advance) * scale; continue; }
        if (!force_builtin && font_rom_ready && font_rom.fold_lower && code >= 'a' && code <= 'z') code -= 32;
        f = font_for(code);
        idx = code - f->first;
        if (idx < 0) { pen += f->advance * scale; continue; }
        if (f != bound) {
            if (bound != NULL) glEnd();
            glBindTexture(GL_TEXTURE_2D, f->tex);
            glColor4f(c.r, c.g, c.b, c.a);
            glBegin(GL_QUADS);
            bound = f;
        }
        cx = (idx % f->cols) * f->cell;
        cy = (idx / f->cols) * f->cell;
        size = f->cell * scale;
        u0 = (float)cx / (float)f->tex_w;
        v0 = (float)cy / (float)f->tex_h;
        u1 = (float)(cx + f->cell) / (float)f->tex_w;
        v1 = (float)(cy + f->cell) / (float)f->tex_h;
        glTexCoord2f(u0, v0); glVertex2i(pen, y);
        glTexCoord2f(u1, v0); glVertex2i(pen + size, y);
        glTexCoord2f(u1, v1); glVertex2i(pen + size, y + size);
        glTexCoord2f(u0, v1); glVertex2i(pen, y + size);
        pen += f->advance * scale;
    }
    if (bound != NULL) glEnd();
    glDisable(GL_TEXTURE_2D);
}

void sbk_ui_text_shadow(int x, int y, int scale, const char *s, struct SbkColor c) {
    struct SbkColor sh = { 0.0f, 0.0f, 0.0f, 0.8f };
    sh.a *= c.a;
    sbk_ui_text(x + scale, y + scale, scale, s, sh);
    sbk_ui_text(x, y, scale, s, c);
}
