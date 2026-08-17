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

#ifndef COMMON_H
#define COMMON_H

#ifndef MINQLXTENDED_VERSION
#define MINQLXTENDED_VERSION "NOT_SET"
#endif

#define DEBUG_PRINT_PREFIX "[minqlxtended] "
#define DEBUG_ERROR_FORMAT "[minqlxtended] ERROR @ %s:%d in %s:\n" DEBUG_PRINT_PREFIX

#ifndef NOPY
#define SV_TAGS_PREFIX "minqlxtended"
#else
#define SV_TAGS_PREFIX "minqlxtended-nopy"
#endif

// TODO: Add minqlxtended version to serverinfo.

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(__x86_64__) && !defined(_M_X64)
#error "minqlxtended supports x64 only; there is no 32-bit Quake Live dedicated server."
#endif

// Pointer-sized integers, since Windows keeps int at 32 bits on 64-bit builds.
typedef uint64_t pint;
typedef int64_t sint;
#define __cdecl

extern int common_initialized;
extern int cvars_initialized;

/*
 * Non-zero while SV_SpawnServer is rebuilding the world. It suppresses the per-frame
 * dispatchers only: player_disconnect, cvar_changed and console_print all still fire, and
 * Plugin.game is None throughout. See the Events page in the wiki.
 */
extern int sv_spawning;

/*
 * Set for the whole interval in which there is no usable game module. Raised in
 * My_G_ShutdownGame, which the engine calls immediately before VM_Free on every path that drops
 * the module, and cleared in My_Sys_SetModuleOffset once the rebuild is done; a shutdown with no
 * reload leaves it raised until a map loads. g_entities, level and bg_itemlist are NULLed with
 * it, so it only selects the message the resolvers raise. The game thread writes it and Python
 * worker threads read it, so a call already past the check is still exposed for the few
 * instructions before its first dereference.
 */
extern atomic_int vm_rehooking;

/*
 * The game thread: the one the engine runs Com_Frame on, and the only one that may touch engine
 * or game-module memory. NoteGameThread is called once from InitializeStatic, which the engine
 * reaches on that thread and before any other thread exists, so OnGameThread reads a value
 * written once and needs no synchronisation of its own.
 */
void NoteGameThread(void);
int OnGameThread(void);

void InitializeStatic(void);
void InitializeVm(void);
void InitializeCvars(void);
void SearchVmFunctions(void); // Needs to be called every time the VM is loaded.
void HookStatic(void);
void HookVm(void);
// DebugError's varargs start after func, hence the 5.
void DebugPrint(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void DebugError(const char* fmt, const char* file, int line, const char* func, ...)
    __attribute__((format(printf, 1, 5)));

void* PatternSearch(void* address, size_t length, const char* pattern, const char* mask);

// Making engine memory writable is in hook/protect.h, with its only two callers.

// PatternSearchModule is in server/maps_parser.h, alongside the module_info_t it takes.

#endif /* COMMON_H */
