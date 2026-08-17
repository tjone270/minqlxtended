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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // for dl_iterate_phdr
#endif

#include <inttypes.h>
#include <link.h>
#include <stdio.h>
#include <string.h>

#include "maps_parser.h"

// static, since the library is built without -fvisibility=hidden and a global named `fmt`
// is open to interposition. Offset, device and inode are matched and discarded, leaving
// four conversions: the two addresses, the perms and the pathname.
static const char fmt[] = ("%" SCNxPTR "-%" SCNxPTR " %31s %*x %*x:%*x %*u %4095[^\n]");

// Pass a module_info_t with its name initialised, get it full of info back. Returns a negative
// number on error, otherwise the number of pages found under that module name.
int GetModuleInfo(module_info_t* module_info) {
    int ret = 0;
    pint start, end;
    char path[4096], linebuf[8192];

    // Zeroed up front so an error return can't leave a caller's stack-allocated module_info
    // holding an indeterminate count for PatternSearchModule to loop over.
    module_info->entries = 0;

    if (!strlen(module_info->name)) {
        return -1;
    }

    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        return -3;
    }

    size_t max_entries = sizeof(module_info->address_start) / sizeof(module_info->address_start[0]);
    while (fgets(linebuf, sizeof(linebuf), fp) != 0) {
        // Zeroed every line: %31s writes only as far as the perms field goes, and the
        // reads below index flags[3].
        char flags[32] = {0};

        // An anonymous mapping has no pathname, so the trailing conversion fails and the
        // count comes back short. Unchecked, the executable's .bss inherits the preceding
        // qzeroded.x64 path and gets byte-scanned for function prologues.
        path[0] = '\0';
        if (sscanf(linebuf, fmt, &start, &end, flags, path) != 4) {
            continue;
        }

        size_t pathlen = strlen(path);
        if (!pathlen) {
            continue;
        }

        int slash = -1;
        for (size_t i = 0; i < pathlen; i++) {
            if (path[i] == '/') {
                slash = i;
            }
        }

        if (slash == -1) {
            continue;
        }

        if (strcmp(module_info->name, &path[slash + 1])) {
            continue;
        }

        // Return error if there's an ambiguity. Could happen if two modules
        // are different, but have the same filename.
        // TODO: Add option to pass the path instead of name to avoid this.
        if (ret && strcmp(path, module_info->path)) {
            fclose(fp);
            return -2;
        }

        if (!ret) { // Only once.
            strcpy(module_info->path, path);
        }

        // Stop before overflowing the fixed-size entry arrays.
        if ((size_t)ret >= max_entries) {
            break;
        }

        // Addresses
        module_info->address_start[ret] = start;
        module_info->address_end[ret]   = end;

        // Permissions
        module_info->permissions[ret] = 0;
        if (flags[0] == 'r') {
            module_info->permissions[ret] |= PG_READ;
        }
        if (flags[1] == 'w') {
            module_info->permissions[ret] |= PG_WRITE;
        }
        if (flags[2] == 'x') {
            module_info->permissions[ret] |= PG_EXECUTE;
        }
        if (flags[3] == 'p') {
            module_info->permissions[ret] |= PG_PRIVATE;
        }
        if (flags[3] == 's') {
            module_info->permissions[ret] |= PG_SHARED;
        }

        ret++;
    }
    fclose(fp);

    module_info->entries = ret;
    return ret;
}

typedef struct {
    pint address;
    int found;
} image_probe_t;

static int ProbeImage(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    image_probe_t* probe = data;

    // The main program is the one entry with an empty name, and under LD_PRELOAD that is
    // qzeroded.
    if (info->dlpi_name != NULL && info->dlpi_name[0] != '\0') {
        return 0;
    }

    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)* phdr = &info->dlpi_phdr[i];
        if (phdr->p_type != PT_LOAD) {
            continue;
        }
        pint start = (pint)(info->dlpi_addr + phdr->p_vaddr);
        if (probe->address >= start && probe->address < start + phdr->p_memsz) {
            probe->found = 1;
            break;
        }
    }

    return 1;
}

// Is this address part of the executable's image? Measured against the PT_LOAD headers using
// p_memsz: p_filesz stops at the end of .data and the loader maps .bss anonymously, so
// GetModuleInfo above cannot record it, and the engine globals we chase all live there.
int InImage(pint address) {
    image_probe_t probe = {address, 0};
    dl_iterate_phdr(ProbeImage, &probe);
    return probe.found;
}
