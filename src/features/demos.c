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

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "demos.h"
#include "profile.h"
#include "engine/quake_common.h"

extern serverStatic_t *svs; // defined in dllmain.c

#define SVC_EOF 8
#define MAX_DEMO_CLIENTS 64                // QL MAX_CLIENTS
#define DEMO_RING_SIZE (16u * 1024 * 1024) // power of two

typedef enum {
    DEMO_REC_OPEN = 1,  // payload: null-terminated file path
    DEMO_REC_BLOCK,     // payload: message bytes incl. Huffman svc_EOF
    DEMO_REC_CLOSE,     // no payload
    DEMO_REC_CLOSE_ALL, // no payload, slot ignored
    DEMO_REC_SHUTDOWN,  // no payload; writer finalises every open demo and exits
} demo_rec_type_t;

typedef struct {
    int32_t type; // demo_rec_type_t
    int32_t slot;
    int32_t seq;  // BLOCK only: netchan outgoingSequence
    uint32_t len; // payload bytes following this header
} demo_rec_hdr_t; // 16 bytes

typedef enum {
    DEMO_THREAD_STOPPED = 0,
    DEMO_THREAD_RUNNING,
    DEMO_THREAD_STOPPING, // writer still draining
} demo_thread_state_t;

static unsigned char demo_ring[DEMO_RING_SIZE];
static uint64_t demo_head; // advanced by the game thread.
static uint64_t demo_tail; // advanced by the writer thread.
static pthread_mutex_t demo_lock             = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t demo_cond              = PTHREAD_COND_INITIALIZER;
static demo_thread_state_t demo_thread_state = DEMO_THREAD_STOPPED;

static demo_thread_state_t demo_state_cached = DEMO_THREAD_STOPPED;
static uint8_t demo_active[MAX_DEMO_CLIENTS]; // slot has an open segment.
static unsigned char demo_scratch[MAX_NETCHAN_MSGLEN + 64];

// Per-slot override of sv_demoRecord set from Python: 0 follows the cvar, 1 always
// records, -1 never does. Game thread only, same as demo_active.
static int8_t demo_request[MAX_DEMO_CLIENTS];
static int demo_forced_on; // number of slots currently set to 1.

static char demo_path[MAX_DEMO_CLIENTS][512];
static uint32_t demo_gen[MAX_DEMO_CLIENTS]; // bumped per OPEN, to match completions up.

typedef struct {
    FILE *fh;
    char path[512];
    long blocks;  // blocks written to this segment (gamestate counts as 1.)
    long bytes;   // bytes written to the file so far.
    uint32_t gen; // demo_gen[slot] of the OPEN this segment came from.
} demo_client_t;
static demo_client_t demos[MAX_DEMO_CLIENTS];
// The failure paths publish the ".part" name, so the completion field has to hold it.
// Truncating there would report a file that is not on disk.
_Static_assert(sizeof(((demo_finished_t *)0)->path) >= sizeof(demos[0].path) + 8,
               "demo_finished_t.path must hold a segment path plus the .part suffix");
static unsigned char writer_scratch[MAX_NETCHAN_MSGLEN + 64];

// Finalised segments waiting to be reported to Python: writer pushes, game thread pops,
// both under demo_lock. Sized to absorb a whole-server finalise, 64 slots at once.
#define DEMO_DONE_MAX (MAX_DEMO_CLIENTS + 16)
static demo_finished_t demo_done[DEMO_DONE_MAX];
static unsigned demo_done_head, demo_done_tail;
static unsigned demo_done_dropped;
// Queued + dropped, so the frame hook can bail without touching demo_lock at all.
static atomic_uint demo_done_pending;

// Records queued and records fully handled, so Demo_DrainFinalise can wait for one to have taken
// effect rather than merely been read out of the ring. demo_seq_put is written only by the game
// thread and demo_seq_done only by the writer, so a release store paired with the other side's
// acquire load is enough.
static atomic_ullong demo_seq_put;
static atomic_ullong demo_seq_done;

// How long the shutdown finalise waits for the writer before giving up and leaving the
// segments as .part. Bounded so a wedged writer cannot hang the server's exit.
#define DEMO_DRAIN_TIMEOUT_MS 2000

static cvar_t *sv_demoRecord;       // 0 = off, 1 = record every connected client
static cvar_t *sv_demoDir;          // output subdirectory, under fs_homepath
static cvar_t *sv_demoNameFormat;   // filename template: %date %slot %name
static cvar_t *sv_demoCleanupParts; // remove leftover .part files at startup
static cvar_t *fs_homepath;

static const int32_t demo_eof[2] = {-1, -1};

static void demo_sanitise(char *dst, size_t n, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < n; i++) {
        char c = src[i];
        if (c == '^' && src[i + 1]) { // colour code: skip '^' and the following char.
            i++;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            dst[j++] = c;
        } else if (c == ' ') {
            dst[j++] = '_';
        }
    }
    if (j == 0) {
        dst[j++] = 'x';
    }
    dst[j] = '\0';
}

static void demo_mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void demo_build_name(char *out, size_t n, int slot, client_t *client) {
    const char *subdir = (sv_demoDir && sv_demoDir->string[0]) ? sv_demoDir->string : "demos";

    time_t now = time(NULL);
    struct tm tmv;
    char date[32] = "00000000-000000";
    if (localtime_r(&now, &tmv)) {
        strftime(date, sizeof(date), "%Y%m%d-%H%M%S", &tmv);
    }

    char name[64];
    demo_sanitise(name, sizeof(name), client->name);

    const char *fmt = (sv_demoNameFormat && sv_demoNameFormat->string[0]) ? sv_demoNameFormat->string
                                                                          : "%date_slot%slot_%name";
    char body[256];
    size_t o = 0;
    for (size_t i = 0; fmt[i] && o + 1 < sizeof(body);) {
        if (fmt[i] == '%') {
            const char *rep = NULL;
            char num[16];
            if (!strncmp(fmt + i + 1, "date", 4)) {
                rep = date;
                i += 5;
            } else if (!strncmp(fmt + i + 1, "name", 4)) {
                rep = name;
                i += 5;
            } else if (!strncmp(fmt + i + 1, "slot", 4)) {
                snprintf(num, sizeof(num), "%02d", slot);
                rep = num;
                i += 5;
            }
            if (rep) {
                for (const char *p = rep; *p && o + 1 < sizeof(body); p++) {
                    body[o++] = *p;
                }
                continue;
            }
        }
        body[o++] = fmt[i++];
    }
    body[o] = '\0';

    snprintf(out, n, "%s/%s/%s.dm_91", fs_homepath->string, subdir, body);
}

// Both ring copy helpers require demo_lock to be held and handle wraparound with a split copy.
static void ring_copy_in(uint64_t pos, const void *src, size_t n) {
    size_t off   = (size_t)(pos & (DEMO_RING_SIZE - 1));
    size_t first = DEMO_RING_SIZE - off;
    if (first > n) {
        first = n;
    }
    memcpy(demo_ring + off, src, first);
    memcpy(demo_ring, (const unsigned char *)src + first, n - first);
}

static void ring_copy_out(uint64_t pos, void *dst, size_t n) {
    size_t off   = (size_t)(pos & (DEMO_RING_SIZE - 1));
    size_t first = DEMO_RING_SIZE - off;
    if (first > n) {
        first = n;
    }
    memcpy(dst, demo_ring + off, first);
    memcpy((unsigned char *)dst + first, demo_ring, n - first);
}

// Caller holds demo_lock. Returns 0 on success, -1 if the record does not fit. The signal
// cannot wake the writer before the caller unlocks, so state may be published after it in
// the same critical section.
static int demo_ring_put_locked(const demo_rec_hdr_t *hdr, const void *payload) {
    size_t need = sizeof(*hdr) + hdr->len;

    if (DEMO_RING_SIZE - (demo_head - demo_tail) < need) {
        return -1;
    }
    ring_copy_in(demo_head, hdr, sizeof(*hdr));
    if (hdr->len) {
        ring_copy_in(demo_head + sizeof(*hdr), payload, hdr->len);
    }
    demo_head += need;
    // Under the lock and before the signal, so the writer can never bump demo_seq_done
    // past a put that has not been counted yet.
    atomic_store_explicit(&demo_seq_put, atomic_load_explicit(&demo_seq_put, memory_order_relaxed) + 1,
                          memory_order_relaxed);
    pthread_cond_signal(&demo_cond);
    return 0;
}

// Game thread only. Returns 0 on success, -1 if the record does not fit.
static int demo_ring_put(const demo_rec_hdr_t *hdr, const void *payload) {
    pthread_mutex_lock(&demo_lock);
    int rc = demo_ring_put_locked(hdr, payload);
    pthread_mutex_unlock(&demo_lock);
    return rc;
}

// d->path holds the final name; the segment is recorded into "<name>.part" until finalised.
static void demo_part_name(char *out, size_t n, const char *path) {
    snprintf(out, n, "%s.part", path);
}

// Queues a finished segment for the game thread to report. Must not block: on overflow we
// count the loss instead of stalling the writer.
static void writer_publish_done(int slot, uint32_t gen, const char *path, long bytes, int discarded,
                                int failed) {
    pthread_mutex_lock(&demo_lock);
    if (demo_done_head - demo_done_tail >= DEMO_DONE_MAX) {
        demo_done_dropped++;
    } else {
        demo_finished_t *f = &demo_done[demo_done_head % DEMO_DONE_MAX];
        f->slot            = slot;
        f->gen             = gen;
        f->discarded       = discarded;
        f->failed          = failed;
        f->bytes           = bytes;
        snprintf(f->path, sizeof(f->path), "%s", path);
        demo_done_head++;
    }
    // Under the lock, so the count can never drift from the queue it describes.
    atomic_fetch_add_explicit(&demo_done_pending, 1, memory_order_relaxed);
    pthread_mutex_unlock(&demo_lock);
}

static void writer_finalise(demo_client_t *d) {
    if (!d->fh) {
        return;
    }
    // The stream is 64 KB fully buffered, so on a full disk the last blocks are still in
    // stdio and fclose's return is the only sign.
    int bad = (fwrite(demo_eof, sizeof(demo_eof), 1, d->fh) != 1) || ferror(d->fh);
    d->bytes += (long)sizeof(demo_eof);
    bad |= (fclose(d->fh) != 0);
    d->fh = NULL;

    int slot = (int)(d - demos);

    char part[sizeof(d->path) + 8];
    demo_part_name(part, sizeof(part), d->path);
    if (bad) {
        // Leave it as .part and report the failure, matching writer_handle_block. The byte
        // count is what we handed to stdio, so it overstates what reached the disk.
        DebugPrint("demo: write failed finalising %s\n", part);
        writer_publish_done(slot, d->gen, part, d->bytes, 0, 1);
        return;
    }
    if (d->blocks <= 1) { // only the gamestate.
        unlink(part);
        DebugPrint("demo: discarded empty segment %s\n", d->path);
        writer_publish_done(slot, d->gen, d->path, d->bytes, 1, 0);
        return;
    }
    // Published only once the file is in its final place, so a handler never sees a
    // half-written .part. A failed rename leaves it under the .part name, so report that.
    if (rename(part, d->path)) {
        DebugPrint("demo: could not rename %s into place\n", part);
        writer_publish_done(slot, d->gen, part, d->bytes, 0, 1);
        return;
    }
    writer_publish_done(slot, d->gen, d->path, d->bytes, 0, 0);
}

static void writer_handle_open(int slot, uint32_t gen, const char *path) {
    demo_client_t *d = &demos[slot];
    writer_finalise(d); // just in case this slot's CLOSE was dropped.

    d->gen = gen;

    size_t plen = strlen(path);
    if (plen >= sizeof(d->path)) {
        DebugPrint("demo: path too long, not recording slot %d\n", slot);
        writer_publish_done(slot, gen, path, 0, 0, 1);
        return;
    }
    memcpy(d->path, path, plen + 1);

    char dir[512];
    memcpy(dir, d->path, plen + 1);
    char *sep = strrchr(dir, '/');
    if (sep) {
        *sep = '\0';
        demo_mkdir_p(dir);
    }

    char part[sizeof(d->path) + 8];
    demo_part_name(part, sizeof(part), d->path);
    d->fh = fopen(part, "wb");
    if (!d->fh) {
        DebugPrint("demo: could not open %s\n", part);
        writer_publish_done(slot, gen, part, 0, 0, 1);
        return;
    }
    setvbuf(d->fh, NULL, _IOFBF, 64 * 1024);
    d->blocks = 0;
    d->bytes  = 0;
    DebugPrint("demo: recording slot %d -> %s\n", slot, d->path);
}

static void writer_handle_block(int slot, int32_t seq, const unsigned char *data, uint32_t len) {
    demo_client_t *d = &demos[slot];
    if (!d->fh) {
        return; // dropped OPEN or earlier write error.
    }
    int32_t hdr[2] = {seq, (int32_t)len};
    if (fwrite(hdr, sizeof(hdr), 1, d->fh) != 1 || fwrite(data, 1, len, d->fh) != len) {
        DebugPrint("demo: write error on slot %d, closing\n", slot);
        fclose(d->fh); // left behind as .part to mark it incomplete.
        d->fh = NULL;
        // Report it so the game thread stops capturing this segment; without that it
        // keeps queueing blocks the writer will drop until the next gamestate.
        char part[sizeof(d->path) + 8];
        demo_part_name(part, sizeof(part), d->path);
        writer_publish_done(slot, d->gen, part, d->bytes, 0, 1);
        return;
    }
    d->blocks++;
    d->bytes += (long)sizeof(hdr) + (long)len;
}

static void *demo_writer_main(void *unused) {
    (void)unused;

    // Keep the engine's asynchronous signal handling on the main thread. SIGSEGV, SIGBUS,
    // SIGFPE and SIGILL stay unblocked, since blocking a fault the thread raises itself is
    // undefined, and SIGABRT with them so the engine still prints a backtrace.
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGSEGV);
    sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGFPE);
    sigdelset(&all, SIGILL);
    sigdelset(&all, SIGABRT);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    for (;;) {
        demo_rec_hdr_t hdr;
        uint32_t len;

        pthread_mutex_lock(&demo_lock);
        while (demo_head == demo_tail) {
            pthread_cond_wait(&demo_cond, &demo_lock);
        }
        ring_copy_out(demo_tail, &hdr, sizeof(hdr));
        len = hdr.len;
        if (len > sizeof(writer_scratch)) { // should be impossible.
            len = 0;
        }
        if (len) {
            ring_copy_out(demo_tail + sizeof(hdr), writer_scratch, len);
        }
        demo_tail += sizeof(hdr) + hdr.len;
        pthread_mutex_unlock(&demo_lock);

        if (hdr.type == DEMO_REC_SHUTDOWN) {
            for (int i = 0; i < MAX_DEMO_CLIENTS; i++) {
                writer_finalise(&demos[i]);
            }
            pthread_mutex_lock(&demo_lock);
            demo_thread_state = DEMO_THREAD_STOPPED;
            pthread_mutex_unlock(&demo_lock);
            DebugPrint("demo: writer thread stopped\n");
            return NULL;
        }
        // A malformed record is still a record that has been consumed, so it has to be
        // counted like any other or Demo_DrainFinalise would wait for it forever.
        if (len == hdr.len && hdr.slot >= 0 && hdr.slot < MAX_DEMO_CLIENTS) {
            switch (hdr.type) {
            case DEMO_REC_OPEN:
                if (len > 0) {
                    writer_scratch[len - 1] = '\0';
                    writer_handle_open(hdr.slot, (uint32_t)hdr.seq, (const char *)writer_scratch);
                }
                break;
            case DEMO_REC_BLOCK:
                writer_handle_block(hdr.slot, hdr.seq, writer_scratch, len);
                break;
            case DEMO_REC_CLOSE:
                writer_finalise(&demos[hdr.slot]);
                break;
            case DEMO_REC_CLOSE_ALL:
                for (int i = 0; i < MAX_DEMO_CLIENTS; i++) {
                    writer_finalise(&demos[i]);
                }
                break;
            }
        }

        // Release, so a waiter that sees this count has also seen the finalise behind it.
        atomic_store_explicit(&demo_seq_done,
                              atomic_load_explicit(&demo_seq_done, memory_order_relaxed) + 1,
                              memory_order_release);
    }
}

// An explicit per-slot request wins over the global cvar.
static int demo_slot_wanted(int slot) {
    if (demo_request[slot] > 0) {
        return 1;
    }
    if (demo_request[slot] < 0) {
        return 0;
    }
    return sv_demoRecord->integer != 0;
}

static int demo_reconcile_thread(void) {
    // Needed if we're recording everyone, or any one slot was explicitly asked for.
    int enabled = sv_demoRecord->integer != 0 || demo_forced_on > 0;

    if (enabled && demo_state_cached == DEMO_THREAD_RUNNING) {
        return 1;
    }
    if (!enabled && demo_state_cached == DEMO_THREAD_STOPPED) {
        return 0;
    }

    if (demo_state_cached == DEMO_THREAD_STOPPING) {
        pthread_mutex_lock(&demo_lock);
        demo_state_cached = demo_thread_state;
        pthread_mutex_unlock(&demo_lock);
        if (demo_state_cached != DEMO_THREAD_STOPPED) {
            return 0; // writer still draining
        }
        if (!enabled) {
            return 0;
        }
    }

    if (enabled) { // cached state is STOPPED, so start the writer.
        pthread_t th;
        if (pthread_create(&th, NULL, demo_writer_main, NULL)) {
            DebugPrint("demo: could not start writer thread; recording disabled\n");
            return 0;
        }
        pthread_detach(th);
        pthread_mutex_lock(&demo_lock);
        demo_thread_state = DEMO_THREAD_RUNNING;
        pthread_mutex_unlock(&demo_lock);
        demo_state_cached = DEMO_THREAD_RUNNING;
        DebugPrint("demo: writer thread started\n");
        return 1;
    }

    // Disabled while RUNNING: stop the writer, and SHUTDOWN finalises all open demos.
    // STOPPING is published in the same critical section as the record. Released first, the
    // writer can store STOPPED before this lands, leaving STOPPING stuck with no writer
    // alive: recording dead for the process, and every shutdown waiting out the drain.
    demo_rec_hdr_t hdr = {DEMO_REC_SHUTDOWN, 0, 0, 0};
    pthread_mutex_lock(&demo_lock);
    int queued = demo_ring_put_locked(&hdr, NULL);
    if (queued == 0) {
        demo_thread_state = DEMO_THREAD_STOPPING;
    }
    pthread_mutex_unlock(&demo_lock);

    if (queued == 0) {
        demo_state_cached = DEMO_THREAD_STOPPING;
        memset(demo_active, 0, sizeof(demo_active));
    } // ring full: retried on a later message.
    return 0;
}

// sv_demoNameFormat can contain a '/', so segments are not necessarily all in one directory.
#define DEMO_SWEEP_DEPTH   8
#define DEMO_SWEEP_MIN_AGE 60 // seconds; see the note about other servers below.

static void demo_sweep_dir(const char* dir, int depth, time_t cutoff, unsigned* removed, unsigned* kept) {
    DIR* d = opendir(dir);
    if (!d) {
        return;
    }

    for (struct dirent* e = readdir(d); e; e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }

        char path[512];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path)) {
            continue;
        }

        struct stat st;
        if (lstat(path, &st)) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (depth + 1 < DEMO_SWEEP_DEPTH) {
                demo_sweep_dir(path, depth + 1, cutoff, removed, kept);
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue; // lstat, so a symlink is left alone rather than followed.
        }

        size_t len = strlen(e->d_name);
        if (len <= 5 || strcmp(e->d_name + len - 5, ".part")) {
            continue;
        }

        // Another server sharing this directory could have one of these open right now, and
        // an open segment gets written to constantly, so leave anything recent alone.
        if (st.st_mtime > cutoff) {
            (*kept)++;
            continue;
        }
        if (unlink(path)) {
            DebugPrint("demo: could not remove %s\n", path);
        } else {
            (*removed)++;
        }
    }

    closedir(d);
}

// The writer renames a segment into place once finalised, and every ordinary way out of the
// server finalises first, so a surviving .part belongs to a run that was killed outright or
// crashed with it open, and will never be completed.
static void demo_sweep_parts(void) {
    if (sv_demoCleanupParts && !sv_demoCleanupParts->integer) {
        return;
    }
    if (!fs_homepath || !fs_homepath->string[0]) {
        return;
    }

    const char* subdir = (sv_demoDir && sv_demoDir->string[0]) ? sv_demoDir->string : "demos";
    char dir[512];
    if ((size_t)snprintf(dir, sizeof(dir), "%s/%s", fs_homepath->string, subdir) >= sizeof(dir)) {
        return;
    }

    unsigned removed = 0, kept = 0;
    demo_sweep_dir(dir, 0, time(NULL) - DEMO_SWEEP_MIN_AGE, &removed, &kept);
    if (removed) {
        DebugPrint("demo: removed %u incomplete .part file(s) left by a previous run.\n", removed);
    }
    if (kept) {
        DebugPrint("demo: left %u .part file(s) written in the last %d seconds alone; another server may "
                   "still be recording them.\n",
                   kept, DEMO_SWEEP_MIN_AGE);
    }
}

void Demo_Init(void) {
    if (!Cvar_Get) {
        return;
    }
    sv_demoRecord       = Cvar_Get("sv_demoRecord", "0", CVAR_ARCHIVE);
    sv_demoDir          = Cvar_Get("sv_demoDir", "demos", CVAR_ARCHIVE);
    sv_demoNameFormat   = Cvar_Get("sv_demoNameFormat", "%date_slot%slot_%name", CVAR_ARCHIVE);
    sv_demoCleanupParts = Cvar_Get("sv_demoCleanupParts", "1", CVAR_ARCHIVE);
    fs_homepath         = Cvar_FindVar("fs_homepath");

    // Once per process: by the second G_InitGame the .part files on disk are our own. So
    // sv_demoCleanupParts only has a say at startup.
    static qboolean swept = qfalse;
    if (!swept) {
        swept = qtrue;
        demo_sweep_parts();
    }
}

// Split out of Demo_Capture so the profiler wrapper covers every early return.
static void Demo_CaptureBody(msg_t *msg, client_t *client) {
    if (!sv_demoRecord || !MSG_WriteBits || !svs || !svs->clients || !fs_homepath) {
        return;
    }
    if (!demo_reconcile_thread()) {
        return;
    }
    if (msg->cursize <= 0 || msg->cursize > MAX_NETCHAN_MSGLEN) {
        return;
    }

    long slot = client - svs->clients;
    if (slot < 0 || slot >= MAX_DEMO_CLIENTS) {
        return;
    }

    int seq          = client->netchan.outgoingSequence;
    int is_gamestate = (seq == client->gamestateMessageNum);

    demo_rec_hdr_t hdr = {0};
    hdr.slot           = (int32_t)slot;

    if (!demo_slot_wanted((int)slot)) {
        // Turned off mid-segment, so finalise now instead of waiting for a disconnect.
        if (demo_active[slot]) {
            hdr.type = DEMO_REC_CLOSE;
            demo_ring_put(&hdr, NULL);
            demo_active[slot] = 0;
        }
        return;
    }

    if (is_gamestate) {
        // A valid demo begins at a gamestate.
        if (demo_active[slot]) {
            hdr.type = DEMO_REC_CLOSE;
            demo_ring_put(&hdr, NULL); // OPEN should also finalise the writer-side.
            demo_active[slot] = 0;
        }
        char path[512];
        demo_build_name(path, sizeof(path), (int)slot, client);
        hdr.type = DEMO_REC_OPEN;
        hdr.len  = (uint32_t)strlen(path) + 1;
        hdr.seq  = (int32_t)(demo_gen[slot] + 1); // seq is unused for OPEN.
        if (demo_ring_put(&hdr, path) != 0) {
            return; // ring full; retry at this client's next gamestate.
        }
        demo_gen[slot]++;
        demo_active[slot] = 1;
        snprintf(demo_path[slot], sizeof(demo_path[slot]), "%s", path);
    } else if (!demo_active[slot]) {
        return; // we have not seen this slot's gamestate yet.
    }

    // Use a scratch buffer, never mutate the live outgoing message.
    msg_t tmp   = *msg;
    tmp.data    = demo_scratch;
    tmp.maxsize = (int)sizeof(demo_scratch);
    memcpy(demo_scratch, msg->data, (size_t)msg->cursize);
    MSG_WriteBits(&tmp, SVC_EOF, 8); // bit-accurate append.

    hdr.type = DEMO_REC_BLOCK;
    hdr.seq  = (int32_t)seq;
    hdr.len  = (uint32_t)tmp.cursize;
    if (demo_ring_put(&hdr, demo_scratch) != 0) {
        DebugPrint("demo: ring full, dropping slot %ld segment\n", slot);
        demo_active[slot] = 0;
        hdr.type          = DEMO_REC_CLOSE;
        hdr.len           = 0;
        demo_ring_put(&hdr, NULL);
    }
}

void Demo_Capture(msg_t *msg, client_t *client) {
    PROF_BEGIN(t_capture);
    Demo_CaptureBody(msg, client);
    PROF_END(PROF_DEMO_CAPTURE, t_capture);
}

void Demo_ClientDisconnect(int slot) {
    if (slot < 0 || slot >= MAX_DEMO_CLIENTS) {
        return;
    }
    demo_active[slot] = 0;
    // Drop the override too: the next player in this slot shouldn't inherit it.
    Demo_Request(slot, 0);
    if (demo_state_cached == DEMO_THREAD_RUNNING) {
        demo_rec_hdr_t hdr = {DEMO_REC_CLOSE, slot, 0, 0};
        demo_ring_put(&hdr, NULL);
    }
}

void Demo_CloseAll(void) {
    memset(demo_active, 0, sizeof(demo_active));
    if (demo_state_cached == DEMO_THREAD_RUNNING) {
        demo_rec_hdr_t hdr = {DEMO_REC_CLOSE_ALL, 0, 0, 0};
        demo_ring_put(&hdr, NULL);
    }
}

// Drops every per-slot override. Not part of Demo_CloseAll: a map change keeps the same clients
// in the same slots, so an override has to survive it. A shutdown does not. SV_Shutdown Z_Free's
// the client array outright (@qzeroded 0x43d880) and never calls SV_DropClient, so nothing
// reaches Demo_ClientDisconnect and the override would still stand for the next occupant.
void Demo_ClearRequests(void) {
    for (int slot = 0; slot < MAX_DEMO_CLIENTS; slot++) {
        Demo_Request(slot, 0); // keeps demo_forced_on in step
    }
}

// Milliseconds from now, on the clock PTHREAD_COND_INITIALIZER leaves a condvar using.
static void demo_deadline(struct timespec *ts, long ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int demo_past_deadline(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec != deadline->tv_sec) {
        return now.tv_sec > deadline->tv_sec;
    }
    return now.tv_nsec >= deadline->tv_nsec;
}

// Finalise every open segment and wait for the writer to have actually done it. Demo_CloseAll
// only posts to the ring, and the writer is detached, so at process exit that races and finalises
// nothing. The wait is bounded and polled. See demo_seq_done.
void Demo_DrainFinalise(void) {
    struct timespec deadline;
    demo_deadline(&deadline, DEMO_DRAIN_TIMEOUT_MS);

    // Already stopping means a SHUTDOWN record is in flight and finalises everything on its way
    // out, so wait for the writer rather than posting a CLOSE_ALL behind it. Nothing else can
    // have queued that SHUTDOWN; demo_reconcile_thread posts it from this same thread.
    if (demo_state_cached == DEMO_THREAD_STOPPING) {
        for (;;) {
            pthread_mutex_lock(&demo_lock);
            demo_thread_state_t state = demo_thread_state;
            pthread_mutex_unlock(&demo_lock);
            if (state == DEMO_THREAD_STOPPED) {
                demo_state_cached = state;
                return;
            }
            if (demo_past_deadline(&deadline)) {
                DebugPrint("demo: writer still draining after %d ms; segment(s) left as .part\n",
                           DEMO_DRAIN_TIMEOUT_MS);
                return;
            }
            usleep(1000);
        }
    }

    if (demo_state_cached != DEMO_THREAD_RUNNING) {
        return; // nothing has been recorded, so there is nothing open to finalise.
    }

    memset(demo_active, 0, sizeof(demo_active));
    demo_rec_hdr_t hdr = {DEMO_REC_CLOSE_ALL, 0, 0, 0};
    if (demo_ring_put(&hdr, NULL) != 0) {
        DebugPrint("demo: ring full at shutdown; open segment(s) left as .part\n");
        return;
    }

    // No other thread puts, so the count now standing is the record we just queued.
    unsigned long long target = atomic_load_explicit(&demo_seq_put, memory_order_relaxed);
    while (atomic_load_explicit(&demo_seq_done, memory_order_acquire) < target) {
        if (demo_past_deadline(&deadline)) {
            DebugPrint("demo: writer did not finalise within %d ms; segment(s) left as .part\n",
                       DEMO_DRAIN_TIMEOUT_MS);
            return;
        }
        usleep(1000);
    }
}

qboolean Demo_Request(int slot, int mode) {
    if (slot < 0 || slot >= MAX_DEMO_CLIENTS) {
        return qfalse;
    }

    int8_t want = (mode > 0) ? 1 : (mode < 0 ? -1 : 0);
    if (demo_request[slot] == want) {
        return qtrue;
    }

    // demo_forced_on only changes here, so it can't drift from the array.
    if (demo_request[slot] > 0) {
        demo_forced_on--;
    }
    if (want > 0) {
        demo_forced_on++;
    }
    demo_request[slot] = want;
    return qtrue;
}

int Demo_GetRequest(int slot) {
    if (slot < 0 || slot >= MAX_DEMO_CLIENTS) {
        return 0;
    }
    return demo_request[slot];
}

qboolean Demo_IsRecording(int slot) {
    if (slot < 0 || slot >= MAX_DEMO_CLIENTS) {
        return qfalse;
    }
    return demo_active[slot] ? qtrue : qfalse;
}

const char *Demo_GetPath(int slot) {
    if (slot < 0 || slot >= MAX_DEMO_CLIENTS || !demo_active[slot] || !demo_path[slot][0]) {
        return NULL;
    }
    return demo_path[slot];
}

// The writer lost the segment, so stop feeding it. Ignored unless the slot is still on
// the same segment, so a reopen in the meantime is never killed by a stale completion.
void Demo_AbandonSlot(int slot, uint32_t gen) {
    if (slot < 0 || slot >= MAX_DEMO_CLIENTS || demo_gen[slot] != gen) {
        return;
    }
    demo_active[slot] = 0;
}

unsigned Demo_PendingFinished(void) {
    return atomic_load_explicit(&demo_done_pending, memory_order_relaxed);
}

qboolean Demo_PollFinished(demo_finished_t *out) {
    qboolean got = qfalse;
    pthread_mutex_lock(&demo_lock);
    if (demo_done_tail != demo_done_head) {
        *out = demo_done[demo_done_tail % DEMO_DONE_MAX];
        demo_done_tail++;
        atomic_fetch_sub_explicit(&demo_done_pending, 1, memory_order_relaxed);
        got = qtrue;
    }
    pthread_mutex_unlock(&demo_lock);
    return got;
}

unsigned Demo_TakeDroppedCount(void) {
    pthread_mutex_lock(&demo_lock);
    unsigned n        = demo_done_dropped;
    demo_done_dropped = 0;
    atomic_fetch_sub_explicit(&demo_done_pending, n, memory_order_relaxed);
    pthread_mutex_unlock(&demo_lock);
    return n;
}
