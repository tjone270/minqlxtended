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

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "stream.h"
#include "engine/quake_common.h"

extern serverStatic_t *svs; // defined in dllmain.c
extern server_t *sv;        // defined in dllmain.c; NULL if its pattern did not resolve

#define STREAM_RING_SIZE   (16u * 1024 * 1024) // power of two
#define STREAM_OUT_SIZE    (256u * 1024)
#define STREAM_BODY_MAX    (MAX_NETCHAN_MSGLEN + 64) // an outgoing message plus the svc_EOF append
#define STREAM_FRAME_MAX   (8u + STREAM_BODY_MAX)
#define STREAM_CS_CHUNK    24576 // configstring payload per record
#define STREAM_WAIT_MS     100
#define STREAM_DRAIN_MS    500 // shutdown: how long we wait for the closes to reach the socket
#define STREAM_INBOUND_MAX 64  // the relay's frames are all fixed-length and small
#define STREAM_BACKOFF_MIN 1000
#define STREAM_BACKOFF_MAX 30000

_Static_assert(STREAM_OUT_SIZE > 2 * STREAM_FRAME_MAX, "the out buffer must hold a gamestate frame");

// Ring records, game thread to stream thread.
typedef enum {
    STREAM_REC_OPEN = 1,      // payload: stream_open_t
    STREAM_REC_BLOCK,         // payload: message bytes incl. the Huffman svc_EOF
    STREAM_REC_CLOSE,         // no payload; seq holds the reason
    STREAM_REC_CLOSE_ALL,     // no payload; seq holds the reason, slot ignored
    STREAM_REC_GAP,           // payload: stream_gap_t
    STREAM_REC_SERVERINFO,    // payload: configstring 0; seq holds sv->serverId
    STREAM_REC_CONFIGSTRINGS, // payload: packed pairs; seq holds serverId, gen holds the pair count
    STREAM_REC_RECONNECT,     // no payload; drop the link and connect again
    STREAM_REC_SHUTDOWN,      // send what is queued, then exit
} stream_rec_type_t;

typedef struct {
    int32_t type; // stream_rec_type_t
    int32_t slot;
    int32_t seq;  // BLOCK: netchan outgoingSequence. CLOSE: reason. SERVERINFO/CS: serverId.
    uint32_t gen; // stream_gen[slot] of the OPEN this record belongs to
    uint32_t flags;
    uint32_t len;   // payload bytes following this header
    uint32_t stamp; // stream_frame_ms when the game thread queued it; see stream_lag_ms
} stream_rec_hdr_t;          // 28 bytes
_Static_assert(sizeof(stream_rec_hdr_t) == 28, "the ring header must have no padding");

typedef struct {
    uint64_t steam_id;
    uint32_t gamestate_seq;
    uint32_t command_seq;
    char name[40];
} stream_open_t;

typedef struct {
    uint32_t first_seq;
    uint32_t dropped;
    uint32_t reason;
} stream_gap_t;

// Wire frame types. The payload layouts are documented with each emitter below.
#define SF_HELLO         0x01
#define SF_HELLO_ACK     0x02
#define SF_HEARTBEAT     0x03
#define SF_SLOT_OPEN     0x10
#define SF_BLOCK         0x11
#define SF_SLOT_CLOSE    0x12
#define SF_GAP           0x13
#define SF_CONFIGSTRINGS 0x20
#define SF_SERVER_INFO   0x21

#define STREAM_MAGIC    0x56544C51u
#define STREAM_PROTOCOL 1u
#define STREAM_SLOT_ANY 0xFFFFu

#define SBF_GAMESTATE 0x01 // SF_BLOCK: this block is the gamestate that opened the POV
#define SBF_FULL      0x02 // SF_BLOCK: this block is a full, non-delta snapshot
#define SOF_LIVE      0x01 // SF_SLOT_OPEN: the gamestate behind it is going out now
#define SOF_REPLAY    0x02 // SF_SLOT_OPEN: a cached gamestate, replayed after a reconnect
#define SCF_FIRST     0x01
#define SCF_LAST      0x02

#define SCR_DISCONNECT 0 // SF_SLOT_CLOSE reasons
#define SCR_MAP_CHANGE 1
#define SCR_SHUTDOWN   2
#define SCR_GAMESTATE  3
#define SCR_DISABLED   4

#define SGR_RING_FULL 0 // SF_GAP reasons
#define SGR_STALLED   1
#define SGR_LINK_DOWN 2
#define SGR_LAGGING   3

typedef enum {
    STREAM_THREAD_STOPPED = 0,
    STREAM_THREAD_RUNNING,
    STREAM_THREAD_STOPPING,
} stream_thread_state_t;

static unsigned char stream_ring[STREAM_RING_SIZE];
static uint64_t stream_head; // advanced by the game thread
static uint64_t stream_tail; // advanced by the stream thread
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stream_cond;
static int stream_cond_ready;
static stream_thread_state_t stream_thread_state = STREAM_THREAD_STOPPED;

// Records queued and records handled. Stream_DrainClose waits on the second: a close counted
// there has reached the socket.
static atomic_ullong stream_seq_put;
static atomic_ullong stream_seq_done;

// Written by the stream thread, read by the game thread.
static atomic_int stream_link_up;
static atomic_uint stream_resync_epoch;
static atomic_uchar stream_desynced[MAX_STREAM_CLIENTS];

// How far behind the stream is, in ms. A relay that reads slowly but never stops keeps
// stream_out_progress fresh, so the stall timer never fires and the ring fills to 16MB, about
// eighty seconds. Measuring the lag directly is what stops the game thread feeding it at
// sv_demoStreamMaxLag. Stream_Frame publishes the clock once per frame; see stream_put.
static atomic_uint stream_frame_ms;
static atomic_uint stream_lag_ms;

// Counters. Relaxed atomics: the thread bumps some of these per frame it encodes, and taking
// stream_lock that often would contend with the game thread's puts.
static atomic_uint stream_st_high_water;
static atomic_ullong stream_st_sent;
static atomic_ullong stream_st_frames;
static atomic_uint stream_st_dropped_ring;
static atomic_uint stream_st_dropped_link;
static atomic_uint stream_st_dropped_stall;
static atomic_uint stream_st_dropped_lag;
static atomic_uint stream_st_gaps;
static atomic_uint stream_st_resyncs;
static atomic_uint stream_st_reconnects;
static char stream_error[96]; // under stream_lock; written on failure, read by the report

// Where to dial and what to say on arrival. Copied under stream_lock when the game thread sees a
// change. cvar_t::string is Z_Free'd on every set, so the thread never holds a pointer into one.
typedef struct {
    char host[128];
    char port[16];
    char token[64];
} stream_endpoint_t;
static stream_endpoint_t stream_endpoint;

// Bumped after stream_endpoint is written, so the thread can check for a stale copy without the lock.
static atomic_uint stream_endpoint_gen;

// Republished every frame. Plain atomics, so the thread reads them without a lock.
static atomic_int stream_cfg_heartbeat_ms;
static atomic_int stream_cfg_timeout_ms;
static atomic_int stream_cfg_stall_ms;
static atomic_int stream_cfg_drop_ms;
static atomic_int stream_cfg_max_clients;
static atomic_int stream_cfg_fps;
static atomic_ullong stream_cfg_steam_id;

static int stream_cfg_int(atomic_int *v) { return atomic_load_explicit(v, memory_order_relaxed); }

// Game thread only, all of it.

static cvar_t *sv_demoStream;
static cvar_t *sv_demoStreamHost;
static cvar_t *sv_demoStreamPort;
static cvar_t *sv_demoStreamToken;
static cvar_t *sv_demoStreamTokenFile;
static cvar_t *sv_demoStreamSlots;
static cvar_t *sv_demoStreamHeartbeat;
static cvar_t *sv_demoStreamTimeout;
static cvar_t *sv_demoStreamStall;
static cvar_t *sv_demoStreamDrop;
static cvar_t *sv_demoStreamMaxLag;
static cvar_t *sv_demoStreamResync;
static cvar_t *sv_demoStreamResyncPerFrame;
static cvar_t *sv_demoStreamKeyframeSec;
static cvar_t *sv_demoStreaming; // read-only, so a player can tell the server streams
static cvar_t *sv_fps;
static cvar_t *sv_steamAccount;

static stream_thread_state_t stream_state_cached = STREAM_THREAD_STOPPED;
static uint8_t stream_active[MAX_STREAM_CLIENTS];
static uint32_t stream_gen[MAX_STREAM_CLIENTS];
static char stream_name[MAX_STREAM_CLIENTS][40];
static uint8_t stream_resync_pending[MAX_STREAM_CLIENTS];
// When this slot last queued a full snapshot; see stream_arm_keyframes.
static uint64_t stream_last_full_ms[MAX_STREAM_CLIENTS];

// Per-slot override of the sv_demoStreamSlots mask, set from Python: 0 follows the mask, 1 always
// streams, -1 never. stream_forced_on counts the slots set to 1 and only changes in Stream_Request.
static int8_t stream_request[MAX_STREAM_CLIENTS];
static int stream_forced_on;

// A gap the game thread saw but could not queue yet, accumulated per slot until it fits.
static uint32_t stream_gap_count[MAX_STREAM_CLIENTS];
static uint32_t stream_gap_first[MAX_STREAM_CLIENTS];
static uint8_t stream_gap_reason[MAX_STREAM_CLIENTS];

static uint64_t stream_slot_mask = ~0ull; // sv_demoStreamSlots, parsed
static char stream_slots_seen[256];       // the string it was parsed from
static char stream_cfg_seen[256];         // host/port/id/token as of the last snapshot
static char stream_fingerprint[16];

// The token file is read only when its path changes. See stream_read_token.
static char stream_token_cache[64];
static char stream_tokenfile_seen[256];
static int stream_token_cached;
static unsigned stream_epoch_seen;
static int stream_serverid_seen;
static int stream_serverid_valid;
static char stream_guid_seen[128]; // CS_MATCH_GUID as last published; see stream_publish_guid
static int stream_mirror_seen = -1;
static int stream_resync_cursor; // round-robin, so a resync does not always start at slot 0
static int stream_shedding;      // past sv_demoStreamMaxLag; see stream_over_lag

// The stream thread's own. The game thread never touches these.

static int stream_fd = -1;
static int stream_acked;
static int stream_replay_pending;
static int stream_replay_cursor;
static uint64_t stream_ack_deadline;
static uint64_t stream_retry_at;
static unsigned stream_backoff_ms = STREAM_BACKOFF_MIN;
static uint64_t stream_session_id;
static unsigned stream_cfg_generation;
static int stream_ever_connected;
static int stream_shutdown_seen;

static unsigned char stream_out[STREAM_OUT_SIZE];
static size_t stream_out_head; // write cursor
static size_t stream_out_tail; // send cursor
static uint64_t stream_out_progress;
static int stream_stalled;
static uint64_t stream_last_beat;
static uint64_t stream_started_at;

static unsigned char stream_scratch[STREAM_BODY_MAX];
static unsigned char stream_in[STREAM_INBOUND_MAX + 8];
static size_t stream_in_len;

// The newest gamestate per slot, replayed on reconnect. It carries the configstrings and entity
// baselines, and a relay that was down when the client primed cannot decode the POV without it.
// Baselines hold for the whole map (SV_CreateBaseline runs once, from SV_SpawnServer).
static unsigned char *stream_gs[MAX_STREAM_CLIENTS];
static uint32_t stream_gs_len[MAX_STREAM_CLIENTS];
static uint32_t stream_gs_seq[MAX_STREAM_CLIENTS];
static uint32_t stream_gs_gen[MAX_STREAM_CLIENTS];
static stream_open_t stream_gs_open[MAX_STREAM_CLIENTS];

static char stream_info[MAX_INFO_STRING];
static uint32_t stream_info_len;
static int stream_info_serverid;

// Gaps still to be declared to the relay, the thread's own drops and the game thread's. Kept out
// of the ring so a full ring cannot lose the report that it was full.
static uint32_t stream_gap_owed[MAX_STREAM_CLIENTS];
static uint32_t stream_gap_seq[MAX_STREAM_CLIENTS];
static uint8_t stream_gap_why[MAX_STREAM_CLIENTS];

static uint64_t stream_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void stream_deadline(struct timespec *ts, long ms) {
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static uint64_t stream_hash(const void *data, size_t n, uint64_t seed) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h             = seed;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

static void wr_u8(unsigned char *p, uint8_t v) { p[0] = v; }

static void wr_u16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void wr_u32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void wr_u64(unsigned char *p, uint64_t v) {
    wr_u32(p, (uint32_t)(v & 0xFFFFFFFFull));
    wr_u32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFull));
}

static uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void stream_bump(atomic_uint *counter) {
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

static void stream_note_error(const char *what) {
    pthread_mutex_lock(&stream_lock);
    snprintf(stream_error, sizeof(stream_error), "%s", what ? what : "");
    pthread_mutex_unlock(&stream_lock);
}

// Both ring copy helpers require stream_lock and handle wraparound with a split copy.
static void ring_copy_in(uint64_t pos, const void *src, size_t n) {
    size_t off   = (size_t)(pos & (STREAM_RING_SIZE - 1));
    size_t first = STREAM_RING_SIZE - off;
    if (first > n) {
        first = n;
    }
    memcpy(stream_ring + off, src, first);
    memcpy(stream_ring, (const unsigned char *)src + first, n - first);
}

static void ring_copy_out(uint64_t pos, void *dst, size_t n) {
    size_t off   = (size_t)(pos & (STREAM_RING_SIZE - 1));
    size_t first = STREAM_RING_SIZE - off;
    if (first > n) {
        first = n;
    }
    memcpy(dst, stream_ring + off, first);
    memcpy((unsigned char *)dst + first, stream_ring, n - first);
}

// Game thread only. Returns 0 on success, -1 if the record does not fit.
static int stream_put(const stream_rec_hdr_t *hdr, const void *payload) {
    size_t need = sizeof(*hdr) + hdr->len;
    int rc      = -1;

    // Stamped here, for every caller.
    stream_rec_hdr_t stamped = *hdr;
    stamped.stamp            = atomic_load_explicit(&stream_frame_ms, memory_order_relaxed);

    pthread_mutex_lock(&stream_lock);
    if (STREAM_RING_SIZE - (stream_head - stream_tail) >= need) {
        ring_copy_in(stream_head, &stamped, sizeof(stamped));
        if (hdr->len) {
            ring_copy_in(stream_head + sizeof(*hdr), payload, hdr->len);
        }
        stream_head += need;
        unsigned depth = (unsigned)(stream_head - stream_tail);
        if (depth > atomic_load_explicit(&stream_st_high_water, memory_order_relaxed)) {
            atomic_store_explicit(&stream_st_high_water, depth, memory_order_relaxed);
        }
        // Under the lock and before the signal, so stream_seq_done can never pass an uncounted put.
        atomic_store_explicit(&stream_seq_put,
                              atomic_load_explicit(&stream_seq_put, memory_order_relaxed) + 1,
                              memory_order_relaxed);
        pthread_cond_signal(&stream_cond);
        rc = 0;
    }
    pthread_mutex_unlock(&stream_lock);
    return rc;
}

// Stream thread only. Returns 0 when the ring is empty.
static int stream_take(stream_rec_hdr_t *hdr, unsigned char *payload) {
    int got = 0;
    pthread_mutex_lock(&stream_lock);
    if (stream_head != stream_tail) {
        ring_copy_out(stream_tail, hdr, sizeof(*hdr));
        uint32_t len = hdr->len;
        if (len > STREAM_BODY_MAX) { // should be impossible
            len = 0;
        }
        if (len) {
            ring_copy_out(stream_tail + sizeof(*hdr), payload, len);
        }
        stream_tail += sizeof(*hdr) + hdr->len;
        hdr->len = len;
        got      = 1;
    }
    pthread_mutex_unlock(&stream_lock);
    return got;
}

static unsigned stream_pending_bytes(void) {
    pthread_mutex_lock(&stream_lock);
    unsigned n = (unsigned)(stream_head - stream_tail);
    pthread_mutex_unlock(&stream_lock);
    return n;
}

static size_t out_room(void) { return STREAM_OUT_SIZE - stream_out_head; }

static void out_compact(void) {
    if (stream_out_tail == stream_out_head) {
        stream_out_head = stream_out_tail = 0;
    } else if (stream_out_tail > 0) {
        memmove(stream_out, stream_out + stream_out_tail, stream_out_head - stream_out_tail);
        stream_out_head -= stream_out_tail;
        stream_out_tail = 0;
    }
}

// Reserves a frame header plus payload and returns the payload cursor, or NULL if it will not fit.
//
//   +0  u8  type    +1  u8  flags    +2  u16 slot    +4  u32 length
static unsigned char *out_frame(uint8_t type, uint8_t flags, uint16_t slot, uint32_t len) {
    if (out_room() < 8 + (size_t)len) {
        return NULL;
    }
    unsigned char *p = stream_out + stream_out_head;
    wr_u8(p, type);
    wr_u8(p + 1, flags);
    wr_u16(p + 2, slot);
    wr_u32(p + 4, len);
    stream_out_head += 8 + (size_t)len;
    atomic_fetch_add_explicit(&stream_st_frames, 1, memory_order_relaxed);
    return p + 8;
}

static void stream_close_socket(const char *why) {
    if (stream_fd >= 0) {
        close(stream_fd);
        stream_fd = -1;
    }
    stream_acked          = 0;
    stream_stalled        = 0;
    stream_in_len         = 0;
    stream_out_head       = 0;
    stream_out_tail       = 0;
    stream_replay_pending = 0;
    stream_replay_cursor  = 0;
    atomic_store_explicit(&stream_link_up, 0, memory_order_relaxed);
    if (why) {
        DebugPrint("stream: link down (%s)\n", why);
        stream_note_error(why);
    }
}

//   +0 u32 magic  +4 u32 protocol  +8 u64 session  +16 u64 steam_id
//   +24 u32 max_clients  +28 u32 sv_fps  +32 token[64]  +96 reserved[32]  +128 build[40]
// The relay tells servers apart by steam_id, falling back to the peer address.
static void stream_send_hello(const stream_endpoint_t *ep) {
    unsigned char *p = out_frame(SF_HELLO, 0, STREAM_SLOT_ANY, 168);
    if (!p) {
        return;
    }
    memset(p, 0, 168);
    wr_u32(p, STREAM_MAGIC);
    wr_u32(p + 4, STREAM_PROTOCOL);
    wr_u64(p + 8, stream_session_id);
    wr_u64(p + 16, atomic_load_explicit(&stream_cfg_steam_id, memory_order_relaxed));
    wr_u32(p + 24, (uint32_t)stream_cfg_int(&stream_cfg_max_clients));
    wr_u32(p + 28, (uint32_t)stream_cfg_int(&stream_cfg_fps));
    memcpy(p + 32, ep->token, strnlen(ep->token, 64));

    size_t build_len = strlen(MINQLXTENDED_VERSION);
    memcpy(p + 128, MINQLXTENDED_VERSION, build_len > 40 ? 40 : build_len);
}

static void stream_connect(const stream_endpoint_t *ep) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc               = getaddrinfo(ep->host, ep->port[0] ? ep->port : "27999", &hints, &res);
    if (rc != 0 || !res) {
        stream_note_error(gai_strerror(rc));
        return;
    }

    int timeout_ms = stream_cfg_int(&stream_cfg_timeout_ms);
    int fd         = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            int err           = 0;
            socklen_t elen    = sizeof(err);
            if (poll(&pfd, 1, timeout_ms) == 1 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
                break;
            }
            stream_note_error(err ? strerror(err) : "connect timed out");
        } else {
            stream_note_error(strerror(errno));
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        return;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int sndbuf = 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    uint64_t now        = stream_now_ms();
    stream_fd           = fd;
    stream_session_id   = stream_hash(ep->host, strnlen(ep->host, sizeof(ep->host)),
                                      now ^ ((uint64_t)getpid() << 32));
    stream_ack_deadline = now + (uint64_t)timeout_ms;
    stream_out_progress = now;
    stream_last_beat    = now;
    stream_send_hello(ep);
    DebugPrint("stream: connecting to %s:%s\n", ep->host, ep->port);
}

// Pushes as much of the out buffer as the socket will take. Returns 0 if the link died.
static int stream_flush(void) {
    while (stream_out_tail < stream_out_head) {
        ssize_t n = send(stream_fd, stream_out + stream_out_tail,
                         stream_out_head - stream_out_tail, MSG_NOSIGNAL);
        if (n > 0) {
            stream_out_tail += (size_t)n;
            stream_out_progress = stream_now_ms();
            atomic_fetch_add_explicit(&stream_st_sent, (unsigned long long)n, memory_order_relaxed);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            break;
        }
        stream_close_socket(n == 0 ? "socket closed" : strerror(errno));
        return 0;
    }
    out_compact();
    return 1;
}

// The relay may send SF_HELLO_ACK and SF_HEARTBEAT, both fixed-length, and nothing else. Anything
// else drops the connection: a game server must not be steerable from the far end of a stream.
static int stream_service_in(void) {
    for (;;) {
        if (stream_in_len >= sizeof(stream_in)) {
            // Unreachable: a frame is at most 72 bytes and the loop below consumes whole frames.
            // Guarded because a zero-length recv reads as a clean close.
            stream_close_socket("inbound buffer full");
            return 0;
        }
        ssize_t n = recv(stream_fd, stream_in + stream_in_len, sizeof(stream_in) - stream_in_len, 0);
        if (n == 0) {
            stream_close_socket("relay closed the connection");
            return 0;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            stream_close_socket(strerror(errno));
            return 0;
        }
        stream_in_len += (size_t)n;

        while (stream_in_len >= 8) {
            uint8_t type = stream_in[0];
            uint32_t len = rd_u32(stream_in + 4);
            if (len > STREAM_INBOUND_MAX || (type != SF_HELLO_ACK && type != SF_HEARTBEAT)) {
                stream_close_socket("relay sent a frame we do not accept");
                return 0;
            }
            if (stream_in_len < 8 + (size_t)len) {
                break;
            }
            if (type == SF_HELLO_ACK) {
                if (len < 8 || rd_u32(stream_in + 8) != STREAM_PROTOCOL) {
                    stream_close_socket("relay protocol mismatch");
                    return 0;
                }
                uint32_t status = rd_u32(stream_in + 12);
                if (status != 0) {
                    // A relay refusing our token will keep refusing it. Back off to the cap.
                    stream_backoff_ms = STREAM_BACKOFF_MAX;
                    stream_close_socket(status == 1 ? "relay rejected the token"
                                                    : "relay refused the connection");
                    return 0;
                }
                stream_acked          = 1;
                stream_replay_pending = 1;
                stream_replay_cursor  = 0;
                stream_backoff_ms     = STREAM_BACKOFF_MIN;
                atomic_store_explicit(&stream_link_up, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&stream_resync_epoch, 1, memory_order_relaxed);
                if (stream_ever_connected) {
                    stream_bump(&stream_st_reconnects);
                }
                stream_ever_connected = 1;
                stream_note_error("");
                DebugPrint("stream: link up\n");
            }
            memmove(stream_in, stream_in + 8 + len, stream_in_len - (8 + (size_t)len));
            stream_in_len -= 8 + (size_t)len;
        }
    }
    return 1;
}

//   +0 u32 gen  +4 u32 first_seq  +8 u32 dropped  +12 u32 reason
static void stream_emit_gap(int slot) {
    if (!stream_gap_owed[slot]) {
        return;
    }
    unsigned char *p = out_frame(SF_GAP, 0, (uint16_t)slot, 16);
    if (!p) {
        return; // no room; the gap stays owed and still goes out ahead of the next block
    }
    wr_u32(p, stream_gs_gen[slot]);
    wr_u32(p + 4, stream_gap_seq[slot]);
    wr_u32(p + 8, stream_gap_owed[slot]);
    wr_u32(p + 12, stream_gap_why[slot]);
    stream_gap_owed[slot] = 0;
    stream_bump(&stream_st_gaps);
}

static void stream_owe_gap(int slot, uint32_t seq, uint8_t why) {
    if (!stream_gap_owed[slot]) {
        stream_gap_seq[slot] = seq;
        stream_gap_why[slot] = why;
    }
    if (stream_gap_owed[slot] < 0xFFFFFFFFu) {
        stream_gap_owed[slot]++;
    }
    atomic_store_explicit(&stream_desynced[slot], 1, memory_order_relaxed);
}

//   +0 u32 gen  +4 u32 gamestate_seq  +8 u64 steam_id  +16 u32 flags
//   +20 u32 command_seq  +24 u32 gs_command_seq  +28 u32 reserved  +32 name[40]
static void stream_emit_open(int slot, const stream_open_t *open, uint32_t gen, uint8_t flags) {
    unsigned char *p = out_frame(SF_SLOT_OPEN, flags, (uint16_t)slot, 72);
    if (!p) {
        return;
    }
    memset(p, 0, 72);
    wr_u32(p, gen);
    wr_u32(p + 4, open->gamestate_seq);
    wr_u64(p + 8, open->steam_id);
    wr_u32(p + 16, flags);
    // On a replay the live sequence is unknown. The relay numbers what it synthesises from
    // gs_command_seq + 1, inside the gap it missed, so a collision can only hit a command it never got.
    wr_u32(p + 20, (flags & SOF_REPLAY) ? 0u : open->command_seq);
    wr_u32(p + 24, open->command_seq);
    memcpy(p + 32, open->name, strnlen(open->name, sizeof(open->name)));
}

//   +0 u32 seq  +4 u32 gen  +8 body[len]. The body is exactly a .dm_91 block body.
static void stream_emit_block(int slot, uint32_t seq, uint32_t gen, uint8_t flags,
                              const unsigned char *body, uint32_t len) {
    stream_emit_gap(slot);
    unsigned char *p = out_frame(SF_BLOCK, flags, (uint16_t)slot, 8 + len);
    if (!p) {
        stream_owe_gap(slot, seq, SGR_STALLED);
        stream_bump(&stream_st_dropped_stall);
        return;
    }
    wr_u32(p, seq);
    wr_u32(p + 4, gen);
    memcpy(p + 8, body, len);
    if (flags & SBF_FULL) {
        atomic_store_explicit(&stream_desynced[slot], 0, memory_order_relaxed);
    }
}

static void stream_emit_close(int slot, uint32_t gen, uint32_t reason) {
    unsigned char *p = out_frame(SF_SLOT_CLOSE, 0, (uint16_t)slot, 8);
    if (p) {
        wr_u32(p, gen);
        wr_u32(p + 4, reason);
    }
}

//   +0 u32 server_id  +4 u32 text_len  +8 text[text_len]
static void stream_emit_serverinfo(void) {
    if (!stream_info_len) {
        return;
    }
    unsigned char *p = out_frame(SF_SERVER_INFO, 0, STREAM_SLOT_ANY, 8 + stream_info_len);
    if (!p) {
        return;
    }
    wr_u32(p, (uint32_t)stream_info_serverid);
    wr_u32(p + 4, stream_info_len);
    memcpy(p + 8, stream_info, stream_info_len);
}

static void stream_cache_gamestate(int slot, uint32_t seq, uint32_t gen, const unsigned char *body,
                                   uint32_t len) {
    if (!stream_gs[slot]) {
        stream_gs[slot] = (unsigned char *)malloc(STREAM_BODY_MAX);
        if (!stream_gs[slot]) {
            return;
        }
    }
    memcpy(stream_gs[slot], body, len);
    stream_gs_len[slot] = len;
    stream_gs_seq[slot] = seq;
    stream_gs_gen[slot] = gen;
}

static void stream_forget(int slot) {
    stream_gs_len[slot]   = 0;
    stream_gap_owed[slot] = 0;
    atomic_store_explicit(&stream_desynced[slot], 0, memory_order_relaxed);
}

// What the relay needs to pick the POVs back up: the serverinfo, then each cached gamestate as a
// replay. The game thread then forces full snapshots against the epoch this connection bumped.
// Resumable, since a whole server's gamestates will not fit the out buffer at once.
static void stream_replay(void) {
    if (stream_replay_cursor == 0) {
        stream_emit_serverinfo();
    }
    while (stream_replay_cursor < MAX_STREAM_CLIENTS) {
        int i = stream_replay_cursor;
        if (out_room() < STREAM_FRAME_MAX + 96) {
            return; // the rest go out once the socket drains
        }
        stream_replay_cursor++;
        if (!stream_gs_len[i] || !stream_gs[i]) {
            continue;
        }
        stream_emit_open(i, &stream_gs_open[i], stream_gs_gen[i], SOF_REPLAY);
        stream_emit_block(i, stream_gs_seq[i], stream_gs_gen[i], SBF_GAMESTATE, stream_gs[i],
                          stream_gs_len[i]);
    }
    stream_replay_pending = 0;
}

// One ring record. Cacheable records are kept whether or not the link is up; the rest are dropped
// and owed as a gap.
static void stream_apply(const stream_rec_hdr_t *hdr, const unsigned char *payload, int live) {
    int slot = hdr->slot;

    switch (hdr->type) {
    case STREAM_REC_SERVERINFO:
        stream_info_len = hdr->len < sizeof(stream_info) ? hdr->len : (uint32_t)sizeof(stream_info);
        memcpy(stream_info, payload, stream_info_len);
        stream_info_serverid = hdr->seq;
        if (live) {
            stream_emit_serverinfo();
        }
        return;

    case STREAM_REC_CONFIGSTRINGS:
        if (live) {
            //   +0 u32 server_id  +4 u32 count  then count x { u16 index; u16 len; char[len] }
            // SCF_FIRST begins a fresh set; a frame without it patches the set the relay holds.
            unsigned char *p =
                out_frame(SF_CONFIGSTRINGS, (uint8_t)hdr->flags, STREAM_SLOT_ANY, 8 + hdr->len);
            if (p) {
                wr_u32(p, (uint32_t)hdr->seq);
                wr_u32(p + 4, hdr->gen);
                memcpy(p + 8, payload, hdr->len);
            }
        }
        return;

    case STREAM_REC_CLOSE_ALL:
        for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
            if (stream_gs_len[i]) {
                if (live) {
                    stream_emit_close(i, stream_gs_gen[i], (uint32_t)hdr->seq);
                }
                stream_forget(i);
            }
        }
        return;

    default:
        break;
    }

    if (slot < 0 || slot >= MAX_STREAM_CLIENTS) {
        return;
    }

    switch (hdr->type) {
    case STREAM_REC_OPEN:
        if (hdr->len >= sizeof(stream_open_t)) {
            memcpy(&stream_gs_open[slot], payload, sizeof(stream_open_t));
            stream_gs_gen[slot] = hdr->gen;
            // The gamestate for this open is the next record. Until then the cache holds the
            // previous generation's.
            stream_gs_len[slot]   = 0;
            stream_gap_owed[slot] = 0;
            atomic_store_explicit(&stream_desynced[slot], 0, memory_order_relaxed);
            if (live) {
                stream_emit_open(slot, &stream_gs_open[slot], hdr->gen, SOF_LIVE);
            }
        }
        break;

    case STREAM_REC_BLOCK:
        if (hdr->flags & SBF_GAMESTATE) {
            stream_cache_gamestate(slot, (uint32_t)hdr->seq, hdr->gen, payload, hdr->len);
        }
        if (live) {
            stream_emit_block(slot, (uint32_t)hdr->seq, hdr->gen, (uint8_t)hdr->flags, payload,
                              hdr->len);
        } else if (!(hdr->flags & SBF_GAMESTATE)) {
            stream_owe_gap(slot, (uint32_t)hdr->seq, stream_stalled ? SGR_STALLED : SGR_LINK_DOWN);
            stream_bump(stream_stalled ? &stream_st_dropped_stall : &stream_st_dropped_link);
        }
        break;

    case STREAM_REC_CLOSE:
        if (live) {
            stream_emit_close(slot, hdr->gen, (uint32_t)hdr->seq);
        }
        stream_forget(slot);
        break;

    case STREAM_REC_GAP:
        if (hdr->len >= sizeof(stream_gap_t)) {
            stream_gap_t g;
            memcpy(&g, payload, sizeof(g));
            if (!stream_gap_owed[slot]) {
                stream_gap_seq[slot] = g.first_seq;
                stream_gap_why[slot] = (uint8_t)g.reason;
            }
            stream_gap_owed[slot] += g.dropped;
            atomic_store_explicit(&stream_desynced[slot], 1, memory_order_relaxed);
        }
        break;

    default:
        break;
    }
}

static void stream_backoff(uint64_t now) {
    stream_retry_at = now + stream_backoff_ms;
    stream_backoff_ms =
        stream_backoff_ms < STREAM_BACKOFF_MAX ? stream_backoff_ms * 2 : STREAM_BACKOFF_MAX;
}

static void *stream_main(void *unused) {
    (void)unused;

    // Keep the engine's signal handling on the main thread. The faults stay unblocked, since
    // blocking one the thread raises itself is undefined, and SIGABRT so the engine still prints
    // a backtrace.
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGSEGV);
    sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGFPE);
    sigdelset(&all, SIGILL);
    sigdelset(&all, SIGABRT);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    stream_started_at = stream_now_ms();
    stream_last_beat  = stream_started_at;
    stream_retry_at   = 0;
    stream_backoff_ms = STREAM_BACKOFF_MIN;

    stream_endpoint_t ep;
    memset(&ep, 0, sizeof(ep));
    int have_ep = 0; // the generation alone cannot say this, since a restarted thread inherits it

    for (;;) {
        uint64_t now = stream_now_ms();

        // A changed endpoint means the link we hold is to the wrong place. One relaxed load per pass.
        unsigned gen = atomic_load_explicit(&stream_endpoint_gen, memory_order_acquire);
        if (gen != stream_cfg_generation || !have_ep) {
            stream_cfg_generation = gen;
            have_ep               = 1;
            pthread_mutex_lock(&stream_lock);
            ep = stream_endpoint;
            pthread_mutex_unlock(&stream_lock);
            if (stream_fd >= 0) {
                stream_close_socket("configuration changed");
            }
            stream_backoff_ms = STREAM_BACKOFF_MIN;
            stream_retry_at   = 0;
        }

        if (stream_fd < 0 && ep.host[0] && !stream_shutdown_seen && now >= stream_retry_at) {
            stream_connect(&ep);
            if (stream_fd < 0) {
                stream_backoff(now);
            }
            // The connect blocks and stamps stream_out_progress with a fresh clock. Read it again,
            // or the idle check below wraps and drops the link with the HELLO unsent.
            now = stream_now_ms();
        }

        if (stream_fd >= 0 && !stream_acked && now >= stream_ack_deadline) {
            stream_close_socket("no handshake reply");
            stream_backoff(now);
        }

        // A relay that has not read for this long will not catch up. Drop it and resync.
        if (stream_fd >= 0 && stream_out_tail < stream_out_head) {
            uint64_t idle = now > stream_out_progress ? now - stream_out_progress : 0;
            if (idle > (uint64_t)stream_cfg_int(&stream_cfg_drop_ms)) {
                stream_close_socket("relay stopped reading");
                stream_backoff(now);
            } else if (idle > (uint64_t)stream_cfg_int(&stream_cfg_stall_ms)) {
                stream_stalled = 1;
            }
        } else {
            stream_stalled      = 0;
            stream_out_progress = now;
        }

        // Before the drain, so a replayed gamestate precedes the live blocks queued behind it.
        if (stream_replay_pending && stream_acked) {
            stream_replay();
        }

        int drained = 0;
        for (;;) {
            int live = stream_acked && !stream_stalled;
            // Always drain, connected or not: the cacheable records are what a reconnect replays,
            // and a full ring backs up into the game thread's puts.
            if (live && (stream_replay_pending || out_room() < STREAM_FRAME_MAX + 96)) {
                break;
            }
            stream_rec_hdr_t hdr;
            if (!stream_take(&hdr, stream_scratch)) {
                atomic_store_explicit(&stream_lag_ms, 0, memory_order_relaxed);
                break;
            }
            if (hdr.stamp) { // zero only for a record queued before the first Stream_Frame
                // The game thread can stamp a record after this pass read the clock, so take the
                // difference signed. Unsigned it wraps to ~50 days and starts a shed.
                int32_t lag = (int32_t)((uint32_t)now - hdr.stamp);
                atomic_store_explicit(&stream_lag_ms, lag > 0 ? (uint32_t)lag : 0u,
                                      memory_order_relaxed);
            }
            if (hdr.type == STREAM_REC_SHUTDOWN) {
                stream_shutdown_seen = 1;
            } else if (hdr.type == STREAM_REC_RECONNECT) {
                if (stream_fd >= 0) {
                    stream_close_socket("asked to reconnect");
                }
                stream_retry_at   = 0;
                stream_backoff_ms = STREAM_BACKOFF_MIN;
            } else {
                stream_apply(&hdr, stream_scratch, live);
            }
            // Release, so a waiter that sees this count has also seen the work behind it.
            atomic_store_explicit(&stream_seq_done,
                                  atomic_load_explicit(&stream_seq_done, memory_order_relaxed) + 1,
                                  memory_order_release);
            drained = 1;
        }

        if (stream_fd >= 0) {
            if (stream_acked && now - stream_last_beat >= (uint64_t)stream_cfg_int(&stream_cfg_heartbeat_ms)) {
                stream_last_beat = now;
                //   +0 u32 uptime_ms (of this thread)  +4 u32 bytes still in the ring
                unsigned char *p = out_frame(SF_HEARTBEAT, 0, STREAM_SLOT_ANY, 8);
                if (p) {
                    wr_u32(p, (uint32_t)(now - stream_started_at));
                    wr_u32(p + 4, stream_pending_bytes());
                }
            }
            if (stream_flush()) {
                stream_service_in();
            }
        }

        if (stream_shutdown_seen && stream_out_tail == stream_out_head && !stream_pending_bytes()) {
            break;
        }

        if (!drained) {
            if (stream_fd >= 0 && stream_out_tail < stream_out_head) {
                struct pollfd pfd = {stream_fd, POLLIN | POLLOUT, 0};
                poll(&pfd, 1, STREAM_WAIT_MS);
            } else {
                pthread_mutex_lock(&stream_lock);
                if (stream_head == stream_tail) {
                    struct timespec ts;
                    stream_deadline(&ts, STREAM_WAIT_MS);
                    pthread_cond_timedwait(&stream_cond, &stream_lock, &ts);
                }
                pthread_mutex_unlock(&stream_lock);
            }
        }
    }

    // Back to the starting state, so a second thread does not inherit this one's slots.
    stream_close_socket(NULL);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        free(stream_gs[i]);
        stream_gs[i]          = NULL;
        stream_gs_len[i]      = 0;
        stream_gap_owed[i]    = 0;
        atomic_store_explicit(&stream_desynced[i], 0, memory_order_relaxed);
    }
    stream_shutdown_seen  = 0;
    stream_ever_connected = 0;
    stream_info_len       = 0;
    stream_info_serverid  = 0;

    pthread_mutex_lock(&stream_lock);
    stream_thread_state = STREAM_THREAD_STOPPED;
    pthread_mutex_unlock(&stream_lock);
    DebugPrint("stream: thread stopped\n");
    return NULL;
}

// A request starts the stream, as demo_forced_on does for the writer, but a host is still needed.
static int stream_enabled(void) {
    if (!sv_demoStream || !sv_demoStreamHost || !sv_demoStreamHost->string[0]) {
        return 0;
    }
    return sv_demoStream->integer || stream_forced_on > 0;
}

static int stream_reconcile_thread(void) {
    int enabled = stream_enabled();

    if (enabled && stream_state_cached == STREAM_THREAD_RUNNING) {
        return 1;
    }
    if (!enabled && stream_state_cached == STREAM_THREAD_STOPPED) {
        return 0;
    }

    if (stream_state_cached == STREAM_THREAD_STOPPING) {
        pthread_mutex_lock(&stream_lock);
        stream_state_cached = stream_thread_state;
        pthread_mutex_unlock(&stream_lock);
        if (stream_state_cached != STREAM_THREAD_STOPPED || !enabled) {
            return 0; // the thread is still draining
        }
    }

    if (enabled) {
        pthread_t th;
        if (pthread_create(&th, NULL, stream_main, NULL)) {
            DebugPrint("stream: could not start the thread; streaming disabled\n");
            return 0;
        }
        pthread_detach(th);
        pthread_mutex_lock(&stream_lock);
        stream_thread_state = STREAM_THREAD_RUNNING;
        pthread_mutex_unlock(&stream_lock);
        stream_state_cached = STREAM_THREAD_RUNNING;
        DebugPrint("stream: thread started\n");
        return 1;
    }

    // Turned off while running. STOPPING goes out in the same critical section as the record:
    // released first, the thread could store STOPPED before it and leave STOPPING stuck.
    stream_rec_hdr_t hdr = {STREAM_REC_SHUTDOWN, 0, 0, 0, 0, 0};
    if (stream_put(&hdr, NULL) == 0) {
        pthread_mutex_lock(&stream_lock);
        stream_thread_state = STREAM_THREAD_STOPPING;
        pthread_mutex_unlock(&stream_lock);
        stream_state_cached = STREAM_THREAD_STOPPING;
        memset(stream_active, 0, sizeof(stream_active));
    } // ring full: retried on a later message
    return 0;
}

// Accumulated on the game thread until it fits. A block runs to 32KB and a gap record is 36
// bytes, so the report gets through a ring too full for the block.
static void stream_note_drop(int slot, int seq, uint8_t why) {
    if (!stream_gap_count[slot]) {
        stream_gap_first[slot]  = (uint32_t)seq;
        stream_gap_reason[slot] = why;

        // A forced full snapshot repairs a gap, so ask for one, once per episode. Once per block
        // would force this client a full snapshot every frame during a shed.
        if (!sv_demoStreamResync || sv_demoStreamResync->integer) {
            stream_resync_pending[slot] = 1;
        }
    }
    if (stream_gap_count[slot] < 0xFFFFFFFFu) {
        stream_gap_count[slot]++;
    }

    if (why == SGR_RING_FULL) {
        stream_bump(&stream_st_dropped_ring);
    } else if (why == SGR_LAGGING) {
        stream_bump(&stream_st_dropped_lag);
    } else {
        stream_bump(&stream_st_dropped_link);
    }
}

// Whether the stream is further behind than allowed. Hysteresis at half the watermark, so it does
// not flap on alternate messages. sv_demoStreamMaxLag 0 turns it off.
static int stream_over_lag(void) {
    int max_lag = sv_demoStreamMaxLag ? sv_demoStreamMaxLag->integer : 2000;
    if (max_lag <= 0) {
        stream_shedding = 0;
        return 0;
    }
    if (max_lag < 100) {
        max_lag = 100;
    } else if (max_lag > 60000) {
        max_lag = 60000;
    }

    unsigned lag = atomic_load_explicit(&stream_lag_ms, memory_order_relaxed);
    if (stream_shedding) {
        if (lag <= (unsigned)max_lag / 2) {
            stream_shedding = 0;
        }
    } else if (lag > (unsigned)max_lag) {
        stream_shedding = 1;
        DebugPrint("stream: %ums behind, shedding blocks until it catches up\n", lag);
    }
    return stream_shedding;
}

static void stream_flush_gap(int slot) {
    if (!stream_gap_count[slot]) {
        return;
    }
    stream_gap_t g = {stream_gap_first[slot], stream_gap_count[slot], stream_gap_reason[slot]};
    stream_rec_hdr_t hdr = {STREAM_REC_GAP, slot, 0, stream_gen[slot], 0, (uint32_t)sizeof(g)};
    if (stream_put(&hdr, &g) == 0) {
        stream_gap_count[slot] = 0;
    }
}

// The engine's own check in SV_SendClientSnapshot, read back. Nothing touches deltaMessage
// between the header write and the send. The two out-of-date fallbacks emit a zero delta byte
// with deltaMessage still above zero, so a false reading only delays re-arming by one frame.
static int block_is_full_snapshot(const client_t *client) {
    return (client->state != CS_ACTIVE) || (client->deltaMessage <= 0);
}

// An explicit per-slot request wins over the sv_demoStreamSlots mask.
static int stream_slot_wanted(int slot) {
    if (stream_request[slot] > 0) {
        return 1;
    }
    if (stream_request[slot] < 0) {
        return 0;
    }
    return (int)((stream_slot_mask >> slot) & 1ull);
}

qboolean Stream_Request(int slot, int mode) {
    if (slot < 0 || slot >= MAX_STREAM_CLIENTS) {
        return qfalse;
    }

    int8_t want = (mode > 0) ? 1 : (mode < 0 ? -1 : 0);
    if (stream_request[slot] == want) {
        return qtrue;
    }

    if (stream_request[slot] > 0) {
        stream_forced_on--;
    }
    if (want > 0) {
        stream_forced_on++;
    }
    stream_request[slot] = want;
    return qtrue;
}

int Stream_GetRequest(int slot) {
    if (slot < 0 || slot >= MAX_STREAM_CLIENTS) {
        return 0;
    }
    return stream_request[slot];
}

// Compared against stream_link_up instead of counting transitions, so a flap between two frames
// reports nothing. Seeded 0, so a server that never streams never says "disconnected".
static int stream_link_seen;

qboolean Stream_PollLinkChange(stream_link_event_t *out) {
    int up = atomic_load_explicit(&stream_link_up, memory_order_relaxed);
    if (up == stream_link_seen) {
        return qfalse;
    }
    stream_link_seen = up;

    memset(out, 0, sizeof(*out));
    out->connected = up;
    if (sv_demoStreamHost && sv_demoStreamHost->string[0]) {
        snprintf(out->endpoint, sizeof(out->endpoint), "%s:%s", sv_demoStreamHost->string,
                 sv_demoStreamPort ? sv_demoStreamPort->string : "");
    }
    if (!up) {
        pthread_mutex_lock(&stream_lock);
        snprintf(out->error, sizeof(out->error), "%s", stream_error);
        pthread_mutex_unlock(&stream_lock);
    }
    return qtrue;
}

qboolean Stream_IsStreaming(int slot) {
    if (slot < 0 || slot >= MAX_STREAM_CLIENTS) {
        return qfalse;
    }
    return stream_active[slot] ? qtrue : qfalse;
}

// Drops every override. Kept out of Stream_CloseAll: a map change keeps the same clients in the
// same slots. SV_Shutdown never calls SV_DropClient, so nothing else clears them.
void Stream_ClearRequests(void) {
    for (int slot = 0; slot < MAX_STREAM_CLIENTS; slot++) {
        Stream_Request(slot, 0); // keeps stream_forced_on in step
    }
}

int Stream_Interested(int slot) {
    if (slot < 0 || slot >= MAX_STREAM_CLIENTS) {
        return 0;
    }
    // A live thread still needs reconciling and an open POV still needs closing, whatever the cvar says.
    if (stream_state_cached != STREAM_THREAD_STOPPED || stream_active[slot] ||
        stream_request[slot] > 0) {
        return 1;
    }
    return stream_enabled();
}

int Stream_Want(int slot, int is_gamestate, client_t *client) {
    if (slot < 0 || slot >= MAX_STREAM_CLIENTS) {
        return 0;
    }

    if (!stream_enabled() || !stream_slot_wanted(slot)) {
        if (stream_active[slot]) {
            stream_rec_hdr_t hdr = {STREAM_REC_CLOSE, slot, SCR_DISABLED, stream_gen[slot], 0, 0};
            stream_put(&hdr, NULL);
            stream_active[slot] = 0;
        }
        return 0;
    }
    if (!stream_reconcile_thread()) {
        return 0;
    }

    if (is_gamestate) {
        if (stream_active[slot]) {
            stream_rec_hdr_t hdr = {STREAM_REC_CLOSE, slot, SCR_GAMESTATE, stream_gen[slot], 0, 0};
            stream_put(&hdr, NULL);
            stream_active[slot] = 0;
        }

        stream_open_t open;
        memset(&open, 0, sizeof(open));
        open.steam_id      = client->steam_id;
        open.gamestate_seq = (uint32_t)client->netchan.outgoingSequence;
        open.command_seq   = (uint32_t)client->reliableSequence;
        snprintf(open.name, sizeof(open.name), "%s", client->name);

        stream_rec_hdr_t hdr = {
            STREAM_REC_OPEN, slot, 0, stream_gen[slot] + 1, 0, (uint32_t)sizeof(open)};
        if (stream_put(&hdr, &open) != 0) {
            return 0; // ring full at an open; retry at this client's next gamestate
        }
        stream_gen[slot]            = hdr.gen;
        stream_active[slot]         = 1;
        stream_gap_count[slot]      = 0;
        stream_resync_pending[slot] = 0;
        stream_last_full_ms[slot]   = stream_now_ms();
        snprintf(stream_name[slot], sizeof(stream_name[slot]), "%s", client->name);
        // Queued whether the link is up or not: the thread caches and replays it, and a relay that
        // reconnects mid-map cannot decode this POV without it.
        return 1;
    }

    if (!stream_active[slot]) {
        return 0; // no gamestate seen for this slot yet
    }

    // Link down: drop at the source. The loss is counted here and declared once the link is back.
    if (!atomic_load_explicit(&stream_link_up, memory_order_relaxed)) {
        stream_note_drop(slot, client->netchan.outgoingSequence, SGR_LINK_DOWN);
        return 0;
    }

    // Too far behind. Dropping here saves the copy, and the gap tells the relay to start again
    // from the full snapshot this arranges. Gamestates return above.
    if (stream_over_lag()) {
        stream_note_drop(slot, client->netchan.outgoingSequence, SGR_LAGGING);
        return 0;
    }

    stream_flush_gap(slot);
    return 1;
}

void Stream_Block(int slot, int seq, int is_gamestate, client_t *client, const unsigned char *data,
                  uint32_t len) {
    if (slot < 0 || slot >= MAX_STREAM_CLIENTS) {
        return;
    }

    uint32_t flags = 0;
    if (is_gamestate) {
        flags |= SBF_GAMESTATE;
    } else if (block_is_full_snapshot(client)) {
        flags |= SBF_FULL;
        stream_resync_pending[slot] = 0; // the resync this slot was waiting for has gone out
    }

    stream_rec_hdr_t hdr = {STREAM_REC_BLOCK, slot, seq, stream_gen[slot], flags, len};
    if (stream_put(&hdr, data) != 0) {
        stream_note_drop(slot, seq, SGR_RING_FULL);
    } else if (flags & (SBF_FULL | SBF_GAMESTATE)) {
        stream_last_full_ms[slot] = stream_now_ms();
    }
}

void Stream_ClientDisconnect(int slot) {
    if (slot < 0 || slot >= MAX_STREAM_CLIENTS) {
        return;
    }
    stream_gap_count[slot]      = 0;
    stream_resync_pending[slot] = 0;
    // Drop the override too: the next player in this slot shouldn't inherit it.
    Stream_Request(slot, 0);
    if (stream_active[slot] && stream_state_cached == STREAM_THREAD_RUNNING) {
        stream_rec_hdr_t hdr = {STREAM_REC_CLOSE, slot, SCR_DISCONNECT, stream_gen[slot], 0, 0};
        stream_put(&hdr, NULL);
    }
    stream_active[slot] = 0;
}

static void stream_close_all(int reason) {
    memset(stream_active, 0, sizeof(stream_active));
    memset(stream_gap_count, 0, sizeof(stream_gap_count));
    memset(stream_resync_pending, 0, sizeof(stream_resync_pending));
    stream_serverid_valid = 0;
    stream_guid_seen[0]   = '\0';
    if (stream_state_cached == STREAM_THREAD_RUNNING) {
        stream_rec_hdr_t hdr = {STREAM_REC_CLOSE_ALL, 0, reason, 0, 0, 0};
        stream_put(&hdr, NULL);
    }
}

void Stream_CloseAll(void) { stream_close_all(SCR_MAP_CHANGE); }

void Stream_DrainClose(void) {
    if (stream_state_cached != STREAM_THREAD_RUNNING) {
        return;
    }
    stream_close_all(SCR_SHUTDOWN);

    // No other thread puts, so the count standing now is the record just queued. The thread
    // flushes in the pass that consumes it, so at the target the closes have reached the socket.
    unsigned long long target = atomic_load_explicit(&stream_seq_put, memory_order_relaxed);
    uint64_t deadline         = stream_now_ms() + STREAM_DRAIN_MS;
    while (atomic_load_explicit(&stream_seq_done, memory_order_acquire) < target) {
        if (stream_now_ms() >= deadline) {
            DebugPrint("stream: closes not taken within %d ms; the relay sees a truncated stream\n",
                       STREAM_DRAIN_MS);
            return;
        }
        usleep(1000);
    }
}

void Stream_Reconnect(void) {
    // Also re-reads a token file whose contents changed under the same path.
    stream_token_cached = 0;
    stream_cfg_seen[0]  = '\0';

    if (stream_state_cached != STREAM_THREAD_RUNNING) {
        return;
    }
    stream_rec_hdr_t hdr = {STREAM_REC_RECONNECT, 0, 0, 0, 0, 0};
    stream_put(&hdr, NULL);
}

void Stream_ResyncAll(void) {
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (stream_active[i]) {
            stream_resync_pending[i] = 1;
        }
    }
}

void Stream_Reset(void) {
    atomic_store_explicit(&stream_st_high_water, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_st_sent, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_st_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_st_dropped_ring, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_st_dropped_link, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_st_dropped_stall, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_st_dropped_lag, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_st_gaps, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_st_resyncs, 0, memory_order_relaxed);
    atomic_store_explicit(&stream_st_reconnects, 0, memory_order_relaxed);
    stream_note_error("");
}

static void stream_parse_slots(void) {
    const char *s = sv_demoStreamSlots ? sv_demoStreamSlots->string : "";
    if (!strcmp(s, stream_slots_seen)) {
        return;
    }
    snprintf(stream_slots_seen, sizeof(stream_slots_seen), "%s", s);

    if (!s[0]) {
        stream_slot_mask = ~0ull;
        return;
    }
    uint64_t mask = 0;
    for (const char *p = s; *p;) {
        while (*p == ',' || *p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        char *end = NULL;
        long n    = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        if (n >= 0 && n < MAX_STREAM_CLIENTS) {
            mask |= 1ull << n;
        }
        p = end;
    }
    stream_slot_mask = mask;
}

// A token from a file keeps the secret out of a cvar dump and out of rcon "cvarlist". Read when
// the path changes, since this runs on the game thread. "qlx_stream reconnect" re-reads a file
// whose contents changed under the same path.
static void stream_read_token(char *out, size_t n) {
    const char *path = sv_demoStreamTokenFile ? sv_demoStreamTokenFile->string : "";

    if (!stream_token_cached || strcmp(path, stream_tokenfile_seen)) {
        snprintf(stream_tokenfile_seen, sizeof(stream_tokenfile_seen), "%s", path);
        stream_token_cached   = 1;
        stream_token_cache[0] = '\0';

        if (path[0]) {
            FILE *fh = fopen(path, "rb");
            if (!fh) {
                DebugPrint("stream: could not read the token file %s\n", path);
            } else {
                size_t got = fread(stream_token_cache, 1, sizeof(stream_token_cache) - 1, fh);
                fclose(fh);
                stream_token_cache[got] = '\0';
                for (size_t i = 0; i < got; i++) {
                    if (stream_token_cache[i] == '\n' || stream_token_cache[i] == '\r') {
                        stream_token_cache[i] = '\0';
                        break;
                    }
                }
            }
        }
    }

    if (stream_token_cache[0]) {
        snprintf(out, n, "%s", stream_token_cache);
        return;
    }
    snprintf(out, n, "%s", sv_demoStreamToken ? sv_demoStreamToken->string : "");
}

static void stream_publish_cfg(void) {
    char token[64];
    stream_read_token(token, sizeof(token));

    char seen[256];
    snprintf(seen, sizeof(seen), "%s|%s|%s", sv_demoStreamHost->string,
             sv_demoStreamPort ? sv_demoStreamPort->string : "", token);
    if (!strcmp(seen, stream_cfg_seen)) {
        return;
    }
    snprintf(stream_cfg_seen, sizeof(stream_cfg_seen), "%s", seen);

    uint64_t h = stream_hash(token, strlen(token), 14695981039346656037ull);
    snprintf(stream_fingerprint, sizeof(stream_fingerprint), "%08x", (unsigned)(h >> 32));

    pthread_mutex_lock(&stream_lock);
    snprintf(stream_endpoint.host, sizeof(stream_endpoint.host), "%s", sv_demoStreamHost->string);
    snprintf(stream_endpoint.port, sizeof(stream_endpoint.port), "%s",
             sv_demoStreamPort ? sv_demoStreamPort->string : "");
    snprintf(stream_endpoint.token, sizeof(stream_endpoint.token), "%s", token);
    pthread_mutex_unlock(&stream_lock);
    // Released, so a thread that sees the new generation also sees the endpoint behind it.
    atomic_fetch_add_explicit(&stream_endpoint_gen, 1, memory_order_release);

    // Turning this on should leave a line saying where the POVs are going.
    ENGINE_PRINTF(DEBUG_PRINT_PREFIX "demo stream: sending every POV to %s:%s (token %s)\n",
                  sv_demoStreamHost->string,
                  sv_demoStreamPort ? sv_demoStreamPort->string : "", token[0] ? stream_fingerprint : "none");
}

static void stream_publish_timers(void) {
    atomic_store_explicit(&stream_cfg_heartbeat_ms,
                          cvar_clamped(sv_demoStreamHeartbeat, 5000, 1000, 60000),
                          memory_order_relaxed);
    atomic_store_explicit(&stream_cfg_timeout_ms,
                          cvar_clamped(sv_demoStreamTimeout, 5000, 500, 60000),
                          memory_order_relaxed);
    atomic_store_explicit(&stream_cfg_stall_ms, cvar_clamped(sv_demoStreamStall, 2000, 200, 60000),
                          memory_order_relaxed);
    atomic_store_explicit(&stream_cfg_drop_ms,
                          cvar_clamped(sv_demoStreamDrop, 10000, 1000, 300000),
                          memory_order_relaxed);
    atomic_store_explicit(&stream_cfg_max_clients,
                          sv_maxclients ? sv_maxclients->integer : MAX_STREAM_CLIENTS,
                          memory_order_relaxed);
    atomic_store_explicit(&stream_cfg_fps, cvar_clamped(sv_fps, 40, 1, 250), memory_order_relaxed);
    if (sv_steamAccount && sv_steamAccount->string[0]) {
        atomic_store_explicit(&stream_cfg_steam_id, strtoull(sv_steamAccount->string, NULL, 10),
                              memory_order_relaxed);
    }
}

static void stream_put_configstrings(int serverid, const unsigned char *buf, uint32_t used,
                                     uint32_t count, uint32_t flags) {
    stream_rec_hdr_t hdr = {STREAM_REC_CONFIGSTRINGS, 0, serverid, count, flags, used};
    stream_put(&hdr, buf);
}

// Configstrings for the relay to apply over a replayed gamestate, whose own copies are as old as
// the block. Chunked, so one scratch buffer on the thread serves every record type.
static void stream_publish_configstrings(int serverid) {
    if (!sv) {
        return;
    }

    unsigned char buf[STREAM_CS_CHUNK];
    uint32_t used  = 0;
    uint32_t count = 0;
    int first      = 1;

    for (int i = 0; i < MAX_CONFIGSTRINGS; i++) {
        const char *value = sv->configstrings[i];
        if (!value || !value[0]) {
            continue;
        }
        size_t vlen = strlen(value);
        if (vlen > 0xFFFF || 4 + vlen > sizeof(buf)) {
            continue; // cannot be framed; nothing this large is a real configstring
        }
        if (used + 4 + vlen > sizeof(buf)) {
            stream_put_configstrings(serverid, buf, used, count, first ? SCF_FIRST : 0);
            first = 0;
            used  = 0;
            count = 0;
        }
        wr_u16(buf + used, (uint16_t)i);
        wr_u16(buf + used + 2, (uint16_t)vlen);
        memcpy(buf + used + 4, value, vlen);
        used += (uint32_t)(4 + vlen);
        count++;
    }

    // Always a final chunk, empty if need be, so the relay knows the set is complete.
    stream_put_configstrings(serverid, buf, used, count, (first ? SCF_FIRST : 0) | SCF_LAST);
}

// The match GUID changes every match and the full set only goes out per map or reconnect. Every
// later "cs 712" is inside a block the relay does not decode, so send the change as a patch.
static void stream_publish_guid(int serverid) {
    if (!sv) {
        return;
    }
    const char *guid = sv->configstrings[CS_MATCH_GUID];
    if (!guid) {
        guid = "";
    }
    if (!strcmp(guid, stream_guid_seen)) {
        return;
    }
    snprintf(stream_guid_seen, sizeof(stream_guid_seen), "%s", guid);

    size_t vlen = strlen(guid);
    if (vlen > sizeof(stream_guid_seen) - 1) {
        return;
    }
    unsigned char buf[4 + sizeof(stream_guid_seen)];
    wr_u16(buf, CS_MATCH_GUID);
    wr_u16(buf + 2, (uint16_t)vlen);
    memcpy(buf + 4, guid, vlen);
    stream_put_configstrings(serverid, buf, (uint32_t)(4 + vlen), 1, 0);
}

static void stream_publish_serverinfo(int serverid) {
    if (!sv || !sv->configstrings[CS_SERVERINFO]) {
        return;
    }
    const char *info = sv->configstrings[CS_SERVERINFO];
    uint32_t len     = (uint32_t)strlen(info);
    if (len >= MAX_INFO_STRING) {
        len = MAX_INFO_STRING - 1;
    }
    stream_rec_hdr_t hdr = {STREAM_REC_SERVERINFO, 0, serverid, 0, 0, len};
    stream_put(&hdr, info);
}

// 0 when keyframes are off, else the interval in ms.
static uint64_t stream_keyframe_ms(void) {
    if (!sv_demoStreamKeyframeSec) {
        return 10 * 1000;
    }
    if (sv_demoStreamKeyframeSec->integer <= 0) {
        return 0;
    }
    return (uint64_t)cvar_clamped(sv_demoStreamKeyframeSec, 10, 2, 60) * 1000;
}

// A relay can start a viewer only at a full snapshot. This bounds how far back the last one is by
// arming the same resync flag a gap does; stream_force_resyncs spends the per-frame budget on it.
static void stream_arm_keyframes(void) {
    if (!svs || !svs->clients || !sv_maxclients) {
        return;
    }
    if (sv_demoStreamResync && !sv_demoStreamResync->integer) {
        return;
    }
    uint64_t interval = stream_keyframe_ms();
    if (interval == 0) {
        return;
    }
    if (!atomic_load_explicit(&stream_link_up, memory_order_relaxed)) {
        return;
    }
    if (stream_shedding) {
        return; // a full snapshot is the largest message; do not add one while behind
    }
    int limit = sv_maxclients->integer;
    if (limit > MAX_STREAM_CLIENTS) {
        limit = MAX_STREAM_CLIENTS;
    }
    if (limit <= 0) {
        return;
    }

    uint64_t now = stream_now_ms();
    for (int slot = 0; slot < limit; slot++) {
        if (stream_active[slot] && !stream_resync_pending[slot] &&
            now - stream_last_full_ms[slot] >= interval) {
            stream_resync_pending[slot] = 1;
        }
    }
}

// A forced full snapshot means a fragmented message and a visible pause for that client. Doing
// every client at once hitches the whole server, so one per frame by default.
static void stream_force_resyncs(void) {
    if (!svs || !svs->clients || !sv_maxclients) {
        return;
    }
    if (sv_demoStreamResync && !sv_demoStreamResync->integer) {
        return;
    }

    int budget = cvar_clamped(sv_demoStreamResyncPerFrame, 1, 1, MAX_STREAM_CLIENTS);
    int limit  = sv_maxclients->integer;
    if (limit > MAX_STREAM_CLIENTS) {
        limit = MAX_STREAM_CLIENTS;
    }
    if (limit <= 0) {
        return;
    }

    for (int n = 0; n < limit && budget > 0; n++) {
        int slot = (stream_resync_cursor + n) % limit;
        if (!stream_resync_pending[slot] || !stream_active[slot]) {
            continue;
        }
        client_t *cl = &svs->clients[slot];
        if (cl->state != CS_ACTIVE || cl->netchan.unsentFragments) {
            continue; // it would not be sent a snapshot this frame anyway
        }
        cl->deltaMessage     = -1;
        stream_resync_cursor = (slot + 1) % limit;
        budget--;
        stream_bump(&stream_st_resyncs);
    }
}

void Stream_Frame(void) {
    if (!sv_demoStream) {
        return;
    }

    // The clock stream_put stamps records with; see stream_lag_ms.
    atomic_store_explicit(&stream_frame_ms, (uint32_t)stream_now_ms(), memory_order_relaxed);

    int on = stream_enabled();
    if (stream_mirror_seen != on) {
        stream_mirror_seen = on;
        // Guarded on the engine pointer: in a Python build the macro is the hook and never null.
        if (Cvar_Set2) {
            ENGINE_CVAR_SET2("sv_demoStreaming", on ? "1" : "0", qtrue);
        }
    }

    if (!on) {
        if (stream_state_cached != STREAM_THREAD_STOPPED) {
            stream_reconcile_thread();
        }
        return;
    }

    stream_parse_slots();
    stream_publish_cfg();
    stream_publish_timers();

    if (!stream_reconcile_thread()) {
        return;
    }

    // A new serverId is a new map. The relay needs its serverinfo and configstrings first.
    int serverid = sv ? sv->serverId : 0;
    if (!stream_serverid_valid || serverid != stream_serverid_seen) {
        stream_serverid_seen  = serverid;
        stream_serverid_valid = 1;
        stream_publish_serverinfo(serverid);
        stream_publish_configstrings(serverid);
    }

    // The thread bumps the epoch when a handshake completes. Everything open re-anchors.
    unsigned epoch = atomic_load_explicit(&stream_resync_epoch, memory_order_relaxed);
    if (epoch != stream_epoch_seen) {
        stream_epoch_seen = epoch;
        stream_publish_serverinfo(serverid);
        stream_publish_configstrings(serverid);
        Stream_ResyncAll();
    }

    stream_publish_guid(serverid);
    stream_arm_keyframes();
    stream_force_resyncs();
}

void Stream_Status(stream_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->enabled   = stream_enabled();
    out->connected = atomic_load_explicit(&stream_link_up, memory_order_relaxed);
    if (sv_demoStreamHost && sv_demoStreamHost->string[0]) {
        snprintf(out->endpoint, sizeof(out->endpoint), "%s:%s", sv_demoStreamHost->string,
                 sv_demoStreamPort ? sv_demoStreamPort->string : "");
    }
    snprintf(out->fingerprint, sizeof(out->fingerprint), "%s", stream_fingerprint);
    out->pending = stream_pending_bytes();
    out->lag     = atomic_load_explicit(&stream_lag_ms, memory_order_relaxed);

    out->high_water    = atomic_load_explicit(&stream_st_high_water, memory_order_relaxed);
    out->sent          = atomic_load_explicit(&stream_st_sent, memory_order_relaxed);
    out->frames        = atomic_load_explicit(&stream_st_frames, memory_order_relaxed);
    out->dropped_ring  = atomic_load_explicit(&stream_st_dropped_ring, memory_order_relaxed);
    out->dropped_link  = atomic_load_explicit(&stream_st_dropped_link, memory_order_relaxed);
    out->dropped_stall = atomic_load_explicit(&stream_st_dropped_stall, memory_order_relaxed);
    out->dropped_lag   = atomic_load_explicit(&stream_st_dropped_lag, memory_order_relaxed);
    out->gaps          = atomic_load_explicit(&stream_st_gaps, memory_order_relaxed);
    out->resyncs       = atomic_load_explicit(&stream_st_resyncs, memory_order_relaxed);
    out->reconnects    = atomic_load_explicit(&stream_st_reconnects, memory_order_relaxed);

    pthread_mutex_lock(&stream_lock);
    snprintf(out->error, sizeof(out->error), "%s", stream_error);
    pthread_mutex_unlock(&stream_lock);

    int limit = (sv_maxclients && sv_maxclients->integer > 0 &&
                 sv_maxclients->integer < MAX_STREAM_CLIENTS)
                    ? sv_maxclients->integer
                    : MAX_STREAM_CLIENTS;
    out->slot_count = limit;
    for (int i = 0; i < limit; i++) {
        out->slots[i].slot      = i;
        out->slots[i].active    = stream_active[i];
        out->slots[i].desynced  = atomic_load_explicit(&stream_desynced[i], memory_order_relaxed);
        out->slots[i].resyncing = stream_resync_pending[i];
        out->slots[i].requested = stream_request[i];
        out->slots[i].gen       = stream_gen[i];
        // Whole-row copy: both are char[40] and the source row is always terminated.
        memcpy(out->slots[i].name, stream_name[i], sizeof(out->slots[i].name));
        out->slots[i].name[sizeof(out->slots[i].name) - 1] = '\0';
    }
}

void Stream_Report(void) {
    stream_status_t st;
    Stream_Status(&st);

    ENGINE_PRINTF("Demo stream: %s, endpoint %s, link %s.\n", st.enabled ? "on" : "off",
                  st.endpoint[0] ? st.endpoint : "(unset)", st.connected ? "up" : "down");
    ENGINE_PRINTF("Token %s. %llu frames, %llu bytes sent. Ring %u now, %u deepest, %ums behind.\n",
                  st.fingerprint[0] ? st.fingerprint : "(none)", st.frames, st.sent, st.pending,
                  st.high_water, st.lag);
    ENGINE_PRINTF("Dropped: %u ring full, %u link down, %u relay stalled, %u too far behind. "
                  "%u gaps, %u resyncs, %u reconnects.\n",
                  st.dropped_ring, st.dropped_link, st.dropped_stall, st.dropped_lag, st.gaps,
                  st.resyncs, st.reconnects);
    if (st.error[0]) {
        ENGINE_PRINTF("Last error: %s\n", st.error);
    }

    ENGINE_PRINTF("slot  open  desync  resync  gen  name\n");
    for (int i = 0; i < st.slot_count; i++) {
        if (!st.slots[i].active && !st.slots[i].desynced) {
            continue;
        }
        ENGINE_PRINTF("%4d  %4d  %6d  %6d  %3u  %s\n", st.slots[i].slot, st.slots[i].active,
                      st.slots[i].desynced, st.slots[i].resyncing, st.slots[i].gen,
                      st.slots[i].name);
    }
}

void Stream_Init(void) {
    if (!Cvar_Get) {
        return;
    }

    if (!stream_cond_ready) {
        // CLOCK_MONOTONIC, so a wall-clock step cannot stretch a backoff. PTHREAD_COND_INITIALIZER
        // would use CLOCK_REALTIME.
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&stream_cond, &attr);
        pthread_condattr_destroy(&attr);
        stream_cond_ready = 1;
    }

    sv_demoStream     = Cvar_Get("sv_demoStream", "0", CVAR_ARCHIVE);
    sv_demoStreamHost = Cvar_Get("sv_demoStreamHost", "", CVAR_ARCHIVE);
    sv_demoStreamPort = Cvar_Get("sv_demoStreamPort", "27999", CVAR_ARCHIVE);
    // Never CVAR_SERVERINFO or CVAR_SYSTEMINFO: those go out to every client that connects.
    sv_demoStreamToken          = Cvar_Get("sv_demoStreamToken", "", CVAR_ARCHIVE);
    sv_demoStreamTokenFile      = Cvar_Get("sv_demoStreamTokenFile", "", CVAR_ARCHIVE);
    sv_demoStreamSlots          = Cvar_Get("sv_demoStreamSlots", "", CVAR_ARCHIVE);
    sv_demoStreamHeartbeat      = Cvar_Get("sv_demoStreamHeartbeat", "5000", CVAR_ARCHIVE);
    sv_demoStreamTimeout        = Cvar_Get("sv_demoStreamTimeout", "5000", CVAR_ARCHIVE);
    sv_demoStreamStall          = Cvar_Get("sv_demoStreamStall", "2000", CVAR_ARCHIVE);
    sv_demoStreamDrop           = Cvar_Get("sv_demoStreamDrop", "10000", CVAR_ARCHIVE);
    // How far behind live the stream may fall before blocks are dropped. 0 lets the ring fill,
    // about eighty seconds on a busy server.
    sv_demoStreamMaxLag = Cvar_Get("sv_demoStreamMaxLag", "2000", CVAR_ARCHIVE);
    sv_demoStreamResync         = Cvar_Get("sv_demoStreamResync", "1", CVAR_ARCHIVE);
    sv_demoStreamResyncPerFrame = Cvar_Get("sv_demoStreamResyncPerFrame", "1", CVAR_ARCHIVE);
    // Forces a full snapshot on each streamed POV at least this often, so a relay can start late
    // viewers. 0 is off, otherwise 2..60 seconds. No effect with sv_demoStreamResync 0.
    sv_demoStreamKeyframeSec = Cvar_Get("sv_demoStreamKeyframeSec", "10", CVAR_ARCHIVE);

    // Tells a player the server streams. The host and token stay out of it.
    sv_demoStreaming = Cvar_Get("sv_demoStreaming", "0", CVAR_SERVERINFO | CVAR_ROM);

    sv_fps          = Cvar_FindVar("sv_fps");
    sv_steamAccount = Cvar_FindVar("sv_steamAccount");

    stream_publish_timers();
}
