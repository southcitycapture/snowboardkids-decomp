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
   Big Snowman won, 20,200G in the purse, six races run, none of it saved.

7. **The campaign saves** (2026-09-11). `--autonav`
   (`port/src/debug/menu_nav.c`) replaces the menu monkey with a navigator that
   knows which screen is up: it walks `gActiveGameTaskList` and names each
   task's callback with `dladdr()` (`--menutrace` prints them), so it can
   branch on `updateRaceSplitscreenSelectMenu`,
   `updateControllerPakRaceRecordSaveFlow` and the rest by name.

   What the map turned out to be, from `race_flow.c`: the post-race **Game
   Menu** is `updateRaceSplitscreenSelectMenu` and `gRaceSplitscreenMode`
   decides where its A press goes -- 0/2 the next race, 1 the race type menu,
   3 the shop, **4 EXIT / SAVE**. So the navigator parks that variable instead
   of counting D-pad presses, exactly as `--trial` parks the course cursor.

   Three things had to be *parked* rather than pressed, because a queued A
   lands on a prompt the same frame it appears and a stick-up queued behind it
   is a frame late -- each of these defaults to the answer that backs out:
   `gControllerPakMenuState.confirmChoice = 0` (ARE YOU SURE -> YES),
   `gMenuChoicePromptState[0] = 3` (DATA SAVE -> SAVE), and the same 3 on the
   startup save menu (USE THIS SAVE; 4 is a new game, which is why every
   restarted campaign began at 0G even with a saved pak). Before that fix the
   save flow ran end to end and wrote nothing: `--status` read `savemoney` out
   of RAM while the pak's note table stayed all zeros. The check that it is
   real is the pak itself -- `NSKE` plus the game's note name at 0x300 -- and a
   restart coming back up at money 15470 with the same win flags.

   **The shop is the board shop.** The Game Menu's third entry leads to
   `initCourseSelectMenu` -> `updateCourseSelectModeMenu`, whose three rows are
   BOARD (FREE STYLE / ALL AROUND / ALPINE), PAINT and RETURN; screenshots
   `g4-shots/shop-stuck.png` and `shop3.png`. An unaimed run sat in it for
   100,000 retraces, which is what the navigator's stuck-screen watchdog (B out
   after 3600 retraces with no change) now prevents. Where
   `gCourseUnlockPrices` is actually spent is still open -- most likely the
   pre-race course list itself. It does not hold the campaign up: the port
   raises `gHighestUnlockedCourse`, so every course is offered and a win on a
   course still for sale counts. Progression level 1 (wins on 0-4 and 9) was
   reached without buying anything. `--shop` is off by default.

   Result of the first `--autonav` session: **the campaign finished the game.**
   Courses 9, 0, 1, 2, 3, 4, 5 and 6 all won first place on the book's setups,
   a save after every race, the ending credits rolling on the G4
   (`g4-shots/ending-credits.png`), and a final save of 56,820G at progression
   level 3 that reloads on the next start. It took several restarts (a new
   binary each time) and every one resumed from the pak.

   Still open: a trial win does not always transfer. Sunset Rock (course 1) was
   won by char=3 board=2 at boost 0 from a fresh save but came 3rd repeatedly
   in the campaign; re-measured under `--nopak --nopad` it needs boost 32, and
   at that setting the campaign wins it. The book is worth re-measuring from a
   played save rather than a fresh one.

8. **A front end** (2026-09-11). `port/src/settings.c` keeps
   `settings.txt` next to the Controller Pak, `port/src/ui/` is an
   immediate-mode GL 1.3 UI on one generated bitmap font, and between them
   they add a launcher, an in-game options overlay, `n64` / `2x` resolution
   modes and scanline / grille / smooth filters. See README "Launcher and
   options" for the user-facing description; the design facts worth keeping:

   * **The settings file must not reach a scripted run.** `--play`,
     `--record`, `--headless` and `--nolauncher` set `sbk_settings_scripted`:
     the file is never read, the defaults are used and no post-processing is
     applied. Otherwise a golden replay recorded on one afternoon would
     depend on whatever the launcher last wrote, which is exactly the class
     of drift `--nopak --nopad` was introduced to remove. Explicit
     `--mode=` / `--resolution=` / `--filter=` still win, so the filters can
     be screenshotted over a scripted race; `nightmare_search.py` passes
     none of them.
   * **A 3x1 aperture-grille mask does not work on a Radeon 9000.** NPOT
     textures are unsupported, so a 3x1 mask is an *incomplete* texture:
     texturing switches off silently, the quad is drawn white and
     `GL_DST_COLOR`/`GL_ZERO` multiplies the frame by 1. The first version
     looked like it did nothing and the screenshot's pixels proved it. The
     mask is now 4x1 (R, G, B, neutral), which is POT and still lands one
     texel per output pixel.
   * **Moire is a mapping question, not a tuning one.** Both masks are drawn
     with `s = out_w / period` and `t = out_h / period` over the output
     rectangle, so one texel always covers exactly one output pixel whatever
     the window size; the period never has to divide the width.
   * **gfx_pc did not need touching for the resolution modes.** It asks the
     window layer for the framebuffer size, so `gfx_sdl_get_dimensions()`
     answering 320x240 moves viewport, scissor and clear with it; the GL
     backend copies that corner of the window into a POT texture at
     `finish_render` and draws it into the output rectangle.
   * **The UI saves and restores GL state with `glPushAttrib`/`glPopAttrib`
     rather than invalidating gfx_pc's cache.** `GL_TEXTURE_BIT` covers all
     six units, so the combiner chains come back exactly as the game left
     them and the next frame's cached shader/texture state is still true.
   * **Synthetic key events do not reach the fullscreen window over SSH.**
     `osascript` reports success and nothing arrives, so the launcher and the
     overlay are driven by `--uiscript SEQ` for testing and screenshots.
   * **An unattended mode must not sit on a menu.** `--autoplay`, `--soak`
     and `--autonav` turn the launcher off the way `--nolauncher` does.

   Cost measured on the G4 during a race (`--perf`): `finish_render` 0.3-0.4
   ms for the copy-and-filter pass, the frame still 60.0 Hz at 32% CPU.

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

## The rendering audit, 2026-09-12

Every screen the first game has was put next to the real game's frame. The
verdict is that **the first game's renderer matches**: logos, the title demo
race, the title screen, mode select, the Controller Pak prompts, rider
select, the Game Menu, the course list and detail, board select, the race
itself and the finish all place the same pixels, and the only differences
found were one-frame offsets in menu wipes and in the START banner.

How the references were made, because the method is reusable:

* `mupen64plus-ui-console` has no movie playback, so the audit built a
  ~60-line **input plugin that replays a `.m64`** -- `GetKeys` hands out one
  sample per controller read, `SBK_M64` names the file. `port/scripts/golden/
  *.m64` are real Mupen64 TAS files, so the same movie drives the native G4
  build and the emulator on the Mac. `--testshots f1,f2,...` writes the
  emulator's frame at the VI count the port's new `--shotat` dumps, and the
  two stills can be subtracted.
* The one thing an input replay cannot reproduce is an `--autoplay` race:
  autoplay flips `gRacePlayers[0].isCpu`, which is game state, not input.
  A pure-input replay of any golden therefore lands on Rookie Mt. (the course
  the menu starts on) and sits at the start line -- which is still a perfectly
  good, frame-exact comparison of every menu and of that course's scenery.
  For the courses the campaign reaches, a 480p longplay was used instead:
  good for "is this element present and is the geometry right", not for
  colour.
* A text script in `--play` syntax can be compiled to a `.m64` with the same
  semantics (`wait` / `press` / `hold` / `release` / `stick`), so new
  references can be authored for a screen without recording on the G4 first.

Contact sheet: `sbk1-audit.png` in the session's scratchpad.

Two changes carried over from the sequel's half of the audit, neither of
which alters a pixel here -- this game never turns hardware lighting on
(every `gsSP*GeometryMode` in `src/` *clears* `G_LIGHTING`; models are drawn
with vertex colours) and `regress` passes 8/8 after both:

* `MAX_LIGHTS` is 7 rather than sm64-port's 2, and `G_MW_LIGHTCOL`
  (`gSPLightColor`) is implemented. In the sequel a four-light viewport
  against `MAX_LIGHTS 2` dropped the ambient and read past the light arrays,
  which put every rider at about 0.4x brightness.
* An I4/I8 texel is `(I,I,I,I)`, not `(I,I,I,1)`: `import_texture_i4`/`i8`
  now take alpha from the intensity. The sequel's rider shadow is a 16x16 I4
  circle drawn with `G_CC_MODULATEIA` and was rendering as a grey square.

Three more carried over on 2026-09-12 night, again with no pixel changed here
(`regress` 8/8 after them, and the first game never enables lighting at all):

* **The display-list dumper no longer walks off RDRAM.** `--dumpdl` on one of
  the sequel's race tasks died with SIGSEGV, and the same `dump_addr_ok` is in
  this tree.  It vetted the **unresolved** command word: anything below
  `0x10000000` was assumed segmented and let through whatever the segment base
  happened to hold, and anything at or above it was dereferenced as a native
  pointer.  A dump follows branches the RSP never takes, so it is handed
  nonsense by design.  It now resolves first and then checks -- `size` bytes
  inside the emulated RDRAM, 8-byte aligned as the RSP's own DMA requires, the
  walk of a nested list clamped to the room left before the end of memory --
  and the dumper keeps its own copy of the segment table rather than writing
  into `gfx_pc`'s, where a skipped branch could leave a base behind for the
  real run of the same list.  `--dumpdlat R[:N]` came with it: arm the dump at
  a *retrace* and take the next N gfx tasks, because a gfx task count from boot
  is no way to find the list that drew one moment of a race.
* **`gSPPopMatrix` has to recalculate the light directions.**
  `calculate_normal_dir` caches each light's direction in the modelview's
  space and `rsp.lights_changed` says when the cache is stale; `gSPMatrix` set
  it and `gSPPopMatrix` did not, so the first lit vertex after a pop was shaded
  with the pushed object's light directions.  Moot in this game, which never
  turns `G_LIGHTING` on, but it is the same shared `gfx_pc.c`.
* **The reference tooling is in the tree**: `port/tools/input_m64.c`,
  `port/tools/mkm64.py` and `port/docs/reference-frames.md`.  One trap the
  audit did not write down at the time -- `--input <path>` on its own makes
  mupen64plus look for *every* plugin next to that path, so it finds no video
  plugin and dies inside `osd_init`; pass `--plugindir` as well.  (And the
  emulator stops at the **last** `--testshots` entry without capturing it, so
  the list wants a sentinel frame on the end.)

The sequel's one open rendering item, "the near rider is black under the
overhead camera", closed on 2026-09-12 as **not a bug**: the attract demo's
frame-exact pair (port retrace 1444, emulator VI 2000) shows the same near-black
histogram on both sides.  The rider's hair really is an RGBA5551 texel whose
components are 1.

Open, and not a renderer question: the `SETTIMG outside RDRAM` guard fires
eight times a run at `0x80cc0100`, above the 4 MB window. It is the
`renderCourseTextureMarkers` class of garbage descriptor the guard was
written for -- hardware would load it harmlessly and never show it -- and the
rider-select and course-select screens it fires on are pixel-identical to the
emulator, so nothing visible is being skipped.

## The Enhanced-mode distance haze, 2026-09-13

`--drawdistance N` pushes the race camera's far plane out by N. What it
actually buys you, and what it costs, took three measurements to find out, and
none of them was what the feature description assumed.

**1. The extra geometry is not spread over the extra range.** A three-way
pixel diff -- `--drawdistance 1`, `4` without the haze, `4` with it, all three
frame-exact off the same deterministic trial (`--nopad --eeprom scratch
--trial level=0,quit=1 --shotat R`, which reproduce to the pixel) -- puts the
whole difference in one band: 23,000 pixels of a 640x480 frame, a row of trees
and a hut on a ridge, between 3800 and about 5000 units. A haze ramp that only
reached full strength at the new far plane (15,200) was 6% thick there. The
ramp has to be anchored to the far plane the *unmodified* game clipped at,
not to the extended one.

**2. Most of that band is not new geometry at all.** It is the same geometry
with the *game's own fog* taken off it. `gSPFogPosition` takes normalised
depth, not world units, so a fog band written as 0x3E3..0x3E7 (995..999 per
mille) stretches with the far plane: on the N64 it ran about 1950 to 3190
world units and was total by the time anything clipped, and at
`--drawdistance 4` the same two numbers mean 3170 to 8640. Pushing the far
plane out does not so much draw more as *un-fog* what was already being drawn.
That is the honest description of what `--drawdistance` does to this game.

**3. So the haze has to beat the game's fog, not merely add to it.** The two
are the same colour -- both come from the course's own atmosphere colour -- so
a ramp thinner than the game's changes not one pixel, and every early version
of this measured exactly zero pixels different. `--hazeflat`, which paints
every hazed vertex flat instead of ramped, is what settled it: with the colour
swapped for magenta the band it covers is unmistakable, and it is exactly the
ground past the N64's horizon. The shipped ramp starts at 0.85x the old far
plane and is total at 1.5x, which passes the game's own fog at about 4000
units.

### What it is made of

No post-process and no shader. F3DEX already carries a per-vertex fog factor,
`gfx_pc.c` already computes one for `G_FOG` geometry and `gfx_gl13.c` already
hands it to `GL_FOG` as a per-vertex fog coordinate (`GL_EXT_fog_coord`,
`GL_LINEAR`, start 0 end 1), which the Radeon 9000 does in fixed function.
`port/src/gfx/haze.c` fills in that same slot; where the game has a factor of
its own the thicker of the two wins, per vertex, so the haze can only ever add.

* **Distance** is the vertex's clip-space *w* divided by the length of the
  projection's w column. The game MULs the view matrices into the same
  `G_MTX_PROJECTION` slot, but the view part is rigid, so that column's length
  is exactly the scale `guPerspective` was given -- 0.5 in the first game, 1.0
  in the sequel -- and `w / scale` is the eye distance in the units the far
  plane is written in. No estimate, and no per-game constant.
* **Which viewport** comes from `gSPPerspNormalize`, which sm64-port ignores
  as an RSP precision register but which is `2*65536/(near+far)` and therefore
  names the far plane on its own. The race camera and every other pass in the
  frame carry different integers, so the sky, the HUD and the menus are left
  alone without a heuristic. `--hazedbg` prints the whole histogram.
* **Colour** is the game's own per-course atmosphere colour, so there is no
  table in the port and no sampling of the sky.
* **Cost** on the real G4 at `--drawdistance 4`, 500 triangles a frame:
  gfx 3.5 ms against 3.6 ms, 48-54 draw calls either way, 60.0 Hz and 44-46%
  CPU in both. Under 0.1 ms, which is inside the noise of the readout.
* **Off** in Original mode, in a scripted run and in a golden replay, the same
  way the filters are, so `regress` still replays bit-identical.

### Where the first game differs from the sequel

The sequel keeps a per-viewport fog colour in `LevelConfig.environmentColors`
and hands it to `setViewportFogById` for every race viewport. The first game
has no per-viewport fog at all: one global `gSPFogPosition(gFadeTimer, 1000)`
a frame, whose colour is the per-course `gFadeColorRed/Green/Blue` that
`setBootFadeColor` / `setTitleFadeColor` writes at the end of each case of
`initRaceCourseSceneTasks`. That is the colour the haze uses. Silver Mountain
is the one course with a real fog of its own (white, a 950 start and a 1000
far plane) and it is also the one course `--drawdistance` does not touch, so
the haze's perspNorm test leaves it alone by construction.

## The four Enhanced rendering options, 2026-09-13

Four options next to `--drawdistance` and the haze, on the same terms: a
`settings.txt` key, a flag, a launcher row and the same row in the overlay,
off in Original, forced off in a scripted or golden run, passed across the
cross-launch.  `port/README.md` is the user-facing description; what follows
is what had to be measured, and what the card turned out to allow.

### What the Radeon 9000 actually offers (`--glinfo`)

The extension list was worth printing before designing anything:

    ATI Radeon 9000 OpenGL Engine / 1.3 ATI-1.5.28
    GL_ARB_multisample  GL_ARB_imaging  GL_EXT_blend_color
    GL_ARB_texture_env_combine  GL_ATI_texture_env_combine3  crossbar
    GL_ARB_vertex_program  GL_ATI_text_fragment_shader
    (no ARB_fragment_program, no FBO, no NPOT)

Two of those decided two of the options.  `GL_ARB_multisample` means
anti-aliasing is real and not an approximation.  `GL_ARB_imaging` /
`GL_EXT_blend_color` mean a draw can be faded with a constant alpha without
the combiner knowing anything about it, which is what makes the far-object
fade-in work for every one of the game's combiner chains rather than the
subset that happens to multiply by shade alpha.

`GL_ATI_text_fragment_shader` is the one surprise: the card is not quite
"fixed function only".  It is not usable here, though -- see the three-point
filter below.

### Far-object fade-in: correct, and measurably almost inert in this game

The mechanism is in `port/src/gfx/haze.c` next to the haze, because it needs
the same two answers: which projection is the race camera's, and how to turn
a clip-space *w* into world units.  Two things are new:

* `sbk_fadein_note_cull()` in `patches.txt`, wrapping the cull constant the
  way `sbk_haze_note_far` wraps the far plane.  The port never keeps a copy of
  a game constant it can be handed.
* `rsp.obj_dist = MP[3][3] * w_to_dist` in `gfx_sp_matrix`: the object's own
  origin, once per `G_MTX`.  A prop then fades as one object instead of
  across its own depth, which is what a per-vertex ramp would have done.

* `sbk_fadein_note_object()`, also in `patches.txt`, on
  `allocFixedTransformMatrix` -- the game's own builder for a prop's, an
  effect's or a projectile's transform.  `haze.c` keeps the frame's pointers
  in a 1,024-entry table and a draw fades only when the modelview it loaded is
  one of them.

That last one is not belt and braces, it is the whole discrimination, and the
sequel is what proved it: keyed on distance alone (plus the guard that all
three of a triangle's vertices must be past the start of the band) the sequel
faded 23,000 of 42,000 triangles a second to alpha 0, because its terrain is
drawn in chunks with their own far-away origins and a distant chunk is
arithmetically the same thing as a distant prop.  The vertex guard stays as a
second line of defence.  The table is cleared once the frame's display list
has been walked, because the matrices come out of a per-frame scratch
allocator.

Then the measurement, which is the part worth keeping.  `--fadedbg` prints the
cull range, the band and how many draws fade:

    sbk-fade: r9960 cull 2976 fade 2559..2976 draws 11820 faded 448 minalpha 0.41
    sbk-fade: r10020 cull 11904 fade 10237..11904 draws 24539 faded 0 minalpha 1.00

* At `--drawdistance` 2 and 4 the cull box is 5,952 and 11,904 units, because
  `patches.txt` scales it along with the far plane, and **no course in this
  game reaches it**: `faded 0` for the whole race.
* At `--drawdistance 1` it does bite -- 300-500 draws a second are inside the
  band -- but the box is square in XZ while the frustum is not, so most of
  those are beside or behind the camera.  Frame-exact diffs on the retraces
  where one is actually on screen, approaching the JUMP banner on Rookie
  Mountain, come out at **16, 11, 5, 1 and 0 pixels** over five consecutive
  retraces.  The reason it is that small is the one the haze ran into: the
  game's own fog is 83% thick at 2,976 units, so an object arriving there is
  already nearly the colour of the air.

So the honest description is that the first game does not have a visible
pop-in at its cull boundary, and the option is kept because it is free, it is
correct, and the *sequel's* cull box is a cube of 4,074 units that
`--drawdistance` does not scale at all -- which is where the same code has
something to do.

### Widescreen: the aspect correction was already most of the answer

`gfx_adjust_x_for_aspect_ratio` multiplies every x by `(4/3) / render_aspect`,
for 2D and 3D alike.  Give it a 16:9 render target and it does exactly the
right thing on its own: the 4:3 frustum lands on the middle three quarters of
the frame (nothing stretched, vertical FOV untouched) and the wider viewport
is filled by geometry the race camera's own projection was already producing
and the 4:3 frame was cutting off.  2D passes go through the same correction
and therefore stay a centred 4:3 box, which is what the HUD and the menus
want.

So the port-side work was three small things: an aspect for the output
rectangle in `gfx_sdl_gl13.c`, render sizes that follow it (427x240 and
854x480, still inside the 512/1024 copy textures), and a scissor clamp that
keeps a *non-race* frame inside its 4:3 box so a menu's 3D backdrop is not
asked for scenery nobody drew.

**The clamp has to be decided per frame, not per draw.**  Keyed on each draw's
projection -- which is how the haze works, and the obvious thing to reuse --
the sky, the HUD and the item overlays were clamped inside a race, because
they hang off their own viewports with their own far planes.  The result was a
race with black wedges in the top corners where the sky should have been.  A draw under the race projection now
only sets a flag, and `gfx_run` uses what the *previous* frame turned out to
be; the frame of lag at the start of a race is behind the game's own fade-in.

### Anti-aliasing: the driver does have multisample formats

Asked before `SDL_CreateWindow` and then verified against the framebuffer
rather than against SDL's own attribute:

    sbk: multisample: asked 4, SDL buffers 1 samples 4
    gfx_gl13: multisample buffers 1 samples 4 -> anti-aliasing ON

A driver with no format of that size fails the window creation outright
instead of downgrading, so the port retries once without multisampling and
prints why.  `glCopyTexSubImage2D` resolves as it copies, so the `n64` and
`2x` resolution modes are multisampled at their own size and then scaled.  The
count is a pixel-format attribute, so the overlay row writes the file and the
next start takes it; changing it live would mean destroying the context under
a running game.

### Three-point filtering: not possible here, and why

The RDP samples three texels of the texel quad, not four, which is the
characteristic diagonal look.  The fixed-function recipe would be: three
texture units sampling the same texture one texel apart (a texture matrix
each), a fourth unit holding a weight texture addressed by the *fractional*
texel coordinate -- which a scaled texture coordinate and `GL_REPEAT` do give
you for free -- and then `T0*w.r + T1*w.g + T2*w.b`.

The last step is the one that cannot be written.  A texture environment stage
multiplies by a whole RGB vector and there is no channel swizzle, so three
weights carried in one texel cannot be applied to three separate samples; and
there are not three spare units anyway, since the game's own combiner chains
already use up to six.  `GL_ATI_text_fragment_shader` could express the
arithmetic, but using it would mean re-implementing the N64 colour combiner in
that shader language for every combiner the game uses -- a different renderer,
not a filter.

So `texfilter` ships `rdp` (each tile as the game asked, which is what the
console did and the port's default), `point` and `bilinear`, and the README
says why there is no fourth.  The override is applied in both places the
filter shows: the GL sampler, and the half-texel offset bilinear needs.

### Cost, measured the same way the haze was

The course-9 golden replayed at real time with `--perf`, Enhanced, 640x480
windowed, averaged over retraces 9,200-11,100 in every run so that the same
scenery is being compared (an unaligned window is not a comparison: the
triangle count in this race swings between 120 and 500 a frame):

    run                    gfx ms   GL ms   CPU   Hz     tris
    all four off             2.44    0.39   35%   60.0    297
    fade-in                  2.48    0.39   36%   60.0    305
    anti-aliasing 2x         2.47    0.39   35%   60.0    304
    anti-aliasing 4x         2.48    0.38   35%   60.0    302
    texfilter=point          2.46    0.38   35%   60.0    302
    widescreen 16:9          2.81    0.41   38%   60.0    369

Anti-aliasing costs the CPU nothing, which is what it should cost: the
multisampling is the Radeon's work and the port builds the same display list.
Widescreen costs a quarter more triangles and 0.37 ms, which is the field of
view and not an overhead.
