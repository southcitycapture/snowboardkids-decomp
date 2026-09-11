/* Persistent user settings: key=value in
 * ~/Library/Application Support/SnowboardKids/settings.txt
 *
 * Loaded before the command line is parsed, so a flag always wins for that
 * run; saved whenever the launcher or the in-game overlay changes something.
 * Scripted runs (--play / --headless / --nolauncher) never read the file:
 * a golden replay has to see the port's defaults, not whatever the last
 * session left behind. */
#ifndef SBK_SETTINGS_H
#define SBK_SETTINGS_H

enum { SBK_MODE_ORIGINAL = 0, SBK_MODE_ENHANCED = 1, SBK_MODE_CUSTOM = 2 };
enum { SBK_RES_NATIVE = 0, SBK_RES_N64 = 1, SBK_RES_2X = 2 };
enum { SBK_FILTER_NONE = 0, SBK_FILTER_SCANLINES = 1, SBK_FILTER_GRILLE = 2, SBK_FILTER_SMOOTH = 3 };

/* Registered games. Only sbk1 exists today; the table is what lets a second
 * executable/ROM be added later without touching the launcher. */
struct SbkGameEntry {
    const char *id;        /* "sbk1" */
    const char *title;     /* "Snowboard Kids" */
    const char *rom;       /* ROM file name looked for next to the executable */
    int installed;         /* 0 = shown greyed out with "not installed" */
};

struct SbkSettings {
    char game[16];
    int mode;            /* SBK_MODE_* */
    int draw_distance;   /* 1..4 */
    int resolution;      /* SBK_RES_* */
    int filter;          /* SBK_FILTER_* */
    int widescreen;      /* 0/1 -- --wide */
    int fullscreen;      /* 0/1 */
    int vsync;           /* 0/1 */
    int volume;          /* 0..100 */
    int launcher;        /* 0/1 -- show the launcher at startup */
    int perf;            /* 0/1 -- --perf overlay/logging */
};

extern struct SbkSettings sbk_settings;
extern int sbk_settings_scripted;   /* 1 = no file, no launcher, no filters */
/* Set when --mode= / --resolution= / --filter= was given: those are honoured
 * even in a scripted run ("apply no filters unless asked"). Goldens never
 * pass them, so replays stay bit-identical. */
extern int sbk_settings_forced;
extern int sbk_settings_loaded;

const char *sbk_settings_dir(void);
const char *sbk_settings_path(void);
void sbk_settings_defaults(void);
void sbk_settings_load(void);
void sbk_settings_save(void);

/* Push the current settings into the running port (draw distance, resolution
 * mode, filter, widescreen, vsync, volume). Safe to call every time something
 * changes; cheap and idempotent. */
void sbk_settings_apply(void);

/* Set every derived setting from mode (Original / Enhanced). */
void sbk_settings_apply_mode(int mode);

int sbk_game_count(void);
const struct SbkGameEntry *sbk_game_at(int i);
int sbk_game_index(const char *id);

#endif
