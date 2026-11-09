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

// Write half of the .dm_91 message bitstream: id Tech 3's MSG_Write* for the
// Huffman path only, plus the protocol-91 delta encoders. Exact mirror of
// dc_msg_read.c - the two must be edited together or a re-encoded message stops
// decoding.

#include "dc_msg.h"
#include "dc_huffman.h"

#include <string.h>

void dc_msg_write_bits(dc_msg_t *msg, int value, int bits) {
    int i;

    if (msg->overflowed) {
        return;
    }
    if (bits == 0 || bits < -31 || bits > 32) {
        msg->overflowed = 1;
        return;
    }
    if (bits < 0) {
        bits = -bits;
    }

    value &= (int)(0xffffffffu >> (32 - bits));

    if (bits & 7) {
        int nbits = bits & 7;
        if (msg->bit + nbits >= msg->maxsize << 3) {
            msg->overflowed = 1;
            return;
        }
        // The coder's own single-bit write, inlined - mirror of the read side
        // in dc_msg_read.c, including "zero the byte this write starts".
        for (i = 0; i < nbits; i++) {
            const int byteIndex = msg->bit >> 3;
            if ((msg->bit & 7) == 0) {
                msg->data[byteIndex] = 0;
            }
            msg->data[byteIndex] |= (dc_byte)((value & 1) << (msg->bit & 7));
            msg->bit++;
            value = (int)((unsigned int)value >> 1);
        }
        bits = bits - nbits;
    }
    if (bits) {
        for (i = 0; i < bits; i += 8) {
            dc_huff_encode_byte((value & 0xff), msg->data, &msg->bit, msg->maxsize << 3);
            value = (int)((unsigned int)value >> 8);

            if (msg->bit >= msg->maxsize << 3) {
                msg->overflowed = 1;
                return;
            }
        }
    }
    msg->cursize = (msg->bit >> 3) + 1;
}

void dc_msg_write_byte(dc_msg_t *msg, int c) {
    dc_msg_write_bits(msg, c & 0xff, 8);
}

void dc_msg_write_short(dc_msg_t *msg, int c) {
    dc_msg_write_bits(msg, c & 0xffff, 16);
}

void dc_msg_write_long(dc_msg_t *msg, int c) {
    dc_msg_write_bits(msg, c, 32);
}

void dc_msg_write_data(dc_msg_t *msg, const void *data, int length) {
    for (int i = 0; i < length; i++) {
        dc_msg_write_byte(msg, ((const dc_byte *)data)[i]);
    }
}

static void dc_write_string_common(dc_msg_t *msg, const char *s, char *scratch, int size) {
    int l, i;

    if (!s) {
        dc_msg_write_data(msg, "", 1);
        return;
    }
    l = (int)strlen(s);
    if (l >= size) {
        // Longer than the protocol allows: the engine drops it to an empty
        // string rather than truncating, so do the same instead of shipping
        // half a configstring.
        dc_msg_write_data(msg, "", 1);
        return;
    }
    memcpy(scratch, s, (size_t)l);
    scratch[l] = '\0';
    for (i = 0; i < l; i++) {
        // Protocol 91 keeps bytes >127 (UTF-8 names); only '%' is rewritten.
        // Same rule dc_msg_read.c applies on the way in, so a string that came
        // out of a real message goes back in unchanged.
        if (scratch[i] == '%') {
            scratch[i] = '.';
        }
    }
    dc_msg_write_data(msg, scratch, l + 1);
}

void dc_msg_write_string(dc_msg_t *msg, const char *s) {
    static _Thread_local char scratch[DC_MAX_STRING_CHARS];
    dc_write_string_common(msg, s, scratch, (int)sizeof(scratch));
}

void dc_msg_write_big_string(dc_msg_t *msg, const char *s) {
    static _Thread_local char scratch[DC_BIG_INFO_STRING];
    dc_write_string_common(msg, s, scratch, (int)sizeof(scratch));
}

// ---------------------------------------------------------------------------
// entityState_t / playerState_t delta encode (protocol 91)
// ---------------------------------------------------------------------------

void dc_msg_write_delta_entity(dc_msg_t *msg, const dc_entityState_t *from, const dc_entityState_t *to, int force) {
    static const dc_entityState_t nullState;
    const dc_netField_t *field;
    int i, lc;
    const int numFields = dc_entityStateFieldCount91;

    if (from == NULL) {
        from = &nullState;
    }

    // a NULL to is a delta remove message
    if (to == NULL) {
        dc_msg_write_bits(msg, from->number, DC_GENTITYNUM_BITS);
        dc_msg_write_bits(msg, 1, 1);
        return;
    }
    if (to->number < 0 || to->number >= DC_MAX_GENTITIES) {
        msg->overflowed = 1;
        return;
    }

    lc    = 0;
    field = dc_entityStateFields91;
    for (i = 0; i < numFields; i++, field++) {
        const int *fromF = (const int *)((const dc_byte *)from + field->offset);
        const int *toF   = (const int *)((const dc_byte *)to + field->offset);
        if (*fromF != *toF) {
            lc = i + 1;
        }
    }

    if (lc == 0) {
        if (!force) {
            return; // nothing at all
        }
        dc_msg_write_bits(msg, to->number, DC_GENTITYNUM_BITS);
        dc_msg_write_bits(msg, 0, 1); // not removed
        dc_msg_write_bits(msg, 0, 1); // no delta
        return;
    }

    dc_msg_write_bits(msg, to->number, DC_GENTITYNUM_BITS);
    dc_msg_write_bits(msg, 0, 1); // not removed
    dc_msg_write_bits(msg, 1, 1); // we have a delta
    dc_msg_write_byte(msg, lc);

    field = dc_entityStateFields91;
    for (i = 0; i < lc; i++, field++) {
        const int *fromF = (const int *)((const dc_byte *)from + field->offset);
        const int *toF   = (const int *)((const dc_byte *)to + field->offset);

        if (*fromF == *toF) {
            dc_msg_write_bits(msg, 0, 1); // no change
            continue;
        }
        dc_msg_write_bits(msg, 1, 1); // changed

        if (field->bits == 0) {
            const float fullFloat = *(const float *)toF;
            const int trunc       = (int)fullFloat;

            if (fullFloat == 0.0f) {
                dc_msg_write_bits(msg, 0, 1);
            } else {
                dc_msg_write_bits(msg, 1, 1);
                if ((float)trunc == fullFloat && trunc + DC_FLOAT_INT_BIAS >= 0 &&
                    trunc + DC_FLOAT_INT_BIAS < (1 << DC_FLOAT_INT_BITS)) {
                    dc_msg_write_bits(msg, 0, 1);
                    dc_msg_write_bits(msg, trunc + DC_FLOAT_INT_BIAS, DC_FLOAT_INT_BITS);
                } else {
                    dc_msg_write_bits(msg, 1, 1);
                    dc_msg_write_bits(msg, *toF, 32);
                }
            }
        } else {
            if (*toF == 0) {
                dc_msg_write_bits(msg, 0, 1);
            } else {
                dc_msg_write_bits(msg, 1, 1);
                dc_msg_write_bits(msg, *toF, field->bits);
            }
        }
    }
}

void dc_msg_write_delta_playerstate(dc_msg_t *msg, const dc_playerState_t *from, const dc_playerState_t *to) {
    static const dc_playerState_t nullState;
    const dc_netField_t *field;
    int i, lc;
    int statsbits, persistantbits, ammobits, powerupbits;
    const int numFields = dc_playerStateFieldCount91;

    if (from == NULL) {
        from = &nullState;
    }

    lc    = 0;
    field = dc_playerStateFields91;
    for (i = 0; i < numFields; i++, field++) {
        const int *fromF = (const int *)((const dc_byte *)from + field->offset);
        const int *toF   = (const int *)((const dc_byte *)to + field->offset);
        if (*fromF != *toF) {
            lc = i + 1;
        }
    }

    dc_msg_write_byte(msg, lc);

    field = dc_playerStateFields91;
    for (i = 0; i < lc; i++, field++) {
        const int *fromF = (const int *)((const dc_byte *)from + field->offset);
        const int *toF   = (const int *)((const dc_byte *)to + field->offset);

        if (*fromF == *toF) {
            dc_msg_write_bits(msg, 0, 1); // no change
            continue;
        }
        dc_msg_write_bits(msg, 1, 1); // changed

        if (field->bits == 0) {
            const float fullFloat = *(const float *)toF;
            const int trunc       = (int)fullFloat;

            if ((float)trunc == fullFloat && trunc + DC_FLOAT_INT_BIAS >= 0 &&
                trunc + DC_FLOAT_INT_BIAS < (1 << DC_FLOAT_INT_BITS)) {
                dc_msg_write_bits(msg, 0, 1);
                dc_msg_write_bits(msg, trunc + DC_FLOAT_INT_BIAS, DC_FLOAT_INT_BITS);
            } else {
                dc_msg_write_bits(msg, 1, 1);
                dc_msg_write_bits(msg, *toF, 32);
            }
        } else {
            dc_msg_write_bits(msg, *toF, field->bits);
        }
    }

    statsbits = 0;
    for (i = 0; i < DC_MAX_STATS; i++) {
        if (to->stats[i] != from->stats[i]) {
            statsbits |= 1 << i;
        }
    }
    persistantbits = 0;
    for (i = 0; i < DC_MAX_PERSISTANT; i++) {
        if (to->persistant[i] != from->persistant[i]) {
            persistantbits |= 1 << i;
        }
    }
    ammobits = 0;
    for (i = 0; i < DC_MAX_WEAPONS; i++) {
        if (to->ammo[i] != from->ammo[i]) {
            ammobits |= 1 << i;
        }
    }
    powerupbits = 0;
    for (i = 0; i < DC_MAX_POWERUPS; i++) {
        if (to->powerups[i] != from->powerups[i]) {
            powerupbits |= 1 << i;
        }
    }

    if (!statsbits && !persistantbits && !ammobits && !powerupbits) {
        dc_msg_write_bits(msg, 0, 1); // no change
        return;
    }
    dc_msg_write_bits(msg, 1, 1); // changed

    if (statsbits) {
        dc_msg_write_bits(msg, 1, 1);
        dc_msg_write_bits(msg, statsbits, DC_MAX_STATS);
        for (i = 0; i < DC_MAX_STATS; i++) {
            if (statsbits & (1 << i)) {
                dc_msg_write_short(msg, to->stats[i]);
            }
        }
    } else {
        dc_msg_write_bits(msg, 0, 1);
    }

    if (persistantbits) {
        dc_msg_write_bits(msg, 1, 1);
        dc_msg_write_bits(msg, persistantbits, DC_MAX_PERSISTANT);
        for (i = 0; i < DC_MAX_PERSISTANT; i++) {
            if (persistantbits & (1 << i)) {
                dc_msg_write_short(msg, to->persistant[i]);
            }
        }
    } else {
        dc_msg_write_bits(msg, 0, 1);
    }

    if (ammobits) {
        dc_msg_write_bits(msg, 1, 1);
        dc_msg_write_bits(msg, ammobits, DC_MAX_WEAPONS);
        for (i = 0; i < DC_MAX_WEAPONS; i++) {
            if (ammobits & (1 << i)) {
                dc_msg_write_short(msg, to->ammo[i]);
            }
        }
    } else {
        dc_msg_write_bits(msg, 0, 1);
    }

    if (powerupbits) {
        dc_msg_write_bits(msg, 1, 1);
        dc_msg_write_bits(msg, powerupbits, DC_MAX_POWERUPS);
        for (i = 0; i < DC_MAX_POWERUPS; i++) {
            if (powerupbits & (1 << i)) {
                dc_msg_write_long(msg, to->powerups[i]);
            }
        }
    } else {
        dc_msg_write_bits(msg, 0, 1);
    }
}
