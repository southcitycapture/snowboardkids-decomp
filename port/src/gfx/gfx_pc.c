#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <sys/time.h>
#include "../ultra/sbk_os.h"

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_pc.h"
#include "../debug/perf.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"

static void gfx_unsupported(const char *what, int line) {
    static int n;
    if (n++ < 20) fprintf(stderr, "gfx_pc: unsupported: %s (line %d)\n", what, line);
}
static uint32_t segment_table[16];
#define SUPPORT_CHECK(x) do { if (!(x)) gfx_unsupported(#x, __LINE__); } while (0)

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_) * 0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_) * 0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_) * 0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define RATIO_X (gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH))
#define RATIO_Y (gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT))

#define MAX_BUFFERED 256
#define MAX_LIGHTS 2
#define MAX_VERTICES 64

struct RGBA {
    uint8_t r, g, b, a;
};

struct XYWidthHeight {
    uint16_t x, y, width, height;
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    struct RGBA color;
    uint8_t clip_rej;
};

struct TextureHashmapNode {
    struct TextureHashmapNode *next;
    
    const uint8_t *texture_addr;
    const uint8_t *tlut;
    uint32_t size_bytes;
    uint32_t content_hash; /* of the texel bytes and the palette: this game rewrites scratch textures and scaled palettes in place */
    uint8_t fmt, siz;
    
    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;
};
static struct {
    struct TextureHashmapNode *hashmap[1024];
    struct TextureHashmapNode pool[512];
    uint32_t pool_pos;
} gfx_texture_cache;

struct ColorCombiner {
    uint32_t cc_id;
    struct ShaderProgram *prg;
    uint8_t shader_input_mapping[2][4];
};

static struct ColorCombiner color_combiner_pool[64];
static uint8_t color_combiner_pool_size;

static struct RSP {
    float modelview_matrix_stack[11][4][4];
    uint8_t modelview_matrix_stack_size;
    
    float MP_matrix[4][4];
    float P_matrix[4][4];
    
    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;
    
    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;
    
    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;
    
    struct LoadedVertex loaded_vertices[MAX_VERTICES + 4];

    uint8_t saved_opcode;
    uint8_t saved_tile;
    uint16_t saved_uls, saved_ult;
    int32_t saved_lrx, saved_lry, saved_ulx, saved_uly;
} rsp;

static struct RDP {
    const uint8_t *palette;
    const uint8_t *tlut[16];   /* source of each 16-entry TLUT slot (CI4 palette index) */
    struct {
        const uint8_t *addr;
        uint8_t siz;
        uint8_t tile_number;
        uint32_t tmem;
        uint32_t width;      /* image width in texels (SETTIMG), the DRAM row stride */
        uint32_t line_bytes; /* the load tile's line: TMEM row stride for LOADTILE */
    } texture_to_load;
    struct {
        const uint8_t *addr;
        uint32_t size_bytes;
        const uint8_t *key; /* identifies the source (LOADTILE decodes into a scratch buffer) */
    } loaded_texture[2];
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
        uint8_t palette;
        uint8_t masks, maskt;
    } texture_tile;
    bool textures_changed[2];
    
    uint32_t other_mode_l, other_mode_h;
    uint32_t combine_mode;
    
    struct RGBA env_color, prim_color, fog_color, fill_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
} rdp;

static struct RenderingState {
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    struct TextureHashmapNode *textures[2];
} rendering_state;

struct GfxDimensions gfx_current_dimensions;


static float buf_vbo[MAX_BUFFERED * (26 * 3)]; // 3 vertices in a triangle and 26 floats per vtx
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

static unsigned long get_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long)tv.tv_sec * 1000000 + (unsigned long)tv.tv_usec;
}

static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
        int num = buf_vbo_num_tris;
        unsigned long t0 = get_time();
        sbk_perf_draws++;
        sbk_perf_tris += buf_vbo_num_tris;
        gfx_rapi->draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
        unsigned long t1 = get_time();
        /*if (t1 - t0 > 1000) {
            printf("f: %d %d\n", num, (int)(t1 - t0));
        }*/
    }
}

static struct ShaderProgram *gfx_lookup_or_create_shader_program(uint32_t shader_id, uint32_t aux) {
    struct ShaderProgram *prg = gfx_rapi->lookup_shader(shader_id, aux);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id, aux);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static void gfx_generate_cc(struct ColorCombiner *comb, uint32_t cc_id) {
    uint8_t c[2][4];
    uint32_t shader_id = (cc_id >> 24) << 24;
    uint8_t shader_input_mapping[2][4] = {{0}};
    for (int i = 0; i < 4; i++) {
        c[0][i] = (cc_id >> (i * 3)) & 7;
        c[1][i] = (cc_id >> (12 + i * 3)) & 7;
    }
    for (int i = 0; i < 2; i++) {
        if (c[i][0] == c[i][1] || c[i][2] == CC_0) {
            c[i][0] = c[i][1] = c[i][2] = 0;
        }
        uint8_t input_number[8] = {0};
        int next_input_number = SHADER_INPUT_1;
        for (int j = 0; j < 4; j++) {
            int val = 0;
            switch (c[i][j]) {
                case CC_0:
                    break;
                case CC_TEXEL0:
                    val = SHADER_TEXEL0;
                    break;
                case CC_TEXEL1:
                    val = SHADER_TEXEL1;
                    break;
                case CC_TEXEL0A:
                    val = SHADER_TEXEL0A;
                    break;
                case CC_PRIM:
                case CC_SHADE:
                case CC_ENV:
                case CC_LOD:
                    if (input_number[c[i][j]] == 0) {
                        shader_input_mapping[i][next_input_number - 1] = c[i][j];
                        input_number[c[i][j]] = next_input_number++;
                    }
                    val = input_number[c[i][j]];
                    break;
            }
            shader_id |= val << (i * 12 + j * 3);
        }
    }
    uint32_t aux = 0;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            if (shader_input_mapping[i][j] == CC_SHADE) {
                aux |= 1u << (i * 4 + j);
            }
        }
    }
    comb->cc_id = cc_id;
    comb->prg = gfx_lookup_or_create_shader_program(shader_id, aux);
    memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
}

static struct ColorCombiner *gfx_lookup_or_create_color_combiner(uint32_t cc_id) {
    static struct ColorCombiner *prev_combiner;
    if (prev_combiner != NULL && prev_combiner->cc_id == cc_id) {
        return prev_combiner;
    }
    
    for (size_t i = 0; i < color_combiner_pool_size; i++) {
        if (color_combiner_pool[i].cc_id == cc_id) {
            return prev_combiner = &color_combiner_pool[i];
        }
    }
    gfx_flush();
    struct ColorCombiner *comb = &color_combiner_pool[color_combiner_pool_size++];
    gfx_generate_cc(comb, cc_id);
    return prev_combiner = comb;
}

static uint32_t content_hash(uint32_t h, const uint8_t *p, uint32_t n) {
    /* FNV-1a over 32-bit words (the data is 8-byte aligned; n is a multiple of 8) */
    const uint32_t *w = (const uint32_t *)p;
    uint32_t i;
    for (i = 0; i < n / 4; i++) {
        h = (h ^ w[i]) * 16777619u;
    }
    return h;
}

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr, uint32_t fmt, uint32_t siz, const uint8_t *tlut, uint32_t size_bytes, uint32_t chash) {
    size_t hash = (uintptr_t)orig_addr ^ ((uintptr_t)tlut >> 3) ^ (size_bytes << 7) ^ chash;
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz && (*node)->tlut == tlut && (*node)->size_bytes == size_bytes && (*node)->content_hash == chash) {
            gfx_rapi->select_texture(tile, (*node)->texture_id);
            *n = *node;
            return true;
        }
        node = &(*node)->next;
    }
    if (gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode)) {
        // Pool is full. We just invalidate everything and start over.
        gfx_texture_cache.pool_pos = 0;
        node = &gfx_texture_cache.hashmap[hash];
        //puts("Clearing texture cache");
    }
    *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];
    if ((*node)->texture_addr == NULL) {
        (*node)->texture_id = gfx_rapi->new_texture();
    }
    gfx_rapi->select_texture(tile, (*node)->texture_id);
    gfx_rapi->set_sampler_parameters(tile, false, 0, 0);
    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->linear_filter = false;
    (*node)->next = NULL;
    (*node)->texture_addr = orig_addr;
    (*node)->tlut = tlut;
    (*node)->size_bytes = size_bytes;
    (*node)->content_hash = chash;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    *n = *node;
    return false;
}

static void import_texture_rgba16(int tile) {
    uint8_t rgba32_buf[8192];
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes / 2; i++) {
        uint16_t col16 = (rdp.loaded_texture[tile].addr[2 * i] << 8) | rdp.loaded_texture[tile].addr[2 * i + 1];
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_rgba32(int tile) {
    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = (rdp.loaded_texture[tile].size_bytes / 2) / rdp.texture_tile.line_size_bytes;
    gfx_rapi->upload_texture(rdp.loaded_texture[tile].addr, width, height);
}

static void import_texture_ia4(int tile) {
    uint8_t rgba32_buf[32768];
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part >> 1;
        uint8_t alpha = part & 1;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_3_8(r);
        rgba32_buf[4*i + 1] = SCALE_3_8(g);
        rgba32_buf[4*i + 2] = SCALE_3_8(b);
        rgba32_buf[4*i + 3] = alpha ? 255 : 0;
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_ia8(int tile) {
    uint8_t rgba32_buf[16384];
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t intensity = rdp.loaded_texture[tile].addr[i] >> 4;
        uint8_t alpha = rdp.loaded_texture[tile].addr[i] & 0xf;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        rgba32_buf[4*i + 3] = SCALE_4_8(alpha);
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_ia16(int tile) {
    uint8_t rgba32_buf[8192];
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes / 2; i++) {
        uint8_t intensity = rdp.loaded_texture[tile].addr[2 * i];
        uint8_t alpha = rdp.loaded_texture[tile].addr[2 * i + 1];
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        rgba32_buf[4*i + 3] = alpha;
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_i4(int tile) {
    uint8_t rgba32_buf[32768];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        rgba32_buf[4*i + 3] = 255;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_i8(int tile) {
    uint8_t rgba32_buf[16384];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t intensity = rdp.loaded_texture[tile].addr[i];
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        rgba32_buf[4*i + 3] = 255;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}


static void import_texture_ci4(int tile) {
    uint8_t rgba32_buf[32768];
    const uint8_t *pal = rdp.tlut[rdp.texture_tile.palette];
    if (pal == NULL) pal = rdp.palette;
    if (pal == NULL) { gfx_unsupported("CI4 texture without a TLUT", __LINE__); return; }
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
        uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint16_t col16 = (pal[idx * 2] << 8) | pal[idx * 2 + 1]; // Big endian load
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_ci8(int tile) {
    uint8_t rgba32_buf[16384];
    const uint8_t *pal = rdp.tlut[0] != NULL ? rdp.tlut[0] : rdp.palette;
    if (pal == NULL) { gfx_unsupported("CI8 texture without a TLUT", __LINE__); return; }
    
    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t idx = rdp.loaded_texture[tile].addr[i];
        uint16_t col16 = (pal[idx * 2] << 8) | pal[idx * 2 + 1]; // Big endian load
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }
    
    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;
    
    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

extern int sbk_tex_dump_left;
int sbk_tri_dump_all; /* --dumptris: log every triangle of the dumped tasks */
int sbk_bigtri_area;  /* --bigtri N: log any on-screen triangle covering more than N pixels */
int sbk_bigtri_left;  /* remaining lines to log */
static const void *sbk_last_mtx_addr;
static uint8_t sbk_last_mtx_params;
static const void *sbk_last_vtx_addr;
static unsigned sbk_last_vtx_n, sbk_last_vtx_v0;
int sbk_tri_drawn;    /* triangles logged since the last arm (cap 6000) */

static bool sbk_texture_addr_ok(const void *addr);

static void import_texture(int tile) {
    uint8_t fmt = rdp.texture_tile.fmt;
    uint8_t siz = rdp.texture_tile.siz;
    if (sbk_tex_dump_left > 0 && sbk_tex_dump_left > 100) { /* only the first few after a dump starts */
        printf("sbk-tex: import tile%d fmt=%u siz=%u line_bytes=%u loaded=%u bytes tilesize=(%u,%u)-(%u,%u) pal=%u cms=%u cmt=%u addr=%p\n",
               tile, fmt, siz, rdp.texture_tile.line_size_bytes, rdp.loaded_texture[tile].size_bytes,
               rdp.texture_tile.uls >> 2, rdp.texture_tile.ult >> 2, rdp.texture_tile.lrs >> 2, rdp.texture_tile.lrt >> 2,
               rdp.texture_tile.palette, rdp.texture_tile.cms, rdp.texture_tile.cmt, (const void *)rdp.loaded_texture[tile].addr);
    }
    
    const uint8_t *tlut = NULL;
    if (fmt == G_IM_FMT_CI) {
        tlut = siz == G_IM_SIZ_4b ? rdp.tlut[rdp.texture_tile.palette] : rdp.tlut[0];
        if (tlut == NULL) tlut = rdp.palette;
    }
    {
        const uint8_t *key = rdp.loaded_texture[tile].key != NULL ? rdp.loaded_texture[tile].key : rdp.loaded_texture[tile].addr;
        uint32_t chash = content_hash(2166136261u, rdp.loaded_texture[tile].addr, rdp.loaded_texture[tile].size_bytes & ~7u);
        if (tlut != NULL) {
            chash = content_hash(chash, tlut, siz == G_IM_SIZ_4b ? 32 : 512);
        }
        if (gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], key, fmt, siz, tlut, rdp.loaded_texture[tile].size_bytes, chash)) {
            return;
        }
    }
    
    int t0 = get_time();
    sbk_perf_tex++;
    sbk_perf_tex_bytes += rdp.loaded_texture[tile].size_bytes;
    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile);
        } else {
            abort();
        }
    } else {
        abort();
    }
    int t1 = get_time();
    //printf("Time diff: %d\n", t1 - t0);
}

static void gfx_normalize_vector(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void calculate_normal_dir(const Light_t *light, float coeffs[3]) {
    float light_dir[3] = {
        light->dir[0] / 127.0f,
        light->dir[1] / 127.0f,
        light->dir[2] / 127.0f
    };
    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] +
                        a[i][1] * b[1][j] +
                        a[i][2] * b[2][j] +
                        a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

static void gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
    float matrix[4][4];
    sbk_last_mtx_addr = addr;
    sbk_last_mtx_params = parameters;
#ifndef GBI_FLOATS
    // Original GBI where fixed point matrices are used
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif
    
    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
            if (rsp.modelview_matrix_stack_size > 0) {
                gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
            }
        }
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    return x * (4.0f / 3.0f) / ((float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height);
}

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    sbk_last_vtx_addr = vertices;
    sbk_last_vtx_n = (unsigned)n_vertices;
    sbk_last_vtx_v0 = (unsigned)dest_index;
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];
        
        float x = v->ob[0] * rsp.MP_matrix[0][0] + v->ob[1] * rsp.MP_matrix[1][0] + v->ob[2] * rsp.MP_matrix[2][0] + rsp.MP_matrix[3][0];
        float y = v->ob[0] * rsp.MP_matrix[0][1] + v->ob[1] * rsp.MP_matrix[1][1] + v->ob[2] * rsp.MP_matrix[2][1] + rsp.MP_matrix[3][1];
        float z = v->ob[0] * rsp.MP_matrix[0][2] + v->ob[1] * rsp.MP_matrix[1][2] + v->ob[2] * rsp.MP_matrix[2][2] + rsp.MP_matrix[3][2];
        float w = v->ob[0] * rsp.MP_matrix[0][3] + v->ob[1] * rsp.MP_matrix[1][3] + v->ob[2] * rsp.MP_matrix[2][3] + rsp.MP_matrix[3][3];
        
        x = gfx_adjust_x_for_aspect_ratio(x);
        
        short U = v->tc[0] * rsp.texture_scaling_factor.s >> 16;
        short V = v->tc[1] * rsp.texture_scaling_factor.t >> 16;
        
        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
                calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
                rsp.lights_changed = false;
            }
            
            int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
            int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
            int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];
            
            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                float intensity = 0;
                intensity += vn->n[0] * rsp.current_lights_coeffs[i][0];
                intensity += vn->n[1] * rsp.current_lights_coeffs[i][1];
                intensity += vn->n[2] * rsp.current_lights_coeffs[i][2];
                intensity /= 127.0f;
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }
            
            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;
            
            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                dotx += vn->n[0] * rsp.current_lookat_coeffs[0][0];
                dotx += vn->n[1] * rsp.current_lookat_coeffs[0][1];
                dotx += vn->n[2] * rsp.current_lookat_coeffs[0][2];
                doty += vn->n[0] * rsp.current_lookat_coeffs[1][0];
                doty += vn->n[1] * rsp.current_lookat_coeffs[1][1];
                doty += vn->n[2] * rsp.current_lookat_coeffs[1][2];
                
                U = (int32_t)((dotx / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.s);
                V = (int32_t)((doty / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.t);
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }
        
        d->u = U;
        d->v = V;
        
        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) d->clip_rej |= 1;
        if (x > w) d->clip_rej |= 2;
        if (y < -w) d->clip_rej |= 4;
        if (y > w) d->clip_rej |= 8;
        if (z < -w) d->clip_rej |= 16;
        if (z > w) d->clip_rej |= 32;
        
        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;
        
        if (rsp.geometry_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }
            
            float winv = 1.0f / w;
            if (winv < 0.0f) {
                winv = 32767.0f;
            }
            
            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            if (fog_z < 0) fog_z = 0;
            if (fog_z > 255) fog_z = 255;
            d->color.a = fog_z; // Use alpha variable to store fog factor
        } else {
            d->color.a = v->cn[3];
        }
    }
}

/* The N64 triple-buffers and pre-clears the *next* framebuffer while the
 * current one is still to be shown; the single GL back buffer represents
 * whichever N64 buffer geometry was last drawn into, and full-screen fills
 * aimed at any other buffer are remembered and applied when drawing switches
 * to it. */
static void *gl_held_target;
static struct {
    void *target;
    bool color;
    float r, g, b;
    bool depth;
} pending_clear;

static void gfx_switch_target(void) {
    if (rdp.color_image_address == gl_held_target) {
        return;
    }
    gfx_flush();
    gl_held_target = rdp.color_image_address;
    if (pending_clear.color && pending_clear.target == gl_held_target) {
        gfx_rapi->clear(true, pending_clear.r, pending_clear.g, pending_clear.b, pending_clear.depth);
    } else {
        gfx_rapi->clear(true, 0.0f, 0.0f, 0.0f, true);
    }
    pending_clear.color = false;
    pending_clear.depth = false;
}

/* F3DEX G_MODIFYVTX: patch one attribute of an already transformed vertex.
 * This game's menu assets build their 2D sprites this way (fresh S/T per
 * vertex, sometimes screen positions), so it is load-bearing. */
static void gfx_sp_modify_vertex(uint16_t vtx, uint8_t where, uint32_t val) {
    struct LoadedVertex *d;
    if (vtx >= MAX_VERTICES) {
        return;
    }
    d = &rsp.loaded_vertices[vtx];
    switch (where) {
        case G_MWO_POINT_RGBA:
            d->color.r = (uint8_t)(val >> 24);
            d->color.g = (uint8_t)(val >> 16);
            d->color.b = (uint8_t)(val >> 8);
            d->color.a = (uint8_t)val;
            break;
        case G_MWO_POINT_ST: /* S10.5 texel coordinates, already scaled */
            d->u = (int16_t)(val >> 16);
            d->v = (int16_t)(val & 0xFFFF);
            break;
        case G_MWO_POINT_XYSCREEN: { /* S13.2 screen coordinates */
            float sx = (int16_t)(val >> 16) / 4.0f;
            float sy = (int16_t)(val & 0xFFFF) / 4.0f;
            float w = d->w != 0.0f ? d->w : 1.0f;
            d->x = gfx_adjust_x_for_aspect_ratio((sx / HALF_SCREEN_WIDTH - 1.0f) * w);
            d->y = (1.0f - sy / HALF_SCREEN_HEIGHT) * w;
            d->clip_rej = 0;
            if (d->x < -w) d->clip_rej |= 1;
            if (d->x > w) d->clip_rej |= 2;
            if (d->y < -w) d->clip_rej |= 4;
            if (d->y > w) d->clip_rej |= 8;
            break;
        }
        case G_MWO_POINT_ZSCREEN: { /* 16.16 screen depth (0..0x03ff.ffff) */
            float zs = (int32_t)val / 65536.0f / 1023.0f; /* 0..1 */
            float w = d->w != 0.0f ? d->w : 1.0f;
            d->z = (zs * 2.0f - 1.0f) * w;
            break;
        }
        default:
            gfx_unsupported("G_MODIFYVTX where", __LINE__);
            break;
    }
}

/* F3DEX G_CULLDL: if the bounding volume spanned by loaded vertices
 * [vstart, vend] is entirely outside the frustum, the rest of the display
 * list is skipped. Returns true when the caller should stop. */
static bool gfx_sp_cull_dl(uint16_t vstart, uint16_t vend) {
    uint8_t all = 0xFF;
    uint16_t i;
    if (vend >= MAX_VERTICES || vstart > vend) {
        return false;
    }
    for (i = vstart; i <= vend; i++) {
        all &= rsp.loaded_vertices[i].clip_rej;
    }
    return all != 0;
}

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};
    
    gfx_switch_target();
    
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        // The whole triangle lies outside the visible area
        return;
    }
    
    if ((rsp.geometry_mode & G_CULL_BOTH) != 0) {
        float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
        float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
        float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
        float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
        float cross = dx1 * dy2 - dy1 * dx2;
        
        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }
        
        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) return;
                break;
            case G_CULL_BACK:
                if (cross >= 0) return;
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                return;
        }
    }
    
    bool depth_test = (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }
    
    bool z_upd = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }
    
    bool zmode_decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }
    
    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }
    
    uint32_t cc_id = rdp.combine_mode;
    
    bool use_alpha = (rdp.other_mode_l & (G_BL_A_MEM << 18)) == 0;
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;
    
    if (texture_edge) {
        use_alpha = true;
    }
    
    if (use_alpha) cc_id |= SHADER_OPT_ALPHA;
    if (use_fog) cc_id |= SHADER_OPT_FOG;
    if (texture_edge) cc_id |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_id |= SHADER_OPT_NOISE;
    
    if (!use_alpha) {
        cc_id &= ~0xfff000;
    }
    
    struct ColorCombiner *comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram *prg = comb->prg;
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }
    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);
    
    for (int i = 0; i < 2; i++) {
        if (used_textures[i]) {
            if (rdp.textures_changed[i] || rendering_state.textures[i] == NULL) {
                if (rdp.loaded_texture[i].addr == NULL) {
                    continue; /* combiner samples a tile nothing was ever loaded into */
                }
                gfx_flush();
                import_texture(i);
                rdp.textures_changed[i] = false;
            }
            bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
            /* N64 clamping applies at the tile's edge while the mask wraps the
             * texture inside it: a 64x32 texture under a 128x256 tile repeats
             * 2x8 times before clamping. GL has one wrap mode, so when the
             * tile is larger than the masked texture, wrap (keep mirroring);
             * clamp only when the tile does not exceed the texture. Without
             * this, near-camera course polygons render as flat sheets of the
             * edge texel with the texture only along one border. */
            uint32_t cms_eff = rdp.texture_tile.cms, cmt_eff = rdp.texture_tile.cmt;
            {
                uint32_t tw = ((rdp.texture_tile.lrs - rdp.texture_tile.uls) >> 2) + 1;
                uint32_t th = ((rdp.texture_tile.lrt - rdp.texture_tile.ult) >> 2) + 1;
                if (rdp.texture_tile.masks != 0 && tw > (1u << rdp.texture_tile.masks)) cms_eff &= ~(uint32_t)G_TX_CLAMP;
                if (rdp.texture_tile.maskt != 0 && th > (1u << rdp.texture_tile.maskt)) cmt_eff &= ~(uint32_t)G_TX_CLAMP;
            }
            if (linear_filter != rendering_state.textures[i]->linear_filter || cms_eff != rendering_state.textures[i]->cms || cmt_eff != rendering_state.textures[i]->cmt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, cms_eff, cmt_eff);
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = (uint8_t)cms_eff;
                rendering_state.textures[i]->cmt = (uint8_t)cmt_eff;
            }
        }
    }
    
    bool use_texture = used_textures[0] || used_textures[1];
    // Normalise texel coordinates by the size of the texture actually uploaded
    // (one TMEM line per row), not by the tile size: tiles are often wider than
    // the texture (mirrored/wrapped backgrounds via masks) or narrower (a
    // 24-texel sprite in 32-texel lines).
    uint32_t tex_bits = rdp.texture_tile.siz == G_IM_SIZ_4b ? 4 : rdp.texture_tile.siz == G_IM_SIZ_8b ? 8 : rdp.texture_tile.siz == G_IM_SIZ_16b ? 16 : 32;
    uint32_t tex_width = rdp.texture_tile.line_size_bytes != 0 ? rdp.texture_tile.line_size_bytes * 8 / tex_bits : 1;
    uint32_t tex_height = rdp.texture_tile.line_size_bytes != 0 ? rdp.loaded_texture[0].size_bytes / rdp.texture_tile.line_size_bytes : 1;
    if (rdp.texture_tile.masks != 0 && (1u << rdp.texture_tile.masks) < tex_width) tex_width = 1u << rdp.texture_tile.masks;
    if (rdp.texture_tile.maskt != 0 && (1u << rdp.texture_tile.maskt) < tex_height) tex_height = 1u << rdp.texture_tile.maskt;
    if (tex_width == 0) tex_width = 1;
    if (tex_height == 0) tex_height = 1;
    
    bool z_is_from_0_to_1 = gfx_rapi->z_is_from_0_to_1();

    if (sbk_bigtri_area > 0 && sbk_bigtri_left > 0 &&
        v_arr[0]->w > 0.0f && v_arr[1]->w > 0.0f && v_arr[2]->w > 0.0f) {
        float sx[3], sy[3];
        float area;
        for (int k = 0; k < 3; k++) {
            sx[k] = v_arr[k]->x / v_arr[k]->w * HALF_SCREEN_WIDTH + HALF_SCREEN_WIDTH;
            sy[k] = -v_arr[k]->y / v_arr[k]->w * HALF_SCREEN_HEIGHT + HALF_SCREEN_HEIGHT;
        }
        area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (area < 0) area = -area;
        area *= 0.5f;
        if (area > (float)sbk_bigtri_area) {
            const float (*mv)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
            sbk_bigtri_left--;
            printf("sbk-bigtri: area=%.0f cc=%08x tex=%ux%u@%p fmt=%u siz=%u pal=%u om_l=%08x om_h=%08x gm=%08x mtx=%p(p%02x) vtx=%p(n%u v0%u) stk=%u",
                   area, (unsigned)cc_id, tex_width, tex_height, (const void *)rdp.loaded_texture[0].addr,
                   rdp.texture_tile.fmt, rdp.texture_tile.siz, rdp.texture_tile.palette,
                   (unsigned)rdp.other_mode_l, (unsigned)rdp.other_mode_h, (unsigned)rsp.geometry_mode,
                   sbk_last_mtx_addr, sbk_last_mtx_params, sbk_last_vtx_addr, sbk_last_vtx_n, sbk_last_vtx_v0,
                   rsp.modelview_matrix_stack_size);
            for (int k = 0; k < 3; k++) {
                printf(" v%d=(%.1f,%.1f,w%.1f uv %d,%d rgba %02x%02x%02x%02x)", k, sx[k], sy[k], v_arr[k]->w,
                       (int)v_arr[k]->u, (int)v_arr[k]->v,
                       v_arr[k]->color.r, v_arr[k]->color.g, v_arr[k]->color.b, v_arr[k]->color.a);
            }
            printf(" mv=[");
            for (int r = 0; r < 4; r++)
                printf("%s%.3f,%.3f,%.3f,%.3f", r ? " | " : "", mv[r][0], mv[r][1], mv[r][2], mv[r][3]);
            printf("]\n");
        }
    }

    if (sbk_tex_dump_left > 0 || sbk_tri_dump_all) {
        extern int sbk_tri_drawn;
        int *drawnp = &sbk_tri_drawn;
        if ((*drawnp)++ < 40000) {
            printf("sbk-tri: cc=%08x tex=%ux%u@%p tile=(%u,%u)-(%u,%u) fmt=%u siz=%u pal=%u om_l=%08x om_h=%08x",
                   (unsigned)cc_id, tex_width, tex_height, (const void *)rdp.loaded_texture[0].addr, rdp.texture_tile.uls >> 2, rdp.texture_tile.ult >> 2,
                   rdp.texture_tile.lrs >> 2, rdp.texture_tile.lrt >> 2, rdp.texture_tile.fmt, rdp.texture_tile.siz,
                   rdp.texture_tile.palette, (unsigned)rdp.other_mode_l, (unsigned)rdp.other_mode_h);
            for (int k = 0; k < 3; k++) {
                printf(" v%d=(%.1f,%.1f,w%.1f uv %d,%d rgba %02x%02x%02x%02x)", k,
                       v_arr[k]->w != 0 ? v_arr[k]->x / v_arr[k]->w * HALF_SCREEN_WIDTH + HALF_SCREEN_WIDTH : -1.0f,
                       v_arr[k]->w != 0 ? -v_arr[k]->y / v_arr[k]->w * HALF_SCREEN_HEIGHT + HALF_SCREEN_HEIGHT : -1.0f,
                       v_arr[k]->w, (int)v_arr[k]->u, (int)v_arr[k]->v,
                       v_arr[k]->color.r, v_arr[k]->color.g, v_arr[k]->color.b, v_arr[k]->color.a);
            }
            printf(" prim=%02x%02x%02x%02x env=%02x%02x%02x%02x mtx=%p(p%02x) vtx=%p(n%u v0%u) gm=%08x\n",
                   rdp.prim_color.r, rdp.prim_color.g, rdp.prim_color.b, rdp.prim_color.a,
                   rdp.env_color.r, rdp.env_color.g, rdp.env_color.b, rdp.env_color.a,
                   sbk_last_mtx_addr, sbk_last_mtx_params, sbk_last_vtx_addr, sbk_last_vtx_n, sbk_last_vtx_v0,
                   (unsigned)rsp.geometry_mode);
        }
    }
    
    for (int i = 0; i < 3; i++) {
        float z = v_arr[i]->z, w = v_arr[i]->w;
        if (z_is_from_0_to_1) {
            z = (z + w) / 2.0f;
        }
        buf_vbo[buf_vbo_len++] = v_arr[i]->x;
        buf_vbo[buf_vbo_len++] = v_arr[i]->y;
        buf_vbo[buf_vbo_len++] = z;
        buf_vbo[buf_vbo_len++] = w;
        
        if (use_texture) {
            float u = (v_arr[i]->u - rdp.texture_tile.uls * 8) / 32.0f;
            float v = (v_arr[i]->v - rdp.texture_tile.ult * 8) / 32.0f;
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            buf_vbo[buf_vbo_len++] = u / tex_width;
            buf_vbo[buf_vbo_len++] = v / tex_height;
        }
        
        if (use_fog) {
            buf_vbo[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = v_arr[i]->color.a / 255.0f; // fog factor (not alpha)
        }
        
        for (int j = 0; j < num_inputs; j++) {
            struct RGBA *color;
            struct RGBA tmp;
            for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                switch (comb->shader_input_mapping[k][j]) {
                    case CC_PRIM:
                        color = &rdp.prim_color;
                        break;
                    case CC_SHADE:
                        color = &v_arr[i]->color;
                        break;
                    case CC_ENV:
                        color = &rdp.env_color;
                        break;
                    case CC_LOD:
                    {
                        float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                        if (distance_frac < 0.0f) distance_frac = 0.0f;
                        if (distance_frac > 1.0f) distance_frac = 1.0f;
                        tmp.r = tmp.g = tmp.b = tmp.a = distance_frac * 255.0f;
                        color = &tmp;
                        break;
                    }
                    default:
                        memset(&tmp, 0, sizeof(tmp));
                        color = &tmp;
                        break;
                }
                if (k == 0) {
                    buf_vbo[buf_vbo_len++] = color->r / 255.0f;
                    buf_vbo[buf_vbo_len++] = color->g / 255.0f;
                    buf_vbo[buf_vbo_len++] = color->b / 255.0f;
                } else {
                    if (use_fog && color == &v_arr[i]->color) {
                        // Shade alpha is 100% for fog
                        buf_vbo[buf_vbo_len++] = 1.0f;
                    } else {
                        buf_vbo[buf_vbo_len++] = color->a / 255.0f;
                    }
                }
            }
        }
        /*struct RGBA *color = &v_arr[i]->color;
        buf_vbo[buf_vbo_len++] = color->r / 255.0f;
        buf_vbo[buf_vbo_len++] = color->g / 255.0f;
        buf_vbo[buf_vbo_len++] = color->b / 255.0f;
        buf_vbo[buf_vbo_len++] = color->a / 255.0f;*/
    }
    if (++buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] / 4.0f) + height / 2.0f);
    
    width *= RATIO_X;
    height *= RATIO_Y;
    x *= RATIO_X;
    y *= RATIO_Y;
    
    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;
    {
        static int logged;
        extern int sbk_trace;
        if (sbk_trace && logged < 4) {
            logged++;
            printf("sbk-vp: vscale=(%d,%d) vtrans=(%d,%d) -> x=%g y=%g w=%g h=%g (window %ux%u)\n",
                   viewport->vscale[0], viewport->vscale[1], viewport->vtrans[0], viewport->vtrans[1],
                   x, y, width, height, gfx_current_dimensions.width, gfx_current_dimensions.height);
        }
    }
    
    rdp.viewport_or_scissor_changed = true;
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t *) data);
            break;
#if 0
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            //rsp.lights_changed = 1;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            }
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
#endif
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT:
            segment_table[(offset / 4) & 0xF] = data;
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x = ulx / 4.0f * RATIO_X;
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
    float width = (lrx - ulx) / 4.0f * RATIO_X;
    float height = (lry - uly) / 4.0f * RATIO_Y;
    
    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;
    
    rdp.viewport_or_scissor_changed = true;
}

/* Every texture this game draws lives in RDRAM (its heaps are inside the 4 MB
 * window); anything else is a garbage descriptor (e.g. renderCourseTextureMarkers
 * reading past an asset table) that hardware would load harmlessly and never
 * show. Skip those instead of dereferencing them. */
static bool sbk_texture_addr_ok(const void *addr) {
    uintptr_t a = (uintptr_t)addr;
    return a >= 0x80000000u && a < 0x80400000u;
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, const void* addr) {
    if (!sbk_texture_addr_ok(addr)) {
        static int warned;
        extern unsigned sbk_task_count;
        if (warned++ < 8) {
            printf("sbk-gfx: task %u: SETTIMG outside RDRAM (%p), texture skipped\n", sbk_task_count, addr);
        }
    }
    rdp.texture_to_load.addr = addr;
    rdp.texture_to_load.siz = size;
    rdp.texture_to_load.width = width + 1; /* G_SETTIMG stores width - 1 */
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.palette = (uint8_t)(palette & 0xF); // upper 4 bits of the colour index in 4b mode
        rdp.texture_tile.masks = (uint8_t)masks;
        rdp.texture_tile.maskt = (uint8_t)maskt;
        rdp.texture_tile.fmt = fmt;
        rdp.texture_tile.siz = siz;
        rdp.texture_tile.cms = cms;
        rdp.texture_tile.cmt = cmt;
        rdp.texture_tile.line_size_bytes = line * 8;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    
    if (tile == G_TX_LOADTILE) {
        rdp.texture_to_load.tile_number = tmem / 256;
        rdp.texture_to_load.tmem = tmem;
        rdp.texture_to_load.line_bytes = line * 8;
    }
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.uls = uls;
        rdp.texture_tile.ult = ult;
        rdp.texture_tile.lrs = lrs;
        rdp.texture_tile.lrt = lrt;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
}

static void gfx_dp_load_tlut(uint8_t tile, uint32_t high_index) {
    if (!sbk_texture_addr_ok(rdp.texture_to_load.addr)) return; /* garbage SETTIMG: keep the previous texture */
    // TMEM's upper half holds 256 TLUT entries in 16 slots of 16 (each entry
    // occupies 8 bytes there, so a slot is 16 tmem words): gDPLoadTLUT_pal16
    // targets slot (tmem - 256) / 16, gDPLoadTLUT_pal256 fills all of them.
    uint32_t slot = rdp.texture_to_load.tmem >= 256 ? (rdp.texture_to_load.tmem - 256) / 16 : 0;
    uint32_t entries = high_index + 1;
    uint32_t k;
    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    for (k = 0; k * 16 < entries && slot + k < 16; k++) {
        rdp.tlut[slot + k] = rdp.texture_to_load.addr + k * 32;
    }
    if (slot == 0) {
        rdp.palette = rdp.texture_to_load.addr;
    }
}

static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    if (!sbk_texture_addr_ok(rdp.texture_to_load.addr)) return; /* garbage SETTIMG: keep the previous texture */
    if (tile == 1) return;
    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);
    
    // The lrs field rather seems to be number of pixels to load
    uint32_t word_size_shift;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0; // Or -1? It's unused in SM64 anyway.
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }
    uint32_t size_bytes = (lrs + 1) << word_size_shift;
    if (size_bytes > 4096) {
        /* TMEM is 4 KB; hardware wraps the load. Keep the first 4 KB. */
        static int warned;
        extern int sbk_gfx_bad_dl;
        extern unsigned sbk_task_count;
        sbk_gfx_bad_dl = 1;
        if (warned++ < 8) {
            printf("sbk-gfx: task %u: LOADBLOCK %u bytes (siz %u, lrs %u, width %u, tile %u) at %p, clamped\n",
                   sbk_task_count, size_bytes, rdp.texture_to_load.siz, lrs, rdp.texture_to_load.width, tile,
                   (void *)rdp.texture_to_load.addr);
        }
        size_bytes = 4096;
    }
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].key = NULL;
    
    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
}

/* LOADTILE copies a rectangle of the SETTIMG image into TMEM, one padded
 * 8-byte-aligned line per row. Emulate that so sub-rectangles of sprite
 * atlases (the menu renderer's gDPLoadTextureTile_4b) come out contiguous. */
static uint8_t tmem_scratch[2][4096];

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    if (!sbk_texture_addr_ok(rdp.texture_to_load.addr)) return; /* garbage SETTIMG: keep the previous texture */
    if (tile == 1) return;
    SUPPORT_CHECK(tile == G_TX_LOADTILE);

    uint32_t x0 = uls >> G_TEXTURE_IMAGE_FRAC, y0 = ult >> G_TEXTURE_IMAGE_FRAC;
    uint32_t w = (lrs >> G_TEXTURE_IMAGE_FRAC) - x0 + 1;
    uint32_t h = (lrt >> G_TEXTURE_IMAGE_FRAC) - y0 + 1;
    uint32_t bits;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b: bits = 4; break;
        case G_IM_SIZ_8b: bits = 8; break;
        case G_IM_SIZ_16b: bits = 16; break;
        default: bits = 32; break;
    }
    uint32_t row_bytes = (w * bits + 7) / 8;
    /* Rows land in TMEM at the load tile's line stride. Games routinely pass
     * inclusive coordinates one texel too far (width instead of width - 1);
     * on hardware the overrun byte is overwritten by the next row and the
     * extra row never sampled, so trim both. */
    uint32_t line_bytes = rdp.texture_to_load.line_bytes != 0 ? rdp.texture_to_load.line_bytes : ((row_bytes + 7) & ~7u);
    uint32_t stride = (rdp.texture_to_load.width * bits + 7) / 8;
    uint32_t size_bytes;
    int tn = rdp.texture_to_load.tile_number & 1;
    const uint8_t *src = rdp.texture_to_load.addr;

    if (row_bytes > line_bytes) {
        row_bytes = line_bytes;
    }
    if (h > 2 && ((h - 1) & (h - 2)) == 0) {
        h -= 1; /* 65 -> 64, 33 -> 32, 5 -> 4: the inclusive overrun row (the title fade's 16x4 dither is passed as (0,0)-(16,4)) */
    }
    size_bytes = line_bytes * h;

    if (size_bytes > sizeof(tmem_scratch[0])) {
        gfx_unsupported("LOADTILE larger than TMEM", __LINE__);
        size_bytes = sizeof(tmem_scratch[0]);
        h = size_bytes / line_bytes;
    }
    if (bits == 4 && (x0 & 1)) {
        gfx_unsupported("4-bit LOADTILE at an odd texel", __LINE__);
    }
    src += y0 * stride + (x0 * bits) / 8;
    for (uint32_t y = 0; y < h; y++) {
        memcpy(&tmem_scratch[tn][y * line_bytes], src + y * stride, row_bytes);
        if (line_bytes > row_bytes) {
            memset(&tmem_scratch[tn][y * line_bytes + row_bytes], 0, line_bytes - row_bytes);
        }
    }
    rdp.loaded_texture[tn].addr = tmem_scratch[tn];
    rdp.loaded_texture[tn].size_bytes = size_bytes;
    rdp.loaded_texture[tn].key = src; /* the rectangle's origin in the source image */
    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;

    rdp.textures_changed[tn] = true;
}


static uint8_t color_comb_component(uint32_t v) {
    switch (v) {
        case G_CCMUX_TEXEL0:
            return CC_TEXEL0;
        case G_CCMUX_TEXEL1:
            return CC_TEXEL1;
        case G_CCMUX_PRIMITIVE:
            return CC_PRIM;
        case G_CCMUX_SHADE:
            return CC_SHADE;
        case G_CCMUX_ENVIRONMENT:
            return CC_ENV;
        case G_CCMUX_TEXEL0_ALPHA:
            return CC_TEXEL0A;
        case G_CCMUX_LOD_FRACTION:
            return CC_LOD;
        default:
            return CC_0;
    }
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return color_comb_component(a) |
           (color_comb_component(b) << 3) |
           (color_comb_component(c) << 6) |
           (color_comb_component(d) << 9);
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha) {
    rdp.combine_mode = rgb | (alpha << 12);
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }
    
    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;
    
    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    
    ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
    lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);
    
    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    
    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;
    
    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;
    
    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;
    
    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;
    
    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;
    
    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;
    
    gfx_sp_tri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3);
    gfx_sp_tri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3);
    
    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;
    
    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    uint32_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;
        
        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), color_comb(0, 0, 0, G_ACMUX_TEXEL0));
        
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;
    
    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }
    
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    bool full_screen = ulx <= 0 && uly <= 0 && lrx >= (SCREEN_WIDTH - 1) * 4 && lry >= (SCREEN_HEIGHT - 1) * 4;
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Z buffer clear: applied when drawing next switches framebuffers.
        pending_clear.depth = true;
        return;
    }
    if (full_screen && rdp.color_image_address != gl_held_target) {
        pending_clear.target = rdp.color_image_address;
        pending_clear.color = true;
        pending_clear.r = rdp.fill_color.r / 255.0f;
        pending_clear.g = rdp.fill_color.g / 255.0f;
        pending_clear.b = rdp.fill_color.b / 255.0f;
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));
    
    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }
    
    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        struct LoadedVertex* v = &rsp.loaded_vertices[i];
        v->color = rdp.fill_color;
    }
    
    uint32_t saved_combine_mode = rdp.combine_mode;
    gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), color_comb(0, 0, 0, G_ACMUX_SHADE));
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
}

/* Segmented (0x0S......) and KSEG0/physical RDRAM addresses all resolve to the
 * emulated RDRAM mapped at 0x80000000. */
static inline void *seg_addr(uintptr_t w1) {
    uint32_t a = (uint32_t)w1;
    if (a < 0x10000000u) {
        a = segment_table[(a >> 24) & 0xF] + (a & 0x00FFFFFFu);
    }
    return sbk_phys_to_host(a);
}

/* Debug: forget every cached texture so the next frame re-decodes them all. */
void gfx_debug_flush_texture_cache(void) {
    memset(gfx_texture_cache.hashmap, 0, sizeof(gfx_texture_cache.hashmap));
    gfx_texture_cache.pool_pos = 0;
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    rendering_state.textures[0] = rendering_state.textures[1] = NULL;
}

/* For the display-list dumper: resolve with the current segment table. */
void *gfx_debug_seg_addr(uint32_t w1) {
    return seg_addr(w1);
}
void gfx_debug_set_segment(uint32_t seg, uint32_t base) {
    segment_table[seg & 0xF] = base;
}

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

static void gfx_run_dl(Gfx* cmd) {
    int dummy = 0;
    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
        
        switch (opcode) {
            // RSP commands:
            case G_MTX:
#ifdef F3DEX_GBI_2
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr(cmd->words.w1));
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_POPMTX:
#ifdef F3DEX_GBI_2
                gfx_sp_pop_matrix(cmd->words.w1 / 64);
#else
                gfx_sp_pop_matrix(1);
#endif
                break;
            case G_MOVEMEM:
#ifdef F3DEX_GBI_2
                gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(cmd->words.w1));
#else
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_MOVEWORD:
#ifdef F3DEX_GBI_2
                gfx_sp_moveword(C0(16, 8), C0(0, 16), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
#endif
                break;
            case G_VTX:
#ifdef F3DEX_GBI_2
                gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_vertex(C0(10, 6), C0(16, 8) / 2, seg_addr(cmd->words.w1));
#else
                gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), seg_addr(cmd->words.w1));
#endif
                break;
            case G_DL:
                if (C0(16, 1) == 0) {
                    // Push return address
                    gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
                } else {
                    cmd = (Gfx *)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
#ifdef F3DEX_GBI_2
            case G_GEOMETRYMODE:
                gfx_sp_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
#else
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
#endif
            case (uint8_t)G_TRI1:
#ifdef F3DEX_GBI_2
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
#else
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10);
#endif
                break;
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
            case (uint8_t)G_MODIFYVTX:
                gfx_sp_modify_vertex((uint16_t)(C0(0, 16) / 2), (uint8_t)C0(16, 8), cmd->words.w1);
                break;
            case (uint8_t)G_CULLDL:
                if (gfx_sp_cull_dl((uint16_t)(C0(0, 16) / 2), (uint16_t)(C1(0, 16) / 2))) {
                    return;
                }
                break;
            case (uint8_t)G_BRANCH_Z:
                gfx_unsupported("G_BRANCH_Z", __LINE__);
                break;
            case (uint8_t)G_TRI2:
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                break;
#endif
            case (uint8_t)G_SETOTHERMODE_L:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);
#else
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;
#ifdef F3D_OLD
            case (uint8_t)G_RDPHALF_2:
#else
            case (uint8_t)G_RDPHALF_1:
#endif
                switch (rsp.saved_opcode) {
                    case G_TEXRECT:
                    case G_TEXRECTFLIP:
#ifdef F3DEX_GBI_2E
                        rsp.saved_ulx = (int32_t)(C0(0, 24) << 8) >> 8;
#endif
                        rsp.saved_uls = (uint16_t)C1(16, 16);
                        rsp.saved_ult = (uint16_t)C1(0, 16);
                        break;
#ifdef F3DEX_GBI_2E
                    case G_FILLRECT:
                    {
                        int32_t ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                        int32_t uly = (int32_t)(C1(0, 24) << 8) >> 8;
                        gfx_dp_fill_rectangle(ulx, uly, rsp.saved_lrx, rsp.saved_lry);
                        rsp.saved_opcode = G_NOOP;
                        break;
                    }
#endif
                }
                break;
#ifdef F3D_OLD
            case (uint8_t)G_RDPHALF_CONT:
#else
            case (uint8_t)G_RDPHALF_2:
#endif
                switch (rsp.saved_opcode) {
                    case G_TEXRECT:
                    case G_TEXRECTFLIP:
                    {
                        uint8_t tile = rsp.saved_tile;
                        int32_t ulx = rsp.saved_ulx, lrx = rsp.saved_lrx, lry = rsp.saved_lry;
                        uint16_t uls = rsp.saved_uls, ult = rsp.saved_ult;
#ifdef F3DEX_GBI_2E
                        int32_t uly = (int32_t)(C0(0, 24) << 8) >> 8;
#else
                        int32_t uly = rsp.saved_uly;
#endif
                        uint16_t dsdx = (uint16_t)C1(16, 16);
                        uint16_t dtdy = (uint16_t)C1(0, 16);
                        gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, rsp.saved_opcode == G_TEXRECTFLIP);
                        rsp.saved_opcode = G_NOOP;
                        break;
                    }
                }
            
            // RDP Commands:
            case G_SETTIMG:
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 12), seg_addr(cmd->words.w1));
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
                gfx_dp_set_combine_mode(
                    color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                    color_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)));
                    /*color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)),
                    color_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));*/
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                rsp.saved_opcode = opcode;
#ifdef F3DEX_GBI_2E
                rsp.saved_lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                rsp.saved_lry = (int32_t)(C1(0, 24) << 8) >> 8;
                rsp.saved_tile = (int32_t)C1(24, 3);
#else
                rsp.saved_lrx = C0(12, 12);
                rsp.saved_lry = C0(0, 12);
                rsp.saved_tile = C1(24, 3);
                rsp.saved_ulx = C1(12, 12);
                rsp.saved_uly = C1(0, 12);
#endif
                break;
            }
            case G_FILLRECT:
#ifdef F3DEX_GBI_2E
            {
                rsp.saved_opcode = G_FILLRECT;
                rsp.saved_lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                rsp.saved_lry = (int32_t)(C1(0, 24) << 8) >> 8;
                break;
            }
#else
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
#endif
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
        }
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
}

void gfx_get_dimensions(uint32_t *width, uint32_t *height) {
    gfx_wapi->get_dimensions(width, height);
}

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen) {
    gfx_wapi = wapi;
    gfx_rapi = rapi;
    gfx_wapi->init(game_name, start_in_fullscreen);
    gfx_rapi->init();
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

static bool frame_open;

void gfx_run(Gfx *commands) {
    gfx_sp_reset();
    if (!frame_open) {
        gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
        if (gfx_current_dimensions.height == 0) {
            gfx_current_dimensions.height = 1;
        }
        gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
        gfx_rapi->start_frame();
        frame_open = true;
        /* re-apply viewport and scissor on the first draw of every frame: a
         * fullscreen context can have them reset behind our back */
        memset(&rendering_state.viewport, 0xFF, sizeof(rendering_state.viewport));
        memset(&rendering_state.scissor, 0xFF, sizeof(rendering_state.scissor));
    }
    gfx_run_dl(commands);
    gfx_flush();
}

unsigned sbk_stat_present;

void gfx_present(void) {
    extern int sbk_headless;
    if (sbk_headless) { sbk_stat_present++; return; }
    if (!frame_open) {
        return;
    }
    sbk_stat_present++;
    SBK_PERF_TIMED(SBK_PERF_ENDFRAME, gfx_rapi->end_frame());
    SBK_PERF_TIMED(SBK_PERF_FINISH, gfx_rapi->finish_render());
    SBK_PERF_TIMED(SBK_PERF_SWAP, gfx_wapi->swap_buffers());
    frame_open = false;
    gl_held_target = NULL; /* the back buffer is undefined after a swap */
}

void gfx_set_window_title(const char *title) {
    if (gfx_wapi != NULL && gfx_wapi->set_title != NULL) {
        gfx_wapi->set_title(title);
    }
}

void gfx_handle_events(void) {
    gfx_wapi->handle_events();
}
