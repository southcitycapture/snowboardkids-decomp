# Snowboard Kids 1+2 PowerPC Edition — 1.0

13 September 2026. Two native PowerPC ports, shipped as one package for a
Quicksilver Power Mac G4 running Mac OS X 10.5 with a Radeon 9000.

Not an emulator: each game's own C code is compiled for the G4's processor and
runs there. What is interpreted is the pair of things that used to be
hardware — the N64's display lists (into fixed-function OpenGL 1.3) and its
audio command lists (into CoreAudio) — plus a libultra replacement for the
threads, the DMA, the Controller Pak and the EEPROM.

## The package

```
Snowboard Kids 1+2 PowerPC Edition/
    Snowboard Kids.app
    Snowboard Kids 2.app
    Read Me.txt
```

Both applications are self-contained: SDL2 is linked in statically, there is
nothing to install alongside them, and they are launched from the Finder like
any other Mac application. Each declares `LSMinimumSystemVersion` 10.5 — the
code is built against the 10.4u SDK, but the pad path reaches IOHIDManager,
which is 10.5 and later.

Built by `port/tools/make_package.sh` in the first game's repository, which
drives the sequel's repository next door.

## What works

| | Snowboard Kids | Snowboard Kids 2 |
| --- | --- | --- |
| Boot, title, attract demo | yes | yes |
| Every menu, character and course select | yes | yes |
| Racing, all courses | yes | yes |
| Story mode / overworld | n/a | yes |
| Music and sound effects | yes | yes |
| Controller Pak saves | yes | yes |
| Cartridge EEPROM save | n/a | yes |
| Game pad, including rumble | yes | yes |
| Fullscreen, 4:3 and 16:9 | yes | yes |
| Self-play campaign to the credits | yes | yes |

Both games have been played end to end on the real machine.

## Bring your own ROM

No ROM ships in either bundle. Both read one shared folder,

    ~/Library/Application Support/SnowboardKids/ROMs/

created on first run. Any filename, any of the three byte orders (.z64, .v64,
.n64): each file is read, put in big-endian order and identified by SHA-1
against the known-good USA dump of each game, so a wrong-region cartridge or a
damaged file is named as such rather than quietly ignored. The hash is cached
by size and mtime, so the 8 MB and 16 MB SHA-1s are paid once, not per launch.

Saves and settings live beside the ROMs, in
`~/Library/Application Support/SnowboardKids/` (and `SnowboardKids2/` for the
sequel's EEPROM). That folder is the one to back up; replacing either
application does not touch it.

## The launcher and the Enhanced options

Either application's launcher offers both games — picking the other one hands
the session over to its bundle, carrying the chosen settings across — and
three modes:

- **Original**: 320x240, 4:3, the cartridge's own draw distance and fog.
- **Enhanced** (the default on a first run, fullscreen): the four options
  below, together.
- **Custom**: whatever the Options page is set to.

The four Enhanced options, each also a row on the Options page and on the
in-game overlay:

1. **Draw distance 2x.** Mostly the game's own fog taken back off: the far
   plane moves out and the cull box moves with it, which the original code did
   not do for distant course geometry (as opposed to distant props).
2. **Distance haze.** What the extra draw distance costs is a hard edge where
   scenery appears; the haze fades it into the colour of that course's own
   air, read from the course rather than guessed.
3. **Far fade-in.** Objects entering the extended range fade up over a short
   band instead of popping.
4. **2x anti-aliasing.** Multisampling, which the Radeon 9000 has.

Everything else — resolution (native, the N64's 320x240, 2x), scanline and
aperture-grille filters, texture filtering, 4:3 or 16:9, vsync, volume — is on
the Options page and can be changed mid-race from the overlay (F1, or Back/View
on a pad). The overlay pauses the game while it is open.

## For the curious: self-play

The ports can play themselves, which is how they were tested. None of it is
needed to play, and none of it is on by default.

```
--autoplay          drive a race with the game's own CPU rider logic
--soak              autoplay, forever, with no launcher
--autonav           drive the menus by name: race, save, shop
--plan C:CH:B:BO,.. play a course list: course, character, board, boots
--trial SPEC        make the CPU rider win a particular way
--racedbg           per-second race telemetry
--nightmare         the hardest settings the search found
--perf              frame cost readout
--trace             boot, thread, DMA and display-list logging
--paddbg            raw game-pad reports and button/axis numbers
--headless          no rendering; --turbo, no frame pacing
--play / --record   deterministic input movies (.m64)
--hashframe         per-frame fingerprints for run-to-run comparison
```

A run given `--play`, `--record`, `--headless` or `--nolauncher` is a
*scripted* run: it ignores the settings file, skips the launcher and turns off
every post-processing option, so a golden replay sees exactly the pixels it
was recorded against. Golden replays also want `--nopak --nopad`, so that no
Controller Pak state and no stray stick input can reach them.

The self-play campaign declares handicaps to itself when it needs to reach a
late course quickly. Those are the campaign's own affair: they are set by the
`--trial` and `--plan` flags and by nothing else, and a normal launch — from
the Finder, or from the launcher — never sets them. Playing the game by hand
is playing the retail game.

## Known differences from the cartridge

- **Rendering is a reimplementation, not a pixel-exact RDP.** The renderer was
  audited frame by frame against reference captures and matches them, but the
  combiner is fixed-function OpenGL 1.3 on a Radeon 9000: six texture units,
  `env_combine` and `combine3`, no fragment programs, no framebuffer objects,
  no non-power-of-two textures. A handful of RDP modes are approximated.
- **Enhanced mode is deliberately not the cartridge.** Original mode is there
  for when that matters.
- **Timing is not a cycle-accurate N64.** A retrace is delivered when the game
  has gone idle for the frame rather than on a wall clock, which is what makes
  input movies reproducible; the wall clock only paces delivery.
- **Audio is the real command lists through CoreAudio**, at the port's own
  buffer size rather than the N64's.
- **Two players share one keyboard**; only one USB pad is read.
- **The G4 is the target.** Nothing here is tuned for anything faster.

## Licence

The games belong to their rightsholders. Neither repository has ever contained
a ROM and neither bundle ships one: the player brings their own cartridge
dumps.

The display-list interpreter derives from the sm64-port project and is used
under that project's source-only licence — its text is in
`port/src/gfx/LICENSE-fast3d.txt`. The decompilations are
[snowboardkids-decomp](https://github.com/cdlewis/snowboardkids-decomp) and
[snowboardkids2-decomp](https://github.com/cdlewis/snowboardkids2-decomp).
SDL2 is zlib-licensed.

This package is built for one person's own Power Mac and is not for
redistribution.
