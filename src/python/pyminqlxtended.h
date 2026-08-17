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

#ifndef PYMINQLXTENDED_H
#define PYMINQLXTENDED_H

#define PYTHON_FILENAME L"python3"

#if DEBUG
#define CORE_MODULE "minqlxtended_debug.zip"
#else
#define CORE_MODULE "minqlxtended.zip"
#endif

#include <Python.h>

#include "engine/quake_common.h"

// Whether initialization worked.
typedef enum {
    PYM_SUCCESS,
    PYM_PY_INIT_ERROR,
    PYM_MAIN_SCRIPT_ERROR,
    PYM_ALREADY_INITIALIZED
} PyMinqlxtended_InitStatus_t;

// Used primarily in Python, but defined here and added using PyModule_AddIntMacro().
enum {
    RET_NONE,
    RET_STOP,       // Stop execution of event handlers within Python.
    RET_STOP_EVENT, // Only stop the event, but let other handlers process it.
    RET_STOP_ALL,   // Stop execution at an engine level. SCARY STUFF!
    RET_USAGE       // Used for commands. Replies to the channel with a command's usage.
};

enum {
    PRI_HIGHEST,
    PRI_HIGH,
    PRI_NORMAL,
    PRI_LOW,
    PRI_LOWEST
};

// Called exactly once, from InitializeStatic. There is no counterpart; the interpreter is
// never torn down. See the comment at the end of PyMinqlxtended_Initialize.
PyMinqlxtended_InitStatus_t PyMinqlxtended_Initialize(void);

/* Event handlers, one PyObject pointer per event. register_handler() writes these slots from
 * any thread, so a dispatcher must load its slot under the GIL and hold a reference for the
 * call. A bare read outside the GIL is only a fast-path hint. */
typedef struct {
    char* name;
    PyObject** handler;
} handler_t;
extern PyObject* client_command_handler;
extern PyObject* server_command_handler;
extern PyObject* client_connect_handler;
extern PyObject* client_loaded_handler;
extern PyObject* client_disconnect_handler;
extern PyObject* frame_handler;
extern PyObject* new_game_handler;
extern PyObject* spawn_server_handler;
extern PyObject* set_configstring_handler;
extern PyObject* rcon_handler;
extern PyObject* console_print_handler;
extern PyObject* client_spawn_handler;

extern PyObject* kamikaze_use_handler;
extern PyObject* kamikaze_explode_handler;

extern PyObject* demo_finished_handler;

// Events sourced from the game module.
extern PyObject* player_death_handler;
extern PyObject* round_countdown_handler;
extern PyObject* round_start_handler;
extern PyObject* round_end_handler;
extern PyObject* game_countdown_handler;
extern PyObject* game_start_handler;
extern PyObject* game_end_handler;
extern PyObject* team_switch_handler;
extern PyObject* item_pickup_handler;
extern PyObject* vote_called_handler;
extern PyObject* vote_started_handler;
extern PyObject* vote_ended_handler;
extern PyObject* objective_handler;
extern PyObject* chat_handler;
extern PyObject* team_switch_attempt_handler;
extern PyObject* userinfo_handler;

// Gated, like damage. FireWeapon runs on every shot, so Python only arms the slot while
// the event has hooks.
extern PyObject* weapon_fired_handler;

/* Gated: EventDispatcher arms it on the event's first hook and NULLs it on the last. See
 * EventDispatcher.gated_handler in _events.py. */
extern PyObject* damage_handler;

/*
 * Gated too. My_Cvar_Set2 has to copy the old value out before the engine frees it, which
 * costs a lookup and a copy on every set.
 */
extern PyObject* cvar_changed_handler;

// Custom console command handler. These are commands added through Python that can be used
// from the console or using RCON.
extern PyObject* custom_command_handler;

// Tells player_info not to return None inside My_ClientConnect, which dispatches before
// the real ClientConnect moves the connection state off CS_FREE. Same for My_SV_DropClient.
extern int allow_free_client;

/* Releases the GIL at the end of a call into Python, flushing any exception first. Every
 * dispatcher exits through this, and so must anything else pairing with PyGILState_Ensure on
 * the game thread: a leaked error indicator survives into the next Ensure. */
void DispatcherRelease(PyGILState_STATE gstate);

/* Dispatchers, called from the hooks. Return values often decide what reaches the engine,
 * so a handler can filter chat, rewrite a userinfo command, or drop a broken UTF sequence
 * before it reaches a client. */
char* ClientCommandDispatcher(int client_id, char* cmd);
char* ServerCommandDispatcher(int client_id, char* cmd);
void FrameDispatcher(void);
char* ClientConnectDispatcher(int client_id, int is_bot);
int ClientLoadedDispatcher(int client_id);
void ClientDisconnectDispatcher(int client_id, const char* reason);
void SpawnServerDispatcher(void);
void NewGameDispatcher(int restart);
char* SetConfigstringDispatcher(int index, char* value);
void RconDispatcher(const char* cmd);
char* ConsolePrintDispatcher(char* cmd);
void ClientSpawnDispatcher(int client_id);

void KamikazeUseDispatcher(int client_id);
void KamikazeExplodeDispatcher(int client_id, int is_used_on_demand);
void DemoFinishedDispatcher(int client_id, const char* path, long bytes, int discarded, int failed);

/* Events read out of the game module. killer_id is -1 when no client is responsible, mod is a
 * raw meansOfDeath_t and the team arguments are raw team_t values. RoundEndDispatcher carries
 * the number round_start reported. TeamSwitchDispatcher returns 0 when a handler cancelled. */
void PlayerDeathDispatcher(int victim_id, int killer_id, int mod);
void RoundCountdownDispatcher(int round_number);
void RoundStartDispatcher(int round_number);
void RoundEndDispatcher(int round_number, int winning_team, int time_ms);
void GameCountdownDispatcher(void);
void GameStartDispatcher(void);
void GameEndDispatcher(int aborted);
int TeamSwitchDispatcher(int client_id, int old_team, int new_team);
void ItemPickupDispatcher(int client_id, const char* item_name);

/*
 * A player calling a vote, from the Cmd_CallVote_f hook. vote and args are the engine's own
 * tokenisation, with args "" when the vote takes none. Returns 0 when a handler cancelled.
 */
int VoteCalledDispatcher(int client_id, const char* vote, const char* args);

/* The vote lifecycle, from the frame poll in game_events.c: qagame inlines the `vote` command
 * and the resolution, so there is nothing to hook. caller_id is -1 when the engine started the
 * vote. vote_string is the raw level->voteString; yes and no are the tallies from the last
 * frame the vote was live, since the engine clears the vote configstrings as it resolves. */
void VoteStartedDispatcher(int caller_id, const char* vote_string);
void VoteEndedDispatcher(int passed, const char* vote_string, int yes, int no);

/* A player's objective counter going up: a capture, a return, an assist, a defend. From the
 * frame poll. count is the new total, so a jump of two arrives as one event. */
void ObjectiveDispatcher(int client_id, int kind, int count);

/* Chat, from the G_Say hook. mode is SAY_ALL, SAY_TEAM or SAY_TELL; target_id is the recipient
 * for a tell and -1 otherwise. The text keeps the engine's quotes and colour codes. Returns 0
 * when a handler cancelled. */
int ChatDispatcher(int client_id, int target_id, int mode, const char* text);

/*
 * A player trying to join a team, from the SetTeam hook. target is the raw argument
 * ("red", "s", "follow1"), old_team a raw team_t. Returns 0 to prevent the switch.
 */
int TeamSwitchAttemptDispatcher(int client_id, int old_team, const char* target);

// A shot fired. weapon is the raw ps.weapon / WP_* value. Gated; cannot cancel.
void WeaponFiredDispatcher(int client_id, int weapon);

/*
 * A client changing its userinfo, from the SV_UpdateUserinfo_f hook. Returns 0 to drop the
 * change, 1 to pass it through, or 2 having written a replacement infostring into `out`.
 */
int UserinfoDispatcher(int client_id, const char* infostring, char* out, size_t out_size);

/* Damage, after G_Damage has run, so the victim's health already reflects the hit. It cannot
 * be cancelled. attacker_id is -1 when no client was responsible; dflags is a DAMAGE_*
 * bitfield and mod a raw meansOfDeath_t. */
void DamageDispatcher(int target_id, int attacker_id, int damage, int dflags, int mod);

/* A cvar write that changed the live value, after it was applied; cannot cancel. new_value is
 * what the engine kept, so a range-flagged cvar reports its clamped text. Nothing fires for
 * creation, a same-value write, or an unforced write to a CVAR_LATCH cvar. */
void CvarChangedDispatcher(const char* name, const char* old_value, const char* new_value);

#endif /* PYMINQLXTENDED_H */
