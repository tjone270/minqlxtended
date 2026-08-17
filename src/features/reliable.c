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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "reliable.h"

/* See reliable.h for what this guards against and why. */

#define RELIABLE_RING     MAX_RELIABLE_COMMANDS // 64 slots per client, engine-fixed
#define RELIABLE_CMD_MAX  1022                  // SV_SendServerCommand silently drops >= 1023
#define RELIABLE_QUEUE    128                   // entries; beyond this we stop pacing and send straight through
#define PRINT_PAYLOAD_MAX 980                   // leaves room for the print "..."\n wrapper

// How many frames an entry may be held before it goes out regardless of the backlog. Anything
// held this long is waiting on a client whose acknowledge has stopped advancing, and unbounded
// that head entry blocks every other client's commands behind it.
#define RELIABLE_MAX_HOLD 40

typedef struct {
    int slot; // -1 for a broadcast
    unsigned frame; // rel_frame when it was queued
    char cmd[RELIABLE_CMD_MAX + 1];
} rel_entry_t;

static rel_entry_t rel_queue[RELIABLE_QUEUE];
static int rel_head, rel_count;

static unsigned rel_frame; // counts Reliable_Flush calls; only differences are used
static int rel_sent_this_frame;
static int rel_warned; // one "we had to pace" line per map, however many bursts

static cvar_t* qlx_reliableGuard;
static cvar_t* qlx_reliableWatermark;
static cvar_t* qlx_reliableBurst;

static struct {
    unsigned queued;   // commands held back at least one frame
    unsigned merged;   // broadcast prints folded into a preceding batch
    unsigned bypassed; // sent straight through, unpaced, because the queue was full
    int worst_backlog; // deepest reliableSequence - reliableAcknowledge ever seen
    int worst_slot;
    // Seeded here as well as in Reliable_Reset: a "qlx_reliable" from a config that runs
    // before the first map spawn would otherwise report the zero-initialised slot 0.
} rel_stats = {.worst_slot = -1};

void Reliable_Init(void) {
    if (!Cvar_Get) {
        return;
    }
    qlx_reliableGuard     = Cvar_Get("qlx_reliableGuard", "1", CVAR_ARCHIVE);
    qlx_reliableWatermark = Cvar_Get("qlx_reliableWatermark", "32", CVAR_ARCHIVE);
    qlx_reliableBurst     = Cvar_Get("qlx_reliableBurst", "8", CVAR_ARCHIVE);
}

// Deferring starts here. Kept well below the ring size because the client's cgame is
// always further behind than reliableAcknowledge suggests.
static int watermark(void) { return cvar_clamped(qlx_reliableWatermark, 32, 8, RELIABLE_RING - 8); }

// Commands allowed straight out per frame before pacing starts, and the number released
// per frame once it has.
static int burst(void) { return cvar_clamped(qlx_reliableBurst, 8, 1, 32); }

static qboolean enabled(void) {
    return (qlx_reliableGuard && qlx_reliableGuard->integer && svs && svs->clients && sv_maxclients) ? qtrue : qfalse;
}

// Only output the client merely displays is eligible for pacing. Anything its state machine reads
// (cs, bcs0/1/2, tinfo, scores, disconnect, map_restart) and anything not listed here goes out
// untouched.
static qboolean is_cosmetic(const char* cmd) {
    static const char* const words[] = {
        "print", "cp", "pcp", "chat", "tchat", "playSound", "playMusic", "clearSounds", "stopMusic",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (cmd_word_is(cmd, words[i])) {
            return qtrue;
        }
    }
    return qfalse;
}

// Splits print "text" into its payload so consecutive ones can be concatenated. Refuses anything
// it cannot rebuild byte for byte: an embedded quote, which the engine's tokeniser would treat as
// the end of the argument, or trailing junk after the closing quote.
static qboolean print_payload(const char* cmd, const char** out, size_t* len) {
    if (strncmp(cmd, "print \"", 7)) {
        return qfalse;
    }
    const char* start = cmd + 7;
    const char* end   = strchr(start, '"');
    if (!end) {
        return qfalse;
    }
    for (const char* p = end + 1; *p; p++) {
        if (*p != '\n' && *p != ' ' && *p != '\t' && *p != '\r') {
            return qfalse; // not a plain print; leave it alone
        }
    }
    *out = start;
    *len = (size_t)(end - start);
    return qtrue;
}

static int backlog_of(const client_t* cl) {
    return cl->reliableSequence - cl->reliableAcknowledge;
}

// How close to the ring the worst affected client is. For a broadcast this mirrors the engine's
// delivery filter (SV_SendServerCommand skips clients below CS_PRIMED, and a CS_PRIMED client
// receives nothing but "chat "), so a client that will never see the command cannot hold it back
// for everyone else. worst_slot, when given, receives the client the returned depth belongs to.
static int backlog_for(int slot, const char* cmd, int* worst_slot) {
    if (worst_slot) {
        *worst_slot = slot;
    }
    if (!svs || !svs->clients || !sv_maxclients) {
        return 0;
    }

    if (slot >= 0) {
        return (slot < sv_maxclients->integer) ? backlog_of(&svs->clients[slot]) : 0;
    }

    qboolean is_chat = cmd_word_is(cmd, "chat");
    int worst        = 0;
    for (int i = 0; i < sv_maxclients->integer; i++) {
        const client_t* cl = &svs->clients[i];
        if (cl->state <= CS_CONNECTED || (cl->state == CS_PRIMED && !is_chat)) {
            continue;
        }
        int b = backlog_of(cl);
        if (b > worst) {
            worst = b;
            if (worst_slot) {
                *worst_slot = i;
            }
        }
    }
    return worst;
}

static void note_backlog(int slot, int backlog) {
    if (backlog > rel_stats.worst_backlog) {
        rel_stats.worst_backlog = backlog;
        rel_stats.worst_slot    = slot;
    }
}

static void send_now(int slot, const char* cmd) {
    client_t* cl = NULL;
    if (slot >= 0) {
        // Same guard backlog_for opens with. Unreachable today, since a queued entry
        // implies enabled(), but the flush can run with pacing disabled.
        if (!svs || !svs->clients || !sv_maxclients || slot >= sv_maxclients->integer) {
            return;
        }
        cl = &svs->clients[slot];
        if (cl->state <= CS_CONNECTED) {
            return; // slot emptied or restarted while the command sat in the queue
        }
    }
    // The real SV_SendServerCommand, going around our own hook. The server_command event
    // already fired for this text when it was first submitted.
    SV_SendServerCommand(cl, "%s", cmd);
    rel_sent_this_frame++;
}

static qboolean enqueue(int slot, const char* cmd, int backlog) {
    if (rel_count >= RELIABLE_QUEUE) {
        // Nothing is discarded: qfalse makes Reliable_Intercept decline the command and
        // My_SV_SendServerCommand sends it itself, ahead of everything still queued. Pacing
        // and submission order are both lost at this depth.
        rel_stats.bypassed++;
        return qfalse;
    }
    rel_entry_t* e = &rel_queue[(rel_head + rel_count) % RELIABLE_QUEUE];
    e->slot        = slot;
    e->frame       = rel_frame;
    snprintf(e->cmd, sizeof(e->cmd), "%s", cmd);
    rel_count++;
    rel_stats.queued++;

    if (!rel_warned) {
        rel_warned = 1;
        // The backlog measured for *this* command. rel_stats' running worst would name a
        // client with nothing to do with this burst.
        if (slot < 0) {
            ENGINE_PRINTF(DEBUG_PRINT_PREFIX "pacing reliable commands: a broadcast found a client %d/%d "
                                          "commands behind. See \"qlx_reliable\".\n",
                       backlog, RELIABLE_RING);
        } else {
            ENGINE_PRINTF(DEBUG_PRINT_PREFIX "pacing reliable commands: client %d is %d/%d commands behind. "
                                          "See \"qlx_reliable\".\n",
                       slot, backlog, RELIABLE_RING);
        }
    }
    return qtrue;
}

qboolean Reliable_Intercept(client_t* cl, const char* cmd) {
    if (!enabled() || !cmd || !cmd[0]) {
        return qfalse;
    }

    int slot = cl ? (int)(cl - svs->clients) : -1;
    if (slot < -1 || slot >= sv_maxclients->integer) {
        return qfalse; // not one of ours; let the engine deal with it
    }

    // The engine drops anything this long outright, and queueing it would truncate it into
    // something the engine would then have accepted.
    if (strlen(cmd) > RELIABLE_CMD_MAX) {
        return qfalse;
    }

    // Measured for every command, including the ones we can't pace, so "qlx_reliable" still
    // shows how deep a configstring flood got.
    int worst_slot = slot;
    int backlog    = backlog_for(slot, cmd, &worst_slot);
    note_backlog(worst_slot, backlog);

    if (!is_cosmetic(cmd)) {
        return qfalse;
    }

    // Nothing waiting and plenty of ring left: the common case, sent with no added latency.
    // Otherwise it queues, keeping submission order. The counter covers commands let through
    // as well as ones the flush emits, so the cap is on everything leaving in a frame.
    if (!rel_count && rel_sent_this_frame < burst() && backlog < watermark()) {
        rel_sent_this_frame++;
        return qfalse;
    }

    return enqueue(slot, cmd, backlog);
}

void Reliable_Flush(void) {
    rel_frame++;
    rel_sent_this_frame = 0;
    if (!rel_count) {
        return;
    }

    // Turned off mid-match with output still held. Hand it all back so nothing strands.
    qboolean paced = enabled();
    int budget     = paced ? burst() : rel_count;
    char batch[RELIABLE_CMD_MAX + 1];
    char payload[PRINT_PAYLOAD_MAX + 1];

    while (rel_count > 0 && budget > 0) {
        rel_entry_t* head = &rel_queue[rel_head];
        if (paced && backlog_for(head->slot, head->cmd, NULL) >= watermark() &&
            (rel_frame - head->frame) < RELIABLE_MAX_HOLD) {
            break; // still too deep; try again next frame
        }

        // Off the queue before anything else can run, and by value. send_now below reaches
        // Com_Printf, which is hooked, so a console_print handler can queue more or drop a
        // client, and Reliable_ClientGone then compacts this queue. Advancing the head afterwards
        // would be advancing past an entry already compacted away. See reliable.h.
        rel_entry_t entry = *head;
        rel_head          = (rel_head + 1) % RELIABLE_QUEUE;
        rel_count--;
        budget--;

        const char* text = entry.cmd;
        const char* p;
        size_t plen, used = 0;

        // Fold as many consecutive broadcast prints as fit into one command. Only
        // newline-terminated payloads are merged, so no two lines are ever run together.
        // Nothing here can re-enter, so the queue is still ours to walk.
        if (entry.slot == -1 && print_payload(entry.cmd, &p, &plen) && plen <= PRINT_PAYLOAD_MAX) {
            memcpy(payload, p, plen);
            used = plen;
            while (used && payload[used - 1] == '\n' && rel_count > 0) {
                rel_entry_t* next = &rel_queue[rel_head];
                if (next->slot != -1 || !print_payload(next->cmd, &p, &plen) || used + plen > PRINT_PAYLOAD_MAX) {
                    break;
                }
                memcpy(payload + used, p, plen);
                used += plen;
                rel_head = (rel_head + 1) % RELIABLE_QUEUE;
                rel_count--;
                rel_stats.merged++;
            }
            payload[used] = '\0';
            snprintf(batch, sizeof(batch), "print \"%s\"\n", payload);
            text = batch;
        }

        send_now(entry.slot, text);
    }
}

void Reliable_ClientGone(int slot) {
    if (!rel_count) {
        return;
    }
    // Compact in place; broadcasts and other clients' commands keep their order.
    int kept = 0;
    for (int i = 0; i < rel_count; i++) {
        rel_entry_t* src = &rel_queue[(rel_head + i) % RELIABLE_QUEUE];
        if (src->slot == slot) {
            continue;
        }
        rel_entry_t* dst = &rel_queue[(rel_head + kept) % RELIABLE_QUEUE];
        if (dst != src) {
            *dst = *src;
        }
        kept++;
    }
    rel_count = kept;
}

void Reliable_Reset(void) {
    rel_head = rel_count = rel_sent_this_frame = 0;
    rel_warned = 0;
    memset(&rel_stats, 0, sizeof(rel_stats));
    rel_stats.worst_slot = -1;
}

void Reliable_Status(reliable_status_t* out) {
    out->enabled       = enabled() == qtrue;
    out->watermark     = watermark();
    out->burst         = burst();
    out->waiting       = rel_count;
    out->queued        = rel_stats.queued;
    out->merged        = rel_stats.merged;
    out->bypassed      = rel_stats.bypassed;
    // The chat variant of the delivery filter, the most inclusive one, so the number
    // is honest for the widest audience a plugin can address.
    out->backlog       = backlog_for(-1, "chat", NULL);
    out->worst_backlog = rel_stats.worst_backlog;
    out->worst_slot    = rel_stats.worst_slot;
}

void Reliable_Report(void) {
    ENGINE_PRINTF("Reliable command guard: %s, watermark %d of %d, burst %d/frame.\n",
               (qlx_reliableGuard && qlx_reliableGuard->integer) ? "on" : "off", watermark(), RELIABLE_RING, burst());
    ENGINE_PRINTF("Since the last map: %u queued, %u merged into a batch, %u sent unpaced. "
               "Deepest backlog %d (client %d).\n",
               rel_stats.queued, rel_stats.merged, rel_stats.bypassed, rel_stats.worst_backlog, rel_stats.worst_slot);
    ENGINE_PRINTF("Waiting now: %d.\n", rel_count);

    if (!svs || !svs->clients || !sv_maxclients) {
        return;
    }
    ENGINE_PRINTF("slot  state  backlog  name\n");
    for (int i = 0; i < sv_maxclients->integer; i++) {
        const client_t* cl = &svs->clients[i];
        if (cl->state == CS_FREE) {
            continue;
        }
        ENGINE_PRINTF("%4d  %5d  %7d  %s\n", i, cl->state, backlog_of(cl), cl->name);
    }
}
