# Snowboard Kids: native port to the Power Mac G4

Goal: run the 100%-matched Snowboard Kids decompilation natively on the
Quicksilver G4 (Leopard 10.5, Radeon 9000), the third step of the G4 quest
line. Upstream is a *matching decomp* (it rebuilds the ROM byte for byte and is
explicitly not a port), so this branch adds a platform layer next to the game
code, sm64-port style, and leaves the game sources as untouched as possible.

## Architecture

```
 game code (src/menu, race, demo, ending, engine, audio, libmus, math)   unchanged
 libaudio synthesizer + gu math (src/ultra/audio, src/ultra/gu)          unchanged C
 ----------------------------------------------------------------------------------
 port/src/ultra   libultra replacement: threads as coroutines, message queues,
                  events, VI, SP task dispatch, PI DMA from the ROM file, AI,
                  controllers, Controller Pak (later), cache ops as no-ops
 port/src/gfx     gfx_pc.c display-list interpreter (Fast3D layer from
                  sm64-port, F3DEX_GBI build) + a new fixed-function OpenGL 1.3
                  backend for the Radeon 9000 + SDL2 window/input
 port/src/audio   interpreter for the aspMain (ABI 1) audio command lists
 port/src/main.c  host loop: run threads, deliver retraces/events, pace 60 Hz
```

### Facts from the survey that drive the design

- Threads (id/priority): boot 1/10 (then parks itself at 0 = idle), game 2/10,
  controller 4/20, swap-buffer 5/100, audio 3/110, scheduler 6/120, plus
  libultra's PI (150) and VI (254) managers, which the port replaces.
- The game thread polls three queues with OS_MESG_NOBLOCK in a busy loop and
  relies on interrupts to preempt it. Port rule: an empty NOBLOCK poll yields to
  the host loop, which delivers pending events (VI retrace, SP/DP done, SI).
- Game logic runs on alternate retraces (30 Hz); audio task and render task
  submissions are retrace driven; there is **no** osGetTime/osSetTimer use.
- VI mode NTSC LAN1, 320x240 RGBA5551, triple buffered, one Z buffer; the CPU
  never reads or writes framebuffer pixels. All fades are RDP-side.
- Graphics ucode F3DLX (F3DEX_GBI command set, G_TRI2 etc.), submitted as
  M_GFXTASK; audio is the classic libaudio (`alInit`, not n_audio) so the
  aspMain command list is ABI 1 (A_ENVMIXER/A_RESAMPLE/A_ADPCM/A_MIXER...).
- ROM access: `dmaReadRom()` (osPiStartDma in 8 KB chunks) and the audio DMA
  callback. Assets are addressed by `NAME_ROM_START/_ROM_END` linker symbols and
  display lists by segmented `NAME_VRAM` symbols; the port emits them as
  absolute symbols mined from the N64 build's map (port/tools/gen_rom_syms.py).
- Saves go to the Controller Pak only (osPfs*, note "SNOWBOARD KIDS", game code
  NSKE, 0x7900 bytes); Rumble Pak via osMotor*; no EEPROM/SRAM.
- The title demo bakes absolute RDRAM pointers (0x8017xxxx into the relocatable
  heap) into replay data, and the menu renderer turns pointers into physical
  addresses with `+ 0x80000000`. So the port maps a fixed 4 MB "RDRAM" at
  0x80000000 (verified to work on Leopard PPC) and places the big game buffers
  (relocatable heap, RSP buffers, framebuffers) at their N64 addresses.
- 15 `#pragma weak A = B` aliases (IDO-only) crash GCC on Mach-O; the port
  build filters them out and provides the aliases as real wrapper functions.
- Darwin PPC packs double/long long at 4 bytes inside structs; MIPS o32 uses 8.
  Build everything with `-malign-natural` (verified on the G4).

### Target hardware (verified on the real G4)

Radeon 9000, OpenGL 1.3 ATI-1.5.28, 6 texture units, 2048 max texture,
ARB_texture_env_combine + ATI_texture_env_combine3 + crossbar, vertex programs
only (no fragment programs, no FBO, no NPOT). CGL contexts work from an SSH
session, so headless render tests do not need the console runner.

## Milestones

0. **Toolchain** (done 2026-09-09): matching ROM build on the Mac; PPC smoke
   compile of all game files; GL and mmap probes on the G4.
1. **Boot to first frame**: libultra layer, gfx_pc + GL 1.3 backend, stubbed
   audio task, keyboard input; `g4 shot` shows the title/intro rendering.
2. **Menus + gameplay**: combiner coverage for everything the game draws,
   controller mapping, frame pacing, Controller Pak emulation (a 32 KB file).
   DONE 2026-09-10 including the Controller Pak (evening: libultra pfs code
   over a 32 KB image, os_pfs.c; save at EXIT / SAVE verified, reload on
   restart verified): every menu through to a race
   on Rookie Mt. renders and runs (scripts/race-walk.txt, scripts/race-drive.txt);
   text steady (content-hashed texture cache), title fade smooth (overrun
   row trim), the race-entry crash fixed (garbage SETTIMG skipped). Scripted
   playback is deterministic (idle-gated retraces, simulated AI length).
3. **Audio**: ABI 1 command-list interpreter feeding SDL audio at 22050 Hz.
   DONE 2026-09-10: port/src/audio/audio_task.c runs the game's aspMain lists
   on a host DMEM image (ADPCM, 4-tap resampler, per-sample envelope lanes,
   mixer, reverb delay lines with the first-order low-pass, interleave).
   `--wav OUT.wav` captures the AI stream, `--mute` queues silence instead.
   Verified on the G4: title theme, START sweep and menu music; the frame
   hash of the deterministic menu-walk run is unchanged.
4. **Polish**: performance on the G4, fullscreen, gamepad, Rumble, save UX.
   Performance measured 2026-09-10 (--perf): a race costs ~6 ms per 16.7 ms
   retrace (game 1.2, gfx 2-3.5, audio 1, present 1-2), 30-40% of one CPU;
   worst frames 20-50 ms come from vsync waits in present and display-list
   bursts. Self-play: --autoplay/--soak/--nightmare (port/src/debug/race_dbg.c).
   Fullscreen DONE 2026-09-10 night: SDL_WINDOW_FULLSCREEN (mode switch) with a
   4:3 letterbox applied in the GL backend (output rect offset on viewport and
   scissor; window cleared black per frame). FULLSCREEN_DESKTOP costs ~8 ms per
   present at 1680x1050, exclusive ~3 ms. A connected VNC client stalls every
   swap ~120 ms about once a second (AppleVNCServer reading the surface).
   Gamepad DONE 2026-09-11 (SDL controllers + an IOKit driver for Xbox One pads,
   port/src/platform/input_xone.c). Rumble Pak DONE 2026-09-11: osMotor* over the
   pad's rumble (GIP packet 0x09 on Xbox pads, SDL haptic otherwise).
5. **Letting the CPU rider learn the game** (2026-09-10 night): `--trial
   course=,char=,board=,action=,item=,boost=,money=,quit=` runs one measured
   race headless (~30 s on the G4 at 12x) and prints one result line;
   `port/tools/nightmare_search.py` sweeps and keeps
   `port/tools/nightmare_results.csv`.

   How the course is chosen from the port, since this cost the most time to
   establish: the character-select course menu keeps a *cursor index* into
   `gCharacterSelectActiveCourseOptions` in `gRaceCourseIndex` and converts it
   to a real course id only in `fadeOutCharacterSelectCourseMenu`. So a trial
   parks the cursor on the course it wants and lets the script's own A press
   confirm -- the game's normal flow, just aimed. The two obvious ways to tell
   "the course list is on screen" both fail: `gCurrentGameTask` is NULL between
   the scheduler's dispatches, and `gRacePlayers[0].isActive` is 1 in the menus
   too (an early version of the trial hooked the wrong branch because of it and
   silently did nothing). What works is the menu's own state:
   `gRacePlayers[0].menuState == 0` while the list is navigable and >= 7 once a
   choice is confirmed, with `gCharacterSelectCourseCursorState.listCursorState`
   zeroed at the menu's init to re-arm. `--coursetrace` prints all of it.

   Course ids are the game's own and the menu's order is `9, 0, 1, 2, 3, 4, 5,
   6`: **course 9 is the one the menu starts on**, so every result measured
   before `course=` existed was course 9, not course 0. `gHighestUnlockedCourse`
   picks which of the three option lists the menu offers and the game only ever
   raises it, so raising it from the port exposes 5 and 6 without the shop.

   Why a bigger `boost` is not always better (the surprise of the sweep): the
   trial's boost lands in `unk25C`, and `updateRacePlayer` rebuilds the frame's
   target speed `unk310` from `unk25C` every frame -- but for a *CPU* rider it
   then applies the rank handicap in `race_player_update.c` ~625-645:
   `rankArrow == 1` (behind) adds 0x70000, `rankArrow == 2` (out in front)
   divides the target by **three**, `rankArrow == 3` shaves 1/16th. Player 1 is
   a CPU rider under `--autoplay`, so it is handicapped like any other: more top
   speed takes the lead sooner and buys the /3 throttle sooner. Measured on
   course 3 (Grass Valley), char=1 board=2: boost 0 -> 2nd in 24366 frames,
   boost 32 -> **4th** in 23520, boost 64 -> 1st in 23018. The ladder has to be
   searched, not extrapolated.

   Progression, from `initRaceStartTransition` (`cupPlacements` is the save's
   per-course "took first here" flag, index 0x18 of it *is* progressionLevel):
   level 1 needs wins on courses 0-4 and 9, level 2 adds 5, level 3 adds 6 and
   sets `gPendingEndingCreditsFlow`. `--status` prints the purse, the
   progression level, the unlock states and the win flags; `course=-2` aims each
   race at the first course still unwon.

6. **The rider learns the game** (2026-09-10 night). Every course the menu
   offers (9 and 0-6) now has a setup the CPU rider takes first place with;
   `port/tools/nightmare_search.py table` prints the book and
   `port/scripts/golden/courseN.m64` is the recorded run for each, replayed by
   `nightmare_search.py regress` (8 checked, 0 failed, every rank and frame
   count identical). `--plan COURSE:CHAR:BOARD:BOOST,...` feeds the book to a
   campaign session, which `--trial course=-2` aims at the first course still
   unwon.

   Campaign state as of the first driven session (experiment pak
   `/Users/zach/trial-pak.mpk`, never the user's own save): Rookie Mountain and
   Big Snowman won, 20,200G in the purse, six races run. Two things are still
   open:

   - **The purse is not saved yet.** The Controller Pak is written only from
     the Game Menu's EXIT / SAVE, and the driver's A presses walk straight from
     the results screen back into the next race without ever stopping on the
     Game Menu, so `savemoney` stayed 0. The driver needs a way back (B out of
     the course select, most likely) before any course can be bought.
   - **A trial win does not always transfer.** Sunset Rock (course 1) is won by
     char=3 board=2 at boost 0 from a fresh save, but the same setup came 3rd,
     3rd, 3rd and 2nd in the campaign. The trial races a fresh save and the
     campaign does not, so the book needs re-measuring *in campaign conditions*,
     not just from the title screen.

## Build

    make ... (N64 build, once)        # map + linker script + assets
    port/build-ppc.sh -j8              # Docker cross-build -> port/build-ppc/snowboardkids
    g4 push / g4 run / g4 shot         # via isle-ppc-tools (needs a generic exe name)

## Licenses

`port/src/gfx/gfx_pc.c`, `gfx_cc.*` and the API headers come from sm64-port
(Emill, MaikelChan) under a source-only license: redistribution in binary form
is not allowed. This port is a personal project; do not publish binaries that
include those files.

## Open observations (2026-09-11 night)

- **"Stretched model" at item hits: settled 2026-09-11, it is not a port bug.**
  The large tan/beige polygons that appear around the rider for a fraction of a
  second are the game's own *pickup shards*: when a rider rides through an item
  panel, `updateRacePickup` calls `spawnPickupShardParticle` eight times and
  `renderPickupShardParticle`
  (src/race/course/race_course_props_and_pickups.c:1593) draws eight of the
  panel-box's own 20x20 faces (`gRacePickupTopVertices`, quads of +/-10 units)
  flying outward at 4 units per frame for `timer = 0xA` = 10 game frames. The
  camera sits about 50 units behind the rider, so a shard aimed at the camera
  reaches ~25 units from the eye and covers about a fifth of the screen width
  before it expires. The texture is asset 0x22 of `gAssetHandles[0x1C]`
  (32x32 CI4 at 0x80237f40 with the 16-entry TLUT at 0x802342a0): a bevelled
  cream panel, 84% palette index 6 = RGBA5551 0xff71 = (255,238,197), shaded by
  the face's vertex colour 0xcd or 0x9b, giving exactly the (205,192,158) and
  (155,145,120) measured in the frame dumps.

  Verified, not assumed:
  * *Which draw*: a point-in-triangle test over `--dumptris` for the tan pixels
    of the dumped frame lands on `vtx=0x800d9398` = `gRacePickupTopVertices+0x40`
    with `tex=32x32@0x80237f40`, i.e. `renderPickupShardParticle`.
  * *The quads are not distorted*: reconstructing eye space from the dumped
    screen coordinates and w of one shard gives two adjacent edges of equal
    length, 20 units, with dZ/dw = 2 (the game's projection), so the fixed-point
    `G_MTX` decode and the vertex transform are right.
  * *The texture decode is right*: `--peek 80237f40` / `--peek 802342a0` rebuild
    a clean bevelled panel and a wood-crate palette (brown ramp, reds, blues).
  * *Count and lifetime are right*: eight quads, present for exactly ten
    presented frames, matching `timer = 0xA` at 30 Hz.
  * **The reference shows the same thing.** mupen64plus on the retail ROM, its
    title demo race screenshotted every ten VI frames, produces frames with the
    same tan panels across the rider right after the flower-cloud item -- the
    same shapes, the same colour, the same moment as the port's
    `g4-shots/st-nm-05.png`. Kept as `g4-shots/ref-n64-shards-1.png` and
    `-2.png` next to the port's `g4-shots/port-shards-task17173.png`.

  So the port is faithful here and nothing was changed in the renderer. What the
  effect really looks like is the item box bursting open past the camera; it
  reads as a stuck, stretched model because the panels are big, flat and
  untextured-looking.

- **`regress` drifted because trials were not isolated. Settled 2026-09-11.**
  Two things outside the script reached the game and moved a race:

  1. *The Controller Pak.* The game writes it during the menu walk (the file's
     mtime moves mid-run), so the first trial after any pak change took a
     different path from the ones after it, and the golden rows -- recorded
     against a pak that later changed -- went stale (course 0 at 18114 against
     a recorded 18066).
  2. *The gamepad.* An open pad makes `osMotorInit` report a Rumble Pak, which
     changes the menus' pak prompts, and the Xbox One pad over IOKit is claimed
     only if the previous process has finished letting go of it. So the *same*
     movie replayed twice in a row landed on two different races: 20244 frames
     with `sbk: Xbox One controller 045e:02ea via IOKit`, 21132 with
     `sbk: no gamepad; keyboard only`. The fingerprints diverge at retrace 3503
     on one extra DMA, deep in the menus, well before the race starts.

  The fix is two switches, `--nopak` (os_pfs.c never opens or writes the image;
  `osPfsInitPak` sees PFS_ERR_NOPACK) and `--nopad` (input_sdl.c skips gamepad
  init entirely), passed by `nightmare_search.py` for every trial. With them,
  three runs of one trial print identical `--hashframe` fingerprints and
  `regress` passes 8/8 twice over. A pak-backed run (the campaign) is
  deterministic only while the pak's contents do not change, which is exactly
  what a saving session cannot promise -- so goldens are cut without one.

  The re-measured book: 9 = 3/2/64, 0 = 1/2/0, 1 = 3/2/**32**, 2 = 1/2/**64**,
  3 = 1/2/64, 4 = 3/1/96, 5 = 3/1/**64**, 6 = 4/1/64. Courses 1, 2 and 5 lost
  at the pak-era boosts once the conditions were clean. The CSV keeps a `mode`
  column (`pak` / `nopak`) and `best_row` prefers the reproducible rows.

- Debug tooling added while chasing this: `--bigtri N` logs every on-screen
  triangle whose screen area exceeds N pixels together with the modelview
  matrix, the address of the `G_MTX` it came from and the `G_VTX` source, and
  `--dumptris` lines now carry the same `mtx=`/`vtx=`/`gm=` fields. Matching a
  `vtx=` address against `build/snowboardkids.map` names the drawing function
  in one step; that is what identified the shards.
- Gamepad: SDL now has the IOKit joystick driver (build-tiger-joy), but the
  controller the user plugged in did not appear on either USB bus (`ioreg -p
  IOUSB` shows only the keyboard hub, keyboard and mouse). Check the cable or
  port; the port hot-plugs and logs the pad's name and GUID when it appears.
