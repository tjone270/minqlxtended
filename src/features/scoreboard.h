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
 * Tiered team scoreboards, kept under the size the engine will actually send.
 *
 * There are two size limits and they do not line up. A qagame builder stops once the client rows
 * alone pass 0x3FF, and SelectScoreboardMessage then sends the "smscores" stub instead.
 * SV_SendServerCommand checks the whole command, and throws away anything 1023 bytes or longer
 * without printing anything. The builders never count their own header, which is 37 fields in
 * CTF, so a scoreboard the builder was happy with can still reach no one.
 *
 * We use the engine's limit: header plus rows, under qlx_scoreboardBudget. Row width is worked
 * out from the same fields each builder prints, and rows are cut back only until it fits:
 *
 *     1. spectator stats blanked
 *     2. team ranks past qlx_scoreboardFullPerTeam blanked
 *     3. team ranks past qlx_scoreboardLightPerTeam dropped
 *     4. spectator rows dropped
 *     5. team rows dropped from the bottom, both teams evenly, to qlx_scoreboardMinPerTeam
 *
 * The row belonging to whoever asked for the scoreboard is always kept, so a player never gets
 * one they are missing from.
 *
 * We measure the header instead of working it out. It is level_locals_t counters that get wider
 * as a match runs, and TDM, CTF and Freeze Tag zero half of them depending on which team asked.
 * The largest one seen so far for that builder is what the next budget uses.
 *
 * If the stub turns up anyway, our widths were wrong, so we drop it and build the scoreboard
 * again with nothing blanked and fewer rows.
 *
 * Game thread only, and only across one synchronous call.
 */

void Scoreboard_Init(void); // register cvars; safe to call more than once

/*
 * Wrapped around the real SelectScoreboardMessage. FilterCommand sees the command as the game
 * module wrote it, before Python does, and returns qtrue to have it thrown away. EndTrim puts
 * back whatever is still armed. TakeRetry tells you, once, that BeginFallback still has a
 * scoreboard to rebuild.
 */
void Scoreboard_BeginTrim(gentity_t* ent);
void Scoreboard_BeginFallback(gentity_t* ent);
qboolean Scoreboard_FilterCommand(const char* cmd);
void Scoreboard_EndTrim(void);
qboolean Scoreboard_TakeRetry(void);

void Scoreboard_Reset(void);  // map change: counters and any armed state are stale
void Scoreboard_Report(void); // "qlx_scoreboard" console output
void Scoreboard_Verify(void); // "qlx_scoreboard verify": the widths we expect for the current roster

#endif /* SCOREBOARD_H */
