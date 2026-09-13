/* See rom_codec.h. */
#include <string.h>
#include "rom_codec.h"

/* --- the first game: Huffman, then LZ ------------------------------------ */

/* 256 leaves at most, so 255 internal nodes: 511 is the ceiling and 544 is a
 * round number above it.  The tree is rebuilt per asset, which costs nothing
 * at this size and keeps the decoder re-entrant-ish (one asset at a time). */
#define HUFF_MAX 544

struct HuffNode {
    short prev, next;   /* the weight-ordered queue: next -> head, prev -> tail */
    short weight;
    short left, right;
    short value;        /* -1 for an internal node */
};

struct Huff {
    struct HuffNode n[HUFF_MAX];
    int count;
    short head, tail;   /* head = heaviest, tail = lightest */
    int qcount;
};

static void q_insert(struct Huff *h, short i) {
    short cur = h->head;
    h->qcount++;
    if (cur == -1) {
        h->head = h->tail = i;
        h->n[i].next = -1;
        h->n[i].prev = -1;
        return;
    }
    /* walk from the heavy end toward the light one until something lighter */
    while (cur >= 0 && h->n[cur].weight >= h->n[i].weight) cur = h->n[cur].prev;
    if (cur == -1) {                       /* lighter than everything: new tail */
        short t = h->tail;
        h->n[t].prev = i;
        h->n[i].prev = -1;
        h->n[i].next = t;
        h->tail = i;
        return;
    }
    h->n[i].prev = cur;
    h->n[i].next = h->n[cur].next;
    h->n[cur].next = i;
    if (h->n[i].next == -1) h->head = i;
    else h->n[h->n[i].next].prev = i;
}

static void q_remove(struct Huff *h, short i) {
    short nx, pv;
    if (h->head == -1) return;
    h->qcount--;
    nx = h->n[i].next;
    pv = h->n[i].prev;
    if (nx != -1) h->n[nx].prev = pv;
    else h->head = pv;
    if (pv != -1) h->n[pv].next = nx;
    else h->tail = nx;
}

static unsigned long be32(const unsigned char *p) {
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

unsigned long sbk_huffman_size(const unsigned char *src, unsigned long src_len) {
    if (src == NULL || src_len < 8) return 0;
    return be32(src);
}

/* The bitstream reader the game's own loop spells inline: one bit at a time,
 * most significant first, walking down from the root until a leaf. */
struct Bits { const unsigned char *p; unsigned long n, byte; int bit; };

static int bits_get(struct Bits *b) {
    int v;
    if (b->bit == 8) { b->byte++; b->bit = 0; }
    if (b->byte >= b->n) return -1;
    v = (b->p[b->byte] & (1 << (7 - b->bit))) != 0;
    b->bit++;
    return v;
}

static int huff_symbol(struct Huff *h, struct Bits *b) {
    int i = h->count - 1;
    if (i < 0) return -1;
    while (h->n[i].value == -1) {
        int bit = bits_get(b);
        if (bit < 0) return -1;
        i = bit ? h->n[i].right : h->n[i].left;
        if (i < 0 || i >= h->count) return -1;
    }
    return h->n[i].value & 0xFF;
}

unsigned long sbk_decompress_huffman(const unsigned char *src, unsigned long src_len,
                                     unsigned char *out, unsigned long out_cap) {
    static struct Huff h;      /* 544 nodes is 7 KB; not worth a stack frame */
    unsigned long want;
    unsigned long pos = 0, t = 0;
    unsigned char flags;
    const unsigned char *payload;
    struct Bits b;

    if (src == NULL || out == NULL || src_len < 8) return 0;
    want = be32(src);
    flags = src[4];
    if (want == 0 || want > out_cap) return 0;
    payload = src + 5;
    if (src_len < 5) return 0;
    src_len -= 5;

    h.count = 0;
    h.head = h.tail = -1;
    h.qcount = 0;

    /* the symbol table: ranges of "first last weight weight ...", ended by a
     * zero range-start that is not the very first byte */
    for (;;) {
        int lo, hi, v;
        if (t >= src_len) return 0;
        lo = payload[t++];
        if (t != 1 && lo == 0) break;
        if (t >= src_len) return 0;
        hi = payload[t++];
        for (v = lo; v <= hi; v++) {
            if (h.count >= HUFF_MAX || t >= src_len) return 0;
            h.n[h.count].weight = payload[t++];
            h.n[h.count].left = -1;
            h.n[h.count].right = -1;
            h.n[h.count].value = (short)v;
            q_insert(&h, (short)h.count);
            h.count++;
        }
    }
    if (h.count == 0) return 0;

    /* two lightest become one node, until one is left */
    while (h.qcount >= 2) {
        short a = h.tail;
        short c;
        q_remove(&h, a);
        c = h.tail;
        q_remove(&h, c);
        if (h.count >= HUFF_MAX) return 0;
        h.n[h.count].weight = (short)(h.n[c].weight + h.n[a].weight);
        h.n[h.count].left = c;
        h.n[h.count].right = a;
        h.n[h.count].value = -1;
        q_insert(&h, (short)h.count);
        h.count++;
    }

    b.p = payload;
    b.n = src_len;
    b.byte = t;
    b.bit = 0;

    if (flags == 0) {
        /* plain Huffman: every symbol is an output byte */
        while (pos < want) {
            int s = huff_symbol(&h, &b);
            if (s < 0) return 0;
            out[pos++] = (unsigned char)s;
        }
    } else {
        /* Huffman over an LZ token stream: the first symbol is either 0 (the
         * next symbol is a literal) or the high nibble is a copy length and
         * the low nibble plus the next symbol are a 12-bit back distance. */
        while (pos < want) {
            int s0 = huff_symbol(&h, &b);
            int s1;
            if (s0 < 0) return 0;
            s1 = huff_symbol(&h, &b);
            if (s1 < 0) return 0;
            if (s0 == 0) {
                out[pos++] = (unsigned char)s1;
            } else {
                int len = (s0 >> 4) & 0xF;
                long from = (long)pos - (long)(((s0 << 8) | s1) & 0xFFF);
                int k;
                if (from < 0) return 0;
                for (k = 0; k < len && pos < want; k++) out[pos++] = out[from + k];
            }
        }
    }
    return pos;
}

/* --- the sequel: "Sno" --------------------------------------------------- */

unsigned long sbk_decompress_sno(const unsigned char *src, unsigned long src_len,
                                 unsigned char *out, unsigned long out_size) {
    unsigned long i = 0, pos = 0;
    if (src == NULL || out == NULL) return 0;
    while (pos < out_size) {
        unsigned char b0, b1;
        if (i + 1 >= src_len) return 0;
        b0 = src[i++];
        b1 = src[i++];
        if (b0 == 0) {
            out[pos++] = b1;
        } else {
            int len = b0 >> 4;
            long from = (long)pos - (long)(((b0 & 0x0F) << 8) | b1);
            int k;
            if (from < 0) return 0;
            for (k = 0; k < len && pos < out_size; k++) out[pos++] = out[from + k];
        }
    }
    return pos;
}
