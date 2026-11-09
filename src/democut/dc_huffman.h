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

#ifndef DC_HUFFMAN_H
#define DC_HUFFMAN_H

#include "dc_protocol.h"

// The static Huffman code every .dm_91 message is coded with. The tree itself
// and the lookup tables derived from it are private to dc_huffman.c; callers
// only ever need "give me the next byte" and "put this byte".
//
// Builds the tree on first use. Thread-safe and idempotent, so every entry
// point can just call it.
void dc_huff_init_static(void);

// Decodes one symbol starting at bit *offset and advances *offset past it.
// On running out of input it returns 0 and sets *offset to maxoffset + 1,
// which is how the message layer detects a truncated block - same contract
// id Tech 3's Huff_offsetReceive has.
int dc_huff_decode_byte(const dc_byte *fin, int *offset, int maxoffset);

// Encodes one symbol at bit *offset and advances *offset past it. Writing past
// maxoffset leaves *offset at maxoffset + 1, which the message layer turns into
// an overflow - same contract as Huff_offsetTransmit.
void dc_huff_encode_byte(int ch, dc_byte *fout, int *offset, int maxoffset);

#endif /* DC_HUFFMAN_H */
