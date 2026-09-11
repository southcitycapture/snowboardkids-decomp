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
| Gamepad: SDL game-controller layer, hot-plug, raw fallback with mapping log | built, untested (no pad seen on USB yet) |
| Fullscreen: exclusive mode, 4:3 letterbox, Cmd+Return/Cmd+F/F11 toggle, on by default from the Finder | done |
| Gamepad: SDL game controllers (HID pads) and Xbox One pads over USB via IOKit | done |
| Rumble Pak: the pad's rumble (Xbox GIP packet or SDL haptic) behind osMotorInit/Start/Stop | done |

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
                  [--dumptris] [--bigtri N] [--racedbg] [--peek ADDR:LEN] [--perf]
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
    port/tools/nightmare_search.py table       # best setup per course so far
    port/tools/nightmare_search.py record 0    # re-run the winner with --record
    port/tools/nightmare_search.py regress     # replay the golden movies

A trial that has not printed a result after 300 s is hung (a healthy one costs
30-60 s of wall clock); the sweep stops it and carries on rather than blocking.

### Golden movies and `regress`

`record N` replays the best winning row for course N once more with
`--record /Users/zach/golden-courseN.m64` and copies the movie into
`port/scripts/golden/courseN.m64`. Because the rider is the game's *own* CPU
logic, the movie holds the menu walk, not the driving: the race is reproduced by
replaying the movie under the same `--trial` spec, which is what `regress` does.

    port/tools/nightmare_search.py regress        # every recorded course
    port/tools/nightmare_search.py regress 0 2    # just these

It prints a pass/fail table: a course passes when the replay finishes with the
same rank *and* the same frame count as the CSV row it was measured from. The
port is deterministic under scripted input, so any drift is a real regression.

    course   spec                             expected       got            result
    9        course=9,char=3,board=2,boost=64 rank=1/13124   rank=1/13124   PASS
    0        course=0,char=1,board=2          rank=1/18066   rank=1/18066   PASS
    1        course=1,char=3,board=2          rank=1/28002   rank=1/28002   PASS
    2        course=2,char=1,board=2          rank=1/22436   rank=1/22436   PASS
    3        course=3,char=1,board=2,boost=64 rank=1/23018   rank=1/23018   PASS
    4        course=4,char=3,board=1,boost=96 rank=1/21138   rank=1/21138   PASS
    5        course=5,char=3,board=1,boost=32 rank=1/22696   rank=1/22696   PASS
    6        course=6,char=4,board=1,boost=64 rank=1/20868   rank=1/20868   PASS
    8 course(s) checked, 0 failed

### What the rider learned (2026-09-10)

Every course in the game has a setup the CPU rider wins with. `boost` is in
1/256ths of the character+board top speed; where it is 0 the stock rider
already wins.

| course | name | char | board | boost | frames |
|---|---|---|---|---|---|
| 9 | Rookie Mountain | 3 | 2 | 64 | 13124 |
| 0 | Big Snowman | 1 | 2 | 0 | 18066 |
| 1 | Sunset Rock | 3 | 2 | 0 | 28002 |
| 2 | Night Highway | 1 | 2 | 0 | 22436 |
| 3 | Grass Valley | 1 | 2 | 64 | 23018 |
| 4 | Dizzy Land | 3 | 1 | 96 | 21138 |
| 5 | Quicksand Valley | 3 | 1 | 32 | 22696 |
| 6 | Silver Mountain | 4 | 1 | 64 | 20868 |

The boost column is not a difficulty dial: because player 1 is a CPU rider it
is rank-handicapped like the rest of the field (see docs/PLAN.md), so a bigger
boost can cost places. Course 3 goes 2nd, 4th, 1st at boost 0, 32, 64.

## Playing the campaign: `--plan` and the driver

`--trial course=-2` aims every race at the first course still unwon, but a
single `--trial` spec cannot carry a different rider for each course.
`--plan COURSE:CHAR:BOARD:BOOST,...` is the rider's book: at each race's init
the port looks up the course the race actually started on and applies that row.
`nightmare_search.py plan` prints the book straight out of the CSV, and
`campaign` starts a session with it on the experiment Controller Pak
(`/Users/zach/trial-pak.mpk` -- never the user's own save):

    port/tools/nightmare_search.py campaign     # start the session
    port/tools/nightmare_search.py drive 60     # walk it through the menus
    port/tools/nightmare_search.py status       # the last sbk-status line

`drive` is the autopilot: between races it feeds A presses through `--cmds`,
and it stays quiet while a race is under way.

First driven session (2026-09-10): Rookie Mountain and Big Snowman won,
20,200G earned over six races. Not yet solved: the driver never stops on the
Game Menu, so EXIT / SAVE never runs and nothing reaches the pak; and Sunset
Rock, which the trial wins at boost 0, came 3rd three times and 2nd once under
campaign conditions -- the book wants re-measuring from a played save rather
than a fresh one. See docs/PLAN.md.

**Never press START from the driver.** It is what `--soak`'s monkey does, and a
START still queued when the next race begins *pauses* that race. Worse, the
PAUSE / CONTINUE / QUIT / RETRY overlay does not answer A at all -- only
another START dismisses it -- so the session sits on the overlay forever. The
first Big Snowman run lost 20 minutes of game time to exactly this. `nudge`
sends A and stick-up only.

## Gamepad

HID pads (DualShock 4, most "DirectInput" pads, Xbox 360 with a driver) come
through SDL's game-controller layer. Xbox One pads are not HID: they speak
Microsoft's GIP protocol on a vendor interface and Leopard has no driver, so
`port/src/platform/input_xone.c` talks to the pad itself through IOUSBLib
(interface class ff/47/d0, power-on packets, 64-byte interrupt reports, the
layout the Linux xpad driver documents). The SDL build for this is
`isle-ppc-tools/tiger/build-sdl2-tiger-joy.sh` (joystick + haptic on, an
IOHIDManager shim for the 10.4 SDK).

Mapping: left stick = stick, A = A, B and X = B, Y = C-down (item), triggers
= Z, LB/RB = L/R, Menu/View = Start, D-pad = D-pad, right stick = C buttons.

The Rumble Pak is the pad's own rumble: `osMotorInit` reports a pak on port 1
whenever the pad can rumble, Start/Stop drive it (a GIP rumble packet on Xbox
pads, SDL haptic rumble otherwise). A Controller Pak and a Rumble Pak are both
present at once here, which a real controller cannot do, so every swap prompt
passes immediately.

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

`--bigtri N` logs every on-screen triangle covering more than N pixels with the
modelview matrix, the `G_MTX` address it came from and the `G_VTX` source array;
`--dumptris` lines carry the same fields. Looking a `vtx=` address up in
`build/snowboardkids.map` names the game function that drew it. That is how the
"stretched model at an item hit" report was traced to the game's own pickup
shards (see `docs/PLAN.md`) -- the retail ROM in mupen64plus draws the same
panels, so it is not a port bug.

Design notes, survey facts and the gotchas are in `docs/PLAN.md`.

## Licence note

`src/gfx/gfx_pc.c` derives from sm64-port, whose licence allows source
distribution only. Share this branch as source; do not distribute binaries.
