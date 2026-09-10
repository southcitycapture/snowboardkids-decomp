/* Interpreter for the N64 audio microcode command lists (aspMain, "ABI 1")
 * that this game's libaudio synthesizer builds every frame.
 *
 * The RSP has 4 KB of DMEM that the commands address directly; each command
 * list decodes ADPCM voices into it, resamples them to the output pitch,
 * envelope-mixes them into dry/wet buses, runs the reverb delay lines, mixes
 * the buses, interleaves left/right and saves the result to DRAM (the
 * game's AI buffer). This runs the same steps on the host's DMEM image.
 *
 * Fixed-point conventions follow the microcode (and sm64-port's account of
 * it): gains are Q15 with round-to-nearest, ADPCM frames are 9 bytes for 16
 * samples with a 2x8-coefficient predictor book, and the resampler is a
 * 4-tap 64-phase windowed sinc stepping 16.16 by twice the 16-bit pitch.
 *
 * Big-endian host only for now: DRAM samples are used in place. */
#include "../ultra/ultra.h"
#include <stdio.h>
#include <string.h>
#include <PR/abi.h>
#include "../ultra/sbk_os.h"
#include "resample_table.h"

#define DMEM_SIZE 4096
#define DMEM_GUARD 64          /* the resampler writes its history just before its input */
#define ROUND_UP_8(v) (((v) + 7) & ~7u)
#define ROUND_UP_16(v) (((v) + 15) & ~15u)
#define ROUND_UP_32(v) (((v) + 31) & ~31u)

static uint8_t dmem_raw[DMEM_GUARD + DMEM_SIZE + 64];
#define DMEM_U8(a) (dmem_raw + DMEM_GUARD + (a))
#define DMEM_S16(a) ((int16_t *)(dmem_raw + DMEM_GUARD + (a)))

static struct {
    uint16_t in, out, nbytes;       /* A_SETBUFF */
    uint16_t dry_right, wet_left, wet_right; /* A_SETBUFF | A_AUX */
    int16_t vol[2];                 /* A_SETVOL | A_VOL */
    int16_t target[2];              /* A_SETVOL | A_RATE */
    int32_t rate[2];                /* 16.16 multiplier per 8 samples */
    int16_t vol_dry, vol_wet;       /* A_SETVOL | A_AUX */
    uint32_t segments[16];          /* A_SEGMENT */
    int16_t *loop_state;            /* A_SETLOOP */
    int16_t book[16][2][8];         /* A_LOADADPCM, up to 16 predictors */
} rsp;

unsigned sbk_audio_peak;            /* |sample| peak of the last saved buffer, for the stats line */

static inline int32_t clamp32(int64_t v) {
    if (v > 0x7FFFFFFF) return 0x7FFFFFFF;
    if (v < -0x7FFFFFFF - 1) return -0x7FFFFFFF - 1;
    return (int32_t)v;
}

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

int sbk_audio_disabled;
static uint32_t cur_w0, cur_w1;

static void *dram(uint32_t addr) {
    uint32_t seg = (addr >> 24) & 0xF;
    uint32_t phys = rsp.segments[seg] + (addr & 0x00FFFFFFu);
    static int warned;
    if (phys >= 0x00400000u && warned++ < 8) { /* every audio buffer/state lives in RDRAM */
        fprintf(stderr, "sbk audio: DRAM address %08x outside RDRAM (cmd %08x %08x)\n", addr, cur_w0, cur_w1);
    }
    return sbk_phys_to_host(phys);
}

/* ---- commands ------------------------------------------------------------ */

static void cmd_setbuff(uint8_t flags, uint16_t in, uint16_t out, uint16_t count) {
    if (flags & A_AUX) {
        rsp.dry_right = in;
        rsp.wet_left = out;
        rsp.wet_right = count;
    } else {
        rsp.in = in;
        rsp.out = out;
        rsp.nbytes = count;
    }
}

static void cmd_setvol(uint8_t flags, int16_t v, uint32_t w1) {
    if (flags & A_AUX) {
        rsp.vol_dry = v;
        rsp.vol_wet = (int16_t)(w1 & 0xFFFF);
    } else if (flags & A_VOL) {
        rsp.vol[(flags & A_LEFT) ? 0 : 1] = v;
    } else {
        int c = (flags & A_LEFT) ? 0 : 1;
        rsp.target[c] = v;
        rsp.rate[c] = (int32_t)w1;
    }
}

static void cmd_clearbuff(uint16_t addr, uint32_t count) {
    if (count == 0) return;
    memset(DMEM_U8(addr), 0, ROUND_UP_16(count));
}

static void cmd_loadbuff(uint32_t addr) {
    if (rsp.nbytes == 0) return;
    memcpy(DMEM_U8(rsp.in), dram(addr), ROUND_UP_8(rsp.nbytes));
}

static void cmd_savebuff(uint32_t addr) {
    const int16_t *s = DMEM_S16(rsp.out);
    unsigned n = ROUND_UP_8(rsp.nbytes) / 2, i, peak = 0;
    if (rsp.nbytes == 0) return;
    memcpy(dram(addr), s, ROUND_UP_8(rsp.nbytes));
    for (i = 0; i < n; i++) {
        unsigned a = (unsigned)(s[i] < 0 ? -s[i] : s[i]);
        if (a > peak) peak = a;
    }
    sbk_audio_peak = peak;
}

static void cmd_dmemmove(uint16_t in, uint16_t out, uint16_t count) {
    memmove(DMEM_U8(out), DMEM_U8(in), ROUND_UP_16(count));
}

static void cmd_loadadpcm(uint32_t count, uint32_t addr) {
    if (count > sizeof(rsp.book)) count = sizeof(rsp.book);
    memcpy(rsp.book, dram(addr), count);
}

static void cmd_mixer(int16_t gain, uint16_t in_addr, uint16_t out_addr) {
    const int16_t *in = DMEM_S16(in_addr);
    int16_t *out = DMEM_S16(out_addr);
    unsigned n = ROUND_UP_32(rsp.nbytes) / 2, i;
    for (i = 0; i < n; i++) {
        int32_t v = ((int32_t)out[i] * 0x7FFF + (int32_t)in[i] * gain + 0x4000) >> 15;
        out[i] = clamp16(v);
    }
}

static void cmd_interleave(uint16_t left, uint16_t right) {
    const int16_t *l = DMEM_S16(left);
    const int16_t *r = DMEM_S16(right);
    int16_t *d = DMEM_S16(rsp.out);
    unsigned n = ROUND_UP_16(rsp.nbytes) / 2, i; /* nbytes = bytes per channel */
    for (i = 0; i < n; i++) {
        d[2 * i] = l[i];
        d[2 * i + 1] = r[i];
    }
}

/* ADPCM: the 16-sample history is written first (the resampler that follows
 * needs it), then each 9-byte frame decodes to 16 samples. */
static void cmd_adpcm(uint8_t flags, uint32_t state_addr) {
    const uint8_t *in = DMEM_U8(rsp.in);
    int16_t *out = DMEM_S16(rsp.out);
    int16_t *state = (int16_t *)dram(state_addr);
    int nbytes = (int)ROUND_UP_32(rsp.nbytes);

    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if ((flags & A_LOOP) && rsp.loop_state != NULL) {
        memcpy(out, rsp.loop_state, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }
    out += 16;

    while (nbytes > 0) {
        int shift = *in >> 4;
        int table_index = *in++ & 0xF;
        int16_t (*tbl)[8] = rsp.book[table_index];
        int half;
        for (half = 0; half < 2; half++) {
            int16_t ins[8];
            int16_t prev1 = out[-1];
            int16_t prev2 = out[-2];
            int j, k;
            for (j = 0; j < 4; j++) {
                ins[j * 2] = (int16_t)((((int32_t)(*in >> 4) << 28) >> 28) << shift);
                ins[j * 2 + 1] = (int16_t)((((int32_t)(*in++ & 0xF) << 28) >> 28) << shift);
            }
            for (j = 0; j < 8; j++) {
                int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + ((int32_t)ins[j] << 11);
                for (k = 0; k < j; k++) {
                    acc += tbl[1][j - k - 1] * ins[k];
                }
                *out++ = clamp16(acc >> 11);
            }
        }
        nbytes -= 16 * (int)sizeof(int16_t);
    }
    memcpy(state, out - 16, 16 * sizeof(int16_t));
}

/* Resampler state: 4 history samples, then the 16.16 phase accumulator. */
static void cmd_resample(uint8_t flags, uint16_t pitch, uint32_t state_addr) {
    int16_t *state = (int16_t *)dram(state_addr);
    int16_t *in = DMEM_S16(rsp.in);
    int16_t *out = DMEM_S16(rsp.out);
    int nbytes = (int)ROUND_UP_16(rsp.nbytes);
    uint32_t acc;
    int16_t hist[16];

    if (flags & A_INIT) {
        memset(hist, 0, sizeof(hist));
    } else {
        memcpy(hist, state, sizeof(hist));
    }
    in -= 4;
    memcpy(in, hist, 4 * sizeof(int16_t)); /* into the guard bytes before the input */
    acc = (uint16_t)hist[4];

    while (nbytes > 0) {
        int i;
        for (i = 0; i < 8; i++) {
            const int16_t *tbl = resample_table[(acc * 64) >> 16];
            int32_t s = ((in[0] * tbl[0] + 0x4000) >> 15) + ((in[1] * tbl[1] + 0x4000) >> 15)
                      + ((in[2] * tbl[2] + 0x4000) >> 15) + ((in[3] * tbl[3] + 0x4000) >> 15);
            *out++ = clamp16(s);
            acc += (uint32_t)pitch << 1;
            in += acc >> 16;
            acc &= 0xFFFF;
        }
        nbytes -= 8 * (int)sizeof(int16_t);
    }
    memcpy(state, in, 4 * sizeof(int16_t));
    state[4] = (int16_t)acc;
}

/* Envelope mixer. Each channel's volume ramps exponentially per sample
 * (rate is a 16.16 multiplier) toward a target; the input is added to the
 * dry L/R buses scaled by volume * dry and, with A_AUX, to the wet buses
 * scaled by volume * wet. The microcode keeps 8 volume lanes per channel
 * (one per vector slot); the 80-byte state carries lanes, targets, rates and
 * gains, in the layout sm64-port recovered. */
static void cmd_envmixer(uint8_t flags, uint32_t state_addr) {
    int16_t *state = (int16_t *)dram(state_addr);
    const int16_t *in = DMEM_S16(rsp.in);
    int16_t *dry[2] = { DMEM_S16(rsp.out), DMEM_S16(rsp.dry_right) };
    int16_t *wet[2] = { DMEM_S16(rsp.wet_left), DMEM_S16(rsp.wet_right) };
    int nbytes = (int)ROUND_UP_16(rsp.nbytes);
    int16_t target[2];
    int32_t rate[2];
    int16_t vol_dry, vol_wet;
    int32_t vols[2][8];
    int c, i;

    if (flags & A_INIT) {
        for (c = 0; c < 2; c++) {
            int32_t step = rsp.vol[c] * (rsp.rate[c] - 0x10000) / 8;
            target[c] = rsp.target[c];
            rate[c] = rsp.rate[c];
            for (i = 0; i < 8; i++) {
                vols[c][i] = clamp32((int64_t)((int32_t)rsp.vol[c] << 16) + (int64_t)step * (i + 1));
            }
        }
        vol_dry = rsp.vol_dry;
        vol_wet = rsp.vol_wet;
    } else {
        memcpy(vols[0], state, 32);
        memcpy(vols[1], state + 16, 32);
        target[0] = state[32];
        target[1] = state[35];
        rate[0] = (state[33] << 16) | (uint16_t)state[34];
        rate[1] = (state[36] << 16) | (uint16_t)state[37];
        vol_dry = state[38];
        vol_wet = state[39];
    }

    while (nbytes > 0) {
        for (c = 0; c < 2; c++) {
            for (i = 0; i < 8; i++) {
                int32_t v;
                if ((rate[c] >> 16) > 0) {
                    if ((vols[c][i] >> 16) > target[c]) vols[c][i] = target[c] << 16;
                } else {
                    if ((vols[c][i] >> 16) < target[c]) vols[c][i] = target[c] << 16;
                }
                v = vols[c][i] >> 16;
                dry[c][i] = clamp16((dry[c][i] * 0x7FFF + in[i] * ((v * vol_dry + 0x4000) >> 15) + 0x4000) >> 15);
                if (flags & A_AUX) {
                    wet[c][i] = clamp16((wet[c][i] * 0x7FFF + in[i] * ((v * vol_wet + 0x4000) >> 15) + 0x4000) >> 15);
                }
                vols[c][i] = clamp32(((int64_t)vols[c][i] * rate[c]) >> 16);
            }
            dry[c] += 8;
            wet[c] += 8;
        }
        in += 8;
        nbytes -= 16;
    }

    memcpy(state, vols[0], 32);
    memcpy(state + 16, vols[1], 32);
    state[32] = target[0];
    state[35] = target[1];
    state[33] = (int16_t)(rate[0] >> 16);
    state[34] = (int16_t)rate[0];
    state[36] = (int16_t)(rate[1] >> 16);
    state[37] = (int16_t)rate[1];
    state[38] = vol_dry;
    state[39] = vol_wet;
}

/* Low-pass filter used by the reverb: libaudio's _init_lpfilter loads 16
 * coefficients that are the powers of the pole (its unrolled 8-sample form)
 * and passes gain = 1 - pole, i.e. the filter is the first-order recurrence
 *     y[n] = gain * x[n] + pole * y[n-1]
 * The state keeps the last output. */
static void cmd_polef(uint8_t flags, int16_t gain, uint32_t state_addr) {
    int16_t *state = (int16_t *)dram(state_addr);
    int32_t pole = rsp.book[0][1][0]; /* coefficient 8 = fc */
    const int16_t *in = DMEM_S16(rsp.in);
    int16_t *out = DMEM_S16(rsp.out);
    unsigned n = ROUND_UP_16(rsp.nbytes) / 2, i;
    int32_t prev = (flags & A_INIT) ? 0 : state[3];

    for (i = 0; i < n; i++) {
        prev = clamp16((in[i] * gain + prev * pole + 0x4000) >> 15);
        out[i] = (int16_t)prev;
    }
    state[0] = state[1] = state[2] = 0;
    state[3] = (int16_t)prev;
}

/* ---- task entry ---------------------------------------------------------- */

void sbk_audio_task(OSTask *task) {
    const Acmd *cmd = (const Acmd *)sbk_phys_to_host((uint32_t)(uintptr_t)task->t.data_ptr);
    unsigned n = task->t.data_size / sizeof(Acmd), i;
    static unsigned unknown_warned;

    if (sbk_audio_disabled) return;
    for (i = 0; i < n; i++, cmd++) {
        uint32_t w0 = cmd->words.w0, w1 = cmd->words.w1;
        cur_w0 = w0; cur_w1 = w1;
        uint8_t op = (uint8_t)(w0 >> 24);
        uint8_t flags = (uint8_t)(w0 >> 16);
        switch (op) {
            case A_SPNOOP: break;
            case A_ADPCM: cmd_adpcm(flags, w1); break;
            case A_CLEARBUFF: cmd_clearbuff((uint16_t)(w0 & 0xFFFF), w1 & 0xFFFF); break;
            case A_ENVMIXER: cmd_envmixer(flags, w1); break;
            case A_LOADBUFF: cmd_loadbuff(w1); break;
            case A_RESAMPLE: cmd_resample(flags, (uint16_t)(w0 & 0xFFFF), w1); break;
            case A_SAVEBUFF: cmd_savebuff(w1); break;
            case A_SEGMENT: rsp.segments[(w1 >> 24) & 0xF] = w1 & 0x00FFFFFFu; break;
            case A_SETBUFF: cmd_setbuff(flags, (uint16_t)(w0 & 0xFFFF), (uint16_t)(w1 >> 16), (uint16_t)(w1 & 0xFFFF)); break;
            case A_SETVOL: cmd_setvol(flags, (int16_t)(w0 & 0xFFFF), w1); break;
            case A_DMEMMOVE: cmd_dmemmove((uint16_t)(w0 & 0xFFFF), (uint16_t)(w1 >> 16), (uint16_t)(w1 & 0xFFFF)); break;
            case A_LOADADPCM: cmd_loadadpcm(w0 & 0xFFFFFF, w1); break;
            case A_MIXER: cmd_mixer((int16_t)(w0 & 0xFFFF), (uint16_t)(w1 >> 16), (uint16_t)(w1 & 0xFFFF)); break;
            case A_INTERLEAVE: cmd_interleave((uint16_t)(w1 >> 16), (uint16_t)(w1 & 0xFFFF)); break;
            case A_POLEF: cmd_polef(flags, (int16_t)(w0 & 0xFFFF), w1); break;
            case A_SETLOOP: rsp.loop_state = (int16_t *)dram(w1); break;
            default:
                if (unknown_warned++ < 8) {
                    fprintf(stderr, "sbk audio: unknown command %02x\n", op);
                }
                break;
        }
    }
}
