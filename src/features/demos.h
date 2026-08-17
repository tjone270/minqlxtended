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

#ifndef DEMOS_H
#define DEMOS_H

#include "engine/quake_common.h"

// A segment the writer thread has finished with, handed back to the game thread so the
// completion is reported to Python from a context that can take the GIL.
typedef struct {
    int slot;
    int discarded;  // held only a gamestate and was removed again; nothing at path
    int failed;     // open/write/rename error; path is the .part left on disk
    uint32_t gen;   // demo_gen[slot] at the time the segment was opened
    long bytes;     // bytes written to the file
    // 512 for the final name, plus room for the ".part" suffix the failure paths report.
    char path[520];
} demo_finished_t;

void Demo_Init(void);                            // register + cache cvars
void Demo_Capture(msg_t *msg, client_t *client); // called for every S2C message
void Demo_ClientDisconnect(int slot);            // finalise a client's demo
void Demo_CloseAll(void);                        // finalise every open demo

// Demo_CloseAll, but waits (bounded) for the writer to finish with the files rather than only
// queueing the request. For the shutdown path, where returning early leaves the process exiting
// from under the writer and the segments still .part.
void Demo_DrainFinalise(void);

// Per-slot override of sv_demoRecord: mode 1 always records, -1 never, 0 follows the
// cvar. qfalse if the slot is out of range. Game thread only, as are the four below.
qboolean Demo_Request(int slot, int mode);
void Demo_ClearRequests(void);       // server shutdown only; see the note on the definition
int Demo_GetRequest(int slot);       // current override mode for the slot
qboolean Demo_IsRecording(int slot); // a segment is currently open for the slot
const char *Demo_GetPath(int slot);  // final name of the open segment, else NULL
void Demo_AbandonSlot(int slot, uint32_t gen); // stop capturing a segment the writer lost

// Completion queue. Demo_PendingFinished is a lock-free count so the frame hook can skip
// the rest when there is nothing to report. Game thread only.
unsigned Demo_PendingFinished(void);
qboolean Demo_PollFinished(demo_finished_t *out);
unsigned Demo_TakeDroppedCount(void); // dropped-on-overflow count, then clears it

#endif /* DEMOS_H */
