/* --racedbg: print the race players' state once a second (port-side view of
 * the game's globals, which are pinned at their N64 addresses). */
#include "../ultra/ultra.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "game/race/player/race_player_input.h"
#include "../platform/input.h"
#define RACE_PLAYER_READY_FLAG 0x40 /* race_flow.c: the rider has finished */

int sbk_race_debug_enabled;
int sbk_autoplay; /* --autoplay: player 1 is driven by the game's own CPU rider logic */
int sbk_soak;     /* --soak: outside a race, keep confirming through the menus (implies --autoplay) */
int sbk_nightmare;

/* --trial char=N,board=N,action=N,item=N,boost=N,quit=1 (comma or space separated): a race experiment.
 * Outside a race the rider selection is pinned to char/board so the race
 * inits with them; during the race the chances are pinned and `boost` (in
 * 1/256ths) is added to the speed limit every retrace. When player 1
 * finishes, one result line is printed; with quit=1 the process exits. */
static struct { int on, chr, board, action, item, boost, quit; } trial = { 0, -1, -1, -1, -1, 0, 0 };
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
        }
        while (*p && *p != ' ' && *p != ',') p++;
        while (*p == ' ' || *p == ',') p++;
    }
    sbk_autoplay = 1;
    return 0;
}

static void trial_tick(unsigned long retraces) {
    RacePlayer *p = &gRacePlayers[0];
    if (!trial.on) return;
    if (!p->isActive) {
        if (trial.chr >= 0) { p->selectedCharacterId = (u8)trial.chr; p->characterId = (u8)trial.chr; }
        if (trial.board >= 0) p->characterVariant = (u8)trial.board;
        trial_start = 0;
        return;
    }
    if (trial_start == 0) {
        trial_start = retraces;
        trial_money0 = p->money;
        trial_done = 0;
        printf("sbk-trial: start r=%lu char=%d board=%d\n", retraces, p->characterId, p->characterVariant);
    }
    if (trial.action >= 0) p->actionTriggerChance = (u8)trial.action;
    if (trial.item >= 0) p->itemTriggerChance = (u8)trial.item;
    if (trial.boost != 0) p->speedLimit += (s32)(((long long)p->speedLimit * trial.boost) >> 8);
    if (!trial_done && (p->stateFlags & RACE_PLAYER_READY_FLAG)) {
        int i, ahead = 0;
        trial_done = 1;
        trial_frames = retraces - trial_start;
        for (i = 1; i < RACE_PLAYER_COUNT; i++) {
            if (gRacePlayers[i].isActive && (gRacePlayers[i].stateFlags & RACE_PLAYER_READY_FLAG)) ahead++;
        }
        printf("sbk-trial: result char=%d board=%d action=%d item=%d boost=%d rank=%d finished_before=%d frames=%lu money=%d\n",
               p->characterId, p->characterVariant, p->actionTriggerChance, p->itemTriggerChance, trial.boost,
               p->rankIndex + 1, ahead, trial_frames, p->money - trial_money0);
        fflush(stdout);
        if (trial.quit) {
            extern void sbk_request_quit_now(void);
            sbk_request_quit_now();
        }
    }
} /* --nightmare: every CPU rider uses items and tricks at every chance (the game's per-course table gives 100/255) */

/* Called every retrace. The race code checks isCpu each update, so flipping it
 * while the player is active hands the rider to the AI that knows the course. */
void sbk_autoplay_tick(unsigned long retraces) {
    static unsigned soak_step;
    trial_tick(retraces);
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
