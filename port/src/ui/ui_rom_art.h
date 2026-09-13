/* The launcher's art, taken out of the games' own ROMs at run time.
 *
 * Nothing here ships in the repository: the font the launcher writes with and
 * the picture on the front of each box are read out of whichever cartridge
 * dump the player put in the ROM folder, decompressed and decoded on the spot.
 * That is licence-clean -- the bytes never leave the user's own file -- and it
 * matches the games by construction rather than by imitation.
 *
 * With no ROM for a game, the box front falls back to a generated one and the
 * text falls back to the port's own 5x7 bitmap font (ui_font.h). */
#ifndef SBK_UI_ROM_ART_H
#define SBK_UI_ROM_ART_H

/* Look at the scanned ROM slots and, for the first game that has one, install
 * that game's sprite font as the UI font.  Safe to call more than once; does
 * nothing when no ROM is present. */
void sbk_ui_rom_art_init(void);

/* The box front for registered game `i`, as a GL texture, or 0 if it could not
 * be built.  256x512, the top 256x358 carrying the picture (see
 * SBK_BOX_FRONT_*). */
unsigned int sbk_ui_rom_art_box_front(int i);

#define SBK_BOX_FRONT_TEX_W 256
#define SBK_BOX_FRONT_TEX_H 512
#define SBK_BOX_FRONT_W     256
#define SBK_BOX_FRONT_H     358   /* 256 * 1.4, the proportion of an N64 box */

#endif
