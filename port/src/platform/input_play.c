/* Scripted controller input: lets the game play itself.
 *
 * Two formats, both advanced once per controller read (the game polls at
 * 30 Hz, every other retrace):
 *
 *  1. Text script (any extension but .m64), one command per line:
 *        wait N                 do nothing for N reads
 *        press BTN[+BTN..] [N]  hold the buttons for N reads (default 1), then release
 *        hold BTN[+BTN..]       keep holding from now on
 *        release BTN[+BTN..]|all
 *        stick X Y              analog stick, -80..80, until changed
 *        stick X Y N            stick for N reads, then centre
 *        # comment
 *     Buttons: A B Z START L R CU CD CL CR DU DD DL DR (the D-pad).
 *
 *  2. Mupen64 .m64 TAS movies: the 1024-byte header is skipped, then each
 *     4-byte sample is exactly the N64 controller word (buttons big-endian,
 *     stick x, stick y). Only controller 1 is fed.
 *
 * --record FILE.m64 writes what the game actually read (keyboard, joystick or
 * script) in the same .m64 layout, so a session at the keyboard becomes a
 * replayable "ghost". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include "../ultra/ultra.h"
#include "input.h"

struct cmd {
    enum { C_WAIT, C_PRESS, C_HOLD, C_RELEASE, C_STICK, C_DUMP } op;
    uint16_t buttons;
    int8_t x, y;
    int frames;
};

static struct cmd *script;
static int script_len, script_pos, script_left;
static uint16_t held, pressed;
static unsigned reads;
static int8_t stick_x, stick_y;
static int stick_left = -1;
static int script_done;

static uint8_t *movie;
static long movie_len, movie_pos;

static FILE *rec;
static uint32_t rec_samples;

static const struct { const char *name; uint16_t bit; } names[] = {
    { "A", CONT_A }, { "B", CONT_B }, { "Z", CONT_G }, { "START", CONT_START },
    { "L", CONT_L }, { "R", CONT_R },
    { "CU", CONT_E }, { "CD", CONT_D }, { "CL", CONT_C }, { "CR", CONT_F },
    { "DU", CONT_UP }, { "DD", CONT_DOWN }, { "DL", CONT_LEFT }, { "DR", CONT_RIGHT },
    { NULL, 0 }
};

static uint16_t parse_buttons(char *s) {
    uint16_t b = 0;
    char *tok;
    if (strcasecmp(s, "all") == 0) {
        return 0xFFFF;
    }
    for (tok = strtok(s, "+"); tok != NULL; tok = strtok(NULL, "+")) {
        int i, found = 0;
        for (i = 0; names[i].name != NULL; i++) {
            if (strcasecmp(tok, names[i].name) == 0) {
                b |= names[i].bit;
                found = 1;
            }
        }
        if (!found) {
            fprintf(stderr, "sbk: input script: unknown button '%s'\n", tok);
        }
    }
    return b;
}

static int cap;

/* Parse one script line and append it; returns 0 if the line held no command. */
static int add_line(char *line) {
    char op[32], a1[128], a2[32], a3[32];
    int n;
    struct cmd c;
    char *hash = strchr(line, '#');
    if (hash != NULL) *hash = '\0';
    n = sscanf(line, "%31s %127s %31s %31s", op, a1, a2, a3);
    if (n < 1) return 0;
    memset(&c, 0, sizeof(c));
    if (strcasecmp(op, "wait") == 0 && n >= 2) {
        c.op = C_WAIT; c.frames = atoi(a1);
    } else if (strcasecmp(op, "press") == 0 && n >= 2) {
        c.op = C_PRESS; c.buttons = parse_buttons(a1); c.frames = n >= 3 ? atoi(a2) : 1;
    } else if (strcasecmp(op, "hold") == 0 && n >= 2) {
        c.op = C_HOLD; c.buttons = parse_buttons(a1);
    } else if (strcasecmp(op, "release") == 0 && n >= 2) {
        c.op = C_RELEASE; c.buttons = parse_buttons(a1);
    } else if (strcasecmp(op, "stick") == 0 && n >= 3) {
        c.op = C_STICK; c.x = (int8_t)atoi(a1); c.y = (int8_t)atoi(a2); c.frames = n >= 4 ? atoi(a3) : -1;
    } else if (strcasecmp(op, "dump") == 0) {
        c.op = C_DUMP; c.frames = n >= 2 ? atoi(a1) : 4; /* dump N presented frames (+ the next task's list) */
    } else {
        fprintf(stderr, "sbk: input script: bad line: %s", line);
        return 0;
    }
    if (script_len == cap) {
        cap = cap ? cap * 2 : 64;
        script = realloc(script, sizeof(*script) * cap);
    }
    script[script_len++] = c;
    return 1;
}

static int load_script(const char *path) {
    FILE *f = fopen(path, "r");
    char line[256];
    if (f == NULL) {
        fprintf(stderr, "sbk: cannot open input script %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        add_line(line);
    }
    fclose(f);
    printf("sbk: input script %s: %d commands\n", path, script_len);
    return 0;
}

/* Programmatic script lines (soak mode etc.). */
void sbk_input_play_add(const char *line) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s\n", line);
    if (add_line(buf)) script_done = 0;
}

/* --cmds FILE: whenever FILE appears, its lines are appended to the script
 * and the file removed, so a session can be driven step by step from the
 * host (write to a temp name, then rename into place). */
static const char *cmdfile;

void sbk_input_play_set_cmdfile(const char *path) {
    cmdfile = path;
    printf("sbk: live commands from %s\n", path);
}

void sbk_input_play_poll(void) {
    FILE *f;
    char line[256];
    int added = 0;
    if (cmdfile == NULL || (f = fopen(cmdfile, "r")) == NULL) return;
    while (fgets(line, sizeof(line), f) != NULL) {
        added += add_line(line);
    }
    fclose(f);
    remove(cmdfile);
    if (added) {
        script_done = 0;
        printf("sbk-play: read %u: +%d live commands\n", reads, added);
    }
}

static int load_movie(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL) {
        fprintf(stderr, "sbk: cannot open movie %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 1024 + 4) {
        fclose(f);
        fprintf(stderr, "sbk: %s is not an .m64 movie\n", path);
        return -1;
    }
    movie = malloc((size_t)n);
    if (fread(movie, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    movie_len = n;
    movie_pos = 1024;
    printf("sbk: movie %s: %ld samples\n", path, (n - 1024) / 4);
    return 0;
}

int sbk_input_play_load(const char *path) {
    size_t len = strlen(path);
    if (len > 4 && strcasecmp(path + len - 4, ".m64") == 0) {
        return load_movie(path);
    }
    return load_script(path);
}

int sbk_input_record_start(const char *path) {
    static uint8_t header[1024];
    rec = fopen(path, "wb");
    if (rec == NULL) {
        fprintf(stderr, "sbk: cannot create movie %s\n", path);
        return -1;
    }
    memcpy(header, "M64\x1A", 4);
    header[4] = 3;                              /* version */
    header[0x1C] = 60; header[0x1D] = 0;        /* VI/s (LE u16 at 0x18? keep simple) */
    header[0x15] = 1;                           /* controllers */
    header[0x20] = 1;                           /* controller 1 present */
    strncpy((char *)header + 0xC4, "SNOWBOARD KIDS", 32);
    fwrite(header, 1, sizeof(header), rec);
    printf("sbk: recording inputs to %s\n", path);
    return 0;
}

/* Called once per controller read with the pad the game is about to see;
 * scripted input is merged in, the result recorded. */
void sbk_input_play_step(uint16_t *buttons, int8_t *x, int8_t *y) {
    reads++;
    if (movie != NULL) {
        if (movie_pos + 4 <= movie_len) {
            *buttons |= (uint16_t)((movie[movie_pos] << 8) | movie[movie_pos + 1]);
            if (movie[movie_pos + 2] != 0 || movie[movie_pos + 3] != 0) {
                *x = (int8_t)movie[movie_pos + 2];
                *y = (int8_t)movie[movie_pos + 3];
            }
            movie_pos += 4;
        } else if (!script_done) {
            script_done = 1;
            printf("sbk: movie finished\n");
        }
    } else if (script != NULL && !script_done) {
        /* advance the script */
        while (script_left == 0 && script_pos < script_len) {
            struct cmd *c = &script[script_pos++];
            switch (c->op) {
                case C_WAIT: script_left = c->frames; break;
                case C_PRESS: pressed = c->buttons; script_left = c->frames; printf("sbk-play: read %u: press %04x for %d\n", reads, pressed, c->frames); break;
                case C_HOLD: held |= c->buttons; break;
                case C_RELEASE: held &= (uint16_t)~c->buttons; break;
                case C_STICK: stick_x = c->x; stick_y = c->y; stick_left = c->frames; break;
                case C_DUMP: {
                    extern int sbk_frame_dump_left, sbk_dump_task, sbk_dump_frames;
                    extern unsigned sbk_task_count;
                    sbk_dump_frames = c->frames;
                    sbk_dump_task = (int)sbk_task_count + 1; /* the dumper arms on this task */
                    printf("sbk-play: read %u: dump %d frames from task %d\n", reads, c->frames, sbk_dump_task);
                    break;
                }
            }
        }
        if (script_left > 0) {
            script_left--;
            *buttons |= pressed;
            if (script_left == 0) {
                pressed = 0;
            }
        } else if (script_pos >= script_len) {
            script_done = 1;
            printf("sbk: input script finished\n");
        }
        *buttons |= held;
        if (stick_left != 0) {
            *x = stick_x;
            *y = stick_y;
            if (stick_left > 0 && --stick_left == 0) {
                stick_x = stick_y = 0;
            }
        }
    }
    if (rec != NULL) {
        uint8_t s[4];
        s[0] = (uint8_t)(*buttons >> 8);
        s[1] = (uint8_t)*buttons;
        s[2] = (uint8_t)*x;
        s[3] = (uint8_t)*y;
        fwrite(s, 1, 4, rec);
        if ((++rec_samples & 63) == 0) {
            fflush(rec);
        }
    }
}

void sbk_input_play_shutdown(void) {
    if (rec != NULL) {
        uint32_t le = rec_samples;
        uint8_t b[4] = { (uint8_t)le, (uint8_t)(le >> 8), (uint8_t)(le >> 16), (uint8_t)(le >> 24) };
        fseek(rec, 0x18, SEEK_SET); /* input sample count */
        fwrite(b, 1, 4, rec);
        fseek(rec, 0x0C, SEEK_SET); /* frame (VI) count: same, good enough */
        fwrite(b, 1, 4, rec);
        fclose(rec);
        rec = NULL;
        printf("sbk: recorded %u input samples\n", (unsigned)rec_samples);
    }
}
