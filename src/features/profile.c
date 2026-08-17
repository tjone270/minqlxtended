/*
Copyright (C) 2026 Thomas Jones <me@thomasjones.id.au>

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
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "profile.h"
#include "engine/quake_common.h"

int prof_enabled = 0;

typedef struct {
    uint64_t count;
    uint64_t total_ns;
    uint64_t max_ns;
} prof_slot_t;

static prof_slot_t prof_slots[PROF_COUNT];
// Start of the window we're sampling, 0 when we aren't, plus what earlier windows banked.
// Kept apart so idle time between "off" and reading the report doesn't skew per sec.
static uint64_t prof_since_ns;
static uint64_t prof_elapsed_ns;

// Same order as prof_id_t.
static const char* const prof_names[PROF_COUNT] = {
    "frame (incl. engine)",
    "GIL wait",
    "frame dispatch",
    "demo dispatch",
    "demo capture",
    "client_command",
    "server_command",
    "set_configstring",
    "console_print",
    "player_connect",
    "player_loaded",
    "player_disconnect",
    "new_game",
    "player_spawn",
    "kamikaze_use",
    "kamikaze_explode",
    "rcon",
    "custom_command",
    "demo_finished",
    "player_death",
    "round_countdown",
    "round_start",
    "round_end",
    "game_countdown",
    "game_start",
    "game_end",
    "team_switch",
    "item_pickup",
    "vote_called",
    "vote_started",
    "vote_ended",
    "objective",
    "chat",
    "team_switch_attempt",
    "userinfo",
    "weapon_fired",
    "damage",
    "cvar_changed",
    "game event poll",
};

// prof_names and prof_id_t are parallel arrays, so a probe added to one and not the other
// mislabels every entry after it.
_Static_assert(sizeof(prof_names) / sizeof(*prof_names) == PROF_COUNT,
               "prof_names must stay in step with prof_id_t");

uint64_t Profile_Now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void Profile_Record(prof_id_t id, uint64_t ns) {
    if (id >= PROF_COUNT) {
        return;
    }

    prof_slot_t* slot = &prof_slots[id];
    slot->count++;
    slot->total_ns += ns;
    if (ns > slot->max_ns) {
        slot->max_ns = ns;
    }
}

void Profile_Reset(void) {
    memset(prof_slots, 0, sizeof(prof_slots));
    prof_elapsed_ns = 0;
    prof_since_ns   = prof_enabled ? Profile_Now() : 0;
}

// How long we've actually been sampling, over however many on/off windows.
static uint64_t prof_elapsed(void) {
    uint64_t elapsed = prof_elapsed_ns;
    if (prof_since_ns) {
        elapsed += Profile_Now() - prof_since_ns;
    }

    return elapsed;
}

void Profile_SetEnabled(int enabled) {
    if (enabled) {
        // Starting always means a fresh measurement. Profile_Reset checks prof_enabled to
        // decide whether to start the clock, so set it first.
        prof_enabled = 1;
        Profile_Reset();
    } else {
        if (!prof_enabled) {
            return;
        }
        prof_enabled = 0;
        // Bank the window that just ended and stop the clock, so the counters can be
        // read later on without the idle time skewing anything.
        prof_elapsed_ns = prof_elapsed();
        prof_since_ns   = 0;
    }
}

// Prints through Com_Printf with literal formats. After hooking that symbol holds the
// trampoline to the engine's own, so the report can't recurse into console_print.
void Profile_Report(void) {
    uint64_t elapsed_ms = prof_elapsed() / 1000000ull;

    ENGINE_PRINTF("minqlxtended profiler: %s, %llu.%03llus sampled\n", prof_enabled ? "on" : "off",
               (unsigned long long)(elapsed_ms / 1000), (unsigned long long)(elapsed_ms % 1000));
    ENGINE_PRINTF("%-22s %10s %8s %12s %10s %10s\n", "probe", "count", "per sec", "total ms", "mean us", "max us");

    for (int i = 0; i < PROF_COUNT; i++) {
        const prof_slot_t* slot = &prof_slots[i];
        if (!slot->count) {
            continue;
        }

        // Fixed-point throughout, since we don't hand floats to the engine's printf.
        uint64_t per_sec_x10 = elapsed_ms ? (slot->count * 10000ull / elapsed_ms) : 0;
        uint64_t mean_ns     = slot->total_ns / slot->count;

        ENGINE_PRINTF("%-22s %10llu %6llu.%1llu %8llu.%03llu %7llu.%02llu %7llu.%02llu\n", prof_names[i],
                   (unsigned long long)slot->count, (unsigned long long)(per_sec_x10 / 10), (unsigned long long)(per_sec_x10 % 10),
                   (unsigned long long)(slot->total_ns / 1000000ull), (unsigned long long)((slot->total_ns % 1000000ull) / 1000ull),
                   (unsigned long long)(mean_ns / 1000ull), (unsigned long long)((mean_ns % 1000ull) / 10ull),
                   (unsigned long long)(slot->max_ns / 1000ull), (unsigned long long)((slot->max_ns % 1000ull) / 10ull));
    }

    if (!prof_enabled) {
        ENGINE_PRINTF("Profiler is off. Use \"qlx_prof on\" to start sampling.\n");
    }
    ENGINE_PRINTF("Read the max column first; it is the jitter that costs frames. Totals for nested\n"
               "dispatches overlap, so do not sum them. \"frame\" includes the engine's own work.\n");
}
