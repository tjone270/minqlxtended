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

// Read half of the .dm_91 message bitstream: id Tech 3's MSG_Read* for the
// Huffman path only, plus the protocol-91 delta decoders.
//
// Every out-of-range access sets msg->readOverrun instead of erroring out the
// way the engine does: this library parses files that a crashed server may have
// truncated mid-block, and the caller wants "this file ends here" rather than a
// fatal.

#include "dc_msg.h"
#include "dc_huffman.h"

#include <string.h>

void dc_msg_init(dc_msg_t *buf, dc_byte *data, int length) {
    dc_huff_init_static();
    memset(buf, 0, sizeof(*buf));
    buf->data    = data;
    buf->maxsize = length;
}

void dc_msg_begin_reading(dc_msg_t *msg) {
    msg->readcount   = 0;
    msg->bit         = 0;
    msg->readOverrun = 0;
}

int dc_msg_read_bits(dc_msg_t *msg, int bits) {
    int value = 0;
    int get;
    int sgn;
    int i, nbits;

    if (msg->readcount > msg->cursize) {
        return 0;
    }

    if (bits < 0) {
        bits = -bits;
        sgn  = 1;
    } else {
        sgn = 0;
    }

    nbits = 0;
    if (bits & 7) {
        nbits = bits & 7;
        if (msg->bit + nbits > msg->cursize << 3) {
            msg->readcount   = msg->cursize + 1;
            msg->readOverrun = 1;
            return 0;
        }
        // The coder's own single-bit read, inlined. This loop runs once per
        // delta-field "changed?" flag, which makes it the hottest thing in the
        // parser by call count; going through a function that parks the cursor
        // in a thread-local on every bit cost more than the read itself.
        for (i = 0; i < nbits; i++) {
            value |= (((msg->data[msg->bit >> 3] >> (msg->bit & 7)) & 1) << i);
            msg->bit++;
        }
        bits = bits - nbits;
    }
    if (bits) {
        for (i = 0; i < bits; i += 8) {
            get = dc_huff_decode_byte(msg->data, &msg->bit, msg->cursize << 3);
            value = (int)((unsigned int)value | ((unsigned int)get << (i + nbits)));

            if (msg->bit > msg->cursize << 3) {
                msg->readcount   = msg->cursize + 1;
                msg->readOverrun = 1;
                return 0;
            }
        }
    }
    msg->readcount = (msg->bit >> 3) + 1;

    if (sgn && bits > 0 && bits < 32) {
        if (value & (1 << (bits - 1))) {
            value |= -1 ^ ((1 << bits) - 1);
        }
    }

    return value;
}

// -1 signals "past the end", which is how the string readers detect a truncated
// message; a genuine 0xFF byte comes back as 255 because the field is read
// unsigned.
static int dc_msg_read_byte_raw(dc_msg_t *msg) {
    int c = (unsigned char)dc_msg_read_bits(msg, 8);
    if (msg->readcount > msg->cursize) {
        return -1;
    }
    return c;
}

int dc_msg_read_byte(dc_msg_t *msg) {
    return dc_msg_read_byte_raw(msg);
}

int dc_msg_read_short(dc_msg_t *msg) {
    int c = (short)dc_msg_read_bits(msg, 16);
    if (msg->readcount > msg->cursize) {
        return -1;
    }
    return c;
}

int dc_msg_read_long(dc_msg_t *msg) {
    int c = dc_msg_read_bits(msg, 32);
    if (msg->readcount > msg->cursize) {
        return -1;
    }
    return c;
}

void dc_msg_read_data(dc_msg_t *msg, void *data, int len) {
    for (int i = 0; i < len; i++) {
        ((dc_byte *)data)[i] = (dc_byte)dc_msg_read_byte(msg);
    }
}

static const char *dc_read_string_common(dc_msg_t *msg, char *string, int size) {
    int l = 0;
    for (;;) {
        int c = dc_msg_read_byte_raw(msg);
        if (c == -1 || c == 0) {
            break;
        }
        // The engine rewrites '%' on the way in (format-string hardening) for
        // protocol 91 too, so a configstring that round-trips through here
        // matches what a real client has in memory, not what the file holds.
        // Kept deliberately: dropping it would make a re-encoded gamestate
        // differ from the one the engine would have built from the same bytes.
        if (c == '%') {
            c = '.';
        }
        if (l >= size - 1) {
            break;
        }
        string[l++] = (char)c;
    }
    string[l] = '\0';
    return string;
}

const char *dc_msg_read_string(dc_msg_t *msg) {
    static _Thread_local char string[DC_MAX_STRING_CHARS];
    return dc_read_string_common(msg, string, (int)sizeof(string));
}

const char *dc_msg_read_big_string(dc_msg_t *msg) {
    static _Thread_local char string[DC_BIG_INFO_STRING];
    return dc_read_string_common(msg, string, (int)sizeof(string));
}

// ---------------------------------------------------------------------------
// entityState_t / playerState_t delta decode (protocol 91)
// ---------------------------------------------------------------------------

void dc_msg_read_delta_entity(dc_msg_t *msg, const dc_entityState_t *from, dc_entityState_t *to, int number) {
    static const dc_entityState_t nullState;
    const dc_netField_t *field;
    int i, lc, trunc;
    const int numFields = dc_entityStateFieldCount91;

    if (number < 0 || number >= DC_MAX_GENTITIES) {
        msg->readOverrun = 1;
        return;
    }
    if (from == NULL) {
        from = &nullState;
    }

    // check for a remove
    if (dc_msg_read_bits(msg, 1) == 1) {
        memset(to, 0, sizeof(*to));
        to->number = DC_MAX_GENTITIES - 1;
        return;
    }

    // check for no delta
    if (dc_msg_read_bits(msg, 1) == 0) {
        *to        = *from;
        to->number = number;
        return;
    }

    lc = dc_msg_read_byte(msg);
    if (lc > numFields || lc < 0) {
        msg->readOverrun = 1;
        *to              = *from;
        to->number       = number;
        return;
    }

    to->number = number;

    field = dc_entityStateFields91;
    for (i = 0; i < lc; i++, field++) {
        const int *fromF = (const int *)((const dc_byte *)from + field->offset);
        int *toF         = (int *)((dc_byte *)to + field->offset);

        if (!dc_msg_read_bits(msg, 1)) {
            *toF = *fromF; // no change
            continue;
        }
        if (field->bits == 0) {
            // float
            if (dc_msg_read_bits(msg, 1) == 0) {
                *(float *)toF = 0.0f;
            } else if (dc_msg_read_bits(msg, 1) == 0) {
                trunc         = dc_msg_read_bits(msg, DC_FLOAT_INT_BITS);
                trunc        -= DC_FLOAT_INT_BIAS;
                *(float *)toF = (float)trunc;
            } else {
                *toF = dc_msg_read_bits(msg, 32);
            }
        } else {
            if (dc_msg_read_bits(msg, 1) == 0) {
                *toF = 0;
            } else {
                *toF = dc_msg_read_bits(msg, field->bits);
            }
        }
    }

    field = &dc_entityStateFields91[lc];
    for (i = lc; i < numFields; i++, field++) {
        const int *fromF = (const int *)((const dc_byte *)from + field->offset);
        int *toF         = (int *)((dc_byte *)to + field->offset);
        *toF             = *fromF;
    }
}

void dc_msg_read_delta_playerstate(dc_msg_t *msg, const dc_playerState_t *from, dc_playerState_t *to) {
    static const dc_playerState_t nullState;
    const dc_netField_t *field;
    int i, lc, bits, trunc;
    const int numFields = dc_playerStateFieldCount91;

    if (from == NULL) {
        from = &nullState;
    }
    *to = *from;

    lc = dc_msg_read_byte(msg);
    if (lc > numFields || lc < 0) {
        msg->readOverrun = 1;
        return;
    }

    field = dc_playerStateFields91;
    for (i = 0; i < lc; i++, field++) {
        const int *fromF = (const int *)((const dc_byte *)from + field->offset);
        int *toF         = (int *)((dc_byte *)to + field->offset);

        if (!dc_msg_read_bits(msg, 1)) {
            *toF = *fromF; // no change
            continue;
        }
        if (field->bits == 0) {
            // float: no "exactly zero" bit here, unlike the entity encoding
            if (dc_msg_read_bits(msg, 1) == 0) {
                trunc         = dc_msg_read_bits(msg, DC_FLOAT_INT_BITS);
                trunc        -= DC_FLOAT_INT_BIAS;
                *(float *)toF = (float)trunc;
            } else {
                *toF = dc_msg_read_bits(msg, 32);
            }
        } else {
            *toF = dc_msg_read_bits(msg, field->bits);
        }
    }

    field = &dc_playerStateFields91[lc];
    for (i = lc; i < numFields; i++, field++) {
        const int *fromF = (const int *)((const dc_byte *)from + field->offset);
        int *toF         = (int *)((dc_byte *)to + field->offset);
        *toF             = *fromF;
    }

    // the arrays
    if (dc_msg_read_bits(msg, 1)) {
        if (dc_msg_read_bits(msg, 1)) {
            bits = dc_msg_read_bits(msg, DC_MAX_STATS);
            for (i = 0; i < DC_MAX_STATS; i++) {
                if (bits & (1 << i)) {
                    to->stats[i] = dc_msg_read_short(msg);
                }
            }
        }
        if (dc_msg_read_bits(msg, 1)) {
            bits = dc_msg_read_bits(msg, DC_MAX_PERSISTANT);
            for (i = 0; i < DC_MAX_PERSISTANT; i++) {
                if (bits & (1 << i)) {
                    to->persistant[i] = dc_msg_read_short(msg);
                }
            }
        }
        if (dc_msg_read_bits(msg, 1)) {
            bits = dc_msg_read_bits(msg, DC_MAX_WEAPONS);
            for (i = 0; i < DC_MAX_WEAPONS; i++) {
                if (bits & (1 << i)) {
                    to->ammo[i] = dc_msg_read_short(msg);
                }
            }
        }
        if (dc_msg_read_bits(msg, 1)) {
            bits = dc_msg_read_bits(msg, DC_MAX_POWERUPS);
            for (i = 0; i < DC_MAX_POWERUPS; i++) {
                if (bits & (1 << i)) {
                    to->powerups[i] = dc_msg_read_long(msg);
                }
            }
        }
    }
}
