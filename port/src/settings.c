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

static const char *const sbk1_bundles[] = { "Snowboard Kids.app", "SnowboardKids.app", NULL };
static const char *const sbk2_bundles[] = { "Snowboard Kids 2.app", "SnowboardKids2.app", NULL };

/* `self` is the one this executable IS: it is always installed, and the other
 * row lights up only when its bundle is found. The sequel's copy of this file
 * differs in exactly these two flags. */
static struct SbkGameEntry games[] = {
    { "sbk1", "Snowboard Kids",   "snowboardkids.z64",  sbk1_bundles, 1, 1, { 0 } },
    { "sbk2", "Snowboard Kids 2", "snowboardkids2.z64", sbk2_bundles, 0, 0, { 0 } },
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
int sbk_game_self_index(void) {
    int i;
    for (i = 0; i < sbk_game_count(); i++) if (games[i].self) return i;
    return 0;
}

/* --- finding the other bundle ------------------------------------------- */

static int is_exec(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111) != 0;
}

/* .../Foo.app/Contents/MacOS/isle -> .../ (the directory the bundle sits in).
 * Returns 0 when argv0 does not look like it came out of a bundle. */
static int bundle_parent(const char *argv0, char *out, size_t n) {
    char buf[1024];
    int i;
    char *slash;
    if (argv0 == NULL || argv0[0] == '\0') return 0;
    snprintf(buf, sizeof(buf), "%s", argv0);
    for (i = 0; i < 4; i++) {          /* isle, MacOS, Contents, Foo.app */
        slash = strrchr(buf, '/');
        if (slash == NULL) return 0;
        *slash = '\0';
    }
    if (buf[0] == '\0') { snprintf(out, n, "/"); return 1; }
    snprintf(out, n, "%s", buf);
    return 1;
}

void sbk_games_probe(const char *argv0) {
    const char *home = getenv("HOME");
    char dirs[5][1024];
    int ndirs = 0, i, d, b;

    snprintf(dirs[ndirs++], sizeof(dirs[0]), "/Applications");
    if (home != NULL) snprintf(dirs[ndirs++], sizeof(dirs[0]), "%s/Applications", home);
    if (bundle_parent(argv0, dirs[ndirs], sizeof(dirs[0]))) ndirs++;
    if (home != NULL) snprintf(dirs[ndirs++], sizeof(dirs[0]), "%s", home);

    for (i = 0; i < sbk_game_count(); i++) {
        if (games[i].self) { games[i].installed = 1; continue; }
        games[i].installed = 0;
        games[i].exe[0] = '\0';
        for (d = 0; d < ndirs && !games[i].installed; d++) {
            for (b = 0; games[i].bundles[b] != NULL; b++) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/%s/Contents/MacOS/isle", dirs[d], games[i].bundles[b]);
                if (is_exec(path)) {
                    snprintf(games[i].exe, sizeof(games[i].exe), "%s", path);
                    games[i].installed = 1;
                    printf("sbk: %s found at %s/%s\n", games[i].title, dirs[d], games[i].bundles[b]);
                    break;
                }
            }
        }
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
    sbk_settings.widescreen = SBK_WIDE_4_3;
    sbk_settings.fadein = 0;
    sbk_settings.msaa = 0;
    sbk_settings.texfilter = SBK_TEXFILTER_RDP;
    sbk_settings.fullscreen = 0;
    sbk_settings.vsync = 1;
    sbk_settings.volume = 100;
    sbk_settings.launcher = 1;
    sbk_settings.perf = 0;
    sbk_settings.haze = 0;
}

void sbk_settings_apply_mode(int mode) {
    sbk_settings.mode = mode;
    if (mode == SBK_MODE_ENHANCED) {
        sbk_settings.draw_distance = 2;
        sbk_settings.resolution = SBK_RES_NATIVE;
        sbk_settings.filter = SBK_FILTER_NONE;
        sbk_settings.haze = 1;
        sbk_settings.fadein = 1;
        sbk_settings.msaa = 2;
        sbk_settings.texfilter = SBK_TEXFILTER_RDP;
    } else if (mode == SBK_MODE_ORIGINAL) {
        sbk_settings.draw_distance = 1;
        sbk_settings.resolution = SBK_RES_N64;
        sbk_settings.filter = SBK_FILTER_NONE;
        sbk_settings.widescreen = SBK_WIDE_4_3;
        sbk_settings.haze = 0;
        sbk_settings.fadein = 0;
        sbk_settings.msaa = 0;
        sbk_settings.texfilter = SBK_TEXFILTER_RDP;
    }
}

/* What a player gets on a first run: the game as it looks best on the G4,
 * filling the screen. sbk_settings_defaults() above is deliberately NOT this:
 * a scripted run uses it and every golden replay was cut against those values,
 * so it has to stay where it is. */
void sbk_settings_player_defaults(void) {
    sbk_settings_defaults();
    sbk_settings_apply_mode(SBK_MODE_ENHANCED);
    sbk_settings.fullscreen = 1;
    sbk_settings.vsync = 1;
    sbk_settings.volume = 100;
    sbk_settings.launcher = 1;
    sbk_settings.perf = 0;
}

/* Which mode the individual settings currently spell, so that changing one
 * knob on the Options page moves the Mode line to CUSTOM rather than lying. */
int sbk_settings_derive_mode(void) {
    /* Widescreen is deliberately not part of either preset's fingerprint: it
     * is a framing choice for the player's own screen, not a quality dial, so
     * picking 16:9 must not turn ENHANCED into CUSTOM. */
    if (sbk_settings.draw_distance == 2 && sbk_settings.resolution == SBK_RES_NATIVE &&
        sbk_settings.filter == SBK_FILTER_NONE &&
        sbk_settings.haze && sbk_settings.fadein && sbk_settings.msaa == 2 &&
        sbk_settings.texfilter == SBK_TEXFILTER_RDP) return SBK_MODE_ENHANCED;
    if (sbk_settings.draw_distance == 1 && sbk_settings.resolution == SBK_RES_N64 &&
        sbk_settings.filter == SBK_FILTER_NONE && !sbk_settings.widescreen &&
        !sbk_settings.haze && !sbk_settings.fadein && sbk_settings.msaa == 0 &&
        sbk_settings.texfilter == SBK_TEXFILTER_RDP) return SBK_MODE_ORIGINAL;
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
static const char *const wide_names[] = { "4:3", "16:9" };
static const char *const texfilter_names[] = { "rdp", "point", "bilinear" };

const char *sbk_settings_mode_name(int v) { return mode_names[clampi(v, 0, 2)]; }
const char *sbk_settings_res_name(int v) { return res_names[clampi(v, 0, 2)]; }
const char *sbk_settings_filter_name(int v) { return filter_names[clampi(v, 0, 3)]; }
const char *sbk_settings_wide_name(int v) { return wide_names[clampi(v, 0, 1)]; }
const char *sbk_settings_texfilter_name(int v) { return texfilter_names[clampi(v, 0, 2)]; }

float sbk_settings_aspect(void) {
    return sbk_settings.widescreen == SBK_WIDE_16_9 ? 16.0f / 9.0f : 4.0f / 3.0f;
}

void sbk_settings_load(void) {
    FILE *f;
    char line[256];
    int haze_seen = 0, fadein_seen = 0;
    sbk_settings_defaults();
    if (sbk_settings_scripted) return;   /* determinism: goldens see the defaults */
    f = fopen(sbk_settings_path(), "r");
    if (f == NULL) {
        /* first run: leave the player a file with the settings they got */
        sbk_settings_player_defaults();
        sbk_settings_save();
        printf("sbk: wrote first-run settings to %s\n", sbk_settings_path());
        return;
    }
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
        else if (strcmp(key, "widescreen") == 0) {
            /* "4:3" / "16:9", and the 0 / 1 an older file wrote.  Not atoi():
             * atoi("4:3") is 4, so the file's own 4:3 read back as widescreen
             * and a soak came up 16:9 against a settings.txt that said 4:3. */
            sbk_settings.widescreen = (strcmp(val, "16:9") == 0 || strcmp(val, "1") == 0)
                                      ? SBK_WIDE_16_9 : SBK_WIDE_4_3;
        }
        else if (strcmp(key, "fadein") == 0) { sbk_settings.fadein = atoi(val) != 0; fadein_seen = 1; }
        else if (strcmp(key, "msaa") == 0) {
            int n = atoi(val);
            sbk_settings.msaa = n >= 4 ? 4 : (n >= 2 ? 2 : 0);
        }
        else if (strcmp(key, "texfilter") == 0) sbk_settings.texfilter = name_index(val, texfilter_names, 3, SBK_TEXFILTER_RDP);
        else if (strcmp(key, "fullscreen") == 0) sbk_settings.fullscreen = atoi(val) != 0;
        else if (strcmp(key, "vsync") == 0) sbk_settings.vsync = atoi(val) != 0;
        else if (strcmp(key, "volume") == 0) sbk_settings.volume = clampi(atoi(val), 0, 100);
        else if (strcmp(key, "launcher") == 0) sbk_settings.launcher = atoi(val) != 0;
        else if (strcmp(key, "perf") == 0) sbk_settings.perf = atoi(val) != 0;
        else if (strcmp(key, "haze") == 0) { sbk_settings.haze = atoi(val) != 0; haze_seen = 1; }
    }
    fclose(f);
    /* A settings.txt written before the haze existed has no key for it, and
     * the port's baseline is off -- so an Enhanced player who has been running
     * since before this landed would silently not get it. Enhanced means the
     * haze; give it to them, and the next save writes the key. */
    if (!haze_seen && sbk_settings.mode == SBK_MODE_ENHANCED) sbk_settings.haze = 1;
    /* Same again for the options that landed after the haze: an Enhanced
     * player whose file predates them means the Enhanced of today. */
    if (!fadein_seen && sbk_settings.mode == SBK_MODE_ENHANCED) {
        sbk_settings.fadein = 1;
        sbk_settings.msaa = 2;
    }
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
    fprintf(f, "widescreen=%s\n", sbk_settings_wide_name(sbk_settings.widescreen));
    fprintf(f, "fadein=%d\n", sbk_settings.fadein);
    fprintf(f, "msaa=%d\n", sbk_settings.msaa);
    fprintf(f, "texfilter=%s\n", sbk_settings_texfilter_name(sbk_settings.texfilter));
    fprintf(f, "fullscreen=%d\n", sbk_settings.fullscreen);
    fprintf(f, "vsync=%d\n", sbk_settings.vsync);
    fprintf(f, "volume=%d\n", sbk_settings.volume);
    fprintf(f, "launcher=%d\n", sbk_settings.launcher);
    fprintf(f, "perf=%d\n", sbk_settings.perf);
    fprintf(f, "haze=%d\n", sbk_settings.haze);
    fclose(f);
}

/* --- pushing the values into the running port --------------------------- */

extern float sbk_far_scale;
extern int sbk_wide_output;
extern int sbk_perf_enabled;
extern int sbk_haze_enabled;          /* gfx/haze.c */
extern int sbk_fadein_enabled;        /* gfx/haze.c: far-object fade-in */
void gfx_gl13_set_texfilter(int mode); /* gfx_gl13.c */
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
    sbk_haze_enabled = sbk_settings.haze;
    sbk_fadein_enabled = sbk_settings.fadein;
    if (sbk_settings_scripted && !sbk_settings_forced) {
        /* No post-processing at all under a script: a golden replay must see
         * exactly the pixels it was recorded against. */
        gfx_gl13_set_render_scale(SBK_RES_NATIVE);
        gfx_gl13_set_filter(SBK_FILTER_NONE);
        sbk_haze_enabled = 0;   /* a golden replay sees the pixels it was cut against */
        sbk_fadein_enabled = 0;
        gfx_gl13_set_texfilter(SBK_TEXFILTER_RDP);
        if (sbk_wide_output != SBK_WIDE_4_3) {
            sbk_wide_output = SBK_WIDE_4_3;
            sbk_gfx_refresh_output_rect();
        }
        return;
    }
    if (sbk_wide_output != sbk_settings.widescreen) {
        sbk_wide_output = sbk_settings.widescreen;
        sbk_gfx_refresh_output_rect();
    }
    gfx_gl13_set_texfilter(sbk_settings.texfilter);
    gfx_gl13_set_render_scale(sbk_settings.resolution);
    gfx_gl13_set_filter(sbk_settings.filter);
    sbk_gfx_set_vsync(sbk_settings.vsync);
}
