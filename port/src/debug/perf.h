#ifndef SBK_PERF_H
#define SBK_PERF_H

enum { SBK_PERF_GAME, SBK_PERF_GFX, SBK_PERF_AUDIO, SBK_PERF_PRESENT, SBK_PERF_IDLE, SBK_PERF_ENDFRAME, SBK_PERF_FINISH, SBK_PERF_SWAP, SBK_PERF_PHASES };

extern int sbk_perf_enabled;
extern unsigned long sbk_perf_tris, sbk_perf_draws, sbk_perf_tex, sbk_perf_tex_bytes;

double sbk_perf_now(void);
void sbk_perf_add(int phase, double usec);
void sbk_perf_frame(void);
void sbk_perf_report(void);

/* Time a call: SBK_PERF_TIMED(SBK_PERF_GFX, fn(args)); */
#define SBK_PERF_TIMED(phase, call) do { double _t0 = sbk_perf_now(); call; sbk_perf_add((phase), sbk_perf_now() - _t0); } while (0)

#endif
