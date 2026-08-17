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

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "common.h"
#include "console_command.h"
#include "engine/quake_common.h"

// ====================================================================
//                     COMMANDS THAT RELOAD QAGAME
// ====================================================================

#define COMMAND_NAME_SIZE 64

// Every console command in qzeroded that releases the game module, read off the callers of
// SV_SpawnServer, SV_RestartGameProgs and SV_Shutdown. `startRandomMap` reaches one indirectly,
// handing a `map` command to Cbuf_ExecuteText with EXEC_NOW. `vstr` and `exec` are absent by
// design: both push their text through Cbuf_InsertText, so it runs from Com_Frame anyway.
static const char *const reload_names[] = {
    "map", "devmap", "arena", "map_restart", "startRandomMap", "killserver", "quit",
};

// The engine registers `map` ahead of its aliases, so the handler is known by the time the
// aliases come through and a name a later QL build hangs off the same handler is covered without
// editing the table above. The names are the floor; this only widens it.
#define RELOAD_HANDLERS_MAX 8
#define RELOAD_ALIASES_MAX 16

static void *reload_handlers[RELOAD_HANDLERS_MAX];
static int reload_handlers_count;
static char reload_aliases[RELOAD_ALIASES_MAX][COMMAND_NAME_SIZE];
static int reload_aliases_count;

static qboolean IsReloadName(const char *name) {
    for (size_t i = 0; i < sizeof(reload_names) / sizeof(reload_names[0]); i++) {
        if (!strcasecmp(name, reload_names[i])) {
            return qtrue;
        }
    }

    return qfalse;
}

void ConsoleCommand_NoteRegistration(const char *name, void *function) {
    if (!name || !function || strlen(name) >= COMMAND_NAME_SIZE) {
        return;
    }

    if (IsReloadName(name)) {
        for (int i = 0; i < reload_handlers_count; i++) {
            if (reload_handlers[i] == function) {
                return;
            }
        }
        if (reload_handlers_count < RELOAD_HANDLERS_MAX) {
            reload_handlers[reload_handlers_count++] = function;
        }
        return;
    }

    for (int i = 0; i < reload_handlers_count; i++) {
        if (reload_handlers[i] != function) {
            continue;
        }
        if (reload_aliases_count < RELOAD_ALIASES_MAX) {
            strcpy(reload_aliases[reload_aliases_count++], name);
            DebugPrint("%s shares a handler with a map command, so it goes to the command "
                       "buffer too\n", name);
        }
        return;
    }
}

// Cmd_TokenizeString's separator test, verified at qzeroded 0x420840: everything from 0x01 to
// 0x20 ends a token, and only NUL ends the line.
static qboolean is_separator(char c) {
    return (unsigned char)(c - 1) < 0x20 ? qtrue : qfalse;
}

// argv[0], parsed the way Cmd_TokenizeString parses it, all from qzeroded 0x420840: every byte
// from 0x01 to 0x20 is whitespace; a double quote opens a token that runs to the next quote, with
// the quotes stripped; an unquoted token also ends at a quote, at "//" and at the start of a
// "/ *" comment. A name this misreads is not in the table above, and runs in place.
static void CommandName(const char *cmd, char *out, size_t size) {
    size_t n = 0;

    while (*cmd && is_separator(*cmd)) {
        cmd++;
    }

    if (*cmd == '"') {
        cmd++;
        while (*cmd && *cmd != '"' && n + 1 < size) {
            out[n++] = *cmd++;
        }
    } else {
        while (*cmd && !is_separator(*cmd) && *cmd != '"' && n + 1 < size) {
            if (*cmd == '/' && (cmd[1] == '/' || cmd[1] == '*')) {
                break;
            }
            out[n++] = *cmd++;
        }
    }

    out[n] = '\0';
}

static qboolean ReloadsGameModule(const char *cmd) {
    char name[COMMAND_NAME_SIZE];
    CommandName(cmd, name, sizeof(name));

    // Nothing usable came out, so the line is a comment, a stray quote or whitespace alone. The
    // buffer is safe for every command there is, so send it there.
    if (!name[0]) {
        return qtrue;
    }

    if (IsReloadName(name)) {
        return qtrue;
    }

    for (int i = 0; i < reload_aliases_count; i++) {
        if (!strcasecmp(name, reload_aliases[i])) {
            return qtrue;
        }
    }

    return qfalse;
}

// ====================================================================
//                        THE OFF-THREAD QUEUE
// ====================================================================

// Deep enough for the bursts that happen: a shuffle issues a `put` per player.
#define CMDQ_MAX 128

// Cbuf_AddText appends the bytes and nothing else: no terminator, no separator, so two commands
// with nothing between them arrive as one line and each carries its own newline below. It also
// drops the whole string once cmd_text is full, and cmd_text is small, so the drain moves a
// budget's worth per frame and leaves the rest for the next one.
#define CMDQ_BYTES_PER_FRAME 2048

static char cmdq[CMDQ_MAX][MAX_STRING_CHARS];
static unsigned cmdq_head, cmdq_tail;
static pthread_mutex_t cmdq_lock = PTHREAD_MUTEX_INITIALIZER;

// Read every frame, so the usual empty case never takes the lock.
static atomic_uint cmdq_pending;

static void HandToBuffer(const char *cmd, size_t len) {
    if (!Cbuf_ExecuteText) {
        return;
    }

    char text[MAX_STRING_CHARS + 1]; // the newline, on top of what Run accepted
    memcpy(text, cmd, len);
    text[len]     = '\n';
    text[len + 1] = '\0';

    Cbuf_ExecuteText(EXEC_APPEND, text);
}

static qboolean Enqueue(const char *cmd, size_t len) {
    // The lock is really only against the drain, since every producer holds the GIL and so
    // is already serialised against the others.
    pthread_mutex_lock(&cmdq_lock);
    qboolean queued = qfalse;
    if (cmdq_head - cmdq_tail < CMDQ_MAX) {
        memcpy(cmdq[cmdq_head % CMDQ_MAX], cmd, len + 1);
        cmdq_head++;
        // Under the lock, so the count can never drift from the queue it describes.
        atomic_fetch_add_explicit(&cmdq_pending, 1, memory_order_relaxed);
        queued = qtrue;
    }
    pthread_mutex_unlock(&cmdq_lock);

    if (!queued) {
        DebugPrint("command queue full at %d; dropped: %s\n", CMDQ_MAX, cmd);
    }

    return queued;
}

void ConsoleCommand_Drain(void) {
    if (!atomic_load_explicit(&cmdq_pending, memory_order_relaxed)) {
        return;
    }

    size_t sent = 0;
    while (sent < CMDQ_BYTES_PER_FRAME) {
        char cmd[MAX_STRING_CHARS];
        size_t len = 0;

        pthread_mutex_lock(&cmdq_lock);
        qboolean have = (cmdq_tail != cmdq_head);
        if (have) {
            char *queued = cmdq[cmdq_tail % CMDQ_MAX];
            len          = strlen(queued);
            memcpy(cmd, queued, len + 1);
            sent += len + 1; // as HandToBuffer will send it, newline included
            cmdq_tail++;
            atomic_fetch_sub_explicit(&cmdq_pending, 1, memory_order_relaxed);
        }
        pthread_mutex_unlock(&cmdq_lock);

        if (!have) {
            break;
        }

        // Outside the lock: a Python worker queueing the next one shouldn't wait on the
        // engine, and the command buffer can reach arbitrary command handlers.
        HandToBuffer(cmd, len);
    }
}

// ====================================================================
//                              RUNNING ONE
// ====================================================================

// A console_print handler that issues a command comes back here through My_Com_Printf. Past this
// depth the command is queued instead, so a cycle costs a frame each time round rather than the
// stack.
#define EXEC_MAX_DEPTH 4

static int exec_depth; // game thread only, as is everything it guards

// The outermost in-place command, copied rather than pointed at. Com_Error longjmps out of
// Cmd_ExecuteString and abandons the Python frame that owns the argument, so a pointer into it
// outlives its owner. A depth left standing by the same longjmp only costs the in-place path:
// past EXEC_MAX_DEPTH commands are queued, which still runs them. A name left standing is what
// the two backstops in hooks.c read, so that one gets cleared explicitly.
static char executing_cmd[MAX_STRING_CHARS];

// What the engine last tokenised. Cmd_TokenizeString copies its argument into an 8192-byte buffer
// of its own (Q_strncpyz(cmd_cmd, text_in, 0x2000) at qzeroded 0x420840), so this has to be the
// same size; saving less would restore a line the engine reads as a different argv.
#define TOKENIZED_MAX 8192
static char last_tokenized[TOKENIZED_MAX];

void ConsoleCommand_NoteTokenized(const char *text) {
    if (!text) {
        last_tokenized[0] = '\0';
        return;
    }

    snprintf(last_tokenized, sizeof(last_tokenized), "%s", text);
}

const char *ConsoleCommand_Executing(void) {
    return executing_cmd[0] ? executing_cmd : NULL;
}

void ConsoleCommand_ForgetExecuting(void) {
    executing_cmd[0] = '\0';
}

qboolean ConsoleCommand_Run(const char *cmd) {
    if (!cmd) {
        return qfalse;
    }

    // Cbuf_Execute isolates at most 1023 characters per command (qzeroded 0x420c10), so a longer
    // one arrives at the deferred path as a different command. The tokeniser itself takes 8192,
    // but the limit is the same whichever path a command takes. The dispatcher buffers in
    // python_dispatchers.c are 4096; see the note there.
    size_t len = strlen(cmd);
    if (len == 0 || len >= MAX_STRING_CHARS) {
        return qfalse;
    }

    if (!OnGameThread()) {
        return Enqueue(cmd, len);
    }

    // Straight to the command buffer, which the engine empties from Com_Frame with none of
    // our frames and none of qagame's left on the stack. See console_command.h.
    if (ReloadsGameModule(cmd)) {
        HandToBuffer(cmd, len);
        return qtrue;
    }

    if (exec_depth >= EXEC_MAX_DEPTH || !Cmd_ExecuteString) {
        return Enqueue(cmd, len);
    }

    // Whatever the engine was part-way through, put back afterwards. We are almost always called
    // from inside a command handler that is still holding argv, and Cmd_ExecuteString below
    // overwrites the tokeniser out from under it. Saved at every depth, the outermost included.
    char saved[TOKENIZED_MAX];
    snprintf(saved, sizeof(saved), "%s", last_tokenized);

    if (!exec_depth) {
        snprintf(executing_cmd, sizeof(executing_cmd), "%s", cmd);
    }
    exec_depth++;

    Cmd_ExecuteString(cmd);

    exec_depth--;
    if (!exec_depth) {
        executing_cmd[0] = '\0';
    }

    if (Cmd_TokenizeString) {
        // Cmd_TokenizeString is the trampoline, so this does not reach My_Cmd_TokenizeString and
        // would leave last_tokenized holding the command we just ran. The next in-place command
        // under the same engine handler would then restore that instead of the handler's argv.
        ConsoleCommand_NoteTokenized(saved);
        Cmd_TokenizeString(saved);
    }

    return qtrue;
}
