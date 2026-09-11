/* Keyboard and gamepad as N64 controller 1.
 *
 * Keyboard:
 *   arrows / WASD  stick        Z / X      A / B         C  Z trigger
 *   Return         Start        Q / E      L / R         IJKL  C buttons
 *   TFGH           D-pad        Escape     quit
 *
 * Gamepad: any pad SDL knows as a game controller (its built-in mapping
 * table, or SDL_GAMECONTROLLERCONFIG) maps
 *   left stick -> stick     A -> A    B / X -> B    right stick + Y -> C
 *   LT -> Z    LB / RB -> L / R    Start -> Start    D-pad -> D-pad
 * An unrecognised joystick gets the raw fallback (axes 0/1, buttons 0-7),
 * and every raw button/axis event is logged as `sbk-pad:` for a while
 * after the pad appears so a mapping line can be written for it. Pads can
 * be plugged in after launch. */
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "../ultra/ultra.h"
#include "input.h"
#include "input_xone.h"

static SDL_GameController *sbk_pad;
static SDL_Joystick *sbk_joy;      /* raw fallback, or the controller's joystick for logging */
static SDL_Haptic *sbk_haptic;     /* rumble on an SDL pad, if it has it */
static int sbk_quit;
static uint16_t sbk_buttons;
static int8_t sbk_stick_x, sbk_stick_y;
static unsigned sbk_pad_log_left;  /* raw events still to log */

static void open_pad(int index) {
    if (sbk_pad != NULL || sbk_joy != NULL) return;
    if (SDL_IsGameController(index)) {
        sbk_pad = SDL_GameControllerOpen(index);
        if (sbk_pad != NULL) {
            sbk_joy = SDL_GameControllerGetJoystick(sbk_pad);
            printf("sbk: gamepad: %s (mapped: %s)\n", SDL_GameControllerName(sbk_pad), SDL_GameControllerMapping(sbk_pad));
        }
    }
    if (sbk_pad == NULL) {
        sbk_joy = SDL_JoystickOpen(index);
        if (sbk_joy != NULL) {
            char guid[64];
            SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(sbk_joy), guid, sizeof(guid));
            printf("sbk: joystick (no controller mapping, raw fallback): %s  axes=%d buttons=%d hats=%d guid=%s\n",
                   SDL_JoystickName(sbk_joy), SDL_JoystickNumAxes(sbk_joy), SDL_JoystickNumButtons(sbk_joy),
                   SDL_JoystickNumHats(sbk_joy), guid);
            printf("sbk: to map it, set SDL_GAMECONTROLLERCONFIG=\"%s,name,a:b0,b:b1,x:b2,y:b3,start:b7,leftshoulder:b4,rightshoulder:b5,lefttrigger:a2,leftx:a0,lefty:a1,rightx:a3,righty:a4,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2\" using the numbers from the sbk-pad lines\n", guid);
        }
    }
    sbk_pad_log_left = 200;
    if (sbk_joy != NULL && SDL_JoystickIsHaptic(sbk_joy)) {
        sbk_haptic = SDL_HapticOpenFromJoystick(sbk_joy);
        if (sbk_haptic != NULL && SDL_HapticRumbleInit(sbk_haptic) != 0) {
            SDL_HapticClose(sbk_haptic);
            sbk_haptic = NULL;
        }
        if (sbk_haptic != NULL) printf("sbk: gamepad rumble available\n");
    }
}

/* Rumble Pak: a pad that can rumble counts as a plugged-in pak. */
int sbk_input_rumble_supported(void) {
    return sbk_haptic != NULL || sbk_xone_present();
}

void sbk_input_rumble(int on) {
    static int last = -1;
    if (on == last) return;
    last = on;
    if (sbk_xone_present()) {
        sbk_xone_rumble(on ? 70 : 0, on ? 70 : 0);
    } else if (sbk_haptic != NULL) {
        if (on) SDL_HapticRumblePlay(sbk_haptic, 0.7f, 5000);
        else SDL_HapticRumbleStop(sbk_haptic);
    }
}

/* --nopad: no gamepad at all. Not a convenience -- a determinism switch. A
 * pad that is open reports a Rumble Pak through osMotorInit and feeds its own
 * stick and buttons into every frame, and whether the Xbox One pad can be
 * claimed at startup depends on whether the previous process has finished
 * letting go of it. That is what made headless trials drift run to run: two
 * outcomes, one with the pad and one without. */
int sbk_nopad;

void sbk_input_init(void) {
    int i;
    if (sbk_nopad) {
        printf("sbk: no gamepad (--nopad)\n");
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0) {
        printf("sbk: no joystick subsystem: %s\n", SDL_GetError());
        return;
    }
    SDL_JoystickEventState(SDL_ENABLE);
    SDL_GameControllerEventState(SDL_ENABLE);
    for (i = 0; i < SDL_NumJoysticks() && sbk_joy == NULL; i++) {
        open_pad(i);
    }
    if (sbk_joy == NULL) {
        if (sbk_xone_open()) {
            /* an Xbox One pad on the vendor interface (input_xone.c) */
        } else {
            printf("sbk: no gamepad; keyboard only\n");
        }
    }
}

/* Called from the SDL event loop (gfx_sdl_gl13.c) for joystick events. */
void sbk_input_joy_event(const SDL_Event *ev) {
    switch (ev->type) {
        case SDL_JOYDEVICEADDED:
            open_pad(ev->jdevice.which);
            break;
        case SDL_JOYDEVICEREMOVED:
            if (sbk_joy != NULL && SDL_JoystickInstanceID(sbk_joy) == ev->jdevice.which) {
                printf("sbk: gamepad unplugged\n");
                if (sbk_pad != NULL) SDL_GameControllerClose(sbk_pad); else SDL_JoystickClose(sbk_joy);
                sbk_pad = NULL;
                sbk_joy = NULL;
            }
            break;
        case SDL_JOYBUTTONDOWN:
            if (sbk_pad_log_left > 0) { sbk_pad_log_left--; printf("sbk-pad: button %d down\n", ev->jbutton.button); }
            break;
        case SDL_JOYAXISMOTION:
            if (sbk_pad_log_left > 0 && (ev->jaxis.value > 16000 || ev->jaxis.value < -16000)) {
                sbk_pad_log_left--; printf("sbk-pad: axis %d = %d\n", ev->jaxis.axis, ev->jaxis.value);
            }
            break;
        case SDL_JOYHATMOTION:
            if (sbk_pad_log_left > 0 && ev->jhat.value != 0) { sbk_pad_log_left--; printf("sbk-pad: hat %d = %d\n", ev->jhat.hat, ev->jhat.value); }
            break;
        default:
            break;
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

/* SDL axis (-32768..32767) to the N64's -80..80 with a dead zone and a
 * circular clamp, so a full diagonal stays inside the stick's range. */
static void map_stick(int ax, int ay, int8_t *x, int8_t *y) {
    const int dead = 5000;
    int mx = ax, my = -ay;
    long long len2;
    if (mx > -dead && mx < dead) mx = 0;
    if (my > -dead && my < dead) my = 0;
    if (mx == 0 && my == 0) return; /* keep the keyboard's value */
    mx = mx * 85 / 32767;
    my = my * 85 / 32767;
    len2 = (long long)mx * mx + (long long)my * my;
    if (len2 > 80 * 80) {
        double s = 80.0 / __builtin_sqrt((double)len2);
        mx = (int)(mx * s);
        my = (int)(my * s);
    }
    *x = (int8_t)mx;
    *y = (int8_t)my;
}

/* --- the UI's own input tap ---------------------------------------------
 * The launcher runs before the game exists and the overlay runs while it is
 * paused, so neither can go through the N64 controller path. Both read the
 * keyboard and the pad here directly. */
int sbk_ui_owns_input;

void sbk_ui_input_raw(struct SbkUiRaw *r) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    memset(r, 0, sizeof(*r));
    r->up = k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W];
    r->down = k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S];
    r->left = k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A];
    r->right = k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D];
    r->accept = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER] || k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_Z];
    r->cancel = k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_X] || k[SDL_SCANCODE_BACKSPACE];
    r->menu = k[SDL_SCANCODE_F1];

    if (sbk_pad != NULL) {
        int lx = SDL_GameControllerGetAxis(sbk_pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(sbk_pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || ly < -12000) r->up = 1;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ly > 12000) r->down = 1;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || lx < -12000) r->left = 1;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx > 12000) r->right = 1;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_A)) r->accept = 1;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_B)) r->cancel = 1;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_BACK)) r->menu = 1;
    } else if (sbk_xone_present()) {
        struct sbk_xone_state xs;
        sbk_xone_get(&xs);
        if ((xs.dpad & 0x01) || xs.ly > 12000) r->up = 1;
        if ((xs.dpad & 0x02) || xs.ly < -12000) r->down = 1;
        if ((xs.dpad & 0x04) || xs.lx < -12000) r->left = 1;
        if ((xs.dpad & 0x08) || xs.lx > 12000) r->right = 1;
        if (xs.buttons & 0x10) r->accept = 1;
        if (xs.buttons & 0x20) r->cancel = 1;
        if (xs.buttons & 0x08) r->menu = 1;   /* View */
    } else if (sbk_joy != NULL) {
        int lx = SDL_JoystickGetAxis(sbk_joy, 0), ly = SDL_JoystickGetAxis(sbk_joy, 1);
        if (ly < -12000) r->up = 1;
        if (ly > 12000) r->down = 1;
        if (lx < -12000) r->left = 1;
        if (lx > 12000) r->right = 1;
        if (SDL_JoystickGetButton(sbk_joy, 0)) r->accept = 1;
        if (SDL_JoystickGetButton(sbk_joy, 1)) r->cancel = 1;
        if (SDL_JoystickGetButton(sbk_joy, 6)) r->menu = 1;
        if (SDL_JoystickNumHats(sbk_joy) > 0) {
            Uint8 h = SDL_JoystickGetHat(sbk_joy, 0);
            if (h & SDL_HAT_UP) r->up = 1;
            if (h & SDL_HAT_DOWN) r->down = 1;
            if (h & SDL_HAT_LEFT) r->left = 1;
            if (h & SDL_HAT_RIGHT) r->right = 1;
        }
    }
}

void sbk_input_update(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint16_t b = 0;

    if (sbk_ui_owns_input) {
        /* the overlay is open: the game sees a controller nobody is holding */
        sbk_buttons = 0;
        sbk_stick_x = 0;
        sbk_stick_y = 0;
        return;
    }

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

    if (sbk_pad != NULL) {
        int rx, ry;
        map_stick(SDL_GameControllerGetAxis(sbk_pad, SDL_CONTROLLER_AXIS_LEFTX),
                  SDL_GameControllerGetAxis(sbk_pad, SDL_CONTROLLER_AXIS_LEFTY), &sbk_stick_x, &sbk_stick_y);
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_A)) b |= CONT_A;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_B)) b |= CONT_B;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_X)) b |= CONT_B;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_Y)) b |= CONT_D; /* C-down: the item button */
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_START)) b |= CONT_START;
        /* BACK/View is the overlay button (sbk_ui_input_raw), not Start. */
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) b |= CONT_L;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) b |= CONT_R;
        if (SDL_GameControllerGetAxis(sbk_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) b |= CONT_G;
        if (SDL_GameControllerGetAxis(sbk_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) b |= CONT_G;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) b |= CONT_UP;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) b |= CONT_DOWN;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) b |= CONT_LEFT;
        if (SDL_GameControllerGetButton(sbk_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) b |= CONT_RIGHT;
        rx = SDL_GameControllerGetAxis(sbk_pad, SDL_CONTROLLER_AXIS_RIGHTX);
        ry = SDL_GameControllerGetAxis(sbk_pad, SDL_CONTROLLER_AXIS_RIGHTY);
        if (ry < -12000) b |= CONT_E;
        if (ry > 12000) b |= CONT_D;
        if (rx < -12000) b |= CONT_C;
        if (rx > 12000) b |= CONT_F;
    } else if (sbk_xone_present()) {
        struct sbk_xone_state xs;
        sbk_xone_get(&xs);
        map_stick(xs.lx, -xs.ly, &sbk_stick_x, &sbk_stick_y); /* map_stick expects SDL's Y-down */
        if (xs.buttons & 0x10) b |= CONT_A;
        if (xs.buttons & 0x20) b |= CONT_B;
        if (xs.buttons & 0x40) b |= CONT_B;
        if (xs.buttons & 0x80) b |= CONT_D;     /* Y: C-down, the item button */
        if (xs.buttons & 0x04) b |= CONT_START; /* menu */
        /* 0x08 View is the overlay button (sbk_ui_input_raw), not Start. */
        if (xs.dpad & 0x10) b |= CONT_L;
        if (xs.dpad & 0x20) b |= CONT_R;
        if (xs.lt > 256 || xs.rt > 256) b |= CONT_G;
        if (xs.dpad & 0x01) b |= CONT_UP;
        if (xs.dpad & 0x02) b |= CONT_DOWN;
        if (xs.dpad & 0x04) b |= CONT_LEFT;
        if (xs.dpad & 0x08) b |= CONT_RIGHT;
        if (xs.ry > 12000) b |= CONT_E;
        if (xs.ry < -12000) b |= CONT_D;
        if (xs.rx < -12000) b |= CONT_C;
        if (xs.rx > 12000) b |= CONT_F;
        if (xs.guide) sbk_quit = sbk_quit; /* reserved */
    } else if (sbk_joy != NULL) {
        map_stick(SDL_JoystickGetAxis(sbk_joy, 0), SDL_JoystickGetAxis(sbk_joy, 1), &sbk_stick_x, &sbk_stick_y);
        if (SDL_JoystickGetButton(sbk_joy, 0)) b |= CONT_A;
        if (SDL_JoystickGetButton(sbk_joy, 1)) b |= CONT_B;
        if (SDL_JoystickGetButton(sbk_joy, 2)) b |= CONT_G;
        if (SDL_JoystickGetButton(sbk_joy, 3)) b |= CONT_D;
        if (SDL_JoystickGetButton(sbk_joy, 4)) b |= CONT_L;
        if (SDL_JoystickGetButton(sbk_joy, 5)) b |= CONT_R;
        if (SDL_JoystickGetButton(sbk_joy, 6) || SDL_JoystickGetButton(sbk_joy, 7)) b |= CONT_START;
        if (SDL_JoystickNumHats(sbk_joy) > 0) {
            Uint8 h = SDL_JoystickGetHat(sbk_joy, 0);
            if (h & SDL_HAT_UP) b |= CONT_UP;
            if (h & SDL_HAT_DOWN) b |= CONT_DOWN;
            if (h & SDL_HAT_LEFT) b |= CONT_LEFT;
            if (h & SDL_HAT_RIGHT) b |= CONT_RIGHT;
        }
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
