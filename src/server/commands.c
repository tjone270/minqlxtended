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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// First, ahead of any system header: Python.h sets _POSIX_C_SOURCE and _XOPEN_SOURCE.
#ifndef NOPY
#include "python/pyminqlxtended.h"
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "features/profile.h"
#include "engine/quake_common.h"
#include "features/reliable.h"
#include "features/scoreboard.h"

// Registered from InitializeStatic, long before InitializeCvars and InitializeVm populate
// sv_maxclients, g_entities and svs. A `stopfollowing 0` exec'd from a cfg before the first map
// load would dereference NULL.
static qboolean ClientCommandsReady(void) {
    if (!sv_maxclients || !g_entities || !svs || !svs->clients) {
        ENGINE_PRINTF("No game is running yet.\n");
        return qfalse;
    }
    return qtrue;
}

void __cdecl SendServerCommand(void) {
    SV_SendServerCommand(NULL, "%s\n", Cmd_Args());
}

void __cdecl CenterPrint(void) {
    SV_SendServerCommand(NULL, "cp \"%s\"\n", Cmd_Args());
}

void __cdecl RegularPrint(void) {
    SV_SendServerCommand(NULL, "print \"%s\n\"\n", Cmd_Args());
}

void __cdecl DownloadWorkshopItem(void) { // different to steam_downloadugc as we defer the FS_Restart.
    int argc = Cmd_Argc();
    if (argc < 2) {
        ENGINE_PRINTF("Usage: %s <workshop_id>\n", Cmd_Argv(0));
        return;
    }

    // PublishedFileId_t is 64-bit and passed INT_MAX years ago, so atoi wraps every ID
    // published since. strtoull with the end pointer also rejects trailing junk.
    char*    arg = Cmd_Argv(1);
    char*    end = NULL;
    errno        = 0;
    uint64_t id  = strtoull(arg, &end, 10);
    if (end == arg || *end != '\0' || errno == ERANGE) {
        ENGINE_PRINTF("Usage: %s <workshop_id>\n", Cmd_Argv(0));
        return;
    }

    idSteamServer_DownloadItem(id, 1);
}

void __cdecl StopFollowing(void) {
    int argc = Cmd_Argc();
    if (argc < 2) {
        ENGINE_PRINTF("Usage: %s <client_id>\n", Cmd_Argv(0));
        return;
    }
    if (!ClientCommandsReady()) {
        return;
    }

    int i = atoi(Cmd_Argv(1));
    if (i < 0 || i >= sv_maxclients->integer) {
        ENGINE_PRINTF("client_id must be a number between 0 and %d\n.", sv_maxclients->integer - 1);
        return;
    }

    if (!g_entities[i].inuse) {
        ENGINE_PRINTF("That player isn't currently active.\n");
        return;
    }

    if (g_entities[i].client->sess.spectatorState != SPECTATOR_FOLLOW) {
        ENGINE_PRINTF("That player is not following anyone, current spectatorState == %d\n", g_entities[i].client->sess.spectatorState);
        return;
    }

    ENGINE_PRINTF("Stopping player %d following player %d... ", i, g_entities[i].client->sess.spectatorClient);
    g_entities[i].client->sess.spectatorState = SPECTATOR_FREE;
    g_entities[i].client->ps.pm_flags &= ~PMF_FOLLOW;
    g_entities[i].r.svFlags &= ~SVF_BOT;
    g_entities[i].client->ps.clientNum = i;
    ENGINE_PRINTF("Done.\n");
}

// Reports how long the game thread spends in everything we add to a frame. Off by
// default. See profile.h for how to read the output.
void __cdecl ProfileCommand(void) {
    if (Cmd_Argc() < 2) {
        Profile_Report();
        return;
    }

    const char* arg = Cmd_Argv(1);
    if (!strcmp(arg, "on")) {
        Profile_SetEnabled(1);
        ENGINE_PRINTF("Profiler on, counters reset.\n");
    } else if (!strcmp(arg, "off")) {
        Profile_SetEnabled(0);
        ENGINE_PRINTF("Profiler off. Counters are kept; \"%s\" still reports them.\n", Cmd_Argv(0));
    } else if (!strcmp(arg, "reset")) {
        Profile_Reset();
        ENGINE_PRINTF("Counters reset.\n");
    } else {
        ENGINE_PRINTF("Usage: %s [on|off|reset]\n", Cmd_Argv(0));
    }
}

// Both hang off hooks that only exist in a Python build. See the matching guard on their
// registration in dllmain.c.
#ifndef NOPY
// Reports how close each client is to having reliable commands cycled out of its ring,
// and what the guard has had to do about it. See reliable.h.
void __cdecl ReliableCommand(void) {
    if (Cmd_Argc() > 1 && !strcmp(Cmd_Argv(1), "reset")) {
        Reliable_Reset();
        // Reliable_Reset is the map-change hook, so it drops whatever is still queued as
        // well as the counters.
        ENGINE_PRINTF("Counters reset, queued commands dropped.\n");
        return;
    }
    Reliable_Report();
}

// Reports how much of the team scoreboard is being held back. See scoreboard.h.
void __cdecl ScoreboardCommand(void) {
    if (Cmd_Argc() > 1 && !strcmp(Cmd_Argv(1), "reset")) {
        Scoreboard_Reset();
        ENGINE_PRINTF("Counters reset.\n");
        return;
    }
    if (Cmd_Argc() > 1 && !strcmp(Cmd_Argv(1), "verify")) {
        Scoreboard_Verify();
        return;
    }
    Scoreboard_Report();
}

// Turns CPython's perf trampoline on and off, so `perf record -g -p <pid>` can attribute samples
// to Python functions. minqlxtended._core.perf_trampoline makes the decisions and hands back the
// line to print. Off by default: a trampoline is compiled per code object on first execution.
void __cdecl PyPerfCommand(void) {
    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject* module = PyImport_ImportModule("minqlxtended");
    PyObject* result = NULL;
    if (module != NULL) {
        result = PyObject_CallMethod(module, "perf_trampoline", "s", Cmd_Argc() > 1 ? Cmd_Argv(1) : "");
    }

    if (result != NULL) {
        const char* line = PyUnicode_AsUTF8(result);
        // Never the format itself; the string is built in Python.
        ENGINE_PRINTF("%s\n", line ? line : "perf_trampoline returned something unprintable.");
    } else {
        // DispatcherRelease flushes the indicator, so say something before it goes.
        ENGINE_PRINTF("qlx_pyperf failed; see the log.\n");
    }

    Py_XDECREF(result);
    Py_XDECREF(module);
    DispatcherRelease(gstate);
}

// Execute a pyminqlxtended command as if it were the owner executing it.
// Output will appear in the console.
void __cdecl PyRcon(void) {
    RconDispatcher(Cmd_Args());
}

void __cdecl PyCommand(void) {
    if (!custom_command_handler) {
        return; // No registered handler.
    }
    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    // Re-read the slot under the GIL and hold a reference for the call. The check above is
    // only a hint: register_handler() can empty the slot from any thread while we block in
    // PyGILState_Ensure. Same rule as CallHandler in python_dispatchers.c.
    PyObject* handler = Py_XNewRef(custom_command_handler);
    if (!handler) {
        PROF_END(PROF_CUSTOM_COMMAND, t_work);
        DispatcherRelease(gstate);
        return;
    }

    PyObject* result = PyObject_CallFunction(handler, "s", Cmd_Args());
    if (result == Py_False) {
        ENGINE_PRINTF("The command failed to be executed. pyminqlxtended found no handler.\n");
    }

    Py_XDECREF(result);
    Py_DECREF(handler);
    PROF_END(PROF_CUSTOM_COMMAND, t_work);
    DispatcherRelease(gstate);
}

#endif
