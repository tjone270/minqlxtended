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

#ifndef CONSOLE_COMMAND_H
#define CONSOLE_COMMAND_H

#include "engine/quake_common.h"

// Console commands from Python. Most run where they were asked for, through Cmd_ExecuteString.
// Two can't: one that reloads the game module, since Python is reached through hooks under
// qagame's vmMain and would return into a dlclose'd module through a rewound trampoline pool,
// and one from a worker thread, since cmd_text belongs to the game thread. Both go to the buffer.

// Every name the engine registers, so one sharing a handler with a command known to reload
// the game module is treated as reloading too. From My_Cmd_AddCommand.
void ConsoleCommand_NoteRegistration(const char *name, void *function);

// Any thread. Runs cmd now where that is safe and hands it to the engine's command buffer
// where it isn't. qfalse if it was refused: longer than the tokeniser takes, or the queue was
// full.
qboolean ConsoleCommand_Run(const char *cmd);

// Hands the queue to the command buffer, oldest first, up to a per-frame byte budget. Game
// thread only, once per frame.
void ConsoleCommand_Drain(void);

// What Cmd_ExecuteString is running on this thread, or NULL. My_SV_SpawnServer and
// My_SV_Shutdown use it to catch a command that got past the classifier: see the note there.
const char *ConsoleCommand_Executing(void);

// Forgets it, leaving the depth alone. For the backstops that read it: once one has deferred a
// command, that name has been dealt with, and a Com_Error out of Cmd_ExecuteString skips the
// clear at the end of ConsoleCommand_Run. Without this a stale name defers every later map
// change and every shutdown, so quit and killserver stop working.
void ConsoleCommand_ForgetExecuting(void);

// The text the engine last tokenised, recorded from My_Cmd_TokenizeString. Running a command in
// place re-tokenises, and the engine handler we were called from is very likely holding argv
// across the call: SV_Kick_f compares Cmd_Argv(1) against "all" after a Com_Printf that reaches
// Python. ConsoleCommand_Run uses this to put the tokeniser back the way it found it.
void ConsoleCommand_NoteTokenized(const char *text);

#endif /* CONSOLE_COMMAND_H */
