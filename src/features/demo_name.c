/*
Copyright (C) 2015 Mino <mino@minomino.org>
Copyright (C) 2022-2026 Thomas Jones <me@thomasjones.id.au>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>

#include "demo_name.h"

// Length of the well-formed UTF-8 sequence starting at s, or 0 if there is none
// there. Follows RFC 3629's table, so an overlong form, a surrogate and anything
// above U+10FFFF are rejected along with a stray continuation byte. Reads no
// further than the first byte that fails, so a NUL ends the check safely.
static size_t utf8_seq_len(const unsigned char *s) {
    unsigned char lead = s[0];
    unsigned char lo   = 0x80;
    unsigned char hi   = 0xBF;
    size_t len;

    if (lead >= 0xC2 && lead <= 0xDF) {
        len = 2;
    } else if (lead == 0xE0) {
        len = 3;
        lo  = 0xA0; // below is an overlong form
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
        len = 3;
    } else if (lead == 0xED) {
        len = 3;
        hi  = 0x9F; // above is a surrogate
    } else if (lead == 0xF0) {
        len = 4;
        lo  = 0x90; // below is an overlong form
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        len = 4;
    } else if (lead == 0xF4) {
        len = 4;
        hi  = 0x8F; // above is past U+10FFFF
    } else {
        return 0; // ASCII, a continuation byte on its own, or C0/C1/F5..FF
    }

    if (s[1] < lo || s[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < len; i++) {
        if (s[i] < 0x80 || s[i] > 0xBF) {
            return 0;
        }
    }
    return len;
}

void Demo_SanitiseName(char *dst, size_t n, const char *src) {
    const unsigned char *s = (const unsigned char *)src;
    size_t j = 0;

    for (size_t i = 0; s && s[i] && j + 1 < n;) {
        unsigned char c = s[i];
        if (c == '^' && s[i + 1]) { // colour code: skip '^' and the following byte.
            i += 2;
            continue;
        }
        if (c < 0x80) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                c == '_') {
                dst[j++] = (char)c;
            } else if (c == ' ') {
                dst[j++] = '_';
            }
            i++;
            continue;
        }
        size_t len = utf8_seq_len(s + i);
        if (len == 0) { // not a character: drop the byte like any other punctuation.
            i++;
            continue;
        }
        if (j + len >= n) { // the whole character or nothing: never a cut sequence.
            break;
        }
        memcpy(dst + j, s + i, len);
        j += len;
        i += len;
    }
    if (j == 0) {
        dst[j++] = 'x';
    }
    dst[j] = '\0';
}
