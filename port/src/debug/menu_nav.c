/* Campaign navigator: getting the self-playing session to *save*.
 *
 * The purse only reaches the Controller Pak from the Game Menu's EXIT / SAVE
 * (race_type_select_menu.c: selection 3, or B, sets gMenuExitSelection and
 * fades out into the save flow). A monkey pressing A walks from the results
 * screen straight back into the next race and never stops there, so the
 * campaign earned money for an hour and saved none of it.
 *
 * This file is a *navigator* instead: it knows which screen is up and presses
 * what that screen needs. The screen is read from the game's own task list --
 * gActiveGameTaskList is a priority-ordered list of GameTask, each with the
 * callback it will run next -- and the callbacks are named with dladdr(), so
 * --menutrace prints the real function names without a hand-written table.
 *
 * Never START: a START still queued when a race begins pauses it, and only
 * another START dismisses the overlay.
 */
#include "../ultra/ultra.h"
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include "game/race/player/race_player_input.h"
#include "game/race/player/race_player_update.h"
#include "game/race/race_state.h"
#include "game/engine/game_task_scheduler.h"
#include "game/save_data.h"
#include "game/menu/course_select/course_select_menu.h"
#include "game/menu/character_select/character_select_menu.h"
#include "game/menu/main_menu/controller_main_menu_flow.h"
#include "game/menu/controller_pak/controller_pak_menu.h"
#include "../platform/input.h"

extern GameTask gActiveGameTaskList;
extern u8 gHighestUnlockedCourse;

int sbk_menutrace;      /* --menutrace: print the active task callbacks as they change */

/* The callback of the highest-priority active task, plus its name. */
static GameTaskCallback current_callbacks[8];
static int current_count;

static void collect_tasks(void) {
    GameTask *t = gActiveGameTaskList.next;
    current_count = 0;
    while (t != NULL && current_count < 8) {
        current_callbacks[current_count++] = t->callbacks[0];
        t = t->next;
    }
}

const char *sbk_fn_name(void *fn) {
    static char buf[128];
    Dl_info info;
    if (fn == NULL) return "-";
    if (dladdr(fn, &info) && info.dli_sname != NULL) {
        snprintf(buf, sizeof(buf), "%s", info.dli_sname);
        return buf;
    }
    snprintf(buf, sizeof(buf), "%p", fn);
    return buf;
}

/* True when a task whose next callback is `name` is on the active list. */
int sbk_menu_on(const char *name) {
    int i;
    for (i = 0; i < current_count; i++) {
        if (strcmp(sbk_fn_name((void *)current_callbacks[i]), name) == 0) return 1;
    }
    return 0;
}

const char *sbk_menu_top(void) {
    return current_count ? sbk_fn_name((void *)current_callbacks[0]) : "-";
}

/* Every active task's callback name, joined: the screen's real identity. The
 * first task is usually the parent *flow*, which does not change while a whole
 * menu runs, so watching only the top name never sees the screen move. */
const char *sbk_menu_names(void) {
    static char buf[512];
    int i;
    buf[0] = 0;
    for (i = 0; i < current_count; i++) {
        strncat(buf, sbk_fn_name((void *)current_callbacks[i]), sizeof(buf) - strlen(buf) - 2);
        strncat(buf, " ", sizeof(buf) - strlen(buf) - 2);
    }
    return buf;
}

/* The task whose next callback is `name` (gCurrentGameTask is NULL between the
 * scheduler's dispatches, so a navigator cannot use it to reach a menu's own
 * callbackData). */
static GameTask *menu_task(const char *name) {
    GameTask *t = gActiveGameTaskList.next;
    while (t != NULL) {
        if (strcmp(sbk_fn_name((void *)t->callbacks[0]), name) == 0) return t;
        t = t->next;
    }
    return NULL;
}

static void menutrace(unsigned long retraces) {
    static char last[512];
    char now[512];
    int i;
    now[0] = 0;
    for (i = 0; i < current_count; i++) {
        strncat(now, sbk_fn_name((void *)current_callbacks[i]), sizeof(now) - strlen(now) - 2);
        strncat(now, " ", sizeof(now) - strlen(now) - 2);
    }
    if (strcmp(now, last) != 0) {
        snprintf(last, sizeof(last), "%s", now);
        printf("sbk-menu: r=%lu ms=%d money=%d tasks: %s\n", retraces, gRacePlayers[0].menuState,
               gRacePlayers[0].money, now);
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ autonav
 *
 * The map of the post-race menus, read out of race_flow.c:
 * handleRaceSplitscreenSelectFlow branches on gRaceSplitscreenMode, and that
 * menu (updateRaceSplitscreenSelectMenu, five entries) is the "Game Menu":
 *
 *   0, 2 -> the plain course list, i.e. straight into the next race
 *   1    -> the race type menu
 *   3    -> the course shop (initCourseSelectMenu, where courses are bought)
 *   4    -> EXIT / SAVE (initControllerPakRaceRecordSaveFlow, the only path
 *           that writes the Controller Pak)
 *
 * So the navigator does not need to count D-pad presses down the list: it
 * parks gRaceSplitscreenMode on the entry it wants (the menu's own variable,
 * the same trick --trial uses for the course cursor) and lets an A press
 * confirm it, which is the game's normal flow, just aimed.
 *
 * Every other screen answers A, and the prompts want YES (stick up) first.
 * Never START: a queued START pauses the next race, and only another START
 * clears the overlay.
 */
int sbk_autonav;        /* --autonav */
int sbk_autonav_every = 1;  /* save after every N races */
/* --shop: try to buy a course after a save. Off by default -- see the note on
 * nav_pick_course: the shop the Game Menu leads to sells boards and paint, not
 * courses, so this walks into a dead end. */
int sbk_autonav_shop;

enum { NAV_RACE = 0, NAV_SAVE = 1, NAV_SHOP = 2 };
static int nav_want = NAV_RACE;
static int nav_races, nav_saves;
static int nav_save_flow_seen;  /* the pak save flow has run since SAVE was asked for */
static unsigned long nav_last_action;

extern u8 gRaceSplitscreenMode;

/* The shop (gRaceSplitscreenMode 3 -> initCourseSelectMenu).
 *
 * Its course grid is three columns of three, and course_select_menu.c builds
 * the selection from the menu's own cursors:
 *   menuSelection = gMenuChoicePromptState[0] * 3 + (column) - 6
 * so course K sits at column K % 3, row K / 3, i.e. cursor
 * gCharacterSelectHighlightedRosterIndices[0] = K % 3 and
 * gMenuChoicePromptState[0] = K / 3 + 2. A on a course whose unlock state is
 * -1 buys it when the purse covers gCourseUnlockPrices[K]. As everywhere else
 * here, the navigator parks the game's own cursors and presses A.
 */
static int nav_buy = -1;        /* the course being bought, -1 = none */

/* Which course to buy next: the cheapest one still for sale that the purse
 * covers. NOTE (2026-09-11): the shop reached from the Game Menu's third entry
 * (initCourseSelectMenu -> updateCourseSelectModeMenu) turned out to sell
 * BOARDS (row 0: FREE STYLE / ALL AROUND / ALPINE) and PAINT (row 1), with row
 * 2 the way out -- screenshots g4-shots/shop-stuck.png and shop3.png. So the
 * course prices in gCourseUnlockPrices are spent somewhere else, most likely
 * from the pre-race course list itself, and this step is off unless --shop is
 * given. The campaign does not need it: the port raises gHighestUnlockedCourse
 * so every course is offered, and a locked course still counts as a win --
 * this session reached progression level 1 (wins on 0-4 and 9) without buying
 * anything. */
static int nav_pick_course(void) {
    const GameSaveData *sd = &gGameSaveDataBuffer[0];
    int best = -1, k;
    if (!sbk_autonav_shop) return -1;
    for (k = 0; k < 9; k++) {
        if (sd->courseUnlockStates[k] != -1) continue;
        if ((u32)gRacePlayers[0].money < gCourseUnlockPrices[k]) continue;
        if (best < 0 || gCourseUnlockPrices[k] < gCourseUnlockPrices[best]) best = k;
    }
    return best;
}

/* A race has ended when the results flow comes up; the purse is only in the
 * pak once the save data's copy matches the rider's. */
static void nav_watch(void) {
    static int in_results;
    int results = sbk_menu_on("updateRaceResultsFlow") || sbk_menu_on("prepareRaceResultsFlow");
    if (results && !in_results) {
        nav_races++;
        if (nav_races % sbk_autonav_every == 0) {
            nav_want = NAV_SAVE;
            nav_save_flow_seen = 0;
        }
        printf("sbk-nav: race %d finished, want=%s\n", nav_races, nav_want == NAV_SAVE ? "SAVE" : "RACE");
        fflush(stdout);
    }
    in_results = results;
    if (sbk_menu_on("updateControllerPakRaceRecordSaveFlow")) nav_save_flow_seen = 1;
    /* Not just savemoney == money: the purse is credited a little after the
     * results screen comes up, so the two match for a moment before the race's
     * winnings land and the save would look done before it had run. */
    if (nav_want == NAV_SAVE && nav_save_flow_seen &&
        (int)gGameSaveDataBuffer[0].money == gRacePlayers[0].money && gRacePlayers[0].money != 0) {
        nav_saves++;
        nav_buy = nav_pick_course();
        nav_want = nav_buy >= 0 ? NAV_SHOP : NAV_RACE;
        printf("sbk-nav: saved %d (save #%d)%s\n", gRacePlayers[0].money, nav_saves,
               nav_buy >= 0 ? " -> shop" : "");
        if (nav_buy >= 0) {
            printf("sbk-nav: buying course %d for %u (purse %d)\n", nav_buy,
                   (unsigned)gCourseUnlockPrices[nav_buy], gRacePlayers[0].money);
        }
        fflush(stdout);
    }
    if (nav_want == NAV_SHOP && nav_buy >= 0 && gGameSaveDataBuffer[0].courseUnlockStates[nav_buy] != -1) {
        printf("sbk-nav: course %d bought (purse %d)\n", nav_buy, gRacePlayers[0].money);
        fflush(stdout);
        nav_buy = -1;
        nav_want = NAV_SAVE;   /* a purchase is worth writing to the pak at once */
    }
}

static void nav_press(const char *line) {
    sbk_input_play_add(line);
}

static void nav_act(unsigned long retraces) {
    static char last_top[512];
    static unsigned long top_since;
    const char *top = sbk_menu_names();
    if (strcmp(top, last_top) != 0) {
        snprintf(last_top, sizeof(last_top), "%s", top);
        top_since = retraces;
    }
    if (retraces - nav_last_action < 60) return;
    nav_last_action = retraces;
    /* Watchdog: a screen that has not moved in 40 s of game time is one the
     * navigator does not understand. Back out with B and, if a purchase was
     * under way, give it up -- an unattended session must not sit on a menu
     * for an hour, which is how the board shop swallowed the first run. */
    if (retraces - top_since > 3600) {
        printf("sbk-nav: stuck on %s for %lu retraces, backing out\n", top, retraces - top_since);
        fflush(stdout);
        top_since = retraces;
        if (nav_buy >= 0) {
            nav_buy = -1;
            nav_want = NAV_RACE;
        }
        nav_press("press B 3");
        return;
    }
    /* In the shop. Its front page is a three-row menu in
     * gCourseSelectModeSelection: 0 the course shop, 1 the board shop
     * (FREE STYLE / ALL AROUND / ALPINE, which is where an unaimed run ended
     * up and sat), 2 RETURN. */
    if (sbk_menu_on("updateCourseSelectModeMenu")) {
        /* Measured, not guessed: with the front page left on 0 the shop opens
         * the BOARD list (FREE STYLE / ALL AROUND / ALPINE), so the course
         * shop is 1 -- which is also the only value that routes the confirm to
         * updateCourseSelectUnlockCourseList (course_select_menu.c). 2 is
         * RETURN. */
        gCourseSelectModeSelection = (u8)(nav_buy >= 0 ? 1 : 2);
        printf("sbk-nav: shop front page, mode=%d buy=%d\n", gCourseSelectModeSelection, nav_buy);
        fflush(stdout);
        nav_press("press A 3");
        return;
    }
    if (sbk_menu_on("updateCourseSelectPurchasePrompt")) {
        /* callbackData1: 0 = buy, 1 = cancel; 2 and up is the purchase
         * animation, which answers nothing -- pressing A into it only stalls. */
        GameTask *t = menu_task("updateCourseSelectPurchasePrompt");
        if (t != NULL && t->callbackData1 < 2) {
            t->callbackData1 = 0;
            nav_press("press A 3");
        }
        return;
    }
    if (sbk_menu_on("updateCourseSelectUnlockCourseList") || sbk_menu_on("updateCourseSelectCourseList")) {
        if (nav_buy >= 0) {
            gCharacterSelectHighlightedRosterIndices[0] = (s8)(nav_buy % 3);
            gMenuChoicePromptState[0] = (s16)(nav_buy / 3 + 2);
            printf("sbk-nav: shop list, aim course %d (col %d row %d) sel=%d state=%d\n", nav_buy,
                   nav_buy % 3, nav_buy / 3, gRacePlayers[0].menuSelection,
                   gGameSaveDataBuffer[0].courseUnlockStates[nav_buy]);
            fflush(stdout);
            nav_press("press A 3");
        } else {
            nav_press("press B 3");
        }
        return;
    }
    /* The save menu asks USE THIS SAVE / START A NEW GAME (gMenuChoicePromptState
     * 3 and 4, race_setup_menu.c). A plain A took the new game every time, which
     * is why a restarted campaign always began at 0G even with a saved pak:
     * park the choice on USE (3) whenever the prompt is up. */
    if (sbk_menu_on("updateRaceSetupSaveMenu")) {
        if (gMenuChoicePromptState[0] == 4) gMenuChoicePromptState[0] = 3;
        nav_press("press A 3");
        return;
    }
    if (sbk_menu_on("updateRaceSplitscreenSelectMenu")) {
        gRaceSplitscreenMode = (u8)(nav_want == NAV_SAVE ? 4 : nav_want == NAV_SHOP ? 3 : 0);
        nav_press("press A 3");
        return;
    }
    /* The save flow's two choices. Both default to the answer that backs out,
     * and a queued A lands on the prompt the frame it appears, so stick-up
     * arrives too late: park the choice instead.
     *   ARE YOU SURE? -> gControllerPakMenuState.confirmChoice 0 = YES
     *   DATA SAVE     -> gMenuChoicePromptState[0] 3 = SAVE (4 backs out) */
    if (sbk_menu_on("updateControllerPakRaceRecordSaveOverwritePrompt")) {
        gControllerPakMenuState.confirmChoice = 0;
        nav_press("press A 3");
        return;
    }
    if (sbk_menu_on("updateControllerPakRaceRecordSaveFlow")) {
        if (gMenuChoicePromptState[0] == 4) gMenuChoicePromptState[0] = 3;
        nav_press("press A 3");
        return;
    }
    /* Prompts (the pak flow's ARE YOU SURE? / DATA SAVE) want YES, which is up. */
    if (strstr(top, "Prompt") != NULL || strstr(top, "ControllerPak") != NULL ||
        strstr(top, "Confirm") != NULL) {
        nav_press("stick 0 80 3");
        nav_press("wait 6");
        nav_press("press A 3");
        return;
    }
    nav_press("press A 3");
}

void sbk_menu_nav_tick(unsigned long retraces) {
    collect_tasks();
    if (sbk_menutrace) menutrace(retraces);
    if (!sbk_autonav) return;
    nav_watch();
    if (sbk_menu_on("updateRaceGameplayFlow") || sbk_menu_on("startRaceGameplayFlow") ||
        sbk_menu_on("fadeInRaceGameplayViewports") || sbk_menu_on("fadeOutRaceStartTransitionFlow")) {
        return;                 /* a race is under way: hands off the pad */
    }
    nav_act(retraces);
}
