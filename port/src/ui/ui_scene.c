/* See ui_scene.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <OpenGL/gl.h>
#include "ui_scene.h"
#include "ui_gl.h"
#include "ui_rom_art.h"
#include "../settings.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- the soft blob every cloud and snowflake is made of ------------------ */

/* One 64x64 GL_ALPHA texture with a smooth radial falloff.  A cloud is three
 * or four of these at different sizes and a snowflake is one small one, which
 * is the whole particle budget: no sprite sheet to find in the ROM, nothing to
 * decode, and it scales to any window. */
static GLuint blob_tex;

static void blob_upload(void) {
    unsigned char px[64 * 64];
    int y, x;
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            float dx = ((float)x + 0.5f) / 32.0f - 1.0f;
            float dy = ((float)y + 0.5f) / 32.0f - 1.0f;
            float d = sqrtf(dx * dx + dy * dy);
            float a = 1.0f - d;
            if (a < 0.0f) a = 0.0f;
            a = a * a * (3.0f - 2.0f * a);      /* smoothstep: no hard edge */
            px[y * 64 + x] = (unsigned char)(a * 255.0f);
        }
    }
    glGenTextures(1, &blob_tex);
    glBindTexture(GL_TEXTURE_2D, blob_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 64, 64, 0, GL_ALPHA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void blob_quad(float cx, float cy, float r, float rr,
                      float cr, float cg, float cb, float ca) {
    glColor4f(cr, cg, cb, ca);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(cx - r, cy - rr);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(cx + r, cy - rr);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(cx + r, cy + rr);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(cx - r, cy + rr);
}

/* --- clouds and snow ----------------------------------------------------- */

#define CLOUDS 7
#define FLAKES 110

struct Cloud { float x, y, s, a; };
struct Flake { float x, y, s, vy, phase, sway; };

static struct Cloud clouds[CLOUDS];
static struct Flake flakes[FLAKES];
static int seeded;
static float scene_t;

/* A tiny fixed-seed generator: the sky looks the same on every launch, which
 * is what a title screen should do, and it costs no rand() state. */
static unsigned int rng = 0x5B1D2A7Fu;
static float frand(void) {
    rng = rng * 1103515245u + 12345u;
    return (float)((rng >> 8) & 0xFFFFFF) / (float)0x1000000;
}

static void seed(void) {
    int i;
    rng = 0x5B1D2A7Fu;
    for (i = 0; i < CLOUDS; i++) {
        clouds[i].x = frand();
        clouds[i].y = 0.06f + frand() * 0.30f;
        clouds[i].s = 0.10f + frand() * 0.13f;
        clouds[i].a = 0.55f + frand() * 0.35f;
    }
    for (i = 0; i < FLAKES; i++) {
        flakes[i].x = frand();
        flakes[i].y = frand();
        flakes[i].s = 0.0018f + frand() * 0.0042f;
        flakes[i].vy = 0.020f + frand() * 0.045f;
        flakes[i].phase = frand() * 6.2832f;
        flakes[i].sway = 0.004f + frand() * 0.012f;
    }
    seeded = 1;
}

void sbk_ui_scene_step(float dt) {
    int i;
    if (!seeded) seed();
    if (dt > 0.25f) dt = 0.25f;          /* a stall must not teleport the sky */
    scene_t += dt;
    for (i = 0; i < CLOUDS; i++) {
        /* the bigger a cloud is the nearer it reads, so it drifts faster */
        clouds[i].x += dt * 0.0045f * (0.5f + clouds[i].s * 3.0f);
        if (clouds[i].x > 1.35f) clouds[i].x -= 1.7f;
    }
    for (i = 0; i < FLAKES; i++) {
        flakes[i].y += dt * flakes[i].vy;
        if (flakes[i].y > 1.05f) { flakes[i].y -= 1.1f; flakes[i].x = frand(); }
    }
}

float sbk_ui_scene_time(void) { return scene_t; }

/* --- the sky ------------------------------------------------------------- */

/* Sunny Mountain's own air: a deep blue overhead washing out to a pale
 * horizon, which is what every daytime course in both games sits under. */
static const float SKY_TOP[3]  = { 0.11f, 0.40f, 0.78f };
static const float SKY_MID[3]  = { 0.35f, 0.66f, 0.93f };
static const float SKY_LOW[3]  = { 0.78f, 0.90f, 0.99f };

static void sky_gradient(int win_w, int win_h, float a) {
    float h = (float)win_h;
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor4f(SKY_TOP[0], SKY_TOP[1], SKY_TOP[2], a);
    glVertex2f(0.0f, 0.0f);
    glVertex2f((float)win_w, 0.0f);
    glColor4f(SKY_MID[0], SKY_MID[1], SKY_MID[2], a);
    glVertex2f((float)win_w, h * 0.55f);
    glVertex2f(0.0f, h * 0.55f);

    glColor4f(SKY_MID[0], SKY_MID[1], SKY_MID[2], a);
    glVertex2f(0.0f, h * 0.55f);
    glVertex2f((float)win_w, h * 0.55f);
    glColor4f(SKY_LOW[0], SKY_LOW[1], SKY_LOW[2], a);
    glVertex2f((float)win_w, h);
    glVertex2f(0.0f, h);
    glEnd();
}

/* The screens that put a panel up cross-fade the boxes back into the sky
 * rather than snapping them away: the same gradient, painted over them at
 * rising alpha, so what is left behind the panel is the sky and not a ghost
 * of a box showing through it. */
void sbk_ui_scene_sky_veil(int win_w, int win_h, float a) {
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    sky_gradient(win_w, win_h, a);
}

void sbk_ui_scene_sky(int win_w, int win_h) {
    if (!seeded) seed();
    if (blob_tex == 0) blob_upload();
    sky_gradient(win_w, win_h, 1.0f);
}

/* The clouds and the snowfall, drawn after the boxes so a flake can pass in
 * front of one -- and so the veil that fades the boxes away leaves the weather
 * alone. */
void sbk_ui_scene_weather(int win_w, int win_h) {
    int i;
    float h = (float)win_h;
    if (!seeded) seed();
    if (blob_tex == 0) blob_upload();

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, blob_tex);
    glBegin(GL_QUADS);
    for (i = 0; i < CLOUDS; i++) {
        float cx = clouds[i].x * (float)win_w;
        float cy = clouds[i].y * h;
        float r = clouds[i].s * (float)win_w;
        /* four overlapping blobs, flattened: a cloud, not a ball of fog */
        blob_quad(cx,              cy,             r * 0.75f, r * 0.34f, 1,1,1, clouds[i].a);
        blob_quad(cx - r * 0.45f,  cy + r * 0.07f, r * 0.50f, r * 0.24f, 1,1,1, clouds[i].a * 0.9f);
        blob_quad(cx + r * 0.42f,  cy + r * 0.05f, r * 0.55f, r * 0.26f, 1,1,1, clouds[i].a * 0.9f);
        blob_quad(cx + r * 0.08f,  cy - r * 0.16f, r * 0.42f, r * 0.24f, 1,1,1, clouds[i].a * 0.8f);
    }
    for (i = 0; i < FLAKES; i++) {
        float r = flakes[i].s * (float)win_w;
        float x = (flakes[i].x + flakes[i].sway * sinf(scene_t * 0.9f + flakes[i].phase)) * (float)win_w;
        float y = flakes[i].y * h;
        blob_quad(x, y, r, r, 1.0f, 1.0f, 1.0f, 0.85f);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

/* --- the boxes ----------------------------------------------------------- */

/* A real N64 box is about 1 : 1.4 : 0.15.  Half-extents, so the box is one
 * unit wide. */
#define BOX_HW 0.50f
#define BOX_HH 0.70f
#define BOX_HD 0.075f
#define FRONT_V ((float)SBK_BOX_FRONT_H / (float)SBK_BOX_FRONT_TEX_H)

static GLuint front_tex[4];
static int front_built[4];

void sbk_ui_scene_invalidate(void) {
    int i;
    for (i = 0; i < 4; i++) {
        if (front_tex[i] != 0) glDeleteTextures(1, &front_tex[i]);
        front_tex[i] = 0;
        front_built[i] = 0;
    }
}

static GLuint front_for(int game) {
    if (!front_built[game]) {
        front_built[game] = 1;
        front_tex[game] = sbk_ui_rom_art_box_front(game);
    }
    return front_tex[game];
}

/* The colour a box's plain faces and its unlit fallback are painted in: the
 * first game's snow-blue, the sequel's warmer orange, which is how the two
 * boxes read apart at a glance even in the grey "no cartridge" state. */
static void game_colour(int game, float *r, float *g, float *b) {
    if (game == 1) { *r = 0.88f; *g = 0.42f; *b = 0.13f; }
    else           { *r = 0.13f; *g = 0.44f; *b = 0.82f; }
}

static void quad(float x0,float y0,float z0, float x1,float y1,float z1,
                 float x2,float y2,float z2, float x3,float y3,float z3,
                 float nx,float ny,float nz, int textured) {
    glNormal3f(nx, ny, nz);
    if (textured) { glTexCoord2f(0.0f, 0.0f); glVertex3f(x0,y0,z0);
                    glTexCoord2f(1.0f, 0.0f); glVertex3f(x1,y1,z1);
                    glTexCoord2f(1.0f, 1.0f); glVertex3f(x2,y2,z2);
                    glTexCoord2f(0.0f, 1.0f); glVertex3f(x3,y3,z3); }
    else          { glVertex3f(x0,y0,z0); glVertex3f(x1,y1,z1);
                    glVertex3f(x2,y2,z2); glVertex3f(x3,y3,z3); }
}

/* The five plain faces: back, spine (left), open edge (right), top, bottom. */
static void box_shell(float r, float g, float b) {
    const float W = BOX_HW, H = BOX_HH, D = BOX_HD;
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor4f(r * 0.72f, g * 0.72f, b * 0.72f, 1.0f);          /* back */
    quad(+W,-H,-D, -W,-H,-D, -W,+H,-D, +W,+H,-D,  0,0,-1, 0);
    glColor4f(r, g, b, 1.0f);                                   /* spine */
    quad(-W,-H,-D, -W,-H,+D, -W,+H,+D, -W,+H,-D, -1,0,0, 0);
    glColor4f(r * 0.85f, g * 0.85f, b * 0.85f, 1.0f);           /* open edge */
    quad(+W,-H,+D, +W,-H,-D, +W,+H,-D, +W,+H,+D,  1,0,0, 0);
    glColor4f(r * 1.08f, g * 1.08f, b * 1.08f, 1.0f);           /* top */
    quad(-W,+H,-D, +W,+H,-D, +W,+H,+D, -W,+H,+D,  0,1,0, 0);
    glColor4f(r * 0.55f, g * 0.55f, b * 0.55f, 1.0f);           /* bottom */
    quad(-W,-H,+D, +W,-H,+D, +W,-H,-D, -W,-H,-D,  0,-1,0, 0);
    glEnd();
}

static void box_front(int game, int present, float r, float g, float b, float grey) {
    const float W = BOX_HW, H = BOX_HH, D = BOX_HD;
    GLuint t = present ? front_for(game) : 0;
    if (t != 0) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glColor4f(grey, grey, grey, 1.0f);
        glBegin(GL_QUADS);
        /* the front picture lives in the top SBK_BOX_FRONT_H rows of a
         * power-of-two texture (no NPOT on a Radeon 9000), so v stops there */
        glNormal3f(0.0f, 0.0f, 1.0f);
        glTexCoord2f(0.0f, FRONT_V); glVertex3f(-W,-H,+D);
        glTexCoord2f(1.0f, FRONT_V); glVertex3f(+W,-H,+D);
        glTexCoord2f(1.0f, 0.0f);    glVertex3f(+W,+H,+D);
        glTexCoord2f(0.0f, 0.0f);    glVertex3f(-W,+H,+D);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    } else {
        glDisable(GL_TEXTURE_2D);
        glColor4f(r, g, b, 1.0f);
        glBegin(GL_QUADS);
        quad(-W,-H,+D, +W,-H,+D, +W,+H,+D, -W,+H,+D, 0,0,1, 0);
        glEnd();
    }
}

/* Where a point in the box's own space lands on the screen, so the 2D pass can
 * put a name under a box without repeating the maths: the modelview and the
 * projection are asked for rather than rebuilt. */
static void project(float x, float y, float z, const double *mv, const double *pr,
                    const int *vp, int *sx, int *sy) {
    double e[4], c[4];
    int i;
    for (i = 0; i < 4; i++) {
        e[i] = mv[i] * x + mv[4 + i] * y + mv[8 + i] * z + mv[12 + i];
    }
    for (i = 0; i < 4; i++) {
        c[i] = pr[i] * e[0] + pr[4 + i] * e[1] + pr[8 + i] * e[2] + pr[12 + i] * e[3];
    }
    if (c[3] == 0.0) c[3] = 1.0;
    *sx = (int)(vp[0] + vp[2] * (c[0] / c[3] + 1.0) * 0.5);
    /* the UI's own y runs down the screen, GL's runs up */
    *sy = (int)(vp[1] + vp[3] * (1.0 - (c[1] / c[3] + 1.0) * 0.5));
}

void sbk_ui_scene_boxes(int win_w, int win_h, int count, int focus,
                        const int *present, float blend,
                        struct SbkBoxPlace *place) {
    const GLfloat lpos[4] = { -0.45f, 0.80f, 0.95f, 0.0f };   /* over the left shoulder */
    const GLfloat ldif[4] = { 1.00f, 0.98f, 0.94f, 1.0f };
    const GLfloat lamb[4] = { 0.42f, 0.47f, 0.56f, 1.0f };     /* bounced off the snow */
    double mv[16], pr[16];
    int vp[4];
    float aspect = (float)win_w / (float)(win_h > 0 ? win_h : 1);
    float near_z = 0.55f, far_z = 14.0f;
    float top = near_z * 0.46f, right = top * aspect;
    int i;

    if (!seeded) seed();
    if (blob_tex == 0) blob_upload();

    vp[0] = 0; vp[1] = 0; vp[2] = win_w; vp[3] = win_h;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glFrustum(-right, right, -top, top, near_z, far_z);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    /* The camera: a little above the boxes, looking slightly down at them.
     * `blend` pulls it back and lifts the boxes out of the way of a panel. */
    glTranslatef(0.0f, -0.10f + 0.42f * (1.0f - blend), -(3.05f + 1.45f * (1.0f - blend)));
    glRotatef(9.0f, 1.0f, 0.0f, 0.0f);

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    glGetDoublev(GL_MODELVIEW_MATRIX, mv);
    glGetDoublev(GL_PROJECTION_MATRIX, pr);

    /* --- the shelf: a snow plane fading into the horizon --- */
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor4f(0.98f, 0.99f, 1.00f, 1.0f);
    glVertex3f(-9.0f, -BOX_HH, 1.6f);
    glVertex3f(+9.0f, -BOX_HH, 1.6f);
    glColor4f(0.74f, 0.85f, 0.96f, 1.0f);
    glVertex3f(+9.0f, -BOX_HH, -7.0f);
    glVertex3f(-9.0f, -BOX_HH, -7.0f);
    glEnd();

    /* --- the boxes --- */
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    glLightfv(GL_LIGHT0, GL_POSITION, lpos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, ldif);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lamb);
    glShadeModel(GL_SMOOTH);

    for (i = 0; i < count && i < 4; i++) {
        float slot = (float)i - (float)(count - 1) * 0.5f;
        int sel = (i == focus);
        float r, g, b;
        float lift = sel ? 0.17f : 0.0f;
        float back = sel ? 0.0f : 0.58f;
        float scale = sel ? 1.0f : 0.74f;
        float spread = 0.96f + 0.30f * blend;
        float turn;

        if (sel) {
            /* "turning toward the viewer": a slow sway either side of square
             * on, so the front is always readable and the spine still shows. */
            turn = 21.0f * sinf(scene_t * 0.55f);
        } else {
            turn = slot > 0.0f ? -34.0f : 34.0f;
        }

        game_colour(i, &r, &g, &b);
        if (!present[i]) { r = g = b = 0.55f; }
        if (!sel) { r *= 0.62f; g *= 0.62f; b *= 0.62f; }

        glPushMatrix();
        glTranslatef(slot * spread, lift, -back);
        glRotatef(turn, 0.0f, 1.0f, 0.0f);
        glScalef(scale, scale, scale);

        if (!present[i]) {
            /* A box with no cartridge behind it is not lit: it reads as a
             * placeholder rather than as an object in the room.  Its front is
             * still drawn -- ui_rom_art generates one saying which game it is
             * and that there is no cartridge -- but greyed with the rest. */
            glDisable(GL_LIGHTING);
            box_shell(r, g, b);
            box_front(i, 1, r, g, b, sel ? 0.70f : 0.46f);
            glEnable(GL_LIGHTING);
        } else {
            box_shell(r, g, b);
            box_front(i, 1, r, g, b, 1.0f);
        }

        if (place != NULL) {
            double bmv[16];
            glGetDoublev(GL_MODELVIEW_MATRIX, bmv);
            {
                int x0, y0, x1, y1;
                project(-BOX_HW, +BOX_HH, BOX_HD, bmv, pr, vp, &x0, &y0);
                project(+BOX_HW, -BOX_HH, BOX_HD, bmv, pr, vp, &x1, &y1);
                place[i].cx = (x0 + x1) / 2;
                place[i].cy = (y0 + y1) / 2;
                place[i].w = abs(x1 - x0);
                place[i].h = abs(y1 - y0);
            }
        }
        glPopMatrix();
    }

    /* --- a soft shadow under each box, on the snow --- */
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, blob_tex);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);   /* a shadow on the ground is seen from above */
    glBegin(GL_QUADS);
    for (i = 0; i < count && i < 4; i++) {
        float slot = (float)i - (float)(count - 1) * 0.5f;
        int sel = (i == focus);
        float spread = 0.96f + 0.30f * blend;
        float x = slot * spread;
        float z = sel ? 0.0f : -0.52f;
        float w = (sel ? 0.70f : 0.46f);
        float a = sel ? 0.34f : 0.24f;
        float y = -BOX_HH + 0.004f;
        glColor4f(0.20f, 0.32f, 0.48f, a);
        glTexCoord2f(0.0f, 0.0f); glVertex3f(x - w, y, z - w * 0.32f);
        glTexCoord2f(1.0f, 0.0f); glVertex3f(x + w, y, z - w * 0.32f);
        glTexCoord2f(1.0f, 1.0f); glVertex3f(x + w, y, z + w * 0.32f);
        glTexCoord2f(0.0f, 1.0f); glVertex3f(x - w, y, z + w * 0.32f);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);

    /* Back to the 2D projection the rest of the UI draws in. */
    glDisable(GL_LIGHTING);
    glDisable(GL_LIGHT0);
    glDisable(GL_COLOR_MATERIAL);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}
