/* --racedbg: print the race players' state once a second (port-side view of
 * the game's globals, which are pinned at their N64 addresses). */
#include "../ultra/ultra.h"
#include <stdio.h>
#include <stdint.h>
#include "game/race/player/race_player_input.h"
#include "../platform/input.h"

int sbk_race_debug_enabled;
int sbk_autoplay; /* --autoplay: player 1 is driven by the game's own CPU rider logic */
int sbk_soak;     /* --soak: outside a race, keep confirming through the menus (implies --autoplay) */
int sbk_nightmare; /* --nightmare: every CPU rider uses items and tricks at every chance (the game's per-course table gives 100/255) */

/* Called every retrace. The race code checks isCpu each update, so flipping it
 * while the player is active hands the rider to the AI that knows the course. */
void sbk_autoplay_tick(unsigned long retraces) {
    static unsigned soak_step;
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
