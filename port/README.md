# Snowboard Kids on the Power Mac G4

A native PowerPC port of the [Snowboard Kids decompilation](https://github.com/cdlewis/snowboardkids-decomp)
for a Quicksilver G4 running Mac OS X 10.5 with a Radeon 9000. The game sources
are untouched; everything lives in this `port/` directory: a libultra
replacement, an interpreter for the display lists and the audio command lists,
a fixed-function OpenGL 1.3 backend, scripted input.

## Status (10 September 2026)

| Milestone | State |
| --- | --- |
| Toolchain: matching ROM build on the Mac, cross compile in Docker | done |
| First frame: boot, threads, DMA, title demo on the real G4 | done |
| Menus and gameplay: every menu, a race on Rookie Mt. | done |
| Controller Pak: 32 KB image, save and load through libultra's own pfs code | done, `controller-pak-1.mpk` in Application Support |
| Audio: aspMain (ABI 1) interpreter feeding SDL at 22050 Hz | done, music and effects play |
| Performance: `--perf` phase timing; a race uses ~6 of 16.7 ms per retrace on the 1 GHz G4 | measured, headroom |
| Self-play: `--autoplay` (CPU drives player 1), `--soak` (also walks menus), `--nightmare` | done |
| Fullscreen: exclusive mode, 4:3 letterbox, Cmd+Return/Cmd+F/F11 toggle, on by default from the Finder | done |
| Gamepad, Rumble | next, in that order |

Scripted input is deterministic: `--play` a text script or a Mupen `.m64`
movie, `--record` one from a keyboard session, and two runs of the same script
end on the same frame hash (`--frames N --hashframe`).

## Build and run

    make ...                      # the upstream N64 build first (map, linker script, assets)
    port/build-ppc.sh -j6         # Docker cross build -> port/build-ppc-darwin/snowboardkids
    port/tools/make_bundle.sh     # SnowboardKids.app with the ROM in Resources

    snowboardkids [--fullscreen] [--play SCRIPT|MOVIE.m64] [--record MOVIE.m64]
                  [--frames N] [--hashframe] [--mute] [--noaudio] [--wav OUT.wav]
                  [--pak FILE.mpk] [--cmds FILE] [--trace] [--dumpdl N] [--dumpframes N]
                  [--dumptris] [--racedbg] [--peek ADDR:LEN] [--perf]
                  [--autoplay] [--soak] [--nightmare] [--trial SPEC]
                  [--status] [--coursetrace] [rom.z64]

Scripts in `scripts/`: `title-start.txt`, `menu-walk.txt` (to mode select),
`race-walk.txt` (through the pak prompts into a race), `race-drive.txt`
(the same, then taps A with the stick forward), `pak-save.txt`. `--cmds FILE`
appends script lines dropped into FILE at runtime, for driving menus step by step.

## Letting it play itself

`--autoplay` hands player 1 to the game's own CPU rider logic as soon as a
race starts, so the game drives every course with the routes it already
knows. `--soak` adds a menu monkey between races (YES-and-confirm, confirm,
START, confirm every 1.5 s) for unattended beta testing; `--nightmare` sets
every CPU rider's item and trick chance to the maximum. `--perf` prints a
per-second line (and puts it in the window title) with the time spent in
game logic, display lists, audio, present and idle, the worst frame, and
triangle / draw / texture-upload counts. `g4 top` shows the machine's load.

## Making the CPU rider win: `--trial`

`--trial course=N,char=N,board=N,action=N,item=N,boost=N,money=N,quit=1` runs
one measured race. At the race's own init (after the game's tuning) player 1 is
re-tuned from the character and board tables with `boost` added to its top
speed in 1/256ths, the item and trick chances are pinned for the whole race,
and one line is printed when player 1 crosses the line:

    sbk-trial: result course=9 char=3 board=2 action=255 item=255 boost=64 \
               rank=1 finished_before=0 frames=13124 money=0

`course=N` aims the character-select course menu. That menu keeps a *cursor
index* into `gCharacterSelectActiveCourseOptions` in `gRaceCourseIndex` and
only converts it to a real course id when it fades out, so the trial parks the
cursor on the wanted course while the list is live and lets the script's own A
press confirm it. Identifying "the list is live" from the port needed
measuring: `gCurrentGameTask` is NULL between dispatches and
`gRacePlayers[0].isActive` is 1 in the menus too, so the tell is
`gRacePlayers[0].menuState == 0` plus the list cursor state (`--coursetrace`
prints both). Raising `gHighestUnlockedCourse` (the game only ever raises it)
puts every course in the list. Course ids are the game's own: **9 is the course
the menu starts on (Rookie Mt.)**, 0-6 are the rest.

`--status` prints the campaign scoreboard (purse, saved purse, progression
level, per-course unlock states) whenever the money changes and once every five
seconds. Progression, from `initRaceStartTransition`: level 1 needs a win on
courses 0-4 and 9, level 2 adds course 5, level 3 adds course 6 and rolls the
ending credits.

`port/tools/nightmare_search.py` drives trials on the G4 headless (~30 s each,
12x real time) and appends to `port/tools/nightmare_results.csv`:

    port/tools/nightmare_search.py run course=0,char=1,board=2
    port/tools/nightmare_search.py sweep 0 1 2 3 4 5 6

## Fullscreen

`--fullscreen` switches the display to its desktop mode and draws the N64
frame as a 4:3 box in the middle (`--fullscreen=1024x768` picks another
mode, `--fullscreen-desktop` uses a borderless window instead, `--wide`
fills the width sm64-port style). Launching the app from the Finder starts
fullscreen (`SBK_FULLSCREEN=1` does the same from a shell); `--windowed` keeps
the 640x480 window. Cmd+Return, Cmd+F, Option+Return or F11 toggle at runtime.

Note for remote testing: a connected Screen Sharing (VNC) client makes the
VNC server read the fullscreen surface about once a second, which stalls the
swap ~120 ms at 1680x1050 and shows as a flicker on the remote view. The
G4's own display does not have this.

The Controller Pak lives at `~/Library/Application Support/SnowboardKids/controller-pak-1.mpk`,
the raw 32 KB layout emulators use, so saves can move both ways.

Design notes, survey facts and the gotchas are in `docs/PLAN.md`.

## Licence note

`src/gfx/gfx_pc.c` derives from sm64-port, whose licence allows source
distribution only. Share this branch as source; do not distribute binaries.
