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

#include <pthread.h>
#include <string.h>

#include "common.h"
#include "maps_parser.h"
#include "engine/quake_common.h"

// See common.h for what this is for and why it needs no locking.
static pthread_t game_thread;
static int game_thread_known;

void NoteGameThread(void) {
    game_thread       = pthread_self();
    game_thread_known = 1;
}

int OnGameThread(void) {
    return game_thread_known && pthread_equal(pthread_self(), game_thread);
}

void* PatternSearch(void* address, size_t length, const char* pattern, const char* mask) {
    size_t masklen = strlen(mask);
    // Stop before the pattern would run past the end of the region.
    for (size_t i = 0; i + masklen <= length; i++) {
        for (size_t j = 0; mask[j]; j++) {
            if (mask[j] == 'X' && pattern[j] != ((char*)address)[i + j]) {
                break;
            } else if (mask[j + 1]) {
                continue;
            }

            return (void*)(((pint)address) + i);
        }
    }
    return NULL;
}

void* PatternSearchModule(module_info_t* module, const char* pattern, const char* mask) {
    void* res = NULL;
    for (int i = 0; i < module->entries; i++) {
        if (!(module->permissions[i] & PG_READ)) {
            continue;
        }
        size_t size = module->address_end[i] - module->address_start[i];
        res         = PatternSearch((void*)module->address_start[i], size, pattern, mask);
        if (res) {
            break;
        }
    }

    return res;
}
