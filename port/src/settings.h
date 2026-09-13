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

/* Registered games: this port's own game plus its sibling. Either bundle is a
 * front door -- the launcher lists both, and picking the other one hands the
 * session over to that bundle's executable (see sbk_games_probe). */
struct SbkGameEntry {
    const char *id;             /* "sbk1" */
    const char *title;          /* "Snowboard Kids" */
    const char *rom;            /* ROM file name inside the bundle's Resources */
    const char *const *bundles; /* NULL-terminated .app names to look for */
    int self;                   /* 1 = the game this executable is */
    int installed;              /* 0 = shown greyed out with "not installed" */
    char exe[1024];             /* Contents/MacOS/isle of the bundle we found */
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
    int haze;            /* 0/1 -- Enhanced-mode distance haze (gfx/haze.c) */
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
int sbk_game_self_index(void);

/* Look for the other game's bundle in /Applications, ~/Applications, next to
 * this bundle and in the home directory; mark it installed and remember its
 * executable. argv0 is this process's argv[0]. */
void sbk_games_probe(const char *argv0);

/* Sensible first-run settings for a player (Enhanced, fullscreen). Distinct
 * from sbk_settings_defaults(), which is the port's own baseline and has to
 * stay put: a scripted run uses it and golden replays are cut against it. */
void sbk_settings_player_defaults(void);

#endif
