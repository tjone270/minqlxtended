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

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "protect.h"

/*
 * The protection the page holding `address` currently carries, as PROT_* flags. Negative if
 * no mapping covers it. Looked up by address, so it answers for qagame too.
 */
int GetPageProtection(pint address) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        return -1;
    }

    int prot = -1;
    char linebuf[8192];
    while (fgets(linebuf, sizeof(linebuf), fp)) {
        pint start, end;
        char flags[32];

        if (sscanf(linebuf, "%" SCNxPTR "-%" SCNxPTR " %31s", &start, &end, flags) != 3) {
            continue;
        }
        if (address < start || address >= end) {
            continue;
        }

        prot = PROT_NONE;
        if (flags[0] == 'r') {
            prot |= PROT_READ;
        }
        if (flags[1] == 'w') {
            prot |= PROT_WRITE;
        }
        if (flags[2] == 'x') {
            prot |= PROT_EXEC;
        }
        break;
    }

    fclose(fp);
    return prot;
}

// Makes every page the write touches writable, remembering what each one held. A write that
// straddles a page boundary needs the whole span. Fails instead of patching if a protection
// can't be read.
int MakeWritable(pint address, size_t length, page_protection_t* saved) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return errno ? errno : EINVAL;
    }

    saved->page_size = page_size;
    saved->start     = address & ~((pint)page_size - 1);
    saved->end       = (address + length + (pint)page_size - 1) & ~((pint)page_size - 1);
    saved->pages     = 0;

    if ((saved->end - saved->start) / (pint)page_size > PROTECT_MAX_PAGES) {
        return EINVAL; // a patch this big is a mistake; raising the page count won't help
    }

    for (pint page = saved->start; page < saved->end; page += (pint)page_size) {
        int prot = GetPageProtection(page);
        if (prot < 0) {
            return ENOENT;
        }

        saved->prot[saved->pages++] = prot;
    }

    if (mprotect((void*)saved->start, saved->end - saved->start, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        return errno;
    }

    return 0;
}

// Puts back what MakeWritable found. Returns the first errno if a page won't go back.
int RestoreProtection(const page_protection_t* saved) {
    int failed = 0;

    for (int i = 0; i < saved->pages; i++) {
        pint page = saved->start + (pint)i * (pint)saved->page_size;

        if (mprotect((void*)page, (size_t)saved->page_size, saved->prot[i]) && !failed) {
            failed = errno;
        }
    }

    return failed;
}
