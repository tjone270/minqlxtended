/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// Adaptive-Huffman coder from id Tech 3's qcommon/huffman.c, used here exactly
// the way the engine uses it for network/demo messages: the tree is built ONCE
// by replaying the static msg_hData frequency table through addRef, and then
// never updated again, so it behaves as a fixed code. Every .dm_91 in
// existence was written with this tree; reproducing it bit-for-bit is what lets
// this cutter re-encode a message the client will still accept.
//
// Ported rather than linked because minqlxtended is a plain C shared object
// injected into the Quake Live server: it has no id Tech 3 sources of its own,
// and the host process's own copies of these symbols must not be interposed
// (hence the dc_ prefix on everything with external linkage).

#include "dc_huffman.h"

#include <string.h>

// ---------------------------------------------------------------------------
// The tree. Private to this file: callers only ever decode or encode a byte.
// ---------------------------------------------------------------------------

#define DC_HMAX          256
#define DC_NYT           DC_HMAX
#define DC_INTERNAL_NODE (DC_HMAX + 1)

typedef struct dc_node_s {
    struct dc_node_s *left, *right, *parent;
    struct dc_node_s *next, *prev;
    struct dc_node_s **head;
    int weight;
    int symbol;
} dc_node_t;

typedef struct {
    int blocNode;
    int blocPtrs;

    dc_node_t *tree;
    dc_node_t *lhead;
    dc_node_t *ltail;
    dc_node_t *loc[DC_HMAX + 1];
    dc_node_t **freelist;

    dc_node_t nodeList[768];
    dc_node_t *nodePtrs[768];
} dc_huff_t;

// ---------------------------------------------------------------------------
// The lookup tables derived from it, which are what the hot paths actually use.
//
// Walking the tree one bit at a time is a dependent pointer chase per bit, and
// a .dm_91 is nothing but Huffman-coded bytes - it dominated the profile. Both
// tables are built once, from the finished tree, so the tree stays the single
// source of truth and a table can never disagree with it: every entry is
// written by walking the very code the tree defines.
//
// DECODE: DC_DECODE_BITS bits of lookahead indexed directly. The bits come off
// the wire LSB-first within each byte (dc_get_bit), so the index is simply the
// next DC_DECODE_BITS bits in read order, and every code whose length is <=
// DC_DECODE_BITS occupies the 2^(DC_DECODE_BITS - len) entries that share it as
// a prefix. Entry 0 means "this prefix needs more than DC_DECODE_BITS bits" and
// falls back to the tree walk, so rare deep symbols stay correct.
//
// ENCODE: the code and its length per symbol, so a byte is one shift and at
// most five byte stores instead of a recursive climb to the root.
// ---------------------------------------------------------------------------

#define DC_DECODE_BITS 11
#define DC_DECODE_SIZE (1 << DC_DECODE_BITS)
// Codes can be longer than the lookahead; this only has to hold the longest one
// this tree produces, which is checked at build time.
#define DC_MAX_CODE_BITS 32

typedef struct {
    unsigned int code; // bit i of the code is the i-th bit written/read
    int len;           // 0 = no code (symbol absent from the tree)
} dc_code_t;

// id Tech 3 qcommon/msg.c msg_hData[256]: the symbol frequencies the static
// message tree is built from. Verbatim - the tree, and therefore every
// bitstream, changes if a single number here does.
static const int dc_msg_hData[256] = {
     250315,   41193,    6292,    7106,    3730,    3750,    6110,   23283,
      33317,    6950,    7838,    9714,    9257,   17259,    3949,    1778,
       8288,    1604,    1590,    1663,    1100,    1213,    1238,    1134,
       1749,    1059,    1246,    1149,    1273,    4486,    2805,    3472,
      21819,    1159,    1670,    1066,    1043,    1012,    1053,    1070,
       1726,     888,    1180,     850,     960,     780,    1752,    3296,
      10630,    4514,    5881,    2685,    4650,    3837,    2093,    1867,
       2584,    1949,    1972,     940,    1134,    1788,    1670,    1206,
       5719,    6128,    7222,    6654,    3710,    3795,    1492,    1524,
       2215,    1140,    1355,     971,    2180,    1248,    1328,    1195,
       1770,    1078,    1264,    1266,    1168,     965,    1155,    1186,
       1347,    1228,    1529,    1600,    2617,    2048,    2546,    3275,
       2410,    3585,    2504,    2800,    2675,    6146,    3663,    2840,
      14253,    3164,    2221,    1687,    3208,    2739,    3512,    4796,
       4091,    3515,    5288,    4016,    7937,    6031,    5360,    3924,
       4892,    3743,    4566,    4807,    5852,    6400,    6225,    8291,
      23243,    7838,    7073,    8935,    5437,    4483,    3641,    5256,
       5312,    5328,    5370,    3492,    2458,    1694,    1821,    2121,
       1916,    1149,    1516,    1367,    1236,    1029,    1258,    1104,
       1245,    1006,    1149,    1025,    1241,     952,    1287,     997,
       1713,    1009,    1187,     879,    1099,     929,    1078,     951,
       1656,     930,    1153,    1030,    1262,    1062,    1214,    1060,
       1621,     930,    1106,     912,    1034,     892,    1158,     990,
       1175,     850,    1121,     903,    1087,     920,    1144,    1056,
       3462,    2240,    4397,   12136,    7758,    1345,    1307,    3278,
       1950,     886,    1023,    1112,    1077,    1042,    1061,    1071,
       1484,    1001,    1096,     915,    1052,     995,    1070,     876,
       1111,     851,    1059,     805,    1112,     923,    1103,     817,
       1899,    1872,     976,     841,    1127,     956,    1159,     950,
       7791,     954,    1289,     933,    1127,    3207,    1020,     927,
       1355,     768,    1040,     745,     952,     805,    1073,     740,
       1013,     805,    1008,     796,     996,    1057,   11457,   13504
};

// The engine keeps this bit cursor in a file-static, and add_bit/get_bit/send
// all reach for it rather than threading it through. Kept, because every
// function below is a faithful port and diverging here would be an easy place
// to introduce an off-by-one; made thread-local so two threads encoding at once
// cannot corrupt each other's cursor (the engine never needed that, this
// library might).
static _Thread_local int dc_bloc = 0;

// The sub-byte remainder a message carries uncompressed is read and written
// inline by dc_msg_read.c / dc_msg_write.c - it is far too hot to route through
// a call that saves and restores the coder's cursor per bit. What stays here is
// only what the tree walk below needs.
static void dc_add_bit(char bit, dc_byte *fout) {
    if ((dc_bloc & 7) == 0) {
        fout[(dc_bloc >> 3)] = 0;
    }
    fout[(dc_bloc >> 3)] |= bit << (dc_bloc & 7);
    dc_bloc++;
}

static int dc_get_bit(const dc_byte *fin) {
    int t;
    t = (fin[(dc_bloc >> 3)] >> (dc_bloc & 7)) & 0x1;
    dc_bloc++;
    return t;
}

static dc_node_t **dc_get_ppnode(dc_huff_t *huff) {
    dc_node_t **tppnode;
    if (!huff->freelist) {
        return &(huff->nodePtrs[huff->blocPtrs++]);
    }
    tppnode        = huff->freelist;
    huff->freelist = (dc_node_t **)*tppnode;
    return tppnode;
}

static void dc_free_ppnode(dc_huff_t *huff, dc_node_t **ppnode) {
    *ppnode        = (dc_node_t *)huff->freelist;
    huff->freelist = ppnode;
}

/* Swap the location of these two nodes in the tree */
static void dc_swap(dc_huff_t *huff, dc_node_t *node1, dc_node_t *node2) {
    dc_node_t *par1, *par2;

    par1 = node1->parent;
    par2 = node2->parent;

    if (par1) {
        if (par1->left == node1) {
            par1->left = node2;
        } else {
            par1->right = node2;
        }
    } else {
        huff->tree = node2;
    }

    if (par2) {
        if (par2->left == node2) {
            par2->left = node1;
        } else {
            par2->right = node1;
        }
    } else {
        huff->tree = node1;
    }

    node1->parent = par2;
    node2->parent = par1;
}

/* Swap these two nodes in the linked list (update ranks) */
static void dc_swaplist(dc_node_t *node1, dc_node_t *node2) {
    dc_node_t *par1;

    par1        = node1->next;
    node1->next = node2->next;
    node2->next = par1;

    par1        = node1->prev;
    node1->prev = node2->prev;
    node2->prev = par1;

    if (node1->next == node1) {
        node1->next = node2;
    }
    if (node2->next == node2) {
        node2->next = node1;
    }
    if (node1->next) {
        node1->next->prev = node1;
    }
    if (node2->next) {
        node2->next->prev = node2;
    }
    if (node1->prev) {
        node1->prev->next = node1;
    }
    if (node2->prev) {
        node2->prev->next = node2;
    }
}

/* Do the increments */
static void dc_increment(dc_huff_t *huff, dc_node_t *node) {
    dc_node_t *lnode;

    if (!node) {
        return;
    }

    if (node->next != NULL && node->next->weight == node->weight) {
        lnode = *node->head;
        if (lnode != node->parent) {
            dc_swap(huff, lnode, node);
        }
        dc_swaplist(lnode, node);
    }
    if (node->prev && node->prev->weight == node->weight) {
        *node->head = node->prev;
    } else {
        *node->head = NULL;
        dc_free_ppnode(huff, node->head);
    }
    node->weight++;
    if (node->next && node->next->weight == node->weight) {
        node->head = node->next->head;
    } else {
        node->head  = dc_get_ppnode(huff);
        *node->head = node;
    }
    if (node->parent) {
        dc_increment(huff, node->parent);
        if (node->prev == node->parent) {
            dc_swaplist(node, node->parent);
            if (*node->head == node) {
                *node->head = node->parent;
            }
        }
    }
}

static void dc_huff_add_ref(dc_huff_t *huff, dc_byte ch) {
    dc_node_t *tnode, *tnode2;
    if (huff->loc[ch] == NULL) { /* if this is the first transmission of this node */
        tnode  = &(huff->nodeList[huff->blocNode++]);
        tnode2 = &(huff->nodeList[huff->blocNode++]);

        tnode2->symbol = DC_INTERNAL_NODE;
        tnode2->weight = 1;
        tnode2->next   = huff->lhead->next;
        if (huff->lhead->next) {
            huff->lhead->next->prev = tnode2;
            if (huff->lhead->next->weight == 1) {
                tnode2->head = huff->lhead->next->head;
            } else {
                tnode2->head  = dc_get_ppnode(huff);
                *tnode2->head = tnode2;
            }
        } else {
            tnode2->head  = dc_get_ppnode(huff);
            *tnode2->head = tnode2;
        }
        huff->lhead->next = tnode2;
        tnode2->prev      = huff->lhead;

        tnode->symbol = ch;
        tnode->weight = 1;
        tnode->next   = huff->lhead->next;
        if (huff->lhead->next) {
            huff->lhead->next->prev = tnode;
            if (huff->lhead->next->weight == 1) {
                tnode->head = huff->lhead->next->head;
            } else {
                /* this should never happen */
                tnode->head  = dc_get_ppnode(huff);
                *tnode->head = tnode2;
            }
        } else {
            /* this should never happen */
            tnode->head  = dc_get_ppnode(huff);
            *tnode->head = tnode;
        }
        huff->lhead->next = tnode;
        tnode->prev       = huff->lhead;
        tnode->left = tnode->right = NULL;

        if (huff->lhead->parent) {
            if (huff->lhead->parent->left == huff->lhead) { /* lhead is guaranteed to be the NYT */
                huff->lhead->parent->left = tnode2;
            } else {
                huff->lhead->parent->right = tnode2;
            }
        } else {
            huff->tree = tnode2;
        }

        tnode2->right = tnode;
        tnode2->left  = huff->lhead;

        tnode2->parent     = huff->lhead->parent;
        huff->lhead->parent = tnode->parent = tnode2;

        huff->loc[ch] = tnode;

        dc_increment(huff, tnode2->parent);
    } else {
        dc_increment(huff, huff->loc[ch]);
    }
}

static void dc_huff_offset_receive(const dc_node_t *node, int *ch, const dc_byte *fin, int *offset,
                                   int maxoffset) {
    dc_bloc = *offset;
    while (node && node->symbol == DC_INTERNAL_NODE) {
        if (dc_bloc >= maxoffset) {
            *ch     = 0;
            *offset = maxoffset + 1;
            return;
        }
        if (dc_get_bit(fin)) {
            node = node->right;
        } else {
            node = node->left;
        }
    }
    if (!node) {
        *ch = 0;
        return;
    }
    *ch     = node->symbol;
    *offset = dc_bloc;
}

/* Send the prefix code for this node */
static void dc_send(const dc_node_t *node, const dc_node_t *child, dc_byte *fout, int maxoffset) {
    if (node->parent) {
        dc_send(node->parent, node, fout, maxoffset);
    }
    if (child) {
        if (dc_bloc >= maxoffset) {
            dc_bloc = maxoffset + 1;
            return;
        }
        if (node->right == child) {
            dc_add_bit(1, fout);
        } else {
            dc_add_bit(0, fout);
        }
    }
}

static void dc_huff_offset_transmit(const dc_huff_t *huff, int ch, dc_byte *fout, int *offset,
                                    int maxoffset) {
    if (ch < 0 || ch > DC_HMAX || huff->loc[ch] == NULL) {
        // Impossible against the fully-populated static tree (every one of the
        // 256 symbols has a non-zero frequency in dc_msg_hData), but a missing
        // node would otherwise be a null dereference rather than a bad byte.
        return;
    }
    dc_bloc = *offset;
    dc_send(huff->loc[ch], NULL, fout, maxoffset);
    *offset = dc_bloc;
}

static dc_huff_t dc_msgHuff;
static unsigned short dc_decodeLut[DC_DECODE_SIZE]; // (symbol << 4) | length, 0 = miss
static dc_code_t dc_encodeTable[DC_HMAX + 1];
static int dc_msgHuffReady = 0;

// Walks the finished tree and records, for every leaf, the bit path that
// reaches it - in the SAME order the coder emits and consumes those bits
// (dc_send writes root-to-leaf, dc_get_bit reads them in that order), so bit i
// of `code` is the i-th bit on the wire. Fills both tables from that one walk.
static void dc_build_tables(const dc_node_t *node, unsigned int code, int len) {
    if (node == NULL) {
        return;
    }
    if (node->symbol != DC_INTERNAL_NODE) {
        if (node->symbol >= 0 && node->symbol <= DC_HMAX && len > 0 && len <= DC_MAX_CODE_BITS) {
            dc_encodeTable[node->symbol].code = code;
            dc_encodeTable[node->symbol].len  = len;

            if (len <= DC_DECODE_BITS) {
                // Every lookahead window that starts with this code decodes to
                // this symbol; the remaining DC_DECODE_BITS - len bits belong to
                // whatever follows and are simply not consumed.
                const unsigned int step = 1u << len;
                for (unsigned int i = code; i < DC_DECODE_SIZE; i += step) {
                    dc_decodeLut[i] = (unsigned short)((node->symbol << 4) | len);
                }
            }
        }
        return;
    }
    if (len < DC_MAX_CODE_BITS) {
        dc_build_tables(node->left, code, len + 1);
        dc_build_tables(node->right, code | (1u << len), len + 1);
    }
}

// Huff_Init's own seeding, for the single tree this file keeps: start with just
// the NYT node, which every later addRef splits away from.
static void dc_huff_reset(dc_huff_t *huff) {
    memset(huff, 0, sizeof(*huff));

    huff->tree = huff->lhead = huff->ltail = huff->loc[DC_NYT] = &(huff->nodeList[huff->blocNode++]);
    huff->tree->symbol = DC_NYT;
    huff->tree->weight = 0;
    huff->lhead->next = huff->lhead->prev = NULL;
    huff->tree->parent = huff->tree->left = huff->tree->right = NULL;
}

void dc_huff_init_static(void) {
    // Racing callers would both build the same tree into the same memory, which
    // is still not something to rely on, so serialise. pthread_once is not used
    // to keep this file free of a -lpthread dependency of its own; the caller
    // (demo_cut.c) already runs on one dedicated thread, and the guard is here
    // only so a second caller cannot observe a half-built tree.
    static _Atomic int building = 0;
    if (dc_msgHuffReady) {
        return;
    }
    // Cheap spin: the build is a few milliseconds and happens once per process.
    int expected = 0;
    while (!__atomic_compare_exchange_n(&building, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        expected = 0;
        if (dc_msgHuffReady) {
            return;
        }
    }
    if (!dc_msgHuffReady) {
        dc_huff_reset(&dc_msgHuff);
        for (int i = 0; i < 256; i++) {
            for (int j = 0; j < dc_msg_hData[i]; j++) {
                dc_huff_add_ref(&dc_msgHuff, (dc_byte)i);
            }
        }
        memset(dc_decodeLut, 0, sizeof(dc_decodeLut));
        memset(dc_encodeTable, 0, sizeof(dc_encodeTable));
        dc_build_tables(dc_msgHuff.tree, 0, 0);
        __atomic_store_n(&dc_msgHuffReady, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&building, 0, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// The two entry points the message layer uses.
// ---------------------------------------------------------------------------

int dc_huff_decode_byte(const dc_byte *fin, int *offset, int maxoffset) {
    dc_huff_init_static();

    const int bloc = *offset;
    const int byteIndex = bloc >> 3;

    // Fast path only when a whole 32-bit window is inside the buffer, which also
    // guarantees at least 25 readable bits - more than any code the table
    // answers for. Everything else (the last few bytes of a message, and any
    // code longer than the lookahead) goes down the tree walk, which is the
    // definition this table was built from.
    if (byteIndex + 4 <= (maxoffset >> 3)) {
        const unsigned int window = ((unsigned int)fin[byteIndex]) | ((unsigned int)fin[byteIndex + 1] << 8) |
                                    ((unsigned int)fin[byteIndex + 2] << 16) |
                                    ((unsigned int)fin[byteIndex + 3] << 24);
        const unsigned short entry = dc_decodeLut[(window >> (bloc & 7)) & (DC_DECODE_SIZE - 1)];
        if (entry != 0) {
            *offset = bloc + (int)(entry & 15);
            return (int)(entry >> 4);
        }
    }

    int ch = 0;
    dc_huff_offset_receive(dc_msgHuff.tree, &ch, fin, offset, maxoffset);
    return ch;
}

void dc_huff_encode_byte(int ch, dc_byte *fout, int *offset, int maxoffset) {
    dc_huff_init_static();

    if (ch < 0 || ch > DC_HMAX) {
        return;
    }
    const dc_code_t code = dc_encodeTable[ch];
    const int bloc       = *offset;

    // The slow path also owns the truncation behaviour (stop at maxoffset and
    // leave the cursor one past it), so anything that would not fit whole is
    // handed to it rather than half-written here.
    if (code.len > 0 && bloc + code.len <= maxoffset) {
        const int byteIndex = bloc >> 3;
        const int shift     = bloc & 7;
        const unsigned long long value = (unsigned long long)code.code << shift;
        const int byteCount            = (shift + code.len + 7) >> 3;

        // Mirrors dc_add_bit exactly: the first byte is only zeroed when this
        // write starts it, every later byte is started by this write and so is
        // assigned rather than OR-ed.
        if (shift == 0) {
            fout[byteIndex] = (dc_byte)(value & 0xff);
        } else {
            fout[byteIndex] |= (dc_byte)(value & 0xff);
        }
        for (int i = 1; i < byteCount; i++) {
            fout[byteIndex + i] = (dc_byte)((value >> (8 * i)) & 0xff);
        }
        *offset = bloc + code.len;
        return;
    }

    dc_huff_offset_transmit(&dc_msgHuff, ch, fout, offset, maxoffset);
}
