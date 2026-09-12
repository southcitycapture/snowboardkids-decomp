# Frame-exact reference frames

How to put a frame the port drew next to the frame the real game draws from the
same input, and subtract them. This is what the 2026-09-12 rendering audit used
and what settled every "is this colour right?" question in it.

The idea is simple: the port and mupen64plus both take a Mupen64 `.m64` movie,
so the same controller stream drives both. The port then dumps the frame it
presented at a named retrace and the emulator dumps the frame it presented at a
named VI count, and the two PNGs can be differenced pixel for pixel.

## The pieces

* `port/tools/mkm64.py` — compiles a text input script (the same syntax
  `--play` reads, `port/scripts/*.txt`) into a `.m64`. One `.m64` drives both
  sides, so a new reference can be written here rather than recorded on the G4.
* `port/tools/input_m64.c` — a ~60-line mupen64plus **input plugin** that hands
  out one `.m64` sample per controller read. `mupen64plus-ui-console` has no
  movie playback of its own; this is the whole of what it was missing. The
  movie is named by `$SBK_M64`.
* The port's `--play FILE` (replay), `--shotat R1,R2,...` (write the frame
  presented at each of those retraces to `/tmp/sbk-shot-<retrace>.ppm`) and
  `--dumptris` / `--dumpdlat R[:N]` (the per-triangle and display-list dumps for
  when the picture is wrong and you need to know why).

## Making a reference

1. **Write the script.** Plain text, one command per line; `wait N`,
   `press BTN N`, `hold BTN`, `release BTN`, `stick X Y [N]`. `N` counts
   *controller reads*, not VI frames — both games poll once per game loop, so a
   read is about two VI frames while the game keeps 30 fps.

       # /tmp/ref.txt
       wait 3300
       press START 3
       wait 120
       press A 3
       wait 300

2. **Compile it.**

       python3 port/tools/mkm64.py /tmp/ref.txt /tmp/ref.m64

3. **Build the input plugin** (once; it needs the mupen64plus headers, which
   come with the `mupen64plus` Homebrew formula):

       cc -O2 -fPIC -shared -I/opt/homebrew/include \
          -o /tmp/mupen64plus-input-m64.dylib port/tools/input_m64.c

4. **Run the emulator** at the VI counts you want.  Two traps, both of which
   cost the audit an hour:

   * `--input <path>` alone makes the core look for *every* plugin next to
     that path, finds no video plugin, and dies in `osd_init` inside
     `glPixelStorei` with nothing useful on stderr.  Pass `--plugindir` as
     well so the other three are still found where Homebrew put them.
   * mupen64plus stops emulating when it reaches the **last** `--testshots`
     entry, and does not capture it.  Put a sentinel frame past the ones you
     actually want.

       SBK_M64=/tmp/ref.m64 mupen64plus \
         --windowed --resolution 640x480 --nosaveoptions \
         --plugindir /opt/homebrew/lib/mupen64plus \
         --input /tmp/mupen64plus-input-m64.dylib \
         --testshots 2000,2400,99999 "ROMs/<game>.z64"

   The PNGs land in `~/Library/Application Support/Mupen64Plus/screenshot/`,
   numbered in the order they were taken.  The "Unable to open config file"
   line the first run prints is harmless.

5. **Run the port** on the G4 with the same movie and the matching retraces:

       g4 ssh 'cat > /tmp/ref.m64' < /tmp/ref.m64
       g4 run --windowed --nolauncher --nopad --nopak \
              --play /tmp/ref.m64 --shotat 1444,1730
       g4 pull /tmp/sbk-shot-001444.ppm /tmp/port-1444.ppm

6. **Subtract.** Both are 640x480 RGB:

       python3 -c "
       from PIL import Image
       a=Image.open('/tmp/port-1444.ppm').convert('RGB')
       b=Image.open('/tmp/emu-2000.png').convert('RGB')
       print(sum(a.getpixel((x,y))!=b.getpixel((x,y))
                 for y in range(480) for x in range(640)))"

## The one thing to know about the retrace numbers

**The two sides do not agree on the absolute frame count.** The port's boot does
not spend the DMA time the console does, so it arrives everywhere earlier.  In
the sequel, whose numbers the audit wrote down, the port reaches the title at
retrace ~2460 where the emulator needs ~3210, and the attract demo lines up at
port 1444 / emulator 2000 (the other known pair is port 1560 / emulator 2160).
The first game's offsets are its own.  So the workflow is:
shoot a spread on both sides, find the pair that shows the same moment, and use
*that* pair from then on. Once found, a pair is stable — the same two numbers
came back identically across rebuilds.

The input itself is frame-exact; only the clock the two sides count on differs.

## What it cannot do

`--autoplay` is not controller input. Autoplay flips `gRacePlayers[0].isCpu`,
which is game state, so a recorded autoplay race (`port/scripts/golden/*.m64`)
replays only in the port, with the same flags it was recorded under.
For a race reference in the emulator the movie has to actually steer — or use
the attract demo, which needs no input at all and is where the audit took most
of its race-lighting references.

## Worked example: is the near rider black?

This one is from the sequel, but the method is the same in both games.  The
2026-09-12 audit left open "the near rider draws at RGB (1,1,1)..(7,6,8)
while the course is right". The attract demo settles it with no scripting at
all: port retrace 1444 and emulator VI 2000 are the same moment, Slash seen from
behind. The dark pixels in the rider's bounding box:

| | most common values |
|---|---|
| port | (5,5,5) (6,6,6) (2,2,2) (4,4,4) (10,10,10) |
| emulator | (5,5,5) (7,7,7) (6,6,6) (1,1,1) (8,8,8) |

Same histogram. The rider's hair and jacket really are that dark on hardware,
and the "black rider" was the four-light bug, already fixed.
