/* mupen64plus input plugin: replay a Mupen64 .m64 movie instead of reading a
 * controller, so the emulator can be driven by exactly the input that drives
 * the port and its frames can be used as colour- and frame-exact references.
 * mupen64plus-ui-console has no movie playback of its own; this hands out one
 * .m64 sample per controller read on port 1 and nothing on the others.  The
 * movie is named by the SBK_M64 environment variable and is opened at plugin
 * startup; the sample cursor rewinds to the first sample on every RomOpen.
 * Build it against the mupen64plus headers Homebrew installs (they come with
 * the mupen64plus-core formula), then point the emulator at the .dylib:
 *
 *     cc -O2 -fPIC -shared -I/opt/homebrew/include \
 *        -o mupen64plus-input-m64.dylib port/tools/input_m64.c
 *     SBK_M64=/tmp/ref.m64 \
 *       mupen64plus --windowed --resolution 640x480 --nosaveoptions \
 *                   --plugindir /opt/homebrew/lib/mupen64plus \
 *                   --input /tmp/mupen64plus-input-m64.dylib \
 *                   --testshots 1800,2000,99999 "Snowboard Kids 2 (USA).z64"
 *
 * `--plugindir` is not optional: given only `--input`, the core looks for the
 * video plugin next to this one, does not find it, and dies in osd_init.
 *
 * See port/docs/reference-frames.md for the whole reference-frame workflow.
 * Not built by the port's Makefile: it is a host-side audit tool, never
 * shipped, and it only ever has to run on the developer's own Mac. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define M64P_PLUGIN_PROTOTYPES 1
#include <mupen64plus/m64p_types.h>
#include <mupen64plus/m64p_plugin.h>
#include <mupen64plus/m64p_common.h>

static unsigned char *movie; static long movie_len; static long pos = 1024;
static CONTROL *controls;

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle h, void *ctx,
                                     void (*cb)(void *, int, const char *)) {
    const char *path = getenv("SBK_M64");
    FILE *f;
    (void)h; (void)ctx; (void)cb;
    if (path == NULL) { fprintf(stderr, "input_m64: set SBK_M64\n"); return M64ERR_SUCCESS; }
    f = fopen(path, "rb");
    if (f == NULL) { fprintf(stderr, "input_m64: cannot open %s\n", path); return M64ERR_SUCCESS; }
    fseek(f, 0, SEEK_END); movie_len = ftell(f); fseek(f, 0, SEEK_SET);
    movie = malloc((size_t)movie_len);
    if (fread(movie, 1, (size_t)movie_len, f) != (size_t)movie_len) { free(movie); movie = NULL; }
    fclose(f);
    fprintf(stderr, "input_m64: %s, %ld samples\n", path, movie ? (movie_len - 1024) / 4 : 0L);
    return M64ERR_SUCCESS;
}
EXPORT m64p_error CALL PluginShutdown(void) { free(movie); movie = NULL; return M64ERR_SUCCESS; }
EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *t, int *v, int *api, const char **n, int *cap) {
    if (t) *t = M64PLUGIN_INPUT;
    if (v) *v = 0x000100;
    if (api) *api = 0x020100;
    if (n) *n = "m64 movie replay";
    if (cap) *cap = 0;
    return M64ERR_SUCCESS;
}
EXPORT void CALL InitiateControllers(CONTROL_INFO info) {
    int i;
    controls = info.Controls;
    for (i = 0; i < 4; i++) { controls[i].Present = (i == 0); controls[i].RawData = 0; controls[i].Plugin = PLUGIN_NONE; }
}
EXPORT void CALL GetKeys(int c, BUTTONS *k) {
    k->Value = 0;
    if (c != 0 || movie == NULL) return;
    if (pos + 4 <= movie_len) {
        /* .m64 sample bytes are the N64's own SI order, which is exactly the
         * byte order of mupen's little-endian BUTTONS union: b0 buttons
         * A..R_DPAD, b1 L/R/C, b2 stick X, b3 stick Y. */
        k->Value = (unsigned int)movie[pos] | (unsigned int)movie[pos + 1] << 8
                 | (unsigned int)movie[pos + 2] << 16 | (unsigned int)movie[pos + 3] << 24;
        pos += 4;
    }
}
EXPORT void CALL ControllerCommand(int c, unsigned char *d) { (void)c; (void)d; }
EXPORT void CALL ReadController(int c, unsigned char *d) { (void)c; (void)d; }
EXPORT int CALL RomOpen(void) { pos = 1024; return 1; }
EXPORT void CALL RomClosed(void) { }
EXPORT void CALL SDL_KeyDown(int m, int s) { (void)m; (void)s; }
EXPORT void CALL SDL_KeyUp(int m, int s) { (void)m; (void)s; }
EXPORT void CALL RenderCallback(void) { }
