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
static int in_ui;

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
    return (int)strlen(s) * SBK_FONT_ADVANCE * scale;
}

int sbk_ui_text_h(int scale) {
    return SBK_FONT_CELL * scale;
}

void sbk_ui_text(int x, int y, int scale, const char *s, struct SbkColor c) {
    const unsigned char *p = (const unsigned char *)s;
    int pen = x;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, font_tex);
    glColor4f(c.r, c.g, c.b, c.a);
    glBegin(GL_QUADS);
    for (; *p != '\0'; p++, pen += SBK_FONT_ADVANCE * scale) {
        int code = *p & 0x7F;
        float u0, v0, u1, v1;
        int cx = (code % 16) * 8, cy = (code / 16) * 8;
        if (code == ' ') continue;
        u0 = (float)cx / 128.0f;
        v0 = (float)cy / 128.0f;
        u1 = (float)(cx + 8) / 128.0f;
        v1 = (float)(cy + 8) / 128.0f;
        glTexCoord2f(u0, v0); glVertex2i(pen, y);
        glTexCoord2f(u1, v0); glVertex2i(pen + 8 * scale, y);
        glTexCoord2f(u1, v1); glVertex2i(pen + 8 * scale, y + 8 * scale);
        glTexCoord2f(u0, v1); glVertex2i(pen, y + 8 * scale);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

void sbk_ui_text_shadow(int x, int y, int scale, const char *s, struct SbkColor c) {
    struct SbkColor sh = { 0.0f, 0.0f, 0.0f, 0.8f };
    sh.a *= c.a;
    sbk_ui_text(x + scale, y + scale, scale, s, sh);
    sbk_ui_text(x, y, scale, s, c);
}
