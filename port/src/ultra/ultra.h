/* Include libultra's headers from host code.
 *
 * Host system headers go first so size_t and friends come from the host;
 * the host's <errno.h> defines errno as a macro, which would mangle the
 * `errno` fields of OSContPad and friends, so it is undone right before the
 * libultra headers; libultra's libc prototypes are kept out entirely. */
#ifndef SBK_ULTRA_H
#define SBK_ULTRA_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#ifdef errno
#undef errno
#endif
#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#define _OS_LIBC_H_ 1 /* keep PR/os_libc.h out: the host libc serves */
#include <PR/os.h>
#include <PR/rcp.h>
#include <PR/mbi.h>
#include <PR/sptask.h>
#ifdef errno
#undef errno
#endif
#endif
