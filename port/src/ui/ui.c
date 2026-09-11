/* The launcher and the in-game options overlay.
 *
 * Both are the same list widget drawn with ui_gl.c: the launcher owns the
 * whole window before the game boots, the overlay is a panel drawn over the
 * left of the presented frame (over the letterbox bar where one is wide
 * enough) while the game keeps running underneath with its controller
 * reading as idle. */
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "ui.h"
#include "ui_gl.h"
#include "../settings.h"
#include "../platform/input.h"

extern void gfx_handle_events(void);
void sbk_gfx_window_size(int *w, int *h);
void sbk_gfx_swap(void);
void sbk_gfx_set_fullscreen(int on);
int sbk_gfx_is_fullscreen(void);
int sbk_settings_derive_mode(void);
const char *sbk_settings_mode_name(int v);
const char *sbk_settings_res_name(int v);
const char *sbk_settings_filter_name(int v);
void sbk_games_probe(const char *rom_dir);

/* --- list model ---------------------------------------------------------- */

enum { IT_GAME, IT_CHOICE, IT_RANGE, IT_TOGGLE, IT_ACTION, IT_GAP };

enum { ACT_NONE, ACT_OPTIONS, ACT_START, ACT_QUIT, ACT_BACK, ACT_DEFAULTS, ACT_CLOSE };

struct UiItem {
    const char *label;
    int type;
    int *val;
    int lo, hi, step;
    const char *const *names;
    int action;
    int game_index;
};

static const char *const mode_names[]   = { "ORIGINAL", "ENHANCED", "CUSTOM" };
static const char *const res_names[]    = { "NATIVE", "N64 320x240", "2X 640x480" };
static const char *const filter_names[] = { "NONE", "SCANLINES", "GRILLE", "SMOOTH" };
static const char *const off_on[]       = { "OFF", "ON" };

static int ui_fullscreen_shadow;   /* the toggle's backing int */

static struct UiItem options_items[] = {
    { "Mode",          IT_CHOICE, &sbk_settings.mode,          0, 1, 1, mode_names,   0, -1 },
    { "Draw distance", IT_RANGE,  &sbk_settings.draw_distance, 1, 4, 1, NULL,         0, -1 },
    { "Resolution",    IT_CHOICE, &sbk_settings.resolution,    0, 2, 1, res_names,    0, -1 },
    { "Filter",        IT_CHOICE, &sbk_settings.filter,        0, 3, 1, filter_names, 0, -1 },
    { "Widescreen",    IT_TOGGLE, &sbk_settings.widescreen,    0, 1, 1, off_on,       0, -1 },
    { "Fullscreen",    IT_TOGGLE, &ui_fullscreen_shadow,       0, 1, 1, off_on,       0, -1 },
    { "V-sync",        IT_TOGGLE, &sbk_settings.vsync,         0, 1, 1, off_on,       0, -1 },
    { "Volume",        IT_RANGE,  &sbk_settings.volume,        0, 100, 5, NULL,       0, -1 },
    { "Perf readout",  IT_TOGGLE, &sbk_settings.perf,          0, 1, 1, off_on,       0, -1 },
    { "",              IT_GAP,    NULL, 0, 0, 0, NULL, 0, -1 },
    { "Restore defaults", IT_ACTION, NULL, 0, 0, 0, NULL, ACT_DEFAULTS, -1 },
    { "Back",          IT_ACTION, NULL, 0, 0, 0, NULL, ACT_BACK, -1 },
};
#define OPTIONS_N ((int)(sizeof(options_items) / sizeof(options_items[0])))

/* The main page is built at runtime: one row per registered game. */
static struct UiItem main_items[8];
static int main_n;

static void build_main_page(void) {
    int i;
    main_n = 0;
    for (i = 0; i < sbk_game_count() && main_n < 6; i++) {
        struct UiItem *it = &main_items[main_n++];
        memset(it, 0, sizeof(*it));
        it->label = sbk_game_at(i)->title;
        it->type = IT_GAME;
        it->game_index = i;
    }
    {
        struct UiItem gap = { "", IT_GAP, NULL, 0, 0, 0, NULL, 0, -1 };
        struct UiItem mode = { "Mode", IT_CHOICE, &sbk_settings.mode, 0, 1, 1, mode_names, 0, -1 };
        struct UiItem opts = { "Options", IT_ACTION, NULL, 0, 0, 0, NULL, ACT_OPTIONS, -1 };
        struct UiItem start = { "Start", IT_ACTION, NULL, 0, 0, 0, NULL, ACT_START, -1 };
        struct UiItem quit = { "Quit", IT_ACTION, NULL, 0, 0, 0, NULL, ACT_QUIT, -1 };
        main_items[main_n++] = gap;
        main_items[main_n++] = mode;
        main_items[main_n++] = opts;
        main_items[main_n++] = start;
        main_items[main_n++] = quit;
    }
}

static int item_selectable(const struct UiItem *it) {
    if (it->type == IT_GAP) return 0;
    if (it->type == IT_GAME) return sbk_game_at(it->game_index)->installed;
    return 1;
}

/* --- scripted UI input (--uiscript) --------------------------------------
 * The G4 is driven over SSH and synthetic key events do not reach a
 * fullscreen SDL window, so the launcher and the overlay are exercised (and
 * screenshotted) from a token string instead: u d l r = directions, a = A,
 * b = B, m = the overlay button, w = wait a second, . = wait a tick. One
 * token per SBK_UISCRIPT_TICKS ticks, pressed on the first of them. */
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
    struct SbkUiRaw prev;
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

static void adjust(struct UiItem *it, int dir) {
    int v;
    if (it->val == NULL) return;
    v = *it->val + dir * it->step;
    if (v < it->lo) v = it->hi;
    if (v > it->hi) v = it->lo;
    *it->val = v;
    if (it->val == &sbk_settings.mode) {
        sbk_settings_apply_mode(v);
    } else if (it->val == &ui_fullscreen_shadow) {
        sbk_gfx_set_fullscreen(v);
        sbk_settings.fullscreen = v;
    } else {
        sbk_settings.mode = sbk_settings_derive_mode();
    }
    sbk_settings_apply();
    sbk_settings_save();
}

/* --- drawing ------------------------------------------------------------- */

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
        case IT_GAME:
            snprintf(out, n, "%s", sbk_game_at(it->game_index)->installed ? "" : "not installed");
            break;
        default:
            break;
    }
}

/* One list, laid out in a box of `w` pixels starting at (x, y). Returns the
 * height it used. */
static int draw_list(struct UiItem *items, int count, int cursor, int x, int y, int w, int scale) {
    int row_h = sbk_ui_text_h(scale) + scale * 4;
    int i, cy = y;
    char val[64];
    for (i = 0; i < count; i++) {
        struct UiItem *it = &items[i];
        struct SbkColor fg = SBK_UI_FG;
        int selected = (i == cursor);
        if (it->type == IT_GAP) { cy += row_h / 2; continue; }
        if (!item_selectable(it)) fg = SBK_UI_DIM;
        if (selected) {
            sbk_ui_rect(x - scale * 3, cy - scale, w + scale * 6, row_h, SBK_UI_LINE);
            fg = SBK_UI_SEL;
        }
        if (it->type == IT_GAME && item_selectable(it)) {
            int chosen = sbk_game_index(sbk_settings.game) == it->game_index;
            sbk_ui_text(x, cy + scale, scale, chosen ? "\3" : " ", SBK_UI_ACCENT);
        }
        sbk_ui_text(x + (it->type == IT_GAME ? sbk_ui_text_w("  ", scale) : 0), cy + scale, scale, it->label, fg);
        value_text(it, val, sizeof(val));
        if (val[0] != '\0') {
            int vw = sbk_ui_text_w(val, scale);
            /* the < > arrows live inside the panel: always reserve their
               column so the value does not jump when a row is selected */
            int vx = x + w - vw - sbk_ui_text_w("  ", scale);
            struct SbkColor vc = selected ? SBK_UI_SEL : (item_selectable(it) ? SBK_UI_ACCENT : SBK_UI_DIM);
            if (selected && it->type != IT_GAME && it->type != IT_ACTION) {
                sbk_ui_text(vx - sbk_ui_text_w("  ", scale), cy + scale, scale, "\1", SBK_UI_FG);
                sbk_ui_text(x + w - sbk_ui_text_w(" ", scale), cy + scale, scale, "\2", SBK_UI_FG);
            }
            sbk_ui_text(vx, cy + scale, scale, val, vc);
        }
        cy += row_h;
    }
    return cy - y;
}

/* The whole options page in a panel: title, list, hint. */
static void draw_panel(const char *title, struct UiItem *items, int count, int cursor,
                       int px, int py, int pw, int ph, int scale, const char *hint) {
    int pad = scale * 8;
    sbk_ui_rect(px, py, pw, ph, SBK_UI_PANEL);
    sbk_ui_border(px, py, pw, ph, scale, SBK_UI_LINE);
    sbk_ui_text_shadow(px + pad, py + pad, scale, title, SBK_UI_ACCENT);
    sbk_ui_rect(px + pad, py + pad + sbk_ui_text_h(scale) + scale * 2, pw - 2 * pad, scale, SBK_UI_LINE);
    draw_list(items, count, cursor, px + pad, py + pad + sbk_ui_text_h(scale) + scale * 8,
              pw - 2 * pad, scale);
    if (hint != NULL) {
        sbk_ui_text(px + pad, py + ph - pad - sbk_ui_text_h(scale), scale > 1 ? scale - 1 : 1, hint, SBK_UI_DIM);
    }
}

static int list_height(struct UiItem *items, int count, int scale) {
    int row_h = sbk_ui_text_h(scale) + scale * 4;
    int i, h = 0;
    for (i = 0; i < count; i++) h += items[i].type == IT_GAP ? row_h / 2 : row_h;
    return h;
}

/* Width the list needs: the widest label plus the widest value on the same
 * row, plus room for the < > arrows the selected row draws. Measured rather
 * than guessed, or "Snowboard Kids 2" runs into "not installed". */
static int list_width(struct UiItem *items, int count, int scale) {
    int i, w = 0;
    char val[64];
    for (i = 0; i < count; i++) {
        int lw = sbk_ui_text_w(items[i].label, scale);
        int row;
        if (items[i].type == IT_GAME) lw += sbk_ui_text_w("  ", scale);
        value_text(&items[i], val, sizeof(val));
        row = lw + (val[0] != '\0' ? sbk_ui_text_w(val, scale) + sbk_ui_text_w("      ", scale) : 0);
        if (row > w) w = row;
    }
    /* the selected row hangs a "<" left of the value and a ">" past the edge */
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

/* --- the launcher -------------------------------------------------------- */

static struct Nav nav;
static int page;     /* 0 main, 1 options */

static void draw_launcher(int win_w, int win_h) {
    int scale = sbk_ui_pick_scale(win_h);
    struct UiItem *items = page == 0 ? main_items : options_items;
    int count = page == 0 ? main_n : OPTIONS_N;
    while (scale > 1 && panel_height(items, count, scale, 1) > win_h - sbk_ui_text_h(scale * 2) * 2) scale--;
    const char *hint = page == 0 ? "arrows/stick move   Enter/A select   Esc/B quit"
                                 : "left/right change   Enter/A select   Esc/B back";
    int pw = panel_width(items, count, scale, hint);
    int ph = panel_height(items, count, scale, 1);
    int px, py;
    struct SbkColor bg = { 0.03f, 0.05f, 0.10f, 1.0f };
    struct SbkColor bar = { 0.06f, 0.10f, 0.20f, 1.0f };
    if (pw > win_w - scale * 16) pw = win_w - scale * 16;
    px = (win_w - pw) / 2;
    py = (win_h - ph) / 2 + scale * 10;

    sbk_ui_begin(win_w, win_h);
    sbk_ui_rect(0, 0, win_w, win_h, bg);
    sbk_ui_rect(0, 0, win_w, win_h / 3, bar);
    {
        const char *title = "SNOWBOARD KIDS";
        const char *sub = "Power Mac G4 port";
        int tw = sbk_ui_text_w(title, scale * 2);
        sbk_ui_text_shadow((win_w - tw) / 2, py - sbk_ui_text_h(scale * 2) - scale * 18, scale * 2, title, SBK_UI_FG);
        sbk_ui_text((win_w - sbk_ui_text_w(sub, scale)) / 2, py - sbk_ui_text_h(scale) - scale * 6, scale, sub, SBK_UI_DIM);
    }
    draw_panel(page == 0 ? "PLAY" : "OPTIONS", items, count, nav.cursor, px, py, pw, ph, scale, hint);
    sbk_ui_end();
}

int sbk_launcher_run(void) {
    int quit = 0, start = 0;
    if (sbk_settings_scripted || !sbk_settings.launcher) return 1;
    build_main_page();
    ui_fullscreen_shadow = sbk_gfx_is_fullscreen();
    memset(&nav, 0, sizeof(nav));
    page = 0;
    clamp_cursor(&nav, main_items, main_n);
    printf("sbk: launcher (--nolauncher skips it)\n");

    while (!start && !quit) {
        struct SbkUiRaw r;
        struct UiItem *items;
        int count, win_w, win_h;
        gfx_handle_events();
        if (sbk_input_quit_requested()) { quit = 1; break; }
        sbk_ui_input_raw(&r);
        ui_script_apply(&r);
        items = page == 0 ? main_items : options_items;
        count = page == 0 ? main_n : OPTIONS_N;
        clamp_cursor(&nav, items, count);
        if (edge(&nav.hold[0], r.up)) move_cursor(&nav, items, count, -1);
        if (edge(&nav.hold[1], r.down)) move_cursor(&nav, items, count, 1);
        if (edge(&nav.hold[2], r.left)) adjust(&items[nav.cursor], -1);
        if (edge(&nav.hold[3], r.right)) adjust(&items[nav.cursor], 1);
        if (edge(&nav.hold[4], r.accept)) {
            struct UiItem *it = &items[nav.cursor];
            switch (it->type) {
                case IT_GAME:
                    strncpy(sbk_settings.game, sbk_game_at(it->game_index)->id, sizeof(sbk_settings.game) - 1);
                    sbk_settings_save();
                    break;
                case IT_TOGGLE:
                    adjust(it, 1);
                    break;
                case IT_ACTION:
                    if (it->action == ACT_OPTIONS) { page = 1; nav.cursor = 0; clamp_cursor(&nav, options_items, OPTIONS_N); }
                    else if (it->action == ACT_START) start = 1;
                    else if (it->action == ACT_QUIT) quit = 1;
                    else if (it->action == ACT_BACK) { page = 0; nav.cursor = 0; clamp_cursor(&nav, main_items, main_n); }
                    else if (it->action == ACT_DEFAULTS) {
                        sbk_settings_defaults();
                        ui_fullscreen_shadow = sbk_gfx_is_fullscreen();
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
            if (page != 0) { page = 0; nav.cursor = 0; clamp_cursor(&nav, main_items, main_n); }
            else quit = 1;
        }
        sbk_gfx_window_size(&win_w, &win_h);
        draw_launcher(win_w, win_h);
        sbk_gfx_swap();
        SDL_Delay(16);
    }
    if (start) {
        sbk_settings_save();
        sbk_settings_apply();
    }
    return start;
}

/* --- the in-game overlay -------------------------------------------------- */

static int overlay_open;
static int menu_hold;

int sbk_ui_overlay_open(void) { return overlay_open; }

int sbk_ui_overlay_tick(void) {
    struct SbkUiRaw r;
    if (sbk_settings_scripted) return 0;
    sbk_ui_input_raw(&r);
    ui_script_apply(&r);
    if (r.menu && menu_hold == 0) {
        overlay_open = !overlay_open;
        if (overlay_open) {
            ui_fullscreen_shadow = sbk_gfx_is_fullscreen();
            nav.cursor = 0;
            memset(nav.hold, 0, sizeof(nav.hold));
            clamp_cursor(&nav, options_items, OPTIONS_N);
        } else {
            sbk_settings_save();
        }
        sbk_ui_owns_input = overlay_open;
    }
    menu_hold = r.menu;
    if (!overlay_open) return 0;

    if (edge(&nav.hold[0], r.up)) move_cursor(&nav, options_items, OPTIONS_N, -1);
    if (edge(&nav.hold[1], r.down)) move_cursor(&nav, options_items, OPTIONS_N, 1);
    if (edge(&nav.hold[2], r.left)) adjust(&options_items[nav.cursor], -1);
    if (edge(&nav.hold[3], r.right)) adjust(&options_items[nav.cursor], 1);
    if (edge(&nav.hold[4], r.accept)) {
        struct UiItem *it = &options_items[nav.cursor];
        if (it->type == IT_ACTION) {
            if (it->action == ACT_BACK) { overlay_open = 0; sbk_settings_save(); }
            else if (it->action == ACT_DEFAULTS) {
                sbk_settings_defaults();
                ui_fullscreen_shadow = sbk_gfx_is_fullscreen();
                sbk_settings.fullscreen = ui_fullscreen_shadow;
                sbk_settings_apply();
                sbk_settings_save();
            }
        } else {
            adjust(it, 1);
        }
    }
    if (edge(&nav.hold[5], r.cancel)) { overlay_open = 0; sbk_settings_save(); }
    sbk_ui_owns_input = overlay_open;
    return overlay_open;
}

void sbk_ui_overlay_draw(int win_w, int win_h, int out_x, int out_y, int out_w, int out_h) {
    static const char *const hint = "F1 / View closes";
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

    sbk_ui_begin(win_w, win_h);
    draw_panel("OPTIONS", options_items, OPTIONS_N, nav.cursor, px, py, pw, ph, scale, hint);
    sbk_ui_end();
}
