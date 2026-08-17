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

#include "features/profile.h"
#include "pyminqlxtended.h"
#include "engine/quake_common.h"

int allow_free_client = -1;

// The game thread drops the GIL after init, so every dispatcher below reacquires it and
// blocks while a worker holds it. That wait is timed into PROF_GIL_WAIT separately.

/* A string the engine handed us, as a Python str. Use it for every string out of the engine.
 * The decode is lossy: these are unvalidated client bytes, and a strict decode would turn one
 * 0x80 byte into a failed conversion and a handler that never runs. */
static PyObject* FromEngine(const char* s) {
    return PyUnicode_DecodeUTF8(s ? s : "", s ? (Py_ssize_t)strlen(s) : 0, "ignore");
}

/* Why a dispatch produced nothing. An event that can be cancelled needs opposite answers for
 * an empty slot and for arguments it could not convert. */
typedef enum {
    HANDLER_ABSENT = 0, // the slot was empty
    HANDLER_RAN,        // the handler was called; a NULL result means it raised
    HANDLER_BAD_ARGS,   // an argument could not be converted, so nothing was called
} handler_status_t;

/* Vectorcall the handler in `slot` with `argc` arguments, releasing every reference in `argv`
 * afterwards. The caller hands over a reference to each argument, so pass Py_NewRef(Py_True)
 * rather than a bare Py_True. The slot is loaded, checked and referenced here under the GIL,
 * since register_handler() can clear it from another thread. A NULL in argv means a conversion
 * failed: nothing is called, the exception it raised stays set, and `status` reports it. */
static PyObject* CallHandlerStatus(PyObject** slot, PyObject** argv, Py_ssize_t argc,
                                   handler_status_t* status) {
    PyObject* result    = NULL;
    PyObject* handler   = Py_XNewRef(*slot);
    handler_status_t st = HANDLER_ABSENT;

    for (Py_ssize_t i = 0; i < argc; i++) {
        if (!argv[i]) {
            st = HANDLER_BAD_ARGS;
            goto done;
        }
    }

    if (handler) {
        st     = HANDLER_RAN;
        result = PyObject_Vectorcall(handler, argv, (size_t)argc, NULL);
    }

done:
    for (Py_ssize_t i = 0; i < argc; i++) {
        Py_XDECREF(argv[i]);
    }
    Py_XDECREF(handler);

    if (status) {
        *status = st;
    }
    return result;
}

static PyObject* CallHandler(PyObject** slot, PyObject** argv, Py_ssize_t argc) {
    return CallHandlerStatus(slot, argv, argc, NULL);
}

/* Release the GIL at the end of a dispatch, flushing any exception first. PyGILState_Release
 * does not clear the error indicator, and the game thread reuses the main thread state, so a
 * handler's exception would survive into the next Ensure and be raised against unrelated code.
 * Every dispatcher exits through here, as does PyCommand in commands.c. */
void DispatcherRelease(PyGILState_STATE gstate) {
    if (PyErr_Occurred()) {
        // PyErr_Print calls exit() on SystemExit. WriteUnraisable logs through
        // sys.unraisablehook and always clears the indicator.
        PyErr_WriteUnraisable(NULL);
    }
    PyGILState_Release(gstate);
}

/*
 * Copy a handler's replacement string into a dispatcher's static buffer, shortening it if
 * needed. The cut backs up to a UTF-8 boundary, since half a codepoint crashes a client.
 */
static char* CopyReplacement(char* buf, size_t buf_size, const char* s, Py_ssize_t len,
                             const char* what) {
    if ((size_t)len > buf_size - 1) {
        size_t fit = buf_size - 1;
        while (fit > 0 && ((unsigned char)s[fit] & 0xC0) == 0x80) {
            fit--;
        }
        DebugPrint("WARNING: a %s handler returned %zd bytes, over the %zu-byte limit. "
                   "Shortening it to %zu.\n", what, len, buf_size - 1, fit);
        len = (Py_ssize_t)fit;
    }
    memcpy(buf, s, (size_t)len);
    buf[len] = '\0';
    return buf;
}

char* ClientCommandDispatcher(int client_id, char* cmd) {
    char* ret = cmd;
    static char ccmd_buf[4096];
    if (!client_command_handler) {
        return ret; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    size_t cmd_len = strlen(cmd);
    PyObject* cmd_string = FromEngine(cmd);
    // A fresh reference, because the identity check below needs cmd_string to outlive the
    // call and CallHandler releases everything it is given.
    PyObject* argv[] = {PyLong_FromLong(client_id), Py_XNewRef(cmd_string)};
    PyObject* result = CallHandler(&client_command_handler, argv, 2);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = NULL;
    } else if (PyUnicode_Check(result)) {
        Py_ssize_t len;
        const char* s = PyUnicode_AsUTF8AndSize(result, &len);
        if (s) {
            if (result == cmd_string && (size_t)len == cmd_len) {
                // Returned unchanged and the decode was lossless; the engine's
                // buffer already holds these exact bytes, so skip the copy.
                ret = cmd;
            } else {
                ret = CopyReplacement(ccmd_buf, sizeof(ccmd_buf), s, len, "client_command");
            }
        }
    }

    Py_XDECREF(cmd_string);
    Py_XDECREF(result);

    PROF_END(PROF_CLIENT_COMMAND, t_work);
    DispatcherRelease(gstate);
    return ret;
}

char* ServerCommandDispatcher(int client_id, char* cmd) {
    char* ret = cmd;
    static char scmd_buf[4096];
    if (!server_command_handler) {
        return ret; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    size_t cmd_len = strlen(cmd);
    PyObject* cmd_string = FromEngine(cmd);
    // A fresh reference, because the identity check below needs cmd_string to outlive the
    // call and CallHandler releases everything it is given.
    PyObject* argv[] = {PyLong_FromLong(client_id), Py_XNewRef(cmd_string)};
    PyObject* result = CallHandler(&server_command_handler, argv, 2);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = NULL;
    } else if (PyUnicode_Check(result)) {
        Py_ssize_t len;
        const char* s = PyUnicode_AsUTF8AndSize(result, &len);
        if (s) {
            if (result == cmd_string && (size_t)len == cmd_len) {
                // Returned unchanged and the decode was lossless; the engine's
                // buffer already holds these exact bytes, so skip the copy.
                ret = cmd;
            } else {
                ret = CopyReplacement(scmd_buf, sizeof(scmd_buf), s, len, "server_command");
            }
        }
    }

    Py_XDECREF(cmd_string);
    Py_XDECREF(result);

    PROF_END(PROF_SERVER_COMMAND, t_work);
    DispatcherRelease(gstate);
    return ret;
}

void FrameDispatcher(void) {
    if (!frame_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* result = CallHandler(&frame_handler, NULL, 0);
    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n", __FILE__, __LINE__, __func__);
    }

    Py_XDECREF(result);

    PROF_END(PROF_FRAME_DISPATCH, t_work);
    DispatcherRelease(gstate);
    return;
}

char* ClientConnectDispatcher(int client_id, int is_bot) {
    char* ret = NULL;
    static char connect_buf[4096];
    if (!client_connect_handler) {
        return ret; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    // Lets PyMinqlxtended_PlayerInfo read a CS_FREE client. Saved and restored rather than
    // cleared, since a handler that kicks reaches ClientDisconnectDispatcher, and that
    // nested dispatch would drop the permission out from under this one.
    int prev_allow_free_client = allow_free_client;
    allow_free_client         = client_id;
    PyObject* argv[]  = {PyLong_FromLong(client_id), Py_NewRef(is_bot ? Py_True : Py_False)};
    PyObject* result  = CallHandler(&client_connect_handler, argv, 2);
    allow_free_client = prev_allow_free_client;

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = "You are banned from this server.";
    } else if (PyUnicode_Check(result)) {
        Py_ssize_t len;
        const char* s = PyUnicode_AsUTF8AndSize(result, &len);
        if (s) {
            ret = CopyReplacement(connect_buf, sizeof(connect_buf), s, len, "player_connect");
        }
    }

    Py_XDECREF(result);

    PROF_END(PROF_CLIENT_CONNECT, t_work);
    DispatcherRelease(gstate);
    return ret;
}

void ClientDisconnectDispatcher(int client_id, const char* reason) {
    if (!client_disconnect_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    // Tell PyMinqlxtended_PlayerInfo it's OK to get player info for someone with CS_FREE.
    // Saved and restored around the dispatch. See ClientConnectDispatcher.
    int prev_allow_free_client = allow_free_client;
    allow_free_client          = client_id;
    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        FromEngine(reason),
    };
    PyObject* result = CallHandler(&client_disconnect_handler, argv, 2);
    allow_free_client          = prev_allow_free_client;

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }

    Py_XDECREF(result);

    PROF_END(PROF_CLIENT_DISCONNECT, t_work);
    DispatcherRelease(gstate);
    return;
}

// Does not trigger on bots.
int ClientLoadedDispatcher(int client_id) {
    int ret = 1;
    if (!client_loaded_handler) {
        return ret; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(client_id),
    };
    PyObject* result = CallHandler(&client_loaded_handler, argv, 1);

    // Only change to 0 if we got False returned to us.
    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = 0;
    }

    Py_XDECREF(result);

    PROF_END(PROF_CLIENT_LOADED, t_work);
    DispatcherRelease(gstate);
    return ret;
}

/* Fires just before SV_SpawnServer wipes and repopulates the configstring table, so anything
 * cached for the old map is dropped here. Only the GIL wait is probed. */
void SpawnServerDispatcher(void) {
    if (!spawn_server_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);

    PyObject* result = CallHandler(&spawn_server_handler, NULL, 0);
    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n", __FILE__, __LINE__, __func__);
    }

    Py_XDECREF(result);

    DispatcherRelease(gstate);
    return;
}

void NewGameDispatcher(int restart) {
    if (!new_game_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        Py_NewRef(restart ? Py_True : Py_False),
    };
    PyObject* result = CallHandler(&new_game_handler, argv, 1);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n", __FILE__, __LINE__, __func__);
    }

    Py_XDECREF(result);

    PROF_END(PROF_NEW_GAME, t_work);
    DispatcherRelease(gstate);
    return;
}

char* SetConfigstringDispatcher(int index, char* value) {
    char* ret = value;
    static char setcs_buf[4096];
    if (!set_configstring_handler) {
        return ret; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    size_t value_len = strlen(value);
    PyObject* value_string = FromEngine(value);
    PyObject* argv[] = {PyLong_FromLong(index), Py_XNewRef(value_string)};
    PyObject* result = CallHandler(&set_configstring_handler, argv, 2);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = NULL;
    } else if (PyUnicode_Check(result)) {
        Py_ssize_t len;
        const char* s = PyUnicode_AsUTF8AndSize(result, &len);
        if (s) {
            if (result == value_string && (size_t)len == value_len) {
                // Returned unchanged and the decode was lossless; the engine's
                // buffer already holds these exact bytes, so skip the copy.
                ret = value;
            } else {
                ret = CopyReplacement(setcs_buf, sizeof(setcs_buf), s, len, "set_configstring");
            }
        }
    }

    Py_XDECREF(value_string);
    Py_XDECREF(result);

    PROF_END(PROF_SET_CONFIGSTRING, t_work);
    DispatcherRelease(gstate);
    return ret;
}

void RconDispatcher(const char* cmd) {
    if (!rcon_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        FromEngine(cmd),
    };
    PyObject* result = CallHandler(&rcon_handler, argv, 1);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_RCON, t_work);
    DispatcherRelease(gstate);
}

char* ConsolePrintDispatcher(char* text) {
    char* ret = text;
    static char print_buf[4096];
    if (!console_print_handler) {
        return ret; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    size_t text_len = strlen(text);
    PyObject* text_string = FromEngine(text);
    PyObject* argv[] = {Py_XNewRef(text_string)};
    PyObject* result = CallHandler(&console_print_handler, argv, 1);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = NULL;
    } else if (PyUnicode_Check(result)) {
        Py_ssize_t len;
        const char* s = PyUnicode_AsUTF8AndSize(result, &len);
        if (s) {
            if (result == text_string && (size_t)len == text_len) {
                // Returned unchanged and the decode was lossless; the engine's
                // buffer already holds these exact bytes, so skip the copy.
                ret = text;
            } else {
                ret = CopyReplacement(print_buf, sizeof(print_buf), s, len, "console_print");
            }
        }
    }

    Py_XDECREF(text_string);
    Py_XDECREF(result);

    PROF_END(PROF_CONSOLE_PRINT, t_work);
    DispatcherRelease(gstate);
    return ret;
}

void ClientSpawnDispatcher(int client_id) {
    if (!client_spawn_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(client_id),
    };
    PyObject* result = CallHandler(&client_spawn_handler, argv, 1);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_CLIENT_SPAWN, t_work);
    DispatcherRelease(gstate);
}

void KamikazeUseDispatcher(int client_id) {
    if (!kamikaze_use_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(client_id),
    };
    PyObject* result = CallHandler(&kamikaze_use_handler, argv, 1);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_KAMIKAZE_USE, t_work);
    DispatcherRelease(gstate);
}

void KamikazeExplodeDispatcher(int client_id, int is_used_on_demand) {
    if (!kamikaze_explode_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        PyLong_FromLong(is_used_on_demand),
    };
    PyObject* result = CallHandler(&kamikaze_explode_handler, argv, 2);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_KAMIKAZE_EXPLODE, t_work);
    DispatcherRelease(gstate);
}

/* Called from the frame hook and the SV_Shutdown hook, both on the game thread. The demo
 * writer holds no GIL and never calls this. */
void DemoFinishedDispatcher(int client_id, const char* path, long bytes, int discarded, int failed) {
    if (!demo_finished_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        FromEngine(path),
        PyLong_FromLong(bytes),
        Py_NewRef(discarded ? Py_True : Py_False),
        Py_NewRef(failed ? Py_True : Py_False),
    };
    PyObject* result = CallHandler(&demo_finished_handler, argv, 5);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_DEMO_FINISHED, t_work);
    DispatcherRelease(gstate);
}

/*
 * The dispatchers below all run on the game thread, from the frame poll in game_events.c
 * or a hook, so the level still exists when the handler runs.
 */

void PlayerDeathDispatcher(int victim_id, int killer_id, int mod) {
    if (!player_death_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(victim_id),
        PyLong_FromLong(killer_id),
        PyLong_FromLong(mod),
    };
    PyObject* result = CallHandler(&player_death_handler, argv, 3);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_PLAYER_DEATH, t_work);
    DispatcherRelease(gstate);
}

void RoundCountdownDispatcher(int round_number) {
    if (!round_countdown_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(round_number),
    };
    PyObject* result = CallHandler(&round_countdown_handler, argv, 1);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_ROUND_COUNTDOWN, t_work);
    DispatcherRelease(gstate);
}

void RoundStartDispatcher(int round_number) {
    if (!round_start_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(round_number),
    };
    PyObject* result = CallHandler(&round_start_handler, argv, 1);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_ROUND_START, t_work);
    DispatcherRelease(gstate);
}

void RoundEndDispatcher(int round_number, int winning_team, int time_ms) {
    if (!round_end_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(round_number),
        PyLong_FromLong(winning_team),
        PyLong_FromLong(time_ms),
    };
    PyObject* result = CallHandler(&round_end_handler, argv, 3);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_ROUND_END, t_work);
    DispatcherRelease(gstate);
}

void GameCountdownDispatcher(void) {
    if (!game_countdown_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* result = CallHandler(&game_countdown_handler, NULL, 0);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_GAME_COUNTDOWN, t_work);
    DispatcherRelease(gstate);
}

void GameStartDispatcher(void) {
    if (!game_start_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* result = CallHandler(&game_start_handler, NULL, 0);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_GAME_START, t_work);
    DispatcherRelease(gstate);
}

void GameEndDispatcher(int aborted) {
    if (!game_end_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        Py_NewRef(aborted ? Py_True : Py_False),
    };
    PyObject* result = CallHandler(&game_end_handler, argv, 1);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_GAME_END, t_work);
    DispatcherRelease(gstate);
}

int TeamSwitchDispatcher(int client_id, int old_team, int new_team) {
    int ret = 1; // Allowed unless a handler says otherwise.
    if (!team_switch_handler) {
        return ret; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        PyLong_FromLong(old_team),
        PyLong_FromLong(new_team),
    };
    PyObject* result = CallHandler(&team_switch_handler, argv, 3);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = 0;
    }
    Py_XDECREF(result);

    PROF_END(PROF_TEAM_SWITCH, t_work);
    DispatcherRelease(gstate);
    return ret;
}

void ItemPickupDispatcher(int client_id, const char* item_name) {
    if (!item_pickup_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    // The classname comes from bg_itemlist, so it is ASCII and never NULL in practice,
    // but a missing gitem_t would otherwise hand PyUnicode_FromString a NULL.
    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        FromEngine(item_name),
    };
    PyObject* result = CallHandler(&item_pickup_handler, argv, 2);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_ITEM_PICKUP, t_work);
    DispatcherRelease(gstate);
}

int VoteCalledDispatcher(int client_id, const char* vote, const char* args) {
    int ret = 1; // Allowed unless a handler says otherwise.
    if (!vote_called_handler) {
        return ret; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    // Both come from Cmd_Argv, so they're never NULL in practice, but "s" would read a
    // NULL as a missing argument and raise instead of passing None.
    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        FromEngine(vote),
        FromEngine(args),
    };
    handler_status_t status;
    PyObject* result = CallHandlerStatus(&vote_called_handler, argv, 3, &status);

    if (status == HANDLER_BAD_ARGS) {
        // Refused. A handler that was never asked has approved nothing, and the vote text is
        // client-controlled. See FromEngine.
        DebugError("could not describe the vote to Python; refusing it.\n",
                   __FILE__, __LINE__, __func__);
        ret = 0;
    } else if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = 0;
    }
    Py_XDECREF(result);

    PROF_END(PROF_VOTE_CALLED, t_work);
    DispatcherRelease(gstate);
    return ret;
}

void VoteStartedDispatcher(int caller_id, const char* vote_string) {
    if (!vote_started_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(caller_id),
        FromEngine(vote_string),
    };
    PyObject* result = CallHandler(&vote_started_handler, argv, 2);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_VOTE_STARTED, t_work);
    DispatcherRelease(gstate);
}

void VoteEndedDispatcher(int passed, const char* vote_string, int yes, int no) {
    if (!vote_ended_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        Py_NewRef(passed ? Py_True : Py_False),
        FromEngine(vote_string),
        PyLong_FromLong(yes),
        PyLong_FromLong(no),
    };
    PyObject* result = CallHandler(&vote_ended_handler, argv, 4);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_VOTE_ENDED, t_work);
    DispatcherRelease(gstate);
}

int ChatDispatcher(int client_id, int target_id, int mode, const char* text) {
    int ret = 1; // Allowed unless a handler says otherwise.
    if (!chat_handler) {
        return ret;
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    // Decoded with "ignore". These are whatever bytes the client sent, and a malformed
    // sequence must never raise on the game thread.
    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        PyLong_FromLong(target_id),
        PyLong_FromLong(mode),
        FromEngine(text),
    };
    PyObject* result = CallHandler(&chat_handler, argv, 4);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = 0;
    }
    Py_XDECREF(result);

    PROF_END(PROF_CHAT, t_work);
    DispatcherRelease(gstate);
    return ret;
}

int TeamSwitchAttemptDispatcher(int client_id, int old_team, const char* target) {
    int ret = 1;
    if (!team_switch_attempt_handler) {
        return ret;
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        PyLong_FromLong(old_team),
        FromEngine(target),
    };
    handler_status_t status;
    PyObject* result = CallHandlerStatus(&team_switch_attempt_handler, argv, 3, &status);

    if (status == HANDLER_BAD_ARGS) {
        // The target team name comes straight from the client's command. Refuse the switch
        // rather than let it through unseen. See FromEngine.
        DebugError("could not describe the team switch to Python; refusing it.\n",
                   __FILE__, __LINE__, __func__);
        ret = 0;
    } else if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = 0;
    }
    Py_XDECREF(result);

    PROF_END(PROF_TEAM_SWITCH_ATTEMPT, t_work);
    DispatcherRelease(gstate);
    return ret;
}

void WeaponFiredDispatcher(int client_id, int weapon) {
    if (!weapon_fired_handler) {
        return; // Nothing has hooked the event.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        PyLong_FromLong(weapon),
    };
    PyObject* result = CallHandler(&weapon_fired_handler, argv, 2);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_WEAPON_FIRED, t_work);
    DispatcherRelease(gstate);
}

int UserinfoDispatcher(int client_id, const char* infostring, char* out, size_t out_size) {
    int ret = 1; // Unchanged unless a handler says otherwise.
    if (!userinfo_handler) {
        return ret;
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        FromEngine(infostring),
    };
    handler_status_t status;
    PyObject* result = CallHandlerStatus(&userinfo_handler, argv, 2, &status);

    if (status == HANDLER_BAD_ARGS) {
        // The infostring is whatever the client sent. Drop the change rather than apply one
        // no handler was able to see. See FromEngine.
        DebugError("could not describe the userinfo to Python; dropping the change.\n",
                   __FILE__, __LINE__, __func__);
        ret = 0;
    } else if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    } else if (PyBool_Check(result) && result == Py_False) {
        ret = 0;
    } else if (PyUnicode_Check(result)) {
        Py_ssize_t length = 0;
        const char* utf8  = PyUnicode_AsUTF8AndSize(result, &length);
        if (!utf8) {
            PyErr_Clear();
        } else if ((size_t)length >= out_size) {
            // Silently truncating would hand the engine a different infostring than the
            // handler asked for, so the change is dropped and the original stands.
            DebugPrint("WARNING: a userinfo handler returned %zd bytes, over the %zu-byte "
                       "limit. Ignoring it.\n", length, out_size - 1);
        } else {
            memcpy(out, utf8, (size_t)length);
            out[length] = '\0';
            ret         = 2;
        }
    }
    Py_XDECREF(result);

    PROF_END(PROF_USERINFO, t_work);
    DispatcherRelease(gstate);
    return ret;
}

void ObjectiveDispatcher(int client_id, int kind, int count) {
    if (!objective_handler) {
        return; // No registered handler.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(client_id),
        PyLong_FromLong(kind),
        PyLong_FromLong(count),
    };
    PyObject* result = CallHandler(&objective_handler, argv, 3);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_OBJECTIVE, t_work);
    DispatcherRelease(gstate);
}

/*
 * Damage. Python arms and disarms the slot as the event gains and loses hooks. My_G_Damage
 * tests it too.
 */
void DamageDispatcher(int target_id, int attacker_id, int damage, int dflags, int mod) {
    if (!damage_handler) {
        return; // Nothing has hooked the event.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        PyLong_FromLong(target_id),
        PyLong_FromLong(attacker_id),
        PyLong_FromLong(damage),
        PyLong_FromLong(dflags),
        PyLong_FromLong(mod),
    };
    PyObject* result = CallHandler(&damage_handler, argv, 5);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_DAMAGE, t_work);
    DispatcherRelease(gstate);
}

/*
 * A cvar whose live value a write actually changed. Gated like damage, and My_Cvar_Set2
 * tests the slot before copying the old value out.
 */
void CvarChangedDispatcher(const char* name, const char* old_value, const char* new_value) {
    if (!cvar_changed_handler) {
        return; // Nothing has hooked the event.
    }

    PROF_BEGIN(t_gil);
    PyGILState_STATE gstate = PyGILState_Ensure();
    PROF_END(PROF_GIL_WAIT, t_gil);
    PROF_BEGIN(t_work);

    PyObject* argv[] = {
        FromEngine(name),
        FromEngine(old_value),
        FromEngine(new_value),
    };
    PyObject* result = CallHandler(&cvar_changed_handler, argv, 3);

    if (result == NULL) {
        DebugError("CallHandler() returned NULL.\n",
                   __FILE__, __LINE__, __func__);
    }
    Py_XDECREF(result);

    PROF_END(PROF_CVAR_CHANGED, t_work);
    DispatcherRelease(gstate);
}
