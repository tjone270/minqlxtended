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

#ifndef PROTECT_H
#define PROTECT_H

#include "common.h"

// Making engine memory writable long enough to patch it. MakeWritable records what each page held
// so RestoreProtection can put it back afterwards, leaving no engine page writable and executable
// for the life of the process. Saved per page, since a patch can straddle two differently
// protected regions. The only callers are patches.c and simple_hook.c.
#define PROTECT_MAX_PAGES 4

typedef struct {
    pint start;
    pint end;
    long page_size;
    int pages;
    int prot[PROTECT_MAX_PAGES];
} page_protection_t;

int GetPageProtection(pint address);
int MakeWritable(pint address, size_t length, page_protection_t* saved);
int RestoreProtection(const page_protection_t* saved);

#endif /* PROTECT_H */
