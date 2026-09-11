/* Leftovers the game references from libultra/IDO land that have no place in
 * the port: debug printf, the assert hook, IDO's 64-bit helpers, and the RSP
 * microcode blobs (only their addresses are used, to size the boot ucode). */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

void rmonPrintf(const char *fmt, ...) {
    (void)fmt;
}

void osSyncPrintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void __assert(const char *exp, const char *file, int line) {
    fprintf(stderr, "sbk: assertion failed: %s (%s:%d)\n", exp, file, line);
    abort();
}

long long __ll_mul(long long a, long long b) { return a * b; }
long long __ll_div(long long a, long long b) { return a / b; }
long long __ll_rem(long long a, long long b) { return a % b; }
long long __ll_lshift(long long a, long long b) { return a << b; }
long long __ll_rshift(long long a, long long b) { return a >> b; }
unsigned long long __ull_div(unsigned long long a, unsigned long long b) { return a / b; }
unsigned long long __ull_rem(unsigned long long a, unsigned long long b) { return a % b; }
unsigned long long __ull_rshift(unsigned long long a, unsigned long long b) { return a >> b; }
double __ll_to_d(long long a) { return (double)a; }
float __ll_to_f(long long a) { return (float)a; }
long long __d_to_ll(double d) { return (long long)d; }
long long __f_to_ll(float f) { return (long long)f; }
unsigned long long __d_to_ull(double d) { return (unsigned long long)d; }

/* The RSP microcode symbols (rspbootTextStart, aspMainTextStart,
 * gF3dlxMicrocodeText) come from rom_syms.s at their N64 addresses; the game
 * only subtracts them to size the boot ucode. */

/* --drawdistance N: scales the race far plane and the prop cull range (see port/patches.txt) */
float sbk_far_scale = 1.0f;
