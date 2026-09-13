Snowboard Kids 1+2 PowerPC Edition
==================================
Version 1.0


WHAT THIS IS

Two native PowerPC ports of Snowboard Kids and Snowboard Kids 2, built from
the two decompilation projects and running as real Mac OS X applications on a
Power Mac G4. This is not an emulator: the games' own C code is compiled for
the G4's processor, and the N64's display lists and audio command lists are
interpreted into OpenGL and CoreAudio as the game produces them.

Requires Mac OS X 10.5 (Leopard) or later, PowerPC, and an OpenGL 1.3 card
(a Radeon 9000 is enough). SDL2 is linked in; there is nothing else to
install.

Drag both applications into your Applications folder.


YOU BRING THE CARTRIDGE DUMPS

No ROM is included. Put your own dumps of the two cartridges here:

    ~/Library/Application Support/SnowboardKids/ROMs/

Either application creates that folder the first time you open it, and the
launcher has an "Open ROM Folder" button that takes you straight there in the
Finder. Any filename will do, and any of the three byte orders (.z64, .v64,
.n64): each file is read, put in big-endian order and identified by its SHA-1,
so a dump that is the wrong region or a damaged file is named as such rather
than silently ignored. Drop the files in, close the panel, and the game lights
up without restarting anything.

Both applications read the same folder, and it survives replacing either one.


THE LAUNCHER

Whichever application you open, the launcher shows both games; picking the
other one hands the session over to its application. Then:

    Mode      ORIGINAL   the game as the N64 drew it: 320x240, 4:3,
                         the cartridge's own draw distance and fog.
              ENHANCED   native resolution, four times the draw distance
                         with a distance haze in the course's own air
                         colour, a fade-in on far scenery, and 2x
                         multisampling. This is the default.
              CUSTOM     whatever you set on the Options page.

    Options   draw distance, resolution, scanline/aperture-grille filters,
              4:3 or 16:9, texture filtering, multisampling, fullscreen,
              vsync, volume.

A first run comes up fullscreen in Enhanced.


CONTROLS

Keyboard

    Arrow keys / WASD   analogue stick
    Z                   A
    X                   B
    C or Shift          Z trigger
    Q / E               L / R
    T G F H             C-up / C-down / C-left / C-right
    I K J L             C-up / C-down / C-left / C-right (right hand)
    Return              Start
    F1                  the in-game options overlay
    Esc                 quit
    Cmd+Q               quit

In the launcher and the overlay: arrows move, Return or Space or Z chooses,
Esc or X or Backspace goes back.

Game pad (any SDL-recognised pad; an Xbox One pad over USB is driven directly)

    Left stick          analogue stick
    A                   A
    B or X              B
    Y                   C-down (the item button)
    Left/right trigger  Z trigger
    LB / RB             L / R
    D-pad               D-pad
    Right stick         the C buttons
    Start / Menu        Start
    Back / View         the in-game options overlay

Rumble works on a pad that has it: the Rumble Pak the games ask for is wired
to the pad's motors.


THE OVERLAY

F1 (or Back/View on a pad) opens the options list over the running game, which
is paused while it is up. Change anything, press Esc or pick Back, and the
game carries on with the new settings. Quit from the overlay saves first.


WHERE YOUR THINGS LIVE

    ~/Library/Application Support/SnowboardKids/
        ROMs/                 your cartridge dumps
        settings.txt          the launcher's settings, shared by both games
        controller-pak-1.mpk  the Controller Pak both games save to

Snowboard Kids 2 also uses the cartridge's EEPROM save, which is kept
alongside the Controller Pak. Deleting the folder loses your save files, so
that is the folder to back up.


LICENCE AND CREDITS

The games are the property of their rightsholders. Nothing in this package
contains any of their data: you supply your own cartridge dumps, and the two
source repositories have never contained a ROM.

The display-list interpreter comes from the sm64-port project and is used
under that project's source-only licence; its own licence text ships with the
source (port/src/gfx/LICENSE-fast3d.txt). The decompilations are the
snowboardkids-decomp and snowboardkids2-decomp projects. SDL2 is under the
zlib licence.

This package is built for one person's own Power Mac and is not for
redistribution.
