/* The launcher and the in-game options overlay.
 *
 * The launcher owns the window before the game boots and is three screens and
 * no more:
 *
 *   PICK    the two games as N64 boxes on a snow shelf under a drifting sky,
 *           the selected one lifted and slowly turning; its name, whether its
 *           cartridge is in the ROM folder, and the mode it would start in.
 *   MODE    Original / Enhanced / Custom, each with a line saying what it is,
 *           a Tweak entry into the options and Start.
 *   OPTIONS the individual settings, one row each, left/right to change.
 *
 * A game with no cartridge dump gets a grey, unlit box and a note saying where
 * to put one, with an Open folder action; that note is a panel over PICK
 * rather than a fourth screen.
 *
 * The in-game overlay is the OPTIONS screen's list drawn over the running
 * game, in the same style.
 *
 * All the type is the games' own sprite font, pulled out of whichever ROM is
 * present (ui_rom_art.c); with no ROM at all it falls back to the port's
 * generated one and everything else still works. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>
#include "ui.h"
#include "ui_gl.h"
#include "ui_scene.h"
#include "ui_rom_art.h"
#include "../settings.h"
#include "../rom_scan.h"
#include "../platform/input.h"

extern void gfx_handle_events(void);
void sbk_gfx_window_size(int *w, int *h);
void sbk_gfx_swap(void);
void sbk_gfx_set_fullscreen(int on);
int sbk_gfx_is_fullscreen(void);
int sbk_settings_derive_mode(void);
void sbk_input_request_quit(void);

/* Set when Start was pressed on the other game's row: main() hands the
 * session over to that bundle once the launcher returns. */
char sbk_launch_target[1024];
/* 1 while the launcher owns the window: Esc means "back/quit here", not
 * "quit the game" (which is what it does once a race is running). */
int sbk_ui_launcher_active;

/* --- palette -------------------------------------------------------------
 * Warmer and lighter than the old debug-menu blue, because the panels now sit
 * on a bright sky rather than on black. */
static const struct SbkColor PANEL_BG  = { 0.05f, 0.09f, 0.17f, 0.90f };
static const struct SbkColor PANEL_TOP = { 0.09f, 0.16f, 0.30f, 0.92f };
static const struct SbkColor GOLD      = { 1.00f, 0.82f, 0.25f, 1.00f };
static const struct SbkColor SHADE     = { 0.00f, 0.00f, 0.00f, 0.45f };

static struct SbkColor tint(struct SbkColor c, float a) { c.a *= a; return c; }

/* --- list model ---------------------------------------------------------- */

enum { IT_CHOICE = 1, IT_RANGE, IT_TOGGLE, IT_ACTION, IT_GAP, IT_MODE };

enum { ACT_NONE, ACT_OPTIONS, ACT_START, ACT_QUIT, ACT_BACK, ACT_DEFAULTS, ACT_OPENFOLDER };

struct UiItem {
    const char *label;
    int type;
    int *val;
    int lo, hi, step;
    const char *const *names;
    int action;
    int arg;                 /* IT_MODE: which mode this row is */
    const char *note;        /* the line under a mode row */
};

static const char *const mode_names[]   = { "ORIGINAL", "ENHANCED", "CUSTOM" };
static const char *const res_names[]    = { "NATIVE", "N64 320X240", "2X 640X480" };
static const char *const filter_names[] = { "NONE", "SCANLINES", "GRILLE", "SMOOTH" };
static const char *const off_on[]       = { "OFF", "ON" };
static const char *const wide_names[]   = { "4:3", "16:9" };
static const char *const msaa_names[]   = { "OFF", "2X", "4X" };
static const char *const texf_names[]   = { "AS THE RDP ASKS", "POINT", "BILINEAR" };

/* The Anti-aliasing row stores 0/1/2 and the setting stores 0/2/4: a
 * multisample count is chosen before the window exists, so the row is an
 * index and ui_sync_shadows() keeps the two in step. */
static int ui_msaa_shadow;
static int ui_fullscreen_shadow;

#define ROW(l,t,v,lo,hi,st,n) { l, t, v, lo, hi, st, n, 0, 0, NULL }

static struct UiItem options_items[] = {
    ROW("Draw distance", IT_RANGE,  &sbk_settings.draw_distance, 1, 4, 1, NULL),
    ROW("Distance haze", IT_TOGGLE, &sbk_settings.haze,          0, 1, 1, off_on),
    ROW("Resolution",    IT_CHOICE, &sbk_settings.resolution,    0, 2, 1, res_names),
    ROW("Filter",        IT_CHOICE, &sbk_settings.filter,        0, 3, 1, filter_names),
    ROW("Texture filter",IT_CHOICE, &sbk_settings.texfilter,     0, 2, 1, texf_names),
    ROW("Far fade-in",   IT_TOGGLE, &sbk_settings.fadein,        0, 1, 1, off_on),
    ROW("Anti-aliasing", IT_CHOICE, &ui_msaa_shadow,             0, 2, 1, msaa_names),
    ROW("Widescreen",    IT_CHOICE, &sbk_settings.widescreen,    0, 1, 1, wide_names),
    ROW("Fullscreen",    IT_TOGGLE, &ui_fullscreen_shadow,       0, 1, 1, off_on),
    ROW("V-sync",        IT_TOGGLE, &sbk_settings.vsync,         0, 1, 1, off_on),
    ROW("Volume",        IT_RANGE,  &sbk_settings.volume,        0, 100, 5, NULL),
    ROW("Perf readout",  IT_TOGGLE, &sbk_settings.perf,          0, 1, 1, off_on),
    { "",                 IT_GAP,    NULL, 0,0,0, NULL, 0, 0, NULL },
    { "Restore defaults", IT_ACTION, NULL, 0,0,0, NULL, ACT_DEFAULTS, 0, NULL },
    { "Quit",             IT_ACTION, NULL, 0,0,0, NULL, ACT_QUIT,     0, NULL },
    { "Back",             IT_ACTION, NULL, 0,0,0, NULL, ACT_BACK,     0, NULL },
};
#define OPTIONS_N ((int)(sizeof(options_items) / sizeof(options_items[0])))

/* The overlay is the same list without Back's "return to the mode screen"
 * meaning, so it shares the array; only the launcher's own Quit differs. */

static struct UiItem mode_items[] = {
    { "Original", IT_MODE, NULL, 0,0,0, NULL, 0, SBK_MODE_ORIGINAL,
      "the cartridge as it was: 320x240, no haze, no AA" },
    { "Enhanced", IT_MODE, NULL, 0,0,0, NULL, 0, SBK_MODE_ENHANCED,
      "twice the draw distance, hazed, faded in, 2x AA" },
    { "Custom",   IT_MODE, NULL, 0,0,0, NULL, 0, SBK_MODE_CUSTOM,
      "whatever you set on the options screen" },
    { "",         IT_GAP,  NULL, 0,0,0, NULL, 0, 0, NULL },
    { "Tweak",    IT_ACTION, NULL, 0,0,0, NULL, ACT_OPTIONS, 0, NULL },
    { "Start",    IT_ACTION, NULL, 0,0,0, NULL, ACT_START,   0, NULL },
    { "Back",     IT_ACTION, NULL, 0,0,0, NULL, ACT_BACK,    0, NULL },
};
#define MODE_N ((int)(sizeof(mode_items) / sizeof(mode_items[0])))

static struct UiItem norom_items[] = {
    { "Open folder", IT_ACTION, NULL, 0,0,0, NULL, ACT_OPENFOLDER, 0, NULL },
    { "Back",        IT_ACTION, NULL, 0,0,0, NULL, ACT_BACK,       0, NULL },
};
#define NOROM_N ((int)(sizeof(norom_items) / sizeof(norom_items[0])))

static int item_selectable(const struct UiItem *it) {
    return it->type != IT_GAP;
}

/* --- scripted UI input (--uiscript) --------------------------------------
 * The G4 is driven over SSH and synthetic key events do not reach a
 * fullscreen SDL window, so the launcher and the overlay are exercised (and
 * screenshotted) from a token string instead: u d l r = directions, a = A,
 * b = B, m = the overlay button, w = wait a second. One token per
 * SBK_UISCRIPT_TICKS ticks, pressed on the first of them. */
#define SBK_UISCRIPT_TICKS 8

static const char *ui_script;
static int ui_script_pos, ui_script_tick, ui_script_wait;

void sbk_ui_script_set(const char *seq) {
    ui_script = seq;
    ui_script_pos = 0;
    ui_script_tick = 0;
    printf("sbk: ui script \"%s\"\n", seq);
}

static void ui_script_apply(struct SbkUiRaw *r) {
    char c;
    if (ui_script == NULL || ui_script[ui_script_pos] == '\0') return;
    if (ui_script_wait > 0) { ui_script_wait--; return; }
    if (ui_script_tick++ % SBK_UISCRIPT_TICKS != 0) return;
    c = ui_script[ui_script_pos++];
    switch (c) {
        case 'u': r->up = 1; break;
        case 'd': r->down = 1; break;
        case 'l': r->left = 1; break;
        case 'r': r->right = 1; break;
        case 'a': r->accept = 1; break;
        case 'b': r->cancel = 1; break;
        case 'm': r->menu = 1; break;
        case 'w': ui_script_wait = 60; break;
        default: break;
    }
}

/* --- navigation ---------------------------------------------------------- */

struct Nav {
    int hold[6];        /* up, down, left, right, accept, cancel */
    int cursor;
};

#define REPEAT_DELAY 18
#define REPEAT_RATE 4

static int edge(int *hold, int down) {
    if (!down) { *hold = 0; return 0; }
    (*hold)++;
    if (*hold == 1) return 1;
    if (*hold > REPEAT_DELAY && ((*hold - REPEAT_DELAY) % REPEAT_RATE) == 0) return 1;
    return 0;
}

static void move_cursor(struct Nav *n, struct UiItem *items, int count, int dir) {
    int i = n->cursor;
    int guard;
    for (guard = 0; guard < count; guard++) {
        i += dir;
        if (i < 0) i = count - 1;
        if (i >= count) i = 0;
        if (item_selectable(&items[i])) { n->cursor = i; return; }
    }
}

static void clamp_cursor(struct Nav *n, struct UiItem *items, int count) {
    if (n->cursor < 0 || n->cursor >= count) n->cursor = 0;
    if (!item_selectable(&items[n->cursor])) move_cursor(n, items, count, 1);
}

static void ui_sync_shadows(void) {
    ui_fullscreen_shadow = sbk_gfx_is_fullscreen();
    ui_msaa_shadow = sbk_settings.msaa >= 4 ? 2 : (sbk_settings.msaa >= 2 ? 1 : 0);
}

static void adjust(struct UiItem *it, int dir) {
    int v;
    if (it->val == NULL) return;
    v = *it->val + dir * it->step;
    if (v < it->lo) v = it->hi;
    if (v > it->hi) v = it->lo;
    *it->val = v;
    if (it->val == &ui_fullscreen_shadow) {
        sbk_gfx_set_fullscreen(v);
        sbk_settings.fullscreen = v;
    } else if (it->val == &ui_msaa_shadow) {
        /* The sample count is a pixel-format attribute, so it is chosen when
         * the window is created: the row writes the setting and the file, and
         * the next start picks it up. */
        sbk_settings.msaa = v == 0 ? 0 : (v == 1 ? 2 : 4);
        sbk_settings.mode = sbk_settings_derive_mode();
    } else {
        sbk_settings.mode = sbk_settings_derive_mode();
    }
    sbk_settings_apply();
    sbk_settings_save();
}

/* --- drawing helpers ----------------------------------------------------- */

static void value_text(const struct UiItem *it, char *out, size_t n) {
    out[0] = '\0';
    switch (it->type) {
        case IT_CHOICE:
        case IT_TOGGLE:
            snprintf(out, n, "%s", it->names[*it->val]);
            break;
        case IT_RANGE:
            if (it->val == &sbk_settings.volume) snprintf(out, n, "%d%%", *it->val);
            else snprintf(out, n, "%dX", *it->val);
            break;
        default:
            break;
    }
}

/* The selection bar: a gold-edged slab rather than a flat rectangle, so the
 * cursor reads at a glance on a bright background. */
static void selection_bar(int x, int y, int w, int h, float a) {
    struct SbkColor fill = { 1.00f, 0.82f, 0.25f, 0.22f };
    struct SbkColor edge = { 1.00f, 0.86f, 0.35f, 0.95f };
    sbk_ui_rect(x, y, w, h, tint(fill, a));
    sbk_ui_rect(x, y, 3, h, tint(edge, a));
}

static int row_height(int scale) { return sbk_ui_text_h(scale) + scale * 5; }

static int draw_list(struct UiItem *items, int count, int cursor,
                     int x, int y, int w, int scale, float a) {
    int row_h = row_height(scale);
    int i, cy = y;
    char val[64];
    for (i = 0; i < count; i++) {
        struct UiItem *it = &items[i];
        struct SbkColor fg = SBK_UI_FG;
        int selected = (i == cursor);
        int note_h = 0;
        if (it->type == IT_GAP) { cy += row_h / 2; continue; }
        if (it->note != NULL) note_h = sbk_ui_text_h(scale > 1 ? scale - 1 : 1) + scale;
        if (selected) {
            selection_bar(x - scale * 4, cy - scale, w + scale * 8, row_h + note_h, a);
            fg = SBK_UI_SEL;
        }
        if (it->type == IT_MODE) {
            int on = sbk_settings.mode == it->arg;
            sbk_ui_text(x, cy + scale, scale, on ? "\3" : " ", tint(on ? GOLD : SBK_UI_DIM, a));
            sbk_ui_text(x + sbk_ui_text_w("  ", scale), cy + scale, scale, it->label, tint(fg, a));
        } else {
            sbk_ui_text(x, cy + scale, scale, it->label, tint(fg, a));
        }
        value_text(it, val, sizeof(val));
        if (val[0] != '\0') {
            int vw = sbk_ui_text_w(val, scale);
            int vx = x + w - vw - sbk_ui_text_w("  ", scale);
            struct SbkColor vc = selected ? SBK_UI_SEL : SBK_UI_ACCENT;
            if (selected) {
                sbk_ui_text(vx - sbk_ui_text_w("  ", scale), cy + scale, scale, "\1", tint(SBK_UI_FG, a));
                sbk_ui_text(x + w - sbk_ui_text_w(" ", scale), cy + scale, scale, "\2", tint(SBK_UI_FG, a));
            }
            sbk_ui_text(vx, cy + scale, scale, val, tint(vc, a));
        }
        cy += row_h;
        if (it->note != NULL) {
            int s = scale > 1 ? scale - 1 : 1;
            sbk_ui_text(x + sbk_ui_text_w("  ", scale), cy - scale, s, it->note,
                        tint(selected ? SBK_UI_FG : SBK_UI_DIM, a * 0.9f));
            cy += note_h;
        }
    }
    return cy - y;
}

static int list_height(struct UiItem *items, int count, int scale) {
    int row_h = row_height(scale);
    int i, h = 0;
    for (i = 0; i < count; i++) {
        h += items[i].type == IT_GAP ? row_h / 2 : row_h;
        if (items[i].note != NULL) h += sbk_ui_text_h(scale > 1 ? scale - 1 : 1) + scale;
    }
    return h;
}

static int list_width(struct UiItem *items, int count, int scale) {
    int i, w = 0;
    char val[64];
    for (i = 0; i < count; i++) {
        int lw = sbk_ui_text_w(items[i].label, scale);
        int row;
        if (items[i].type == IT_MODE) lw += sbk_ui_text_w("  ", scale);
        value_text(&items[i], val, sizeof(val));
        row = lw + (val[0] != '\0' ? sbk_ui_text_w(val, scale) + sbk_ui_text_w("      ", scale) : 0);
        if (row > w) w = row;
        if (items[i].note != NULL) {
            int nw = sbk_ui_text_w(items[i].note, scale > 1 ? scale - 1 : 1) + sbk_ui_text_w("  ", scale);
            if (nw > w) w = nw;
        }
    }
    return w + sbk_ui_text_w("  ", scale);
}

static int panel_height(struct UiItem *items, int count, int scale, int with_hint) {
    int pad = scale * 8;
    int h = pad + sbk_ui_text_h(scale) + scale * 8 + list_height(items, count, scale) + pad;
    if (with_hint) h += sbk_ui_text_h(scale) + pad / 2;
    return h;
}

static int panel_width(struct UiItem *items, int count, int scale, const char *hint) {
    int pad = scale * 8;
    int w = list_width(items, count, scale);
    int hw = hint != NULL ? sbk_ui_text_w(hint, scale > 1 ? scale - 1 : 1) : 0;
    if (hw > w) w = hw;
    return w + 2 * pad;
}

/* A panel: a dark slab with a lighter title strip and a gold rule under it. */
static void panel_frame(int px, int py, int pw, int ph, int scale, const char *title, float a) {
    int pad = scale * 8;
    int th = sbk_ui_text_h(scale) + pad;
    sbk_ui_rect(px + scale * 2, py + scale * 2, pw, ph, tint(SHADE, a));
    sbk_ui_rect(px, py, pw, ph, tint(PANEL_BG, a));
    sbk_ui_rect(px, py, pw, th, tint(PANEL_TOP, a));
    sbk_ui_rect(px, py + th, pw, scale, tint(GOLD, a));
    sbk_ui_border(px, py, pw, ph, scale > 1 ? 2 : 1, tint(SBK_UI_LINE, a));
    if (title != NULL) sbk_ui_text(px + pad, py + pad / 2, scale, title, tint(GOLD, a));
}

static void draw_panel(const char *title, struct UiItem *items, int count, int cursor,
                       int px, int py, int pw, int ph, int scale, const char *hint, float a) {
    int pad = scale * 8;
    panel_frame(px, py, pw, ph, scale, title, a);
    draw_list(items, count, cursor, px + pad, py + pad + sbk_ui_text_h(scale) + scale * 8,
              pw - 2 * pad, scale, a);
    if (hint != NULL) {
        sbk_ui_text(px + pad, py + ph - pad - sbk_ui_text_h(scale), scale > 1 ? scale - 1 : 1,
                    hint, tint(SBK_UI_DIM, a));
    }
}

static void centred(int cx, int y, int scale, const char *s, struct SbkColor c) {
    sbk_ui_text_shadow(cx - sbk_ui_text_w(s, scale) / 2, y, scale, s, c);
}

/* Break a sentence into lines of at most `cols` characters, on spaces.
 * Returns the number of lines written (at most 4). */
#define WRAP_MAX 4
#define WRAP_COLS 96

static int wrap_text(const char *s, int cols, char out[WRAP_MAX][WRAP_COLS]) {
    int n = 0, len = 0;
    const char *w = s;
    if (cols < 8) cols = 8;
    if (cols > WRAP_COLS - 1) cols = WRAP_COLS - 1;
    out[0][0] = '\0';
    while (*w != '\0' && n < WRAP_MAX) {
        const char *e = w;
        int wl;
        while (*e != '\0' && *e != ' ') e++;
        wl = (int)(e - w);
        if (len > 0 && len + 1 + wl > cols) {
            if (++n >= WRAP_MAX) break;
            out[n][0] = '\0';
            len = 0;
        }
        if (wl > cols) wl = cols;
        if (len > 0) { out[n][len++] = ' '; }
        memcpy(out[n] + len, w, (size_t)wl);
        len += wl;
        out[n][len] = '\0';
        w = (*e == ' ') ? e + 1 : e;
    }
    return n + 1;
}

/* A path that will not fit gets its front eaten, not its end: the filename is
 * the part the player needs to recognise. */
static const char *fit_path(const char *p, int cols, char *buf, size_t n) {
    int len = (int)strlen(p);
    if (cols < 8 || len <= cols) return p;
    snprintf(buf, n, "...%s", p + (len - cols + 3));
    return buf;
}

/* --- the launcher -------------------------------------------------------- */

enum { PAGE_PICK = 0, PAGE_MODE, PAGE_OPTIONS };

static struct Nav nav;
static int page;
static int pick;                 /* which game's box is lifted */
static int norom_open;           /* the "put a cartridge here" panel */
static struct Nav norom_nav;
static float page_fade = 1.0f;   /* 1 = settled; a page change resets it to 0 */
static float scene_blend = 1.0f; /* how much of the screen the boxes get */

static void goto_page(int p, struct UiItem *items, int count) {
    page = p;
    page_fade = 0.0f;
    nav.cursor = 0;
    memset(nav.hold, 0, sizeof(nav.hold));
    if (items != NULL) clamp_cursor(&nav, items, count);
}

static int rom_ok(int g) {
    const struct SbkRomSlot *s = sbk_rom_slot(g);
    return s != NULL && s->status == SBK_ROM_OK;
}

/* Any registered game the launcher can actually start from here: the one this
 * executable is, and the other one when its bundle is installed. */
static int game_reachable(int g) {
    const struct SbkGameEntry *e = sbk_game_at(g);
    return e != NULL && (e->self || e->installed);
}

static void draw_pick(int win_w, int win_h, int scale) {
    struct SbkBoxPlace place[4];
    int present[4];
    int n = sbk_game_count(), i;
    const struct SbkGameEntry *e;
    int cx = win_w / 2;
    int y;
    char line[192];

    for (i = 0; i < n && i < 4; i++) present[i] = rom_ok(i);
    sbk_ui_scene_boxes(win_w, win_h, n, pick, present, scene_blend, place);
    sbk_ui_scene_weather(win_w, win_h);

    /* the masthead: the user's own "Snowboard Kids 1+2 PowerPC Edition" logo */
    {
        int lw = win_w * 46 / 100, lh;
        if (lw > 560) lw = 560;
        lh = lw * 288 / 512;
        if (lh > win_h / 4) { lh = win_h / 4; lw = lh * 512 / 288; }
        sbk_ui_logo((win_w - lw) / 2, win_h / 40, lw, lh);
    }

    /* a name under every box, the selected one gold */
    for (i = 0; i < n && i < 4; i++) {
        int by = place[i].cy + place[i].h / 2 + scale * 9;
        int s = i == pick ? scale : (scale > 1 ? scale - 1 : 1);
        e = sbk_game_at(i);
        if (e == NULL) continue;
        centred(place[i].cx, by, s, e->title,
                tint(i == pick ? GOLD : SBK_UI_DIM, page_fade));
        if (!present[i]) {
            centred(place[i].cx, by + sbk_ui_text_h(s) + scale, scale > 1 ? scale - 1 : 1,
                    "NO CARTRIDGE", tint(SBK_UI_DIM, page_fade));
        } else if (!game_reachable(i)) {
            centred(place[i].cx, by + sbk_ui_text_h(s) + scale, scale > 1 ? scale - 1 : 1,
                    "NOT INSTALLED", tint(SBK_UI_DIM, page_fade));
        }
    }

    /* the strip along the bottom: what pressing A would start */
    y = win_h - sbk_ui_text_h(scale) * 4;
    sbk_ui_rect(0, y - scale * 4, win_w, win_h - y + scale * 4, tint(PANEL_BG, page_fade));
    sbk_ui_rect(0, y - scale * 4, win_w, scale, tint(GOLD, page_fade));
    e = sbk_game_at(pick);
    if (rom_ok(pick) && game_reachable(pick)) {
        snprintf(line, sizeof(line), "%s   -   %s",
                 e != NULL ? e->title : "", mode_names[sbk_settings.mode]);
    } else if (!rom_ok(pick)) {
        /* the short form: the whole sentence is on the panel A opens, and a
         * strip that runs off both edges of the screen says nothing */
        const struct SbkRomSlot *sl = sbk_rom_slot(pick);
        const char *what = sl == NULL || sl->status == SBK_ROM_MISSING ? "NO CARTRIDGE"
                         : sl->status == SBK_ROM_WRONG_REGION ? "WRONG REGION"
                         : "DAMAGED FILE";
        snprintf(line, sizeof(line), "%s   -   PRESS A", what);
    } else {
        snprintf(line, sizeof(line), "%s   -   NOT INSTALLED",
                 e != NULL ? e->title : "");
    }
    {
        int ls = scale;
        while (ls > 1 && sbk_ui_text_w(line, ls) > win_w - scale * 4) ls--;
        centred(cx, y + (sbk_ui_text_h(scale) - sbk_ui_text_h(ls)) / 2, ls, line,
                tint(SBK_UI_FG, page_fade));
    }
    centred(cx, y + sbk_ui_text_h(scale) + scale, scale > 1 ? scale - 1 : 1,
            "LEFT/RIGHT PICK   A/ENTER CHOOSE   B/ESC QUIT", tint(SBK_UI_DIM, page_fade));
}

static void draw_norom(int win_w, int win_h, int scale) {
    const struct SbkRomSlot *sl = sbk_rom_slot(pick);
    const struct SbkGameEntry *e = sbk_game_at(pick);
    int s2 = scale > 1 ? scale - 1 : 1;
    int ps = scale > 2 ? 2 : 1;
    int pad = scale * 8;
    char lines[WRAP_MAX][WRAP_COLS];
    char pbuf[1200], fbuf[1200];
    const char *dir = sbk_rom_dir();
    int nlines, pw, ph, px, py, ty, step, i;
    int cols;

    /* widest the panel may be, then how many characters that is */
    pw = win_w - scale * 12;
    cols = (pw - pad * 2) / (sbk_ui_text_w("M", s2));
    nlines = wrap_text(sbk_rom_status_text(pick), cols, lines);

    /* shrink to what is actually used */
    {
        int want = 0;
        for (i = 0; i < nlines; i++) {
            int w = sbk_ui_text_w(lines[i], s2);
            if (w > want) want = w;
        }
        {
            int w = sbk_ui_text_ascii_w(dir, ps);
            if (w > want) want = w;
        }
        {
            int w = panel_width(norom_items, NOROM_N, scale, NULL) - pad * 2;
            if (w > want) want = w;
        }
        if (want + pad * 2 < pw) pw = want + pad * 2;
    }
    cols = (pw - pad * 2) / sbk_ui_text_ascii_w("M", ps);

    step = sbk_ui_text_h(s2) + scale * 2;
    ph = pad + sbk_ui_text_h(scale) + scale * 8
       + nlines * step + step * 3
       + list_height(norom_items, NOROM_N, scale) + pad;
    px = (win_w - pw) / 2;
    py = (win_h - ph) / 2;

    panel_frame(px, py, pw, ph, scale, e != NULL ? e->title : "NO CARTRIDGE", page_fade);
    ty = py + pad + sbk_ui_text_h(scale) + scale * 8;
    for (i = 0; i < nlines; i++) {
        sbk_ui_text(px + pad, ty, s2, lines[i], tint(SBK_UI_FG, page_fade));
        ty += step;
    }
    ty += scale * 2;
    sbk_ui_text(px + pad, ty, s2, "THE FOLDER BOTH GAMES LOOK IN:", tint(SBK_UI_DIM, page_fade));
    ty += step;
    sbk_ui_text_ascii(px + pad, ty, ps, fit_path(dir, cols, pbuf, sizeof(pbuf)),
                      tint(SBK_UI_ACCENT, page_fade));
    ty += step;
    if (sl != NULL && sl->status != SBK_ROM_MISSING) {
        sbk_ui_text_ascii(px + pad, ty, ps, fit_path(sl->path, cols, fbuf, sizeof(fbuf)),
                          tint(SBK_UI_DIM, page_fade));
    } else {
        sbk_ui_text(px + pad, ty, s2, "ANY FILENAME, .Z64 .N64 OR .V64", tint(SBK_UI_DIM, page_fade));
    }
    ty += step + scale * 2;
    draw_list(norom_items, NOROM_N, norom_nav.cursor, px + pad, ty, pw - pad * 2, scale, page_fade);
}

static void draw_launcher(int win_w, int win_h) {
    int scale = sbk_ui_pick_scale(win_h);
    struct UiItem *items = page == PAGE_MODE ? mode_items : options_items;
    int count = page == PAGE_MODE ? MODE_N : OPTIONS_N;
    const char *hint = page == PAGE_MODE ? "A/ENTER SELECT   B/ESC BACK"
                                         : "LEFT/RIGHT CHANGE   B/ESC BACK";
    if (page != PAGE_PICK) {
        while (scale > 1 && panel_height(items, count, scale, 1) > win_h - scale * 8) scale--;
    }

    sbk_ui_begin(win_w, win_h);
    sbk_ui_scene_sky(win_w, win_h);

    if (page == PAGE_PICK) {
        draw_pick(win_w, win_h, scale);
        if (norom_open) {
            struct SbkColor veil = { 0.0f, 0.0f, 0.0f, 0.45f };
            sbk_ui_rect(0, 0, win_w, win_h, veil);
            draw_norom(win_w, win_h, scale);
        }
    } else {
        int pw = panel_width(items, count, scale, hint);
        int ph = panel_height(items, count, scale, 1);
        int px, py;
        int present[4];
        int i, n = sbk_game_count();
        for (i = 0; i < n && i < 4; i++) present[i] = rom_ok(i);
        if (scene_blend > 0.02f) {
            sbk_ui_scene_boxes(win_w, win_h, n, pick, present, scene_blend, NULL);
            sbk_ui_scene_sky_veil(win_w, win_h, 1.0f - scene_blend);
        }
        sbk_ui_scene_weather(win_w, win_h);
        if (pw > win_w - scale * 8) pw = win_w - scale * 8;
        px = (win_w - pw) / 2;
        py = (win_h - ph) / 2 + (int)((1.0f - page_fade) * scale * 10);
        {
            const struct SbkGameEntry *e = sbk_game_at(pick);
            centred(win_w / 2, py - sbk_ui_text_h(scale) - scale * 6, scale,
                    e != NULL ? e->title : "", tint(GOLD, page_fade));
        }
        draw_panel(page == PAGE_MODE ? "MODE" : "OPTIONS", items, count, nav.cursor,
                   px, py, pw, ph, scale, hint, page_fade);
    }
    sbk_ui_end();
}

static void start_selected(int *start) {
    int g = pick;
    const struct SbkGameEntry *e = sbk_game_at(g);
    strncpy(sbk_settings.game, e != NULL ? e->id : "sbk1", sizeof(sbk_settings.game) - 1);
    sbk_settings.game[sizeof(sbk_settings.game) - 1] = '\0';
    sbk_settings_save();
    if (e != NULL && !e->self && e->installed && e->exe[0] != '\0') {
        snprintf(sbk_launch_target, sizeof(sbk_launch_target), "%s", e->exe);
        printf("sbk: handing over to %s (%s)\n", e->title, e->exe);
    }
    *start = 1;
}

extern int sbk_perf_enabled;

int sbk_launcher_run(void) {
    int quit = 0, start = 0;
    unsigned last_ms, perf_since;
    int perf_frames = 0;
    float perf_ms = 0.0f, perf_draw_ms = 0.0f;
    if (sbk_settings_scripted || !sbk_settings.launcher) return 1;
    sbk_ui_launcher_active = 1;
    ui_sync_shadows();
    memset(&nav, 0, sizeof(nav));
    memset(&norom_nav, 0, sizeof(norom_nav));
    page = PAGE_PICK;
    page_fade = 0.0f;
    pick = sbk_game_index(sbk_settings.game);
    if (!game_reachable(pick)) pick = sbk_game_self_index();
    sbk_ui_rom_art_init();
    printf("sbk: launcher (--nolauncher skips it)\n");
    last_ms = SDL_GetTicks();
    perf_since = last_ms;

    while (!start && !quit) {
        struct SbkUiRaw r;
        struct UiItem *items;
        int count, win_w, win_h;
        unsigned now = SDL_GetTicks();
        float dt = (float)(now - last_ms) / 1000.0f;
        last_ms = now;
        if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;

        gfx_handle_events();
        if (sbk_input_quit_requested()) { quit = 1; break; }
        sbk_ui_input_raw(&r);
        ui_script_apply(&r);

        sbk_ui_scene_step(dt);
        /* nothing slower than 200 ms: a page settles in about 160 */
        page_fade += dt / 0.16f;
        if (page_fade > 1.0f) page_fade = 1.0f;
        {
            float want = page == PAGE_PICK ? 1.0f : 0.0f;
            float k = dt / 0.18f;
            if (scene_blend < want) { scene_blend += k; if (scene_blend > want) scene_blend = want; }
            else if (scene_blend > want) { scene_blend -= k; if (scene_blend < want) scene_blend = want; }
        }

        if (norom_open) {
            if (edge(&norom_nav.hold[0], r.up)) move_cursor(&norom_nav, norom_items, NOROM_N, -1);
            if (edge(&norom_nav.hold[1], r.down)) move_cursor(&norom_nav, norom_items, NOROM_N, 1);
            if (edge(&norom_nav.hold[4], r.accept)) {
                if (norom_items[norom_nav.cursor].action == ACT_OPENFOLDER) sbk_rom_open_folder();
                else { norom_open = 0; page_fade = 0.0f; sbk_rom_rescan(); sbk_ui_scene_invalidate(); }
            }
            if (edge(&norom_nav.hold[5], r.cancel)) {
                /* leaving the note is the moment to look again: the player has
                 * just been in the Finder with the folder open */
                norom_open = 0;
                page_fade = 0.0f;
                sbk_rom_rescan();
                sbk_ui_scene_invalidate();
            }
        } else if (page == PAGE_PICK) {
            int n = sbk_game_count();
            if (edge(&nav.hold[2], r.left) || edge(&nav.hold[0], r.up)) {
                pick = (pick + n - 1) % n;
                page_fade = 0.5f;
            }
            if (edge(&nav.hold[3], r.right) || edge(&nav.hold[1], r.down)) {
                pick = (pick + 1) % n;
                page_fade = 0.5f;
            }
            if (edge(&nav.hold[4], r.accept)) {
                if (!rom_ok(pick) || !game_reachable(pick)) {
                    norom_open = 1;
                    norom_nav.cursor = 0;
                    memset(norom_nav.hold, 0, sizeof(norom_nav.hold));
                    page_fade = 0.0f;
                } else {
                    strncpy(sbk_settings.game, sbk_game_at(pick)->id, sizeof(sbk_settings.game) - 1);
                    sbk_settings_save();
                    goto_page(PAGE_MODE, mode_items, MODE_N);
                }
            }
            if (edge(&nav.hold[5], r.cancel)) quit = 1;
        } else {
            items = page == PAGE_MODE ? mode_items : options_items;
            count = page == PAGE_MODE ? MODE_N : OPTIONS_N;
            clamp_cursor(&nav, items, count);
            if (edge(&nav.hold[0], r.up)) move_cursor(&nav, items, count, -1);
            if (edge(&nav.hold[1], r.down)) move_cursor(&nav, items, count, 1);
            if (edge(&nav.hold[2], r.left)) adjust(&items[nav.cursor], -1);
            if (edge(&nav.hold[3], r.right)) adjust(&items[nav.cursor], 1);
            if (edge(&nav.hold[4], r.accept)) {
                struct UiItem *it = &items[nav.cursor];
                switch (it->type) {
                    case IT_MODE:
                        if (it->arg == SBK_MODE_CUSTOM) sbk_settings.mode = SBK_MODE_CUSTOM;
                        else sbk_settings_apply_mode(it->arg);
                        ui_sync_shadows();
                        sbk_settings_apply();
                        sbk_settings_save();
                        break;
                    case IT_ACTION:
                        if (it->action == ACT_OPTIONS) goto_page(PAGE_OPTIONS, options_items, OPTIONS_N);
                        else if (it->action == ACT_START) start_selected(&start);
                        else if (it->action == ACT_QUIT) quit = 1;
                        else if (it->action == ACT_BACK) {
                            if (page == PAGE_OPTIONS) goto_page(PAGE_MODE, mode_items, MODE_N);
                            else goto_page(PAGE_PICK, NULL, 0);
                        } else if (it->action == ACT_DEFAULTS) {
                            sbk_settings_player_defaults();
                            ui_sync_shadows();
                            sbk_settings.fullscreen = ui_fullscreen_shadow;
                            sbk_settings_apply();
                            sbk_settings_save();
                        }
                        break;
                    default:
                        adjust(it, 1);
                        break;
                }
            }
            if (edge(&nav.hold[5], r.cancel)) {
                if (page == PAGE_OPTIONS) goto_page(PAGE_MODE, mode_items, MODE_N);
                else goto_page(PAGE_PICK, NULL, 0);
            }
        }

        sbk_gfx_window_size(&win_w, &win_h);
        {
            /* --perf on the launcher: what one of its frames costs, separately
             * from the wait for the vertical retrace, so "the launcher must not
             * cost more than the game" is a number and not a hope. */
            unsigned t0 = SDL_GetTicks();
            draw_launcher(win_w, win_h);
            perf_draw_ms += (float)(SDL_GetTicks() - t0);
            sbk_gfx_swap();
            perf_frames++;
            perf_ms += (float)(SDL_GetTicks() - t0);
            if (sbk_perf_enabled && now - perf_since >= 1000) {
                printf("sbk-launcher: %.1f Hz  draw %.2f ms  frame %.2f ms\n",
                       perf_frames * 1000.0f / (float)(now - perf_since),
                       perf_draw_ms / (float)perf_frames,
                       perf_ms / (float)perf_frames);
                perf_since = now;
                perf_frames = 0;
                perf_draw_ms = 0.0f;
                perf_ms = 0.0f;
            }
        }
        SDL_Delay(1);
    }
    sbk_ui_launcher_active = 0;
    if (start) {
        sbk_settings_save();
        sbk_settings_apply();
    } else {
        /* Cmd+Q or Esc out of the launcher is a quit like any other: the mode
         * and the options the player was just looking at are what they get
         * next time, so the file goes down before the process does. */
        sbk_settings_save();
    }
    return start;
}

/* --- the in-game overlay -------------------------------------------------- */

static int overlay_open;
static int menu_hold;
static struct Nav onav;

int sbk_ui_overlay_open(void) { return overlay_open; }

int sbk_ui_overlay_tick(void) {
    struct SbkUiRaw r;
    if (sbk_settings_scripted) return 0;
    sbk_ui_input_raw(&r);
    ui_script_apply(&r);
    if (r.menu && menu_hold == 0) {
        overlay_open = !overlay_open;
        if (overlay_open) {
            ui_sync_shadows();
            onav.cursor = 0;
            memset(onav.hold, 0, sizeof(onav.hold));
            clamp_cursor(&onav, options_items, OPTIONS_N);
        } else {
            sbk_settings_save();
        }
        sbk_ui_owns_input = overlay_open;
    }
    menu_hold = r.menu;
    if (!overlay_open) return 0;

    if (edge(&onav.hold[0], r.up)) move_cursor(&onav, options_items, OPTIONS_N, -1);
    if (edge(&onav.hold[1], r.down)) move_cursor(&onav, options_items, OPTIONS_N, 1);
    if (edge(&onav.hold[2], r.left)) adjust(&options_items[onav.cursor], -1);
    if (edge(&onav.hold[3], r.right)) adjust(&options_items[onav.cursor], 1);
    if (edge(&onav.hold[4], r.accept)) {
        struct UiItem *it = &options_items[onav.cursor];
        if (it->type == IT_ACTION) {
            if (it->action == ACT_BACK) { overlay_open = 0; sbk_settings_save(); }
            else if (it->action == ACT_QUIT) { sbk_settings_save(); sbk_input_request_quit(); }
            else if (it->action == ACT_DEFAULTS) {
                sbk_settings_player_defaults();
                ui_sync_shadows();
                sbk_settings.fullscreen = ui_fullscreen_shadow;
                sbk_settings_apply();
                sbk_settings_save();
            }
        } else {
            adjust(it, 1);
        }
    }
    if (edge(&onav.hold[5], r.cancel)) { overlay_open = 0; sbk_settings_save(); }
    sbk_ui_owns_input = overlay_open;
    return overlay_open;
}

void sbk_ui_overlay_draw(int win_w, int win_h, int out_x, int out_y, int out_w, int out_h) {
    static const char *const hint = "F1 / VIEW CLOSES";
    char head[64];
    int scale, pw, ph, px, py;
    if (!overlay_open) return;
    scale = sbk_ui_pick_scale(win_h);
    if (scale > 1 && panel_height(options_items, OPTIONS_N, scale, 1) > win_h) scale--;
    pw = panel_width(options_items, OPTIONS_N, scale, hint);
    ph = panel_height(options_items, OPTIONS_N, scale, 1);
    /* Draw over the left letterbox bar if it is wide enough for the panel;
     * otherwise let the panel lie translucently over the frame's left edge. */
    if (out_x >= pw) px = (out_x - pw) / 2;
    else px = scale * 6;
    if (pw > win_w - scale * 12) { pw = win_w - scale * 12; px = scale * 6; }
    py = (win_h - ph) / 2;
    if (py < scale * 6) py = scale * 6;
    (void)out_y; (void)out_w; (void)out_h;

    snprintf(head, sizeof(head), "OPTIONS   -   %s", mode_names[sbk_settings.mode]);
    sbk_ui_begin(win_w, win_h);
    draw_panel(head, options_items, OPTIONS_N, onav.cursor, px, py, pw, ph, scale, hint, 1.0f);
    sbk_ui_end();
}
