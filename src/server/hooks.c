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

#define _GNU_SOURCE
#define __STDC_FORMAT_MACROS

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "features/console_command.h"
#include "features/demos.h"
#include "hook/patches.h"
#include "maps_parser.h"
#include "engine/patterns.h"
#include "features/profile.h"
#include "engine/quake_common.h"
#include "features/reliable.h"
#include "features/scoreboard.h"
#include "hook/simple_hook.h"

#ifndef NOPY
#include "features/game_events.h"
#include "python/pyminqlxtended.h"
#endif

// qagame module.
void* qagame;
void* qagame_dllentry;

// How many SV_SpawnServer calls are in progress; declared in common.h. Com_Error longjmps out
// of a failed spawn and skips the decrement, which would silence the frame dispatchers for the
// life of the process, so My_SV_Shutdown zeroes it. Every Com_Error reaches SV_Shutdown.
int sv_spawning;

static void SetTag(void);

void __cdecl My_Cmd_AddCommand(char* cmd, void* func) {
    if (!common_initialized) {
        InitializeStatic();
    }

#ifndef NOPY
    // Every name the engine registers passes through here, which is how console_command()
    // learns that `devmap` and `map` share a handler. See console_command.h.
    ConsoleCommand_NoteRegistration(cmd, func);
#endif

    Cmd_AddCommand(cmd, func);
}

void __cdecl My_Sys_SetModuleOffset(char* moduleName, void* offset) {
    // We should be getting qagame, but check just in case.
    if (!strcmp(moduleName, "qagame")) {
        // Despite the name, this is vmMain. dladdr gets the module base the pointers below
        // are relative to.
        qagame_dllentry = offset;

        Dl_info dlinfo;
        int res = dladdr(offset, &dlinfo);
        if (!res) {
            // Every VM_SEARCH is relative to qagame, so continuing would scan from 0xB000
            // and take a SIGSEGV behind a misleading "Unable to find" log.
            DebugError("dladdr() failed.\n", __FILE__, __LINE__, __func__);
            DebugPrint("Exiting.\n");
            exit(1);
        }
        qagame = dlinfo.dli_fbase;
        DebugPrint("Got qagame: %p\n", qagame); // %p for a 64-bit pointer
    } else {
        DebugPrint("Unknown module: %s\n", moduleName);
    }

    Sys_SetModuleOffset(moduleName, offset);
    if (common_initialized) {
        // vm_rehooking goes up unconditionally, since the first load has no My_G_ShutdownGame
        // before it to raise it, and comes down once the rebuild is finished. See common.h.
        atomic_store_explicit(&vm_rehooking, 1, memory_order_release);
        SearchVmFunctions();
        HookVm();
        InitializeVm();
        patch_vm();
        atomic_store_explicit(&vm_rehooking, 0, memory_order_release);
    }
}

// The game module going away. Every path that drops it calls this export and then VM_Free, so
// this is the last point before qagame is unmapped. The globals are cleared here because a
// stale pointer passes the NULL checks that keep a worker thread off unmapped memory. See
// vm_rehooking in common.h.
void __cdecl My_G_ShutdownGame(int restart) {
    atomic_store_explicit(&vm_rehooking, 1, memory_order_release);

    g_entities  = NULL;
    level       = NULL;
    bg_itemlist = NULL;
    bg_numItems = 0;

    G_ShutdownGame(restart);
}

void __cdecl My_G_InitGame(int levelTime, int randomSeed, int restart) {
    G_InitGame(levelTime, randomSeed, restart);

    if (!cvars_initialized) { // Only called once.
        SetTag();
    }
    InitializeCvars();

#ifndef NOPY
    // Every client is re-seated and level is rebuilt, so the cached round/team state we
    // diff against is stale. A map_restart reaches here without going through SV_SpawnServer.
    GameEvents_Reset();

    if (restart) {
        NewGameDispatcher(restart);
    }
#endif
}

qboolean __cdecl My_Sys_IsLANAddress(netadr_t adr) {
    return qtrue; // the server will always believe that all IPs presented are LAN addresses
}

#ifdef NOPY
// A nopy build does not hook G_RunFrame, so a segment the writer failed on is reconciled from
// the outgoing-message path instead. Without it the slot goes on capturing into a file that has
// been closed and unlinked, and the completion queue fills. One relaxed atomic load when idle.
static void DrainFinishedDemos(void) {
    if (!Demo_PendingFinished()) {
        return;
    }

    demo_finished_t done;
    while (Demo_PollFinished(&done)) {
        if (done.failed) {
            Demo_AbandonSlot(done.slot, done.gen);
        }
    }

    unsigned dropped = Demo_TakeDroppedCount();
    if (dropped) {
        DebugPrint("demo: %u completion(s) dropped\n", dropped);
    }
}
#endif

void __cdecl My_SV_SendMessageToClient(msg_t* msg, client_t* client) {
    Demo_Capture(msg, client);
#ifdef NOPY
    DrainFinishedDemos();
#endif
    SV_SendMessageToClient(msg, client);
}

// This and the two below sit outside the NOPY guard because demos.c is in both builds: every
// point at which a segment is opened, finalised or abandoned has to be reachable from both.
// Only the dispatches inside them are Python's.
void __cdecl My_SV_DropClient(client_t* drop, const char* reason) {
    int slot = (int)(drop - svs->clients);

#ifndef NOPY
    ClientDisconnectDispatcher(slot, reason);
    Reliable_ClientGone(slot); // nothing queued for them is worth sending
#endif

    Demo_ClientDisconnect(slot); // finalise this client's demo, if any

    SV_DropClient(drop, reason);
}

void __cdecl My_SV_SpawnServer(char* server, qboolean killBots) {
#ifndef NOPY
    // Backstop under console_command()'s classifier. Getting here from a command it ran in place
    // means reload_names in console_command.c is missing a name, and spawning now would unmap
    // qagame with our frames on the stack. Defer instead; SV_Map_f has already done its factory
    // cvar walk, so the re-run finds the factory current and goes straight to the spawn.
    const char* running = ConsoleCommand_Executing();
    if (running && Cbuf_ExecuteText) {
        DebugPrint("WARNING: \"%s\" reloads the game module but ran in place; deferring it. "
                   "Add it to reload_names in console_command.c.\n", running);
        char text[MAX_STRING_CHARS + 1];
        snprintf(text, sizeof(text), "%s\n", running);
        ConsoleCommand_ForgetExecuting();
        Cbuf_ExecuteText(EXEC_APPEND, text);
        return;
    }

    Reliable_Reset();   // queued output belongs to the map we are leaving
    Scoreboard_Reset(); // ...as does anything we were part-way through trimming
#endif

    Demo_CloseAll(); // map change: finalise open demos; each client re-primes with a fresh gamestate

#ifndef NOPY
    GameEvents_Reset(); // the outgoing map's round, team and intermission state means nothing here

    // SV_SpawnServer wipes and repopulates the configstring table through
    // SV_SetConfigstring, so those writes all dispatch before NewGameDispatcher below.
    // Drop the cache now, or a set_configstring handler is handed the old map's values.
    SpawnServerDispatcher();
#endif

    sv_spawning++;
    SV_SpawnServer(server, killBots);
    sv_spawning--;

#ifndef NOPY
    // We call NewGameDispatcher here instead of G_InitGame when it's not just a map_restart,
    // otherwise configstring 0 and such won't be initialized and we can't instantiate minqlxtended.Game.
    NewGameDispatcher(qfalse);
#endif
}

#ifndef NOPY
// Every tokenisation the engine does, recorded so console_command() can put the tokeniser back
// after running a command in place. Hooked rather than reading cmd_cmd: its accessor is a
// three-byte lea and a ret, too small to pattern-match.
void __cdecl My_Cmd_TokenizeString(const char* text_in) {
    ConsoleCommand_NoteTokenized(text_in);
    Cmd_TokenizeString(text_in);
}

// The one place a client asks to change its own userinfo. Cancelling drops the change, and a
// replacement infostring is re-tokenised so the engine's own copy is what runs. Hooked here
// rather than at qagame's ClientUserinfoChanged, which has fifteen callers.
void __cdecl My_SV_UpdateUserinfo_f(client_t* cl) {
    if (cl && Cmd_Argv) {
        // Copied out before dispatching: console_command() puts the tokeniser back the way it
        // found it, but only once the handler has returned, and this pointer is read before then.
        char current[MAX_INFO_STRING];
        const char* argv1 = Cmd_Argv(1);
        strncpy(current, argv1 ? argv1 : "", sizeof(current) - 1);
        current[sizeof(current) - 1] = '\0';

        char replacement[MAX_INFO_STRING];
        int action = UserinfoDispatcher((int)(cl - svs->clients), current,
                                        replacement, sizeof(replacement));
        if (action == 0) {
            return; // Cancelled.
        }

        // Only for a replacement. An unchanged infostring is still in the tokeniser, because
        // ConsoleCommand_Run restores it around anything a handler ran.
        if (action == 2 && Cmd_TokenizeString) {
            // Stripped before it goes back through the tokeniser, as SetTag does for sv_tags:
            // a quote would end the argument early and truncate the infostring.
            char sanitised[MAX_INFO_STRING];
            size_t n = 0;
            for (const char* s = replacement; *s && n < sizeof(sanitised) - 1; s++) {
                if (*s != '"' && *s != ';' && *s != '\n' && *s != '\r') {
                    sanitised[n++] = *s;
                }
            }
            sanitised[n] = '\0';

            char rebuilt[MAX_INFO_STRING + 32]; // sanitised can only shrink, so this fits
            snprintf(rebuilt, sizeof(rebuilt), "userinfo \"%s\"", sanitised);
            // Noted by hand: this is the trampoline, so it does not reach My_Cmd_TokenizeString,
            // and a console command run below here would restore the pre-replacement infostring.
            ConsoleCommand_NoteTokenized(rebuilt);
            Cmd_TokenizeString(rebuilt);
        }
    }

    SV_UpdateUserinfo_f(cl);
}

void __cdecl My_SV_ExecuteClientCommand(client_t* cl, char* s, qboolean clientOK) {
    char* res = s;
    if (clientOK && cl->gentity) {
        res = ClientCommandDispatcher(cl - svs->clients, s);
        if (!res) {
            return;
        }
    }

    SV_ExecuteClientCommand(cl, res, clientOK);
}

void __cdecl My_SV_SendServerCommand(client_t* cl, char* fmt, ...) {
    va_list argptr;
    char buffer[MAX_MSGLEN];

    va_start(argptr, fmt);
    vsnprintf((char*)buffer, sizeof(buffer), fmt, argptr);
    va_end(argptr);

    // Undo any scoreboard trim first. The intermission statistics messages run later in the same
    // SelectScoreboardMessage call and read the same fields, and the dispatch below is plugin
    // code that would otherwise see the blanked values. Keyed on what the game module sent,
    // before any handler has rewritten it.
    Scoreboard_NoteCommand(buffer);

    char* res = buffer;
    if (cl && cl->gentity) {
        res = ServerCommandDispatcher(cl - svs->clients, buffer);
    } else if (cl == NULL) {
        res = ServerCommandDispatcher(-1, buffer);
    }

    if (!res) {
        return;
    }

    // Paces bursts that would otherwise cycle commands out of the client's 64-slot
    // reliable ring and drop everyone. qtrue means it owns the command now.
    if (Reliable_Intercept(cl, res)) {
        return;
    }

    SV_SendServerCommand(cl, "%s", res);
}

// Keeps the top of each team's scoreboard intact on a busy server, where the builder
// would otherwise drop everyone to the "smscores" stub. See scoreboard.h.
void __cdecl My_SelectScoreboardMessage(gentity_t* ent) {
    Scoreboard_BeginTrim();
    SelectScoreboardMessage(ent);
    Scoreboard_EndTrim(); // no-op if the command already went out
}

void __cdecl My_SV_ClientEnterWorld(client_t* client, usercmd_t* cmd) {
    clientState_t state = client->state; // State before we call real one.
    SV_ClientEnterWorld(client, cmd);

    // gentity is NULL if the map changed. CS_PRIMED only on their first connect, or the
    // dispatcher would also fire when a game starts.
    if (client->gentity != NULL && state == CS_PRIMED) {
        ClientLoadedDispatcher(client - svs->clients);
    }
}

void __cdecl My_SV_SetConfigstring(int index, char* value) {
    // Indices 16 and 66X are rewritten every frame, and dispatching them costs about 25%
    // more CPU on an empty server. Python mirrors this set as _UNDISPATCHED in
    // _configstring.py, so the cache never holds them; keep the two in step.
    if (index == 16 || (index >= 662 && index < 670)) {
        SV_SetConfigstring(index, value);
        return;
    }

    if (!value) {
        value = "";
    }
    char* res = SetConfigstringDispatcher(index, value);
    // NULL means stop the event.
    if (res) {
        SV_SetConfigstring(index, res);
    }
}

void __cdecl My_Com_Printf(char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    char* res = ConsolePrintDispatcher(buf);
    // NULL means stop the event.
    if (res) {
        Com_Printf("%s", res);
    }
}

#ifndef NOPY
// Every cvar write funnels through here: console `set`, the game module's setters, and Python's
// set_cvar. Gated like damage. The old value is copied out first, since a completed set Z_Frees
// cvar->string. Comparing it against what the engine kept limits the dispatch to writes that
// changed the live value; a range-flagged cvar reports the clamped text.
cvar_t* __cdecl My_Cvar_Set2(const char* var_name, const char* value, qboolean force) {
    if (!cvar_changed_handler) {
        return Cvar_Set2(var_name, value, force);
    }

    char* old_value    = NULL;
    cvar_t* var_before = Cvar_FindVar(var_name);
    if (var_before && var_before->string) {
        old_value = strdup(var_before->string);
    }

    cvar_t* result = Cvar_Set2(var_name, value, force);

    if (old_value && result && result->string && strcmp(old_value, result->string) != 0) {
        CvarChangedDispatcher(result->name, old_value, result->string);
    }
    free(old_value);
    return result;
}
#endif

// The demo writer thread holds no GIL, so it queues finished segments for us to drain and
// dispatch here.
static void DispatchFinishedDemos(void) {
    if (!Demo_PendingFinished()) {
        return;
    }

    demo_finished_t done;
    while (Demo_PollFinished(&done)) {
        if (done.failed) {
            Demo_AbandonSlot(done.slot, done.gen);
        }
        DemoFinishedDispatcher(done.slot, done.path, done.bytes, done.discarded, done.failed);
    }

    unsigned dropped = Demo_TakeDroppedCount();
    if (dropped) {
        DebugPrint("demo: %u completion(s) dropped; a demo_finished event was missed\n", dropped);
    }
}

void __cdecl My_G_RunFrame(int time) {
    // Dropping frames is probably not a good idea, so we don't allow cancelling.
    PROF_BEGIN(t_frame);

    if (!sv_spawning) {
        // What console_command() held back from worker threads. Before the dispatchers, so what
        // they queue goes out next frame rather than partway through this one.
        ConsoleCommand_Drain();

        // Release whatever the guard held back last frame before the dispatchers get a
        // chance to queue more, so the ring drains at a steady rate.
        Reliable_Flush();

        // Skips the frame hooks while the game is uninitialised. FrameDispatcher probes
        // itself, so its figure leaves out the GIL wait as the other event probes do.
        FrameDispatcher();

        PROF_BEGIN(t_demos);
        DispatchFinishedDemos();
        PROF_END(PROF_DEMO_DISPATCH, t_demos);
    }

    G_RunFrame(time);

    // After the engine's frame, so round transitions and team changes made during it are
    // visible on the same frame they happen instead of one late.
    if (!sv_spawning) {
        GameEvents_Frame();
    }

    // The engine's own frame is in here too, so this is what we measure the other probes
    // against. It isn't an overhead figure of its own.
    PROF_END(PROF_FRAME_TOTAL, t_frame);
}

char* __cdecl My_ClientConnect(int clientNum, qboolean firstTime, qboolean isBot) {
    if (firstTime) {
        char* res = ClientConnectDispatcher(clientNum, isBot);
        if (res && !isBot) {
            return res;
        }
    }

    return ClientConnect(clientNum, firstTime, isBot);
}

// Deaths. G_Damage reaches player_die through ent->die, but Cmd_Kill_f and ExecuteTeamChange
// call it by name, so hooking the function catches /kill too. ExecuteTeamChange runs a player
// through player_die on a team change, hence the MOD_SWITCH_TEAMS drop; team_switch reports it.
// No __cdecl: gentity_t.die is declared without it, and on i386 the types would differ.
static void My_player_die(gentity_t* self, gentity_t* inflictor,
                          gentity_t* attacker, int damage, int mod) {
    player_die(self, inflictor, attacker, damage, mod); // trampoline

    if (!self || !self->client || mod == MOD_SWITCH_TEAMS) {
        return;
    }

    // A death with no client behind it (lava, a crusher, a trigger_hurt) reports no killer
    // rather than the world entity. Suicides report themselves.
    int killer_id = (attacker && attacker->client) ? (int)(attacker - g_entities) : -1;

    PlayerDeathDispatcher((int)(self - g_entities), killer_id, mod);
}

void __cdecl My_ClientSpawn(gentity_t* ent) {
    ClientSpawn(ent);

    // After the real function, so a handler setting weapons is not overridden by it.
    ClientSpawnDispatcher(ent - g_entities);
}

// Touch_Item runs for every touch of an item trigger and most are rejected before pickupCount is
// bumped, so diffing pickupCount separates a pickup from a touch. SVF_NOCLIENT will not serve:
// with g_itemTimers on, Touch_Item sets and clears it for any item carrying a timer.
void __cdecl My_Touch_Item(gentity_t* ent, gentity_t* other, trace_t* trace) {
    if (!ent || !other || !other->client) {
        Touch_Item(ent, other, trace);
        return;
    }

    int picked_up_before = ent->pickupCount;

    Touch_Item(ent, other, trace);

    if (ent->pickupCount != picked_up_before && ent->item) {
        ItemPickupDispatcher((int)(other - g_entities), ent->item->classname);
    }
}

// Votes being called. Already tokenised by SV_ExecuteClientCommand, so Cmd_Argv gives the exact
// parse the game will act on. Cancelling stops the vote before it starts.
void __cdecl My_Cmd_CallVote_f(gentity_t* ent) {
    if (ent && ent->client && Cmd_Argv && Cmd_Argc) {
        // Snapshotted before dispatching: console_command() restores the tokeniser around
        // anything a handler runs, but only once the handler has returned, and these pointers
        // are read before then.
        char vote[64];
        char args[MAX_STRING_CHARS];
        const char* argv1 = Cmd_Argv(1);

        strncpy(vote, argv1 ? argv1 : "", sizeof(vote) - 1);
        vote[sizeof(vote) - 1] = '\0';

        // Rejoined with single spaces. The engine collapsed the original whitespace
        // during tokenisation, so this is what the vote string itself will look like.
        int argc    = Cmd_Argc();
        size_t used = 0;
        args[0]     = '\0';
        for (int i = 2; i < argc; i++) {
            const char* arg = Cmd_Argv(i);
            if (!arg) {
                continue;
            }

            int n = snprintf(args + used, sizeof(args) - used, "%s%s", used ? " " : "", arg);
            if (n < 0 || (size_t)n >= sizeof(args) - used) {
                break; // Truncated; snprintf has already NUL-terminated what fit.
            }
            used += (size_t)n;
        }

        if (!VoteCalledDispatcher((int)(ent - g_entities), vote, args)) {
            return;
        }
    }

    Cmd_CallVote_f(ent);
}

// Chat. Every route funnels through here, so say, say_team and tell are all caught. G_Say's
// other callers are Cmd_GameCommand_f and Cmd_Voice_f, so voice chats and game commands raise
// the event too; the mode tells them apart.
void __cdecl My_G_Say(gentity_t* ent, gentity_t* target, int mode, const char* chatText) {
    if (ent && ent->client && chatText) {
        // Only a tell has one; say and say_team report -1, as damage and death do for a
        // world attacker.
        int target_id = (target && target->client) ? (int)(target - g_entities) : -1;

        if (!ChatDispatcher((int)(ent - g_entities), target_id, mode, chatText)) {
            return;
        }
    }

    G_Say(ent, target, mode, chatText);
}

// Joining a team. Cancelling prevents the switch outright. SetTeam is also reached from an admin
// put, the duel queue, follow-cycling and level exit, so a cancelling handler blocks those too.
void __cdecl My_SetTeam(gentity_t* ent, char* s) {
    if (ent && ent->client && s) {
        if (!TeamSwitchAttemptDispatcher((int)(ent - g_entities),
                                         ent->client->sess.sessionTeam, s)) {
            return;
        }
    }

    SetTeam(ent, s);
}

// Every shot fired, roughly 20 a second per player with the lightning gun, hence the same gate
// `damage` uses. See weapon_fired_handler. Post-call, so it can't cancel.
void __cdecl My_FireWeapon(gentity_t* ent) {
    FireWeapon(ent);

    if (!weapon_fired_handler || !ent || !ent->client) {
        return;
    }

    WeaponFiredDispatcher((int)(ent - g_entities), ent->s.weapon);
}

// Damage. Called for every point the game module applies, to shootable world geometry as well as
// players, an order of magnitude more often than anything else here, hence both filters below.
// damage_handler is tested before the pointer arithmetic; keep that order. Post-call, so the
// victim's health is what they were left with.
void __cdecl My_G_Damage(gentity_t* targ, gentity_t* inflictor, gentity_t* attacker,
                         vec3_t dir, vec3_t point, int damage, int dflags, int mod) {
    G_Damage(targ, inflictor, attacker, dir, point, damage, dflags, mod);

    // Doors and other shootable brushes take damage constantly and aren't what anyone
    // hooks this for, so players only.
    if (!damage_handler || !targ || !targ->client) {
        return;
    }

    // As with deaths, world damage reports no attacker instead of pointing at the world
    // entity. A player damaging themselves reports themselves.
    int attacker_id = (attacker && attacker->client) ? (int)(attacker - g_entities) : -1;

    DamageDispatcher((int)(targ - g_entities), attacker_id, damage, dflags, mod);
}

void __cdecl My_G_StartKamikaze(gentity_t* ent) {
    int client_id, is_used_on_demand;

    if (ent->client) {
        // player activated kamikaze item
        ent->client->ps.eFlags &= ~EF_KAMIKAZE;
        client_id         = ent->client->ps.clientNum;
        is_used_on_demand = 1;
    } else if (ent->activator) {
        // dead player's body blast
        client_id         = ent->activator->r.ownerNum;
        is_used_on_demand = 0;
    } else {
        // I don't know
        client_id         = -1;
        is_used_on_demand = 0;
    }

    if (is_used_on_demand) {
        KamikazeUseDispatcher(client_id);
    }

    G_StartKamikaze(ent);

    if (client_id != -1) {
        KamikazeExplodeDispatcher(client_id, is_used_on_demand);
    }
}
#endif

// The server going away: `quit`, `killserver` and sv_killserver, Com_Error, and SV_Frame's
// restart paths. SV_SpawnServer does not come through here, so this can finalise
// unconditionally. Runs before the original, which Z_Free's svs.clients and then memsets svs.
// Outside the NOPY guard, since Demo_Capture is hooked either way.
void __cdecl My_SV_Shutdown(char* finalmsg) {
#ifndef NOPY
    // The same backstop as My_SV_SpawnServer's. killserver reaches it, and so does a Com_Error
    // raised under a command we ran in place; quit goes through Com_Quit_f, which never returns.
    // Forgotten as it defers, so the name cannot defer a second time: the Com_Error case longjmps
    // past ConsoleCommand_Run's own clear, and a name left standing here means the server can
    // never be shut down again.
    const char* running = ConsoleCommand_Executing();
    if (running && Cbuf_ExecuteText) {
        DebugPrint("WARNING: \"%s\" shuts the server down but ran in place; deferring it. "
                   "Add it to reload_names in console_command.c.\n", running);
        char text[MAX_STRING_CHARS + 1];
        snprintf(text, sizeof(text), "%s\n", running);
        ConsoleCommand_ForgetExecuting();
        Cbuf_ExecuteText(EXEC_APPEND, text);
        return;
    }
#endif

    // Below the deferral, which returns with the server still up. From here it is going away, so
    // anything a longjmp left standing is stale: a spawn depth, and whatever we were part-way
    // through trimming.
    sv_spawning = 0;
#ifndef NOPY
    Scoreboard_Reset();
#endif

    Demo_DrainFinalise();

    // killserver keeps the process, so slot 3's override must not still be standing when the
    // next map seats someone else there. SV_Shutdown drops no clients of its own.
    Demo_ClearRequests();

#ifndef NOPY
    // The drain has just produced a batch of completions and there will be no further frame
    // to report them from, so this is the last chance for demo_finished to fire for them.
    DispatchFinishedDemos();
#endif

    SV_Shutdown(finalmsg);
}

// Hook static functions. Can be done before program even runs.
void HookStatic(void) {
    int res, failed = 0;
    DebugPrint("Hooking...\n");
    res = Hook((void*)Cmd_AddCommand, My_Cmd_AddCommand, (void*)&Cmd_AddCommand);
    if (res) {
        DebugPrint("ERROR: Failed to hook Cmd_AddCommand: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)Sys_SetModuleOffset, My_Sys_SetModuleOffset, (void*)&Sys_SetModuleOffset);
    if (res) {
        DebugPrint("ERROR: Failed to hook Sys_SetModuleOffset: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)Sys_IsLANAddress, My_Sys_IsLANAddress, (void*)&Sys_IsLANAddress);
    if (res) {
        DebugPrint("ERROR: Failed to hook Sys_IsLANAddress: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)SV_SendMessageToClient, My_SV_SendMessageToClient, (void*)&SV_SendMessageToClient);
    if (res) {
        DebugPrint("ERROR: Failed to hook SV_SendMessageToClient: %d\n", res);
        failed = 1;
    }

    // The demo recorder's lifecycle, in both builds. SV_SendMessageToClient above opens and
    // feeds a segment; these three finalise one, on the client leaving, the map changing and
    // the server going away.
    res = Hook((void*)SV_Shutdown, My_SV_Shutdown, (void*)&SV_Shutdown);
    if (res) {
        DebugPrint("ERROR: Failed to hook SV_Shutdown: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)SV_DropClient, My_SV_DropClient, (void*)&SV_DropClient);
    if (res) {
        DebugPrint("ERROR: Failed to hook SV_DropClient: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)SV_SpawnServer, My_SV_SpawnServer, (void*)&SV_SpawnServer);
    if (res) {
        DebugPrint("ERROR: Failed to hook SV_SpawnServer: %d\n", res);
        failed = 1;
    }

    // ==============================
    //    ONLY NEEDED FOR PYTHON
    // ==============================
#ifndef NOPY
    res = Hook((void*)SV_ExecuteClientCommand, My_SV_ExecuteClientCommand, (void*)&SV_ExecuteClientCommand);
    if (res) {
        DebugPrint("ERROR: Failed to hook SV_ExecuteClientCommand: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)SV_ClientEnterWorld, My_SV_ClientEnterWorld, (void*)&SV_ClientEnterWorld);
    if (res) {
        DebugPrint("ERROR: Failed to hook SV_ClientEnterWorld: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)SV_SendServerCommand, My_SV_SendServerCommand, (void*)&SV_SendServerCommand);
    if (res) {
        DebugPrint("ERROR: Failed to hook SV_SendServerCommand: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)SV_SetConfigstring, My_SV_SetConfigstring, (void*)&SV_SetConfigstring);
    if (res) {
        DebugPrint("ERROR: Failed to hook SV_SetConfigstring: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)Com_Printf, My_Com_Printf, (void*)&Com_Printf);
    if (res) {
        DebugPrint("ERROR: Failed to hook Com_Printf: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)SV_UpdateUserinfo_f, My_SV_UpdateUserinfo_f, (void*)&SV_UpdateUserinfo_f);
    if (res) {
        DebugPrint("ERROR: Failed to hook SV_UpdateUserinfo_f: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)Cvar_Set2, My_Cvar_Set2, (void*)&Cvar_Set2);
    if (res) {
        DebugPrint("ERROR: Failed to hook Cvar_Set2: %d\n", res);
        failed = 1;
    }

    res = Hook((void*)Cmd_TokenizeString, My_Cmd_TokenizeString, (void*)&Cmd_TokenizeString);
    if (res) {
        DebugPrint("ERROR: Failed to hook Cmd_TokenizeString: %d\n", res);
        failed = 1;
    }

#endif

    if (failed) {
        DebugPrint("Exiting.\n");
        exit(1);
    }
}

// Hooks VM calls. They live in a table of pointers, so those are swapped in place rather than
// trampolined, like hooking a VMT. Must run after Sys_SetModuleOffset; that call sets the pointer
// the table is found through. Prefer a table hook to Hook() wherever you can.
void HookVm(void) {
    DebugPrint("Hooking VM functions...\n");

    pint vm_call_table = *(int32_t*)OFFSET_RELP_VM_CALL_TABLE + OFFSET_RELP_VM_CALL_TABLE + 4;

    // The displacement comes out of vmMain's prologue and the slots below are stored through, so
    // a prologue that changed shape would put the table on arbitrary memory.
    // RELOFFSET_VM_CALL_INITGAME is the higher of the two slots.
    if (!InVm(vm_call_table) || !InVm(vm_call_table + RELOFFSET_VM_CALL_INITGAME)) {
        DebugPrint("ERROR: The VM call table landed outside qagame.\nExiting.\n");
        exit(1);
    }

    /*
     * Both builds take this one. It is what raises vm_rehooking and clears the module globals
     * before the engine unmaps qagame, and a nopy server has the same globals to lose.
     */
    G_ShutdownGame                                            = *(G_ShutdownGame_ptr*)(vm_call_table + RELOFFSET_VM_CALL_SHUTDOWNGAME);
    *(void**)(vm_call_table + RELOFFSET_VM_CALL_SHUTDOWNGAME) = My_G_ShutdownGame;

    G_InitGame                                            = *(G_InitGame_ptr*)(vm_call_table + RELOFFSET_VM_CALL_INITGAME);
    *(void**)(vm_call_table + RELOFFSET_VM_CALL_INITGAME) = My_G_InitGame;

    G_RunFrame = *(G_RunFrame_ptr*)(vm_call_table + RELOFFSET_VM_CALL_RUNFRAME);

#ifndef NOPY
    *(void**)(vm_call_table + RELOFFSET_VM_CALL_RUNFRAME) = My_G_RunFrame;

    // `count` must equal the number of SUCCESSFUL Hook() calls: Hook advances the trampoline
    // allocator only on success, and seek_hook_slot(-count) below rewinds by that many. The
    // unconditional count++ is safe because a failure sets `failed` and exits before the rewind.
    // A tolerated failure must count++ in its success branch instead.
#define HOOK_VM(x)                                                 \
    res = Hook((void*)x, My_##x, (void*)&x);                       \
    if (res) {                                                     \
        DebugPrint("ERROR: Failed to hook " #x ": %d\n", res);     \
        failed = 1;                                                \
    }                                                              \
    count++

    int res, failed = 0, count = 0;

    HOOK_VM(ClientConnect);
    HOOK_VM(G_StartKamikaze);
    HOOK_VM(ClientSpawn);
    HOOK_VM(Touch_Item);
    HOOK_VM(G_Damage);
    HOOK_VM(player_die);
    HOOK_VM(Cmd_CallVote_f);
    HOOK_VM(G_Say);
    HOOK_VM(SetTeam);
    HOOK_VM(FireWeapon);

#undef HOOK_VM

    /*
     * The only tolerated failure, so the only conditional count++. Cmd_CallVote_f above is
     * never NULLed on failure either, since vote_clientkick_fix needs Cmd_CallVote_f_addr.
     */
    if (SelectScoreboardMessage) {
        res = Hook((void*)SelectScoreboardMessage, My_SelectScoreboardMessage, (void*)&SelectScoreboardMessage);
        if (res) {
            DebugPrint("WARNING: Failed to hook SelectScoreboardMessage: %d. Skipping it...\n", res);
            SelectScoreboardMessage = NULL;
        } else {
            count++;
        }
    }

    if (failed) {
        DebugPrint("Exiting.\n");
        exit(1);
    }

    if (!seek_hook_slot(-count)) {
        DebugPrint("ERROR: Failed to rewind hook slot\nExiting.\n");
        exit(1);
    }
#endif
}


/////////////
// HELPERS //
/////////////

static void SetTag(void) {
    // Add minqlxtended tag.
    char tags[1024]; // Surely 1024 is enough?
    cvar_t* sv_tags = Cvar_FindVar("sv_tags");
    // QL registers sv_tags in SV_Init, well before this runs, but a missing cvar here would be
    // a NULL deref on a path nothing else guards.
    if (sv_tags && sv_tags->string && strlen(sv_tags->string) > 2) { // Does it already have tags?
        // The existing value is spliced into a console command, so a quote or semicolon
        // would end the argument and run the rest as further commands. Half the output
        // buffer, so the wrapper cannot push it to where snprintf truncates.
        char sanitised[sizeof(tags) / 2];
        size_t i = 0;
        for (const char* s = sv_tags->string; *s && i < sizeof(sanitised) - 1; s++) {
            if (*s != '"' && *s != ';' && *s != '\n' && *s != '\r') {
                sanitised[i++] = *s;
            }
        }
        sanitised[i] = '\0';

        snprintf(tags, sizeof(tags), "sv_tags \"" SV_TAGS_PREFIX ",%s\"", sanitised);
        Cbuf_ExecuteText(EXEC_INSERT, tags);
    } else {
        Cbuf_ExecuteText(EXEC_INSERT, "sv_tags \"" SV_TAGS_PREFIX "\"");
    }
}
