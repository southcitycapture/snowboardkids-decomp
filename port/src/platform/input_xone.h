#ifndef SBK_INPUT_XONE_H
#define SBK_INPUT_XONE_H
#include <stdint.h>

struct sbk_xone_state {
    uint8_t buttons;   /* 0x04 menu 0x08 view 0x10 A 0x20 B 0x40 X 0x80 Y */
    uint8_t dpad;      /* 0x01 up 0x02 down 0x04 left 0x08 right 0x10 LB 0x20 RB 0x40 LS 0x80 RS */
    uint8_t guide;
    uint16_t lt, rt;   /* 0..1023 */
    int16_t lx, ly, rx, ry; /* Y positive is up */
};

int sbk_xone_open(void);            /* 1 if an Xbox One pad was opened */
int sbk_xone_present(void);
void sbk_xone_get(struct sbk_xone_state *out);
void sbk_xone_close(void);
void sbk_xone_rumble(int strong, int weak); /* 0..100 each; 0,0 stops */

#endif
