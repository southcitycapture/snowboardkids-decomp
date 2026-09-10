/* Real functions for the `#pragma weak ALIAS = TARGET` pairs upstream uses to
 * give one IDO-matched function a second name with a looser prototype (the
 * pragma is IDO-only; the port build neutralises it). Compiled with the game's
 * flags so the game headers apply. */
#include "common.h"
#include "game/audio/audio_engine.h"
#include "game/engine/callback_task_scheduler.h"
#include "game/menu/renderer/menu_renderer.h"
#include "game/menu/renderer/menu_render_utils.h"
#include "game/menu/main_menu/controller_main_menu_flow.h"

void schedulerThreadEntry(void *arg) {
    schedulerThreadMain((SchedulerState *)arg);
}

s32 __MusIntRandomWithContext(s32 range, u8 *sequencePos, PlayerCommandState *state) {
    (void)sequencePos;
    (void)state;
    return __MusIntRandom(range);
}

void *createCallbackTaskPreservingArgsS32(CallbackTaskCallback callback, s32 type, s32 priority) {
    return createCallbackTaskPreservingArgs(callback, type, priority);
}

void *createCallbackTaskS32(CallbackTaskCallback callback, s32 type, s32 priority) {
    return createCallbackTask(callback, type, priority);
}

void drawAssetTableSpriteWideIndex(s16 x, s16 y, AssetTable *table, s32 entryIndex) {
    drawAssetTableSprite(x, y, table, (u16)entryIndex);
}

void drawAssetTableSpriteWithExplicitPaletteWideIndex(s16 x, s16 y, AssetTable *table, s32 entryIndex, u16 paletteIndex) {
    drawAssetTableSpriteWithExplicitPalette(x, y, table, (u16)entryIndex, paletteIndex);
}

void drawMenuAsciiChar(s16 x, s16 y, u8 character, u16 palette) {
    drawMenuAsciiCharImpl(x, y, character, palette);
}

void drawMenuAsciiCharLegacy(s16 x, s16 y, volatile s32 character, u16 palette) {
    drawMenuAsciiCharImpl(x, y, (u8)character, palette);
}

void drawMenuSpriteWithAlphaWideArgs(s32 x, s32 y, AssetTable *table, s32 tileIndex, s32 width, s32 height, s32 palette, s32 alpha, u32 flip) {
    drawMenuSpriteWithAlpha((s16)x, (s16)y, table, (u16)tileIndex, (u16)width, (u16)height, (u8)palette, (u16)alpha, (u8)flip);
}

void requestRumbleMotorInitWithContext(u16 controllerIndex, s32 playerCount, s32 choiceValue) {
    (void)playerCount;
    (void)choiceValue;
    requestRumbleMotorInit(controllerIndex);
}

void requestControllerPakSaveStatusWithContext(u16 controllerIndex, s32 playerCount, s32 choiceValue) {
    (void)playerCount;
    (void)choiceValue;
    requestControllerPakSaveStatus(controllerIndex);
}

void requestControllerPakSaveReadWithContext(u16 controllerIndex, s32 playerCount, s32 choiceValue) {
    (void)playerCount;
    (void)choiceValue;
    requestControllerPakSaveRead(controllerIndex);
}

void requestControllerPakRepairWithContext(u16 controllerIndex, s32 playerCount, s32 choiceValue) {
    (void)playerCount;
    (void)choiceValue;
    requestControllerPakRepair(controllerIndex);
}
