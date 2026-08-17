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

#ifndef MAPS_PARSER_H
#define MAPS_PARSER_H

// Memory layout for a single module, read out of /proc/self/maps.

// For pint, which the addresses below are, and the x86-64 guard that makes it the right width.
#include "common.h"

// Permission flags. The two last are mutually exclusive.
#define PG_READ 1
#define PG_WRITE 2
#define PG_EXECUTE 4
#define PG_PRIVATE 8
#define PG_SHARED 16

typedef struct {
    char name[512];
    char path[4096];
    int entries;
    int permissions[128];
    pint address_start[128];
    pint address_end[128];
} module_info_t;

int GetModuleInfo(module_info_t* module_info);

// Bounds check for addresses derived from an instruction's displacement. Covers .bss,
// which GetModuleInfo's entry list does not.
int InImage(pint address);

// The qagame equivalent, defined in dllmain.c beside the module base it measures from.
int InVm(pint address);

// Defined in misc.c beside PatternSearch, declared here because module_info_t is what it takes.
void* PatternSearchModule(module_info_t* module, const char* pattern, const char* mask);

#endif /* MAPS_PARSER_H */
