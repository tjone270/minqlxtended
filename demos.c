#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "demos.h"
#include "quake_common.h"

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

typedef struct {
    FILE *fh;
    char path[512];
    long blocks; // blocks written to this segment (gamestate counts as 1.)
} demo_client_t;
static demo_client_t demos[MAX_DEMO_CLIENTS];
static unsigned char writer_scratch[MAX_NETCHAN_MSGLEN + 64];

static cvar_t *sv_demoRecord;     // 0 = off, 1 = record every connected client
static cvar_t *sv_demoDir;        // output subdirectory, under fs_homepath
static cvar_t *sv_demoNameFormat; // filename template: %date %slot %name
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

// Game thread only. Returns 0 on success, -1 if the record does not fit.
static int demo_ring_put(const demo_rec_hdr_t *hdr, const void *payload) {
    size_t need = sizeof(*hdr) + hdr->len;

    pthread_mutex_lock(&demo_lock);
    if (DEMO_RING_SIZE - (demo_head - demo_tail) < need) {
        pthread_mutex_unlock(&demo_lock);
        return -1;
    }
    ring_copy_in(demo_head, hdr, sizeof(*hdr));
    if (hdr->len) {
        ring_copy_in(demo_head + sizeof(*hdr), payload, hdr->len);
    }
    demo_head += need;
    pthread_cond_signal(&demo_cond);
    pthread_mutex_unlock(&demo_lock);
    return 0;
}

// d->path holds the final name; the segment is recorded into "<name>.part" until finalised.
static void demo_part_name(char *out, size_t n, const char *path) {
    snprintf(out, n, "%s.part", path);
}

static void writer_finalise(demo_client_t *d) {
    if (!d->fh) {
        return;
    }
    fwrite(demo_eof, sizeof(demo_eof), 1, d->fh);
    fclose(d->fh);
    d->fh = NULL;

    char part[sizeof(d->path) + 8];
    demo_part_name(part, sizeof(part), d->path);
    if (d->blocks <= 1) { // only the gamestate.
        unlink(part);
        DebugPrint("demo: discarded empty segment %s\n", d->path);
        return;
    }
    if (rename(part, d->path)) {
        DebugPrint("demo: could not rename %s into place\n", part);
    }
}

static void writer_handle_open(int slot, const char *path) {
    demo_client_t *d = &demos[slot];
    writer_finalise(d); // just in case this slot's CLOSE was dropped.

    size_t plen = strlen(path);
    if (plen >= sizeof(d->path)) {
        DebugPrint("demo: path too long, not recording slot %d\n", slot);
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
        return;
    }
    setvbuf(d->fh, NULL, _IOFBF, 64 * 1024);
    d->blocks = 0;
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
        return;
    }
    d->blocks++;
}

static void *demo_writer_main(void *unused) {
    (void)unused;

    // Never let the engine's signal handlers run on this thread.
    sigset_t all;
    sigfillset(&all);
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
        if (len != hdr.len || hdr.slot < 0 || hdr.slot >= MAX_DEMO_CLIENTS) {
            continue;
        }

        switch (hdr.type) {
        case DEMO_REC_OPEN:
            if (len > 0) {
                writer_scratch[len - 1] = '\0';
                writer_handle_open(hdr.slot, (const char *)writer_scratch);
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
}

static int demo_reconcile_thread(void) {
    int enabled = sv_demoRecord->integer != 0;

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

    // Disabled while RUNNING: stop the writer. SHUTDOWN finalises all open demos.
    demo_rec_hdr_t hdr = {DEMO_REC_SHUTDOWN, 0, 0, 0};
    if (demo_ring_put(&hdr, NULL) == 0) {
        pthread_mutex_lock(&demo_lock);
        demo_thread_state = DEMO_THREAD_STOPPING;
        pthread_mutex_unlock(&demo_lock);
        demo_state_cached = DEMO_THREAD_STOPPING;
        memset(demo_active, 0, sizeof(demo_active));
    } // ring full: retried on a later message.
    return 0;
}

void Demo_Init(void) {
    if (!Cvar_Get) {
        return;
    }
    sv_demoRecord     = Cvar_Get("sv_demoRecord", "0", CVAR_ARCHIVE);
    sv_demoDir        = Cvar_Get("sv_demoDir", "demos", CVAR_ARCHIVE);
    sv_demoNameFormat = Cvar_Get("sv_demoNameFormat", "%date_slot%slot_%name", CVAR_ARCHIVE);
    fs_homepath       = Cvar_FindVar("fs_homepath");
}

void Demo_Capture(msg_t *msg, client_t *client) {
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
        if (demo_ring_put(&hdr, path) != 0) {
            return; // ring full; retry at this client's next gamestate.
        }
        demo_active[slot] = 1;
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

void Demo_ClientDisconnect(int slot) {
    if (slot < 0 || slot >= MAX_DEMO_CLIENTS) {
        return;
    }
    demo_active[slot] = 0;
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
