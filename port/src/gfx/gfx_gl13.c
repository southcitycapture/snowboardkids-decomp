/* Fixed-function OpenGL 1.3 backend for gfx_pc, written for the Radeon 9000
 * in the Power Mac G4 (ARB_texture_env_combine + ATI_texture_env_combine3 +
 * ARB_texture_env_crossbar, six texture units, no fragment programs).
 *
 * The N64 colour combiner computes (A - B) * C + D per channel. Each
 * ShaderProgram here is that expression compiled into a chain of texture
 * environment stages: SUBTRACT, MODULATE, MODULATE_ADD_ATI, ADD, REPLACE.
 * Sources map as TEXEL0/TEXEL1 -> GL_TEXTUREn (crossbar), SHADE -> the vertex
 * colour array, PRIMITIVE/ENVIRONMENT/LOD -> that stage's GL_CONSTANT colour,
 * whose value is read out of the vertex stream at draw time (gfx_pc bakes it
 * into every vertex) and re-set whenever it changes inside a batch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "../ui/ui.h"

#define MAX_UNITS 6
#define MAX_PROGRAMS 128

enum { SRC_ZERO, SRC_TEX0, SRC_TEX1, SRC_TEX0A, SRC_IN1, SRC_IN2, SRC_IN3, SRC_IN4, SRC_PREV };

struct Op {
    GLenum func;      /* GL_REPLACE, GL_MODULATE, GL_ADD, GL_SUBTRACT_ARB, GL_MODULATE_ADD_ATI */
    int src[3];       /* SRC_* */
    int nsrc;
};

struct Unit {
    struct Op rgb, alpha;
    int const_in;     /* 0 none, 1..4 INPUT_k, -1 zero */
};

struct ShaderProgram {
    uint32_t shader_id;
    uint32_t aux;
    struct CCFeatures cc;
    uint8_t num_inputs;
    bool used_textures[2];
    int shade_in[2];   /* INPUT_k that is SHADE for rgb / alpha, 0 if none */
    int num_units;
    struct Unit units[MAX_UNITS];
    int stride;        /* floats per vertex */
    int off_tex, off_fog, off_in[4];
    bool in_use;
};

static struct ShaderProgram programs[MAX_PROGRAMS];
static int num_programs;
static struct ShaderProgram *cur_prg;
static GLuint dummy_tex;
static GLuint tex_bound[2];
static bool have_combine3;
static bool cur_use_alpha;
static bool cur_texture_edge;
static int warn_count;

/* ---- combiner compiler ------------------------------------------------- */

static int src_from_shader(int v) {
    switch (v) {
        case SHADER_0: return SRC_ZERO;
        case SHADER_INPUT_1: return SRC_IN1;
        case SHADER_INPUT_2: return SRC_IN2;
        case SHADER_INPUT_3: return SRC_IN3;
        case SHADER_INPUT_4: return SRC_IN4;
        case SHADER_TEXEL0: return SRC_TEX0;
        case SHADER_TEXEL0A: return SRC_TEX0A;
        case SHADER_TEXEL1: return SRC_TEX1;
        default: return SRC_ZERO;
    }
}

static bool src_is_const(int s, int shade_in) {
    if (s >= SRC_IN1 && s <= SRC_IN4) {
        return (s - SRC_IN1 + 1) != shade_in;
    }
    return s == SRC_ZERO;
}

static int src_const_id(int s) {
    if (s == SRC_ZERO) return -1;
    return s - SRC_IN1 + 1;
}

static void op_set(struct Op *op, GLenum func, int a, int b, int c) {
    op->func = func;
    op->src[0] = a;
    op->src[1] = b;
    op->src[2] = c;
    op->nsrc = func == GL_REPLACE ? 1 : (func == GL_MODULATE_ADD_ATI ? 3 : 2);
}

/* Count distinct constant inputs an op needs; returns the id of the first. */
static int op_consts(const struct Op *op, int shade_in, int *first) {
    int ids[3], n = 0, i, j;
    *first = 0;
    for (i = 0; i < op->nsrc; i++) {
        if (src_is_const(op->src[i], shade_in)) {
            int id = src_const_id(op->src[i]);
            bool dup = false;
            for (j = 0; j < n; j++) if (ids[j] == id) dup = true;
            if (!dup) ids[n++] = id;
        }
    }
    if (n > 0) *first = ids[0];
    return n;
}

/* Emit ops for (A - B) * C + D so that no op needs more than one constant. */
static int compile_channel(struct Op *out, int a, int b, int c, int d, int shade_in) {
    struct Op ops[8];
    int n = 0, i, k = 0;

    if (a == SRC_ZERO && b == SRC_ZERO) {
        op_set(&ops[n++], GL_REPLACE, d, 0, 0);
    } else {
        if (b == SRC_ZERO) {
            if (d == SRC_ZERO) op_set(&ops[n++], GL_MODULATE, a, c, 0);
            else op_set(&ops[n++], GL_MODULATE_ADD_ATI, a, d, c); /* Arg0*Arg2 + Arg1 */
        } else {
            op_set(&ops[n++], GL_SUBTRACT_ARB, a, b, 0);
            if (d == SRC_ZERO) op_set(&ops[n++], GL_MODULATE, SRC_PREV, c, 0);
            else op_set(&ops[n++], GL_MODULATE_ADD_ATI, SRC_PREV, d, c);
        }
    }

    /* Split ops that need two different constants. */
    for (i = 0; i < n; i++) {
        struct Op op = ops[i];
        int first;
        while (op_consts(&op, shade_in, &first) > 1) {
            if (op.func == GL_MODULATE_ADD_ATI) {
                /* Arg0*Arg2 + Arg1  ->  MODULATE(Arg0, Arg2) ; ADD(PREV, Arg1) */
                struct Op m, add;
                op_set(&m, GL_MODULATE, op.src[0], op.src[2], 0);
                op_set(&add, GL_ADD, SRC_PREV, op.src[1], 0);
                if (op_consts(&m, shade_in, &first) > 1) {
                    struct Op rep;
                    op_set(&rep, GL_REPLACE, m.src[0], 0, 0);
                    out[k++] = rep;
                    m.src[0] = SRC_PREV;
                }
                out[k++] = m;
                op = add;
            } else {
                /* two-source op with two constants: materialise the first */
                struct Op rep;
                op_set(&rep, GL_REPLACE, op.src[0], 0, 0);
                out[k++] = rep;
                op.src[0] = SRC_PREV;
            }
        }
        out[k++] = op;
    }
    return k;
}

static void op_passthrough(struct Op *op) {
    op_set(op, GL_REPLACE, SRC_PREV, 0, 0);
}

static bool compile_program(struct ShaderProgram *p) {
    struct Op rgb[8], alpha[8];
    int nr, na, i = 0, j = 0;
    int ca, cb, cc, cd;

    ca = src_from_shader(p->cc.c[0][0]); cb = src_from_shader(p->cc.c[0][1]);
    cc = src_from_shader(p->cc.c[0][2]); cd = src_from_shader(p->cc.c[0][3]);
    nr = compile_channel(rgb, ca, cb, cc, cd, p->shade_in[0]);

    if (p->cc.opt_alpha) {
        ca = src_from_shader(p->cc.c[1][0]); cb = src_from_shader(p->cc.c[1][1]);
        cc = src_from_shader(p->cc.c[1][2]); cd = src_from_shader(p->cc.c[1][3]);
        na = compile_channel(alpha, ca, cb, cc, cd, p->shade_in[1]);
    } else {
        na = 0;
    }

    p->num_units = 0;
    while (i < nr || j < na) {
        struct Unit *u;
        int first;
        if (p->num_units == MAX_UNITS) {
            return false;
        }
        u = &p->units[p->num_units++];
        u->const_in = 0;
        if (i < nr) {
            u->rgb = rgb[i++];
            if (op_consts(&u->rgb, p->shade_in[0], &first) > 0) u->const_in = first;
        } else {
            op_passthrough(&u->rgb);
        }
        if (j < na) {
            int c = op_consts(&alpha[j], p->shade_in[1], &first);
            if (c == 0 || u->const_in == 0 || u->const_in == first) {
                u->alpha = alpha[j++];
                if (c > 0) u->const_in = first;
            } else {
                op_passthrough(&u->alpha);
            }
        } else {
            op_passthrough(&u->alpha);
        }
    }
    if (p->num_units == 0) {
        struct Unit *u = &p->units[p->num_units++];
        u->const_in = 0;
        op_passthrough(&u->rgb);
        op_passthrough(&u->alpha);
    }
    return true;
}

/* ---- applying a program ----------------------------------------------- */

static GLenum gl_source(int s, int unit_const_in, bool alpha_channel, GLenum *operand) {
    *operand = alpha_channel ? GL_SRC_ALPHA : GL_SRC_COLOR;
    switch (s) {
        case SRC_TEX0: return GL_TEXTURE0;
        case SRC_TEX1: return GL_TEXTURE1;
        case SRC_TEX0A: *operand = GL_SRC_ALPHA; return GL_TEXTURE0;
        case SRC_PREV: return GL_PREVIOUS;
        case SRC_ZERO: return GL_CONSTANT;
        default: break;
    }
    (void)unit_const_in;
    return GL_CONSTANT; /* INPUT_k: constant unless it is the shade input, patched by caller */
}

static void apply_op(const struct Op *op, bool alpha_channel, int shade_in) {
    static const GLenum src_enum[2][3] = {
        { GL_SOURCE0_RGB, GL_SOURCE1_RGB, GL_SOURCE2_RGB },
        { GL_SOURCE0_ALPHA, GL_SOURCE1_ALPHA, GL_SOURCE2_ALPHA }
    };
    static const GLenum opd_enum[2][3] = {
        { GL_OPERAND0_RGB, GL_OPERAND1_RGB, GL_OPERAND2_RGB },
        { GL_OPERAND0_ALPHA, GL_OPERAND1_ALPHA, GL_OPERAND2_ALPHA }
    };
    int i;
    glTexEnvi(GL_TEXTURE_ENV, alpha_channel ? GL_COMBINE_ALPHA : GL_COMBINE_RGB, (GLint)op->func);
    for (i = 0; i < op->nsrc; i++) {
        GLenum operand;
        GLenum src = gl_source(op->src[i], 0, alpha_channel, &operand);
        if (op->src[i] >= SRC_IN1 && op->src[i] <= SRC_IN4 && (op->src[i] - SRC_IN1 + 1) == shade_in) {
            src = GL_PRIMARY_COLOR;
        }
        glTexEnvi(GL_TEXTURE_ENV, src_enum[alpha_channel][i], (GLint)src);
        glTexEnvi(GL_TEXTURE_ENV, opd_enum[alpha_channel][i], (GLint)operand);
    }
    glTexEnvi(GL_TEXTURE_ENV, alpha_channel ? GL_ALPHA_SCALE : GL_RGB_SCALE, 1);
}

static void apply_program(struct ShaderProgram *p) {
    int u;
    for (u = 0; u < MAX_UNITS; u++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        if (u < p->num_units) {
            glEnable(GL_TEXTURE_2D);
            if (u == 0) glBindTexture(GL_TEXTURE_2D, p->used_textures[0] ? tex_bound[0] : dummy_tex);
            else if (u == 1) glBindTexture(GL_TEXTURE_2D, p->used_textures[1] ? tex_bound[1] : dummy_tex);
            else glBindTexture(GL_TEXTURE_2D, dummy_tex);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            apply_op(&p->units[u].rgb, false, p->shade_in[0]);
            apply_op(&p->units[u].alpha, true, p->shade_in[1]);
            if (p->units[u].const_in == -1) {
                static const float zero[4] = { 0, 0, 0, 0 };
                glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, zero);
            }
        } else {
            glDisable(GL_TEXTURE_2D);
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

/* ---- GfxRenderingAPI ---------------------------------------------------- */

static bool gl13_z_is_from_0_to_1(void) {
    return false;
}

static void gl13_unload_shader(struct ShaderProgram *old_prg) {
    (void)old_prg;
}

static void gl13_load_shader(struct ShaderProgram *new_prg) {
    cur_prg = new_prg;
    if (new_prg != NULL) {
        apply_program(new_prg);
    }
}

static struct ShaderProgram *gl13_lookup_shader(uint32_t shader_id, uint32_t aux) {
    int i;
    for (i = 0; i < num_programs; i++) {
        if (programs[i].shader_id == shader_id && programs[i].aux == aux) {
            return &programs[i];
        }
    }
    return NULL;
}

static struct ShaderProgram *gl13_create_and_load_new_shader(uint32_t shader_id, uint32_t aux) {
    struct ShaderProgram *p;
    int n, off, i;
    if (num_programs == MAX_PROGRAMS) {
        fprintf(stderr, "gfx_gl13: out of shader slots\n");
        num_programs = 0;
    }
    p = &programs[num_programs++];
    memset(p, 0, sizeof(*p));
    p->shader_id = shader_id;
    p->aux = aux;
    gfx_cc_get_features(shader_id, &p->cc);
    p->num_inputs = (uint8_t)p->cc.num_inputs;
    p->used_textures[0] = p->cc.used_textures[0];
    p->used_textures[1] = p->cc.used_textures[1];
    for (i = 0; i < 2; i++) {
        int k;
        p->shade_in[i] = 0;
        for (k = 0; k < 4; k++) {
            if (aux & (1u << (i * 4 + k))) {
                p->shade_in[i] = k + 1;
            }
        }
    }
    /* vertex layout (see gfx_pc.c gfx_sp_tri1) */
    off = 4;
    p->off_tex = -1;
    p->off_fog = -1;
    if (p->used_textures[0] || p->used_textures[1]) { p->off_tex = off; off += 2; }
    if (p->cc.opt_fog) { p->off_fog = off; off += 4; }
    n = p->cc.opt_alpha ? 4 : 3;
    for (i = 0; i < 4; i++) {
        p->off_in[i] = i < p->num_inputs ? off + i * n : -1;
    }
    off += p->num_inputs * n;
    p->stride = off;

    if (!compile_program(p)) {
        fprintf(stderr, "gfx_gl13: combiner %08x/%x needs more than %d units; approximating\n",
                (unsigned)shader_id, (unsigned)aux, MAX_UNITS);
    }
    gl13_load_shader(p);
    return p;
}

static void gl13_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static uint32_t gl13_new_texture(void) {
    GLuint id;
    glGenTextures(1, &id);
    return id;
}

static void gl13_select_texture(int tile, uint32_t texture_id) {
    tex_bound[tile] = texture_id;
    glActiveTexture(GL_TEXTURE0 + (GLenum)tile);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glActiveTexture(GL_TEXTURE0);
}

static uint32_t next_pot(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

int sbk_tex_dump_left; /* debug: dump the next N uploaded textures to /tmp/sbk-tex-N.ppm */
static int sbk_tex_dump_index;

static void dump_texture_ppm(const uint8_t *rgba, int w, int h) {
    char name[64];
    FILE *f;
    int i;
    snprintf(name, sizeof(name), "/tmp/sbk-tex-%02d.ppm", sbk_tex_dump_index++);
    f = fopen(name, "wb");
    if (f == NULL) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        /* alpha shown as magenta */
        if (rgba[i * 4 + 3] == 0) { fputc(255, f); fputc(0, f); fputc(255, f); }
        else fwrite(rgba + i * 4, 1, 3, f);
    }
    fclose(f);
    printf("sbk-tex: %s %dx%d\n", name, w, h);
}

static void gl13_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    uint32_t pw = next_pot((uint32_t)width), ph = next_pot((uint32_t)height);
    if (sbk_tex_dump_left > 0) {
        sbk_tex_dump_left--;
        dump_texture_ppm(rgba32_buf, width, height);
    }
    /* selected texture unit is left at the tile by select_texture's caller order:
     * gfx_pc calls select_texture(tile) then upload_texture, so rebind here. */
    if (pw != (uint32_t)width || ph != (uint32_t)height) {
        static uint32_t *scratch;
        static size_t scratch_size;
        size_t need = (size_t)pw * ph * 4;
        uint32_t x, y;
        if (need > scratch_size) {
            scratch = realloc(scratch, need);
            scratch_size = need;
        }
        for (y = 0; y < ph; y++) {
            uint32_t sy = y * (uint32_t)height / ph;
            for (x = 0; x < pw; x++) {
                uint32_t sx = x * (uint32_t)width / pw;
                memcpy(&scratch[y * pw + x], rgba32_buf + (sy * (uint32_t)width + sx) * 4, 4);
            }
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)pw, (GLsizei)ph, 0, GL_RGBA, GL_UNSIGNED_BYTE, scratch);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    }
}

static GLint gl_wrap(uint32_t cm) {
    if (cm & G_TX_CLAMP) return GL_CLAMP_TO_EDGE;
    if (cm & G_TX_MIRROR) return GL_MIRRORED_REPEAT;
    return GL_REPEAT;
}

static void gl13_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    glActiveTexture(GL_TEXTURE0 + (GLenum)tile);
    glBindTexture(GL_TEXTURE_2D, tex_bound[tile]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap(cmt));
    glActiveTexture(GL_TEXTURE0);
}

static void gl13_set_depth_test(bool depth_test) {
    if (depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
}

static void gl13_set_depth_mask(bool z_upd) {
    glDepthMask(z_upd ? GL_TRUE : GL_FALSE);
}

static void gl13_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        glPolygonOffset(-2, -2);
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glPolygonOffset(0, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

/* The N64 frame is drawn into this rectangle of the window (4:3 letterbox in
 * fullscreen); gfx_pc only ever sees its size, the offset is applied here. */
static int out_x, out_y, out_w, out_h, out_win_w, out_win_h;

void gfx_gl13_set_output_rect(int x, int y, int w, int h, int win_w, int win_h) {
    out_x = x; out_y = y; out_w = w; out_h = h; out_win_w = win_w; out_win_h = win_h;
}

/* --- resolution modes and filters (settings.c drives these) ------------- */
/* Filled in below; the render size is 0 in `native` mode, where gfx_pc draws
 * straight into the output rectangle. */
static int render_w, render_h;
static int filter_mode;

int gfx_gl13_render_width(void) { return render_w; }
int gfx_gl13_render_height(void) { return render_h; }

void gfx_gl13_set_render_scale(int mode) {
    int w = 0, h = 0;
    if (mode == 1) { w = 320; h = 240; }
    else if (mode == 2) { w = 640; h = 480; }
    if (w == render_w) return;
    render_w = w; render_h = h;
}

void gfx_gl13_set_filter(int filter) { filter_mode = filter; }

/* ---- the present-time post pass ----------------------------------------
 * The N64 frame is drawn into the bottom-left render_w x render_h of the
 * window (the whole output rectangle in `native`), copied into a
 * power-of-two texture with glCopyTexSubImage2D -- the Radeon 9000 has no
 * FBO and no NPOT textures -- and drawn scaled into the output rectangle.
 * The filters are one more quad each on top of that, with the mask texture
 * mapped exactly one texel per output pixel so no moire is possible. */

static GLuint copy_tex, scan_tex, grille_tex;
static int copy_tw, copy_th;

static int pot(int v) { int p = 1; while (p < v) p <<= 1; return p; }

static void ensure_copy_tex(int w, int h) {
    int tw = pot(w), th = pot(h);
    if (copy_tex != 0 && tw == copy_tw && th == copy_th) return;
    if (copy_tex != 0) glDeleteTextures(1, &copy_tex);
    glGenTextures(1, &copy_tex);
    glBindTexture(GL_TEXTURE_2D, copy_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tw, th, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    copy_tw = tw; copy_th = th;
}

static void ensure_masks(void) {
    /* scanlines: one line clear, the next 35% dark (tuned on 1680x1050 shots) */
    static const unsigned char scan[2] = { 0, 90 };
    /* Aperture grille: an R/G/B triad plus a neutral column, 4 output pixels
     * wide. The period is 4 and not the usual 3 because the Radeon 9000 has
     * no NPOT textures -- a 3x1 mask is an incomplete texture, texturing
     * silently switches off and the mask multiplies the frame by white. */
    static const unsigned char grille[12] = { 255, 216, 216,  216, 255, 216,
                                              216, 216, 255,  236, 236, 236 };
    if (scan_tex == 0) {
        glGenTextures(1, &scan_tex);
        glBindTexture(GL_TEXTURE_2D, scan_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 1, 2, 0, GL_ALPHA, GL_UNSIGNED_BYTE, scan);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    if (grille_tex == 0) {
        glGenTextures(1, &grille_tex);
        glBindTexture(GL_TEXTURE_2D, grille_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, grille);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
}

static void post_begin(void) {
    int u;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    for (u = 5; u >= 0; u--) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_FOG);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glViewport(0, 0, out_win_w, out_win_h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, (GLdouble)out_win_w, (GLdouble)out_win_h, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
}

static void post_end(void) {
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopAttrib();
}

/* (x,y) top-left in window pixels, texture coordinates already in texels. */
static void post_quad(GLuint tex, int x, int y, int w, int h,
                      float s0, float t0, float s1, float t1) {
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBegin(GL_QUADS);
    glTexCoord2f(s0, t1); glVertex2i(x, y);
    glTexCoord2f(s1, t1); glVertex2i(x + w, y);
    glTexCoord2f(s1, t0); glVertex2i(x + w, y + h);
    glTexCoord2f(s0, t0); glVertex2i(x, y + h);
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

static void post_present(void) {
    if (out_w <= 0) return;
    post_begin();
    if (render_w > 0) {
        int linear = filter_mode == 3;
        ensure_copy_tex(render_w, render_h);
        glBindTexture(GL_TEXTURE_2D, copy_tex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, render_w, render_h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        post_quad(copy_tex, out_x, out_y, out_w, out_h,
                  0.0f, 0.0f, (float)render_w / (float)copy_tw, (float)render_h / (float)copy_th);
    }
    if (filter_mode == 1 || filter_mode == 2) {
        ensure_masks();
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, filter_mode == 1 ? GL_MODULATE : GL_REPLACE);
        if (filter_mode == 1) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
            /* one texel per output pixel: the line period is exactly 2 px */
            post_quad(scan_tex, out_x, out_y, out_w, out_h, 0.5f, 0.0f, 0.5f, (float)out_h / 2.0f);
        } else {
            glEnable(GL_BLEND);
            glBlendFunc(GL_DST_COLOR, GL_ZERO);   /* frame *= mask */
            post_quad(grille_tex, out_x, out_y, out_w, out_h, 0.0f, 0.5f, (float)out_w / 4.0f, 0.5f);
        }
    }
    post_end();
}

static void gl13_set_viewport(int x, int y, int width, int height) {
    if (render_w > 0) { glViewport(x, y, width, height); return; }
    glViewport(x + out_x, y + out_y, width, height);
}

static void gl13_set_scissor(int x, int y, int width, int height) {
    if (render_w > 0) { glScissor(x, y, width, height); return; }
    glScissor(x + out_x, y + out_y, width, height);
}

static void gl13_set_use_alpha(bool use_alpha) {
    cur_use_alpha = use_alpha;
    if (use_alpha) glEnable(GL_BLEND); else glDisable(GL_BLEND);
}

/* Set every unit's GL_CONSTANT from the vertex at `v`. */
static void set_constants(const struct ShaderProgram *p, const float *v) {
    int u;
    for (u = 0; u < p->num_units; u++) {
        int k = p->units[u].const_in;
        float c[4];
        if (k <= 0) continue;
        c[0] = v[p->off_in[k - 1] + 0];
        c[1] = v[p->off_in[k - 1] + 1];
        c[2] = v[p->off_in[k - 1] + 2];
        c[3] = p->cc.opt_alpha ? v[p->off_in[k - 1] + 3] : 1.0f;
        glActiveTexture(GL_TEXTURE0 + (GLenum)u);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c);
    }
    glActiveTexture(GL_TEXTURE0);
}

static bool constants_differ(const struct ShaderProgram *p, const float *a, const float *b) {
    int u, n = p->cc.opt_alpha ? 4 : 3;
    for (u = 0; u < p->num_units; u++) {
        int k = p->units[u].const_in;
        if (k > 0 && memcmp(a + p->off_in[k - 1], b + p->off_in[k - 1], (size_t)n * sizeof(float)) != 0) {
            return true;
        }
    }
    return false;
}

static void gl13_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    struct ShaderProgram *p = cur_prg;
    GLsizei stride;
    size_t start = 0, t;
    int u;
    (void)buf_vbo_len;
    if (p == NULL) return;
    stride = (GLsizei)(p->stride * sizeof(float));

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, stride, buf_vbo);

    for (u = 0; u < 2; u++) {
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)u);
        if (p->off_tex >= 0 && p->used_textures[u]) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, stride, buf_vbo + p->off_tex);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
    }
    glClientActiveTexture(GL_TEXTURE0);

    {
        int k = p->shade_in[0] != 0 ? p->shade_in[0] : p->shade_in[1];
        if (k != 0) {
            glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer(p->cc.opt_alpha ? 4 : 3, GL_FLOAT, stride, buf_vbo + p->off_in[k - 1]);
        } else {
            glDisableClientState(GL_COLOR_ARRAY);
            glColor4f(1, 1, 1, 1);
        }
    }

    if (p->off_fog >= 0) {
        glEnable(GL_FOG);
        glFogfv(GL_FOG_COLOR, buf_vbo + p->off_fog);
        glEnableClientState(GL_FOG_COORDINATE_ARRAY_EXT);
        glFogCoordPointerEXT(GL_FLOAT, stride, buf_vbo + p->off_fog + 3);
    } else {
        glDisable(GL_FOG);
        glDisableClientState(GL_FOG_COORDINATE_ARRAY_EXT);
    }

    if (p->cc.opt_texture_edge) {
        /* cut-out alpha: opaque above the threshold, gone below (no blending) */
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.3f);
        glDisable(GL_BLEND);
    } else {
        glDisable(GL_ALPHA_TEST);
        if (cur_use_alpha) glEnable(GL_BLEND);
    }

    set_constants(p, buf_vbo);
    for (t = 1; t < buf_vbo_num_tris; t++) {
        const float *prev = buf_vbo + (t - 1) * 3 * p->stride;
        const float *cur = buf_vbo + t * 3 * p->stride;
        if (constants_differ(p, prev, cur)) {
            glDrawArrays(GL_TRIANGLES, (GLint)(start * 3), (GLsizei)((t - start) * 3));
            set_constants(p, cur);
            start = t;
        }
    }
    glDrawArrays(GL_TRIANGLES, (GLint)(start * 3), (GLsizei)((buf_vbo_num_tris - start) * 3));
}

static void gl13_init(void) {
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    static const uint8_t white[4] = { 255, 255, 255, 255 };
    have_combine3 = ext != NULL && strstr(ext, "GL_ATI_texture_env_combine3") != NULL;
    printf("gfx_gl13: %s / %s (combine3 %s)\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), have_combine3 ? "yes" : "NO");

    glGenTextures(1, &dummy_tex);
    glBindTexture(GL_TEXTURE_2D, dummy_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);   /* gfx_pc culls on the CPU */
    glDisable(GL_DITHER);
    glEnable(GL_SCISSOR_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthRange(0.0, 1.0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 0.0f);
    glFogf(GL_FOG_END, 1.0f);
    glFogi(GL_FOG_COORDINATE_SOURCE_EXT, GL_FOG_COORDINATE_EXT);
    glShadeModel(GL_SMOOTH);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
}

static void gl13_on_resize(void) {
}

static void gl13_start_frame(void) {
    /* letterbox: paint the whole window black, then confine drawing to the output rectangle */
    if (out_x != 0 || out_y != 0) {
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, out_win_w, out_win_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
    }
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_SCISSOR_TEST);
}

static void gl13_end_frame(void) {
}

int sbk_frame_dump_left; /* debug: dump the next N presented frames to /tmp/sbk-frame-N.ppm */
int sbk_hash_frames;     /* --hashframe: FNV-1a of every presented frame, for determinism tests */
unsigned sbk_last_frame_hash;

static void hash_frame(void) {
    static unsigned char *px;
    unsigned h = 2166136261u;
    size_t i, n = (size_t)640 * 480 * 3;
    if (px == NULL) px = malloc(n);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, 640, 480, GL_RGB, GL_UNSIGNED_BYTE, px);
    for (i = 0; i < n; i++) { h ^= px[i]; h *= 16777619u; }
    sbk_last_frame_hash = h;
}

static void gl13_finish_render(void) {
    post_present();
    /* The options overlay goes on last, over the finished frame. */
    sbk_ui_overlay_draw(out_win_w, out_win_h, out_x, out_y, out_w, out_h);
    if (sbk_hash_frames) {
        hash_frame();
    }
    if (sbk_frame_dump_left > 0) {
        static int index;
        GLint vp[4];
        int w, h, y;
        unsigned char *px;
        char name[64];
        FILE *f;
        sbk_frame_dump_left--;
        glGetIntegerv(GL_VIEWPORT, vp);
        w = 640; h = 480; /* window size: viewport may be a sub-rectangle */
        px = malloc((size_t)w * h * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
        snprintf(name, sizeof(name), "/tmp/sbk-frame-%02d.ppm", index++);
        f = fopen(name, "wb");
        if (f != NULL) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
            fclose(f);
            printf("sbk-frame: %s (viewport %d,%d %dx%d)\n", name, vp[0], vp[1], vp[2], vp[3]);
        }
        free(px);
    }
}

static void gl13_clear(bool color, float r, float g, float b, bool depth) {
    GLbitfield mask = 0;
    if (render_w > 0) {
        glScissor(0, 0, render_w, render_h);
    } else if (out_w > 0) {
        glScissor(out_x, out_y, out_w, out_h); /* the frame, not the letterbox bars */
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    if (color) { glClearColor(r, g, b, 1.0f); mask |= GL_COLOR_BUFFER_BIT; }
    if (depth) { glDepthMask(GL_TRUE); mask |= GL_DEPTH_BUFFER_BIT; }
    if (mask) glClear(mask);
    glEnable(GL_SCISSOR_TEST);
}

static void gl13_shutdown(void) {
}

struct GfxRenderingAPI gfx_gl13_rapi = {
    gl13_z_is_from_0_to_1,
    gl13_unload_shader,
    gl13_load_shader,
    gl13_create_and_load_new_shader,
    gl13_lookup_shader,
    gl13_shader_get_info,
    gl13_new_texture,
    gl13_select_texture,
    gl13_upload_texture,
    gl13_set_sampler_parameters,
    gl13_set_depth_test,
    gl13_set_depth_mask,
    gl13_set_zmode_decal,
    gl13_set_viewport,
    gl13_set_scissor,
    gl13_set_use_alpha,
    gl13_draw_triangles,
    gl13_init,
    gl13_on_resize,
    gl13_start_frame,
    gl13_end_frame,
    gl13_finish_render,
    gl13_clear,
    gl13_shutdown
};
