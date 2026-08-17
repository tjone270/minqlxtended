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

#ifndef GAME_EVENTS_H
#define GAME_EVENTS_H

// Events derived by watching game module state instead of hooking a function. The round and game
// lifecycles, votes and team_switch are all readable straight out of level_locals_t, so this
// compares a handful of ints once per frame and only enters Python when something changed. Those
// fields are the source; SetWarmupTime and CS_ROUND_WARMUP are built from them.

// The objective counters in pers.teamState that CheckTeams watches, in report order. See the
// Objective enum in _enums.py for the names.
typedef enum {
    OBJ_CAPTURE = 0,   // captures
    OBJ_RETURN,        // flagrecovery
    OBJ_ASSIST,        // assists
    OBJ_BASE_DEFENSE,  // basedefense
    OBJ_CARRIER_DEFENSE, // carrierdefense
    OBJ_FRAG_CARRIER,  // fragcarrier
    OBJ_COUNT
} objective_t;

// Called once per frame from My_G_RunFrame, after the engine's own frame has run so
// the state we read is post-tick.
void GameEvents_Frame(void);

// Drops all cached state. Called on map load and on map_restart, since level and
// g_entities are re-resolved and every client is re-seated.
void GameEvents_Reset(void);

#endif /* GAME_EVENTS_H */
