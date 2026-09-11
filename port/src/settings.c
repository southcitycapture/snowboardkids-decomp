/* Settings file: see settings.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "settings.h"

struct SbkSettings sbk_settings;
int sbk_settings_scripted;
int sbk_settings_forced;
int sbk_settings_loaded;

/* --- the game list ------------------------------------------------------ */

static struct SbkGameEntry games[] = {
    { "sbk1", "Snowboard Kids",   "snowboardkids.z64",  1 },
    { "sbk2", "Snowboard Kids 2", "snowboardkids2.z64", 0 },
};

int sbk_game_count(void) { return (int)(sizeof(games) / sizeof(games[0])); }
const struct SbkGameEntry *sbk_game_at(int i) {
    if (i < 0 || i >= sbk_game_count()) return NULL;
    return &games[i];
}
int sbk_game_index(const char *id) {
    int i;
    for (i = 0; i < sbk_game_count(); i++) {
        if (strcmp(games[i].id, id) == 0) return i;
    }
    return 0;
}
/* A second game becomes selectable the moment its ROM turns up next to the
 * first one; the launcher needs no change for it. */
void sbk_games_probe(const char *rom_dir) {
    int i;
    char path[1024];
    for (i = 1; i < sbk_game_count(); i++) {
        FILE *f;
        snprintf(path, sizeof(path), "%s/%s", rom_dir != NULL ? rom_dir : ".", games[i].rom);
        f = fopen(path, "rb");
        if (f != NULL) { fclose(f); games[i].installed = 1; }
    }
}

/* --- paths -------------------------------------------------------------- */

const char *sbk_settings_dir(void) {
    static char dir[1024];
    const char *home;
    if (dir[0] != '\0') return dir;
    home = getenv("HOME");
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/SnowboardKids",
             home != NULL ? home : ".");
    return dir;
}

const char *sbk_settings_path(void) {
    static char path[1100];
    if (path[0] == '\0') snprintf(path, sizeof(path), "%s/settings.txt", sbk_settings_dir());
    return path;
}

static void ensure_dir(void) {
    const char *dir = sbk_settings_dir();
    struct stat st;
    if (stat(dir, &st) == 0) return;
    /* the two parents exist on any Mac account; only the last level is ours */
    mkdir(dir, 0755);
}

/* --- defaults and modes ------------------------------------------------- */

void sbk_settings_defaults(void) {
    memset(&sbk_settings, 0, sizeof(sbk_settings));
    strcpy(sbk_settings.game, "sbk1");
    sbk_settings.mode = SBK_MODE_ORIGINAL;
    sbk_settings.draw_distance = 1;
    sbk_settings.resolution = SBK_RES_NATIVE;
    sbk_settings.filter = SBK_FILTER_NONE;
    sbk_settings.widescreen = 0;
    sbk_settings.fullscreen = 0;
    sbk_settings.vsync = 1;
    sbk_settings.volume = 100;
    sbk_settings.launcher = 1;
    sbk_settings.perf = 0;
}

void sbk_settings_apply_mode(int mode) {
    sbk_settings.mode = mode;
    if (mode == SBK_MODE_ENHANCED) {
        sbk_settings.draw_distance = 4;
        sbk_settings.resolution = SBK_RES_NATIVE;
        sbk_settings.filter = SBK_FILTER_SMOOTH;
        sbk_settings.widescreen = 0;
    } else if (mode == SBK_MODE_ORIGINAL) {
        sbk_settings.draw_distance = 1;
        sbk_settings.resolution = SBK_RES_N64;
        sbk_settings.filter = SBK_FILTER_NONE;
        sbk_settings.widescreen = 0;
    }
}

/* Which mode the individual settings currently spell, so that changing one
 * knob on the Options page moves the Mode line to CUSTOM rather than lying. */
int sbk_settings_derive_mode(void) {
    if (sbk_settings.draw_distance == 4 && sbk_settings.resolution == SBK_RES_NATIVE &&
        sbk_settings.filter == SBK_FILTER_SMOOTH && !sbk_settings.widescreen) return SBK_MODE_ENHANCED;
    if (sbk_settings.draw_distance == 1 && sbk_settings.resolution == SBK_RES_N64 &&
        sbk_settings.filter == SBK_FILTER_NONE && !sbk_settings.widescreen) return SBK_MODE_ORIGINAL;
    return SBK_MODE_CUSTOM;
}

/* --- load / save -------------------------------------------------------- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int name_index(const char *v, const char *const *names, int n, int fallback) {
    int i;
    for (i = 0; i < n; i++) if (strcmp(v, names[i]) == 0) return i;
    return fallback;
}

static const char *const mode_names[] = { "original", "enhanced", "custom" };
static const char *const res_names[] = { "native", "n64", "2x" };
static const char *const filter_names[] = { "none", "scanlines", "grille", "smooth" };

const char *sbk_settings_mode_name(int v) { return mode_names[clampi(v, 0, 2)]; }
const char *sbk_settings_res_name(int v) { return res_names[clampi(v, 0, 2)]; }
const char *sbk_settings_filter_name(int v) { return filter_names[clampi(v, 0, 3)]; }

void sbk_settings_load(void) {
    FILE *f;
    char line[256];
    sbk_settings_defaults();
    if (sbk_settings_scripted) return;   /* determinism: goldens see the defaults */
    f = fopen(sbk_settings_path(), "r");
    if (f == NULL) return;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq, *key, *val, *p;
        if (line[0] == '#' || line[0] == '\n') continue;
        eq = strchr(line, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        key = line;
        val = eq + 1;
        for (p = val; *p != '\0'; p++) if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
        while (*key == ' ') key++;
        while (*val == ' ') val++;
        if (strcmp(key, "game") == 0) {
            strncpy(sbk_settings.game, val, sizeof(sbk_settings.game) - 1);
            sbk_settings.game[sizeof(sbk_settings.game) - 1] = '\0';
        }
        else if (strcmp(key, "mode") == 0) sbk_settings.mode = name_index(val, mode_names, 3, SBK_MODE_ORIGINAL);
        else if (strcmp(key, "draw_distance") == 0) sbk_settings.draw_distance = clampi(atoi(val), 1, 4);
        else if (strcmp(key, "resolution") == 0) sbk_settings.resolution = name_index(val, res_names, 3, SBK_RES_NATIVE);
        else if (strcmp(key, "filter") == 0) sbk_settings.filter = name_index(val, filter_names, 4, SBK_FILTER_NONE);
        else if (strcmp(key, "widescreen") == 0) sbk_settings.widescreen = atoi(val) != 0;
        else if (strcmp(key, "fullscreen") == 0) sbk_settings.fullscreen = atoi(val) != 0;
        else if (strcmp(key, "vsync") == 0) sbk_settings.vsync = atoi(val) != 0;
        else if (strcmp(key, "volume") == 0) sbk_settings.volume = clampi(atoi(val), 0, 100);
        else if (strcmp(key, "launcher") == 0) sbk_settings.launcher = atoi(val) != 0;
        else if (strcmp(key, "perf") == 0) sbk_settings.perf = atoi(val) != 0;
    }
    fclose(f);
    sbk_settings_loaded = 1;
    printf("sbk: settings from %s\n", sbk_settings_path());
}

void sbk_settings_save(void) {
    FILE *f;
    if (sbk_settings_scripted) return;
    ensure_dir();
    f = fopen(sbk_settings_path(), "w");
    if (f == NULL) {
        fprintf(stderr, "sbk: cannot write %s\n", sbk_settings_path());
        return;
    }
    fprintf(f, "# Snowboard Kids (Power Mac G4 port) settings\n");
    fprintf(f, "game=%s\n", sbk_settings.game);
    fprintf(f, "mode=%s\n", sbk_settings_mode_name(sbk_settings.mode));
    fprintf(f, "draw_distance=%d\n", sbk_settings.draw_distance);
    fprintf(f, "resolution=%s\n", sbk_settings_res_name(sbk_settings.resolution));
    fprintf(f, "filter=%s\n", sbk_settings_filter_name(sbk_settings.filter));
    fprintf(f, "widescreen=%d\n", sbk_settings.widescreen);
    fprintf(f, "fullscreen=%d\n", sbk_settings.fullscreen);
    fprintf(f, "vsync=%d\n", sbk_settings.vsync);
    fprintf(f, "volume=%d\n", sbk_settings.volume);
    fprintf(f, "launcher=%d\n", sbk_settings.launcher);
    fprintf(f, "perf=%d\n", sbk_settings.perf);
    fclose(f);
}

/* --- pushing the values into the running port --------------------------- */

extern float sbk_far_scale;
extern int sbk_wide_output;
extern int sbk_perf_enabled;
void sbk_audio_out_set_volume(int percent);
void gfx_gl13_set_render_scale(int mode);      /* gfx_gl13.c */
void gfx_gl13_set_filter(int filter);
void sbk_gfx_set_vsync(int on);                /* gfx_sdl_gl13.c */
void sbk_gfx_refresh_output_rect(void);

void sbk_settings_apply(void) {
    sbk_far_scale = (float)sbk_settings.draw_distance;
    if (sbk_far_scale < 0.25f) sbk_far_scale = 0.25f;
    sbk_perf_enabled = sbk_settings.perf;
    sbk_audio_out_set_volume(sbk_settings.volume);
    if (sbk_settings_scripted && !sbk_settings_forced) {
        /* No post-processing at all under a script: a golden replay must see
         * exactly the pixels it was recorded against. */
        gfx_gl13_set_render_scale(SBK_RES_NATIVE);
        gfx_gl13_set_filter(SBK_FILTER_NONE);
        return;
    }
    if (sbk_wide_output != sbk_settings.widescreen) {
        sbk_wide_output = sbk_settings.widescreen;
        sbk_gfx_refresh_output_rect();
    }
    gfx_gl13_set_render_scale(sbk_settings.resolution);
    gfx_gl13_set_filter(sbk_settings.filter);
    sbk_gfx_set_vsync(sbk_settings.vsync);
}
