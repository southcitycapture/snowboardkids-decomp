/* aspMain (audio ucode, ABI 1) command-list interpreter.
 *
 * Milestone 1 stub: the task's output buffer is silenced so the game's audio
 * pipeline (libmus -> libaudio -> command list -> AI) keeps flowing without
 * producing sound. The real interpreter arrives with milestone 3. */
#include "../ultra/ultra.h"
#include <string.h>
#include "../ultra/sbk_os.h"

void sbk_audio_task(OSTask *task) {
    (void)task;
}
