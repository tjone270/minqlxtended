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

#include "python_objects.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "engine_fields.h"

/*
 * The X-macro lists in engine_fields.h expanded through the field kinds below. Each row
 * emits the accessors, a PyGetSetDef row and a _Static_assert on the byte offset.
 */

PyObject* qlx_EngineStateError = NULL;

/*
 * `index` is an entity, client or item number, -1 for the singletons. InitializeVm
 * re-resolves g_entities, level and bg_itemlist on every map load, so nothing is cached.
 */
typedef struct {
    PyObject_HEAD
    int index;
} qlx_ref_t;

static PyTypeObject qlx_level_type;
static PyTypeObject qlx_matchstate_type;
static PyTypeObject qlx_roundstate_type;
static PyTypeObject qlx_intarray_type;
static PyTypeObject qlx_entity_type;
static PyTypeObject qlx_entitystate_type;
static PyTypeObject qlx_entityshared_type;
static PyTypeObject qlx_entityiter_type;
static PyTypeObject qlx_gameclient_type;
static PyTypeObject qlx_playerstate_type;
static PyTypeObject qlx_persistant_type;
static PyTypeObject qlx_session_type;
static PyTypeObject qlx_teamstate_type;
static PyTypeObject qlx_raceinfo_type;
static PyTypeObject qlx_expandedstats_type;
static PyTypeObject qlx_client_type;
static PyTypeObject qlx_netchan_type;
static PyTypeObject qlx_server_type;
static PyTypeObject qlx_serverstatic_type;

// Allocates through the type's own tp_alloc, so the constructible types and the
// singletons are freed the same way. Only valid after PyType_Ready.
static PyObject* qlx_ref_new(PyTypeObject* type, int index) {
    qlx_ref_t* self = (qlx_ref_t*)type->tp_alloc(type, 0);
    if (!self) {
        return NULL;
    }

    self->index = index;
    return (PyObject*)self;
}

// Resolvers: the only place an engine global is touched. Each returns NULL with an exception set.

/*
 * My_G_ShutdownGame clears all three globals and raises vm_rehooking before the engine
 * unmaps qagame, so the caller's pointer test is the guard; this only picks the message.
 */
static void qlx_no_game_module(const char* what) {
    if (atomic_load_explicit(&vm_rehooking, memory_order_acquire)) {
        PyErr_Format(qlx_EngineStateError,
                     "%s is not available; the game module is being reloaded", what);
    } else {
        PyErr_Format(qlx_EngineStateError,
                     "%s is not available; no map has been loaded yet", what);
    }
}

static level_locals_t* qlx_level(PyObject* self) {
    (void)self;
    if (!level) {
        qlx_no_game_module("level");
        return NULL;
    }

    return level;
}

/*
 * sv comes out of a displacement SearchFunctions is allowed to miss, leaving it NULL, so it
 * can be absent on a server that is otherwise fine. level cannot.
 */
static server_t* qlx_server(PyObject* self) {
    (void)self;
    if (!sv) {
        PyErr_SetString(qlx_EngineStateError,
                        "sv could not be resolved in this build of the dedicated server");
        return NULL;
    }

    return sv;
}

static serverStatic_t* qlx_serverstatic(PyObject* self) {
    (void)self;
    if (!svs) {
        PyErr_SetString(qlx_EngineStateError, "svs is not available yet");
        return NULL;
    }

    return svs;
}

static roundState_t* qlx_roundstate(PyObject* self) {
    level_locals_t* lvl = qlx_level(self);
    if (!lvl) {
        return NULL;
    }

    return &lvl->roundState;
}

/*
 * SearchVmFunctions sets all six match-play pointers together, so testing one answers for
 * all. They rest on byte patterns the build may miss, so this can fail on a good server.
 */
static int qlx_matchstate_ready(void) {
    if (!mp_unpauseTime) {
        PyErr_SetString(qlx_EngineStateError,
                        "the match-play globals could not be resolved in this build");
        return 0;
    }

    return 1;
}

static gentity_t* qlx_entity(PyObject* self) {
    int index = ((qlx_ref_t*)self)->index;

    if (!g_entities) {
        qlx_no_game_module("g_entities");
        return NULL;
    }

    /*
     * Memory safety only. An Entity held across a map load describes whatever occupies
     * that slot next.
     */
    if (index < 0 || index >= MAX_GENTITIES) {
        PyErr_Format(PyExc_ValueError, "entity number %d is out of range (0-%d)", index,
                     MAX_GENTITIES - 1);
        return NULL;
    }

    return &g_entities[index];
}

/*
 * The gclient_t behind a client number. Through g_entities[n].client, since
 * &level->clients[n] is non-NULL for every slot and zeroed for an empty one.
 */
static gclient_t* qlx_gclient(PyObject* self) {
    int index = ((qlx_ref_t*)self)->index;

    if (!g_entities) {
        qlx_no_game_module("g_entities");
        return NULL;
    }

    // sv_maxclients is only set once InitializeCvars has run, and nothing else here guards
    // it. A NULL deref would take the server down instead of raising.
    if (!sv_maxclients) {
        PyErr_SetString(qlx_EngineStateError, "sv_maxclients is not available yet");
        return NULL;
    }

    if (index < 0 || index >= sv_maxclients->integer) {
        PyErr_Format(PyExc_ValueError, "client id %d is out of range (0-%d)", index,
                     sv_maxclients->integer - 1);
        return NULL;
    }

    gclient_t* client = g_entities[index].client;
    if (!client) {
        PyErr_Format(qlx_EngineStateError, "client %d has no game client; nobody is in "
                                           "that slot",
                     index);
        return NULL;
    }

    return client;
}

static playerState_t* qlx_playerstate(PyObject* self) {
    gclient_t* client = qlx_gclient(self);
    return client ? &client->ps : NULL;
}

static clientPersistant_t* qlx_pers(PyObject* self) {
    gclient_t* client = qlx_gclient(self);
    return client ? &client->pers : NULL;
}

static clientSession_t* qlx_sess(PyObject* self) {
    gclient_t* client = qlx_gclient(self);
    return client ? &client->sess : NULL;
}

static playerTeamState_t* qlx_teamstate(PyObject* self) {
    gclient_t* client = qlx_gclient(self);
    return client ? &client->pers.teamState : NULL;
}

static raceInfo_t* qlx_race(PyObject* self) {
    gclient_t* client = qlx_gclient(self);
    return client ? &client->race : NULL;
}

static expandedStatObj_t* qlx_xstats(PyObject* self) {
    gclient_t* client = qlx_gclient(self);
    return client ? &client->expandedStats : NULL;
}

// The server's connection record for a client slot, from svs->clients.
static client_t* qlx_client(PyObject* self) {
    int index = ((qlx_ref_t*)self)->index;

    if (!svs || !svs->clients) {
        PyErr_SetString(qlx_EngineStateError,
                        "svs->clients is not available; the server is not up");
        return NULL;
    }

    if (!sv_maxclients) {
        PyErr_SetString(qlx_EngineStateError, "sv_maxclients is not available yet");
        return NULL;
    }

    if (index < 0 || index >= sv_maxclients->integer) {
        PyErr_Format(PyExc_ValueError, "client id %d is out of range (0-%d)", index,
                     sv_maxclients->integer - 1);
        return NULL;
    }

    return &svs->clients[index];
}

static netchan_t* qlx_netchan(PyObject* self) {
    client_t* client = qlx_client(self);
    return client ? &client->netchan : NULL;
}

static entityState_t* qlx_entstate(PyObject* self) {
    gentity_t* ent = qlx_entity(self);
    if (!ent) {
        return NULL;
    }

    return &ent->s;
}

static entityShared_t* qlx_entshared(PyObject* self) {
    gentity_t* ent = qlx_entity(self);
    if (!ent) {
        return NULL;
    }

    return &ent->r;
}

/*
 * A gentity_t pointer as an Entity, or None. Bounds-checked, so a stale pointer comes back
 * as None rather than an Entity whose index is nonsense.
 */
static PyObject* qlx_entity_from_ptr(const gentity_t* p) {
    if (!p || !g_entities || p < g_entities || p >= g_entities + MAX_GENTITIES) {
        Py_RETURN_NONE;
    }

    return qlx_ref_new(&qlx_entity_type, (int)(p - g_entities));
}

// An Entity by number, for python_embed.c natives that hand one back (spawn_entity).
// The caller vouches for the index; this is qlx_ref_new with the type filled in.
PyObject* PyMinqlxtended_MakeEntity(int index) {
    return qlx_ref_new(&qlx_entity_type, index);
}

static int qlx_store_entref(PyObject* value, gentity_t** out, const char* name) {
    if (value == NULL) {
        PyErr_Format(PyExc_TypeError, "cannot delete '%s'", name);
        return -1;
    }

    if (value == Py_None) {
        *out = NULL;
        return 0;
    }

    if (!PyObject_TypeCheck(value, &qlx_entity_type)) {
        PyErr_Format(PyExc_TypeError, "'%s' takes a minqlxtended.Entity or None", name);
        return -1;
    }

    if (!g_entities) {
        PyErr_SetString(qlx_EngineStateError,
                        "g_entities is not available; the game module is not loaded");
        return -1;
    }

    int index = ((qlx_ref_t*)value)->index;
    if (index < 0 || index >= MAX_GENTITIES) {
        PyErr_Format(PyExc_ValueError, "entity number %d is out of range (0-%d)", index,
                     MAX_GENTITIES - 1);
        return -1;
    }

    *out = &g_entities[index];
    return 0;
}

// Shared value converters: the field-kind macros defer to these, so each error message lives in one place.

// Rejects deletion up front. `del level.time` is never meaningful, and CPython signals it
// by passing NULL instead of using a separate slot.
static int qlx_check_assignable(PyObject* value, const char* name) {
    if (value == NULL) {
        PyErr_Format(PyExc_TypeError, "cannot delete '%s'", name);
        return -1;
    }

    return 0;
}

static int qlx_store_int(PyObject* value, int* out, const char* name) {
    if (qlx_check_assignable(value, name)) {
        return -1;
    }

    long v = PyLong_AsLong(value);
    if (v == -1 && PyErr_Occurred()) {
        return -1;
    }

    /*
     * Range-checked, since a Python int wider than the engine's would spill part of itself
     * into the next field along.
     */
    if (v < INT_MIN || v > INT_MAX) {
        PyErr_Format(PyExc_OverflowError, "'%s' does not fit in the engine's int", name);
        return -1;
    }

    *out = (int)v;
    return 0;
}

static int qlx_store_bool(PyObject* value, qboolean* out, const char* name) {
    if (qlx_check_assignable(value, name)) {
        return -1;
    }

    int truth = PyObject_IsTrue(value);
    if (truth < 0) {
        return -1;
    }

    *out = truth ? qtrue : qfalse;
    return 0;
}

static int qlx_store_vec3(PyObject* value, vec_t* out, const char* name) {
    if (qlx_check_assignable(value, name)) {
        return -1;
    }

    PyObject* seq = PySequence_Fast(value, "expected a sequence of three numbers");
    if (!seq) {
        return -1;
    }

    if (PySequence_Fast_GET_SIZE(seq) != 3) {
        PyErr_Format(PyExc_ValueError, "'%s' takes exactly three numbers, got %zd", name,
                     PySequence_Fast_GET_SIZE(seq));
        Py_DECREF(seq);
        return -1;
    }

    vec_t parsed[3];
    for (int i = 0; i < 3; i++) {
        double v = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, i));
        if (v == -1.0 && PyErr_Occurred()) {
            Py_DECREF(seq);
            return -1;
        }
        parsed[i] = (vec_t)v;
    }
    Py_DECREF(seq);

    // Only after every element has converted, so a bad third element cannot leave the
    // first two written and the entity halfway to somewhere.
    for (int i = 0; i < 3; i++) {
        out[i] = parsed[i];
    }

    return 0;
}

static PyObject* qlx_load_charbuf(const char* buf, size_t size) {
    /*
     * strnlen: the engine fills several of these with strncpy and does not always
     * terminate. "ignore", since a player name carries whatever bytes the client sent.
     */
    return PyUnicode_DecodeUTF8(buf, (Py_ssize_t)strnlen(buf, size), "ignore");
}

static int qlx_store_charbuf(PyObject* value, char* buf, size_t size, const char* name) {
    if (qlx_check_assignable(value, name)) {
        return -1;
    }

    if (!PyUnicode_Check(value)) {
        PyErr_Format(PyExc_TypeError, "'%s' takes a str", name);
        return -1;
    }

    Py_ssize_t len = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(value, &len);
    if (!utf8) {
        return -1;
    }

    /*
     * Raises rather than truncating: a vote string clipped at 1023 bytes runs a *different
     * vote* to the one asked for, with nothing at the call site to say so.
     */
    if ((size_t)len >= size) {
        PyErr_Format(PyExc_ValueError,
                     "'%s' holds at most %zu bytes and would need %zd", name, size - 1, len);
        return -1;
    }

    memcpy(buf, utf8, (size_t)len);
    memset(buf + len, 0, size - (size_t)len);
    return 0;
}

// IntArray
// A mutable view onto a fixed int[N] in an engine struct, so `level.team_scores[1] = 5`
// works. It holds an owner reference and a thunk, so no engine pointer is kept.

typedef int* (*qlx_intarray_resolve_t)(PyObject* owner, Py_ssize_t* length);

/*
 * Optional stand-in for storing to the array, so the team locks assign through the game
 * module's own setter. Returns 0, or -1 with an exception set.
 */
typedef int (*qlx_intarray_write_t)(Py_ssize_t index, int value, const char* name);

typedef struct {
    PyObject_HEAD
    PyObject* owner;
    qlx_intarray_resolve_t resolve;
    qlx_intarray_write_t write; // NULL to store straight to the array
    const char* name;
} qlx_intarray_t;

static PyObject* qlx_intarray_new_hooked(PyObject* owner, qlx_intarray_resolve_t resolve,
                                         qlx_intarray_write_t write, const char* name) {
    qlx_intarray_t* self = PyObject_New(qlx_intarray_t, &qlx_intarray_type);
    if (!self) {
        return NULL;
    }

    Py_INCREF(owner);
    self->owner   = owner;
    self->resolve = resolve;
    self->write   = write;
    self->name    = name;
    return (PyObject*)self;
}

static PyObject* qlx_intarray_new(PyObject* owner, qlx_intarray_resolve_t resolve,
                                  const char* name) {
    return qlx_intarray_new_hooked(owner, resolve, NULL, name);
}

static void qlx_intarray_dealloc(PyObject* self) {
    Py_XDECREF(((qlx_intarray_t*)self)->owner);
    PyObject_Del(self);
}

static Py_ssize_t qlx_intarray_length(PyObject* self) {
    qlx_intarray_t* arr = (qlx_intarray_t*)self;
    Py_ssize_t length   = 0;
    if (!arr->resolve(arr->owner, &length)) {
        return -1;
    }

    return length;
}

static PyObject* qlx_intarray_item(PyObject* self, Py_ssize_t i) {
    qlx_intarray_t* arr = (qlx_intarray_t*)self;
    Py_ssize_t length   = 0;
    int* values         = arr->resolve(arr->owner, &length);
    if (!values) {
        return NULL;
    }

    if (i < 0 || i >= length) {
        PyErr_Format(PyExc_IndexError, "'%s' index out of range (0-%zd)", arr->name,
                     length - 1);
        return NULL;
    }

    return PyLong_FromLong((long)values[i]);
}

static int qlx_intarray_ass_item(PyObject* self, Py_ssize_t i, PyObject* value) {
    qlx_intarray_t* arr = (qlx_intarray_t*)self;
    Py_ssize_t length   = 0;
    int* values         = arr->resolve(arr->owner, &length);
    if (!values) {
        return -1;
    }

    if (i < 0 || i >= length) {
        PyErr_Format(PyExc_IndexError, "'%s' index out of range (0-%zd)", arr->name,
                     length - 1);
        return -1;
    }

    if (value == NULL) {
        PyErr_Format(PyExc_TypeError, "cannot delete from '%s'; it is a fixed-size array",
                     arr->name);
        return -1;
    }

    if (arr->write) {
        int wanted = 0;
        if (qlx_store_int(value, &wanted, arr->name) < 0) {
            return -1;
        }

        return arr->write(i, wanted, arr->name);
    }

    return qlx_store_int(value, &values[i], arr->name);
}

// Compares equal to any sequence with the same contents, so a test can say
// `assert level.team_scores == [0, 3, 1, 0]`.
static PyObject* qlx_intarray_richcompare(PyObject* self, PyObject* other, int op) {
    if ((op != Py_EQ && op != Py_NE) || !PySequence_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    PyObject* as_list = PySequence_List(self);
    if (!as_list) {
        return NULL;
    }

    PyObject* other_list = PySequence_List(other);
    if (!other_list) {
        Py_DECREF(as_list);
        return NULL;
    }

    PyObject* result = PyObject_RichCompare(as_list, other_list, op);
    Py_DECREF(as_list);
    Py_DECREF(other_list);
    return result;
}

static PyObject* qlx_intarray_repr(PyObject* self) {
    PyObject* as_list = PySequence_List(self);
    if (!as_list) {
        // A repr should never raise.
        PyErr_Clear();
        return PyUnicode_FromFormat("<%s (engine not ready)>",
                                    ((qlx_intarray_t*)self)->name);
    }

    PyObject* result = PyObject_Repr(as_list);
    Py_DECREF(as_list);
    return result;
}

/*
 * sq_item never sees a slice, so mp_subscript handles slicing and negative indices here.
 * mp_ass_subscript stays undefined, so slice assignment refuses instead of resizing.
 */
static PyObject* qlx_intarray_subscript(PyObject* self, PyObject* key) {
    if (PySlice_Check(key)) {
        PyObject* as_list = PySequence_List(self);
        if (!as_list) {
            return NULL;
        }

        PyObject* result = PyObject_GetItem(as_list, key);
        Py_DECREF(as_list);
        return result;
    }

    Py_ssize_t i = PyNumber_AsSsize_t(key, PyExc_IndexError);
    if (i == -1 && PyErr_Occurred()) {
        return NULL;
    }

    if (i < 0) {
        Py_ssize_t length = qlx_intarray_length(self);
        if (length < 0) {
            return NULL;
        }
        i += length;
    }

    return qlx_intarray_item(self, i);
}

static PySequenceMethods qlx_intarray_as_sequence = {
    .sq_length   = qlx_intarray_length,
    .sq_item     = qlx_intarray_item,
    .sq_ass_item = qlx_intarray_ass_item,
};

static PyMappingMethods qlx_intarray_as_mapping = {
    .mp_subscript = qlx_intarray_subscript,
};

static PyTypeObject qlx_intarray_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.IntArray",
    .tp_basicsize                          = sizeof(qlx_intarray_t),
    .tp_dealloc                            = qlx_intarray_dealloc,
    .tp_repr                               = qlx_intarray_repr,
    .tp_as_sequence                        = &qlx_intarray_as_sequence,
    .tp_as_mapping                         = &qlx_intarray_as_mapping,
    .tp_richcompare                        = qlx_intarray_richcompare,
    .tp_flags                              = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc = "A live view onto a fixed-size int array inside an engine struct.\n\n"
              "Indexing reads and writes the engine directly, so it can be assigned to "
              "element-wise. Assigning to the attribute it came from replaces every "
              "element at once and requires a sequence of exactly the right length.\n\n"
              "Each access re-derives the array, so hoist the view out of a hot loop "
              "instead of re-reading the attribute.",
};

// Whole-array assignment, shared by every INTARR setter.
static int qlx_store_intarray(PyObject* value, int* out, Py_ssize_t length,
                              const char* name) {
    if (qlx_check_assignable(value, name)) {
        return -1;
    }

    PyObject* seq = PySequence_Fast(value, "expected a sequence of ints");
    if (!seq) {
        return -1;
    }

    if (PySequence_Fast_GET_SIZE(seq) != length) {
        PyErr_Format(PyExc_ValueError, "'%s' takes exactly %zd ints, got %zd", name, length,
                     PySequence_Fast_GET_SIZE(seq));
        Py_DECREF(seq);
        return -1;
    }

    // Converted in full before anything is written, so a bad element late in the sequence
    // cannot leave the array half-updated.
    int* parsed = PyMem_New(int, (size_t)length);
    if (!parsed) {
        Py_DECREF(seq);
        PyErr_NoMemory();
        return -1;
    }

    for (Py_ssize_t i = 0; i < length; i++) {
        if (qlx_store_int(PySequence_Fast_GET_ITEM(seq, i), &parsed[i], name)) {
            PyMem_Free(parsed);
            Py_DECREF(seq);
            return -1;
        }
    }
    Py_DECREF(seq);

    memcpy(out, parsed, (size_t)length * sizeof(int));
    PyMem_Free(parsed);
    return 0;
}

// Field kinds
// Each emits a getter and, where writable, a setter. PREFIX namespaces the symbols, CTYPE
// is the struct, RESOLVE re-derives a pointer to it, FIELD the member, NAME the attribute.

#define QLX_GETSET_INT(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                                \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return PyLong_FromLong((long)p->FIELD);                                            \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        int parsed;                                                                        \
        if (qlx_store_int(value, &parsed, #NAME)) {                                        \
            return -1;                                                                     \
        }                                                                                  \
        /*                                                                                 \
         * A plain assignment. An int* alias would land on the next field for the        \
         * narrower enum members here (team_t, moverState_t, ...).                          \
         */                                                                                \
        p->FIELD = parsed;                                                                 \
        return 0;                                                                          \
    }

/*
 * Getter-only kinds. bg_itemlist sits in .data.rel.ro, inside PT_GNU_RELRO, which glibc
 * mprotects PROT_READ after relocation, so a setter for one of Item's scalars would fault.
 */
#define QLX_GETSET_INT_RO(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                             \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        return p ? PyLong_FromLong((long)p->FIELD) : NULL;                                 \
    }

#define QLX_GETSET_UINT_RO(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                            \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        return p ? PyLong_FromUnsignedLong((unsigned long)p->FIELD) : NULL;                \
    }

#define QLX_GETSET_BOOL_RO(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                            \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        return p ? PyBool_FromLong((long)p->FIELD) : NULL;                                 \
    }

/*
 * Narrow integer members, given their own kinds so the value is range-checked against what
 * fits. Otherwise assigning 300 to a movement axis silently becomes 44.
 */
#define QLX_GETSET_SCHAR(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                              \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return PyLong_FromLong((long)(signed char)p->FIELD);                               \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        int parsed;                                                                        \
        if (qlx_store_int(value, &parsed, #NAME)) {                                        \
            return -1;                                                                     \
        }                                                                                  \
        if (parsed < -128 || parsed > 127) {                                               \
            PyErr_Format(PyExc_OverflowError, "'%s' takes -128 to 127", #NAME);            \
            return -1;                                                                     \
        }                                                                                  \
        p->FIELD = (signed char)parsed;                                                    \
        return 0;                                                                          \
    }

#define QLX_GETSET_BYTE(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                               \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return PyLong_FromUnsignedLong((unsigned long)(unsigned char)p->FIELD);            \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        int parsed;                                                                        \
        if (qlx_store_int(value, &parsed, #NAME)) {                                        \
            return -1;                                                                     \
        }                                                                                  \
        if (parsed < 0 || parsed > 255) {                                                  \
            PyErr_Format(PyExc_OverflowError, "'%s' takes 0 to 255", #NAME);               \
            return -1;                                                                     \
        }                                                                                  \
        p->FIELD = (unsigned char)parsed;                                                  \
        return 0;                                                                          \
    }

#define QLX_GETSET_UINT(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                               \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return PyLong_FromUnsignedLong((unsigned long)p->FIELD);                           \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        if (qlx_check_assignable(value, #NAME)) {                                          \
            return -1;                                                                     \
        }                                                                                  \
        unsigned long v = PyLong_AsUnsignedLong(value);                                    \
        if (v == (unsigned long)-1 && PyErr_Occurred()) {                                  \
            return -1;                                                                     \
        }                                                                                  \
        if (v > UINT_MAX) {                                                                \
            PyErr_Format(PyExc_OverflowError, "'%s' does not fit in an unsigned int",      \
                         #NAME);                                                           \
            return -1;                                                                     \
        }                                                                                  \
        p->FIELD = (unsigned int)v;                                                        \
        return 0;                                                                          \
    }

// Steam IDs, which do not fit in anything narrower.
#define QLX_GETSET_U64(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                                \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return PyLong_FromUnsignedLongLong((unsigned long long)p->FIELD);                  \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        if (qlx_check_assignable(value, #NAME)) {                                          \
            return -1;                                                                     \
        }                                                                                  \
        unsigned long long v = PyLong_AsUnsignedLongLong(value);                           \
        if (v == (unsigned long long)-1 && PyErr_Occurred()) {                             \
            return -1;                                                                     \
        }                                                                                  \
        p->FIELD = (uint64_t)v;                                                            \
        return 0;                                                                          \
    }

/*
 * qboolean is a two-value enum, so this is the one kind that does not report the raw C
 * type. Writes take anything truthy.
 */
#define QLX_GETSET_BOOL(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                               \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return PyBool_FromLong((long)p->FIELD);                                            \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        return qlx_store_bool(value, &p->FIELD, #NAME);                                    \
    }

// Reuses the Vector3 struct sequence, so an origin here is the same type as the one
// player_state() reports. Writes take any three-element sequence of numbers.
#define QLX_GETSET_VEC3(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                               \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return PyMinqlxtended_Vector3(p->FIELD);                                           \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        return qlx_store_vec3(value, p->FIELD, #NAME);                                     \
    }

#define QLX_GETSET_CHARBUF(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                            \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return qlx_load_charbuf(p->FIELD, sizeof(p->FIELD));                               \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        return qlx_store_charbuf(value, p->FIELD, sizeof(p->FIELD), #NAME);                \
    }

/*
 * The getter hands back an IntArray view so elements can be assigned; the setter replaces
 * the whole array at exactly its length. The thunk lets the view re-derive without a pointer.
 */
#define QLX_GETSET_INTARR(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                             \
    static int* PREFIX##_arr_##NAME(PyObject* owner, Py_ssize_t* length) {                 \
        CTYPE* p = RESOLVE(owner);                                                         \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        *length = (Py_ssize_t)(sizeof(p->FIELD) / sizeof(p->FIELD[0]));                    \
        return p->FIELD;                                                                   \
    }                                                                                      \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return qlx_intarray_new(self, PREFIX##_arr_##NAME, #NAME);                         \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        return qlx_store_intarray(value, p->FIELD,                                         \
                                  (Py_ssize_t)(sizeof(p->FIELD) / sizeof(p->FIELD[0])),    \
                                  #NAME);                                                  \
    }

/*
 * Writes call the game module's own setter, which broadcasts the change. Storing to the
 * field would flip a lock with clients none the wiser.
 */
static int qlx_teamlock_write(Py_ssize_t index, int value, const char* name) {
    (void)name;
    if (!MP_LockOrUnlockTeam) {
        PyErr_SetString(qlx_EngineStateError,
                        "MP_LockOrUnlockTeam could not be resolved in this build, so teams "
                        "cannot be locked");
        return -1;
    }

    MP_LockOrUnlockTeam((int)index, value != 0);
    return 0;
}

// Read-only sub-object view. No setter: assigning a whole playerState_t or roundState_t
// from Python is a memcpy footgun with no legitimate use. Assign its members instead.
#define QLX_GET_SUBOBJ(PREFIX, TYPE, NAME)                                                 \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        return qlx_ref_new(&TYPE, ((qlx_ref_t*)self)->index);                              \
    }

#define QLX_GETSET_FLOAT(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                              \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return PyFloat_FromDouble((double)p->FIELD);                                       \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        if (qlx_check_assignable(value, #NAME)) {                                          \
            return -1;                                                                     \
        }                                                                                  \
        double v = PyFloat_AsDouble(value);                                                \
        if (v == -1.0 && PyErr_Occurred()) {                                               \
            return -1;                                                                     \
        }                                                                                  \
        p->FIELD = (float)v;                                                               \
        return 0;                                                                          \
    }

/*
 * A char* member, read-only: these point at strings the game module owns, mostly the map's
 * entity-string arena. Inline char[N] members are writable; see QLX_GETSET_CHARBUF.
 */
#define QLX_GETSET_CHARPTR(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                            \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        if (!p->FIELD) {                                                                   \
            Py_RETURN_NONE;                                                                \
        }                                                                                  \
        return PyUnicode_DecodeUTF8(p->FIELD, (Py_ssize_t)strlen(p->FIELD), "ignore");     \
    }

// A gentity_t* member. Hands back an Entity so ent.parent.classname works in one hop;
// Entity.number gives the index back when that is what was wanted.
#define QLX_GETSET_ENTREF(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                             \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return qlx_entity_from_ptr(p->FIELD);                                              \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        return qlx_store_entref(value, (gentity_t**)&p->FIELD, #NAME);                     \
    }

/*
 * A per-weapon int[16] as the Weapons struct sequence, the shape player_expanded_stats()
 * reports. Read-only, like the struct sequence.
 */
#define QLX_GETSET_WEAPONS(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                            \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        return PyMinqlxtended_Weapons(p->FIELD);                                           \
    }

// An array of gentity_t*, read-only. Writing one is niche enough to leave until asked.
#define QLX_GETSET_ENTREFARR(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                          \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        Py_ssize_t count = (Py_ssize_t)(sizeof(p->FIELD) / sizeof(p->FIELD[0]));           \
        PyObject* tuple  = PyTuple_New(count);                                             \
        if (!tuple) {                                                                      \
            return NULL;                                                                   \
        }                                                                                  \
        for (Py_ssize_t i = 0; i < count; i++) {                                           \
            PyObject* item = qlx_entity_from_ptr(p->FIELD[i]);                             \
            if (!item) {                                                                   \
                Py_DECREF(tuple);                                                          \
                return NULL;                                                               \
            }                                                                              \
            PyTuple_SET_ITEM(tuple, i, item);                                              \
        }                                                                                  \
        return tuple;                                                                      \
    }

/*
 * A callback member as a raw address, through a void* alias. A Python callable is refused:
 * it would be invoked from the game thread with no GIL acquisition, which deadlocks.
 */
#define QLX_GETSET_FNPTR(PREFIX, CTYPE, RESOLVE, FIELD, NAME)                              \
    static PyObject* PREFIX##_get_##NAME(PyObject* self, void* closure) {                  \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return NULL;                                                                   \
        }                                                                                  \
        void* fn = *(void**)&p->FIELD;                                                     \
        if (!fn) {                                                                         \
            Py_RETURN_NONE;                                                                \
        }                                                                                  \
        return PyLong_FromVoidPtr(fn);                                                     \
    }                                                                                      \
    static int PREFIX##_set_##NAME(PyObject* self, PyObject* value, void* closure) {       \
        (void)closure;                                                                     \
        CTYPE* p = RESOLVE(self);                                                          \
        if (!p) {                                                                          \
            return -1;                                                                     \
        }                                                                                  \
        if (qlx_check_assignable(value, #NAME)) {                                          \
            return -1;                                                                     \
        }                                                                                  \
        if (value == Py_None) {                                                            \
            *(void**)&p->FIELD = NULL;                                                     \
            return 0;                                                                      \
        }                                                                                  \
        if (PyCallable_Check(value)) {                                                     \
            PyErr_Format(PyExc_TypeError,                                                  \
                         "'%s' cannot be a Python callable; it is called from the game "   \
                         "thread with no GIL held. Assign an address read from another "   \
                         "entity, or 0 to clear it.",                                      \
                         #NAME);                                                           \
            return -1;                                                                     \
        }                                                                                  \
        void* fn = PyLong_AsVoidPtr(value);                                                \
        if (!fn && PyErr_Occurred()) {                                                     \
            return -1;                                                                     \
        }                                                                                  \
        *(void**)&p->FIELD = fn;                                                           \
        return 0;                                                                          \
    }

/*
 * QLX_SETTER yields NULL for the read-only kinds, which CPython turns into the right
 * AttributeError. QLX_DOC falls back to naming the C field.
 */
#define QLX_SETTER_INT(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_BOOL(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_VEC3(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_CHARBUF(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_INTARR(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_FLOAT(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_ENTREF(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_FNPTR(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_SCHAR(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_BYTE(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_UINT(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_U64(PREFIX, NAME) (setter)PREFIX##_set_##NAME
#define QLX_SETTER_WEAPONS(PREFIX, NAME) NULL
#define QLX_SETTER_CHARPTR(PREFIX, NAME) NULL
#define QLX_SETTER_ENTREFARR(PREFIX, NAME) NULL
#define QLX_SETTER_INT_RO(PREFIX, NAME) NULL
#define QLX_SETTER_UINT_RO(PREFIX, NAME) NULL
#define QLX_SETTER_BOOL_RO(PREFIX, NAME) NULL
#define QLX_SETTER(KIND, PREFIX, NAME) QLX_SETTER_##KIND(PREFIX, NAME)

#define QLX_DOC(CTYPE, FIELD, DOC) ((DOC) ? (DOC) : #CTYPE "." #FIELD)

/*
 * The KIND column is hand-written, so this checks it: CHARBUF on a char* memcpys over a
 * pointer the game module owns, and WEAPONS on an array under 16 ints reads past the end.
 */
#define QLX_FIELD_TYPE(CTYPE, FIELD) __typeof__(((CTYPE*)0)->FIELD)
#define QLX_FIELD_SIZE(CTYPE, FIELD) sizeof(((CTYPE*)0)->FIELD)
#define QLX_FIELD_IS(CTYPE, FIELD, T)                                                      \
    __builtin_types_compatible_p(QLX_FIELD_TYPE(CTYPE, FIELD), T)

#define QLX_KIND_OK_INT(CTYPE, FIELD) (QLX_FIELD_SIZE(CTYPE, FIELD) == sizeof(int))
/*
 * -1 goes positive only in an unsigned type, catching a signed member declared UINT. The
 * reverse test is no use for INT: gcc gives an enum with no negative enumerator an
 * unsigned underlying type.
 */
#define QLX_KIND_OK_UINT(CTYPE, FIELD)                                                     \
    (QLX_FIELD_SIZE(CTYPE, FIELD) == sizeof(unsigned int) &&                               \
     (QLX_FIELD_TYPE(CTYPE, FIELD)) - 1 > 0)
// The read-only kinds check the same thing their writable counterparts do.
#define QLX_KIND_OK_INT_RO(CTYPE, FIELD) QLX_KIND_OK_INT(CTYPE, FIELD)
#define QLX_KIND_OK_UINT_RO(CTYPE, FIELD) QLX_KIND_OK_UINT(CTYPE, FIELD)
#define QLX_KIND_OK_BOOL_RO(CTYPE, FIELD) QLX_KIND_OK_BOOL(CTYPE, FIELD)
#define QLX_KIND_OK_SCHAR(CTYPE, FIELD) (QLX_FIELD_SIZE(CTYPE, FIELD) == 1)
#define QLX_KIND_OK_BYTE(CTYPE, FIELD) (QLX_FIELD_SIZE(CTYPE, FIELD) == 1)
#define QLX_KIND_OK_U64(CTYPE, FIELD) (QLX_FIELD_SIZE(CTYPE, FIELD) == 8)
#define QLX_KIND_OK_BOOL(CTYPE, FIELD) QLX_FIELD_IS(CTYPE, FIELD, qboolean)
#define QLX_KIND_OK_FLOAT(CTYPE, FIELD) QLX_FIELD_IS(CTYPE, FIELD, float)
#define QLX_KIND_OK_VEC3(CTYPE, FIELD) QLX_FIELD_IS(CTYPE, FIELD, vec3_t)
#define QLX_KIND_OK_CHARBUF(CTYPE, FIELD)                                                  \
    QLX_FIELD_IS(CTYPE, FIELD, char[QLX_FIELD_SIZE(CTYPE, FIELD)])
#define QLX_KIND_OK_CHARPTR(CTYPE, FIELD)                                                  \
    (QLX_FIELD_IS(CTYPE, FIELD, char*) || QLX_FIELD_IS(CTYPE, FIELD, const char*))
#define QLX_KIND_OK_INTARR(CTYPE, FIELD)                                                   \
    QLX_FIELD_IS(CTYPE, FIELD, int[QLX_FIELD_SIZE(CTYPE, FIELD) / sizeof(int)])
#define QLX_KIND_OK_WEAPONS(CTYPE, FIELD) QLX_FIELD_IS(CTYPE, FIELD, int[MAX_WEAPONS])
#define QLX_KIND_OK_ENTREF(CTYPE, FIELD) QLX_FIELD_IS(CTYPE, FIELD, gentity_t*)
#define QLX_KIND_OK_ENTREFARR(CTYPE, FIELD)                                                \
    QLX_FIELD_IS(CTYPE, FIELD,                                                             \
                 gentity_t * [QLX_FIELD_SIZE(CTYPE, FIELD) / sizeof(gentity_t*)])
#define QLX_KIND_OK_FNPTR(CTYPE, FIELD) (QLX_FIELD_SIZE(CTYPE, FIELD) == sizeof(void*))

#define QLX_KIND_ASSERT(KIND, CTYPE, FIELD, NAME)                                          \
    _Static_assert(QLX_KIND_OK_##KIND(CTYPE, FIELD),                                       \
                   #CTYPE "." #FIELD " is not a " #KIND " field; " #NAME                   \
                          " would read it as one. Fix the KIND column in "                 \
                          "engine_fields.h.");

// Singleton plumbing

static void qlx_ref_dealloc(PyObject* self) {
    Py_TYPE(self)->tp_free(self);
}

// Shared by every index-backed type. Equality and hashing are on (type, index) so a set
// of entities behaves, and two Entity(5) objects compare equal without being identical.
static PyObject* qlx_ref_richcompare(PyObject* self, PyObject* other, int op) {
    if ((op != Py_EQ && op != Py_NE) || Py_TYPE(self) != Py_TYPE(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    int same = ((qlx_ref_t*)self)->index == ((qlx_ref_t*)other)->index;
    return PyBool_FromLong(op == Py_EQ ? same : !same);
}

static Py_hash_t qlx_ref_hash(PyObject* self) {
    Py_hash_t hash = (Py_hash_t)((qlx_ref_t*)self)->index;
    return hash == -1 ? -2 : hash;
}

/*
 * Neither of these dereferences anything, so neither can raise. tp_name carries the
 * qualified name, so one function covers every type that shares the layout.
 */
static PyObject* qlx_ref_repr(PyObject* self) {
    return PyUnicode_FromFormat("<%s %d>", Py_TYPE(self)->tp_name,
                                ((qlx_ref_t*)self)->index);
}

// For the singletons and the iterators, where index is -1 or absent and printing it would
// mean nothing.
static PyObject* qlx_singleton_repr(PyObject* self) {
    return PyUnicode_FromFormat("<%s>", Py_TYPE(self)->tp_name);
}

// RoundState

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(rst, roundState_t, qlx_roundstate, FIELD, NAME)
ROUNDSTATE_FIELDS(X)
#undef X

static PyGetSetDef qlx_roundstate_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)rst_get_##NAME, QLX_SETTER(KIND, rst, NAME),                           \
     QLX_DOC(roundState_t, FIELD, DOC), NULL},
    ROUNDSTATE_FIELDS(X)
#undef X
        {NULL}};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(roundState_t, FIELD) == (OFF),                                 \
                   "roundState_t." #FIELD " moved; minqlxtended.level.round." #NAME        \
                   " reads it");                                                           \
    QLX_KIND_ASSERT(KIND, roundState_t, FIELD, NAME)
ROUNDSTATE_FIELDS(X)
#undef X

static PyTypeObject qlx_roundstate_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.RoundStateView",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_singleton_repr,
    .tp_getset                             = qlx_roundstate_getset,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc   = "The round state of a round-based gametype, as minqlxtended.level.round.\n\n"
                "Game thread only: these read and write live engine memory with no lock. "
                "Marshal with minqlxtended.next_frame if you are on a worker thread.",
};

// Level

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(lvl, level_locals_t, qlx_level, FIELD, NAME)
LEVEL_FIELDS(X)
#undef X

QLX_GET_SUBOBJ(lvl, qlx_roundstate_type, round)

static PyGetSetDef qlx_level_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)lvl_get_##NAME, QLX_SETTER(KIND, lvl, NAME),                           \
     QLX_DOC(level_locals_t, FIELD, DOC), NULL},
    LEVEL_FIELDS(X)
#undef X

        {"round", (getter)lvl_get_round, NULL,
         "The roundState_t sub-object, for round-based gametypes.", NULL},

        /*
         * Four short aliases, sharing the accessors above. The list is closed; new fields
         * get their mechanical name and nothing else.
         */
        {"num_playing", (getter)lvl_get_num_playing_clients,
         (setter)lvl_set_num_playing_clients, "Alias for num_playing_clients.", NULL},
        {"vote_caller", (getter)lvl_get_pending_vote_caller,
         (setter)lvl_set_pending_vote_caller, "Alias for pending_vote_caller.", NULL},
        {"pause_begin", (getter)lvl_get_time_pause_begin, (setter)lvl_set_time_pause_begin,
         "Alias for time_pause_begin.", NULL},
        {"round_state", (getter)rst_get_current, (setter)rst_set_current,
         "Alias for level.round.current.", NULL},
        {NULL}};

/*
 * A regression guard only: the offsets come from the header this checks. quake_common.h's
 * disassembly-derived assertions are what pin level_locals_t to the game module.
 */
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(level_locals_t, FIELD) == (OFF),                               \
                   "level_locals_t." #FIELD " moved; minqlxtended.level." #NAME            \
                   " reads it. Re-run tools/gen_field_offsets.py.");                       \
    QLX_KIND_ASSERT(KIND, level_locals_t, FIELD, NAME)
LEVEL_FIELDS(X)
#undef X

static PyTypeObject qlx_level_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.Level",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_singleton_repr,
    .tp_getset                             = qlx_level_getset,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc =
        "The game module's level state, as the singleton minqlxtended.level.\n\n"
        "Reading an attribute dereferences the engine's own level_locals_t, and assigning "
        "to one writes straight through. It's not a snapshot. Attributes are the C field "
        "names in snake_case, and each one's docstring names the field it came from.\n\n"
        "Game thread only. Don't read from or write to minqlxtended.level from a function "
        "running in a non-game thread (i.e. anything decorated with minqlxtended.thread), "
        "or risk data corruption and likely server crashes. Do the heavy lifting in a "
        "worker, then use minqlxtended.next_frame for the part that touches engine "
        "state.\n\n"
        "Almost everything is writable. Writing a value does whatever the game module does "
        "with that value set.\n\n"
        "Raises minqlxtended.EngineStateError when there's no level, mostly before the "
        "first map loads.",
};

// MatchState
// The game module's match-play globals, as the singleton minqlxtended.match_state. Six
// separate globals outside level_locals_t, so they sit beside Level rather than inside it.

/*
 * Hand-written: there is no struct to generate from, and QLX_GETSET_INT and friends are
 * parameterised on a struct type and expand to p->FIELD. Each accessor reads its own global.
 */
#define QLX_MPS_GETSET_INT(NAME, GLOBAL)                                                   \
    static PyObject* mps_get_##NAME(PyObject* self, void* closure) {                       \
        (void)self;                                                                        \
        (void)closure;                                                                     \
        if (!qlx_matchstate_ready()) {                                                     \
            return NULL;                                                                   \
        }                                                                                  \
        return PyLong_FromLong((long)*GLOBAL);                                             \
    }                                                                                      \
    static int mps_set_##NAME(PyObject* self, PyObject* value, void* closure) {            \
        (void)self;                                                                        \
        (void)closure;                                                                     \
        if (!qlx_matchstate_ready()) {                                                     \
            return -1;                                                                     \
        }                                                                                  \
        int parsed;                                                                        \
        if (qlx_store_int(value, &parsed, #NAME)) {                                        \
            return -1;                                                                     \
        }                                                                                  \
        *GLOBAL = parsed;                                                                  \
        return 0;                                                                          \
    }

QLX_MPS_GETSET_INT(unpause_time, mp_unpauseTime)
QLX_MPS_GETSET_INT(pause_caller, mp_pauseCaller)
QLX_MPS_GETSET_INT(auto_action_state, mp_autoActionState)

// qboolean rather than int, so it reads back as True/False and refuses anything else.
static PyObject* mps_get_paused_by_server(PyObject* self, void* closure) {
    (void)self;
    (void)closure;
    if (!qlx_matchstate_ready()) {
        return NULL;
    }

    return PyBool_FromLong((long)*mp_pausedByServer);
}

static int mps_set_paused_by_server(PyObject* self, PyObject* value, void* closure) {
    (void)self;
    (void)closure;
    if (!qlx_matchstate_ready()) {
        return -1;
    }

    return qlx_store_bool(value, mp_pausedByServer, "paused_by_server");
}

/*
 * The two team-indexed arrays. Length comes from TEAM_NUM_TEAMS rather than a sizeof,
 * since these are `int foo[4]` in the game module and the pointer held is to element 0.
 */
static int* mps_arr_timeouts_used(PyObject* owner, Py_ssize_t* length) {
    (void)owner;
    if (!qlx_matchstate_ready()) {
        return NULL;
    }

    *length = TEAM_NUM_TEAMS;
    return mp_timeoutsUsed;
}

static PyObject* mps_get_timeouts_used(PyObject* self, void* closure) {
    (void)closure;
    if (!qlx_matchstate_ready()) {
        return NULL;
    }

    return qlx_intarray_new(self, mps_arr_timeouts_used, "timeouts_used");
}

static int mps_set_timeouts_used(PyObject* self, PyObject* value, void* closure) {
    (void)self;
    (void)closure;
    if (!qlx_matchstate_ready()) {
        return -1;
    }

    return qlx_store_intarray(value, mp_timeoutsUsed, TEAM_NUM_TEAMS, "timeouts_used");
}

static int* mps_arr_team_locked(PyObject* owner, Py_ssize_t* length) {
    (void)owner;
    if (!qlx_matchstate_ready()) {
        return NULL;
    }

    *length = TEAM_NUM_TEAMS;
    return mp_teamLocked;
}

/*
 * The one array whose writes do not store. Both setters go through qlx_teamlock_write, so
 * each change reaches the game module's own setter and is announced.
 */
static PyObject* mps_get_team_locked(PyObject* self, void* closure) {
    (void)closure;
    if (!qlx_matchstate_ready()) {
        return NULL;
    }

    return qlx_intarray_new_hooked(self, mps_arr_team_locked, qlx_teamlock_write,
                                   "team_locked");
}

static int mps_set_team_locked(PyObject* self, PyObject* value, void* closure) {
    (void)self;
    (void)closure;
    if (!qlx_matchstate_ready()) {
        return -1;
    }

    PyObject* items = PySequence_Fast(value, "expected a sequence");
    if (!items) {
        return -1;
    }

    if (PySequence_Fast_GET_SIZE(items) != TEAM_NUM_TEAMS) {
        PyErr_Format(PyExc_ValueError, "'team_locked' needs exactly %d values",
                     TEAM_NUM_TEAMS);
        Py_DECREF(items);
        return -1;
    }

    for (Py_ssize_t i = 0; i < TEAM_NUM_TEAMS; i++) {
        int wanted = 0;
        if (qlx_store_int(PySequence_Fast_GET_ITEM(items, i), &wanted, "team_locked") < 0 ||
            qlx_teamlock_write(i, wanted, "team_locked") < 0) {
            Py_DECREF(items);
            return -1;
        }
    }

    Py_DECREF(items);
    return 0;
}

static PyGetSetDef qlx_matchstate_getset[] = {
    {"unpause_time", (getter)mps_get_unpause_time, (setter)mps_set_unpause_time,
     "Level time the running timeout ends. 0 during an indefinite pause, and while "
     "unpaused.",
     NULL},
    {"pause_caller", (getter)mps_get_pause_caller, (setter)mps_set_pause_caller,
     "Client id that called the current pause. 0 when the server called it, so read "
     "paused_by_server first.",
     NULL},
    {"paused_by_server", (getter)mps_get_paused_by_server, (setter)mps_set_paused_by_server,
     "Whether the server called the current pause, rather than a player.", NULL},
    {"timeouts_used", (getter)mps_get_timeouts_used, (setter)mps_set_timeouts_used,
     "Timeouts each team has spent, indexed by Team.index. Only the red and blue slots are "
     "ever written.",
     NULL},
    {"team_locked", (getter)mps_get_team_locked, (setter)mps_set_team_locked,
     "Whether each team is locked against joining, indexed by Team.index. Only red and blue "
     "actually gate joining. Assigning calls the game module's setter, so clients are told.",
     NULL},
    {"auto_action_state", (getter)mps_get_auto_action_state,
     (setter)mps_set_auto_action_state,
     "The game module's autorecord/autoscreenshot state machine.", NULL},
    {NULL}};

static PyTypeObject qlx_matchstate_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.MatchState",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_singleton_repr,
    .tp_getset                             = qlx_matchstate_getset,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc =
        "The game module's match-play state, as the singleton minqlxtended.match_state.\n\n"
        "Pauses, timeouts and the per-team join locks. These live in the game module's own "
        "globals. minqlxtended.level reads level_locals_t, which holds none of them.\n\n"
        "None of it is cleared between maps: a lock and a running timeout both outlive a "
        "map change, so do not assume a fresh map means an unlocked one.\n\n"
        "team_locked is the exception to writing straight through. Assigning to it calls "
        "the game module's own setter, so clients are told the same way the lock console "
        "command tells them.\n\n"
        "Game thread only, as with minqlxtended.level.\n\n"
        "Raises minqlxtended.EngineStateError if it could not be resolved in this build.",
};

// Server and ServerStatic: the other two singletons. Same shape as Level: no index, one global behind each.

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(srv, server_t, qlx_server, FIELD, NAME)
SERVER_FIELDS(X)
#undef X

static PyGetSetDef qlx_server_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)srv_get_##NAME, QLX_SETTER(KIND, srv, NAME),                           \
     QLX_DOC(server_t, FIELD, DOC), NULL},
    SERVER_FIELDS(X)
#undef X
        {NULL}};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(server_t, FIELD) == (OFF),                                     \
                   "server_t." #FIELD " moved; minqlxtended.server." #NAME                 \
                   " reads it. Re-run tools/gen_field_offsets.py.");                       \
    QLX_KIND_ASSERT(KIND, server_t, FIELD, NAME)
SERVER_FIELDS(X)
#undef X

static PyTypeObject qlx_server_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.Server",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_singleton_repr,
    .tp_getset                             = qlx_server_getset,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc =
        "The dedicated server's own state for the loaded map, as the singleton "
        "minqlxtended.server.\n\n"
        "This is the server's view; minqlxtended.level is the game module's. Everything "
        "here is wiped and rebuilt by a map load.\n\n"
        "Game thread only, and writable, on the same terms as minqlxtended.level.\n\n"
        "Raises minqlxtended.EngineStateError if sv could not be resolved.",
};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(svs, serverStatic_t, qlx_serverstatic, FIELD, NAME)
SERVERSTATIC_FIELDS(X)
#undef X

static PyGetSetDef qlx_serverstatic_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)svs_get_##NAME, QLX_SETTER(KIND, svs, NAME),                           \
     QLX_DOC(serverStatic_t, FIELD, DOC), NULL},
    SERVERSTATIC_FIELDS(X)
#undef X
        {NULL}};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(serverStatic_t, FIELD) == (OFF),                               \
                   "serverStatic_t." #FIELD " moved; minqlxtended.server_static." #NAME    \
                   " reads it. Re-run tools/gen_field_offsets.py.");                       \
    QLX_KIND_ASSERT(KIND, serverStatic_t, FIELD, NAME)
SERVERSTATIC_FIELDS(X)
#undef X

static PyTypeObject qlx_serverstatic_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.ServerStatic",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_singleton_repr,
    .tp_getset                             = qlx_serverstatic_getset,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc   = "The parts of the server that survive a map change, as the singleton "
                "minqlxtended.server_static.\n\n"
                "server_static.time is the one clock that keeps counting across a map "
                "load; level.time restarts.\n\n"
                "Game thread only, and writable, on the same terms as minqlxtended.level.",
};

// EntityState and EntityShared

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(ents, entityState_t, qlx_entstate, FIELD, NAME)
ENTITYSTATE_FIELDS(X)
#undef X

static PyGetSetDef qlx_entitystate_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)ents_get_##NAME, QLX_SETTER(KIND, ents, NAME),                         \
     QLX_DOC(entityState_t, FIELD, DOC), NULL},
    ENTITYSTATE_FIELDS(X)
#undef X
        {NULL}};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(entityState_t, FIELD) == (OFF),                                \
                   "entityState_t." #FIELD " moved; Entity.s." #NAME                       \
                   " reads it. Re-run tools/gen_field_offsets.py.");                       \
    QLX_KIND_ASSERT(KIND, entityState_t, FIELD, NAME)
ENTITYSTATE_FIELDS(X)
#undef X

static PyTypeObject qlx_entitystate_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.EntityState",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_ref_repr,
    .tp_getset                             = qlx_entitystate_getset,
    .tp_richcompare                        = qlx_ref_richcompare,
    .tp_hash                               = qlx_ref_hash,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc   = "The networked part of an entity, as Entity(n).s.\n\n"
                "This is what the server sends to clients, so it is a snapshot the engine "
                "maintains, rather than the entity's own working state. r.current_origin is where "
                "the entity is; s.origin is where clients were last told it was.",
};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(entr, entityShared_t, qlx_entshared, FIELD, NAME)
ENTITYSHARED_FIELDS(X)
#undef X

static PyGetSetDef qlx_entityshared_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)entr_get_##NAME, QLX_SETTER(KIND, entr, NAME),                         \
     QLX_DOC(entityShared_t, FIELD, DOC), NULL},
    ENTITYSHARED_FIELDS(X)
#undef X
        {NULL}};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(entityShared_t, FIELD) == (OFF),                               \
                   "entityShared_t." #FIELD " moved; Entity.r." #NAME                      \
                   " reads it. Re-run tools/gen_field_offsets.py.");                       \
    QLX_KIND_ASSERT(KIND, entityShared_t, FIELD, NAME)
ENTITYSHARED_FIELDS(X)
#undef X

static PyTypeObject qlx_entityshared_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.EntityShared",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_ref_repr,
    .tp_getset                             = qlx_entityshared_getset,
    .tp_richcompare                        = qlx_ref_richcompare,
    .tp_hash                               = qlx_ref_hash,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc   = "The part of an entity both the server and the game module see, as "
                "Entity(n).r.\n\n"
                "sv_flags is the useful one: SVF_BOT is how the engine itself tells a bot "
                "from a human.",
};

// Entity

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(ent, gentity_t, qlx_entity, FIELD, NAME)
ENTITY_FIELDS(X)
#undef X

QLX_GET_SUBOBJ(ent, qlx_entitystate_type, s)
QLX_GET_SUBOBJ(ent, qlx_entityshared_type, r)

// Defined with the GameClient type, since it needs it; declared here for the table below.
static PyObject* ent_get_client(PyObject* self, void* closure);

static PyObject* ent_get_number(PyObject* self, void* closure) {
    (void)closure;
    // Does not resolve: the number is what the object *is*, so asking for it works even
    // when the game module is not loaded.
    return PyLong_FromLong((long)((qlx_ref_t*)self)->index);
}

static PyGetSetDef qlx_entity_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)ent_get_##NAME, QLX_SETTER(KIND, ent, NAME),                           \
     QLX_DOC(gentity_t, FIELD, DOC), NULL},
    ENTITY_FIELDS(X)
#undef X

        {"number", (getter)ent_get_number, NULL, "This entity's index into g_entities.",
         NULL},
        {"s", (getter)ent_get_s, NULL, "The networked entityState_t.", NULL},
        {"r", (getter)ent_get_r, NULL, "The shared entityShared_t.", NULL},
        {"client", (getter)ent_get_client, NULL,
         "The GameClient for a player entity, or None.", NULL},
        {NULL}};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(gentity_t, FIELD) == (OFF),                                    \
                   "gentity_t." #FIELD " moved; Entity." #NAME                             \
                   " reads it. Re-run tools/gen_field_offsets.py.");                       \
    QLX_KIND_ASSERT(KIND, gentity_t, FIELD, NAME)
ENTITY_FIELDS(X)
#undef X

static PyObject* qlx_entity_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    static char* kwlist[] = {"number", NULL};
    int number;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i:Entity", kwlist, &number)) {
        return NULL;
    }

    /*
     * Bounded by MAX_GENTITIES. level->num_entities is a high-water mark that moves as the
     * map runs, so bounding against it would make Entity() and entities() disagree.
     */
    if (number < 0 || number >= MAX_GENTITIES) {
        PyErr_Format(PyExc_ValueError, "entity number %d is out of range (0-%d)", number,
                     MAX_GENTITIES - 1);
        return NULL;
    }

    return qlx_ref_new(type, number);
}

static PyObject* qlx_entity_repr(PyObject* self) {
    int number      = ((qlx_ref_t*)self)->index;
    gentity_t* ent  = qlx_entity(self);

    if (!ent) {
        // A __repr__ should never raise.
        PyErr_Clear();
        return PyUnicode_FromFormat("<Entity %d (engine not ready)>", number);
    }

    if (!ent->inuse) {
        return PyUnicode_FromFormat("<Entity %d (free)>", number);
    }

    return PyUnicode_FromFormat("<Entity %d '%s'>", number,
                                ent->classname ? ent->classname : "");
}

static PyTypeObject qlx_entity_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.Entity",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_entity_repr,
    .tp_getset                             = qlx_entity_getset,
    .tp_richcompare                        = qlx_ref_richcompare,
    .tp_hash                               = qlx_ref_hash,
    .tp_new                                = qlx_entity_new,
    .tp_flags                              = Py_TPFLAGS_DEFAULT,
    .tp_doc =
        "Entity(number) -- a live view onto one of the game module's entities.\n\n"
        "Reading an attribute dereferences g_entities[number] there and then, and "
        "assigning to one writes straight through. The networked state is on .s and the "
        "server-shared part on .r.\n\n"
        "It holds the number rather than a pointer, so it stays safe to keep: the "
        "game module is remapped on every map load. The flip side is that identity is "
        "not stable. An Entity held across a map change describes whatever occupies "
        "that slot on the new map. Compare .classname or re-derive if that matters.\n\n"
        "Reading most fields off an entity whose .inuse is False is meaningless, and "
        ".classname in particular is a stale pointer the engine never cleared. "
        "entities() filters those out by default.\n\n"
        "Game thread only: nothing here takes a lock. Marshal with "
        "minqlxtended.next_frame if you are on a worker thread.",
};

// entities()

typedef struct {
    PyObject_HEAD
    int next;
    int stop;
    int inuse_only;
    int etype;          // -1 for no filter
    char classname[64]; // empty for no filter
} qlx_entityiter_t;

static PyObject* qlx_entityiter_next(PyObject* self) {
    qlx_entityiter_t* it = (qlx_entityiter_t*)self;

    if (!g_entities) {
        PyErr_SetString(qlx_EngineStateError,
                        "g_entities is not available; the game module is not loaded");
        return NULL;
    }

    while (it->next < it->stop) {
        gentity_t* ent = &g_entities[it->next++];

        // All the filters run in C, so a slot rejected here costs no Entity.
        if (it->inuse_only && !ent->inuse) {
            continue;
        }
        if (it->etype >= 0 && ent->s.eType != it->etype) {
            continue;
        }
        // The inuse test here stays even with inuse=False: a freed slot's classname is
        // a stale pointer the engine never cleared.
        if (it->classname[0] &&
            (!ent->inuse || !ent->classname || strcasecmp(ent->classname, it->classname))) {
            continue;
        }

        return qlx_ref_new(&qlx_entity_type, it->next - 1);
    }

    return NULL; // StopIteration
}

static PyTypeObject qlx_entityiter_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.EntityIterator",
    .tp_basicsize                          = sizeof(qlx_entityiter_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_singleton_repr,
    .tp_iter                               = PyObject_SelfIter,
    .tp_iternext                           = qlx_entityiter_next,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc   = "Lazy iterator over g_entities, from minqlxtended.entities().",
};

PyObject* PyMinqlxtended_Entities(PyObject* self, PyObject* args, PyObject* kwds) {
    (void)self;
    static char* kwlist[] = {"inuse", "etype", "start", "stop", "classname", NULL};
    int inuse             = 1;
    PyObject* etype       = Py_None;
    int start             = 0;
    int stop              = MAX_GENTITIES;
    const char* classname = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|pOiiz:entities", kwlist, &inuse, &etype,
                                     &start, &stop, &classname)) {
        return NULL;
    }

    int etype_filter = -1;
    if (etype != Py_None) {
        long v = PyLong_AsLong(etype);
        if (v == -1 && PyErr_Occurred()) {
            return NULL;
        }
        if (v < 0) {
            PyErr_SetString(PyExc_ValueError, "etype must be a non-negative ET_* value");
            return NULL;
        }
        etype_filter = (int)v;
    }

    if (start < 0) {
        start = 0;
    }
    if (stop > MAX_GENTITIES) {
        stop = MAX_GENTITIES;
    }

    qlx_entityiter_t* it =
        (qlx_entityiter_t*)qlx_entityiter_type.tp_alloc(&qlx_entityiter_type, 0);
    if (!it) {
        return NULL;
    }

    it->next       = start;
    it->stop       = stop;
    it->inuse_only = inuse ? 1 : 0;
    it->etype      = etype_filter;
    it->classname[0] = '\0';
    if (classname != NULL) {
        // Truncation is fine: no real classname reaches 63 characters, and a truncated
        // filter matches nothing rather than the wrong thing.
        strncpy(it->classname, classname, sizeof(it->classname) - 1);
        it->classname[sizeof(it->classname) - 1] = '\0';
    }
    return (PyObject*)it;
}

// GameClient and its sub-views
// Six views onto parts of gclient_t, plus the client itself. All index-backed by the client
// number, so each re-derives through g_entities[n].client on every access.

// Emits the accessors, table, offset asserts and type object for a sub-view. The seven of
// them are identical bar the names.
#define QLX_DEFINE_VIEW(PREFIX, CTYPE, RESOLVE, FIELDS, TYPEVAR, PYNAME, EXTRA, DOCSTRING) \
    FIELDS(QLX_VIEW_ACCESSOR_##PREFIX)                                                     \
    static PyGetSetDef TYPEVAR##_getset[] = {FIELDS(QLX_VIEW_ROW_##PREFIX) EXTRA{NULL}};   \
    FIELDS(QLX_VIEW_ASSERT_##PREFIX)                                                       \
    static PyTypeObject TYPEVAR = {                                                        \
        PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended." PYNAME,                   \
        .tp_basicsize                          = sizeof(qlx_ref_t),                        \
        .tp_dealloc                            = qlx_ref_dealloc,                          \
        .tp_repr                               = qlx_ref_repr,                             \
        .tp_getset                             = TYPEVAR##_getset,                         \
        .tp_richcompare                        = qlx_ref_richcompare,                      \
        .tp_hash                               = qlx_ref_hash,                             \
        .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,                \
        .tp_doc   = DOCSTRING};

// One trio of helper macros per view, since the X callback takes no extra arguments.
#define QLX_VIEW_ACCESSOR_ps(KIND, FIELD, NAME, OFF, DOC)                                  \
    QLX_GETSET_##KIND(ps, playerState_t, qlx_playerstate, FIELD, NAME)
#define QLX_VIEW_ROW_ps(KIND, FIELD, NAME, OFF, DOC)                                       \
    {#NAME, (getter)ps_get_##NAME, QLX_SETTER(KIND, ps, NAME),                             \
     QLX_DOC(playerState_t, FIELD, DOC), NULL},
#define QLX_VIEW_ASSERT_ps(KIND, FIELD, NAME, OFF, DOC)                                    \
    _Static_assert(offsetof(playerState_t, FIELD) == (OFF),                                \
                   "playerState_t." #FIELD " moved; GameClient.ps." #NAME " reads it.");   \
    QLX_KIND_ASSERT(KIND, playerState_t, FIELD, NAME)

#define QLX_VIEW_ACCESSOR_pers(KIND, FIELD, NAME, OFF, DOC)                                \
    QLX_GETSET_##KIND(pers, clientPersistant_t, qlx_pers, FIELD, NAME)
#define QLX_VIEW_ROW_pers(KIND, FIELD, NAME, OFF, DOC)                                     \
    {#NAME, (getter)pers_get_##NAME, QLX_SETTER(KIND, pers, NAME),                         \
     QLX_DOC(clientPersistant_t, FIELD, DOC), NULL},
#define QLX_VIEW_ASSERT_pers(KIND, FIELD, NAME, OFF, DOC)                                  \
    _Static_assert(offsetof(clientPersistant_t, FIELD) == (OFF),                           \
                   "clientPersistant_t." #FIELD " moved; GameClient.pers." #NAME           \
                   " reads it.");                                                          \
    QLX_KIND_ASSERT(KIND, clientPersistant_t, FIELD, NAME)

#define QLX_VIEW_ACCESSOR_sess(KIND, FIELD, NAME, OFF, DOC)                                \
    QLX_GETSET_##KIND(sess, clientSession_t, qlx_sess, FIELD, NAME)
#define QLX_VIEW_ROW_sess(KIND, FIELD, NAME, OFF, DOC)                                     \
    {#NAME, (getter)sess_get_##NAME, QLX_SETTER(KIND, sess, NAME),                         \
     QLX_DOC(clientSession_t, FIELD, DOC), NULL},
#define QLX_VIEW_ASSERT_sess(KIND, FIELD, NAME, OFF, DOC)                                  \
    _Static_assert(offsetof(clientSession_t, FIELD) == (OFF),                              \
                   "clientSession_t." #FIELD " moved; GameClient.sess." #NAME " reads it.");\
    QLX_KIND_ASSERT(KIND, clientSession_t, FIELD, NAME)

#define QLX_VIEW_ACCESSOR_ts(KIND, FIELD, NAME, OFF, DOC)                                  \
    QLX_GETSET_##KIND(ts, playerTeamState_t, qlx_teamstate, FIELD, NAME)
#define QLX_VIEW_ROW_ts(KIND, FIELD, NAME, OFF, DOC)                                       \
    {#NAME, (getter)ts_get_##NAME, QLX_SETTER(KIND, ts, NAME),                             \
     QLX_DOC(playerTeamState_t, FIELD, DOC), NULL},
#define QLX_VIEW_ASSERT_ts(KIND, FIELD, NAME, OFF, DOC)                                    \
    _Static_assert(offsetof(playerTeamState_t, FIELD) == (OFF),                            \
                   "playerTeamState_t." #FIELD " moved; pers.team_state." #NAME            \
                   " reads it.");                                                          \
    QLX_KIND_ASSERT(KIND, playerTeamState_t, FIELD, NAME)

#define QLX_VIEW_ACCESSOR_race(KIND, FIELD, NAME, OFF, DOC)                                \
    QLX_GETSET_##KIND(race, raceInfo_t, qlx_race, FIELD, NAME)
#define QLX_VIEW_ROW_race(KIND, FIELD, NAME, OFF, DOC)                                     \
    {#NAME, (getter)race_get_##NAME, QLX_SETTER(KIND, race, NAME),                         \
     QLX_DOC(raceInfo_t, FIELD, DOC), NULL},
#define QLX_VIEW_ASSERT_race(KIND, FIELD, NAME, OFF, DOC)                                  \
    _Static_assert(offsetof(raceInfo_t, FIELD) == (OFF),                                   \
                   "raceInfo_t." #FIELD " moved; GameClient.race." #NAME " reads it.");    \
    QLX_KIND_ASSERT(KIND, raceInfo_t, FIELD, NAME)

#define QLX_VIEW_ACCESSOR_xs(KIND, FIELD, NAME, OFF, DOC)                                  \
    QLX_GETSET_##KIND(xs, expandedStatObj_t, qlx_xstats, FIELD, NAME)
#define QLX_VIEW_ROW_xs(KIND, FIELD, NAME, OFF, DOC)                                       \
    {#NAME, (getter)xs_get_##NAME, QLX_SETTER(KIND, xs, NAME),                             \
     QLX_DOC(expandedStatObj_t, FIELD, DOC), NULL},
#define QLX_VIEW_ASSERT_xs(KIND, FIELD, NAME, OFF, DOC)                                    \
    _Static_assert(offsetof(expandedStatObj_t, FIELD) == (OFF),                            \
                   "expandedStatObj_t." #FIELD " moved; expanded_stats." #NAME             \
                   " reads it.");                                                          \
    QLX_KIND_ASSERT(KIND, expandedStatObj_t, FIELD, NAME)

// The sub-object rows the generated tables cannot produce, since the type a field maps to
// is not derivable from its name. Empty for most views.
#define QLX_NO_EXTRA_ROWS
#define QLX_PERS_EXTRA_ROWS                                                                \
    {"team_state", (getter)pers_get_team_state, NULL,                                      \
     "Objective statistics: captures, defends, flag returns.", NULL},

QLX_DEFINE_VIEW(ps, playerState_t, qlx_playerstate, PLAYERSTATE_FIELDS, qlx_playerstate_type,
                "PlayerStateView", QLX_NO_EXTRA_ROWS,
                "A player's playerState_t, as GameClient(n).ps.\n\n"
                "The live movement and aim state: viewangles is where they are looking, "
                "pm_flags carries ducked/frozen/following, and stats, persistant, powerups "
                "and ammo are the indexed arrays behind most of the older API.")

QLX_DEFINE_VIEW(ts, playerTeamState_t, qlx_teamstate, TEAMSTATE_FIELDS, qlx_teamstate_type,
                "TeamState", QLX_NO_EXTRA_ROWS,
                "Objective statistics for one player, as GameClient(n).pers.team_state.\n\n"
                "Captures, base defense, flag returns and the rest of what a CTF or "
                "Domination scoreboard reports.")

// Defined between the two views: it hands back a TeamState, which the view above has just
// declared, and the view below puts it in its table.
QLX_GET_SUBOBJ(pers, qlx_teamstate_type, team_state)

QLX_DEFINE_VIEW(pers, clientPersistant_t, qlx_pers, PERSISTANT_FIELDS, qlx_persistant_type,
                "Persistant", QLX_PERS_EXTRA_ROWS,
                "What survives a respawn, as GameClient(n).pers.\n\n"
                "The player's name, country and Steam ID, their warmup ready state, and "
                "their last usercmd flattened onto cmd_* - which is live button and "
                "movement input, updated every time the client sends one.")

QLX_DEFINE_VIEW(sess, clientSession_t, qlx_sess, SESSION_FIELDS, qlx_session_type, "Session",
                QLX_NO_EXTRA_ROWS,
                "What survives a map change, as GameClient(n).sess.\n\n"
                "Team, privileges, wins and losses, mute state, play-queue position, and "
                "who a spectator is following.")

QLX_DEFINE_VIEW(race, raceInfo_t, qlx_race, RACEINFO_FIELDS, qlx_raceinfo_type, "RaceInfo",
                QLX_NO_EXTRA_ROWS,
                "Race gametype state, as GameClient(n).race.\n\n"
                "best_race and current_race are per-checkpoint split times.")

QLX_DEFINE_VIEW(xs, expandedStatObj_t, qlx_xstats, EXPANDEDSTATS_FIELDS,
                qlx_expandedstats_type, "ExpandedStats", QLX_NO_EXTRA_ROWS,
                "The match statistics the scoreboard is built from, as "
                "GameClient(n).expanded_stats.\n\n"
                "The per-weapon arrays come back as a Weapons struct sequence, the same "
                "shape player_expanded_stats() reports them in.")

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(gc, gclient_t, qlx_gclient, FIELD, NAME)
GAMECLIENT_FIELDS(X)
#undef X

QLX_GET_SUBOBJ(gc, qlx_playerstate_type, ps)
QLX_GET_SUBOBJ(gc, qlx_persistant_type, pers)
QLX_GET_SUBOBJ(gc, qlx_session_type, sess)
QLX_GET_SUBOBJ(gc, qlx_raceinfo_type, race)
QLX_GET_SUBOBJ(gc, qlx_expandedstats_type, expanded_stats)

static PyObject* gc_get_id(PyObject* self, void* closure) {
    (void)closure;
    return PyLong_FromLong((long)((qlx_ref_t*)self)->index);
}

static PyGetSetDef qlx_gameclient_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)gc_get_##NAME, QLX_SETTER(KIND, gc, NAME),                             \
     QLX_DOC(gclient_t, FIELD, DOC), NULL},
    GAMECLIENT_FIELDS(X)
#undef X

        {"id", (getter)gc_get_id, NULL, "This client's id.", NULL},
        {"ps", (getter)gc_get_ps, NULL, "The playerState_t.", NULL},
        {"pers", (getter)gc_get_pers, NULL, "What survives a respawn.", NULL},
        {"sess", (getter)gc_get_sess, NULL, "What survives a map change.", NULL},
        {"race", (getter)gc_get_race, NULL, "Race gametype state.", NULL},
        {"expanded_stats", (getter)gc_get_expanded_stats, NULL, "Match statistics.", NULL},
        {NULL}};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(gclient_t, FIELD) == (OFF),                                    \
                   "gclient_t." #FIELD " moved; GameClient." #NAME                         \
                   " reads it. Re-run tools/gen_field_offsets.py.");                       \
    QLX_KIND_ASSERT(KIND, gclient_t, FIELD, NAME)
GAMECLIENT_FIELDS(X)
#undef X

static PyObject* qlx_gameclient_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    static char* kwlist[] = {"client_id", NULL};
    int client_id;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i:GameClient", kwlist, &client_id)) {
        return NULL;
    }

    // Bounded against MAX_CLIENTS, so constructing one works before sv_maxclients is
    // resolved. Accessing it before then raises.
    if (client_id < 0 || client_id >= MAX_CLIENTS) {
        PyErr_Format(PyExc_ValueError, "client id %d is out of range (0-%d)", client_id,
                     MAX_CLIENTS - 1);
        return NULL;
    }

    return qlx_ref_new(type, client_id);
}

static PyObject* qlx_gameclient_repr(PyObject* self) {
    int client_id     = ((qlx_ref_t*)self)->index;
    gclient_t* client = qlx_gclient(self);

    if (!client) {
        PyErr_Clear();
        return PyUnicode_FromFormat("<GameClient %d (empty)>", client_id);
    }

    // Through qlx_load_charbuf, since netname is a fixed char[] the engine does not always
    // terminate and PyUnicode_FromFormat's %s is strlen-based.
    PyObject* name = qlx_load_charbuf(client->pers.netname, sizeof(client->pers.netname));
    if (!name) {
        return NULL;
    }

    PyObject* result = PyUnicode_FromFormat("<GameClient %d '%U'>", client_id, name);
    Py_DECREF(name);
    return result;
}

static PyTypeObject qlx_gameclient_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.GameClient",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_gameclient_repr,
    .tp_getset                             = qlx_gameclient_getset,
    .tp_richcompare                        = qlx_ref_richcompare,
    .tp_hash                               = qlx_ref_hash,
    .tp_new                                = qlx_gameclient_new,
    .tp_flags                              = Py_TPFLAGS_DEFAULT,
    .tp_doc =
        "GameClient(client_id) -- a live view onto the game module's state for one "
        "player.\n\n"
        "Resolved through g_entities[client_id].client, which is NULL exactly when "
        "nobody is in that slot, and accessing anything then raises EngineStateError. "
        "Entity(n).client is the same object, or None.\n\n"
        "The big sub-views are .ps (movement, aim, stats, ammo), .pers (name, ready "
        "state, last usercmd), .sess (team, privileges, mute) and .expanded_stats.\n\n"
        "Game thread only: nothing here takes a lock. Marshal with "
        "minqlxtended.next_frame if you are on a worker thread.",
};

// Entity.client, defined here because it needs the GameClient type.
static PyObject* ent_get_client(PyObject* self, void* closure) {
    (void)closure;
    gentity_t* ent = qlx_entity(self);
    if (!ent) {
        return NULL;
    }

    // None rather than an error: the entity simply isn't a player. Same test the rest of
    // the extension makes.
    if (!ent->client) {
        Py_RETURN_NONE;
    }

    return qlx_ref_new(&qlx_gameclient_type, ((qlx_ref_t*)self)->index);
}

// Client and Netchan

#define QLX_VIEW_ACCESSOR_nc(KIND, FIELD, NAME, OFF, DOC)                                  \
    QLX_GETSET_##KIND(nc, netchan_t, qlx_netchan, FIELD, NAME)
#define QLX_VIEW_ROW_nc(KIND, FIELD, NAME, OFF, DOC)                                       \
    {#NAME, (getter)nc_get_##NAME, QLX_SETTER(KIND, nc, NAME),                             \
     QLX_DOC(netchan_t, FIELD, DOC), NULL},
#define QLX_VIEW_ASSERT_nc(KIND, FIELD, NAME, OFF, DOC)                                    \
    _Static_assert(offsetof(netchan_t, FIELD) == (OFF),                                    \
                   "netchan_t." #FIELD " moved; Client.netchan." #NAME " reads it.");      \
    QLX_KIND_ASSERT(KIND, netchan_t, FIELD, NAME)

QLX_DEFINE_VIEW(nc, netchan_t, qlx_netchan, NETCHAN_FIELDS, qlx_netchan_type, "Netchan",
                QLX_NO_EXTRA_ROWS,
                "One client's network channel, as Client(n).netchan.\n\n"
                "dropped is packet loss since the previous packet. The remote address is "
                "on Client itself, as .address, .ip and .port.")

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(cl, client_t, qlx_client, FIELD, NAME)
CLIENT_FIELDS(X)
#undef X

QLX_GET_SUBOBJ(cl, qlx_netchan_type, netchan)

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(client_t, FIELD) == (OFF),                                     \
                   "client_t." #FIELD " moved; Client." #NAME                              \
                   " reads it. Re-run tools/gen_field_offsets.py.");                       \
    QLX_KIND_ASSERT(KIND, client_t, FIELD, NAME)
CLIENT_FIELDS(X)
#undef X

/*
 * The address the client actually connected from, and what Player.ip reads. Don't use the
 * `ip` key of the userinfo: the client sets that, so a ban keyed on it is worthless.
 */
static PyObject* cl_get_ip(PyObject* self, void* closure) {
    (void)closure;
    client_t* client = qlx_client(self);
    if (!client) {
        return NULL;
    }

    const byte* ip = client->netchan.remoteAddress.ip;
    return PyUnicode_FromFormat("%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

static PyObject* cl_get_port(PyObject* self, void* closure) {
    (void)closure;
    client_t* client = qlx_client(self);
    if (!client) {
        return NULL;
    }

    // Stored network byte order; ntohs would need a header we do not otherwise want here,
    // and the swap is two lines.
    unsigned short port = client->netchan.remoteAddress.port;
    return PyLong_FromLong((long)(unsigned short)((port >> 8) | (port << 8)));
}

static PyObject* cl_get_address(PyObject* self, void* closure) {
    PyObject* ip = cl_get_ip(self, closure);
    if (!ip) {
        return NULL;
    }

    PyObject* port = cl_get_port(self, closure);
    if (!port) {
        Py_DECREF(ip);
        return NULL;
    }

    PyObject* result = PyUnicode_FromFormat("%U:%S", ip, port);
    Py_DECREF(ip);
    Py_DECREF(port);
    return result;
}

static PyObject* cl_get_address_type(PyObject* self, void* closure) {
    (void)closure;
    client_t* client = qlx_client(self);
    if (!client) {
        return NULL;
    }

    return PyLong_FromLong((long)client->netchan.remoteAddress.type);
}

static PyObject* cl_get_id(PyObject* self, void* closure) {
    (void)closure;
    return PyLong_FromLong((long)((qlx_ref_t*)self)->index);
}

static PyGetSetDef qlx_client_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)cl_get_##NAME, QLX_SETTER(KIND, cl, NAME),                             \
     QLX_DOC(client_t, FIELD, DOC), NULL},
    CLIENT_FIELDS(X)
#undef X

        {"id", (getter)cl_get_id, NULL, "This client's id.", NULL},
        {"netchan", (getter)cl_get_netchan, NULL, "The network channel.", NULL},
        {"ip", (getter)cl_get_ip, NULL,
         "The address the client connected from, as a dotted string. Unlike the userinfo "
         "'ip' key, the client cannot choose this.",
         NULL},
        {"port", (getter)cl_get_port, NULL, "The remote port.", NULL},
        {"address", (getter)cl_get_address, NULL, "ip:port.", NULL},
        {"address_type", (getter)cl_get_address_type, NULL,
         "One of the NA_* address types; NA_BOT for a bot.", NULL},
        {NULL}};

static PyObject* qlx_client_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    static char* kwlist[] = {"client_id", NULL};
    int client_id;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i:Client", kwlist, &client_id)) {
        return NULL;
    }

    if (client_id < 0 || client_id >= MAX_CLIENTS) {
        PyErr_Format(PyExc_ValueError, "client id %d is out of range (0-%d)", client_id,
                     MAX_CLIENTS - 1);
        return NULL;
    }

    return qlx_ref_new(type, client_id);
}

static PyObject* qlx_client_repr(PyObject* self) {
    int client_id    = ((qlx_ref_t*)self)->index;
    client_t* client = qlx_client(self);

    if (!client) {
        PyErr_Clear();
        return PyUnicode_FromFormat("<Client %d (server not up)>", client_id);
    }

    // Bounded for the same reason as GameClient's repr: name is a fixed char[] with no
    // guaranteed terminator.
    PyObject* name = qlx_load_charbuf(client->name, sizeof(client->name));
    if (!name) {
        return NULL;
    }

    PyObject* result = PyUnicode_FromFormat("<Client %d '%U'>", client_id, name);
    Py_DECREF(name);
    return result;
}

static PyTypeObject qlx_client_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.Client",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_client_repr,
    .tp_getset                             = qlx_client_getset,
    .tp_richcompare                        = qlx_ref_richcompare,
    .tp_hash                               = qlx_ref_hash,
    .tp_new                                = qlx_client_new,
    .tp_flags                              = Py_TPFLAGS_DEFAULT,
    .tp_doc =
        "Client(client_id) -- the server's connection record for one client slot.\n\n"
        "This is the engine's view of a connection, where GameClient is the game "
        "module's view of a player. Fields that sound the same are not: .ping is the "
        "server's own measurement and .name its own copy, while GameClient.ps.ping and "
        "GameClient.pers.netname are the game module's.\n\n"
        ".ip and .address are the address the client really connected from, which is not "
        "the same as the userinfo 'ip' key, which the client chooses.\n\n"
        "reliable_sequence minus reliable_acknowledge is how far behind a client's "
        "reliable command ring is; it holds 64, and overrunning it drops everyone.\n\n"
        "Game thread only: nothing here takes a lock.",
};

// Item

static gitem_t* qlx_item(PyObject* self) {
    int index = ((qlx_ref_t*)self)->index;

    if (!bg_itemlist) {
        qlx_no_game_module("bg_itemlist");
        return NULL;
    }

    if (index < 0 || index >= bg_numItems) {
        PyErr_Format(PyExc_ValueError, "item number %d is out of range (0-%d)", index,
                     bg_numItems - 1);
        return NULL;
    }

    return &bg_itemlist[index];
}

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(item, gitem_t, qlx_item, FIELD, NAME)
ITEM_FIELDS(X)
#undef X

static PyObject* item_get_number(PyObject* self, void* closure) {
    (void)closure;
    return PyLong_FromLong((long)((qlx_ref_t*)self)->index);
}

static PyGetSetDef qlx_item_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)item_get_##NAME, QLX_SETTER(KIND, item, NAME),                         \
     QLX_DOC(gitem_t, FIELD, DOC), NULL},
    ITEM_FIELDS(X)
#undef X
        {"number", (getter)item_get_number, NULL,
         "The index into bg_itemlist, which is what spawn_item() and replace_items() take.",
         NULL},
        {NULL}};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(gitem_t, FIELD) == (OFF),                                      \
                   "gitem_t." #FIELD " moved; Item." #NAME                                 \
                   " reads it. Re-run tools/gen_field_offsets.py.");                       \
    QLX_KIND_ASSERT(KIND, gitem_t, FIELD, NAME)
ITEM_FIELDS(X)
#undef X

static PyObject* qlx_item_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    static char* kwlist[] = {"number", NULL};
    int number;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i:Item", kwlist, &number)) {
        return NULL;
    }

    // bg_numItems is only known once the game module is up, so we check the bound on
    // access instead of here. Constructing one early is harmless.
    if (number < 0) {
        PyErr_Format(PyExc_ValueError, "item number %d is out of range", number);
        return NULL;
    }

    return qlx_ref_new(type, number);
}

static PyObject* qlx_item_repr(PyObject* self) {
    int number     = ((qlx_ref_t*)self)->index;
    gitem_t* item  = qlx_item(self);

    if (!item) {
        PyErr_Clear();
        return PyUnicode_FromFormat("<Item %d (engine not ready)>", number);
    }

    return PyUnicode_FromFormat("<Item %d '%s'>", number,
                                item->classname ? item->classname : "");
}

static PyTypeObject qlx_item_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.Item",
    .tp_basicsize                          = sizeof(qlx_ref_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_item_repr,
    .tp_getset                             = qlx_item_getset,
    .tp_richcompare                        = qlx_ref_richcompare,
    .tp_hash                               = qlx_ref_hash,
    .tp_new                                = qlx_item_new,
    .tp_flags                              = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Item(number) -- one entry of the game module's item table.\n\n"
              "Read-only, and not by choice: the table sits in a mapping the loader makes "
              "read-only once the game module is relocated, so a write faults rather than "
              "taking effect. It is shared by every entity of that type in any case.\n\n"
              "The number is the index spawn_item() and replace_items() take, which is "
              "also what the MODELINDEX_* constants are.",
};

typedef struct {
    PyObject_HEAD
    int next;
} qlx_itemiter_t;

static PyObject* qlx_itemiter_next(PyObject* self) {
    qlx_itemiter_t* it = (qlx_itemiter_t*)self;

    if (!bg_itemlist) {
        qlx_no_game_module("bg_itemlist");
        return NULL;
    }

    if (it->next >= bg_numItems) {
        return NULL; // StopIteration
    }

    return qlx_ref_new(&qlx_item_type, it->next++);
}

static PyTypeObject qlx_itemiter_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.ItemIterator",
    .tp_basicsize                          = sizeof(qlx_itemiter_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_singleton_repr,
    .tp_iter                               = PyObject_SelfIter,
    .tp_iternext                           = qlx_itemiter_next,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc   = "Lazy iterator over bg_itemlist, from minqlxtended.items().",
};

PyObject* PyMinqlxtended_Items(PyObject* self, PyObject* args) {
    (void)self;
    (void)args;

    qlx_itemiter_t* it =
        (qlx_itemiter_t*)qlx_itemiter_type.tp_alloc(&qlx_itemiter_type, 0);
    if (!it) {
        return NULL;
    }

    // From 1: index 0 is the null item, which spawn_item() and replace_items() both
    // already treat as "not an item".
    it->next = 1;
    return (PyObject*)it;
}

// Cvar

/*
 * The name is kept only so the pointer can be revalidated. The engine caps nothing here,
 * and an over-long name is a ValueError out of the constructor.
 */
#define QLX_CVAR_NAME_MAX 256

typedef struct {
    PyObject_HEAD
    cvar_t* ptr;
    char name[QLX_CVAR_NAME_MAX];
} qlx_cvar_t;

static PyTypeObject qlx_cvar_type;

/*
 * Revalidated on every access: a Cvar cannot be index-backed, so it checks the pointer
 * still carries the name it was created for and looks it up again if not.
 */
static cvar_t* qlx_cvar(PyObject* self) {
    qlx_cvar_t* wrapper = (qlx_cvar_t*)self;

    if (wrapper->ptr && wrapper->ptr->name && !strcmp(wrapper->ptr->name, wrapper->name)) {
        return wrapper->ptr;
    }

    if (!Cvar_FindVar) {
        PyErr_SetString(qlx_EngineStateError, "Cvar_FindVar is not available yet");
        return NULL;
    }

    wrapper->ptr = Cvar_FindVar(wrapper->name);
    if (!wrapper->ptr) {
        PyErr_Format(PyExc_KeyError, "no cvar named '%s'", wrapper->name);
        return NULL;
    }

    return wrapper->ptr;
}

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    QLX_GETSET_##KIND(cvar, cvar_t, qlx_cvar, FIELD, NAME)
CVAR_FIELDS(X)
#undef X

/*
 * force skips Cvar_Set2's latch branch and rewrites ->integer on the spot. SV_ChangeMaxClients
 * sizes svs.clients from sv_maxclients, so forcing it up puts every client-slot walk here past
 * the end of the allocation. Only this name is refused.
 */
qboolean qlx_refuse_forced_write(const char* name, int force) {
    if (!force || !name || strcasecmp(name, "sv_maxclients") != 0) {
        return qfalse;
    }

    PyErr_SetString(PyExc_ValueError,
                    "sv_maxclients cannot be force-set: the engine allocates the client slots "
                    "from it when a map spawns, so forcing it past the latch leaves the count "
                    "and the allocation disagreeing. Set it without force and it applies on "
                    "the next map.");
    return qtrue;
}

/*
 * cvar->string is a heap pointer the engine owns and reallocates. Cvar_Set2 also bumps
 * modificationCount and propagates CVAR_SERVERINFO / CVAR_SYSTEMINFO changes to clients.
 */
static int qlx_cvar_set_string(cvar_t* cvar, const char* value, int force) {
    if (!Cvar_Set2) {
        PyErr_SetString(qlx_EngineStateError, "Cvar_Set2 is not available yet");
        return -1;
    }

    if (qlx_refuse_forced_write(cvar->name, force)) {
        return -1;
    }

    // Through the hook, so a write via the Cvar view raises cvar_changed too.
    My_Cvar_Set2(cvar->name, value, force ? qtrue : qfalse);
    return 0;
}

static PyObject* cvar_get_string(PyObject* self, void* closure) {
    (void)closure;
    cvar_t* cvar = qlx_cvar(self);
    if (!cvar) {
        return NULL;
    }

    if (!cvar->string) {
        Py_RETURN_NONE;
    }

    return PyUnicode_DecodeUTF8(cvar->string, (Py_ssize_t)strlen(cvar->string), "ignore");
}

static int cvar_set_string(PyObject* self, PyObject* value, void* closure) {
    (void)closure;
    cvar_t* cvar = qlx_cvar(self);
    if (!cvar) {
        return -1;
    }

    if (qlx_check_assignable(value, "string")) {
        return -1;
    }

    if (!PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "'string' takes a str");
        return -1;
    }

    const char* utf8 = PyUnicode_AsUTF8(value);
    return utf8 ? qlx_cvar_set_string(cvar, utf8, 0) : -1;
}

static PyObject* cvar_get_value(PyObject* self, void* closure) {
    (void)closure;
    cvar_t* cvar = qlx_cvar(self);
    return cvar ? PyFloat_FromDouble((double)cvar->value) : NULL;
}

static int cvar_set_value(PyObject* self, PyObject* value, void* closure) {
    (void)closure;
    cvar_t* cvar = qlx_cvar(self);
    if (!cvar) {
        return -1;
    }

    if (qlx_check_assignable(value, "value")) {
        return -1;
    }

    double v = PyFloat_AsDouble(value);
    if (v == -1.0 && PyErr_Occurred()) {
        return -1;
    }

    /*
     * The fewest significant digits that read back as the same double; 17 always does.
     * %g's default of six writes 1000000 as "1e+06", which the engine's atoi reads as 1.
     */
    char buffer[64];
    for (int digits = 15; digits <= 17; digits++) {
        snprintf(buffer, sizeof(buffer), "%.*g", digits, v);
        if (strtod(buffer, NULL) == v) {
            break;
        }
    }

    return qlx_cvar_set_string(cvar, buffer, 0);
}

static PyObject* cvar_get_integer(PyObject* self, void* closure) {
    (void)closure;
    cvar_t* cvar = qlx_cvar(self);
    return cvar ? PyLong_FromLong((long)cvar->integer) : NULL;
}

static int cvar_set_integer(PyObject* self, PyObject* value, void* closure) {
    (void)closure;
    cvar_t* cvar = qlx_cvar(self);
    if (!cvar) {
        return -1;
    }

    int parsed;
    if (qlx_store_int(value, &parsed, "integer")) {
        return -1;
    }

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", parsed);
    return qlx_cvar_set_string(cvar, buffer, 0);
}

static PyObject* qlx_cvar_set_method(PyObject* self, PyObject* args, PyObject* kwds) {
    static char* kwlist[] = {"value", "force", NULL};
    const char* value;
    int force = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|p:set", kwlist, &value, &force)) {
        return NULL;
    }

    cvar_t* cvar = qlx_cvar(self);
    if (!cvar) {
        return NULL;
    }

    if (qlx_cvar_set_string(cvar, value, force)) {
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyMethodDef qlx_cvar_methods[] = {
    {"set", (PyCFunction)(void (*)(void))qlx_cvar_set_method, METH_VARARGS | METH_KEYWORDS,
     "set(value, force=False) -- assign the cvar, optionally overriding CVAR_ROM, "
     "CVAR_INIT and a latch."},
    {NULL, NULL, 0, NULL}};

static PyGetSetDef qlx_cvar_getset[] = {
#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    {#NAME, (getter)cvar_get_##NAME, QLX_SETTER(KIND, cvar, NAME),                         \
     QLX_DOC(cvar_t, FIELD, DOC), NULL},
    CVAR_FIELDS(X)
#undef X
        {"string", (getter)cvar_get_string, (setter)cvar_set_string,
         "The value. Writes go through Cvar_Set2, so they propagate and bump the "
         "modification count.",
         NULL},
        {"value", (getter)cvar_get_value, (setter)cvar_set_value,
         "The value as a float.", NULL},
        {"integer", (getter)cvar_get_integer, (setter)cvar_set_integer,
         "The value as an int.", NULL},
        {NULL}};

#define X(KIND, FIELD, NAME, OFF, DOC)                                                     \
    _Static_assert(offsetof(cvar_t, FIELD) == (OFF),                                       \
                   "cvar_t." #FIELD " moved; Cvar." #NAME                                  \
                   " reads it. Re-run tools/gen_field_offsets.py.");                       \
    QLX_KIND_ASSERT(KIND, cvar_t, FIELD, NAME)
CVAR_FIELDS(X)
#undef X

static PyObject* qlx_cvar_new(PyTypeObject* type, PyObject* args, PyObject* kwds) {
    static char* kwlist[] = {"name", NULL};
    const char* name;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s:Cvar", kwlist, &name)) {
        return NULL;
    }

    if (strlen(name) >= QLX_CVAR_NAME_MAX) {
        PyErr_Format(PyExc_ValueError, "cvar name is longer than %d bytes",
                     QLX_CVAR_NAME_MAX - 1);
        return NULL;
    }

    if (!Cvar_FindVar) {
        PyErr_SetString(qlx_EngineStateError, "Cvar_FindVar is not available yet");
        return NULL;
    }

    cvar_t* found = Cvar_FindVar(name);
    if (!found) {
        PyErr_Format(PyExc_KeyError, "no cvar named '%s'", name);
        return NULL;
    }

    qlx_cvar_t* self = (qlx_cvar_t*)type->tp_alloc(type, 0);
    if (!self) {
        return NULL;
    }

    self->ptr = found;
    strncpy(self->name, name, sizeof(self->name) - 1);
    self->name[sizeof(self->name) - 1] = '\0';
    return (PyObject*)self;
}

static PyObject* qlx_cvar_repr(PyObject* self) {
    qlx_cvar_t* wrapper = (qlx_cvar_t*)self;
    cvar_t* cvar        = qlx_cvar(self);

    if (!cvar) {
        PyErr_Clear();
        return PyUnicode_FromFormat("<Cvar '%s' (gone)>", wrapper->name);
    }

    return PyUnicode_FromFormat("<Cvar '%s' = '%s'>", wrapper->name,
                                cvar->string ? cvar->string : "");
}

/*
 * Value semantics on the name, matching qlx_cvar(). On identity, Cvar("sv_fps") ==
 * Cvar("sv_fps") is False and set(cvars()) de-duplicates nothing.
 */
static PyObject* qlx_cvar_richcompare(PyObject* self, PyObject* other, int op) {
    if ((op != Py_EQ && op != Py_NE) || Py_TYPE(self) != Py_TYPE(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    int same = strcmp(((qlx_cvar_t*)self)->name, ((qlx_cvar_t*)other)->name) == 0;
    return PyBool_FromLong(op == Py_EQ ? same : !same);
}

static Py_hash_t qlx_cvar_hash(PyObject* self) {
    // Hashed as the name string so it agrees with the comparison above. Through a str
    // rather than the bytes by hand, to stay consistent with however the interpreter is
    // salting string hashes in this process.
    PyObject* name = PyUnicode_FromString(((qlx_cvar_t*)self)->name);
    if (!name) {
        return -1;
    }

    Py_hash_t hash = PyObject_Hash(name);
    Py_DECREF(name);
    return hash;
}

static PyTypeObject qlx_cvar_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.Cvar",
    .tp_basicsize                          = sizeof(qlx_cvar_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_cvar_repr,
    .tp_richcompare                        = qlx_cvar_richcompare,
    .tp_hash                               = qlx_cvar_hash,
    .tp_methods                            = qlx_cvar_methods,
    .tp_getset                             = qlx_cvar_getset,
    .tp_new                                = qlx_cvar_new,
    .tp_flags                              = Py_TPFLAGS_DEFAULT,
    .tp_doc =
        "Cvar(name) -- a console variable, with everything the engine records about "
        "it.\n\n"
        "get_cvar() returns only the value as a string; this adds the flags, the default "
        "and reset strings, a pending latched value, the modification count, and typed "
        "value/integer reads.\n\n"
        "Writes go through the engine's own setter, so they propagate to clients and bump "
        "the modification count. Use .set(value, force=True) to override CVAR_ROM, "
        "CVAR_INIT or a latch.\n\n"
        "Raises KeyError if there is no such cvar.",
};

/*
 * Enumeration. The one iterator here that holds a raw pointer, because a cvar has no index
 * to hold instead. Safe enough, since the engine never frees a cvar_t.
 */
typedef struct {
    PyObject_HEAD
    cvar_t* next;
} qlx_cvariter_t;

static PyObject* qlx_cvariter_next(PyObject* self) {
    qlx_cvariter_t* it = (qlx_cvariter_t*)self;

    /*
     * Anything the Cvar type cannot represent is walked past. NULL means StopIteration, so
     * stopping on one odd entry would hand back a silently partial list.
     */
    while (it->next) {
        cvar_t* current = it->next;
        it->next        = current->next;

        if (current->name && strlen(current->name) < QLX_CVAR_NAME_MAX) {
            return PyObject_CallFunction((PyObject*)&qlx_cvar_type, "s", current->name);
        }
    }

    return NULL; // StopIteration
}

static PyTypeObject qlx_cvariter_type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "minqlxtended.CvarIterator",
    .tp_basicsize                          = sizeof(qlx_cvariter_t),
    .tp_dealloc                            = qlx_ref_dealloc,
    .tp_repr                               = qlx_singleton_repr,
    .tp_iter                               = PyObject_SelfIter,
    .tp_iternext                           = qlx_cvariter_next,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .tp_doc   = "Lazy iterator over the engine's cvar list, from minqlxtended.cvars().",
};

PyObject* PyMinqlxtended_Cvars(PyObject* self, PyObject* args) {
    (void)self;
    (void)args;

    qlx_cvariter_t* it = (qlx_cvariter_t*)qlx_cvariter_type.tp_alloc(&qlx_cvariter_type, 0);
    if (!it) {
        return NULL;
    }

    /*
     * Cvar_Get prepends, so a cvar registered mid-walk is not seen. cvar_vars is NULL when
     * its offset did not check out, and this then yields nothing.
     */
    it->next = cvar_vars ? *cvar_vars : NULL;
    return (PyObject*)it;
}

// The Cvar type is private to this file, so anything elsewhere that hands one back
// (set_cvar does) goes through here.
PyObject* PyMinqlxtended_MakeCvar(const char* name) {
    return PyObject_CallFunction((PyObject*)&qlx_cvar_type, "s", name);
}

PyObject* PyMinqlxtended_Cvar(PyObject* self, PyObject* args) {
    (void)self;
    const char* name;

    if (!PyArg_ParseTuple(args, "s:cvar", &name)) {
        return NULL;
    }

    if (!Cvar_FindVar) {
        PyErr_SetString(qlx_EngineStateError, "Cvar_FindVar is not available yet");
        return NULL;
    }

    // Answers None for a missing cvar. This is the "does it exist" spelling; Cvar(name)
    // is the "give me it" one.
    if (!Cvar_FindVar(name)) {
        Py_RETURN_NONE;
    }

    return PyMinqlxtended_MakeCvar(name);
}

// Registration

int PyMinqlxtended_AddObjectTypes(PyObject* module) {
    /*
     * Defined in C so it exists before any of the framework imports, and subclasses
     * RuntimeError so callers that only want to know something went wrong need not name it.
     */
    if (!qlx_EngineStateError) {
        qlx_EngineStateError = PyErr_NewExceptionWithDoc(
            "minqlxtended.EngineStateError",
            "The engine is not in a state where that question has an answer. No map is "
            "loaded, or the game VM is not up yet. Distinct from ValueError, which means "
            "the thing you named is out of range.",
            PyExc_RuntimeError, NULL);
        if (!qlx_EngineStateError) {
            return -1;
        }
    }

    Py_INCREF(qlx_EngineStateError);
    if (PyModule_AddObject(module, "EngineStateError", qlx_EngineStateError) == -1) {
        Py_DECREF(qlx_EngineStateError);
        return -1;
    }

    PyTypeObject* types[] = {
        &qlx_intarray_type,      &qlx_roundstate_type,    &qlx_level_type,
        &qlx_entitystate_type,   &qlx_entityshared_type,  &qlx_entity_type,
        &qlx_entityiter_type,    &qlx_playerstate_type,   &qlx_teamstate_type,
        &qlx_persistant_type,    &qlx_session_type,       &qlx_raceinfo_type,
        &qlx_expandedstats_type, &qlx_gameclient_type,    &qlx_netchan_type,
        &qlx_client_type,        &qlx_item_type,          &qlx_itemiter_type,
        &qlx_cvar_type,          &qlx_cvariter_type,      &qlx_server_type,
        &qlx_serverstatic_type,  &qlx_matchstate_type};
    // Laid out three to a row like types[] above so the pairing can be read off by eye.
    // "PlayerStateView" is not a typo: PlayerState is already the struct sequence that
    // player_state() returns, so the live view of playerState_t needs its own name.
    const char* names[] = {
        "IntArray",      "RoundStateView", "Level",
        "EntityState",   "EntityShared",   "Entity",
        "EntityIterator", "PlayerStateView", "TeamState",
        "Persistant",    "Session",        "RaceInfo",
        "ExpandedStats", "GameClient",     "Netchan",
        "Client",        "Item",           "ItemIterator",
        "Cvar",          "CvarIterator",   "Server",
        "ServerStatic",  "MatchState"};

    // types[] and names[] are parallel arrays with nothing else tying them together, so a
    // type added to one and not the other reads past the end of names[]. Same guard, and
    // same reasoning, as prof_names in profile.c.
    _Static_assert(sizeof(types) / sizeof(*types) == sizeof(names) / sizeof(*names),
                   "names[] must stay in step with types[]");

    for (size_t i = 0; i < sizeof(types) / sizeof(*types); i++) {
        if (PyType_Ready(types[i]) == -1) {
            return -1;
        }

        Py_INCREF(types[i]);
        if (PyModule_AddObject(module, names[i], (PyObject*)types[i]) == -1) {
            Py_DECREF(types[i]);
            return -1;
        }
    }

    /*
     * One instance each, installed as module attributes. An import copies the reference, so
     * minqlxtended.level and _minqlxtended.level are the same object.
     */
    static const struct {
        PyTypeObject* type;
        const char* name;
    } singletons[] = {
        {&qlx_level_type, "level"},
        {&qlx_matchstate_type, "match_state"},
        {&qlx_server_type, "server"},
        {&qlx_serverstatic_type, "server_static"},
    };

    for (size_t i = 0; i < sizeof(singletons) / sizeof(*singletons); i++) {
        PyObject* instance = qlx_ref_new(singletons[i].type, -1);
        if (!instance) {
            return -1;
        }

        if (PyModule_AddObject(module, singletons[i].name, instance) == -1) {
            Py_DECREF(instance);
            return -1;
        }
    }

    return 0;
}
