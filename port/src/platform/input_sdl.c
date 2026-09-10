/* Keyboard (and, when present, the first SDL joystick) as an N64 controller.
 *
 *   arrows / WASD  stick        Z / X      A / B         C  Z trigger
 *   Return         Start        Q / E      L / R         IJKL  C buttons
 *   Escape         quit
 */
#include <string.h>
#include <SDL2/SDL.h>
#include "../ultra/ultra.h"
#include "input.h"

static SDL_Joystick *sbk_joy;
static int sbk_quit;
static uint16_t sbk_buttons;
static int8_t sbk_stick_x, sbk_stick_y;

void sbk_input_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0 && SDL_NumJoysticks() > 0) {
        sbk_joy = SDL_JoystickOpen(0);
    }
}

int sbk_input_controller_count(void) {
    return 1;
}

int sbk_input_quit_requested(void) {
    return sbk_quit;
}

void sbk_input_request_quit(void) {
    sbk_quit = 1;
}

static int8_t sbk_axis(const Uint8 *k, SDL_Scancode neg, SDL_Scancode pos, SDL_Scancode neg2, SDL_Scancode pos2) {
    int v = 0;
    if (k[neg] || k[neg2]) v -= 80;
    if (k[pos] || k[pos2]) v += 80;
    return (int8_t)v;
}

void sbk_input_update(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;

    if (k[SDL_SCANCODE_Z]) b |= CONT_A;
    if (k[SDL_SCANCODE_X]) b |= CONT_B;
    if (k[SDL_SCANCODE_C] || k[SDL_SCANCODE_LSHIFT]) b |= CONT_G;
    if (k[SDL_SCANCODE_RETURN]) b |= CONT_START;
    if (k[SDL_SCANCODE_Q]) b |= CONT_L;
    if (k[SDL_SCANCODE_E]) b |= CONT_R;
    if (k[SDL_SCANCODE_I]) b |= CONT_E;
    if (k[SDL_SCANCODE_K]) b |= CONT_D;
    if (k[SDL_SCANCODE_J]) b |= CONT_C;
    if (k[SDL_SCANCODE_L]) b |= CONT_F;
    if (k[SDL_SCANCODE_T]) b |= CONT_UP;
    if (k[SDL_SCANCODE_G]) b |= CONT_DOWN;
    if (k[SDL_SCANCODE_F]) b |= CONT_LEFT;
    if (k[SDL_SCANCODE_H]) b |= CONT_RIGHT;
    if (k[SDL_SCANCODE_ESCAPE]) sbk_quit = 1;

    sbk_stick_x = sbk_axis(k, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_A, SDL_SCANCODE_D);
    sbk_stick_y = sbk_axis(k, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP, SDL_SCANCODE_S, SDL_SCANCODE_W);

    if (sbk_joy != NULL) {
        int ax = SDL_JoystickGetAxis(sbk_joy, 0) * 80 / 32767;
        int ay = -SDL_JoystickGetAxis(sbk_joy, 1) * 80 / 32767;
        if (ax > 12 || ax < -12) sbk_stick_x = (int8_t)ax;
        if (ay > 12 || ay < -12) sbk_stick_y = (int8_t)ay;
        if (SDL_JoystickGetButton(sbk_joy, 0)) b |= CONT_A;
        if (SDL_JoystickGetButton(sbk_joy, 1)) b |= CONT_B;
        if (SDL_JoystickGetButton(sbk_joy, 2)) b |= CONT_G;
        if (SDL_JoystickGetButton(sbk_joy, 7)) b |= CONT_START;
        if (SDL_JoystickGetButton(sbk_joy, 4)) b |= CONT_L;
        if (SDL_JoystickGetButton(sbk_joy, 5)) b |= CONT_R;
    }
    sbk_buttons = b;
}

void sbk_input_read_pad(int port, uint16_t *buttons, int8_t *stick_x, int8_t *stick_y) {
    if (port != 0) {
        *buttons = 0;
        *stick_x = 0;
        *stick_y = 0;
        return;
    }
    *buttons = sbk_buttons;
    *stick_x = sbk_stick_x;
    *stick_y = sbk_stick_y;
}
