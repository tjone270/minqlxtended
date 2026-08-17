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

#ifndef SCOREBOARD_H
#define SCOREBOARD_H

#include "engine/quake_common.h"

/*
 * Tiered team scoreboards. Every scoreboard builder in qagame walks level->sortedClients and
 * returns NULL once the client data passes 0x3FF, the engine's reliable command limit (see
 * RELIABLE_CMD_MAX in reliable.c); SelectScoreboardMessage then falls back to the "smscores"
 * stub and everyone loses their stats. The builder bails before a byte is sent, so this changes
 * what it sees: for the length of one SelectScoreboardMessage call, level->sortedClients is
 * rewritten and some per-client stats blanked, then put back.
 *
 * Ranking is per team in sortedClients order, TEAM_RED and TEAM_BLUE only; spectators are never
 * ranked or trimmed. team_gametype() in scoreboard.c has the eligible list.
 *
 *     to qlx_scoreboardFullPerTeam    untouched
 *     to qlx_scoreboardLightPerTeam   stats blanked
 *     beyond that                     dropped
 *
 * Game thread only, and only across one synchronous call.
 */

void Scoreboard_Init(void); // register cvars; safe to call more than once

// Wrapped around the real SelectScoreboardMessage. NoteCommand restores the moment the game
// module hands the built scoreboard to the engine, before any of it is dispatched to Python, and
// takes the command as the game module wrote it. EndTrim restores it if nothing was sent at all.
void Scoreboard_BeginTrim(void);
void Scoreboard_NoteCommand(const char* cmd);
void Scoreboard_EndTrim(void);

void Scoreboard_Reset(void);  // map change: counters and any armed state are stale
void Scoreboard_Report(void); // "qlx_scoreboard" console output

#endif /* SCOREBOARD_H */
