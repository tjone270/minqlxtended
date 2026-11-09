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

// The .dm_91 message bitstream, split into a read half (dc_msg_read.c) and a
// write half (dc_msg_write.c) that share this one buffer type. Ported from id
// Tech 3's qcommon/msg.c, keeping only the Huffman-coded ("bitstream") path -
// a demo file never contains an out-of-band message, so the whole oob branch
// upstream carries is deliberately absent rather than stubbed.

#ifndef DC_MSG_H
#define DC_MSG_H

#include "dc_protocol.h"

typedef struct {
    int overflowed; // set when a write ran past maxsize; the buffer is then junk
    int readOverrun; // set when a read ran past cursize
    dc_byte *data;
    int maxsize;
    int cursize;
    int readcount;
    int bit; // bit cursor, for both reading and writing
} dc_msg_t;

// --- shared -----------------------------------------------------------------

void dc_msg_init(dc_msg_t *buf, dc_byte *data, int length);
void dc_msg_begin_reading(dc_msg_t *msg);

// --- read (dc_msg_read.c) ---------------------------------------------------

int dc_msg_read_bits(dc_msg_t *msg, int bits);
int dc_msg_read_byte(dc_msg_t *msg);
int dc_msg_read_short(dc_msg_t *msg);
int dc_msg_read_long(dc_msg_t *msg);
void dc_msg_read_data(dc_msg_t *msg, void *data, int len);
// Both return a pointer to a per-thread static buffer, valid until the next
// call on this thread - same contract id Tech 3's MSG_ReadString has.
const char *dc_msg_read_string(dc_msg_t *msg);
const char *dc_msg_read_big_string(dc_msg_t *msg);

void dc_msg_read_delta_entity(dc_msg_t *msg, const dc_entityState_t *from, dc_entityState_t *to, int number);
void dc_msg_read_delta_playerstate(dc_msg_t *msg, const dc_playerState_t *from, dc_playerState_t *to);

// --- write (dc_msg_write.c) -------------------------------------------------

void dc_msg_write_bits(dc_msg_t *msg, int value, int bits);
void dc_msg_write_byte(dc_msg_t *msg, int c);
void dc_msg_write_short(dc_msg_t *msg, int c);
void dc_msg_write_long(dc_msg_t *msg, int c);
void dc_msg_write_data(dc_msg_t *msg, const void *data, int length);
void dc_msg_write_string(dc_msg_t *msg, const char *s);
void dc_msg_write_big_string(dc_msg_t *msg, const char *s);

// force != 0 writes the entity even when nothing changed (what a gamestate
// baseline needs); to == NULL writes a "removed" record.
void dc_msg_write_delta_entity(dc_msg_t *msg, const dc_entityState_t *from, const dc_entityState_t *to, int force);
// from == NULL means "delta against an all-zero playerstate", i.e. a full one.
void dc_msg_write_delta_playerstate(dc_msg_t *msg, const dc_playerState_t *from, const dc_playerState_t *to);

#endif /* DC_MSG_H */
