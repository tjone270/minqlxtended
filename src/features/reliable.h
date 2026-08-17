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

#ifndef RELIABLE_H
#define RELIABLE_H

#include "engine/quake_common.h"

/*
 * Backpressure for the engine's reliable server-command channel. Every client_t has a 64-slot
 * ring of pending commands (client_t::reliableCommands, indexed by reliableSequence & 63).
 * SV_AddServerCommand drops a client the moment reliableSequence - reliableAcknowledge reaches
 * 65, and cgame aborts with "a reliable command was cycled out" once 64 newer commands arrive
 * before it reaches the snapshot an older one was attached to.
 *
 * This sits in My_SV_SendServerCommand, spreading a burst over several frames and merging
 * consecutive broadcast prints into as few commands as the engine's 1022-character limit allows.
 * Commands the client's state machine depends on are never touched, configstrings included, so a
 * configstring flood is the one burst this cannot pace. "qlx_reliable" can only measure it.
 *
 * These functions re-enter each other, despite being game-thread only. Releasing a command
 * reaches Com_Printf, which is hooked, so a console_print handler runs from inside the flush and
 * can call back in or drop a client. Anything walking the queue has to assume it can be
 * compacted underneath it.
 */

void Reliable_Init(void); // register cvars; safe to call more than once

// Called for every outgoing reliable command. qtrue means this module has taken
// ownership of it and the caller must not send it. Game thread only, as are the rest.
qboolean Reliable_Intercept(client_t* cl, const char* cmd);

void Reliable_Flush(void);          // release what is due; call once per game frame
void Reliable_ClientGone(int slot); // forget anything still queued for that slot
void Reliable_Reset(void);          // map change: queued output is stale, drop it
void Reliable_Report(void);         // "qlx_reliable" console output

// What Reliable_Status snapshots: the counters Reliable_Report prints, plus the live
// worst backlog, so a plugin about to mass-message can pace itself against the ring.
typedef struct {
    int enabled;       // the guard cvar, and the engine pointers it needs
    int watermark;     // backlog depth where pacing starts
    int burst;         // commands released per frame once pacing has started
    int waiting;       // commands held in the queue right now
    unsigned queued;   // commands held back at least one frame, since map start
    unsigned merged;   // broadcast prints folded into a preceding batch
    unsigned bypassed; // sent straight through, unpaced, because the queue was full
    int backlog;       // deepest live reliableSequence - reliableAcknowledge right now
    int worst_backlog; // deepest ever seen this map
    int worst_slot;    // the client that reached worst_backlog, -1 for none yet
} reliable_status_t;

void Reliable_Status(reliable_status_t* out); // game thread only, like the rest

#endif /* RELIABLE_H */
