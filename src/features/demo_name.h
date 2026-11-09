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

#ifndef DEMO_NAME_H
#define DEMO_NAME_H

#include <stddef.h>

// Copies the part of src that can go into a demo filename into dst (at most n
// bytes, always NUL-terminated). Colour codes ("^N") are stripped, a space
// becomes '_', ASCII letters, digits, '-' and '_' pass through, and so does any
// well-formed UTF-8 sequence, whole or not at all - a player called "Петров"
// keeps that name on disk. Everything else (punctuation, '/', '.', control
// bytes, stray or malformed UTF-8 bytes) is dropped. An empty result is "x" so
// the name is never missing from the filename.
void Demo_SanitiseName(char *dst, size_t n, const char *src);

#endif /* DEMO_NAME_H */
