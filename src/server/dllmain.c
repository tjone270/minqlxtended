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

#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "engine/patterns.h"
#include "engine/quake_common.h"
#include "features/demos.h"
#include "features/reliable.h"
#include "features/scoreboard.h"
#include "maps_parser.h"
#ifndef NOPY
#include "python/pyminqlxtended.h"
#endif

// For comparison with the dedi's executable name to avoid segfaulting
// bash and the likes if we run this through a script.
extern char* __progname;

const char qzeroded[]    = "qzeroded.x64";
const char qagame_name[] = "qagamex64.so";

// Global variables.
int common_initialized = 0;
int cvars_initialized  = 0;
atomic_int vm_rehooking = 0;
serverStatic_t* svs;
server_t* sv;

Com_Printf_ptr Com_Printf;
Cmd_AddCommand_ptr Cmd_AddCommand;
Cmd_Args_ptr Cmd_Args;
Cmd_Argv_ptr Cmd_Argv;
Cmd_Argc_ptr Cmd_Argc;
Cmd_TokenizeString_ptr Cmd_TokenizeString;
Cbuf_ExecuteText_ptr Cbuf_ExecuteText;
Cvar_FindVar_ptr Cvar_FindVar;
Cvar_Get_ptr Cvar_Get;
Cvar_GetLimit_ptr Cvar_GetLimit;
Cvar_Set2_ptr Cvar_Set2;
SV_SendServerCommand_ptr SV_SendServerCommand;
SV_ExecuteClientCommand_ptr SV_ExecuteClientCommand;
SV_ClientEnterWorld_ptr SV_ClientEnterWorld;
SV_Shutdown_ptr SV_Shutdown;
SV_Map_f_ptr SV_Map_f;
SV_UpdateUserinfo_f_ptr SV_UpdateUserinfo_f;
SV_SetConfigstring_ptr SV_SetConfigstring;
SV_GetConfigstring_ptr SV_GetConfigstring;
SV_DropClient_ptr SV_DropClient;
SV_SendMessageToClient_ptr SV_SendMessageToClient;
MSG_WriteBits_ptr MSG_WriteBits;
Sys_SetModuleOffset_ptr Sys_SetModuleOffset;
SV_SpawnServer_ptr SV_SpawnServer;
Cmd_ExecuteString_ptr Cmd_ExecuteString;
Sys_IsLANAddress_ptr Sys_IsLANAddress;
idSteamServer_DownloadItem_ptr idSteamServer_DownloadItem;
SV_LinkEntity_ptr SV_LinkEntity;
SV_UnlinkEntity_ptr SV_UnlinkEntity;

// VM functions
G_RunFrame_ptr G_RunFrame;
G_AddEvent_ptr G_AddEvent;
G_InitGame_ptr G_InitGame;
G_ShutdownGame_ptr G_ShutdownGame;
CheckPrivileges_ptr CheckPrivileges;
ClientConnect_ptr ClientConnect;
ClientSpawn_ptr ClientSpawn;
Cmd_CallVote_f_ptr Cmd_CallVote_f;
pint Cmd_CallVote_f_addr;
G_Say_ptr G_Say;
SetTeam_ptr SetTeam;
FireWeapon_ptr FireWeapon;
G_Damage_ptr G_Damage;
player_die_ptr player_die;
Touch_Item_ptr Touch_Item;
LaunchItem_ptr LaunchItem;
Drop_Item_ptr Drop_Item;
G_StartKamikaze_ptr G_StartKamikaze;
G_FreeEntity_ptr G_FreeEntity;
G_SpawnGEntityFromSpawnVars_ptr G_SpawnGEntityFromSpawnVars;
SelectScoreboardMessage_ptr SelectScoreboardMessage;
MP_AllowJoin_ptr MP_AllowJoin;
MP_PauseThink_ptr MP_PauseThink;
MP_StopDemo_ptr MP_StopDemo;
MP_LockOrUnlockTeam_ptr MP_LockOrUnlockTeam;

// The match-play globals. All six are set together or not at all; see quake_common.h.
int* mp_unpauseTime;
int* mp_pauseCaller;
qboolean* mp_pausedByServer;
int* mp_timeoutsUsed;
int* mp_teamLocked;
int* mp_autoActionState;

// VM global variables.
gentity_t* g_entities;
level_locals_t* level;
gitem_t* bg_itemlist;
int bg_numItems;

// Cvars.
cvar_t* sv_maxclients;
cvar_t** cvar_vars;

// Both format into a buffer first and emit prefix and body in one stdio call. stdio locks per
// call, and the demo writer thread logs off the game thread, so two calls can otherwise
// interleave into one line with two prefixes and another with none.
#define DEBUG_BODY_SIZE 2048

// TODO: Make it output everything to a file too.
void DebugPrint(const char* fmt, ...) {
    char body[DEBUG_BODY_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    printf(DEBUG_PRINT_PREFIX "%s", body);
}

// TODO: Make it output everything to a file too.
void DebugError(const char* fmt, const char* file, int line, const char* func, ...) {
    char body[DEBUG_BODY_SIZE];
    va_list args;
    va_start(args, func);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    fprintf(stderr, DEBUG_ERROR_FORMAT "%s", file, line, func, body);
}

#define STATIC_SEARCH(x, p, m)                         \
    x = (x##_ptr)PatternSearchModule(&module, p, m);   \
    if (x == NULL) {                                   \
        DebugPrint("ERROR: Unable to find " #x ".\n"); \
        failed = 1;                                    \
    } else                                             \
        DebugPrint(#x ": %p\n", x)

static void SearchFunctions(void) {
    int failed = 0;
    module_info_t module;
    strcpy(module.name, qzeroded);
    int res = GetModuleInfo(&module);
    if (res <= 0) {
        // Without a module map the pattern searches below have nowhere to look.
        DebugError("GetModuleInfo() returned %d.\n", __FILE__, __LINE__, __func__, res);
        DebugPrint("Exiting.\n");
        exit(1);
    }

    DebugPrint("Searching for necessary functions...\n");

    STATIC_SEARCH(Com_Printf, PTRN_COM_PRINTF, MASK_COM_PRINTF);
    STATIC_SEARCH(Cmd_AddCommand, PTRN_CMD_ADDCOMMAND, MASK_CMD_ADDCOMMAND);
    STATIC_SEARCH(Cmd_Args, PTRN_CMD_ARGS, MASK_CMD_ARGS);
    STATIC_SEARCH(Cmd_Argv, PTRN_CMD_ARGV, MASK_CMD_ARGV);
    STATIC_SEARCH(Cmd_TokenizeString, PTRN_CMD_TOKENIZESTRING, MASK_CMD_TOKENIZESTRING);
    STATIC_SEARCH(Cbuf_ExecuteText, PTRN_CBUF_EXECUTETEXT, MASK_CBUF_EXECUTETEXT);
    STATIC_SEARCH(Cvar_FindVar, PTRN_CVAR_FINDVAR, MASK_CVAR_FINDVAR);
    STATIC_SEARCH(Cvar_Get, PTRN_CVAR_GET, MASK_CVAR_GET);
    STATIC_SEARCH(Cvar_GetLimit, PTRN_CVAR_GETLIMIT, MASK_CVAR_GETLIMIT);
    STATIC_SEARCH(Cvar_Set2, PTRN_CVAR_SET2, MASK_CVAR_SET2);
    STATIC_SEARCH(SV_SendServerCommand, PTRN_SV_SENDSERVERCOMMAND, MASK_SV_SENDSERVERCOMMAND);
    STATIC_SEARCH(SV_ExecuteClientCommand, PTRN_SV_EXECUTECLIENTCOMMAND, MASK_SV_EXECUTECLIENTCOMMAND);
    STATIC_SEARCH(SV_Shutdown, PTRN_SV_SHUTDOWN, MASK_SV_SHUTDOWN);
    STATIC_SEARCH(SV_Map_f, PTRN_SV_MAP_F, MASK_SV_MAP_F);
    STATIC_SEARCH(SV_ClientEnterWorld, PTRN_SV_CLIENTENTERWORLD, MASK_SV_CLIENTENTERWORLD);
    STATIC_SEARCH(SV_SetConfigstring, PTRN_SV_SETCONFIGSTRING, MASK_SV_SETCONFIGSTRING);
    STATIC_SEARCH(SV_GetConfigstring, PTRN_SV_GETCONFIGSTRING, MASK_SV_GETCONFIGSTRING);
    STATIC_SEARCH(SV_DropClient, PTRN_SV_DROPCLIENT, MASK_SV_DROPCLIENT);
    STATIC_SEARCH(SV_SendMessageToClient, PTRN_SV_SENDMESSAGETOCLIENT, MASK_SV_SENDMESSAGETOCLIENT);
    STATIC_SEARCH(MSG_WriteBits, PTRN_MSG_WRITEBITS, MASK_MSG_WRITEBITS);
    STATIC_SEARCH(Sys_SetModuleOffset, PTRN_SYS_SETMODULEOFFSET, MASK_SYS_SETMODULEOFFSET);
    STATIC_SEARCH(SV_SpawnServer, PTRN_SV_SPAWNSERVER, MASK_SV_SPAWNSERVER);
    STATIC_SEARCH(Cmd_ExecuteString, PTRN_CMD_EXECUTESTRING, MASK_CMD_EXECUTESTRING);
    STATIC_SEARCH(Sys_IsLANAddress, PTRN_SYS_ISLANADDRESS, MASK_SYS_ISLANADDRESS);
    STATIC_SEARCH(SV_UpdateUserinfo_f, PTRN_SV_UPDATEUSERINFO_F, MASK_SV_UPDATEUSERINFO_F);
    STATIC_SEARCH(idSteamServer_DownloadItem, PTRN_idSteamServer_DownloadItem, MASK_idSteamServer_DownloadItem);

    /*
     * Tolerated misses: these two only back link_entity() and unlink_entity(), which raise
     * when unresolved. The patterns match the bodies; the engine also carries jmp thunks.
     */
    SV_LinkEntity = (SV_LinkEntity_ptr)PatternSearchModule(&module, PTRN_SV_LINKENTITY, MASK_SV_LINKENTITY);
    if (SV_LinkEntity == NULL) {
        DebugPrint("WARNING: Unable to find SV_LinkEntity. link_entity() will not be available.\n");
    } else {
        DebugPrint("SV_LinkEntity: %p\n", SV_LinkEntity);
    }
    SV_UnlinkEntity = (SV_UnlinkEntity_ptr)PatternSearchModule(&module, PTRN_SV_UNLINKENTITY, MASK_SV_UNLINKENTITY);
    if (SV_UnlinkEntity == NULL) {
        DebugPrint("WARNING: Unable to find SV_UnlinkEntity. unlink_entity() will not be available.\n");
    } else {
        DebugPrint("SV_UnlinkEntity: %p\n", SV_UnlinkEntity);
    }

    // Cmd_Argc is too small to search for, so this follows a reference to it. It gets the same
    // InImage() check the globals below do.
    Cmd_Argc = NULL;
    if (SV_Map_f != NULL) {
        pint candidate = *(int32_t*)OFFSET_RELP_CMD_ARGC + OFFSET_RELP_CMD_ARGC + 4;
        if (InImage(candidate)) {
            Cmd_Argc = (Cmd_Argc_ptr)candidate;
            DebugPrint("Cmd_Argc: %p\n", Cmd_Argc);
        }
    }
    if (Cmd_Argc == NULL) {
        DebugPrint("ERROR: Unable to resolve Cmd_Argc.\n");
        failed = 1;
    }

    // Three globals follow, each from a displacement inside an instruction, all in .bss.
    // InImage() is their shared bounds check. A bad cvar_vars costs minqlxtended.cvars() and a
    // bad sv costs minqlxtended.server, so those two only warn; svs is fatal.

    // svs, from SV_Shutdown's read of it. See OFFSET_PP_SVS; the displacement holds an absolute
    // address, as sv's does. Must happen before HookStatic(): OFFSET_PP_SVS is measured from the
    // SV_Shutdown variable, and hooking overwrites that variable with the trampoline.
    svs = NULL;
    if (SV_Shutdown != NULL) {
        pint candidate = (pint)(*(uint32_t*)OFFSET_PP_SVS);
        if (InImage(candidate)) {
            svs = (serverStatic_t*)candidate;
            DebugPrint("svs: %p\n", svs);
        }
    }
    if (svs == NULL) {
        DebugPrint("ERROR: Unable to resolve svs.\n");
        failed = 1;
    }

    /*
     * The cvar list head, from the read inside Cvar_Get. See OFFSET_RELP_CVAR_VARS. Checked
     * against the image, since walking the list from a wrong pointer takes the server down.
     */
    cvar_vars = NULL;
    if (Cvar_Get != NULL) {
        pint candidate = *(int32_t*)OFFSET_RELP_CVAR_VARS + OFFSET_RELP_CVAR_VARS + 4;
        if (InImage(candidate)) {
            cvar_vars = (cvar_t**)candidate;
            DebugPrint("cvar_vars: %p\n", cvar_vars);
        }
    }
    if (cvar_vars == NULL) {
        DebugPrint("WARNING: Unable to resolve cvar_vars. minqlxtended.cvars() will be empty.\n");
    }

    // sv, from SV_GetConfigstring's read of sv.configstrings. See OFFSET_PP_SV_CONFIGSTRINGS. The
    // displacement is an absolute address, read straight, then the member's offset subtracted to
    // reach the top of the struct. Both are checked against the image: the subtraction is 2 KB,
    // so a displacement near the edge could put sv outside it while configstrings looked fine.
    sv = NULL;
    if (SV_GetConfigstring != NULL) {
        pint configstrings = (pint)(*(uint32_t*)OFFSET_PP_SV_CONFIGSTRINGS);
        pint candidate     = configstrings - offsetof(server_t, configstrings);
        if (InImage(configstrings) && InImage(candidate)) {
            sv = (server_t*)candidate;
            DebugPrint("sv: %p\n", sv);
        }
    }
    if (sv == NULL) {
        DebugPrint("WARNING: Unable to resolve sv. minqlxtended.server will raise.\n");
    }

    if (failed) {
        DebugPrint("Exiting.\n");
        exit(1);
    }
}

// Generous upper bound on how far qagame reaches, for sanity-checking derived pointers.
// The module is roughly 6.5 MB in build 1069.
#define VM_MODULE_SPAN 0x1000000

// The qagame equivalent of InImage. Neither /proc/self/maps nor dl_iterate_phdr knows about
// it, so this is a span from the base rather than a look at the real segments.
int InVm(pint address) {
    return address > (pint)qagame && address < (pint)qagame + VM_MODULE_SPAN;
}

#define VM_SEARCH(x, p, m)                                                     \
    x = (x##_ptr)PatternSearch((void*)((pint)qagame + 0xB000), 0xB0000, p, m); \
    if (x == NULL) {                                                           \
        DebugPrint("ERROR: Unable to find " #x ".\n");                         \
        failed = 1;                                                            \
    } else                                                                     \
        DebugPrint(#x ": %p\n", x)

// Every VM_SEARCH below is fatal. A pattern that stops matching means this build doesn't
// describe the binary it's loaded into, and the events behind that hook would quietly stop
// firing. The tolerated misses at the bottom each cost one optional feature.
void SearchVmFunctions(void) {
    int failed = 0;

    // qagame doesn't show up in /proc/self/maps, so VM_SEARCH scans a fixed span from the
    // module base instead of the real segments.
    VM_SEARCH(G_AddEvent, PTRN_G_ADDEVENT, MASK_G_ADDEVENT);
    VM_SEARCH(CheckPrivileges, PTRN_CHECKPRIVILEGES, MASK_CHECKPRIVILEGES);
    VM_SEARCH(ClientConnect, PTRN_CLIENTCONNECT, MASK_CLIENTCONNECT);
    VM_SEARCH(ClientSpawn, PTRN_CLIENTSPAWN, MASK_CLIENTSPAWN);
    VM_SEARCH(G_Damage, PTRN_G_DAMAGE, MASK_G_DAMAGE);
    VM_SEARCH(Touch_Item, PTRN_TOUCH_ITEM, MASK_TOUCH_ITEM);
    VM_SEARCH(LaunchItem, PTRN_LAUNCHITEM, MASK_LAUNCHITEM);
    VM_SEARCH(Drop_Item, PTRN_DROP_ITEM, MASK_DROP_ITEM);
    VM_SEARCH(G_StartKamikaze, PTRN_G_STARTKAMIKAZE, MASK_G_STARTKAMIKAZE);
    VM_SEARCH(G_FreeEntity, PTRN_G_FREEENTITY, MASK_G_FREEENTITY);

    // Searched here so the address is known before HookVm runs, which the callvote-clientkick
    // patch offset needs.
    VM_SEARCH(Cmd_CallVote_f, PTRN_CMD_CALLVOTE_F, MASK_CMD_CALLVOTE_F);
    Cmd_CallVote_f_addr = (pint)Cmd_CallVote_f;

    VM_SEARCH(G_Say, PTRN_G_SAY, MASK_G_SAY);
    VM_SEARCH(SetTeam, PTRN_SETTEAM, MASK_SETTEAM);
    VM_SEARCH(FireWeapon, PTRN_FIREWEAPON, MASK_FIREWEAPON);

    // player_die, through the GOT slot ClientSpawn loads to fill in ent->die. VM_SEARCH sets
    // `failed` without returning, so a ClientSpawn miss still reaches here and the NULL guard
    // turns a SIGSEGV at 0x6A2 into a clean exit(1).
    player_die = NULL;
    if (ClientSpawn != NULL) {
        pint slot = *(int32_t*)OFFSET_RELP_PP_PLAYER_DIE + OFFSET_RELP_PP_PLAYER_DIE + 4;
        // Both the slot and what it holds have to land inside the module, or the
        // displacement we followed isn't the GOT reference we think it is.
        if (InVm(slot)) {
            pint found = *(pint*)slot;
            if (InVm(found)) {
                player_die = (player_die_ptr)found;
            }
        }
    }
    if (player_die == NULL) {
        DebugPrint("ERROR: Unable to find player_die.\n");
        failed = 1;
    } else {
        DebugPrint("player_die: %p\n", player_die);
    }

    // Tolerated: losing SelectScoreboardMessage leaves stock behaviour. The wiki's Internals
    // page lists the others, and every one of them warns here rather than setting `failed`.
    SelectScoreboardMessage = (SelectScoreboardMessage_ptr)PatternSearch(
        (void*)((pint)qagame + 0xB000), 0xB0000, PTRN_SELECTSCOREBOARDMESSAGE,
        MASK_SELECTSCOREBOARDMESSAGE);
    if (SelectScoreboardMessage == NULL) {
        DebugPrint("WARNING: Unable to find SelectScoreboardMessage. Skipping the "
                   "scoreboard trim...\n");
    } else {
        DebugPrint("SelectScoreboardMessage: %p\n", SelectScoreboardMessage);
    }

    // The match-play globals, from three anchors none of which is ever called. Misses are
    // tolerated, costing the match_state view and Game.lock/unlock. MP_AllowJoin gives the base;
    // MP_PauseThink and MP_StopDemo read the first and last global directly, pinning both ends of
    // a block that is a linker layout rather than a struct. If either disagrees, all six stay NULL.
    MP_AllowJoin = (MP_AllowJoin_ptr)PatternSearch(
        (void*)((pint)qagame + 0xB000), 0xB0000, PTRN_MP_ALLOWJOIN, MASK_MP_ALLOWJOIN);
    MP_PauseThink = (MP_PauseThink_ptr)PatternSearch(
        (void*)((pint)qagame + 0xB000), 0xB0000, PTRN_MP_PAUSETHINK, MASK_MP_PAUSETHINK);
    MP_StopDemo = (MP_StopDemo_ptr)PatternSearch(
        (void*)((pint)qagame + 0xB000), 0xB0000, PTRN_MP_STOPDEMO, MASK_MP_STOPDEMO);

    // Cleared first, since this runs on every VM load: a reload that fails a check below must
    // not leave the previous module's addresses behind to be read.
    mp_unpauseTime     = NULL;
    mp_pauseCaller     = NULL;
    mp_pausedByServer  = NULL;
    mp_timeoutsUsed    = NULL;
    mp_teamLocked      = NULL;
    mp_autoActionState = NULL;

    if (MP_AllowJoin == NULL || MP_PauseThink == NULL || MP_StopDemo == NULL) {
        DebugPrint("WARNING: Unable to find the match-play anchors (MP_AllowJoin: %p, "
                   "MP_PauseThink: %p, MP_StopDemo: %p). match_state will not be "
                   "available.\n",
                   MP_AllowJoin, MP_PauseThink, MP_StopDemo);
    } else {
        pint base       = *(int32_t*)OFFSET_RELP_MP_BASE + OFFSET_RELP_MP_BASE + 4;
        pint unpause    = *(int32_t*)OFFSET_RELP_MP_UNPAUSETIME + OFFSET_RELP_MP_UNPAUSETIME + 4;
        pint autoaction = *(int32_t*)OFFSET_RELP_MP_AUTOACTION + OFFSET_RELP_MP_AUTOACTION + 4;

        if (!InVm(base) || !InVm(unpause) || !InVm(autoaction)) {
            DebugPrint("WARNING: the match-play globals landed outside qagame. match_state "
                       "will not be available.\n");
        } else if (base != unpause || autoaction != base + MP_OFF_AUTOACTION) {
            // Both ends of the block have to be where the other anchor says they are.
            DebugPrint("WARNING: the match-play globals are not laid out as expected "
                       "(base %p, unpause %p, autoaction %p). match_state will not be "
                       "available.\n",
                       (void*)base, (void*)unpause, (void*)autoaction);
        } else {
            mp_unpauseTime     = (int*)unpause;
            mp_pauseCaller     = (int*)(base + MP_OFF_PAUSECALLER);
            mp_pausedByServer  = (qboolean*)(base + MP_OFF_PAUSEDBYSERVER);
            mp_timeoutsUsed    = (int*)(base + MP_OFF_TIMEOUTSUSED);
            mp_teamLocked      = (int*)(base + MP_OFF_TEAMLOCKED);
            mp_autoActionState = (int*)autoaction;
            DebugPrint("match-play globals: %p\n", (void*)base);
        }
    }

    // Tolerated separately from the anchors above, since this one only writes the locks.
    // Losing it leaves match_state readable and lock/unlock raising.
    MP_LockOrUnlockTeam = (MP_LockOrUnlockTeam_ptr)PatternSearch(
        (void*)((pint)qagame + 0xB000), 0xB0000, PTRN_MP_LOCKORUNLOCKTEAM,
        MASK_MP_LOCKORUNLOCKTEAM);
    if (MP_LockOrUnlockTeam == NULL) {
        DebugPrint("WARNING: Unable to find MP_LockOrUnlockTeam. Locking teams will not be "
                   "available.\n");
    } else {
        DebugPrint("MP_LockOrUnlockTeam: %p\n", MP_LockOrUnlockTeam);
    }

    // Tolerated: the generic spawn path only backs spawn_entity(), which raises unresolved.
    G_SpawnGEntityFromSpawnVars = (G_SpawnGEntityFromSpawnVars_ptr)PatternSearch(
        (void*)((pint)qagame + 0xB000), 0xB0000, PTRN_G_SPAWNGENTITYFROMSPAWNVARS,
        MASK_G_SPAWNGENTITYFROMSPAWNVARS);
    if (G_SpawnGEntityFromSpawnVars == NULL) {
        DebugPrint("WARNING: Unable to find G_SpawnGEntityFromSpawnVars. spawn_entity() "
                   "will not be available.\n");
    } else {
        DebugPrint("G_SpawnGEntityFromSpawnVars: %p\n", G_SpawnGEntityFromSpawnVars);
    }

    if (failed) {
        DebugPrint("Exiting.\n");
        exit(1);
    }
}

// Called from My_Cmd_AddCommand(). At the entry point this segfaults: Cmd_AddCommand and the
// rest are not initialised yet.
void InitializeStatic(void) {
    DebugPrint("Initializing...\n");

    // Com_Init reaches here on the thread that goes on to run Com_Frame, and the interpreter
    // below is the first thing that could ask. See common.h.
    NoteGameThread();

    Cmd_AddCommand("cmd", SendServerCommand);
    Cmd_AddCommand("cp", CenterPrint);
    Cmd_AddCommand("print", RegularPrint);
    Cmd_AddCommand("steam_downloadugcdefer", DownloadWorkshopItem);
    Cmd_AddCommand("stopfollowing", StopFollowing);
    Cmd_AddCommand("qlx_prof", ProfileCommand);
#ifndef NOPY
    // These guard hooks that only a Python build installs. qlx_prof stays out of the guard,
    // since Demo_Capture is hooked either way.
    Cmd_AddCommand("qlx_reliable", ReliableCommand);
    Cmd_AddCommand("qlx_scoreboard", ScoreboardCommand);
    Cmd_AddCommand("qlx_pyperf", PyPerfCommand);
    Cmd_AddCommand("qlx", PyRcon);
    Cmd_AddCommand("pycmd", PyCommand);
#endif

#ifndef NOPY
    PyMinqlxtended_InitStatus_t res = PyMinqlxtended_Initialize();
    if (res != PYM_SUCCESS) {
        DebugPrint("Python initialization failed: %d\n", res);
        exit(1);
    }
#endif

    common_initialized = 1;
}

// Initialize VM stuff. Needs to be called whenever Sys_SetModuleOffset is called,
// after qagame pointer has been initialized.
void InitializeVm(void) {
    DebugPrint("Initializing VM pointers...\n");

    // All three are displacements followed out of an instruction, so they get the same check svs
    // and cvar_vars get in SearchFunctions. The loop at the bottom walks bg_itemlist to a NULL
    // classname, so a bad pointer there is an unbounded scan.
    pint entities = *(int32_t*)OFFSET_RELP_G_ENTITIES + OFFSET_RELP_G_ENTITIES + 4;
    pint locals   = *(int32_t*)OFFSET_RELP_LEVEL + OFFSET_RELP_LEVEL + 4;
    pint itemlist = *(int32_t*)OFFSET_RELP_BG_ITEMLIST + OFFSET_RELP_BG_ITEMLIST + 4;

    if (!InVm(entities) || !InVm(locals) || !InVm(itemlist)) {
        DebugPrint("ERROR: g_entities, level or bg_itemlist landed outside qagame.\nExiting.\n");
        exit(1);
    }

    g_entities = (gentity_t*)entities;
    level      = (level_locals_t*)locals;

    // itemlist holds a pointer rather than being one, so the value read through it needs
    // checking as well.
    bg_itemlist = (gitem_t*)*(int64_t*)itemlist;
    if (!InVm((pint)bg_itemlist)) {
        DebugPrint("ERROR: bg_itemlist points outside qagame.\nExiting.\n");
        exit(1);
    }

    for (bg_numItems = 1; bg_itemlist[bg_numItems].classname; bg_numItems++)
        ;
}

// Called after the game is initialized.
void InitializeCvars(void) {
    sv_maxclients = Cvar_FindVar("sv_maxclients");

    Demo_Init(); // Register the sv_demo* cvars now.
#ifndef NOPY
    Reliable_Init();   // Same for qlx_reliable*.
    Scoreboard_Init(); // ...and qlx_scoreboard*.
#endif

    cvars_initialized = 1;
}

__attribute__((constructor)) void EntryPoint(void) {
    if (strcmp(__progname, qzeroded)) {
        return;
    }

    // The engine's crash handler prints load addresses for qagame, cgame and ui only, so a
    // backtrace frame in this library has nothing to subtract from. Print ours first.
    Dl_info self;
    if (dladdr((void*)EntryPoint, &self)) {
        DebugPrint("minqlxtended: %p\n", self.dli_fbase);
    }

    // Resolves every function we hook plus the three derived globals (svs, sv, cvar_vars), and
    // exits if anything mandatory is missing.
    SearchFunctions();

    DebugPrint("Shared library loaded! Version: %s\n", MINQLXTENDED_VERSION);
    HookStatic();
}
