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

#include <Python.h>
#include <errno.h>
#include <patchlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <structmember.h>
#include <structseq.h>

#include "common.h"
#include "engine/quake_common.h"
#include "features/console_command.h"
#include "features/demos.h"
#include "features/reliable.h"
#include "pyminqlxtended.h"
#include "python_objects.h"

PyObject* client_command_handler    = NULL;
PyObject* server_command_handler    = NULL;
PyObject* client_connect_handler    = NULL;
PyObject* client_loaded_handler     = NULL;
PyObject* client_disconnect_handler = NULL;
PyObject* frame_handler             = NULL;
PyObject* custom_command_handler    = NULL;
PyObject* new_game_handler          = NULL;
PyObject* spawn_server_handler      = NULL;
PyObject* set_configstring_handler  = NULL;
PyObject* rcon_handler              = NULL;
PyObject* console_print_handler     = NULL;
PyObject* client_spawn_handler      = NULL;

PyObject* kamikaze_use_handler     = NULL;
PyObject* kamikaze_explode_handler = NULL;

PyObject* demo_finished_handler = NULL;

PyObject* player_death_handler = NULL;
PyObject* round_countdown_handler = NULL;
PyObject* round_start_handler     = NULL;
PyObject* round_end_handler       = NULL;
PyObject* game_countdown_handler  = NULL;
PyObject* game_start_handler      = NULL;
PyObject* game_end_handler        = NULL;
PyObject* team_switch_handler  = NULL;
PyObject* item_pickup_handler  = NULL;
PyObject* vote_called_handler  = NULL;
PyObject* vote_started_handler = NULL;
PyObject* vote_ended_handler   = NULL;
PyObject* objective_handler    = NULL;
PyObject* chat_handler         = NULL;
PyObject* team_switch_attempt_handler = NULL;
PyObject* userinfo_handler     = NULL;
PyObject* weapon_fired_handler = NULL;

// Gated: armed by Python only while something is hooking the event. See pyminqlxtended.h.
PyObject* damage_handler       = NULL;
PyObject* cvar_changed_handler = NULL;

static int initialized = 0;

/*
 * Without this, PyRun_File*() and PyRun_String*() return a bare NULL for a module with an
 * error in it and the traceback is unreachable.
 */
static const char loader[] = "import traceback\n"
                             "try:\n"
                             "  import sys\n"
                             "  sys.path.append('" CORE_MODULE "')\n"
                             "  sys.path.append('.')\n"
                             "  import minqlxtended\n"
                             "  minqlxtended.initialize()\n"
                             "  ret = True\n"
                             "except Exception as e:\n"
                             "  e = traceback.format_exc().rstrip('\\n')\n"
                             "  for line in e.split('\\n'): print(line)\n"
                             "  ret = False\n";

// Name-to-slot pairs, iterated by register_handler.
static handler_t handlers[] = {
    {"client_command", &client_command_handler},
    {"server_command", &server_command_handler},
    {"frame", &frame_handler},
    {"player_connect", &client_connect_handler},
    {"player_loaded", &client_loaded_handler},
    {"player_disconnect", &client_disconnect_handler},
    {"custom_command", &custom_command_handler},
    {"new_game", &new_game_handler},
    {"spawn_server", &spawn_server_handler},
    {"set_configstring", &set_configstring_handler},
    {"rcon", &rcon_handler},
    {"console_print", &console_print_handler},
    {"player_spawn", &client_spawn_handler},

    {"kamikaze_use", &kamikaze_use_handler},
    {"kamikaze_explode", &kamikaze_explode_handler},

    {"demo_finished", &demo_finished_handler},

    {"player_death", &player_death_handler},
    {"round_countdown", &round_countdown_handler},
    {"round_start", &round_start_handler},
    {"round_end", &round_end_handler},
    {"game_countdown", &game_countdown_handler},
    {"game_start", &game_start_handler},
    {"game_end", &game_end_handler},
    {"team_switch", &team_switch_handler},
    {"item_pickup", &item_pickup_handler},
    {"vote_called", &vote_called_handler},
    {"vote_started", &vote_started_handler},
    {"vote_ended", &vote_ended_handler},
    {"objective", &objective_handler},
    {"chat", &chat_handler},
    {"team_switch_attempt", &team_switch_attempt_handler},
    {"userinfo", &userinfo_handler},
    {"weapon_fired", &weapon_fired_handler},

    {"damage", &damage_handler},
    {"cvar_changed", &cvar_changed_handler},

    {NULL, NULL}};

// Struct Sequences

// Players
static PyTypeObject player_info_type = {0};

static PyStructSequence_Field player_info_fields[] = {
    {"client_id", "The player's client ID."},
    {"name", "The player's name."},
    {"connection_state", "The player's connection state."},
    {"userinfo", "The player's userinfo."},
    {"steam_id", "The player's 64-bit representation of the Steam ID."},
    {"team", "The player's team."},
    {"privileges", "The player's privileges."},
    {NULL}};

static PyStructSequence_Desc player_info_desc = {
    "PlayerInfo",
    "Information about a player, such as Steam ID, name, client ID, and whatnot.",
    player_info_fields,
    (sizeof(player_info_fields) / sizeof(PyStructSequence_Field)) - 1};

// Player state
static PyTypeObject player_state_type = {0};

static PyStructSequence_Field player_state_fields[] = {
    {"is_alive", "Whether the player's alive or not."},
    {"position", "The player's position."},
    {"velocity", "The player's velocity."},
    {"health", "The player's health."},
    {"armor", "The player's armor."},
    {"speed", "The player's speed."},
    {"gravity", "The player's gravity."},
    {"noclip", "Whether the player has noclip or not."},
    {"weapon", "The weapon the player is currently using."},
    {"weapons", "The player's weapons."},
    {"ammo", "The player's weapon ammo."},
    {"powerups", "The player's powerups."},
    {"holdable", "The player's holdable item."},
    {"flight", "A struct sequence with flight parameters."},
    {"is_frozen", "Whether the player is frozen (freeze tag)."},
    {"keys", "The player's keys."},
    {"flags", "Player entity flags (FL_*)."},
    {"god", "Whether the player has godmode or not."},
    {"notarget", "Whether the player has notarget or not."},
    {NULL}};

static PyStructSequence_Desc player_state_desc = {
    "PlayerState",
    "Information about a player's state in the game.",
    player_state_fields,
    (sizeof(player_state_fields) / sizeof(PyStructSequence_Field)) - 1};

// Stats
static PyTypeObject player_stats_type = {0};

static PyStructSequence_Field player_stats_fields[] = {
    {"score", "The player's primary score."},
    {"kills", "The player's number of kills."},
    {"deaths", "The player's number of deaths."},
    {"damage_dealt", "The player's total damage dealt."},
    {"damage_taken", "The player's total damage taken."},
    {"time", "The time in milliseconds the player has on a team since the game started."},
    {"ping", "The player's ping."},
    {NULL}};

static PyStructSequence_Desc player_stats_desc = {
    "PlayerStats",
    "A player's score and some basic stats.",
    player_stats_fields,
    (sizeof(player_stats_fields) / sizeof(PyStructSequence_Field)) - 1};

// Vectors
static PyTypeObject vector3_type = {0};

static PyStructSequence_Field vector3_fields[] = {
    {"x", NULL},
    {"y", NULL},
    {"z", NULL},
    {NULL}};

static PyStructSequence_Desc vector3_desc = {
    "Vector3",
    "A three-dimensional vector.",
    vector3_fields,
    (sizeof(vector3_fields) / sizeof(PyStructSequence_Field)) - 1};

// Weapons
static PyTypeObject weapons_type = {0};

static PyStructSequence_Field weapons_fields[] = {
    {"g", NULL}, {"mg", NULL}, {"sg", NULL}, {"gl", NULL}, {"rl", NULL}, {"lg", NULL}, {"rg", NULL}, {"pg", NULL}, {"bfg", NULL}, {"gh", NULL}, {"ng", NULL}, {"pl", NULL}, {"cg", NULL}, {"hmg", NULL}, {"hands", NULL}, {NULL}};

static PyStructSequence_Desc weapons_desc = {
    "Weapons",
    "A struct sequence containing all the weapons in the game.",
    weapons_fields,
    (sizeof(weapons_fields) / sizeof(PyStructSequence_Field)) - 1};

// Powerups
static PyTypeObject powerups_type = {0};

static PyStructSequence_Field powerups_fields[] = {
    {"quad", NULL}, {"battlesuit", NULL}, {"haste", NULL}, {"invisibility", NULL}, {"regeneration", NULL}, {"invulnerability", NULL}, {NULL}};

static PyStructSequence_Desc powerups_desc = {
    "Powerups",
    "A struct sequence containing all the powerups in the game.",
    powerups_fields,
    (sizeof(powerups_fields) / sizeof(PyStructSequence_Field)) - 1};

// Flight
static PyTypeObject flight_type = {0};

static PyStructSequence_Field flight_fields[] = {
    {"fuel", NULL},
    {"max_fuel", NULL},
    {"thrust", NULL},
    {"refuel", NULL},
    {NULL}};

static PyStructSequence_Desc flight_desc = {
    "Flight",
    "A struct sequence containing parameters for the flight holdable item.",
    flight_fields,
    (sizeof(flight_fields) / sizeof(PyStructSequence_Field)) - 1};

// Keys
static PyTypeObject keys_type = {0};

static PyStructSequence_Field keys_fields[] = {
    {"silver", NULL},
    {"gold", NULL},
    {"master", NULL},
    {NULL}};

static PyStructSequence_Desc keys_desc = {
    "Keys",
    "A struct sequence containing all the keys in the game.",
    keys_fields,
    (sizeof(keys_fields) / sizeof(PyStructSequence_Field)) - 1};

// Demo status
static PyTypeObject demo_status_type = {0};

static PyStructSequence_Field demo_status_fields[] = {
    {"recording", "Whether a demo segment is currently being written for the player."},
    {"requested", "1 if the player is explicitly being recorded, -1 if explicitly excluded, 0 if following sv_demoRecord."},
    {"path", "Path of the demo being written, or None when not recording."},
    {NULL}};

static PyStructSequence_Desc demo_status_desc = {
    "DemoStatus",
    "The demo recording state of a player.",
    demo_status_fields,
    (sizeof(demo_status_fields) / sizeof(PyStructSequence_Field)) - 1};

// Reliable-channel backpressure, from reliable.c.
static PyTypeObject reliable_status_type = {0};

static PyStructSequence_Field reliable_status_fields[] = {
    {"enabled", "Whether the reliable command guard is pacing at all."},
    {"watermark", "The backlog depth where pacing starts."},
    {"burst", "Commands released per frame once pacing has started."},
    {"waiting", "Commands held in the pacing queue right now."},
    {"queued", "Commands held back at least one frame since the map started."},
    {"merged", "Broadcast prints folded into a preceding batch since the map started."},
    {"bypassed", "Commands sent unpaced because the queue was full."},
    {"backlog", "The deepest live per-client backlog right now, out of the 64-slot ring."},
    {"worst_backlog", "The deepest backlog seen this map."},
    {"worst_slot", "The client that reached worst_backlog, or -1 for none yet."},
    {NULL}};

static PyStructSequence_Desc reliable_status_desc = {
    "ReliableStatus",
    "A snapshot of the reliable server-command channel's backpressure.",
    reliable_status_fields,
    (sizeof(reliable_status_fields) / sizeof(PyStructSequence_Field)) - 1};

// Indexed straight by powerup_t. Not the Powerups sequence, which covers only
// PW_QUAD..PW_INVULNERABILITY and skips PW_FLIGHT.
static PyTypeObject stat_powerups_type = {0};

static PyStructSequence_Field stat_powerups_fields[] = {
    {"spawnarmor", NULL},
    {"redflag", NULL},
    {"blueflag", NULL},
    {"neutralflag", NULL},
    {"quad", NULL},
    {"battlesuit", NULL},
    {"haste", NULL},
    {"invisibility", NULL},
    {"regeneration", NULL},
    {"flight", NULL},
    {"invulnerability", NULL},
    {"scout", NULL},
    {"guard", NULL},
    {"doubler", NULL},
    {"armorregen", NULL},
    {"freeze", NULL},
    {NULL}};

static PyStructSequence_Desc stat_powerups_desc = {
    "StatPowerups",
    "Per-powerup counters, in powerup_t order.",
    stat_powerups_fields,
    (sizeof(stat_powerups_fields) / sizeof(PyStructSequence_Field)) - 1};

// Indexed by holdable_t, skipping HI_NONE as the Weapons sequence skips WP_NONE.
static PyTypeObject stat_holdables_type = {0};

static PyStructSequence_Field stat_holdables_fields[] = {
    {"teleporter", NULL},
    {"medkit", NULL},
    {"kamikaze", NULL},
    {"portal", NULL},
    {"invulnerability", NULL},
    {"flight", NULL},
    {NULL}};

static PyStructSequence_Desc stat_holdables_desc = {
    "StatHoldables",
    "Per-holdable counters, in holdable_t order.",
    stat_holdables_fields,
    (sizeof(stat_holdables_fields) / sizeof(PyStructSequence_Field)) - 1};

// Both index their arrays directly, so an enum change would silently misreport.
_Static_assert((sizeof(stat_powerups_fields) / sizeof(PyStructSequence_Field)) - 1 == PW_NUM_POWERUPS,
               "StatPowerups no longer covers powerup_t");
_Static_assert((sizeof(stat_holdables_fields) / sizeof(PyStructSequence_Field)) - 1 == HI_NUM_HOLDABLE - 1,
               "StatHoldables no longer covers holdable_t minus HI_NONE");

// The rest of expandedStatObj_t, beyond what PlayerStats exposes. Per-weapon values
// reuse the Weapons sequence, whose 15 fields are array indices 1..15.
static PyTypeObject player_expanded_stats_type = {0};

static PyStructSequence_Field player_expanded_stats_fields[] = {
    {"play_time", "Milliseconds spent on a team this match."},
    {"server_rank", NULL},
    {"server_rank_tied", NULL},
    {"team_rank", NULL},
    {"team_rank_tied", NULL},
    {"kills", NULL},
    {"deaths", NULL},
    {"suicides", NULL},
    {"team_kills", "Teammates killed."},
    {"team_killed", "Times killed by a teammate."},
    {"damage_dealt", NULL},
    {"damage_taken", NULL},
    {"captures", NULL},
    {"assists", NULL},
    {"defends", NULL},
    {"holy_shits", NULL},
    {"denials", NULL},
    {"kill_streak", "Current kill streak."},
    {"max_kill_streak", NULL},
    {"midair_shotgun_kills", NULL},
    {"quad_damage_kills", "Kills made while holding quad damage."},
    {"xp", "Experience earned this match."},
    {"dom_three_flags_time", "Milliseconds holding all three domination points."},
    {"weapon_kills", NULL},
    {"weapon_deaths", NULL},
    {"shots_fired", NULL},
    {"shots_hit", NULL},
    {"weapon_damage_dealt", NULL},
    {"weapon_damage_taken", NULL},
    {"weapon_pickups", NULL},
    {"weapon_time", "Milliseconds held per weapon."},
    {"ammo_pickups", NULL},
    {"health_pickups", NULL},
    {"mega_health_pickups", NULL},
    {"first_mega_health_pickups", "Mega healths taken before anyone else."},
    {"mega_health_pickup_time", "Time of the last mega health pickup."},
    {"armor_pickups", "Armor pickups of any kind."},
    {"red_armor_pickups", NULL},
    {"first_red_armor_pickups", "Red armors taken before anyone else."},
    {"red_armor_pickup_time", "Time of the last red armor pickup."},
    {"yellow_armor_pickups", NULL},
    {"first_yellow_armor_pickups", "Yellow armors taken before anyone else."},
    {"yellow_armor_pickup_time", "Time of the last yellow armor pickup."},
    {"green_armor_pickups", NULL},
    {"first_green_armor_pickups", "Green armors taken before anyone else."},
    {"green_armor_pickup_time", "Time of the last green armor pickup."},
    {"quad_damage_pickups", NULL},
    {"battle_suit_pickups", NULL},
    {"regeneration_pickups", NULL},
    {"haste_pickups", NULL},
    {"invisibility_pickups", NULL},
    {"medkit_pickups", NULL},
    {"red_flag_pickups", NULL},
    {"blue_flag_pickups", NULL},
    {"neutral_flag_pickups", NULL},
    {"powerup_pickups", "Pickups per powerup, as a StatPowerups."},
    {"holdable_pickups", "Pickups per holdable item, as a StatHoldables."},
    {NULL}};

static PyStructSequence_Desc player_expanded_stats_desc = {
    "PlayerExpandedStats",
    "Detailed per-player statistics for the current match.",
    player_expanded_stats_fields,
    (sizeof(player_expanded_stats_fields) / sizeof(PyStructSequence_Field)) - 1};

// player_info/players_info

/* A Python int as the engine's int, named *name* in the error. False with an exception set if
 * it doesn't fit. PyLong_AsLong returns a 64-bit long here, so a straight assignment to an int
 * truncates and the range checks downstream would pass on the truncated value. */
static qboolean qlx_as_int(PyObject* value, int* out, const char* name) {
    long v = PyLong_AsLong(value);
    if (v == -1 && PyErr_Occurred()) {
        return qfalse;
    }

    if (v < INT_MIN || v > INT_MAX) {
        PyErr_Format(PyExc_OverflowError, "%s does not fit in the engine's int", name);
        return qfalse;
    }

    *out = (int)v;
    return qtrue;
}

/* Bounds-check a client id. False with an exception set if it is no good. sv_maxclients only
 * resolves once InitializeCvars has run, and reading ->integer off NULL takes the server down
 * rather than raising. */
static qboolean qlx_valid_client_id(int client_id) {
    if (!sv_maxclients) {
        PyErr_SetString(qlx_EngineStateError, "sv_maxclients is not available yet");
        return qfalse;
    }

    if (client_id < 0 || client_id >= sv_maxclients->integer) {
        PyErr_Format(PyExc_ValueError, "client id %d is out of range (0-%d)", client_id,
                     sv_maxclients->integer - 1);
        return qfalse;
    }

    return qtrue;
}

/* False with an exception set if we are not on the game thread. For the demo and reliable state,
 * which is read and written without a lock because everything that touches it runs in the frame
 * loop. A worker thread reaching it races the frame rather than getting a stale answer: the demo
 * request counters are a read-modify-write, and a demo path can be rewritten mid-read. */
static qboolean qlx_on_game_thread(const char* what) {
    if (!OnGameThread()) {
        PyErr_Format(qlx_EngineStateError,
                     "%s is game thread only; call it from a handler or through @next_frame", what);
        return qfalse;
    }

    return qtrue;
}

/* The game module's globals, or false with an exception set. g_entities, level and bg_itemlist
 * resolve together in InitializeVm, so they are NULL before the first map and reading through
 * one takes the server down. svs needs no check; SearchFunctions exits the process if it
 * cannot resolve it. */
static qboolean qlx_vm_ready(void) {
    if (atomic_load_explicit(&vm_rehooking, memory_order_acquire)) {
        PyErr_SetString(qlx_EngineStateError,
                        "the game module is being reloaded; try again on a later frame");
        return qfalse;
    }

    if (!g_entities || !level || !bg_itemlist) {
        PyErr_SetString(qlx_EngineStateError,
                        "the game module is not loaded; no map has been loaded yet");
        return qfalse;
    }

    // Resolved separately, in InitializeCvars, and it is the bound on every walk of the
    // client slots. qlx_valid_client_id repeats this one test for the calls that need the
    // cvar without needing the game module.
    if (!sv_maxclients) {
        PyErr_SetString(qlx_EngineStateError, "sv_maxclients is not available yet");
        return qfalse;
    }

    return qtrue;
}

/* The slot behind a client id, insisting somebody is in it. For the calls that act on a
 * player; the read-only ones use qlx_valid_client_id and answer None for an empty slot. */
static gentity_t* qlx_live_client(int client_id) {
    if (!qlx_valid_client_id(client_id)) {
        return NULL;
    }

    if (atomic_load_explicit(&vm_rehooking, memory_order_acquire)) {
        PyErr_SetString(qlx_EngineStateError,
                        "the game module is being reloaded; try again on a later frame");
        return NULL;
    }

    if (!g_entities) {
        PyErr_SetString(qlx_EngineStateError,
                        "g_entities is not available; the game module is not loaded");
        return NULL;
    }

    if (!g_entities[client_id].client) {
        PyErr_Format(qlx_EngineStateError,
                     "client %d has no game client; nobody is in that slot", client_id);
        return NULL;
    }

    return &g_entities[client_id];
}

static PyObject* makePlayerTuple(int client_id) {
    PyObject *name, *team, *priv;
    PyObject* cid = PyLong_FromLongLong(client_id);

    if (g_entities[client_id].client != NULL) {
        if (g_entities[client_id].client->pers.connected == CON_DISCONNECTED) {
            name = PyUnicode_FromString("");
        } else {
            name = PyUnicode_DecodeUTF8(g_entities[client_id].client->pers.netname,
                                        strlen(g_entities[client_id].client->pers.netname), "ignore");
        }

        if (g_entities[client_id].client->pers.connected == CON_DISCONNECTED) {
            team = PyLong_FromLongLong(TEAM_SPECTATOR); // Set team to spectator if not yet connected.
        } else {
            team = PyLong_FromLongLong(g_entities[client_id].client->sess.sessionTeam);
        }

        priv = PyLong_FromLongLong(g_entities[client_id].client->sess.privileges);
    } else {
        name = PyUnicode_FromString("");
        team = PyLong_FromLongLong(TEAM_SPECTATOR);
        priv = PyLong_FromLongLong(-1);
    }

    PyObject* state    = PyLong_FromLongLong(svs->clients[client_id].state);
    PyObject* userinfo = PyUnicode_DecodeUTF8(svs->clients[client_id].userinfo, strlen(svs->clients[client_id].userinfo), "ignore");
    PyObject* steam_id = PyLong_FromLongLong(svs->clients[client_id].steam_id);

    PyObject* info = PyStructSequence_New(&player_info_type);
    // PyStructSequence_SetItem is PyTuple_SET_ITEM, an unchecked store, so a NULL sequence
    // is a NULL write and a NULL value reaches Python as a tuple slot that blows up later
    // with "null argument to internal routine". Check everything first.
    if (!info || !cid || !name || !state || !userinfo || !steam_id || !team || !priv) {
        Py_XDECREF(info);
        Py_XDECREF(cid);
        Py_XDECREF(name);
        Py_XDECREF(state);
        Py_XDECREF(userinfo);
        Py_XDECREF(steam_id);
        Py_XDECREF(team);
        Py_XDECREF(priv);
        return PyErr_NoMemory();
    }

    PyStructSequence_SetItem(info, 0, cid);
    PyStructSequence_SetItem(info, 1, name);
    PyStructSequence_SetItem(info, 2, state);
    PyStructSequence_SetItem(info, 3, userinfo);
    PyStructSequence_SetItem(info, 4, steam_id);
    PyStructSequence_SetItem(info, 5, team);
    PyStructSequence_SetItem(info, 6, priv);

    return info;
}

static PyObject* PyMinqlxtended_PlayerInfo(PyObject* self, PyObject* args) {
    int i;
    // The suffix names the function in the TypeError PyArg_ParseTuple raises, so it has to
    // match the name this is exported under.
    if (!PyArg_ParseTuple(args, "i:player_info", &i)) {
        return NULL;
    }

    if (!qlx_valid_client_id(i)) {
        return NULL;
    }

    if (allow_free_client != i && svs->clients[i].state == CS_FREE) {
#ifndef NDEBUG
        DebugPrint("WARNING: PyMinqlxtended_PlayerInfo called for CS_FREE client %d.\n", i);
#endif
        Py_RETURN_NONE;
    }

    return makePlayerTuple(i);
}

static PyObject* PyMinqlxtended_PlayersInfo(PyObject* self, PyObject* args) {
    // makePlayerTuple below reads g_entities as well as svs, and sv_maxclients is the loop
    // bound. Both are NULL before the first map.
    if (!qlx_vm_ready()) {
        return NULL;
    }

    PyObject* ret = PyList_New(sv_maxclients->integer);
    if (!ret) {
        return NULL;
    }

    for (int i = 0; i < sv_maxclients->integer; i++) {
        if (svs->clients[i].state == CS_FREE) {
            Py_INCREF(Py_None);
            if (PyList_SetItem(ret, i, Py_None) == -1) {
                Py_DECREF(ret);
                return NULL;
            }
            continue;
        }

        // PyList_SetItem takes a NULL item without complaint, so makePlayerTuple has to be
        // checked here or a failed slot becomes a NULL element with the error still set.
        PyObject* player = makePlayerTuple(i);
        if (!player) {
            Py_DECREF(ret);
            return NULL;
        }

        if (PyList_SetItem(ret, i, player) == -1) {
            Py_DECREF(ret);
            return NULL;
        }
    }

    return ret;
}

// get_userinfo

static PyObject* PyMinqlxtended_GetUserinfo(PyObject* self, PyObject* args) {
    int i;
    if (!PyArg_ParseTuple(args, "i:get_userinfo", &i)) {
        return NULL;
    }

    if (!qlx_valid_client_id(i)) {
        return NULL;
    }

    if (allow_free_client != i && svs->clients[i].state == CS_FREE) {
        Py_RETURN_NONE;
    }

    return PyUnicode_DecodeUTF8(svs->clients[i].userinfo, strlen(svs->clients[i].userinfo), "ignore");
}

// send_server_command

static PyObject* PyMinqlxtended_SendServerCommand(PyObject* self, PyObject* args) {
    PyObject* client_id;
    int i;
    char* cmd;
    if (!PyArg_ParseTuple(args, "Os:send_server_command", &client_id, &cmd)) {
        return NULL;
    }

    if (client_id == Py_None) {
        My_SV_SendServerCommand(NULL, "%s\n", cmd); // Send to all.
        Py_RETURN_TRUE;
    }

    // TypeError for the wrong type and ValueError for the wrong value, rather than one
    // ValueError for both. This is the only one of the three that really does take None.
    if (!PyLong_Check(client_id)) {
        PyErr_SetString(PyExc_TypeError, "client_id must be an int or None.");
        return NULL;
    }

    if (!qlx_as_int(client_id, &i, "client_id")) {
        return NULL;
    }
    if (!qlx_valid_client_id(i)) {
        return NULL;
    }

    if (svs->clients[i].state != CS_ACTIVE) {
        Py_RETURN_FALSE;
    }

    My_SV_SendServerCommand(&svs->clients[i], "%s\n", cmd);
    Py_RETURN_TRUE;
}

// client_command

static PyObject* PyMinqlxtended_ClientCommand(PyObject* self, PyObject* args) {
    int i;
    char* cmd;
    if (!PyArg_ParseTuple(args, "is:client_command", &i, &cmd)) {
        return NULL;
    }

    if (!qlx_valid_client_id(i)) {
        return NULL;
    }

    if (svs->clients[i].state == CS_FREE || svs->clients[i].state == CS_ZOMBIE) {
        Py_RETURN_FALSE;
    }

    My_SV_ExecuteClientCommand(&svs->clients[i], cmd, qtrue);
    Py_RETURN_TRUE;
}

// console_command

/* Runs where it was asked for, except for the commands that reload the game module and calls
 * from a worker thread, which go to the engine's command buffer. console_command.h says why.
 * No qlx_vm_ready() gate: this reaches the engine's own command table, and a `map` before the
 * first level is legitimate. */
static PyObject* PyMinqlxtended_ConsoleCommand(PyObject* self, PyObject* args) {
    char* cmd;
    if (!PyArg_ParseTuple(args, "s:console_command", &cmd)) {
        return NULL;
    }

    size_t len = strlen(cmd);
    if (len == 0) {
        PyErr_SetString(PyExc_ValueError, "the command is empty");
        return NULL;
    }

    if (len >= MAX_STRING_CHARS) {
        PyErr_Format(PyExc_ValueError,
                     "the command is %zu characters; the engine's command buffer takes at "
                     "most %d per command",
                     len, MAX_STRING_CHARS - 1);
        return NULL;
    }

    // Raised rather than counted: a command that vanishes is worse than one that says so,
    // and the queue is deep enough that reaching this means something is looping.
    if (!ConsoleCommand_Run(cmd)) {
        PyErr_SetString(qlx_EngineStateError,
                        "the console command queue is full; try again on a later frame");
        return NULL;
    }

    Py_RETURN_NONE;
}

// get_cvar

static PyObject* PyMinqlxtended_GetCvar(PyObject* self, PyObject* args) {
    char* name;
    if (!PyArg_ParseTuple(args, "s:get_cvar", &name)) {
        return NULL;
    }

    cvar_t* cvar = Cvar_FindVar(name);
    if (cvar) {
        return PyUnicode_FromString(cvar->string);
    }

    Py_RETURN_NONE;
}

// set_cvar

static PyObject* PyMinqlxtended_SetCvar(PyObject* self, PyObject* args, PyObject* kwargs) {
    static char* kwlist[] = {"name", "value", "flags", "force", NULL};
    char *name, *value;
    int flags = 0;
    int force = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "ss|ip:set_cvar", kwlist, &name, &value,
                                     &flags, &force)) {
        return NULL;
    }

    if (qlx_refuse_forced_write(name, force)) {
        return NULL;
    }

    if (!Cvar_FindVar(name)) {
        Cvar_Get(name, value, flags);
    } else {
        // Through the hook rather than the trampoline the pointer holds, so a
        // Python-initiated set raises cvar_changed the same as a console one. As
        // set_configstring does.
        My_Cvar_Set2(name, value, force ? qtrue : qfalse);
    }

    /*
     * The cvar itself, so the caller can read back what the engine made of the value: a
     * latched set, a clamp, a refused CVAR_ROM write.
     */
    return PyMinqlxtended_MakeCvar(name);
}

// set_cvar_limit

static PyObject* PyMinqlxtended_SetCvarLimit(PyObject* self, PyObject* args) {
    char *name, *value, *min, *max;
    int flags = 0;
    if (!PyArg_ParseTuple(args, "ssss|i:set_cvar_limit", &name, &value, &min, &max, &flags)) {
        return NULL;
    }

    Cvar_GetLimit(name, value, min, max, flags);
    Py_RETURN_NONE;
}

// kick

static PyObject* PyMinqlxtended_Kick(PyObject* self, PyObject* args) {
    int i;
    PyObject* reason;

    if (!PyArg_ParseTuple(args, "iO:kick", &i, &reason)) {
        return NULL;
    }

    if (!qlx_valid_client_id(i)) {
        return NULL;
    }

    if (svs->clients[i].state != CS_ACTIVE) {
        PyErr_Format(PyExc_ValueError, "client %d is not an active player", i);
        return NULL;
    }

    // "was kicked" is what SV_Kick_f uses with no reason given, spelled as the engine
    // spells it. A supplied reason is used whole, where SV_Kick_f would render it as
    // "was kicked: <reason>" and discard it outright past 127 characters.
    if (reason == Py_None) {
        My_SV_DropClient(&svs->clients[i], "was kicked");
    } else if (PyUnicode_Check(reason)) {
        const char* reason_str = PyUnicode_AsUTF8(reason);
        if (!reason_str) {
            return NULL; // Propagate the encoding error (e.g. lone surrogates).
        }
        My_SV_DropClient(&svs->clients[i], reason_str[0] == 0 ? "was kicked" : reason_str);
    } else {
        // Without this the call returns None exactly like a successful kick while leaving
        // the player connected, so an admin plugin reports a kick that never happened.
        PyErr_Format(PyExc_TypeError, "reason must be a string or None.");
        return NULL;
    }

    Py_RETURN_NONE;
}

// console_print

static PyObject* PyMinqlxtended_ConsolePrint(PyObject* self, PyObject* args) {
    char* text;
    if (!PyArg_ParseTuple(args, "s:console_print", &text)) {
        return NULL;
    }

    My_Com_Printf("%s\n", text);

    Py_RETURN_NONE;
}

// get_configstring

static PyObject* PyMinqlxtended_GetConfigstring(PyObject* self, PyObject* args) {
    int i;
    char csbuffer[4096];
    if (!PyArg_ParseTuple(args, "i:get_configstring", &i)) {
        return NULL;
    }

    else if (i < 0 || i >= MAX_CONFIGSTRINGS) {
        PyErr_Format(PyExc_ValueError,
                     "index needs to be a number from 0 to %d.",
                     MAX_CONFIGSTRINGS - 1);
        return NULL;
    }

    SV_GetConfigstring(i, csbuffer, sizeof(csbuffer));
    return PyUnicode_DecodeUTF8(csbuffer, strlen(csbuffer), "ignore");
}

// set_configstring

static PyObject* PyMinqlxtended_SetConfigstring(PyObject* self, PyObject* args) {
    int i;
    char* cs;
    if (!PyArg_ParseTuple(args, "is:set_configstring", &i, &cs)) {
        return NULL;
    }

    else if (i < 0 || i >= MAX_CONFIGSTRINGS) {
        PyErr_Format(PyExc_ValueError,
                     "index needs to be a number from 0 to %d.",
                     MAX_CONFIGSTRINGS - 1);
        return NULL;
    }

    My_SV_SetConfigstring(i, cs);

    Py_RETURN_NONE;
}

// force_vote

static PyObject* PyMinqlxtended_ForceVote(PyObject* self, PyObject* args) {
    int pass;
    if (!PyArg_ParseTuple(args, "p:force_vote", &pass)) {
        return NULL;
    }

    if (!qlx_vm_ready()) {
        return NULL;
    }

    if (level->intermissionTime) {
        // voteTime belongs to the end-of-match map vote here, and neither branch below means
        // anything against it: the fail path rewinds voteTime past the 20 second window
        // Cmd_IntermissionVote_f measures, which expires map voting for everyone.
        Py_RETURN_FALSE;
    }

    if (!level->voteTime) {
        // No active vote.
        Py_RETURN_FALSE;
    } else if (pass && level->voteTime) {
        // We tell the server every single client voted yes, making it pass in the next G_RunFrame.
        for (int i = 0; i < sv_maxclients->integer; i++) {
            // The client check matches Callvote below: a map change can zero g_entities
            // while svs->clients still reports CS_ACTIVE.
            if (svs->clients[i].state == CS_ACTIVE && g_entities[i].client) {
                g_entities[i].client->pers.voteState = VOTE_YES;
            }
        }
    } else if (!pass && level->voteTime) {
        // If we tell the server the vote is over, it'll fail it right away.
        level->voteTime -= 30000;
    }

    Py_RETURN_TRUE;
}

// add_console_command

static PyObject* PyMinqlxtended_AddConsoleCommand(PyObject* self, PyObject* args) {
    char* cmd;
    if (!PyArg_ParseTuple(args, "s:add_console_command", &cmd)) {
        return NULL;
    }

    Cmd_AddCommand(cmd, PyCommand);

    Py_RETURN_NONE;
}

// register_handler

static PyObject* PyMinqlxtended_RegisterHandler(PyObject* self, PyObject* args) {
    char* event;
    PyObject* new_handler;

    if (!PyArg_ParseTuple(args, "sO:register_handler", &event, &new_handler)) {
        return NULL;
    }

    else if (new_handler != Py_None && !PyCallable_Check(new_handler)) {
        PyErr_SetString(PyExc_TypeError, "The handler must be callable.");
        return NULL;
    }

    for (handler_t* h = handlers; h->name; h++) {
        if (!strcmp(h->name, event)) {
            // Publish the new pointer before dropping the old reference. The DECREF can run
            // a __del__, and the game thread may be inside CallHandler on this same slot.
            PyObject* old = *h->handler;
            *h->handler   = (new_handler == Py_None) ? NULL : Py_NewRef(new_handler);
            Py_XDECREF(old);

            Py_RETURN_NONE;
        }
    }

    PyErr_SetString(PyExc_ValueError, "Invalid event.");
    return NULL;
}

// player_state

/* Store *value* in *seq*, taking over its reference. -1 when the value is NULL, with the
 * exception that produced it still set. PyStructSequence_SetItem is PyTuple_SET_ITEM, an
 * unchecked store, so a NULL would otherwise land in a live tuple and blow up on the first
 * read with "null argument to internal routine". */
static int qlx_set_item(PyObject* seq, Py_ssize_t index, PyObject* value) {
    if (!value) {
        return -1;
    }

    PyStructSequence_SetItem(seq, index, value);
    return 0;
}

static PyObject* PyMinqlxtended_PlayerState(PyObject* self, PyObject* args) {
    int client_id;

    if (!PyArg_ParseTuple(args, "i:player_state", &client_id)) {
        return NULL;
    }

    if (!qlx_valid_client_id(client_id)) {
        return NULL;
    }

    if (!g_entities || !g_entities[client_id].client) {
        Py_RETURN_NONE;
    }

    // Every nested sequence is checked as it is made, and `sub` holds the one currently
    // being filled so the bail path can free it.
    PyObject* state = PyStructSequence_New(&player_state_type);
    if (!state) {
        return NULL;
    }

    PyObject* sub = NULL;

    if (qlx_set_item(state, 0, PyBool_FromLong(g_entities[client_id].client->ps.pm_type == PM_NORMAL))) {
        goto fail;
    }

    sub = PyStructSequence_New(&vector3_type);
    if (!sub) {
        goto fail;
    }
    for (int i = 0; i < 3; i++) {
        if (qlx_set_item(sub, i, PyFloat_FromDouble(g_entities[client_id].client->ps.origin[i]))) {
            goto fail;
        }
    }
    PyStructSequence_SetItem(state, 1, sub);
    sub = NULL;

    sub = PyStructSequence_New(&vector3_type);
    if (!sub) {
        goto fail;
    }
    for (int i = 0; i < 3; i++) {
        if (qlx_set_item(sub, i, PyFloat_FromDouble(g_entities[client_id].client->ps.velocity[i]))) {
            goto fail;
        }
    }
    PyStructSequence_SetItem(state, 2, sub);
    sub = NULL;

    if (qlx_set_item(state, 3, PyLong_FromLongLong(g_entities[client_id].health)) ||
        qlx_set_item(state, 4, PyLong_FromLongLong(g_entities[client_id].client->ps.stats[STAT_ARMOR])) ||
        qlx_set_item(state, 5, PyLong_FromLongLong(g_entities[client_id].client->ps.speed)) ||
        qlx_set_item(state, 6, PyLong_FromLongLong(g_entities[client_id].client->ps.gravity)) ||
        qlx_set_item(state, 7, PyBool_FromLong(g_entities[client_id].client->noclip)) ||
        qlx_set_item(state, 8, PyLong_FromLongLong(g_entities[client_id].client->ps.weapon))) {
        goto fail;
    }

    // Get weapons and ammo count.
    sub = PyStructSequence_New(&weapons_type);
    if (!sub) {
        goto fail;
    }
    for (int i = 0; i < weapons_desc.n_in_sequence; i++) {
        if (qlx_set_item(sub, i, PyBool_FromLong(g_entities[client_id].client->ps.stats[STAT_WEAPONS] & (1 << (i + 1))))) {
            goto fail;
        }
    }
    PyStructSequence_SetItem(state, 9, sub);
    sub = NULL;

    sub = PyStructSequence_New(&weapons_type);
    if (!sub) {
        goto fail;
    }
    for (int i = 0; i < weapons_desc.n_in_sequence; i++) {
        if (qlx_set_item(sub, i, PyLong_FromLongLong(g_entities[client_id].client->ps.ammo[i + 1]))) {
            goto fail;
        }
    }
    PyStructSequence_SetItem(state, 10, sub);
    sub = NULL;

    sub = PyStructSequence_New(&powerups_type);
    if (!sub) {
        goto fail;
    }
    for (int i = 0; i < powerups_desc.n_in_sequence; i++) {
        int index = i + PW_QUAD;
        if (index == PW_FLIGHT) { // Skip flight.
            index = PW_INVULNERABILITY;
        }
        int remaining = g_entities[client_id].client->ps.powerups[index];
        if (remaining) { // We don't want the time, but the remaining time.
            remaining -= level->time;
        }
        if (qlx_set_item(sub, i, PyLong_FromLongLong(remaining))) {
            goto fail;
        }
    }
    PyStructSequence_SetItem(state, 11, sub);
    sub = NULL;

    PyObject* holdable;
    switch (g_entities[client_id].client->ps.stats[STAT_HOLDABLE_ITEM]) {
    case 0:
        holdable = Py_None;
        Py_INCREF(Py_None);
        break;
    case MODELINDEX_TELEPORTER:
        holdable = PyUnicode_FromString("teleporter");
        break;
    case MODELINDEX_MEDKIT:
        holdable = PyUnicode_FromString("medkit");
        break;
    case MODELINDEX_FLIGHT:
        holdable = PyUnicode_FromString("flight");
        break;
    case MODELINDEX_KAMIKAZE:
        holdable = PyUnicode_FromString("kamikaze");
        break;
    case MODELINDEX_PORTAL:
        holdable = PyUnicode_FromString("portal");
        break;
    case MODELINDEX_INVULNERABILITY:
        holdable = PyUnicode_FromString("invulnerability");
        break;
    default:
        holdable = PyUnicode_FromString("unknown");
    }
    if (qlx_set_item(state, 12, holdable)) {
        goto fail;
    }

    sub = PyStructSequence_New(&flight_type);
    if (!sub) {
        goto fail;
    }
    if (qlx_set_item(sub, 0, PyLong_FromLongLong(g_entities[client_id].client->ps.stats[STAT_CUR_FLIGHT_FUEL])) ||
        qlx_set_item(sub, 1, PyLong_FromLongLong(g_entities[client_id].client->ps.stats[STAT_MAX_FLIGHT_FUEL])) ||
        qlx_set_item(sub, 2, PyLong_FromLongLong(g_entities[client_id].client->ps.stats[STAT_FLIGHT_THRUST])) ||
        qlx_set_item(sub, 3, PyLong_FromLongLong(g_entities[client_id].client->ps.stats[STAT_FLIGHT_REFUEL]))) {
        goto fail;
    }
    PyStructSequence_SetItem(state, 13, sub);
    sub = NULL;

    if (qlx_set_item(state, 14, PyBool_FromLong(g_entities[client_id].client->ps.pm_type == PM_FREEZE))) {
        goto fail;
    }

    sub = PyStructSequence_New(&keys_type);
    if (!sub) {
        goto fail;
    }
    for (int i = 0; i < keys_desc.n_in_sequence; i++) {
        if (qlx_set_item(sub, i, PyBool_FromLong(g_entities[client_id].client->ps.stats[STAT_KEY] & (1 << i)))) {
            goto fail;
        }
    }
    PyStructSequence_SetItem(state, 15, sub);
    sub = NULL;

    if (qlx_set_item(state, 16, PyLong_FromLongLong(g_entities[client_id].flags)) ||
        // God-mode flag set on the player?
        qlx_set_item(state, 17, PyBool_FromLong(((g_entities[client_id].flags & FL_GODMODE) == FL_GODMODE))) ||
        // Notarget-mode flag set on the player?
        qlx_set_item(state, 18, PyBool_FromLong(((g_entities[client_id].flags & FL_NOTARGET) == FL_NOTARGET)))) {
        goto fail;
    }

    return state;

fail:
    Py_XDECREF(sub);
    Py_DECREF(state);
    return NULL;
}

// player_stats

static PyObject* PyMinqlxtended_PlayerStats(PyObject* self, PyObject* args) {
    int client_id;

    if (!PyArg_ParseTuple(args, "i:player_stats", &client_id)) {
        return NULL;
    }

    if (!qlx_valid_client_id(client_id)) {
        return NULL;
    }

    if (!g_entities || !g_entities[client_id].client) {
        Py_RETURN_NONE;
    }

    // Unchecked store, same as player_state above.
    PyObject* stats = PyStructSequence_New(&player_stats_type);
    if (!stats) {
        return NULL;
    }

    int score = g_entities[client_id].client->sess.sessionTeam == TEAM_SPECTATOR ? 0 : g_entities[client_id].client->ps.persistant[PERS_ROUND_SCORE];
    PyStructSequence_SetItem(stats, 0, PyLong_FromLongLong(score));
    PyStructSequence_SetItem(stats, 1, PyLong_FromLongLong(g_entities[client_id].client->expandedStats.numKills));
    PyStructSequence_SetItem(stats, 2, PyLong_FromLongLong(g_entities[client_id].client->expandedStats.numDeaths));
    PyStructSequence_SetItem(stats, 3, PyLong_FromLongLong(g_entities[client_id].client->expandedStats.totalDamageDealt));
    PyStructSequence_SetItem(stats, 4, PyLong_FromLongLong(g_entities[client_id].client->expandedStats.totalDamageTaken));
    PyStructSequence_SetItem(stats, 5, PyLong_FromLongLong(level->time - g_entities[client_id].client->pers.enterTime));
    PyStructSequence_SetItem(stats, 6, PyLong_FromLongLong(g_entities[client_id].client->ps.ping));

    return stats;
}

// drop_holdable

void __cdecl Switch_Touch_Item(gentity_t* ent) {
    // Back to My_Touch_Item, so a dropped holdable raises item_pickup like any other item.
    ent->touch     = (void*)My_Touch_Item;
    ent->think     = G_FreeEntity;
    ent->nextthink = level->time + 29000;
}

/* Touch handler for a just-dropped holdable, stopping whoever dropped it from walking
 * straight back into it. Swapped out for the ordinary one by Switch_Touch_Item. */
void __cdecl DroppedItem_Touch(gentity_t* ent, gentity_t* other, trace_t* trace) {
    if (ent->parent == other) {
        return;
    }
    My_Touch_Item(ent, other, trace);
}

static PyObject* PyMinqlxtended_DropHoldable(PyObject* self, PyObject* args) {
    int client_id, item;
    vec3_t velocity;
    vec_t angle;
    if (!PyArg_ParseTuple(args, "i:drop_holdable", &client_id)) {
        return NULL;
    }

    if (!qlx_live_client(client_id)) {
        return NULL;
    }

    // removing kamikaze flag (surrounding skulls)
    g_entities[client_id].client->ps.eFlags &= ~EF_KAMIKAZE;

    item = g_entities[client_id].client->ps.stats[STAT_HOLDABLE_ITEM];
    if (item < 1 || item >= bg_numItems) {
        Py_RETURN_FALSE;
    }

    angle       = g_entities[client_id].s.apos.trBase[1] * (M_PI * 2 / 360);
    velocity[0] = 150 * cos(angle);
    velocity[1] = 150 * sin(angle);
    velocity[2] = 250;

    gentity_t* entity    = LaunchItem(bg_itemlist + item, g_entities[client_id].s.pos.trBase, velocity);
    entity->touch        = (void*)DroppedItem_Touch;
    entity->parent       = &g_entities[client_id];
    entity->think        = Switch_Touch_Item;
    entity->nextthink    = level->time + 1000;
    entity->s.pos.trTime = level->time - 500;

    // removing holdable from player entity
    g_entities[client_id].client->ps.stats[STAT_HOLDABLE_ITEM] = 0;

    Py_RETURN_TRUE;
}

// callvote

static PyObject* PyMinqlxtended_Callvote(PyObject* self, PyObject* args) {
    char *vote, *vote_disp;
    int vote_time = 30;
    int caller_id = -1;
    char buf[64];
    if (!PyArg_ParseTuple(args, "ss|ii:callvote", &vote, &vote_disp, &vote_time, &caller_id)) {
        return NULL;
    }

    if (!qlx_vm_ready()) {
        return NULL;
    }

    // -1 is the sentinel for "no caller"; anything else has to be a real slot. The field
    // goes to vote_started as a client id, and qagame reads it while resolving the vote.
    if (caller_id != -1 && !qlx_valid_client_id(caller_id)) {
        return NULL;
    }

    /*
     * Always written, including the -1 default. The engine only sets this for votes a
     * client called, so leaving it alone attributes a plugin-started vote to whoever
     * last called one.
     */
    level->pendingVoteCaller = caller_id;

    strncpy(level->voteString, vote, sizeof(level->voteString) - 1);
    level->voteString[sizeof(level->voteString) - 1] = '\0';
    strncpy(level->voteDisplayString, vote_disp, sizeof(level->voteDisplayString) - 1);
    level->voteDisplayString[sizeof(level->voteDisplayString) - 1] = '\0';
    level->voteTime                                                = (level->time - 30000) + vote_time * 1000;
    level->voteYes                                                 = 0;
    level->voteNo                                                  = 0;

    for (int i = 0; i < sv_maxclients->integer; i++) {
        if (g_entities[i].client) {
            g_entities[i].client->pers.voteState = VOTE_PENDING;
        }
    }

    My_SV_SetConfigstring(CS_VOTE_STRING, level->voteDisplayString);
    snprintf(buf, sizeof(buf), "%d", level->voteTime);
    My_SV_SetConfigstring(CS_VOTE_TIME, buf);
    My_SV_SetConfigstring(CS_VOTE_YES, "0");
    My_SV_SetConfigstring(CS_VOTE_NO, "0");

    Py_RETURN_NONE;
}

// player_spawn

static PyObject* PyMinqlxtended_PlayerSpawn(PyObject* self, PyObject* args) {
    int client_id;
    if (!PyArg_ParseTuple(args, "i:player_spawn", &client_id)) {
        return NULL;
    }

    if (!qlx_live_client(client_id)) {
        return NULL;
    }

    g_entities[client_id].client->ps.pm_type = PM_NORMAL;
    My_ClientSpawn(&g_entities[client_id]);
    Py_RETURN_TRUE;
}

// destroy_kamikaze_timers

static PyObject* PyMinqlxtended_DestroyKamikazeTimers(PyObject* self, PyObject* args) {
    int i;
    gentity_t* ent;

    if (!qlx_vm_ready()) {
        return NULL;
    }

    for (i = 0; i < MAX_GENTITIES; i++) {
        ent = &g_entities[i];
        if (!ent->inuse) {
            continue;
        }

        // removing kamikaze skull from dead body
        if (ent->client && ent->health <= 0) {
            ent->client->ps.eFlags &= ~EF_KAMIKAZE;
        }

        if (strcmp(ent->classname, "kamikaze timer") == 0) {
            G_FreeEntity(ent);
        }
    }
    Py_RETURN_TRUE;
}

// spawn_item

static PyObject* PyMinqlxtended_SpawnItem(PyObject* self, PyObject* args) {
    int item_id, x, y, z;
    if (!PyArg_ParseTuple(args, "iiii:spawn_item", &item_id, &x, &y, &z)) {
        return NULL;
    }
    if (!qlx_vm_ready()) {
        return NULL;
    }

    if (item_id < 1 || item_id >= bg_numItems) {
        PyErr_Format(PyExc_ValueError,
                     "item_id needs to be a number from 1 to %d.",
                     bg_numItems - 1);
        return NULL;
    }

    vec3_t origin   = {x, y, z};
    vec3_t velocity = {0};

    gentity_t* ent = LaunchItem(bg_itemlist + item_id, origin, velocity);
    ent->nextthink = 0;
    ent->think     = 0;
    G_AddEvent(ent, EV_ITEM_RESPAWN, 0); // make item be scaled up

    Py_RETURN_TRUE;
}

// drop_item

/* A dropped copy of *item_id*, launched from the player the way death drops are: forward of
 * their yaw plus *angle* degrees. The player's inventory is untouched, since Drop_Item never
 * edits it, so clear the matching GameClient field yourself after a forced flag or weapon
 * drop. */
static PyObject* PyMinqlxtended_DropItem(PyObject* self, PyObject* args) {
    int client_id, item_id;
    float angle = 0.0f;
    if (!PyArg_ParseTuple(args, "ii|f:drop_item", &client_id, &item_id, &angle)) {
        return NULL;
    }
    if (!qlx_vm_ready()) {
        return NULL;
    }
    if (!qlx_valid_client_id(client_id)) {
        return NULL;
    }

    if (item_id < 1 || item_id >= bg_numItems) {
        PyErr_Format(PyExc_ValueError,
                     "item_id needs to be a number from 1 to %d.",
                     bg_numItems - 1);
        return NULL;
    }

    gentity_t* ent = &g_entities[client_id];
    if (!ent->inuse || !ent->client) {
        PyErr_Format(PyExc_ValueError, "client %d is not in use.", client_id);
        return NULL;
    }

    gentity_t* dropped = Drop_Item(ent, bg_itemlist + item_id, angle);
    if (!dropped) {
        Py_RETURN_NONE;
    }

    return PyLong_FromLong((long)(dropped - g_entities));
}

// remove_entity/spawn_entity/link_entity/unlink_entity
// Map entity surgery: freeing a slot, spawning through the engine's own machinery, and
// relinking into the area grid. Verified against qagame/qzeroded 1069.

static PyObject* PyMinqlxtended_RemoveEntity(PyObject* self, PyObject* args) {
    int entity_id;
    if (!PyArg_ParseTuple(args, "i:remove_entity", &entity_id)) {
        return NULL;
    }
    if (!qlx_vm_ready()) {
        return NULL;
    }

    if (entity_id < 0 || entity_id >= MAX_GENTITIES) {
        PyErr_Format(PyExc_ValueError, "entity_id needs to be a number from 0 to %d.",
                     MAX_GENTITIES - 1);
        return NULL;
    }
    if (entity_id < MAX_CLIENTS) {
        PyErr_Format(PyExc_ValueError, "entity %d is a client slot; kick the player instead.",
                     entity_id);
        return NULL;
    }
    if (entity_id >= ENTITYNUM_MAX_NORMAL) {
        PyErr_Format(PyExc_ValueError, "entity %d is the world or the none entity.",
                     entity_id);
        return NULL;
    }

    gentity_t* ent = &g_entities[entity_id];
    if (!ent->inuse) {
        PyErr_Format(PyExc_ValueError, "entity %d is not in use.", entity_id);
        return NULL;
    }
    if (ent->client) {
        PyErr_Format(PyExc_ValueError, "entity %d belongs to a client.", entity_id);
        return NULL;
    }
    // G_FreeEntity checks neverFree only after it has already unlinked, so calling it
    // would half-act. A plugin that means it can clear never_free first.
    if (ent->neverFree) {
        PyErr_Format(PyExc_ValueError, "entity %d is marked never_free.", entity_id);
        return NULL;
    }

    G_FreeEntity(ent);
    Py_RETURN_TRUE;
}

/*
 * One spawn value as text: a str as itself, a bool as the 0/1 the engine reads, a number
 * through str(), a 3-sequence as a space-separated triple. New reference, or NULL set.
 */
static PyObject* qlx_spawn_value_text(PyObject* value) {
    if (PyUnicode_Check(value)) {
        Py_INCREF(value);
        return value;
    }
    if (PyBool_Check(value)) {
        return PyUnicode_FromString(value == Py_True ? "1" : "0");
    }
    if (PyLong_Check(value) || PyFloat_Check(value)) {
        return PyObject_Str(value);
    }
    if (PySequence_Check(value)) {
        Py_ssize_t n = PySequence_Size(value);
        if (n == -1) {
            PyErr_Clear();
        }
        if (n == 3) {
            double v[3];
            for (int i = 0; i < 3; i++) {
                PyObject* item = PySequence_GetItem(value, i);
                if (!item) {
                    return NULL;
                }
                v[i] = PyFloat_AsDouble(item);
                Py_DECREF(item);
                if (PyErr_Occurred()) {
                    return NULL;
                }
            }
            char buf[96];
            snprintf(buf, sizeof(buf), "%g %g %g", v[0], v[1], v[2]);
            return PyUnicode_FromString(buf);
        }
    }
    PyErr_Format(PyExc_TypeError, "spawn values must be str, a number or a 3-sequence, not %s.",
                 Py_TYPE(value)->tp_name);
    return NULL;
}

/* The keys and values of *keys* as owned strs, flattened into one list of alternating key,
 * value. NULL with an exception set. Converting a value runs Python (__str__, __float__),
 * which can mutate the dict mid-walk or re-enter spawn_entity and refill the shared spawn
 * arena, so everything is materialised before the arena is touched. */
static PyObject* qlx_spawn_var_texts(PyObject* keys) {
    PyObject* items = PyDict_Items(keys);
    if (!items) {
        return NULL;
    }

    PyObject* texts = PyList_New(0);
    if (!texts) {
        Py_DECREF(items);
        return NULL;
    }

    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(items); i++) {
        PyObject* pair  = PyList_GET_ITEM(items, i);
        PyObject* key   = PyTuple_GET_ITEM(pair, 0);
        PyObject* value = PyTuple_GET_ITEM(pair, 1);
        if (!PyUnicode_Check(key)) {
            PyErr_Format(PyExc_TypeError, "spawn keys must be str, not %s.",
                         Py_TYPE(key)->tp_name);
            goto fail;
        }
        const char* key_text = PyUnicode_AsUTF8(key);
        if (!key_text) {
            goto fail;
        }
        if (!strcasecmp(key_text, "classname")) {
            PyErr_SetString(PyExc_ValueError,
                            "classname is the first argument; don't repeat it in keys.");
            goto fail;
        }
        PyObject* text = qlx_spawn_value_text(value);
        if (!text) {
            goto fail;
        }
        int appended = PyList_Append(texts, key) == 0 && PyList_Append(texts, text) == 0;
        Py_DECREF(text);
        if (!appended) {
            goto fail;
        }
    }

    Py_DECREF(items);
    return texts;

fail:
    Py_DECREF(items);
    Py_DECREF(texts);
    return NULL;
}

/*
 * Append one NUL-terminated string to level->spawnVarChars, the same arena the map's own
 * entity text is parsed into. The start of the copy, or NULL when the buffer is full.
 */
static char* qlx_push_spawn_string(const char* text) {
    size_t len = strlen(text) + 1;
    if ((size_t)level->numSpawnVarChars + len > MAX_SPAWN_VARS_CHARS) {
        return NULL;
    }
    char* dest = level->spawnVarChars + level->numSpawnVarChars;
    memcpy(dest, text, len);
    level->numSpawnVarChars += (int)len;
    return dest;
}

// Append one key/value pair to level->spawnVars. False with an exception set on overflow.
static qboolean qlx_push_spawn_var(const char* key, const char* value) {
    if (level->numSpawnVars >= MAX_SPAWN_VARS) {
        PyErr_Format(PyExc_ValueError, "spawn_entity() supports at most %d keys.",
                     MAX_SPAWN_VARS - 1);
        return qfalse;
    }
    char* key_copy   = qlx_push_spawn_string(key);
    char* value_copy = key_copy ? qlx_push_spawn_string(value) : NULL;
    if (!value_copy) {
        PyErr_Format(PyExc_ValueError,
                     "spawn_entity() keys and values exceed the engine's %d byte spawn buffer.",
                     MAX_SPAWN_VARS_CHARS);
        return qfalse;
    }
    level->spawnVars[level->numSpawnVars][0] = key_copy;
    level->spawnVars[level->numSpawnVars][1] = value_copy;
    level->numSpawnVars++;
    return qtrue;
}

static PyObject* PyMinqlxtended_SpawnEntity(PyObject* self, PyObject* args) {
    const char* classname;
    PyObject* keys = Py_None;
    if (!PyArg_ParseTuple(args, "s|O:spawn_entity", &classname, &keys)) {
        return NULL;
    }
    if (!qlx_vm_ready()) {
        return NULL;
    }
    if (G_SpawnGEntityFromSpawnVars == NULL) {
        PyErr_SetString(qlx_EngineStateError,
                        "G_SpawnGEntityFromSpawnVars did not resolve in this build.");
        return NULL;
    }
    if (!classname[0]) {
        PyErr_SetString(PyExc_ValueError, "classname must not be empty.");
        return NULL;
    }
    if (keys != Py_None && !PyDict_Check(keys)) {
        PyErr_Format(PyExc_TypeError, "keys must be a dict or None, not %s.",
                     Py_TYPE(keys)->tp_name);
        return NULL;
    }

    /*
     * G_Spawn calls G_Error, ending the map, when every slot from MAX_CLIENTS up is
     * taken, so check the headroom it would have before letting it try.
     */
    if (level->num_entities >= ENTITYNUM_MAX_NORMAL) {
        int found_free = 0;
        for (int i = MAX_CLIENTS; i < ENTITYNUM_MAX_NORMAL; i++) {
            if (!g_entities[i].inuse) {
                found_free = 1;
                break;
            }
        }
        if (!found_free) {
            PyErr_SetString(qlx_EngineStateError, "the entity array is full; nothing can spawn.");
            return NULL;
        }
    }

    // Materialised up front: nothing below this point may run Python until the arena has
    // been filled and read back.
    PyObject* texts = (keys == Py_None) ? PyList_New(0) : qlx_spawn_var_texts(keys);
    if (!texts) {
        return NULL;
    }

    /* Populate the engine's own spawn-var buffers, classname first, as the map's entity text
     * does. QL's G_SpawnString reads them whether or not level.spawning is set, so both
     * counters are zeroed again on every path out. */
    level->numSpawnVars     = 0;
    level->numSpawnVarChars = 0;
    if (!qlx_push_spawn_var("classname", classname)) {
        goto fail;
    }
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(texts); i += 2) {
        const char* key_text   = PyUnicode_AsUTF8(PyList_GET_ITEM(texts, i));
        const char* value_text = key_text ? PyUnicode_AsUTF8(PyList_GET_ITEM(texts, i + 1)) : NULL;
        if (!value_text || !qlx_push_spawn_var(key_text, value_text)) {
            goto fail;
        }
    }

    /* The engine's return value is not trustworthy (void in Q3, garbage in RAX on several
     * qagame paths), so the new entity is found by diffing inuse around the call. G_Spawn
     * hands out the lowest free slot at or above MAX_CLIENTS. */
    char inuse_before[MAX_GENTITIES];
    for (int i = MAX_CLIENTS; i < ENTITYNUM_MAX_NORMAL; i++) {
        inuse_before[i] = g_entities[i].inuse ? 1 : 0;
    }

    G_SpawnGEntityFromSpawnVars();

    level->numSpawnVars     = 0;
    level->numSpawnVarChars = 0;

    int spawned = -1;
    for (int i = MAX_CLIENTS; i < ENTITYNUM_MAX_NORMAL; i++) {
        if (!inuse_before[i] && g_entities[i].inuse) {
            spawned = i;
            break;
        }
    }

    // Held until the diff above is read. A str subclass among the keys can run a __del__
    // here, and that __del__ can spawn entities of its own.
    Py_DECREF(texts);

    if (spawned >= 0) {
        return PyMinqlxtended_MakeEntity(spawned);
    }

    // The engine filtered it out: an unknown classname prints "... doesn't have a spawn
    // function" to the console, and the gametype keys and item rules free it silently.
    Py_RETURN_NONE;

fail:
    level->numSpawnVars     = 0;
    level->numSpawnVarChars = 0;
    Py_DECREF(texts);
    return NULL;
}

static PyObject* PyMinqlxtended_LinkEntity(PyObject* self, PyObject* args) {
    int entity_id;
    if (!PyArg_ParseTuple(args, "i:link_entity", &entity_id)) {
        return NULL;
    }
    if (!qlx_vm_ready()) {
        return NULL;
    }
    if (SV_LinkEntity == NULL) {
        PyErr_SetString(qlx_EngineStateError, "SV_LinkEntity did not resolve in this build.");
        return NULL;
    }

    if (entity_id < 0 || entity_id >= ENTITYNUM_MAX_NORMAL) {
        PyErr_Format(PyExc_ValueError, "entity_id needs to be a number from 0 to %d.",
                     ENTITYNUM_MAX_NORMAL - 1);
        return NULL;
    }
    if (!g_entities[entity_id].inuse) {
        PyErr_Format(PyExc_ValueError, "entity %d is not in use.", entity_id);
        return NULL;
    }

    SV_LinkEntity((sharedEntity_t*)&g_entities[entity_id]);
    Py_RETURN_TRUE;
}

static PyObject* PyMinqlxtended_UnlinkEntity(PyObject* self, PyObject* args) {
    int entity_id;
    if (!PyArg_ParseTuple(args, "i:unlink_entity", &entity_id)) {
        return NULL;
    }
    if (!qlx_vm_ready()) {
        return NULL;
    }
    if (SV_UnlinkEntity == NULL) {
        PyErr_SetString(qlx_EngineStateError, "SV_UnlinkEntity did not resolve in this build.");
        return NULL;
    }

    if (entity_id < 0 || entity_id >= ENTITYNUM_MAX_NORMAL) {
        PyErr_Format(PyExc_ValueError, "entity_id needs to be a number from 0 to %d.",
                     ENTITYNUM_MAX_NORMAL - 1);
        return NULL;
    }
    if (!g_entities[entity_id].inuse) {
        PyErr_Format(PyExc_ValueError, "entity %d is not in use.", entity_id);
        return NULL;
    }

    SV_UnlinkEntity((sharedEntity_t*)&g_entities[entity_id]);
    Py_RETURN_TRUE;
}

// add_event

/* Raise an entity event: pain, gibs, teleport, item respawn. G_AddEvent does it. A repeated
 * event needs a rotating counter in the number or clients drop it as a duplicate; QL keeps
 * that counter where stock playerState_t has externalEventTime, in the field it calls
 * clientNum. */
static PyObject* PyMinqlxtended_AddEvent(PyObject* self, PyObject* args) {
    int entity_id, event, event_parm = 0;
    if (!PyArg_ParseTuple(args, "ii|i:add_event", &entity_id, &event, &event_parm)) {
        return NULL;
    }
    if (!qlx_vm_ready()) {
        return NULL;
    }

    if (entity_id < 0 || entity_id >= MAX_GENTITIES) {
        PyErr_Format(PyExc_ValueError, "entity_id needs to be a number from 0 to %d.",
                     MAX_GENTITIES - 1);
        return NULL;
    }

    // G_AddEvent writes ->eventTime, and ->client->ps for a player. A freed slot has
    // stale values in both.
    if (!g_entities[entity_id].inuse) {
        PyErr_Format(PyExc_ValueError, "entity %d is not in use.", entity_id);
        return NULL;
    }

    if (event <= EV_NONE || event >= EV_NUM_ETYPES) {
        PyErr_Format(PyExc_ValueError, "event needs to be a number from %d to %d.",
                     EV_NONE + 1, EV_NUM_ETYPES - 1);
        return NULL;
    }

    G_AddEvent(&g_entities[entity_id], event, event_parm);
    Py_RETURN_NONE;
}

// remove_dropped_items

static PyObject* PyMinqlxtended_RemoveDroppedItems(PyObject* self, PyObject* args) {
    int i;
    gentity_t* ent;

    if (!qlx_vm_ready()) {
        return NULL;
    }

    for (i = 0; i < MAX_GENTITIES; i++) {
        ent = &g_entities[i];
        if (!ent->inuse) {
            continue;
        }

        if (ent->flags & FL_DROPPED_ITEM) {
            G_FreeEntity(ent);
        }
    }
    Py_RETURN_TRUE;
}

// slay_with_mod

static PyObject* PyMinqlxtended_SlayWithMod(PyObject* self, PyObject* args) {
    int client_id, mod;
    if (!PyArg_ParseTuple(args, "ii:slay_with_mod", &client_id, &mod)) {
        return NULL;
    }

    if (!qlx_live_client(client_id)) {
        return NULL;
    }

    if (g_entities[client_id].health <= 0) {
        Py_RETURN_TRUE;
    }

    // Shared with the `slay` console command in commands.c, so both raise player_die and
    // the death and kill events.
    SlayPlayer(&g_entities[client_id], mod);
    Py_RETURN_TRUE;
}

// replace_items

void replace_item_core(gentity_t* ent, int item_id) {
    char csbuffer[4096];

    if (item_id) {
        ent->s.modelindex = item_id;
        ent->classname    = bg_itemlist[item_id].classname;
        ent->item         = &bg_itemlist[item_id];

        // this forces client to load new item
        SV_GetConfigstring(CS_ITEMS, csbuffer, sizeof(csbuffer));
        csbuffer[item_id] = '1';
        My_SV_SetConfigstring(CS_ITEMS, csbuffer);
    } else {
        G_FreeEntity(ent);
    }
}

static PyObject* PyMinqlxtended_ReplaceItems(PyObject* self, PyObject* args) {
    PyObject *arg1, *arg2;
    int entity_id = 0, item_id = 0;
    const char *entity_classname = NULL, *item_classname = NULL;
    gentity_t* ent;

    if (!PyArg_ParseTuple(args, "OO:replace_items", &arg1, &arg2)) {
        return NULL;
    }

    if (!qlx_vm_ready()) {
        return NULL;
    }

    // checking type of first arg
    if (PyLong_Check(arg1)) {
        if (!qlx_as_int(arg1, &entity_id, "entity")) {
            return NULL;
        }
    } else if (PyUnicode_Check(arg1)) {
        entity_classname = PyUnicode_AsUTF8(arg1);
        if (entity_classname == NULL) {
            return NULL; // Propagate the encoding error (e.g. lone surrogates).
        }
    } else {
        PyErr_SetString(PyExc_TypeError, "entity needs to be type of int or string.");
        return NULL;
    }

    // checking type of second arg
    if (PyLong_Check(arg2)) {
        if (!qlx_as_int(arg2, &item_id, "item")) {
            return NULL;
        }
    } else if (PyUnicode_Check(arg2)) {
        item_classname = PyUnicode_AsUTF8(arg2);
        // Must bail here. Falling through leaves item_id at 0, and
        // "item_id == 0 && item_classname == NULL" is the *remove* case below, so an
        // unencodable string would silently delete every matching item on the map.
        if (item_classname == NULL) {
            return NULL;
        }
    } else {
        PyErr_Format(PyExc_ValueError, "item needs to be type of int or string.");
        return NULL;
    }

    // convert second arg to item_id, if needed
    int i = 1;
    if (item_classname == NULL) {
        i = bg_numItems;
    }
    for (; i < bg_numItems; i++) {
        if (strcmp(bg_itemlist[i].classname, item_classname) == 0) {
            item_id = i;
            break;
        }
    }

    // checking for valid item_id or item_classname
    if (item_classname && item_id == 0) {
        // throw error if invalid item_classname
        PyErr_Format(PyExc_ValueError, "invalid item classname: %s.", item_classname);
        return NULL;
    } else if (item_id < 0 || item_id >= bg_numItems) {
        // throw error if invalid item_id
        PyErr_Format(PyExc_ValueError, "item_id needs to be between 0 and %d.", bg_numItems - 1);
        return NULL;
    }

    // Note: if item_id == 0 and item_classname == NULL, then item will be removed

    if (entity_classname == NULL) {
        // replacing item by entity_id

        // entity_id checking
        if (entity_id < 0 || entity_id >= MAX_GENTITIES) {
            PyErr_Format(PyExc_ValueError, "entity_id needs to be between 0 and %d.", MAX_GENTITIES - 1);
            return NULL;
        } else if (g_entities[entity_id].inuse == 0) {
            PyErr_Format(PyExc_ValueError, "entity #%d is not in use.", entity_id);
            return NULL;
        } else if (g_entities[entity_id].s.eType != ET_ITEM) {
            PyErr_Format(PyExc_ValueError, "entity #%d is not item. Cannot replace it.", entity_id);
            return NULL;
        }

        replace_item_core(&g_entities[entity_id], item_id);
        Py_RETURN_TRUE;
    } else {
        // replacing items by entity_classname

        int is_entity_found = 0;
        for (i = 0; i < MAX_GENTITIES; i++) {
            ent = &g_entities[i];

            if (!ent->inuse) {
                continue;
            }

            if (ent->s.eType != ET_ITEM) {
                continue;
            }

            if (strcmp(ent->classname, entity_classname) == 0) {
                is_entity_found = 1;
                replace_item_core(ent, item_id);
            }
        }

        if (is_entity_found) {
            Py_RETURN_TRUE;
        }

        Py_RETURN_FALSE;
    }
}

// dev_print_items

static PyObject* PyMinqlxtended_DevPrintItems(PyObject* self, PyObject* args) {
    if (!qlx_vm_ready()) {
        return NULL;
    }

    gentity_t* ent;
    char buffer[1024], temp_buffer[1024];
    int buffer_index = 0;
    size_t chars_written;
    char format[]             = "%d %s\n";
    qboolean is_buffer_enough = qtrue;

    // default results
    sprintf(buffer, "No items found in the map");

    for (int i = 0; i < MAX_GENTITIES; i++) {
        ent = &g_entities[i];

        if (!ent->inuse) {
            continue;
        }

        if (ent->s.eType != ET_ITEM) {
            continue;
        }

        // snprintf, since classname points into map-supplied spawn data. The return
        // is still the untruncated length, which the buffer arithmetic below relies on.
        chars_written = snprintf(temp_buffer, sizeof(temp_buffer), format, i, ent->classname);
        if (is_buffer_enough && buffer_index + chars_written >= sizeof(buffer)) {
            is_buffer_enough = qfalse;
            SV_SendServerCommand(NULL, "print \"%s\"", buffer);
            SV_SendServerCommand(NULL, "print \"%s\"\n", "Check server console for other items\n");
        }

        if (is_buffer_enough == qfalse) {
            Com_Printf(format, i, ent->classname);
        } else {
            chars_written = snprintf(&buffer[buffer_index], sizeof(buffer) - buffer_index, format, i, ent->classname);
            buffer_index += chars_written;
        }
    }

    if (is_buffer_enough) {
        SV_SendServerCommand(NULL, "print \"%s\"", buffer);
    }
    Py_RETURN_NONE;
}

// force_weapon_respawn_time

static PyObject* PyMinqlxtended_ForceWeaponRespawnTime(PyObject* self, PyObject* args) {
    int respawn_time;
    gentity_t* ent;

    if (!PyArg_ParseTuple(args, "i:force_weapon_respawn_time", &respawn_time)) {
        return NULL;
    }

    if (!qlx_vm_ready()) {
        return NULL;
    }

    if (respawn_time < 0) {
        PyErr_Format(PyExc_ValueError, "respawn time needs to be an integer 0 or greater");
        return NULL;
    }

    for (int i = 0; i < MAX_GENTITIES; i++) {
        ent = &g_entities[i];

        if (!ent->inuse) {
            continue;
        }

        if (ent->s.eType != ET_ITEM || ent->item == NULL) {
            continue;
        }

        if (ent->item->giType != IT_WEAPON) {
            continue;
        }

        ent->wait = respawn_time;
    }

    Py_RETURN_TRUE;
}

// player_expanded_stats

/* Wraps a 16-entry per-weapon array as a Weapons sequence; its 15 fields are indices 1..15. */
static PyObject* weapons_seq(const int* arr) {
    PyObject* seq = PyStructSequence_New(&weapons_type);
    if (seq == NULL) {
        return NULL;
    }
    for (int i = 0; i < weapons_desc.n_in_sequence; i++) {
        PyStructSequence_SetItem(seq, i, PyLong_FromLong(arr[i + 1]));
    }
    return seq;
}

/*
 * The per-weapon [16] arrays as a Weapons struct sequence. Exposed for python_objects.c so
 * GameClient.expanded_stats and player_expanded_stats() agree on the shape.
 */
PyObject* PyMinqlxtended_Weapons(const int* arr) {
    return weapons_seq(arr);
}

static PyObject* PyMinqlxtended_PlayerExpandedStats(PyObject* self, PyObject* args) {
    int client_id;

    if (!PyArg_ParseTuple(args, "i:player_expanded_stats", &client_id)) {
        return NULL;
    }

    if (!qlx_valid_client_id(client_id)) {
        return NULL;
    }

    if (!g_entities || !g_entities[client_id].client) {
        Py_RETURN_NONE;
    }

    PyObject* stats = PyStructSequence_New(&player_expanded_stats_type);
    if (stats == NULL) {
        return NULL;
    }

    expandedStatObj_t* es = &g_entities[client_id].client->expandedStats;

    PyStructSequence_SetItem(stats, 0, PyLong_FromLong(es->totalPlayTime));
    PyStructSequence_SetItem(stats, 1, PyLong_FromLong(es->serverRank));
    PyStructSequence_SetItem(stats, 2, PyBool_FromLong(es->serverRankIsTied));
    PyStructSequence_SetItem(stats, 3, PyLong_FromLong(es->teamRank));
    PyStructSequence_SetItem(stats, 4, PyBool_FromLong(es->teamRankIsTied));
    PyStructSequence_SetItem(stats, 5, PyLong_FromLong(es->numKills));
    PyStructSequence_SetItem(stats, 6, PyLong_FromLong(es->numDeaths));
    PyStructSequence_SetItem(stats, 7, PyLong_FromLong(es->numSuicides));
    PyStructSequence_SetItem(stats, 8, PyLong_FromLong(es->numTeamKills));
    PyStructSequence_SetItem(stats, 9, PyLong_FromLong(es->numTeamKilled));
    PyStructSequence_SetItem(stats, 10, PyLong_FromLong(es->totalDamageDealt));
    PyStructSequence_SetItem(stats, 11, PyLong_FromLong(es->totalDamageTaken));
    PyStructSequence_SetItem(stats, 12, PyLong_FromLong(es->numCaptures));
    PyStructSequence_SetItem(stats, 13, PyLong_FromLong(es->numAssists));
    PyStructSequence_SetItem(stats, 14, PyLong_FromLong(es->numDefends));
    PyStructSequence_SetItem(stats, 15, PyLong_FromLong(es->numHolyShits));
    PyStructSequence_SetItem(stats, 16, PyLong_FromLong(es->numDenials));
    PyStructSequence_SetItem(stats, 17, PyLong_FromLong(es->killStreak));
    PyStructSequence_SetItem(stats, 18, PyLong_FromLong(es->maxKillStreak));
    PyStructSequence_SetItem(stats, 19, PyLong_FromLong(es->numMidairShotgunKills));
    PyStructSequence_SetItem(stats, 20, PyLong_FromLong(es->numQuadDamageKills));
    PyStructSequence_SetItem(stats, 21, PyLong_FromLong(es->xp));
    PyStructSequence_SetItem(stats, 22, PyLong_FromLong(es->domThreeFlagsTime));

    const int* weapon_arrays[] = {es->numWeaponKills, es->numWeaponDeaths, es->shotsFired,
                                  es->shotsHit,       es->damageDealt,     es->damageTaken,
                                  es->weaponPickups,  es->weaponUsageTime};
    for (unsigned i = 0; i < sizeof(weapon_arrays) / sizeof(*weapon_arrays); i++) {
        PyObject* seq = weapons_seq(weapon_arrays[i]);
        if (seq == NULL) {
            Py_DECREF(stats);
            return NULL;
        }
        PyStructSequence_SetItem(stats, 23 + (int)i, seq);
    }

    PyStructSequence_SetItem(stats, 31, PyLong_FromLong(es->numAmmoPickups));
    PyStructSequence_SetItem(stats, 32, PyLong_FromLong(es->numHealthPickups));
    PyStructSequence_SetItem(stats, 33, PyLong_FromLong(es->numMegaHealthPickups));
    PyStructSequence_SetItem(stats, 34, PyLong_FromLong(es->numFirstMegaHealthPickups));
    PyStructSequence_SetItem(stats, 35, PyLong_FromLong(es->megaHealthPickupTime));
    PyStructSequence_SetItem(stats, 36, PyLong_FromLong(es->numArmorPickups));
    PyStructSequence_SetItem(stats, 37, PyLong_FromLong(es->numRedArmorPickups));
    PyStructSequence_SetItem(stats, 38, PyLong_FromLong(es->numFirstRedArmorPickups));
    PyStructSequence_SetItem(stats, 39, PyLong_FromLong(es->redArmorPickupTime));
    PyStructSequence_SetItem(stats, 40, PyLong_FromLong(es->numYellowArmorPickups));
    PyStructSequence_SetItem(stats, 41, PyLong_FromLong(es->numFirstYellowArmorPickups));
    PyStructSequence_SetItem(stats, 42, PyLong_FromLong(es->yellowArmorPickupTime));
    PyStructSequence_SetItem(stats, 43, PyLong_FromLong(es->numGreenArmorPickups));
    PyStructSequence_SetItem(stats, 44, PyLong_FromLong(es->numFirstGreenArmorPickups));
    PyStructSequence_SetItem(stats, 45, PyLong_FromLong(es->greenArmorPickupTime));
    PyStructSequence_SetItem(stats, 46, PyLong_FromLong(es->numQuadDamagePickups));
    PyStructSequence_SetItem(stats, 47, PyLong_FromLong(es->numBattleSuitPickups));
    PyStructSequence_SetItem(stats, 48, PyLong_FromLong(es->numRegenerationPickups));
    PyStructSequence_SetItem(stats, 49, PyLong_FromLong(es->numHastePickups));
    PyStructSequence_SetItem(stats, 50, PyLong_FromLong(es->numInvisibilityPickups));
    PyStructSequence_SetItem(stats, 51, PyLong_FromLong(es->numMedkitPickups));
    PyStructSequence_SetItem(stats, 52, PyLong_FromLong(es->numRedFlagPickups));
    PyStructSequence_SetItem(stats, 53, PyLong_FromLong(es->numBlueFlagPickups));
    PyStructSequence_SetItem(stats, 54, PyLong_FromLong(es->numNeutralFlagPickups));

    // powerups[] is indexed straight by powerup_t, so index 0 maps to field 0.
    PyObject* powerup_pickups = PyStructSequence_New(&stat_powerups_type);
    if (powerup_pickups == NULL) {
        Py_DECREF(stats);
        return NULL;
    }
    for (int i = 0; i < stat_powerups_desc.n_in_sequence; i++) {
        PyStructSequence_SetItem(powerup_pickups, i, PyLong_FromLong(es->powerups[i]));
    }
    PyStructSequence_SetItem(stats, 55, powerup_pickups);

    // holdablePickups[] skips HI_NONE, so field i is array index i + 1.
    PyObject* holdable_pickups = PyStructSequence_New(&stat_holdables_type);
    if (holdable_pickups == NULL) {
        Py_DECREF(stats);
        return NULL;
    }
    for (int i = 0; i < stat_holdables_desc.n_in_sequence; i++) {
        PyStructSequence_SetItem(holdable_pickups, i, PyLong_FromLong(es->holdablePickups[i + 1]));
    }
    PyStructSequence_SetItem(stats, 56, holdable_pickups);

    return stats;
}

// start_demo/stop_demo/demo_status

static PyObject* PyMinqlxtended_StartDemo(PyObject* self, PyObject* args) {
    int client_id;
    if (!PyArg_ParseTuple(args, "i:start_demo", &client_id)) {
        return NULL;
    }

    if (!qlx_on_game_thread("start_demo()") || !qlx_valid_client_id(client_id)) {
        return NULL;
    }

    if (!Demo_Request(client_id, 1)) {
        Py_RETURN_FALSE;
    }

    // A demo has to begin at a gamestate, so if the client is already in the game we
    // can only start at their next one. True means the demo is already being written; a
    // pending request still reads False.
    return PyBool_FromLong(Demo_IsRecording(client_id) == qtrue);
}

static PyObject* PyMinqlxtended_StopDemo(PyObject* self, PyObject* args) {
    int client_id;
    if (!PyArg_ParseTuple(args, "i:stop_demo", &client_id)) {
        return NULL;
    }

    if (!qlx_on_game_thread("stop_demo()") || !qlx_valid_client_id(client_id)) {
        return NULL;
    }

    qboolean was_recording = Demo_IsRecording(client_id);
    // -1 so this also overrides sv_demoRecord being on globally.
    if (!Demo_Request(client_id, -1)) {
        Py_RETURN_FALSE;
    }

    return PyBool_FromLong(was_recording == qtrue);
}

static PyObject* PyMinqlxtended_DemoStatus(PyObject* self, PyObject* args) {
    int client_id;
    if (!PyArg_ParseTuple(args, "i:demo_status", &client_id)) {
        return NULL;
    }

    if (!qlx_on_game_thread("demo_status()") || !qlx_valid_client_id(client_id)) {
        return NULL;
    }

    PyObject* status = PyStructSequence_New(&demo_status_type);
    if (status == NULL) {
        return NULL;
    }

    const char* path = Demo_GetPath(client_id);
    PyObject* py_path;
    if (path) {
        py_path = PyUnicode_FromString(path);
        if (py_path == NULL) {
            Py_DECREF(status);
            return NULL;
        }
    } else {
        py_path = Py_None;
        Py_INCREF(Py_None);
    }

    PyStructSequence_SetItem(status, 0, PyBool_FromLong(Demo_IsRecording(client_id) == qtrue));
    PyStructSequence_SetItem(status, 1, PyLong_FromLong(Demo_GetRequest(client_id)));
    PyStructSequence_SetItem(status, 2, py_path);

    return status;
}

// reliable_status

static PyObject* PyMinqlxtended_ReliableStatus(PyObject* self, PyObject* args) {
    if (!qlx_on_game_thread("reliable_status()")) {
        return NULL;
    }

    reliable_status_t rs;
    Reliable_Status(&rs);

    PyObject* status = PyStructSequence_New(&reliable_status_type);
    if (status == NULL) {
        return NULL;
    }

    PyStructSequence_SetItem(status, 0, PyBool_FromLong(rs.enabled));
    PyStructSequence_SetItem(status, 1, PyLong_FromLong(rs.watermark));
    PyStructSequence_SetItem(status, 2, PyLong_FromLong(rs.burst));
    PyStructSequence_SetItem(status, 3, PyLong_FromLong(rs.waiting));
    PyStructSequence_SetItem(status, 4, PyLong_FromUnsignedLong(rs.queued));
    PyStructSequence_SetItem(status, 5, PyLong_FromUnsignedLong(rs.merged));
    PyStructSequence_SetItem(status, 6, PyLong_FromUnsignedLong(rs.bypassed));
    PyStructSequence_SetItem(status, 7, PyLong_FromLong(rs.backlog));
    PyStructSequence_SetItem(status, 8, PyLong_FromLong(rs.worst_backlog));
    PyStructSequence_SetItem(status, 9, PyLong_FromLong(rs.worst_slot));

    return status;
}

// Module definition and initialization

static PyMethodDef minqlxtendedMethods[] = {
    {"player_info", PyMinqlxtended_PlayerInfo, METH_VARARGS,
     "player_info(client_id) -- a PlayerInfo for that slot, or None if nobody is in it.\n\n"
     "A struct sequence: index it, unpack it, or read its fields by "
     "name (client_id, name, connection_state, userinfo, steam_id, team, privileges)."},
    {"players_info", PyMinqlxtended_PlayersInfo, METH_NOARGS,
     "players_info() -- a PlayerInfo per client slot, with None for each empty one.\n\n"
     "The list is always sv_maxclients long, so the index is the client id."},
    {"get_userinfo", PyMinqlxtended_GetUserinfo, METH_VARARGS,
     "Returns a string with a player's userinfo."},
    {"send_server_command", PyMinqlxtended_SendServerCommand, METH_VARARGS,
     "Sends a server command to either one specific client or all the clients."},
    {"client_command", PyMinqlxtended_ClientCommand, METH_VARARGS,
     "Tells the server to process a command from a specific client."},
    {"console_command", PyMinqlxtended_ConsoleCommand, METH_VARARGS,
     "console_command(cmd) -- run a command as if from the server console.\n\n"
     "It has run by the time this returns, except for the seven that reload the game module\n"
     "(map, devmap, arena, map_restart, startRandomMap, killserver, quit) and calls from a\n"
     "thread other than the game's, which run on a later frame."},
    {"get_cvar", PyMinqlxtended_GetCvar, METH_VARARGS,
     "Gets a cvar."},
    {"set_cvar", (PyCFunction)(void (*)(void))PyMinqlxtended_SetCvar,
     METH_VARARGS | METH_KEYWORDS,
     "set_cvar(name, value, flags=0, force=False) -- set a cvar, creating it if it does "
     "not exist. force overrides CVAR_ROM, CVAR_INIT and a pending latch. Returns the "
     "Cvar, so the value the engine settled on can be read back."},
    {"set_cvar_limit", PyMinqlxtended_SetCvarLimit, METH_VARARGS,
     "Sets a non-string cvar with a minimum and maximum value."},
    {"kick", PyMinqlxtended_Kick, METH_VARARGS,
     "Kick a player and allowing the admin to supply a reason for it."},
    {"console_print", PyMinqlxtended_ConsolePrint, METH_VARARGS,
     "Prints text on the console. If used during an RCON command, it will be printed in the player's console."},
    {"get_configstring", PyMinqlxtended_GetConfigstring, METH_VARARGS,
     "Get a configstring."},
    {"set_configstring", PyMinqlxtended_SetConfigstring, METH_VARARGS,
     "Sets a configstring and sends it to all the players on the server."},
    {"force_vote", PyMinqlxtended_ForceVote, METH_VARARGS,
     "Forces the current vote to either fail or pass."},
    {"add_console_command", PyMinqlxtended_AddConsoleCommand, METH_VARARGS,
     "Adds a console command that will be handled by Python code."},
    {"register_handler", PyMinqlxtended_RegisterHandler, METH_VARARGS,
     "Register an event handler. Can be called more than once per event, but only the last one will work."},
    {"entities", (PyCFunction)(void (*)(void))PyMinqlxtended_Entities,
     METH_VARARGS | METH_KEYWORDS,
     "entities(inuse=True, etype=None, start=0, stop=MAX_GENTITIES, classname=None) -- "
     "iterate the game module's entities.\n\n"
     "Lazy, so breaking out early costs nothing. Freed slots are skipped by default: "
     "G_FreeEntity does not clear classname, so reading one is a stale pointer. Pass an "
     "ET_* value as etype, or a classname string, to filter in C instead of in the loop "
     "body. The classname match is case-insensitive and only ever matches in-use slots."},
    {"items", PyMinqlxtended_Items, METH_NOARGS,
     "items() -- iterate the game module's item table. Yields Item objects; the null item "
     "at index 0 is skipped."},
    {"cvar", PyMinqlxtended_Cvar, METH_VARARGS,
     "cvar(name) -- the console variable as a Cvar, or None if there is no such cvar. "
     "Unlike get_cvar() this carries the flags, defaults and latched value too."},
    {"cvars", PyMinqlxtended_Cvars, METH_NOARGS,
     "cvars() -- iterate every console variable the engine knows about, as Cvar objects. "
     "Empty if the cvar list head could not be resolved."},
    {"player_state", PyMinqlxtended_PlayerState, METH_VARARGS,
     "Get information about the player's state in the game."},
    {"player_stats", PyMinqlxtended_PlayerStats, METH_VARARGS,
     "Get some player stats."},
    /* No per-client setters here. Writable fields are accessors on Entity, GameClient and
     * level; what follows is what those views cannot express. */
    {"drop_holdable", PyMinqlxtended_DropHoldable, METH_VARARGS,
     "Drops player's holdable item."},
    {"callvote", PyMinqlxtended_Callvote, METH_VARARGS,
     "Calls a vote as if started by the server and not a player. The optional caller_id "
     "is what the vote_started event reports; -1, the default, means nobody."},
    {"player_spawn", PyMinqlxtended_PlayerSpawn, METH_VARARGS,
     "Forces the player to spawn in the map (no matter the team!)"},
    {"destroy_kamikaze_timers", PyMinqlxtended_DestroyKamikazeTimers, METH_NOARGS,
     "Removes all current kamikaze timers."},
    {"spawn_item", PyMinqlxtended_SpawnItem, METH_VARARGS,
     "Spawns item with specified coordinates."},
    {"add_event", PyMinqlxtended_AddEvent, METH_VARARGS,
     "Raises an EntityEvent on an entity, so clients play the sound or draw the effect it "
     "stands for. event_parm is the event's argument and means something different per "
     "event; it defaults to 0."},
    {"remove_dropped_items", PyMinqlxtended_RemoveDroppedItems, METH_NOARGS,
     "Removes all dropped items."},
    {"slay_with_mod", PyMinqlxtended_SlayWithMod, METH_VARARGS,
     "Slay player with mean of death."},
    {"replace_items", PyMinqlxtended_ReplaceItems, METH_VARARGS,
     "Replaces target entity's item with specified one."},
    {"dev_print_items", PyMinqlxtended_DevPrintItems, METH_NOARGS,
     "Prints all items and entity numbers to server console."},
    {"force_weapon_respawn_time", PyMinqlxtended_ForceWeaponRespawnTime, METH_VARARGS,
     "Force all weapons to have a specified respawn time, overriding custom map respawn times set for them."},
    {"player_expanded_stats", PyMinqlxtended_PlayerExpandedStats, METH_VARARGS,
     "Get the full set of detailed stats QL tracks for a player this match."},
    {"start_demo", PyMinqlxtended_StartDemo, METH_VARARGS,
     "Records a demo of the player regardless of sv_demoRecord. Recording begins at the client's next gamestate."},
    {"stop_demo", PyMinqlxtended_StopDemo, METH_VARARGS,
     "Stops recording the player and finalises any open demo, even if sv_demoRecord is on."},
    {"demo_status", PyMinqlxtended_DemoStatus, METH_VARARGS,
     "Returns the player's demo recording state."},
    {"reliable_status", PyMinqlxtended_ReliableStatus, METH_NOARGS,
     "reliable_status() -- a ReliableStatus snapshot of the reliable command channel.\n\n"
     "The backlog field is the deepest live per-client backlog out of the 64-slot ring; "
     "a plugin about to mass-message can pace itself against it."},
    {"drop_item", PyMinqlxtended_DropItem, METH_VARARGS,
     "drop_item(client_id, item_id, angle=0.0) -- launch a dropped copy of the item "
     "from the player, returning the new entity's id, or None if nothing spawned.\n\n"
     "The player's inventory is untouched: a drop that should also take the item away "
     "clears the matching GameClient field itself."},
    {"remove_entity", PyMinqlxtended_RemoveEntity, METH_VARARGS,
     "remove_entity(entity_id) -- free a map entity's slot, as G_FreeEntity does.\n\n"
     "Client slots, the world, never_free entities and already-freed slots are refused. "
     "Other entities' references to this one (enemy, parent, teamchain, target_ent) are "
     "not cleared, so freeing an entity another one depends on (a mover's train corner, a "
     "trigger's target) leaves that logic pointing at a freed slot. Game thread only."},
    {"spawn_entity", PyMinqlxtended_SpawnEntity, METH_VARARGS,
     "spawn_entity(classname, keys=None) -- spawn a map entity through the engine's own "
     "spawn machinery, as if it had been in the map's entity text.\n\n"
     "keys is the entity's spawn dictionary: {'origin': (x, y, z), 'angle': 90, "
     "'wait': 5}; values may be strings, numbers or 3-sequences. Item classnames go "
     "through G_SpawnItem and appear two frames later; everything else runs its spawn "
     "function immediately. Returns the new Entity, or None when the engine filtered it "
     "out (gametype keys, item rules, or an unknown classname), which it prints to the "
     "server console. Game thread only."},
    {"link_entity", PyMinqlxtended_LinkEntity, METH_VARARGS,
     "link_entity(entity_id) -- (re)link an entity into the world, as trap_LinkEntity "
     "does.\n\n"
     "Required after moving a solid, trigger or visible entity for its collision and "
     "networked position to follow: write s.origin, s.pos_base and r.current_origin, "
     "then link. Spawn points never need it; spawn selection reads s.origin directly. "
     "Game thread only."},
    {"unlink_entity", PyMinqlxtended_UnlinkEntity, METH_VARARGS,
     "unlink_entity(entity_id) -- take an entity out of the world, as trap_UnlinkEntity "
     "does.\n\n"
     "Unlinked entities stop colliding and stop being sent to clients; r.linked reads "
     "back False. Game thread only."},
    {NULL, NULL, 0, NULL}};

// "_minqlxtended" matches PyImport_AppendInittab. m_name becomes __name__, and if it disagrees
// with the sys.modules key then tracebacks and help() name the wrong module. The types keep
// tp_name = "minqlxtended.X", which is where they are reached from.
static PyModuleDef minqlxtendedModule = {
    PyModuleDef_HEAD_INIT, "_minqlxtended", NULL, -1, minqlxtendedMethods,
    NULL, NULL, NULL, NULL};

// _fields and _replace: pyStructSequence exposes no field names and no way to copy with one field changed.

static PyObject* PyMinqlxtended_StructSeqReplace(PyObject* self, PyObject* args,
                                                 PyObject* kwargs) {
    if (PyTuple_GET_SIZE(args) != 0) {
        PyErr_SetString(PyExc_TypeError,
                        "_replace() takes no positional arguments; name the fields.");
        return NULL;
    }

    PyObject* fields = PyDict_GetItemString(Py_TYPE(self)->tp_dict, "_fields");
    if (!fields) {
        PyErr_Format(PyExc_TypeError, "%s has no _fields.", Py_TYPE(self)->tp_name);
        return NULL;
    }

    Py_ssize_t count  = PyTuple_GET_SIZE(fields);
    PyObject* values  = PyTuple_New(count);
    if (!values) {
        return NULL;
    }

    // Struct sequences are tuple subclasses, so the current values are a borrowed read.
    Py_ssize_t replaced = 0;
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject* name  = PyTuple_GET_ITEM(fields, i);
        PyObject* value = kwargs ? PyDict_GetItem(kwargs, name) : NULL;
        if (value) {
            replaced++;
        } else {
            if (PyErr_Occurred()) { // PyDict_GetItem swallows errors, but be sure.
                Py_DECREF(values);
                return NULL;
            }
            value = PyTuple_GET_ITEM(self, i);
        }

        Py_INCREF(value);
        PyTuple_SET_ITEM(values, i, value);
    }

    // Anything left over was not a field. Naming it beats "unexpected keyword argument".
    if (kwargs && replaced != PyDict_Size(kwargs)) {
        PyObject *key, *unused;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &unused)) {
            int known = PySequence_Contains(fields, key);
            if (known < 0) {
                Py_DECREF(values);
                return NULL;
            }
            if (!known) {
                // TypeError, matching namedtuple._replace on CPython 3.13 and later.
                // ValueError would make the exception depend on which interpreter the
                // server was built against.
                PyErr_Format(PyExc_TypeError, "%s got unexpected field name %R; it has %R",
                             Py_TYPE(self)->tp_name, key, fields);
                Py_DECREF(values);
                return NULL;
            }
        }
    }

    PyObject* result = PyObject_CallFunctionObjArgs((PyObject*)Py_TYPE(self), values, NULL);
    Py_DECREF(values);
    return result;
}

static PyMethodDef structseq_replace_def = {
    "_replace", (PyCFunction)(void*)PyMinqlxtended_StructSeqReplace,
    METH_VARARGS | METH_KEYWORDS,
    "_replace(**fields) -- a copy of this one with the named fields changed.\n\n"
    "    player.weapons = player.weapons._replace(rl=True, rg=True)\n\n"
    "Same contract as namedtuple._replace. A field name that does not exist is a "
    "TypeError, and the message names the field and lists the real ones."};

/* Attaches _fields and _replace to a struct sequence type. Returns 0, or -1 with an exception
 * set. Only the fields in the sequence: n_in_sequence can be shorter than the full field list,
 * and only the visible ones can go back to the constructor. */
static int AddStructSeqExtras(PyTypeObject* type, PyStructSequence_Desc* desc) {
    PyObject* fields = PyTuple_New(desc->n_in_sequence);
    if (!fields) {
        return -1;
    }

    for (Py_ssize_t i = 0; i < desc->n_in_sequence; i++) {
        const char* name = desc->fields[i].name;
        if (!name) {
            // Nothing here has unnamed fields, and _replace could not address one.
            // Compared against NULL. PyStructSequence_UnnamedField is declared in
            // structseq.h but isn't exported from libpython on every build.
            PyErr_Format(PyExc_SystemError, "%s field %zd is unnamed.", desc->name, i);
            Py_DECREF(fields);
            return -1;
        }

        PyObject* py_name = PyUnicode_InternFromString(name);
        if (!py_name) {
            Py_DECREF(fields);
            return -1;
        }

        PyTuple_SET_ITEM(fields, i, py_name);
    }

    int failed = PyDict_SetItemString(type->tp_dict, "_fields", fields);
    Py_DECREF(fields);
    if (failed) {
        return -1;
    }

    PyObject* replace = PyDescr_NewMethod(type, &structseq_replace_def);
    if (!replace) {
        return -1;
    }

    failed = PyDict_SetItemString(type->tp_dict, "_replace", replace);
    Py_DECREF(replace);
    if (failed) {
        return -1;
    }

    PyType_Modified(type);
    return 0;
}

static PyObject* PyMinqlxtended_InitModule(void) {
    PyObject* module = PyModule_Create(&minqlxtendedModule);
    // Every PyModule_Add* below dereferences it. A NULL here returns to the import
    // machinery, which raises ImportError in the loader script.
    if (!module) {
        return NULL;
    }

    // Set minqlxtended version.
    PyModule_AddStringConstant(module, "__version__", MINQLXTENDED_VERSION);

// Set IS_DEBUG.
#ifndef NDEBUG
    Py_INCREF(Py_True); // PyModule_AddObject steals a reference on success.
    PyModule_AddObject(module, "DEBUG", Py_True);
#else
    Py_INCREF(Py_False); // PyModule_AddObject steals a reference on success.
    PyModule_AddObject(module, "DEBUG", Py_False);
#endif

    // Here rather than in Python, so a function using one as a keyword default never depends on
    // the export order.
    PyModule_AddIntMacro(module, RET_NONE);
    PyModule_AddIntMacro(module, RET_STOP);
    PyModule_AddIntMacro(module, RET_STOP_EVENT);
    PyModule_AddIntMacro(module, RET_STOP_ALL);
    PyModule_AddIntMacro(module, RET_USAGE);
    PyModule_AddIntMacro(module, PRI_HIGHEST);
    PyModule_AddIntMacro(module, PRI_HIGH);
    PyModule_AddIntMacro(module, PRI_NORMAL);
    PyModule_AddIntMacro(module, PRI_LOW);
    PyModule_AddIntMacro(module, PRI_LOWEST);

    // Cvar flags.
    PyModule_AddIntMacro(module, CVAR_ARCHIVE);
    PyModule_AddIntMacro(module, CVAR_USERINFO);
    PyModule_AddIntMacro(module, CVAR_SERVERINFO);
    PyModule_AddIntMacro(module, CVAR_SYSTEMINFO);
    PyModule_AddIntMacro(module, CVAR_INIT);
    PyModule_AddIntMacro(module, CVAR_LATCH);
    PyModule_AddIntMacro(module, CVAR_ROM);
    PyModule_AddIntMacro(module, CVAR_USER_CREATED);
    PyModule_AddIntMacro(module, CVAR_TEMP);
    PyModule_AddIntMacro(module, CVAR_CHEAT);
    PyModule_AddIntMacro(module, CVAR_NORESTART);

    // Privileges.
    PyModule_AddIntMacro(module, PRIV_NONE);
    PyModule_AddIntMacro(module, PRIV_MOD);
    PyModule_AddIntMacro(module, PRIV_ADMIN);
    PyModule_AddIntMacro(module, PRIV_ROOT);
    PyModule_AddIntMacro(module, PRIV_BANNED);

    // Connection states.
    PyModule_AddIntMacro(module, CS_FREE);
    PyModule_AddIntMacro(module, CS_ZOMBIE);
    PyModule_AddIntMacro(module, CS_CONNECTED);
    PyModule_AddIntMacro(module, CS_PRIMED);
    PyModule_AddIntMacro(module, CS_ACTIVE);

    // Server states, for minqlxtended.server.state. SS_GAME is a map up and running;
    // SS_LOADING is one being spawned, which is when the configstring table is repopulated.
    PyModule_AddIntMacro(module, SS_DEAD);
    PyModule_AddIntMacro(module, SS_LOADING);
    PyModule_AddIntMacro(module, SS_GAME);

    // Teams.
    PyModule_AddIntMacro(module, TEAM_FREE);
    PyModule_AddIntMacro(module, TEAM_RED);
    PyModule_AddIntMacro(module, TEAM_BLUE);
    PyModule_AddIntMacro(module, TEAM_SPECTATOR);

    // Weapons. WP_NONE and the WP_NUM_WEAPONS sentinel are left out.
    PyModule_AddIntMacro(module, WP_GAUNTLET);
    PyModule_AddIntMacro(module, WP_MACHINEGUN);
    PyModule_AddIntMacro(module, WP_SHOTGUN);
    PyModule_AddIntMacro(module, WP_GRENADE_LAUNCHER);
    PyModule_AddIntMacro(module, WP_ROCKET_LAUNCHER);
    PyModule_AddIntMacro(module, WP_LIGHTNING);
    PyModule_AddIntMacro(module, WP_RAILGUN);
    PyModule_AddIntMacro(module, WP_PLASMAGUN);
    PyModule_AddIntMacro(module, WP_BFG);
    PyModule_AddIntMacro(module, WP_GRAPPLING_HOOK);
    PyModule_AddIntMacro(module, WP_NAILGUN);
    PyModule_AddIntMacro(module, WP_PROX_LAUNCHER);
    PyModule_AddIntMacro(module, WP_CHAINGUN);
    PyModule_AddIntMacro(module, WP_HMG);
    PyModule_AddIntMacro(module, WP_HANDS);

    // Entity types, for entities()' etype filter and Entity.s.e_type.
    PyModule_AddIntMacro(module, MAX_GENTITIES);
    PyModule_AddIntMacro(module, ET_GENERAL);
    PyModule_AddIntMacro(module, ET_PLAYER);
    PyModule_AddIntMacro(module, ET_ITEM);
    PyModule_AddIntMacro(module, ET_MISSILE);
    PyModule_AddIntMacro(module, ET_MOVER);
    PyModule_AddIntMacro(module, ET_BEAM);
    PyModule_AddIntMacro(module, ET_PORTAL);
    PyModule_AddIntMacro(module, ET_SPEAKER);
    PyModule_AddIntMacro(module, ET_PUSH_TRIGGER);
    PyModule_AddIntMacro(module, ET_TELEPORT_TRIGGER);
    PyModule_AddIntMacro(module, ET_INVISIBLE);
    PyModule_AddIntMacro(module, ET_GRAPPLE);
    PyModule_AddIntMacro(module, ET_TEAM);
    PyModule_AddIntMacro(module, ET_EVENTS);

    /* Three flag families on three fields: SVF_* is entityShared_t.svFlags, FL_* is
     * gentity_t.flags, and EF_* is eFlags on both entityState_t and playerState_t. */

    // Server flags, on Entity.r.sv_flags. SVF_BOT is the engine's own bot test.
    PyModule_AddIntMacro(module, SVF_NOCLIENT);
    PyModule_AddIntMacro(module, SVF_CLIENTMASK);
    PyModule_AddIntMacro(module, SVF_BOT);
    PyModule_AddIntMacro(module, SVF_BROADCAST);
    PyModule_AddIntMacro(module, SVF_PORTAL);
    PyModule_AddIntMacro(module, SVF_USE_CURRENT_ORIGIN);
    PyModule_AddIntMacro(module, SVF_SINGLECLIENT);
    PyModule_AddIntMacro(module, SVF_NOSERVERINFO);
    PyModule_AddIntMacro(module, SVF_CAPSULE);
    PyModule_AddIntMacro(module, SVF_NOTSINGLECLIENT);

    // Entity flags, on Entity.flags. God mode and notarget are bits in here.
    PyModule_AddIntMacro(module, FL_GODMODE);
    PyModule_AddIntMacro(module, FL_NOTARGET);
    PyModule_AddIntMacro(module, FL_TEAMSLAVE);
    PyModule_AddIntMacro(module, FL_NO_KNOCKBACK);
    PyModule_AddIntMacro(module, FL_DROPPED_ITEM);
    PyModule_AddIntMacro(module, FL_NO_BOTS);
    PyModule_AddIntMacro(module, FL_NO_HUMANS);
    PyModule_AddIntMacro(module, FL_FORCE_GESTURE);

    // Entity effects, on Entity.s.e_flags and GameClient(n).ps.e_flags. EF_PLAYER_EVENT
    // goes in before EF_BOUNCE: both are 0x10, and whichever lands first is the canonical
    // member. Players are the common case here.
    PyModule_AddIntMacro(module, EF_DEAD);
    PyModule_AddIntMacro(module, EF_TICKING);
    PyModule_AddIntMacro(module, EF_TELEPORT_BIT);
    PyModule_AddIntMacro(module, EF_AWARD_EXCELLENT);
    PyModule_AddIntMacro(module, EF_PLAYER_EVENT);
    PyModule_AddIntMacro(module, EF_BOUNCE);
    PyModule_AddIntMacro(module, EF_BOUNCE_HALF);
    PyModule_AddIntMacro(module, EF_AWARD_GAUNTLET);
    PyModule_AddIntMacro(module, EF_NODRAW);
    PyModule_AddIntMacro(module, EF_FIRING);
    PyModule_AddIntMacro(module, EF_KAMIKAZE);
    PyModule_AddIntMacro(module, EF_MOVER_STOP);
    PyModule_AddIntMacro(module, EF_AWARD_CAP);
    PyModule_AddIntMacro(module, EF_TALK);
    PyModule_AddIntMacro(module, EF_CONNECTION);
    PyModule_AddIntMacro(module, EF_VOTED);
    PyModule_AddIntMacro(module, EF_AWARD_IMPRESSIVE);
    PyModule_AddIntMacro(module, EF_AWARD_DEFEND);
    PyModule_AddIntMacro(module, EF_AWARD_ASSIST);
    PyModule_AddIntMacro(module, EF_AWARD_DENIED);
    PyModule_AddIntMacro(module, EF_TEAMVOTED);

    /* Indices into the three arrays on playerState_t: stats, persistant and powerups. */

    // statIndex_t, indexing ps.stats. Armour, the weapon and key bitfields, the holdable
    // and the four flight parameters all live here.
    PyModule_AddIntMacro(module, STAT_HEALTH);
    PyModule_AddIntMacro(module, STAT_HOLDABLE_ITEM);
    PyModule_AddIntMacro(module, STAT_RUNE);
    PyModule_AddIntMacro(module, STAT_WEAPONS);
    PyModule_AddIntMacro(module, STAT_ARMOR);
    PyModule_AddIntMacro(module, STAT_BSKILL);
    PyModule_AddIntMacro(module, STAT_CLIENTS_READY);
    PyModule_AddIntMacro(module, STAT_MAX_HEALTH);
    PyModule_AddIntMacro(module, STAT_SPINUP);
    PyModule_AddIntMacro(module, STAT_FLIGHT_THRUST);
    PyModule_AddIntMacro(module, STAT_MAX_FLIGHT_FUEL);
    PyModule_AddIntMacro(module, STAT_CUR_FLIGHT_FUEL);
    PyModule_AddIntMacro(module, STAT_FLIGHT_REFUEL);
    PyModule_AddIntMacro(module, STAT_QUADKILLS);
    PyModule_AddIntMacro(module, STAT_ARMORTYPE);
    PyModule_AddIntMacro(module, STAT_KEY);
    PyModule_AddIntMacro(module, STAT_OTHER_HEALTH);
    PyModule_AddIntMacro(module, STAT_OTHER_ARMOR);

    // Indices into ps.persistant. ROUND_SCORE is the score; the rest are medal counters.
    PyModule_AddIntMacro(module, PERS_ROUND_SCORE);
    PyModule_AddIntMacro(module, PERS_COMBOKILL_COUNT);
    PyModule_AddIntMacro(module, PERS_RAMPAGE_COUNT);
    PyModule_AddIntMacro(module, PERS_MIDAIR_COUNT);
    PyModule_AddIntMacro(module, PERS_REVENGE_COUNT);
    PyModule_AddIntMacro(module, PERS_PERFORATED_COUNT);
    PyModule_AddIntMacro(module, PERS_HEADSHOT_COUNT);
    PyModule_AddIntMacro(module, PERS_ACCURACY_COUNT);
    PyModule_AddIntMacro(module, PERS_QUADGOD_COUNT);
    PyModule_AddIntMacro(module, PERS_FIRSTFRAG_COUNT);
    PyModule_AddIntMacro(module, PERS_PERFECT_COUNT);

    // powerup_t, indexing ps.powerups. Each slot holds the level time the powerup expires at,
    // or 0 for not held. PW_NONE and PW_SPAWNARMOR are both 0 and PW_NUM_POWERUPS is a
    // sentinel, so all three are left out. The four NOTPW_* persistent powerups occupy slots
    // 11-14 of the same array without sharing the prefix, so index those by number.
    PyModule_AddIntMacro(module, PW_REDFLAG);
    PyModule_AddIntMacro(module, PW_BLUEFLAG);
    PyModule_AddIntMacro(module, PW_NEUTRALFLAG);
    PyModule_AddIntMacro(module, PW_QUAD);
    PyModule_AddIntMacro(module, PW_BATTLESUIT);
    PyModule_AddIntMacro(module, PW_HASTE);
    PyModule_AddIntMacro(module, PW_INVIS);
    PyModule_AddIntMacro(module, PW_REGEN);
    PyModule_AddIntMacro(module, PW_FLIGHT);
    PyModule_AddIntMacro(module, PW_INVULNERABILITY);
    PyModule_AddIntMacro(module, PW_FREEZE);

    // keys_t, the bits of ps.stats[STAT_KEY].
    PyModule_AddIntMacro(module, KEY_SILVER);
    PyModule_AddIntMacro(module, KEY_GOLD);
    PyModule_AddIntMacro(module, KEY_MASTER);

    // Mover states, on Entity.mover_state.
    PyModule_AddIntMacro(module, MOVER_POS1);
    PyModule_AddIntMacro(module, MOVER_POS2);
    PyModule_AddIntMacro(module, MOVER_1TO2);
    PyModule_AddIntMacro(module, MOVER_2TO1);

    // Trajectory types, on Entity.s.pos_type and apos_type.
    PyModule_AddIntMacro(module, TR_STATIONARY);
    PyModule_AddIntMacro(module, TR_INTERPOLATE);
    PyModule_AddIntMacro(module, TR_LINEAR);
    PyModule_AddIntMacro(module, TR_LINEAR_STOP);
    PyModule_AddIntMacro(module, TR_SINE);
    PyModule_AddIntMacro(module, TR_GRAVITY);
    PyModule_AddIntMacro(module, TR_CUSTOM_GRAVITY);

    // G_Say's mode, as handed to the chat handler.
    PyModule_AddIntMacro(module, SAY_ALL);
    PyModule_AddIntMacro(module, SAY_TEAM);
    PyModule_AddIntMacro(module, SAY_TELL);

    // Damage flags, as carried by the damage event's dflags argument. A bitfield, so
    // test with &.
    PyModule_AddIntMacro(module, DAMAGE_RADIUS);
    PyModule_AddIntMacro(module, DAMAGE_NO_ARMOR);
    PyModule_AddIntMacro(module, DAMAGE_NO_KNOCKBACK);
    PyModule_AddIntMacro(module, DAMAGE_NO_PROTECTION);
    PyModule_AddIntMacro(module, DAMAGE_NO_TEAM_PROTECTION);

    // Means of death, carried by the death and kill events.
    // minqlxtended.MeansOfDeath.from_index() maps these to the names those events report.
    PyModule_AddIntMacro(module, MOD_UNKNOWN);
    PyModule_AddIntMacro(module, MOD_SHOTGUN);
    PyModule_AddIntMacro(module, MOD_GAUNTLET);
    PyModule_AddIntMacro(module, MOD_MACHINEGUN);
    PyModule_AddIntMacro(module, MOD_GRENADE);
    PyModule_AddIntMacro(module, MOD_GRENADE_SPLASH);
    PyModule_AddIntMacro(module, MOD_ROCKET);
    PyModule_AddIntMacro(module, MOD_ROCKET_SPLASH);
    PyModule_AddIntMacro(module, MOD_PLASMA);
    PyModule_AddIntMacro(module, MOD_PLASMA_SPLASH);
    PyModule_AddIntMacro(module, MOD_RAILGUN);
    PyModule_AddIntMacro(module, MOD_LIGHTNING);
    PyModule_AddIntMacro(module, MOD_BFG);
    PyModule_AddIntMacro(module, MOD_BFG_SPLASH);
    PyModule_AddIntMacro(module, MOD_WATER);
    PyModule_AddIntMacro(module, MOD_SLIME);
    PyModule_AddIntMacro(module, MOD_LAVA);
    PyModule_AddIntMacro(module, MOD_CRUSH);
    PyModule_AddIntMacro(module, MOD_TELEFRAG);
    PyModule_AddIntMacro(module, MOD_FALLING);
    PyModule_AddIntMacro(module, MOD_SUICIDE);
    PyModule_AddIntMacro(module, MOD_TARGET_LASER);
    PyModule_AddIntMacro(module, MOD_TRIGGER_HURT);
    PyModule_AddIntMacro(module, MOD_NAIL);
    PyModule_AddIntMacro(module, MOD_CHAINGUN);
    PyModule_AddIntMacro(module, MOD_PROXIMITY_MINE);
    PyModule_AddIntMacro(module, MOD_KAMIKAZE);
    PyModule_AddIntMacro(module, MOD_JUICED);
    PyModule_AddIntMacro(module, MOD_GRAPPLE);
    PyModule_AddIntMacro(module, MOD_SWITCH_TEAMS);
    PyModule_AddIntMacro(module, MOD_THAW);
    PyModule_AddIntMacro(module, MOD_LIGHTNING_DISCHARGE);
    PyModule_AddIntMacro(module, MOD_HMG);
    PyModule_AddIntMacro(module, MOD_RAILGUN_HEADSHOT);

    // Entity events, what add_event() raises. EV_NONE and the EV_NUM_ETYPES sentinel are
    // left out, the same as WP_NONE above.
    PyModule_AddIntMacro(module, EV_FOOTSTEP);
    PyModule_AddIntMacro(module, EV_FOOTSTEP_METAL);
    PyModule_AddIntMacro(module, EV_FOOTSPLASH);
    PyModule_AddIntMacro(module, EV_FOOTWADE);
    PyModule_AddIntMacro(module, EV_SWIM);
    PyModule_AddIntMacro(module, EV_FALL_SHORT);
    PyModule_AddIntMacro(module, EV_FALL_MEDIUM);
    PyModule_AddIntMacro(module, EV_FALL_FAR);
    PyModule_AddIntMacro(module, EV_JUMP_PAD);
    PyModule_AddIntMacro(module, EV_JUMP);
    PyModule_AddIntMacro(module, EV_WATER_TOUCH);
    PyModule_AddIntMacro(module, EV_WATER_LEAVE);
    PyModule_AddIntMacro(module, EV_WATER_UNDER);
    PyModule_AddIntMacro(module, EV_WATER_CLEAR);
    PyModule_AddIntMacro(module, EV_ITEM_PICKUP);
    PyModule_AddIntMacro(module, EV_GLOBAL_ITEM_PICKUP);
    PyModule_AddIntMacro(module, EV_NOAMMO);
    PyModule_AddIntMacro(module, EV_CHANGE_WEAPON);
    PyModule_AddIntMacro(module, EV_DROP_WEAPON);
    PyModule_AddIntMacro(module, EV_FIRE_WEAPON);
    PyModule_AddIntMacro(module, EV_USE_ITEM0);
    PyModule_AddIntMacro(module, EV_USE_ITEM1);
    PyModule_AddIntMacro(module, EV_USE_ITEM2);
    PyModule_AddIntMacro(module, EV_USE_ITEM3);
    PyModule_AddIntMacro(module, EV_USE_ITEM4);
    PyModule_AddIntMacro(module, EV_USE_ITEM5);
    PyModule_AddIntMacro(module, EV_USE_ITEM6);
    PyModule_AddIntMacro(module, EV_USE_ITEM7);
    PyModule_AddIntMacro(module, EV_USE_ITEM8);
    PyModule_AddIntMacro(module, EV_USE_ITEM9);
    PyModule_AddIntMacro(module, EV_USE_ITEM10);
    PyModule_AddIntMacro(module, EV_USE_ITEM11);
    PyModule_AddIntMacro(module, EV_USE_ITEM12);
    PyModule_AddIntMacro(module, EV_USE_ITEM13);
    PyModule_AddIntMacro(module, EV_USE_ITEM14);
    PyModule_AddIntMacro(module, EV_USE_ITEM15);
    PyModule_AddIntMacro(module, EV_ITEM_RESPAWN);
    PyModule_AddIntMacro(module, EV_ITEM_POP);
    PyModule_AddIntMacro(module, EV_PLAYER_TELEPORT_IN);
    PyModule_AddIntMacro(module, EV_PLAYER_TELEPORT_OUT);
    PyModule_AddIntMacro(module, EV_GRENADE_BOUNCE);
    PyModule_AddIntMacro(module, EV_GENERAL_SOUND);
    PyModule_AddIntMacro(module, EV_GLOBAL_SOUND);
    PyModule_AddIntMacro(module, EV_GLOBAL_TEAM_SOUND);
    PyModule_AddIntMacro(module, EV_BULLET_HIT_FLESH);
    PyModule_AddIntMacro(module, EV_BULLET_HIT_WALL);
    PyModule_AddIntMacro(module, EV_MISSILE_HIT);
    PyModule_AddIntMacro(module, EV_MISSILE_MISS);
    PyModule_AddIntMacro(module, EV_MISSILE_MISS_METAL);
    PyModule_AddIntMacro(module, EV_RAILTRAIL);
    PyModule_AddIntMacro(module, EV_SHOTGUN);
    PyModule_AddIntMacro(module, EV_BULLET);
    PyModule_AddIntMacro(module, EV_PAIN);
    PyModule_AddIntMacro(module, EV_DEATH1);
    PyModule_AddIntMacro(module, EV_DEATH2);
    PyModule_AddIntMacro(module, EV_DEATH3);
    PyModule_AddIntMacro(module, EV_DROWN);
    PyModule_AddIntMacro(module, EV_OBITUARY);
    PyModule_AddIntMacro(module, EV_POWERUP_QUAD);
    PyModule_AddIntMacro(module, EV_POWERUP_BATTLESUIT);
    PyModule_AddIntMacro(module, EV_POWERUP_REGEN);
    PyModule_AddIntMacro(module, EV_POWERUP_ARMORREGEN);
    PyModule_AddIntMacro(module, EV_GIB_PLAYER);
    PyModule_AddIntMacro(module, EV_SCOREPLUM);
    PyModule_AddIntMacro(module, EV_PROXIMITY_MINE_STICK);
    PyModule_AddIntMacro(module, EV_PROXIMITY_MINE_TRIGGER);
    PyModule_AddIntMacro(module, EV_KAMIKAZE);
    PyModule_AddIntMacro(module, EV_OBELISKEXPLODE);
    PyModule_AddIntMacro(module, EV_OBELISKPAIN);
    PyModule_AddIntMacro(module, EV_INVUL_IMPACT);
    PyModule_AddIntMacro(module, EV_JUICED);
    PyModule_AddIntMacro(module, EV_LIGHTNINGBOLT);
    PyModule_AddIntMacro(module, EV_DEBUG_LINE);
    PyModule_AddIntMacro(module, EV_TAUNT);
    PyModule_AddIntMacro(module, EV_TAUNT_YES);
    PyModule_AddIntMacro(module, EV_TAUNT_NO);
    PyModule_AddIntMacro(module, EV_TAUNT_FOLLOWME);
    PyModule_AddIntMacro(module, EV_TAUNT_GETFLAG);
    PyModule_AddIntMacro(module, EV_TAUNT_GUARDBASE);
    PyModule_AddIntMacro(module, EV_TAUNT_PATROL);
    PyModule_AddIntMacro(module, EV_FOOTSTEP_SNOW);
    PyModule_AddIntMacro(module, EV_FOOTSTEP_WOOD);
    PyModule_AddIntMacro(module, EV_ITEM_PICKUP_SPEC);
    PyModule_AddIntMacro(module, EV_OVERTIME);
    PyModule_AddIntMacro(module, EV_GAMEOVER);
    PyModule_AddIntMacro(module, EV_MISSILE_MISS_DMGTHROUGH);
    PyModule_AddIntMacro(module, EV_THAW_PLAYER);
    PyModule_AddIntMacro(module, EV_THAW_TICK);
    PyModule_AddIntMacro(module, EV_SHOTGUN_KILL);
    PyModule_AddIntMacro(module, EV_POI);
    PyModule_AddIntMacro(module, EV_DEBUG_HITBOX);
    PyModule_AddIntMacro(module, EV_LIGHTNING_DISCHARGE);
    PyModule_AddIntMacro(module, EV_RACE_START);
    PyModule_AddIntMacro(module, EV_RACE_CHECKPOINT);
    PyModule_AddIntMacro(module, EV_RACE_FINISH);
    PyModule_AddIntMacro(module, EV_DAMAGEPLUM);
    PyModule_AddIntMacro(module, EV_AWARD);
    PyModule_AddIntMacro(module, EV_INFECTED);
    PyModule_AddIntMacro(module, EV_NEW_HIGH_SCORE);

    // Engine limits, for bounding client ids and configstring indices without hardcoding
    // 64 and 1024.
    PyModule_AddIntMacro(module, MAX_CLIENTS);
    PyModule_AddIntMacro(module, MAX_CONFIGSTRINGS);

    // Configstring indices. Note the CS_ prefix is shared with the connection states
    // above; those are clientState_t values, these are indices into the configstrings.
    PyModule_AddIntMacro(module, CS_SERVERINFO);
    PyModule_AddIntMacro(module, CS_SYSTEMINFO);
    PyModule_AddIntMacro(module, CS_MUSIC);
    PyModule_AddIntMacro(module, CS_MESSAGE);
    PyModule_AddIntMacro(module, CS_MOTD);
    PyModule_AddIntMacro(module, CS_WARMUP);
    PyModule_AddIntMacro(module, CS_SCORES1);
    PyModule_AddIntMacro(module, CS_SCORES2);
    PyModule_AddIntMacro(module, CS_VOTE_TIME);
    PyModule_AddIntMacro(module, CS_VOTE_STRING);
    PyModule_AddIntMacro(module, CS_VOTE_YES);
    PyModule_AddIntMacro(module, CS_VOTE_NO);
    PyModule_AddIntMacro(module, CS_GAME_VERSION);
    PyModule_AddIntMacro(module, CS_LEVEL_START_TIME);
    PyModule_AddIntMacro(module, CS_INTERMISSION);
    PyModule_AddIntMacro(module, CS_ITEMS);
    PyModule_AddIntMacro(module, CS_BOTINFO);
    PyModule_AddIntMacro(module, CS_MODELS);
    PyModule_AddIntMacro(module, CS_SOUNDS);
    PyModule_AddIntMacro(module, CS_PLAYERS);
    PyModule_AddIntMacro(module, CS_LOCATIONS);
    PyModule_AddIntMacro(module, CS_LAST_GENERIC);
    PyModule_AddIntMacro(module, CS_FLAGSTATUS);
    PyModule_AddIntMacro(module, CS_SCORES1PLAYER);
    PyModule_AddIntMacro(module, CS_SCORES2PLAYER);
    PyModule_AddIntMacro(module, CS_ROUND_WARMUP);
    PyModule_AddIntMacro(module, CS_ROUND_START_TIME);
    PyModule_AddIntMacro(module, CS_TEAMCOUNT_RED);
    PyModule_AddIntMacro(module, CS_TEAMCOUNT_BLUE);
    PyModule_AddIntMacro(module, CS_SHADERSTATE);
    PyModule_AddIntMacro(module, CS_NEXTMAP);
    PyModule_AddIntMacro(module, CS_PRACTICE);
    PyModule_AddIntMacro(module, CS_FREECAM);
    PyModule_AddIntMacro(module, CS_PAUSE_START_TIME);
    PyModule_AddIntMacro(module, CS_PAUSE_END_TIME);
    PyModule_AddIntMacro(module, CS_TIMEOUTS_RED);
    PyModule_AddIntMacro(module, CS_TIMEOUTS_BLUE);
    PyModule_AddIntMacro(module, CS_MODEL_OVERRIDE);
    PyModule_AddIntMacro(module, CS_PLAYER_CYLINDERS);
    PyModule_AddIntMacro(module, CS_DEBUGFLAGS);
    PyModule_AddIntMacro(module, CS_ENABLEBREATH);
    PyModule_AddIntMacro(module, CS_DMGTHROUGHDEPTH);
    PyModule_AddIntMacro(module, CS_AUTHOR);
    PyModule_AddIntMacro(module, CS_AUTHOR2);
    PyModule_AddIntMacro(module, CS_ADVERT_DELAY);
    PyModule_AddIntMacro(module, CS_PMOVEINFO);
    PyModule_AddIntMacro(module, CS_ARMORINFO);
    PyModule_AddIntMacro(module, CS_WEAPONINFO);
    PyModule_AddIntMacro(module, CS_PLAYERINFO);
    PyModule_AddIntMacro(module, CS_SCORE1STPLAYER);
    PyModule_AddIntMacro(module, CS_SCORE2NDPLAYER);
    PyModule_AddIntMacro(module, CS_CLIENTNUM1STPLAYER);
    PyModule_AddIntMacro(module, CS_CLIENTNUM2NDPLAYER);
    PyModule_AddIntMacro(module, CS_ATMOSEFFECT);
    PyModule_AddIntMacro(module, CS_MOST_DAMAGEDEALT_PLYR);
    PyModule_AddIntMacro(module, CS_MOST_ACCURATE_PLYR);
    PyModule_AddIntMacro(module, CS_REDTEAMBASE);
    PyModule_AddIntMacro(module, CS_BLUETEAMBASE);
    PyModule_AddIntMacro(module, CS_BEST_ITEMCONTROL_PLYR);
    PyModule_AddIntMacro(module, CS_MOST_VALUABLE_OFFENSIVE_PLYR);
    PyModule_AddIntMacro(module, CS_MOST_VALUABLE_DEFENSIVE_PLYR);
    PyModule_AddIntMacro(module, CS_MOST_VALUABLE_PLYR);
    PyModule_AddIntMacro(module, CS_GENERIC_COUNT_RED);
    PyModule_AddIntMacro(module, CS_GENERIC_COUNT_BLUE);
    PyModule_AddIntMacro(module, CS_AD_SCORES);
    PyModule_AddIntMacro(module, CS_ROUND_WINNER);
    PyModule_AddIntMacro(module, CS_CUSTOM_SETTINGS);
    PyModule_AddIntMacro(module, CS_ROTATIONMAPS);
    PyModule_AddIntMacro(module, CS_ROTATIONVOTES);
    PyModule_AddIntMacro(module, CS_DISABLE_VOTE_UI);
    PyModule_AddIntMacro(module, CS_ALLREADY_TIME);
    PyModule_AddIntMacro(module, CS_INFECTED_SURVIVOR_MINSPEED);
    PyModule_AddIntMacro(module, CS_RACE_POINTS);
    PyModule_AddIntMacro(module, CS_DISABLE_LOADOUT);
    PyModule_AddIntMacro(module, CS_MATCH_GUID);
    PyModule_AddIntMacro(module, CS_STARTING_WEAPONS);
    PyModule_AddIntMacro(module, CS_STEAM_ID);
    PyModule_AddIntMacro(module, CS_STEAM_WORKSHOP_IDS);
    PyModule_AddIntMacro(module, CS_MAX);

    // Item model indices (bg_itemlist ordering).
    PyModule_AddIntMacro(module, MODELINDEX_ARMORSHARD);
    PyModule_AddIntMacro(module, MODELINDEX_ARMORYELLOW);
    PyModule_AddIntMacro(module, MODELINDEX_ARMORRED);
    PyModule_AddIntMacro(module, MODELINDEX_ARMORGREEN);
    PyModule_AddIntMacro(module, MODELINDEX_HEALTH5);
    PyModule_AddIntMacro(module, MODELINDEX_HEALTH25);
    PyModule_AddIntMacro(module, MODELINDEX_HEALTH50);
    PyModule_AddIntMacro(module, MODELINDEX_HEALTHMEGA);
    PyModule_AddIntMacro(module, MODELINDEX_GAUNTLET);
    PyModule_AddIntMacro(module, MODELINDEX_SHOTGUN);
    PyModule_AddIntMacro(module, MODELINDEX_MACHINEGUN);
    PyModule_AddIntMacro(module, MODELINDEX_GRENADELAUNCHER);
    PyModule_AddIntMacro(module, MODELINDEX_ROCKETLAUNCHER);
    PyModule_AddIntMacro(module, MODELINDEX_LIGHTNING);
    PyModule_AddIntMacro(module, MODELINDEX_RAILGUN);
    PyModule_AddIntMacro(module, MODELINDEX_PLASMAGUN);
    PyModule_AddIntMacro(module, MODELINDEX_BFG10K);
    PyModule_AddIntMacro(module, MODELINDEX_GRAPPLINGHOOK);
    PyModule_AddIntMacro(module, MODELINDEX_SHELLS);
    PyModule_AddIntMacro(module, MODELINDEX_BULLETS);
    PyModule_AddIntMacro(module, MODELINDEX_GRENADES);
    PyModule_AddIntMacro(module, MODELINDEX_CELLS);
    PyModule_AddIntMacro(module, MODELINDEX_LIGHTNINGAMMO);
    PyModule_AddIntMacro(module, MODELINDEX_ROCKETS);
    PyModule_AddIntMacro(module, MODELINDEX_SLUGS);
    PyModule_AddIntMacro(module, MODELINDEX_BFGAMMO);
    PyModule_AddIntMacro(module, MODELINDEX_TELEPORTER);
    PyModule_AddIntMacro(module, MODELINDEX_MEDKIT);
    PyModule_AddIntMacro(module, MODELINDEX_QUADDAMAGE);
    PyModule_AddIntMacro(module, MODELINDEX_BATTLESUIT);
    PyModule_AddIntMacro(module, MODELINDEX_HASTE);
    PyModule_AddIntMacro(module, MODELINDEX_INVISIBILITY);
    PyModule_AddIntMacro(module, MODELINDEX_REGENERATION);
    PyModule_AddIntMacro(module, MODELINDEX_FLIGHT);
    PyModule_AddIntMacro(module, MODELINDEX_REDFLAG);
    PyModule_AddIntMacro(module, MODELINDEX_BLUEFLAG);
    PyModule_AddIntMacro(module, MODELINDEX_KAMIKAZE);
    PyModule_AddIntMacro(module, MODELINDEX_PORTAL);
    PyModule_AddIntMacro(module, MODELINDEX_INVULNERABILITY);
    PyModule_AddIntMacro(module, MODELINDEX_NAILS);
    PyModule_AddIntMacro(module, MODELINDEX_MINES);
    PyModule_AddIntMacro(module, MODELINDEX_BELT);
    PyModule_AddIntMacro(module, MODELINDEX_SCOUT);
    PyModule_AddIntMacro(module, MODELINDEX_GUARD);
    PyModule_AddIntMacro(module, MODELINDEX_DAMAGE);
    PyModule_AddIntMacro(module, MODELINDEX_AMMOREGEN);
    PyModule_AddIntMacro(module, MODELINDEX_DOMINATIONPOINT);
    PyModule_AddIntMacro(module, MODELINDEX_REDSKULL);
    PyModule_AddIntMacro(module, MODELINDEX_BLUESKULL);
    PyModule_AddIntMacro(module, MODELINDEX_NAILGUN);
    PyModule_AddIntMacro(module, MODELINDEX_PROXLAUNCHER);
    PyModule_AddIntMacro(module, MODELINDEX_CHAINGUN);
    PyModule_AddIntMacro(module, MODELINDEX_SPAWNARMOR);
    PyModule_AddIntMacro(module, MODELINDEX_HEAVYMACHINEGUN);
    PyModule_AddIntMacro(module, MODELINDEX_HEAVYBULLETS);
    PyModule_AddIntMacro(module, MODELINDEX_AMMOPACK);
    PyModule_AddIntMacro(module, MODELINDEX_KEYSILVER);
    PyModule_AddIntMacro(module, MODELINDEX_KEYGOLD);
    PyModule_AddIntMacro(module, MODELINDEX_KEYMASTER);

    // Initialize struct sequence types.
    // Each one gains _fields and _replace immediately afterwards. See AddStructSeqExtras.
    PyStructSequence_InitType(&player_info_type, &player_info_desc);
    PyStructSequence_InitType(&player_state_type, &player_state_desc);
    PyStructSequence_InitType(&player_stats_type, &player_stats_desc);
    PyStructSequence_InitType(&vector3_type, &vector3_desc);
    PyStructSequence_InitType(&weapons_type, &weapons_desc);
    PyStructSequence_InitType(&powerups_type, &powerups_desc);
    PyStructSequence_InitType(&flight_type, &flight_desc);
    PyStructSequence_InitType(&keys_type, &keys_desc);
    PyStructSequence_InitType(&demo_status_type, &demo_status_desc);
    PyStructSequence_InitType(&reliable_status_type, &reliable_status_desc);
    PyStructSequence_InitType(&stat_powerups_type, &stat_powerups_desc);
    PyStructSequence_InitType(&stat_holdables_type, &stat_holdables_desc);
    PyStructSequence_InitType(&player_expanded_stats_type, &player_expanded_stats_desc);

    // _fields and _replace on every one of them.
    static const struct {
        PyTypeObject* type;
        PyStructSequence_Desc* desc;
    } structseqs[] = {
        {&player_info_type, &player_info_desc},
        {&player_state_type, &player_state_desc},
        {&player_stats_type, &player_stats_desc},
        {&vector3_type, &vector3_desc},
        {&weapons_type, &weapons_desc},
        {&powerups_type, &powerups_desc},
        {&flight_type, &flight_desc},
        {&keys_type, &keys_desc},
        {&demo_status_type, &demo_status_desc},
        {&reliable_status_type, &reliable_status_desc},
        {&stat_powerups_type, &stat_powerups_desc},
        {&stat_holdables_type, &stat_holdables_desc},
        {&player_expanded_stats_type, &player_expanded_stats_desc},
    };

    for (size_t i = 0; i < sizeof(structseqs) / sizeof(structseqs[0]); i++) {
        if (AddStructSeqExtras(structseqs[i].type, structseqs[i].desc) == -1) {
            Py_DECREF(module);
            return NULL;
        }
    }

    Py_INCREF((PyObject*)&player_info_type);
    Py_INCREF((PyObject*)&player_state_type);
    Py_INCREF((PyObject*)&player_stats_type);
    Py_INCREF((PyObject*)&vector3_type);
    Py_INCREF((PyObject*)&weapons_type);
    Py_INCREF((PyObject*)&powerups_type);
    Py_INCREF((PyObject*)&flight_type);
    Py_INCREF((PyObject*)&keys_type);
    Py_INCREF((PyObject*)&demo_status_type);
    Py_INCREF((PyObject*)&reliable_status_type);
    Py_INCREF((PyObject*)&stat_powerups_type);
    Py_INCREF((PyObject*)&stat_holdables_type);
    Py_INCREF((PyObject*)&player_expanded_stats_type);
    // Add new types.
    PyModule_AddObject(module, "PlayerInfo", (PyObject*)&player_info_type);
    PyModule_AddObject(module, "PlayerState", (PyObject*)&player_state_type);
    PyModule_AddObject(module, "PlayerStats", (PyObject*)&player_stats_type);
    PyModule_AddObject(module, "Vector3", (PyObject*)&vector3_type);
    PyModule_AddObject(module, "Weapons", (PyObject*)&weapons_type);
    PyModule_AddObject(module, "Powerups", (PyObject*)&powerups_type);
    PyModule_AddObject(module, "Flight", (PyObject*)&flight_type);
    PyModule_AddObject(module, "Keys", (PyObject*)&keys_type);
    PyModule_AddObject(module, "DemoStatus", (PyObject*)&demo_status_type);
    PyModule_AddObject(module, "ReliableStatus", (PyObject*)&reliable_status_type);
    PyModule_AddObject(module, "StatPowerups", (PyObject*)&stat_powerups_type);
    PyModule_AddObject(module, "StatHoldables", (PyObject*)&stat_holdables_type);
    PyModule_AddObject(module, "PlayerExpandedStats", (PyObject*)&player_expanded_stats_type);

    // The live engine views. After the struct sequences, since Vector3 has to exist for
    // the vec3_t fields to hand one back.
    if (PyMinqlxtended_AddObjectTypes(module) == -1) {
        Py_DECREF(module);
        return NULL;
    }

    return module;
}

/*
 * Vector3 is static to this file, and stays that way. python_objects.c needs to build one
 * for every vec3_t it exposes, so it gets this instead of the type object.
 */
PyObject* PyMinqlxtended_Vector3(const vec_t v[3]) {
    PyObject* vec = PyStructSequence_New(&vector3_type);
    if (!vec) {
        return NULL;
    }

    for (int i = 0; i < 3; i++) {
        PyObject* component = PyFloat_FromDouble(v[i]);
        if (!component) {
            Py_DECREF(vec);
            return NULL;
        }
        PyStructSequence_SetItem(vec, i, component); // steals the reference
    }

    return vec;
}

PyMinqlxtended_InitStatus_t PyMinqlxtended_Initialize(void) {
    if (initialized) {
        DebugPrint("%s was called while already initialized!\n", __func__);
        return PYM_ALREADY_INITIALIZED;
    }

    DebugPrint("Initializing Python...\n");
    PyImport_AppendInittab("_minqlxtended", &PyMinqlxtended_InitModule); // must precede initialisation

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    PyStatus status = PyConfig_SetString(&config, &config.program_name, PYTHON_FILENAME);
    if (!PyStatus_Exception(status)) {
        status = Py_InitializeFromConfig(&config);
    }
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        DebugPrint("Py_InitializeFromConfig() failed: %s\n", status.err_msg ? status.err_msg : "unknown error");
        return PYM_PY_INIT_ERROR;
    }

    // Add the main module.
    PyObject* main_module = PyImport_AddModule("__main__");
    PyObject* main_dict   = PyModule_GetDict(main_module);
    // Run script to load pyminqlxtended.
    PyObject* res = PyRun_String(loader, Py_file_input, main_dict, main_dict);
    if (res == NULL) {
        DebugPrint("PyRun_String() returned NULL. Did you modify the loader?\n");
        return PYM_MAIN_SCRIPT_ERROR;
    }
    PyObject* ret = PyDict_GetItemString(main_dict, "ret"); // Borrowed reference; do not decref.
    Py_DECREF(res);
    if (ret == NULL) {
        DebugPrint("The loader script return value doesn't exist?\n");
        return PYM_MAIN_SCRIPT_ERROR;
    } else if (ret != Py_True) {
        // No need to print anything, since the traceback should be printed already.
        return PYM_MAIN_SCRIPT_ERROR;
    }

    /* Drop the GIL and let it go. Python is initialised exactly once per process, with no
     * teardown: PyType_Ready no-ops once Py_TPFLAGS_READY is set, so the static PyTypeObjects
     * here and in python_objects.c would stay bound to a freed interpreter. */
    (void)PyEval_SaveThread();
    initialized = 1;
    DebugPrint("Python initialized!\n");
    return PYM_SUCCESS;
}
