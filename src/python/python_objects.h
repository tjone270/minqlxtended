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

#ifndef PYTHON_OBJECTS_H
#define PYTHON_OBJECTS_H

#include <Python.h>

#include "engine/quake_common.h"

/*
 * Live views onto the engine's own structs, as Python objects with properties. Reading an
 * attribute dereferences the engine global there and then, and assigning writes straight
 * through, so none of these is a snapshot the way python_embed.c's struct sequences are.
 * minqlxtended.level is the first, and Entity, GameClient, Client, Item and Cvar follow the
 * same shape.
 *
 *   - Game thread only. Touch one of these from a worker thread and you get a torn read,
 *     or a corrupted level on a write. Use minqlxtended.next_frame.
 *
 *   - Almost everything is writable, and writing a nonsensical value does whatever the
 *     game module does with one.
 */

// 0 on success, -1 with an exception set. Call after the struct sequences are registered.
int PyMinqlxtended_AddObjectTypes(PyObject* module);

/*
 * Raised when the engine global asked about is not there: no map loaded, or no VM yet.
 * Distinct from ValueError, which means the caller named something out of range.
 */
extern PyObject* qlx_EngineStateError;

/*
 * Builds a Vector3, the struct sequence python_embed.c registers and defines. New
 * reference, or NULL with an exception set.
 */
PyObject* PyMinqlxtended_Vector3(const vec_t v[3]);

/*
 * A per-weapon int[16] as the Weapons struct sequence, skipping index 0 (WP_NONE). Also in
 * python_embed.c, so GameClient.expanded_stats and player_expanded_stats() agree on shape.
 */
PyObject* PyMinqlxtended_Weapons(const int* arr);

/*
 * minqlxtended.entities(). Defined here, registered in python_embed.c's method table.
 */
PyObject* PyMinqlxtended_Entities(PyObject* self, PyObject* args, PyObject* kwds);
PyObject* PyMinqlxtended_Items(PyObject* self, PyObject* args);

/*
 * qtrue with a ValueError set if `name` must not be written with force. Both set_cvar() and
 * the Cvar view's setters go through it; see the definition for which cvar and why.
 */
qboolean qlx_refuse_forced_write(const char* name, int force);

/*
 * An Entity by number, for the natives in python_embed.c that hand one back. New
 * reference, or NULL with an exception set. The caller vouches for the index.
 */
PyObject* PyMinqlxtended_MakeEntity(int index);
PyObject* PyMinqlxtended_MakeCvar(const char* name);
PyObject* PyMinqlxtended_Cvar(PyObject* self, PyObject* args);
PyObject* PyMinqlxtended_Cvars(PyObject* self, PyObject* args);

#endif /* PYTHON_OBJECTS_H */
