/* --racedbg: print the race players' state once a second (port-side view of
 * the game's globals, which are pinned at their N64 addresses). */
#include "../ultra/ultra.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "game/race/player/race_player_input.h"
#include "game/race/player/race_player_update.h"
#include "game/race/race_state.h"
#include "game/engine/game_task_scheduler.h"
#include "game/menu/character_select/character_select_course_menu.h"
#include "game/save_data.h"
#include "../platform/input.h"
extern u8 gRaceDemoPlaybackEnabled;
#define RACE_PLAYER_READY_FLAG 0x40 /* race_flow.c: the rider has finished */

int sbk_race_debug_enabled;
int sbk_autoplay; /* --autoplay: player 1 is driven by the game's own CPU rider logic */
int sbk_soak;     /* --soak: outside a race, keep confirming through the menus (implies --autoplay) */
int sbk_nightmare;
int sbk_dumpon;      /* --dumpon: dump 30 frames (with triangles) the first time player 1 takes an item hit or spins out */

static void dumpon_tick(void) {
    static int last_mode = -1, fired;
    RacePlayer *p = &gRacePlayers[0];
    extern int sbk_dump_task, sbk_dump_frames, sbk_dump_tris;
    extern unsigned sbk_task_count;
    if (!sbk_dumpon || !p->isActive || gRaceDemoPlaybackEnabled) return;
    if (p->mode != last_mode) {
        printf("sbk-dumpon: mode %d -> %d hit=%04x st=%08x\n", last_mode, p->mode, (unsigned)p->pendingItemHitFlags, (unsigned)p->stateFlags);
        last_mode = p->mode;
    }
    {
        extern int sbk_frame_dump_left;
        static unsigned last_arm;
        if (fired < 6 && sbk_frame_dump_left == 0 && sbk_task_count > last_arm + 240 &&
            (p->pendingItemHitFlags != 0 || p->mode == 4 || p->mode == 5 || p->mode == 29)) {
            fired++;
            last_arm = sbk_task_count;
            sbk_dump_frames = 24;
            sbk_dump_tris = 1;
            sbk_dump_task = (int)sbk_task_count + 1;
        printf("sbk-dumpon: armed at task %d (hit=%04x mode=%d)\n", sbk_dump_task, (unsigned)p->pendingItemHitFlags, p->mode);
        }
    }
}

/* --trial char=N,board=N,action=N,item=N,boost=N,quit=1 (comma or space separated): a race experiment.
 * Outside a race the rider selection is pinned to char/board so the race
 * inits with them; during the race the chances are pinned and `boost` (in
 * 1/256ths) is added to the speed limit every retrace. When player 1
 * finishes, one result line is printed; with quit=1 the process exits. */
#define TRIAL_COURSE_NONE (-1)
#define TRIAL_COURSE_CAMPAIGN (-2) /* course=-2: aim at the next course still unwon */

static struct { int on, chr, board, action, item, boost, quit, course, money; } trial = { 0, -1, -1, -1, -1, 0, 0, TRIAL_COURSE_NONE, -1 };

/* The character-select course list: the menu keeps a *cursor index* into this
 * option table in gRaceCourseIndex and only converts it to the real course id
 * when the menu fades out (fadeOutCharacterSelectCourseMenu). So `course=N`
 * moves the cursor onto N every frame the course menu is up and lets the
 * script's own A press confirm it -- the normal flow, just aimed. */
typedef s16 CharacterSelectOptionList[10];
extern CharacterSelectOptionList *gCharacterSelectActiveCourseOptions;
extern u8 gHighestUnlockedCourse;
void updateCharacterSelectCourseMenu(void);
void updateCharacterSelectCourseSubmenu(void);
void handleCharacterSelectCourseSelection(void);

static int trial_course_seen = -99;

/* Aiming the course menu.
 *
 * The character-select course menu keeps a *cursor index* into
 * gCharacterSelectActiveCourseOptions in gRaceCourseIndex and only converts it
 * to the real course id when the menu fades out. So `course=N` parks the cursor
 * on N for as long as the list is live and lets the script's own A press
 * confirm it: the game's normal flow, just aimed.
 *
 * Identifying "the list is live" from outside: gCurrentGameTask is NULL between
 * dispatches, and gRacePlayers[0].isActive is 1 in the menus too, so neither is
 * usable. What works is the menu's own state -- gRacePlayers[0].menuState is 0
 * while the list is navigable and >= 7 once a choice is confirmed, and
 * gCharacterSelectCourseCursorState.listCursorState is zeroed by the menu's
 * init. Arm on (cursorState == 0 && menuState == 0), disarm on menuState >= 7.
 */
static int course_pin_armed;

/* --coursetrace: one line whenever the course index or the course menu's state
 * changes, which is how the menu's shape above was established. */
int sbk_course_trace;

static void course_trace(unsigned long retraces) {
    static int last = -12345, lastcur = -1, lastms = -1;
    if (!sbk_course_trace) return;
    if (gRaceCourseIndex.signedValue != last || gCharacterSelectCourseCursorState.listCursorState != lastcur ||
        gRacePlayers[0].menuState != lastms) {
        const s16 *o = (const s16 *)gCharacterSelectActiveCourseOptions;
        last = gRaceCourseIndex.signedValue;
        lastcur = gCharacterSelectCourseCursorState.listCursorState;
        lastms = gRacePlayers[0].menuState;
        printf("sbk-course: r=%lu idx=%d cur=%d ms=%d act=%d unlock=%d opts=%p [%d %d %d %d %d %d %d %d]\n",
               retraces, last, lastcur, lastms, gRacePlayers[0].isActive, gHighestUnlockedCourse, (void *)o,
               o ? o[0] : -9, o ? o[1] : -9, o ? o[2] : -9, o ? o[3] : -9,
               o ? o[4] : -9, o ? o[5] : -9, o ? o[6] : -9, o ? o[7] : -9);
        fflush(stdout);
    }
}

static void trial_course_pin(void) {
    const s16 *opt = (const s16 *)gCharacterSelectActiveCourseOptions;
    int ms = gRacePlayers[0].menuState;
    int want = trial.course;
    int i;
    if (trial.course == TRIAL_COURSE_NONE) return;
    /* The list the menu offers is picked at its init from gHighestUnlockedCourse
     * (0 -> courses 9,0-4; 1 -> +5; 2 -> +6). The game only ever raises it, so
     * raising it here, every frame, exposes every course to the trial. */
    if (gHighestUnlockedCourse < 2) gHighestUnlockedCourse = 2;
    if (opt == NULL) return;
    if (ms != 0) {
        if (ms >= 7) course_pin_armed = 0; /* confirmed: the index is a course id now */
        return;
    }
    if (gCharacterSelectCourseCursorState.listCursorState == 0) course_pin_armed = 1;
    if (!course_pin_armed) return;
    if (want == TRIAL_COURSE_CAMPAIGN) {
        /* The campaign's next objective: the first course in the menu's own
         * order that slot 0 has not won yet. cupPlacements[course] == 1 is the
         * "took first here" flag initRaceStartTransition reads to raise the
         * progression level (0-4 and 9 -> 1, +5 -> 2, +6 -> 3 and the credits). */
        want = 9;
        for (i = 0; i < 10 && opt[i] != -1; i++) {
            if (opt[i] >= 0 && opt[i] < 11 && gGameSaveDataBuffer[0].cupPlacements[opt[i]] != 1) {
                want = opt[i];
                break;
            }
        }
    }
    for (i = 0; i < 10 && opt[i] != -1; i++) {
        if (opt[i] == want) {
            if (gRaceCourseIndex.signedValue != i) {
                gRaceCourseIndex.signedValue = (s16)i;
                if (trial_course_seen != want) {
                    trial_course_seen = want;
                    printf("sbk-trial: course menu: cursor -> %d (course %d)\n", i, want);
                    fflush(stdout);
                }
            }
            return;
        }
    }
}
/* --plan COURSE:CHAR:BOARD:BOOST,...: the rider's learned book, one row per
 * course. With `course=-2` the campaign picks its own course, so the trial
 * cannot know in advance which setup it needs; the row for the course the race
 * actually started on is applied at the race's init. Rows come from
 * port/tools/nightmare_results.csv (`nightmare_search.py table`). */
#define PLAN_MAX 12
static struct { int course, chr, board, boost; } plan[PLAN_MAX];
static int nplan;

int sbk_plan_parse(const char *spec) {
    const char *p = spec;
    while (*p && nplan < PLAN_MAX) {
        int c, ch, b, bo;
        if (sscanf(p, "%d:%d:%d:%d", &c, &ch, &b, &bo) == 4) {
            plan[nplan].course = c; plan[nplan].chr = ch;
            plan[nplan].board = b; plan[nplan].boost = bo;
            nplan++;
        }
        while (*p && *p != ',') p++;
        while (*p == ',') p++;
    }
    printf("sbk: plan: %d course rows\n", nplan);
    return nplan;
}

/* The plan row for a course, applied into the trial's own fields. */
static void plan_apply(int course) {
    int i;
    for (i = 0; i < nplan; i++) {
        if (plan[i].course != course) continue;
        trial.chr = plan[i].chr;
        trial.board = plan[i].board;
        trial.boost = plan[i].boost;
        printf("sbk: plan: course %d -> char=%d board=%d boost=%d\n", course, trial.chr, trial.board, trial.boost);
        return;
    }
}

static unsigned long trial_start, trial_frames;
static int trial_money0, trial_done;

int sbk_trial_parse(const char *spec) {
    const char *p = spec;
    trial.on = 1;
    while (*p) {
        char key[16]; int val;
        if (sscanf(p, "%15[a-z]=%d", key, &val) == 2) {
            if (!strcmp(key, "char")) trial.chr = val;
            else if (!strcmp(key, "board")) trial.board = val;
            else if (!strcmp(key, "action")) trial.action = val;
            else if (!strcmp(key, "item")) trial.item = val;
            else if (!strcmp(key, "boost")) trial.boost = val;
            else if (!strcmp(key, "quit")) trial.quit = val;
            else if (!strcmp(key, "course")) trial.course = val;
            else if (!strcmp(key, "money")) trial.money = val;
        }
        while (*p && *p != ' ' && *p != ',') p++;
        while (*p == ' ' || *p == ',') p++;
    }
    sbk_autoplay = 1;
    return 0;
}

/* applyRacePlayerTuning, port side: the race inits the rider from the menu's
 * choice, so a trial re-tunes player 1 on its first active frame from the
 * character and board tables, with `boost` (1/256ths) on the top speed. */
static void trial_retune(RacePlayer *p) {
    const RacePlayerTuning *c = &gRacePlayerCharacterTuning[p->characterId % RACE_PLAYER_CHARACTER_TUNING_COUNT];
    const RacePlayerTuning *b = &gRacePlayerBoardTuning[p->characterVariant % RACE_PLAYER_BOARD_TUNING_COUNT];
    s32 top = (c->maxSpeed + b->maxSpeed) << 8;
    top += (s32)(((long long)top * trial.boost) >> 8);
    p->unk25C = top;
    p->speedLimit = top;
    p->gravity = (c->gravity + b->gravity) << 4;
    p->unk264 = (c->aerialGravity + b->aerialGravity) << 4;
    p->unk268 = c->turnStrength + b->turnStrength;
    p->unk274 = (c->lateralDeceleration + b->lateralDeceleration) << 4;
    p->unk26C = (c->turnRadiusAtFullLean + b->turnRadiusAtFullLean) << 4;
    p->unk270 = (c->turnRadiusAtHalfLean + b->turnRadiusAtHalfLean) << 4;
    p->unk278 = (c->forwardDeceleration + b->forwardDeceleration) << 4;
    p->unk27C = (c->reverseDeceleration + b->reverseDeceleration) << 4;
    if (p->isCpu) p->unk274 = (p->characterId == 5) ? 0xC0000 : 0x10000; /* the CPU rule in initRacePlayer */
}

/* Called when the race init has just reset player 1 (isCpu back to 0): the
 * game's own tuning ran a moment ago, so ours can replace it. */
static void trial_arm(RacePlayer *p, unsigned long retraces) {
    if (!trial.on || gRaceDemoPlaybackEnabled) return;
    trial_start = retraces;
    trial_money0 = p->money;
    trial_done = 0;
    plan_apply(gRaceCourseIndex.signedValue);
    if (trial.chr >= 0) { p->selectedCharacterId = (u8)trial.chr; p->characterId = (u8)trial.chr; }
    if (trial.board >= 0) p->characterVariant = (u8)trial.board;
    trial_retune(p);
    if (trial.money >= 0) p->money = trial.money;
    printf("sbk-trial: start r=%lu course=%d char=%d board=%d top=%d money=%d\n", retraces,
           gRaceCourseIndex.signedValue, p->characterId, p->characterVariant, p->unk25C, p->money);
}

static void trial_tick(unsigned long retraces) {
    RacePlayer *p = &gRacePlayers[0];
    if (!trial.on) return;
    trial_course_pin(); /* the menus run with isActive still set, so aim first */
    if (!p->isActive) {
        trial_start = 0;
        return;
    }
    if (trial_start == 0) return; /* armed by trial_arm() at the real race init */
    if (trial.action >= 0) p->actionTriggerChance = (u8)trial.action;
    if (trial.item >= 0) p->itemTriggerChance = (u8)trial.item;
    if (!trial_done && (p->stateFlags & RACE_PLAYER_READY_FLAG)) {
        int i, ahead = 0;
        trial_done = 1;
        trial_frames = retraces - trial_start;
        for (i = 1; i < RACE_PLAYER_COUNT; i++) {
            if (gRacePlayers[i].isActive && (gRacePlayers[i].stateFlags & RACE_PLAYER_READY_FLAG)) ahead++;
        }
        printf("sbk-trial: result course=%d char=%d board=%d action=%d item=%d boost=%d rank=%d finished_before=%d frames=%lu money=%d\n",
               gRaceCourseIndex.signedValue, p->characterId, p->characterVariant, p->actionTriggerChance, p->itemTriggerChance, trial.boost,
               p->rankIndex + 1, ahead, trial_frames, p->money - trial_money0);
        fflush(stdout);
        if (trial.quit) {
            extern void sbk_request_quit_now(void);
            sbk_request_quit_now();
        }
    }
}

/* --status: the campaign's scoreboard. Save slot 0 holds the purse, the
 * progression level and the per-course unlock states (-1 = still for sale in
 * the course shop); gRacePlayers[0].money is the live purse during a run. */
int sbk_status;

static void status_tick(unsigned long retraces) {
    static int last_money = -1, last_prog = -1;
    const GameSaveData *sd = &gGameSaveDataBuffer[0];
    int money = gRacePlayers[0].money;
    int i;
    if (!sbk_status) return;
    if (retraces % 300 != 0 && money == last_money && sd->progressionLevel == last_prog) return;
    if (retraces % 300 != 0 && money == last_money) return;
    last_money = money;
    last_prog = sd->progressionLevel;
    printf("sbk-status: r=%lu money=%d savemoney=%d prog=%d highest=%d unlocks=", retraces, money,
           (int)sd->money, sd->progressionLevel, gHighestUnlockedCourse);
    for (i = 0; i < 12; i++) printf("%s%d", i ? "," : "", sd->courseUnlockStates[i]);
    printf(" won=");
    for (i = 0; i < 11; i++) printf("%s%d", i ? "," : "", sd->cupPlacements[i]);
    printf("\n");
    fflush(stdout);
}

void sbk_autoplay_tick(unsigned long retraces) {
    static unsigned soak_step;
    extern void sbk_menu_nav_tick(unsigned long);
    sbk_menu_nav_tick(retraces);
    status_tick(retraces);
    course_trace(retraces);
    trial_tick(retraces);
    dumpon_tick();
    if (gRacePlayers[0].isActive) {
        if (sbk_nightmare) {
            int i;
            for (i = 0; i < RACE_PLAYER_COUNT; i++) {
                if (gRacePlayers[i].isActive && gRacePlayers[i].isCpu) {
                    gRacePlayers[i].actionTriggerChance = 255;
                    gRacePlayers[i].itemTriggerChance = 255;
                }
            }
        }
        if (sbk_autoplay && gRacePlayers[0].isCpu == 0) {
            gRacePlayers[0].isCpu = 1;
            printf("sbk: autoplay: player 1 handed to the CPU rider\n");
            trial_arm(&gRacePlayers[0], retraces);
        }
        return;
    }
    /* Menus: a cycle of YES-and-confirm, confirm, START, confirm every 1.5 s
     * gets through the prompts, the title and the results screens. It is a
     * monkey, not a navigator: it may loop in a shop, but it keeps the game
     * moving without anyone at the keyboard. */
    if (sbk_soak && retraces % 90 == 0) {
        switch (soak_step++ & 3) {
            case 0: sbk_input_play_add("stick 0 80 3"); sbk_input_play_add("wait 6"); sbk_input_play_add("press A 3"); break;
            case 1: sbk_input_play_add("press A 3"); break;
            case 2: sbk_input_play_add("press START 3"); break;
            default: sbk_input_play_add("press A 3"); break;
        }
    }
}

/* --peek ADDR:LEN (hex, repeatable): dump RDRAM bytes once a second */
static struct { unsigned addr, len; } peeks[8];
static int npeeks;

int sbk_peek_add(const char *spec) {
    unsigned a, n;
    if (npeeks >= 8 || sscanf(spec, "%x:%x", &a, &n) != 2) return -1;
    peeks[npeeks].addr = a; peeks[npeeks].len = n > 256 ? 256 : n; npeeks++;
    sbk_race_debug_enabled = 1;
    return 0;
}

static void dump_peeks(unsigned long retraces) {
    int i; unsigned k;
    for (i = 0; i < npeeks; i++) {
        const unsigned char *p = (const unsigned char *)(uintptr_t)peeks[i].addr;
        printf("sbk-peek: r=%lu %08x:", retraces, peeks[i].addr);
        for (k = 0; k < peeks[i].len; k++) printf("%s%02x", (k % 4) ? "" : " ", p[k]);
        printf("\n");
    }
}

void sbk_race_debug(unsigned long retraces) {
    int i;
    dump_peeks(retraces);
    if (npeeks) return;
    for (i = 0; i < RACE_PLAYER_COUNT; i++) {
        RacePlayer *p = &gRacePlayers[i];
        if (i != 0 && !p->isActive) continue;
        printf("sbk-race: r=%lu p%d cpu=%d act=%d mode=%d st=%08x in=%08x cur=%08x dis=%08x stick=%d,%d rep=%d u15=%d pos=%d,%d,%d vel=%d,%d,%d surf=%d timer=%d\n",
               retraces, i, p->isCpu, p->isActive, p->mode, (unsigned)p->stateFlags, (unsigned)p->inputFlags,
               (unsigned)p->currentInputFlags, (unsigned)p->disabledInputFlags, p->stickX, p->stickY,
               p->replayInputSource, p->unk15, p->pos.x, p->pos.y, p->pos.z, p->velocity.x, p->velocity.y,
               p->velocity.z, p->unk330, p->stateTimer);
    }
}
