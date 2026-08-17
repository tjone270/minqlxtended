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
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common.h"
#include "patches.h"
#include "protect.h"
#include "engine/quake_common.h"

_Static_assert(sizeof(PTRN_VOTE_CLIENTKICK_FIX) == sizeof(MASK_VOTE_CLIENTKICK_FIX) &&
                   sizeof(PRE_VOTE_CLIENTKICK_FIX) == sizeof(MASK_VOTE_CLIENTKICK_FIX),
               "callvote-clientkick pattern, mask and pre-image must be the same length");

int patch_by_mask(pint offset, char* pattern, char* mask, int* written) {
    size_t length = strlen(mask);

    *written = 0;

    page_protection_t saved;
    int res = MakeWritable(offset, length, &saved);
    if (res) {
        return res;
    }

    for (size_t i = 0; i < length; i++) {
        if (mask[i] != 'X') {
            continue;
        }

        *(int8_t*)(offset + i) = pattern[i];
    }
    *written = 1;

    return RestoreProtection(&saved);
}

static void* find_in_vm(const void* needle, size_t length) {
    const char* base = (const char*)VM_SEARCH_START;
    for (size_t i = 0; i + length <= VM_SEARCH_LENGTH; i++) {
        if (!memcmp(base + i, needle, length)) {
            return (void*)(base + i);
        }
    }

    return NULL;
}

static void fix_log_format(const char* what, const char* original, const char* replacement) {
    size_t length     = strlen(original) + 1;
    size_t fix_length = strlen(replacement) + 1;

    if (fix_length > length) {
        DebugPrint("ERROR: The %s replacement is longer than what it replaces. Skipping...\n", what);
        return;
    }

    char* at = find_in_vm(original, length);
    if (!at) {
        if (!find_in_vm(replacement, fix_length)) {
            DebugPrint("WARNING: Unable to find the %s log format. Skipping...\n", what);
        }
        return;
    }

    page_protection_t saved;
    int res = MakeWritable((pint)at, length, &saved);
    if (res) {
        DebugPrint("ERROR: mprotect() returned %d. Skipping the %s patch...\n", res, what);
        return;
    }

    memcpy(at, replacement, fix_length);
    memset(at + fix_length, 0, length - fix_length);

    res = RestoreProtection(&saved);
    if (res) {
        DebugPrint("WARNING: could not restore protection after the %s patch: %d\n", what, res);
    }

    DebugPrint("Patched the %s log format.\n", what);
}

static void bad_userinfo_log_fix(void) {
    fix_log_format("deformed userinfo kick", STR_BAD_USERINFO_KICK, STR_BAD_USERINFO_KICK_FIX);
    fix_log_format("deformed userinfo warning", STR_BAD_USERINFO_KEEP, STR_BAD_USERINFO_KEEP_FIX);
}

static int already_patched(pint offset, const char* pattern, const char* mask) {
    for (size_t i = 0; mask[i]; i++) {
        if (mask[i] == 'X' && *(const int8_t*)(offset + i) != pattern[i]) {
            return 0;
        }
    }

    return 1;
}

void vote_clientkick_fix(void) {
    pint        offset = ADDR_VOTE_CLIENTKICK_FIX;
    const char* pre    = PRE_VOTE_CLIENTKICK_FIX;
    size_t      length = strlen(MASK_VOTE_CLIENTKICK_FIX);

    if (memcmp((const void*)offset, pre, length)) {
        if (already_patched(offset, PTRN_VOTE_CLIENTKICK_FIX, MASK_VOTE_CLIENTKICK_FIX)) {
            return;
        }
        // A fixed +0x11C8 into the body, so a build that moved anything ahead of it lands
        // mid-instruction. Refuse rather than write.
        DebugPrint("WARNING: the callvote-clientkick patch site at %p does not match this qagame. "
                   "Skipping the patch.\n",
                   (void*)offset);
        return;
    }

    int written;
    int res = patch_by_mask(offset, PTRN_VOTE_CLIENTKICK_FIX, MASK_VOTE_CLIENTKICK_FIX, &written);
    if (res && !written) {
        DebugPrint("ERROR: mprotect() returned %d. Skipping the callvote-clientkick patch...\n", res);
    } else if (res) {
        DebugPrint("WARNING: the callvote-clientkick patch was applied, but restoring page "
                   "protection returned %d.\n",
                   res);
    }
}

void patch_vm(void) {
    vote_clientkick_fix();
    bad_userinfo_log_fix();
}
