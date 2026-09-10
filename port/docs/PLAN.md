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
   Next: gamepad, then Rumble Pak.

## Build

    make ... (N64 build, once)        # map + linker script + assets
    port/build-ppc.sh -j8              # Docker cross-build -> port/build-ppc/snowboardkids
    g4 push / g4 run / g4 shot         # via isle-ppc-tools (needs a generic exe name)

## Licenses

`port/src/gfx/gfx_pc.c`, `gfx_cc.*` and the API headers come from sm64-port
(Emill, MaikelChan) under a source-only license: redistribution in binary form
is not allowed. This port is a personal project; do not publish binaries that
include those files.
