/* The two compression schemes the games keep their assets in.
 *
 * Neither is MIO0 or Yay0: the first game uses a Huffman tree followed by an
 * LZ77-ish back-reference pass (the tree is rebuilt from a symbol-weight table
 * carried in front of the bitstream), and the sequel uses a plain two-byte
 * token LZ its own tools call "Sno".  Both are small enough to carry in the
 * port, and carrying them here rather than calling the game's own decompressor
 * means the launcher can read the *other* game's ROM -- which is the whole
 * point, since either bundle draws both boxes.
 *
 * Ported from src/engine/asset_manager.c (decompressHuffmanAssetPayload,
 * insertHuffmanQueueNode) and tools/sno.py in the two decompilations. */
#ifndef SBK_ROM_CODEC_H
#define SBK_ROM_CODEC_H

/* The first game: `src` points at the compressed asset as it sits in the ROM
 * -- a big-endian decompressed size, a flags byte, then the payload.  Writes
 * exactly the size the header names into `out`, which must be that big, and
 * returns the number of bytes written (0 on a malformed asset). */
unsigned long sbk_decompress_huffman(const unsigned char *src, unsigned long src_len,
                                     unsigned char *out, unsigned long out_cap);

/* The size the first game's asset header names, without decompressing. */
unsigned long sbk_huffman_size(const unsigned char *src, unsigned long src_len);

/* The sequel: no header at all, so the caller supplies the size (the constant
 * the game passes to loadCompressedData).  Returns bytes written. */
unsigned long sbk_decompress_sno(const unsigned char *src, unsigned long src_len,
                                 unsigned char *out, unsigned long out_size);

#endif
