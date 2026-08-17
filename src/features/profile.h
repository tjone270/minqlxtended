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

#ifndef PROFILE_H
#define PROFILE_H

#include <stdint.h>

// Latency probes for everything added to the game thread, driven by the "qlx_prof" console
// command. No cvar, since sampling costs a clock_gettime per probe. A frame is 25ms at sv_fps 40
// and as little as 5ms once it has been raised. PROF_GIL_WAIT is kept apart: the game thread
// drops the GIL after init, so every dispatcher reacquires it and blocks whenever a worker holds
// it. Dispatches nest, so don't sum the totals. Game thread only, so the counters need no locking.

typedef enum {
    PROF_FRAME_TOTAL = 0, // My_G_RunFrame, engine frame included.
    PROF_GIL_WAIT,        // PyGILState_Ensure, summed across every dispatcher.
    PROF_FRAME_DISPATCH,  // FrameDispatcher: handle_frame, task drain, frame event.
    PROF_DEMO_DISPATCH,   // DispatchFinishedDemos.
    PROF_DEMO_CAPTURE,    // Demo_Capture, once per outgoing message per recorded client.
    PROF_CLIENT_COMMAND,
    PROF_SERVER_COMMAND,
    PROF_SET_CONFIGSTRING,
    PROF_CONSOLE_PRINT,
    PROF_CLIENT_CONNECT,
    PROF_CLIENT_LOADED,
    PROF_CLIENT_DISCONNECT,
    PROF_NEW_GAME,
    PROF_CLIENT_SPAWN,
    PROF_KAMIKAZE_USE,
    PROF_KAMIKAZE_EXPLODE,
    PROF_RCON,
    PROF_CUSTOM_COMMAND,
    PROF_DEMO_FINISHED,
    PROF_PLAYER_DEATH,
    PROF_ROUND_COUNTDOWN,
    PROF_ROUND_START,
    PROF_ROUND_END,
    PROF_GAME_COUNTDOWN,
    PROF_GAME_START,
    PROF_GAME_END,
    PROF_TEAM_SWITCH,
    PROF_ITEM_PICKUP,
    PROF_VOTE_CALLED,
    PROF_VOTE_STARTED,
    PROF_VOTE_ENDED,
    PROF_OBJECTIVE,
    PROF_CHAT,
    PROF_TEAM_SWITCH_ATTEMPT,
    PROF_USERINFO,
    PROF_WEAPON_FIRED, // Gated: no samples unless something has hooked `weapon_fired`.
    PROF_DAMAGE,       // Gated: no samples at all unless something has hooked `damage`.
    PROF_CVAR_CHANGED, // Gated: no samples unless something has hooked `cvar_changed`.
    PROF_GAME_EVENTS,  // GameEvents_Frame, the whole per-frame state poll.
    PROF_COUNT
} prof_id_t;

// Off by default.
extern int prof_enabled;

uint64_t Profile_Now(void);
void Profile_Record(prof_id_t id, uint64_t ns);
void Profile_SetEnabled(int enabled);
void Profile_Reset(void);
void Profile_Report(void);

// A zero timestamp means no sampling.
#define PROF_BEGIN(v) uint64_t v = prof_enabled ? Profile_Now() : 0
#define PROF_END(id, v)                                \
    do {                                               \
        if (v) {                                       \
            Profile_Record((id), Profile_Now() - (v)); \
        }                                              \
    } while (0)

#endif /* PROFILE_H */
