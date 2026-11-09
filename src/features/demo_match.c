// demo_match.c - per-match orchestration on top of upstream's demos.c capture.
//
// ---------------------------------------------------------------------------
// WHY THIS FILE EXISTS, AND WHY IT IS SO MUCH SMALLER THAN WHAT IT REPLACES
//
// Before minqlxtended v1.0.0 this feature was a whole-body replacement of
// demos.c: a per-slot capture state machine (IDLE -> SEEDED -> RECORDING), a
// hand-rolled per-slot prebuffer that hoarded every S2C message from a client's
// connection-time gamestate onwards, extra ring op-codes to hand that prebuffer
// to the writer thread, and our own writer thread on the other end of it.
//
// All of that existed for ONE reason: the old demos.c could only record "every
// connected client, whenever sv_demoRecord is 1", so there was no way to say
// "start recording THIS slot, from ITS OWN gamestate, when the match arms" -
// and a .dm_91 is a delta chain whose first message MUST be a gamestate, so
// starting late is not an option. The prebuffer was that missing mechanism.
//
// Upstream now ships the mechanism itself:
//
//   Demo_Request(slot, 1)  -> demo_request[slot] = 1
//   Demo_CaptureBody()     -> if (!demo_slot_wanted(slot)) return;   // reads it
//                             if (is_gamestate) { ...open a fresh segment... }
//
// and with sv_demoRecord set, upstream itself opens a segment at every
// client's own gamestate, moments after they connect. That is byte-for-byte
// the outcome the prebuffer was reconstructing by hand, produced by code that
// is already in the binary and already maintained upstream. The prebuffer, the
// extra op-codes and our writer thread are therefore gone, and demo_slot.c/.h
// with them.
//
// Capture is therefore entirely sv_demoRecord's: this file never asks for a
// slot to be recorded. What it adds, under sv_demoCut, is knowing WHERE the
// matches are inside those captures - match start and end are read off the
// engine's own state by game_events.c, which calls the DemoMatch_OnGame*()
// entry points below - and what upstream does not do with that knowledge:
//   - attribute a segment to a match_id / map - for the PLAYERS only: a
//     spectator's capture is a camera follow, so it is neither packed into the
//     match nor kept, while someone who joins the game mid-match gets a POV
//     starting sv_demoCutJoinLead seconds before their join;
//   - name the shipped file after that match instead of the wall clock;
//   - the two-stage cut (countdown trim, then one shared server-time window
//     across every POV of the match) and the per-POV snapshot index;
//   - trim the pre-match warmup out of captures no match claimed;
//   - the demo_recording_started / demo_match_finalized Python events.
//
// ---------------------------------------------------------------------------
// THREADING
//
// Everything above the "finalize thread" banner is GAME THREAD ONLY, the same
// contract upstream puts on Demo_Request/Demo_GetPath/Demo_PollFinished. The
// only cross-thread hand-off is demo_finalize_push(), which is locked.
//
// The finalize thread is NOT part of the machinery upstream replaced, and is
// kept deliberately: demo_cut()/demo_scan()/demo_index() each walk (and the cut
// rewrites) a whole demo file, which is tens to hundreds of milliseconds on a
// real match. Doing that from the frame hook would hitch the server for every
// POV of every match.
//
// ---------------------------------------------------------------------------
// WHERE THE FILES LIVE
//
// Upstream picks the capture path itself, from sv_demoDir + sv_demoNameFormat,
// under fs_homepath - we do not get to choose it, and there is no longer a
// tmpfs staging directory (sv_demoRawDir/sv_demoPrebufMaxMB are retired). So a
// raw capture lands in the demo directory next to the finished files, and this
// file cuts it, publishes the result under the match-named path and unlinks the
// raw one. Failure paths leave the raw capture in place, which is a valid demo
// under an ordinary upstream name rather than a lost recording.
//
// A capture is NOT ended when the match inside it ends. It is copied, under a
// dot-prefixed name in a ".sn_cache" staging directory beside the demos, and the
// copy is what gets cut; the capture itself keeps running. That is what lets two
// matches be played back-to-back on one map: the map_restart that starts the
// second one sends no fresh gamestate, and a .dm_91 must begin at one, so a
// capture closed at the end of match 1 could never be reopened for match 2.
// Once such a capture finally does end, its own file is removed rather than
// trimmed and kept - every match in it has already shipped from its own copy.
//
// A capture that no match ever claims - a client who connected and left during
// warmup, an aborted countdown, a warmup-only session - is handed to the same
// finalize thread as a trim-only job: cut down to [went-live, end] if a match
// went live inside it, deleted if one never did. Both are what sv_demoCut asks
// for; with it off, this file leaves every capture exactly as upstream wrote it.
//
// The shipped name is
// "{match_id}_{map}_p{slot}_{name}_{seg_time}_{seg_id}.dm_91", so a consumer
// (an archiving plugin, say) finds a match's POVs by globbing
// "{match_id}_*.dm_91" and parses the rest of the stem back out. That glob
// cannot collide with the plain raw names: those start with "%Y%m%d-%H%M%S"
// and a match_id starts with "%Y%m%dT%H%M%SZ".

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// First, ahead of any system header: Python.h sets _POSIX_C_SOURCE and _XOPEN_SOURCE.
#ifndef NOPY
#include "python/pyminqlxtended.h"
#endif

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "features/demo_match.h"
#include "features/demo_name.h"
#include "features/demos.h"
#include "engine/quake_common.h"
#include "democut/democut.h"

extern serverStatic_t* svs; // defined in dllmain.c

#define MAX_DEMO_CLIENTS 64 // QL MAX_CLIENTS, same constant demos.c uses

#define DEMO_FINALIZE_QUEUE_SIZE 128u
#define DEMO_COPY_CHUNK          (256u * 1024u)

// How many POVs of one match the finalize thread can hold open across stage 1
// while it waits for that match's last job. One entry per finalised segment,
// not per player: a slot can legitimately produce several segments in one match
// (a re-gamestate closes one and opens the next), so this is deliberately
// larger than MAX_DEMO_CLIENTS. Overflow is handled, not fatal - see demo_stage1().
#define DEMO_MAX_POVS_PER_MATCH 128

// Longer than the 520-byte capture/final paths because the stage directories
// and the generated "<name>_CUT_<mm>_<ss>.dm_91" output name are built on
// top of them.
#define DEMO_LONG_PATH 768

// Below this, the shared window is not a match. 5s is "a few seconds" and is
// comfortably shorter than any real QL match while still being ~200 snapshots
// at sv_fps 40, so it can never trip on a normal game - the realistic way to
// land under it is an intersection that is empty or nearly so because one POV
// barely overlaps the others (a player who connected seconds before game_end).
#define DEMO_MIN_WINDOW_MS 5000

// How far from the earliest start (or the latest end) a POV may sit and still
// help DEFINE the shared window, rather than be treated as an outlier that the
// window is computed without.
//
// The number is not a guess: stage 1 normalises every POV's start to the first
// snapshot at or after its own arm instant, and every POV of a match sits on the
// same server snapshot grid, so POVs that were genuinely present at the
// countdown come out of stage 1 within about ONE snapshot frame of each other
// (measured pre-port: three real POVs armed at three very different points in
// the capture all produced stage-1 files starting at exactly 201750 ms, i.e.
// 25 ms after the shared arm time of 201725). Anything starting more than 30
// seconds later than the earliest POV is therefore not measurement noise - it
// is a client that connected mid-match, which this file deliberately allows.
//
// Symmetrically for the end: a player who disconnects before game_end finalises
// their POV early, and that early last_ms must not be allowed to truncate
// everyone else's demo.
//
// Without this, a SINGLE such POV silently defined the whole match's window:
// max(first_ms)/min(last_ms) over every POV means the latest joiner and the
// earliest leaver each get to discard everything outside their own range for
// every other POV. Outliers are excluded from the computation instead, and any
// POV that then cannot be trimmed to the resulting window ships its stage-1
// file instead of forcing the rest of the match down to its range - see
// demo_stage2_flush().
#define DEMO_OUTLIER_WINDOW_MS 30000

// How long (wall seconds) demo_disarm_match waits for a match's still-open
// segments to come back through the completion queue before firing the
// match-finalized marker anyway. A segment is closed by upstream on that
// client's next outgoing message, so this is normally a frame or two; the
// deadline only exists so a client that stops receiving messages entirely
// cannot strand a match's .qlmatch forever.
#define DEMO_CLOSE_TIMEOUT_S 30

// How many matches can be draining their last segments at once. A new match's
// game_countdown can legitimately arrive while the previous match's segments
// are still closing.
#define DEMO_MAX_CLOSING 4

static cvar_t* sv_demoDir;
static cvar_t* sv_demoRecord;
static cvar_t* fs_homepath;
static cvar_t* mapname;
static cvar_t* sv_demoCut;
static cvar_t* sv_demoCutJoinLead;

// Fallback when sv_demoCutJoinLead holds nonsense, and the ceiling that stops a
// large value from reaching back into the warmup it is the point of this file to
// trim off. 10 minutes is longer than any sane lead and shorter than a long
// warmup, so a typo cannot silently turn a match POV back into a full capture.
#define DEMO_JOIN_LEAD_DEFAULT_S 15
#define DEMO_JOIN_LEAD_MAX_S     600

// CS_MATCH_GUID as stored here: a 36-character lower-case UUID plus its NUL,
// rounded up. See demo_match_guid_read for what counts as valid.
#define DEMO_MATCH_GUID_LEN  36
#define DEMO_MATCH_GUID_SIZE 40

// ---------------------------------------------------------------------------
// Small helpers, ported unchanged from the pre-port demos.c body. They are
// static there too, so these are separate copies rather than a shared symbol -
// demos.c stays untouched upstream code. The one exception is the name
// sanitiser, Demo_SanitiseName() in demo_name.c: both files build demo
// filenames from a player name, and two private copies had already drifted
// apart on what a name may contain.
// ---------------------------------------------------------------------------

static void demo_mkdir_p(const char* path) {
    char tmp[DEMO_LONG_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

// The ".part"-then-rename convention, reused here for the publish step.
static void demo_part_name(char* out, size_t n, const char* path) {
    snprintf(out, n, "%s.part", path);
}

// Ported from demo_slot.c, which existed only for the prebuffer and this.
static void demo_build_pov_name(char* out, size_t n, const char* match_id, const char* map, int slot,
                                const char* client_name) {
    char name[64];
    Demo_SanitiseName(name, sizeof(name), client_name);
    snprintf(out, n, "%s_%s_p%d_%s.dm_91", match_id, map, slot, name);
}

// ---------------------------------------------------------------------------
// Finalize thread: finished capture -> two-stage cut -> shipped demo.
//
// WHY TWO STAGES:
//
//   A .dm_91 is a delta chain whose first message must be a gamestate, so
//   capture has to start when the client connects. What we SHIP must instead
//   start at countdown, and - because the demo editor looks up "the snapshot at
//   match clock T" in every POV of a match - every shipped file must share one
//   first/last server time. Those are two different cuts:
//
//   Stage 1 (per POV, as each job is dequeued): translate this POV's arm_seq
//     into a server-time millisecond with demo_scan(), then cut
//     [arm_ms, end-of-file] out of the capture. The result plays as if the
//     client had typed /record at game_countdown.
//
//   Stage 2 (once, when the match's last job has been dequeued): take
//     win_start = max(stage-1 first snapshot times) and
//     win_end   = min(stage-1 last snapshot times) across the match's POVs,
//     and cut every stage-1 file again to that one window.
//
// THREADING: everything under this banner, g_fin_acc included, is touched by
// the finalize thread and nothing else. Stage 2 needs to know when a match's
// jobs are all through stage 1; it learns that from the last_for_match marker,
// which the game thread pushes onto this same FIFO queue only once every one of
// that match's segments has come back through the completion queue and been
// pushed ahead of it. Since the game thread is the only producer, FIFO order is
// the ordering guarantee - the same property the pre-port design got from
// riding our own writer ring.
//
// UNITS: demo_cut()'s start/end are ABSOLUTE server-time milliseconds read back
// out of the demo itself, never wall clock and never "seconds into the file".
// job->seed_at is wall time and is deliberately NOT used for any of this. See
// democut/democut.h for the evidence behind that.
// ---------------------------------------------------------------------------

// Handed from the game thread to the finalize thread as a pointer through the
// queue below. Exactly one thread owns a given job at a time; the finalize
// thread frees it.
typedef struct {
    char raw_path[520];   // what upstream captured; sized like demo_finished_t::path
    char final_path[512]; // what we ship
    char match_id[64];
    time_t seed_at;     // wall time only: diagnostics/naming, never a cut time
    int32_t arm_seq;    // netchan outgoingSequence at the instant the match armed
    int join_lead_ms;   // how far before arm_seq the window may open; 0 normally
    int slot;
    int last_for_match; // terminal marker: no file, just "this match is done"
    // Who and which game, for the snapshot index. Both were read on the game
    // thread (demo_seg_arm, demo_match_guid_capture) and are only printed by the
    // finalize thread. 0 / "" = unknown, and the index then omits the key.
    uint64_t steam_id;
    char match_guid[DEMO_MATCH_GUID_SIZE];
    // sv_demoCut job: raw_path is the only field that matters (final_path and
    // match_id are left empty). Handled by demo_trim_only() before this job
    // would otherwise touch g_fin_acc/match_id at all - see the dispatch in
    // demo_finalize_main().
    int trim_only;
} demo_finalize_job_t;

static demo_finalize_job_t* demo_fin_queue[DEMO_FINALIZE_QUEUE_SIZE];
static unsigned demo_fin_head; // advanced by the game thread
static unsigned demo_fin_tail; // advanced by the finalize thread
static pthread_mutex_t demo_fin_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t demo_fin_cond  = PTHREAD_COND_INITIALIZER;
static int demo_fin_started;         // game thread only

// Game thread -> finalize thread. Non-blocking by design: the game thread must
// never stall on a slow disk copy, so a full queue drops the job (and the
// caller leaves the capture on disk rather than deleting it).
static int demo_finalize_push(demo_finalize_job_t* job) {
    pthread_mutex_lock(&demo_fin_lock);
    if (demo_fin_head - demo_fin_tail >= DEMO_FINALIZE_QUEUE_SIZE) {
        pthread_mutex_unlock(&demo_fin_lock);
        return -1;
    }
    demo_fin_queue[demo_fin_head % DEMO_FINALIZE_QUEUE_SIZE] = job;
    demo_fin_head++;
    pthread_cond_signal(&demo_fin_cond);
    pthread_mutex_unlock(&demo_fin_lock);
    return 0;
}

// One finalised segment of one match, held from its stage-1 cut until the
// match's stage-2 flush.
typedef struct {
    char source[DEMO_LONG_PATH];    // what to publish: the stage-1 cut, or the
                                    // capture when stage 1 could not run
    char stage_dir[DEMO_LONG_PATH]; // scratch dir holding source, "" if none
    char final_path[512];
    int slot;
    int first_ms; // stage-1 output's own snapshot range, stage-2's input
    int last_ms;
    int cuttable; // 0 = copy-through fallback, excluded from the intersection
    // Server time the match went live for this POV (demo_scan's live_ms over the
    // capture, which is the only file that still carries the "cs 5 -> \time\0"
    // command - a cut file's gamestate absorbs it). -1 when unknown, which is
    // normal and common: see demo_scan's live_ms contract. Published as
    // game_start_server_time in this POV's snapshot index.
    int live_ms;
    // Copied from the job; see demo_finalize_job_t. Published as steam_id64 and
    // match_guid in this POV's snapshot index, omitted when 0 / "".
    uint64_t steam_id;
    char match_guid[DEMO_MATCH_GUID_SIZE];
    // Snapshot range read back out of the SHIPPED file after a successful
    // stage-2 cut, or -1/-1 when this POV was not stage-2 cut (or the re-scan
    // failed). Used only by the post-cut alignment check in
    // demo_stage2_flush(), which is diagnostic: see the comment there.
    int shipped_first_ms;
    int shipped_last_ms;
} demo_pov_t;

static struct {
    char match_id[64];
    int count;
    int dropped;   // segments published on their own because the table was full
    int published; // files that actually reached their final path, all paths counted
    demo_pov_t povs[DEMO_MAX_POVS_PER_MATCH];
} g_fin_acc; // FINALIZE THREAD ONLY

static unsigned char demo_copy_buf[DEMO_COPY_CHUNK]; // finalize thread only

// Byte-for-byte copy. Returns 1 on success. The fallback for every way a cut
// can fail.
static int demo_copy_stream(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) {
        return 0;
    }
    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    int ok = 1;
    for (;;) {
        size_t n = fread(demo_copy_buf, 1, sizeof(demo_copy_buf), in);
        if (n == 0) {
            break;
        }
        if (fwrite(demo_copy_buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    if (ferror(in)) {
        ok = 0;
    }
    fclose(in);
    if (fclose(out) != 0) {
        ok = 0;
    }
    return ok;
}

// Moves src to final_path via "<final>.part" + rename, creating the destination
// directory. A copy rather than a plain rename because src is not guaranteed to
// be on the same filesystem as final_path (it was tmpfs before this port, and a
// stage directory is only "usually" a sibling). Returns 1 on success and leaves
// src alone either way (callers own its cleanup).
static int demo_publish(const char* src, const char* final_path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", final_path);
    char* sep = strrchr(dir, '/');
    if (sep) {
        *sep = '\0';
        demo_mkdir_p(dir);
    }

    char part[512 + 8];
    demo_part_name(part, sizeof(part), final_path);

    if (!demo_copy_stream(src, part)) {
        DebugPrint("demo: copy of %s -> %s failed\n", src, part);
        unlink(part);
        return 0;
    }
    if (rename(part, final_path)) {
        DebugPrint("demo: could not rename %s into place\n", part);
        unlink(part);
        return 0;
    }
    return 1;
}

// Unlinks every regular entry in dir, then removes dir itself. Only ever
// pointed at a scratch directory this file created for one cut.
static void demo_purge_dir(const char* dir) {
    if (!dir || !dir[0]) {
        return;
    }
    DIR* d = opendir(dir);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
                continue;
            }
            char p[DEMO_LONG_PATH + 256];
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(dir);
}

// Finds the single .dm_91 demo_cut() wrote into dir. democut.h's contract is that the
// caller gives demo_cut() a directory it owns exclusively for that one call,
// which is what makes this safe - there is deliberately no "newest file wins"
// heuristic here. Returns 1 and fills out on success.
static int demo_only_output(const char* dir, char* out, size_t out_len) {
    DIR* d = opendir(dir);
    if (!d) {
        return 0;
    }
    int found = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        found++;
        if (found == 1) {
            snprintf(out, out_len, "%s/%s", dir, e->d_name);
        }
    }
    closedir(d);
    if (found != 1) {
        DebugPrint("demo: expected exactly 1 cut output in %s, found %d\n", dir, found);
        return 0;
    }
    return 1;
}

// demo_cut() into a scratch directory of our own, returning the path it chose.
// dir must not exist yet (or must be empty). start_seq pins the opening edge of
// the window to a block sequence number (-1 for none) - see democut.h; every
// caller here that HAS one passes it, because a capture can hold a stale clock
// epoch whose times overlap the window wanted. Returns 1 on success.
static int demo_cut_into(const char* src, const char* dir, int start_ms, int end_ms, int start_seq, char* out,
                         size_t out_len) {
    demo_mkdir_p(dir);
    char err[256];
    if (demo_cut(src, dir, start_ms, end_ms, start_seq, err, (int)sizeof(err)) != 0) {
        DebugPrint("demo: cut of %s [%d,%d] failed: %s\n", src, start_ms, end_ms, err);
        return 0;
    }
    return demo_only_output(dir, out, out_len);
}

// Builds "<dir of path>/.<tag>_<basename of path minus .dm_91>". Unique per
// segment because the basename already carries the per-segment discriminator.
// Returns 1 on success, 0 if it would not fit.
static int demo_stage_dir(char* out, size_t out_len, const char* path, const char* tag) {
    char base[DEMO_LONG_PATH];
    snprintf(base, sizeof(base), "%s", path);

    const char* leaf = base;
    char* sep        = strrchr(base, '/');
    if (sep) {
        *sep = '\0';
        leaf = sep + 1;
    } else {
        base[0] = '.';
        base[1] = '\0';
        leaf    = path;
    }

    char trimmed[DEMO_LONG_PATH];
    snprintf(trimmed, sizeof(trimmed), "%s", leaf);
    size_t n = strlen(trimmed);
    if (n > 6 && !strcmp(trimmed + n - 6, ".dm_91")) {
        trimmed[n - 6] = '\0';
    }

    int written = snprintf(out, out_len, "%s/.%s_%s", base, tag, trimmed);
    return (written > 0 && (size_t)written < out_len);
}

// Startup sweep for stage directories a PREVIOUS process left behind.
//
// demo_purge_dir only runs on the finalize thread's normal completion path. If
// the process dies mid-finalize (restart, OOM, crash, `docker restart`), the
// ".s1_"/".s2_" directory and the full-size demo file inside it are orphaned
// permanently and invisibly: the name is dot-prefixed, so neither the manifest's
// "{match_id}_*.dm_91" glob nor a plain `ls` shows it. Pre-port these lived on
// tmpfs (/dev/shm) and a reboot cleared them for free; since the port they sit on
// real disk under fs_homepath/sv_demoDir, so nothing cleans them up.
//
// Modelled on upstream's own sv_demoCleanupParts sweep (src/features/demos.c,
// demo_sweep_dir/demo_sweep_parts): same bounded recursion (sv_demoNameFormat may
// contain a '/', so segments are not necessarily all in one directory), same
// lstat-so-symlinks-are-left-alone rule, same "leave anything recent alone"
// guard, same once-per-process placement. The min age is much larger than
// upstream's 60 s because a stage directory that is genuinely in use belongs to
// THIS process's finalize thread, and a cut of a long match can legitimately hold
// one open for a while - only a leftover from a previous run should ever be
// removed here, and there is nothing time-sensitive about catching it promptly.
#define DEMO_STAGE_SWEEP_DEPTH   8
#define DEMO_STAGE_SWEEP_MIN_AGE (4 * 60 * 60) // seconds

static void demo_stage_sweep_dir(const char* dir, int depth, time_t cutoff, unsigned* removed, unsigned* kept) {
    DIR* d = opendir(dir);
    if (!d) {
        return;
    }

    for (struct dirent* e = readdir(d); e; e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }

        char path[DEMO_LONG_PATH];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path)) {
            continue;
        }

        struct stat st;
        if (lstat(path, &st)) {
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            continue; // lstat, so a symlink to a directory is left alone, not followed.
        }

        // Not one of ours: recurse, because sv_demoNameFormat may nest.
        if (strncmp(e->d_name, ".s1_", 4) && strncmp(e->d_name, ".s2_", 4) && strncmp(e->d_name, ".tc_", 4) &&
            strncmp(e->d_name, ".sn_", 4)) {
            if (depth + 1 < DEMO_STAGE_SWEEP_DEPTH) {
                demo_stage_sweep_dir(path, depth + 1, cutoff, removed, kept);
            }
            continue;
        }

        if (st.st_mtime > cutoff) {
            (*kept)++;
            continue;
        }
        demo_purge_dir(path);
        (*removed)++;
    }

    closedir(d);
}

static void demo_stage_sweep(void) {
    if (!fs_homepath || !fs_homepath->string[0]) {
        return;
    }

    const char* subdir = (sv_demoDir && sv_demoDir->string[0]) ? sv_demoDir->string : "demos";
    char dir[DEMO_LONG_PATH];
    if ((size_t)snprintf(dir, sizeof(dir), "%s/%s", fs_homepath->string, subdir) >= sizeof(dir)) {
        return;
    }

    unsigned removed = 0, kept = 0;
    demo_stage_sweep_dir(dir, 0, time(NULL) - DEMO_STAGE_SWEEP_MIN_AGE, &removed, &kept);
    if (removed) {
        DebugPrint("demo: removed %u orphaned cut staging director%s left by a previous run.\n", removed,
                   removed == 1 ? "y" : "ies");
    }
    if (kept) {
        DebugPrint("demo: left %u cut staging director%s modified in the last %d s alone.\n", kept,
                   kept == 1 ? "y" : "ies", DEMO_STAGE_SWEEP_MIN_AGE);
    }
}

// ---------------------------------------------------------------------------
// Per-POV snapshot index (spec: index/p{slot}.snaps.json).
//
// Built from the SHIPPED file and nothing else. Every offset in it is a byte
// offset into that exact file, and a stage-2 cut rewrites the gamestate and
// renumbers every message, so an index built from the capture or from a stage-1
// intermediate would point at the wrong bytes of the file that ships. That is
// why this runs at the two places a POV reaches its final form
// (demo_pov_publish, and the post-rename step of demo_stage2_flush's pass 3)
// and nowhere earlier.
//
// Keys, in order: file, client_num, steam_id64 (17-digit string, omitted for a
// bot or an unknown id), match_guid (lower-case UUID from CS_MATCH_GUID,
// omitted when none was read), first_server_time, last_server_time,
// game_start_server_time, index_framing, snapshots. The two optional keys are
// read on the game thread and only printed here; a consumer must treat their
// absence as "unknown", never as an error.
// ---------------------------------------------------------------------------

// No JSON library is vendored and this file emits exactly one shape of
// document, so the serialiser is these ~40 lines rather than a dependency.
// Numbers are printed with snprintf and need no escaping at all; only the
// string values (a file path, the steam id, the match guid and a fixed literal)
// go through here.
static void demo_json_str(FILE* f, const char* s) {
    fputc('"', f);
    for (const unsigned char* p = (const unsigned char*)s; p && *p; p++) {
        switch (*p) {
        case '"':
            fputs("\\\"", f);
            break;
        case '\\':
            fputs("\\\\", f);
            break;
        case '\b':
            fputs("\\b", f);
            break;
        case '\f':
            fputs("\\f", f);
            break;
        case '\n':
            fputs("\\n", f);
            break;
        case '\r':
            fputs("\\r", f);
            break;
        case '\t':
            fputs("\\t", f);
            break;
        default:
            // Everything else is passed through as-is. Bytes >= 0x80 are
            // already valid UTF-8 here or they would not be in a path we built,
            // and JSON allows them raw; only C0 controls must be escaped, and
            // \u00xx is the only form that covers them all.
            if (*p < 0x20) {
                fprintf(f, "\\u%04x", (unsigned)*p);
            } else {
                fputc((int)*p, f);
            }
        }
    }
    fputc('"', f);
}

static const char* demo_basename(const char* path) {
    const char* sep = strrchr(path, '/');
    return sep ? sep + 1 : path;
}

// "<directory of the .dm_91>/index/<basename of the .dm_91 minus its
// extension>.snaps.json". The spec's own naming (index/p{slot}.snaps.json)
// predates the discovery that one slot can legitimately produce several
// segments in a single match. Keying the index name on bare "slot" alone would
// let the second segment's index silently overwrite the first's. Deriving the
// index name from the demo's own basename instead (which already carries the
// match/slot/seg_time/seg_id discriminator, since it IS final_path's basename)
// inherits that uniqueness for free and pairs exactly one index with exactly
// one demo, which is the property that actually matters here - not the literal
// "p{slot}" spelling. Returns 1 on success, 0 if the name would not fit.
static int demo_index_path(char* out, size_t out_len, const char* dm91, int slot) {
    (void)slot; // kept for call-site symmetry with demo_write_index's other slot uses
    char dir[DEMO_LONG_PATH];
    int w = snprintf(dir, sizeof(dir), "%s", dm91);
    if (w <= 0 || (size_t)w >= sizeof(dir)) {
        return 0;
    }
    char* sep        = strrchr(dir, '/');
    const char* leaf = dm91;
    if (sep) {
        leaf = sep + 1;
        *sep = '\0';
    } else {
        dir[0] = '.';
        dir[1] = '\0';
    }

    char stem[512];
    w = snprintf(stem, sizeof(stem), "%s", leaf);
    if (w <= 0 || (size_t)w >= (int)sizeof(stem)) {
        return 0;
    }
    size_t n = strlen(stem);
    if (n > 6 && !strcmp(stem + n - 6, ".dm_91")) {
        stem[n - 6] = '\0';
    }

    w = snprintf(out, out_len, "%s/index/%s.snaps.json", dir, stem);
    return (w > 0 && (size_t)w < out_len);
}

// Walks p->final_path once and writes its snapshot index beside it. Fills
// *out_scan with that walk's own demo_scan_t when the walk succeeded (first_ms
// stays -1 otherwise) so callers can reuse it instead of scanning the same file
// twice. Returns 1 only when the index file was written.
//
// A failure here never undoes a publish: the .dm_91 is the product, the index is
// a seek accelerator for it. The spec's rule is "fail that file's index and do
// not zip [it]", which is exactly "no index file exists", plus a loud log.
static int demo_write_index(const demo_pov_t* p, demo_scan_t* out_scan) {
    if (out_scan) {
        memset(out_scan, 0, sizeof(*out_scan));
        out_scan->first_ms = -1;
        out_scan->last_ms  = -1;
    }

    demo_index_result_t idx;
    char err[256];
    if (demo_index(p->final_path, &idx, err, (int)sizeof(err)) != 0) {
        DebugPrint("demo: no snapshot index for %s (%s)\n", p->final_path, err);
        return 0;
    }
    if (out_scan) {
        *out_scan = idx.scan;
    }

    int ok         = 0;
    int client_num = p->slot;
    char path[DEMO_LONG_PATH];
    char part[DEMO_LONG_PATH + 8];
    FILE* f = NULL;

    // An index over a file whose clock restarts mid-way would be monotonic
    // nowhere, so a consumer binary-searching it for "the snapshot at T" would
    // land anywhere at all. Refuse rather than ship one. Only a copy-through POV
    // can be in this state - a cut output always has exactly 1 gamestate.
    if (idx.scan.gamestate_count != 1 || idx.scan.clock_resets != 0) {
        DebugPrint("demo: no snapshot index for %s: %d gamestate(s), %d clock reset(s)\n", p->final_path,
                   idx.scan.gamestate_count, idx.scan.clock_resets);
        goto done;
    }
    if (!demo_index_path(path, sizeof(path), p->final_path, p->slot)) {
        DebugPrint("demo: index path too long for %s\n", p->final_path);
        goto done;
    }

    {
        char dir[DEMO_LONG_PATH];
        snprintf(dir, sizeof(dir), "%s", path);
        char* sep = strrchr(dir, '/');
        if (sep) {
            *sep = '\0';
            demo_mkdir_p(dir);
        }
    }
    demo_part_name(part, sizeof(part), path);
    f = fopen(part, "wb");
    if (!f) {
        DebugPrint("demo: could not open %s\n", part);
        goto done;
    }
    setvbuf(f, NULL, _IOFBF, 64 * 1024);

    // The demo's own gamestate is the authority on whose POV this is; the slot
    // is only what the capture side named the file after. They agree on every
    // normal capture - say so when they do not rather than silently pick one.
    if (idx.scan.client_num >= 0) {
        client_num = idx.scan.client_num;
    }
    if (idx.scan.client_num >= 0 && idx.scan.client_num != p->slot) {
        DebugPrint("demo: %s records client_num %d but was captured on slot %d\n", p->final_path,
                   idx.scan.client_num, p->slot);
    }

    fputs("{\n", f);
    fputs("  \"file\": ", f);
    {
        // Relative to the .qlmatch root, where the demos always live under
        // "demos/" regardless of sv_demoDir on this server.
        char rel[600];
        snprintf(rel, sizeof(rel), "demos/%s", demo_basename(p->final_path));
        demo_json_str(f, rel);
    }
    fprintf(f, ",\n  \"client_num\": %d,\n", client_num);
    // Who played this POV and which game it was, both strings so a consumer
    // never has to trust a JSON number with 64 bits. Omitted when unknown: a
    // bot's steam id is 0, and CS_MATCH_GUID may never have held a valid UUID.
    if (p->steam_id) {
        char sid[24];
        int n = snprintf(sid, sizeof(sid), "%llu", (unsigned long long)p->steam_id);
        if (n == 17) {
            fputs("  \"steam_id64\": ", f);
            demo_json_str(f, sid);
            fputs(",\n", f);
        }
    }
    if (p->match_guid[0]) {
        fputs("  \"match_guid\": ", f);
        demo_json_str(f, p->match_guid);
        fputs(",\n", f);
    }
    fprintf(f, "  \"first_server_time\": %d,\n", idx.scan.first_ms);
    fprintf(f, "  \"last_server_time\": %d,\n", idx.scan.last_ms);
    fprintf(f, "  \"game_start_server_time\": %d,\n", p->live_ms);
    fputs("  \"index_framing\": \"with_header\",\n", f);
    fputs("  \"snapshots\": [", f);
    for (int i = 0; i < idx.count; i++) {
        const demo_snap_t* s = &idx.snaps[i];
        char row[128];
        int n = snprintf(row, sizeof(row), "%s\n    {\"t\": %d, \"off\": %lld, \"len\": %d, \"delta\": %d}",
                         i ? "," : "", s->t, s->off, s->len, s->delta);
        if (n <= 0 || (size_t)n >= sizeof(row) || fwrite(row, 1, (size_t)n, f) != (size_t)n) {
            DebugPrint("demo: write error building %s\n", part);
            fclose(f);
            f = NULL;
            unlink(part);
            goto done;
        }
    }
    fputs("\n  ]\n}\n", f);

    // ferror first, then fclose UNCONDITIONALLY: short-circuiting on ferror
    // would skip the close and leak the descriptor on exactly the path where
    // something already went wrong.
    {
        int werr = ferror(f) ? 1 : 0;
        if (fclose(f) != 0) {
            werr = 1;
        }
        f = NULL;
        if (werr) {
            DebugPrint("demo: write error finishing %s\n", part);
            unlink(part);
            goto done;
        }
    }
    if (rename(part, path)) {
        DebugPrint("demo: could not rename %s into place\n", part);
        unlink(part);
        goto done;
    }

    ok = 1;
    if (idx.delta_unknown) {
        DebugPrint("demo: %s: %d of %d index row(s) have an unknown delta (-1)\n", path, idx.delta_unknown,
                   idx.count);
    }
    DebugPrint("demo: indexed %s: %d snapshot(s) over [%d,%d], client_num %d\n", path, idx.count,
               idx.scan.first_ms, idx.scan.last_ms, client_num);

done:
    if (f) {
        fclose(f);
    }
    demo_index_free(&idx);
    return ok;
}

// Publishes one accumulated POV and releases its scratch space. Used both for
// the normal stage-2 output and for every fallback path.
static void demo_pov_publish(demo_pov_t* p, const char* why) {
    if (!p->cuttable) {
        why = "untrimmed copy-through";
    }
    if (p->source[0] && demo_publish(p->source, p->final_path)) {
        g_fin_acc.published++;
        DebugPrint("demo: finalised %s (%s)\n", p->final_path, why);
        // The file is now in its final form, so this is where its index can be
        // built (see demo_write_index). Stage-2-cut POVs never reach here - they
        // rename into place in demo_stage2_flush and index there.
        demo_write_index(p, NULL);
        if (!p->stage_dir[0]) {
            // No scratch dir means source is the capture itself (a
            // copy-through). demo_purge_dir below only clears scratch dirs, so
            // without this the capture would stay in the demo directory under
            // its upstream name forever. Only on success: a failed publish
            // leaves it recoverable.
            unlink(p->source);
        }
    } else {
        DebugPrint("demo: could not finalise %s from %s (%s)\n", p->final_path, p->source, why);
    }
    demo_purge_dir(p->stage_dir);
    p->source[0]    = '\0';
    p->stage_dir[0] = '\0';
}

static void demo_acc_reset(void) {
    g_fin_acc.match_id[0] = '\0';
    g_fin_acc.count       = 0;
    g_fin_acc.dropped     = 0;
    g_fin_acc.published   = 0;
}

// STAGE 2. Every POV of g_fin_acc.match_id has been through stage 1; force them
// all onto one shared server-time window and publish.
//
// Returns how many files actually reached their final path. The caller needs
// that to decide whether the match is worth telling Python about at all: a match
// that armed and shipped nothing (a countdown whose captures all failed, an arm
// a map change outran) used to fire demo_match_finalized anyway, which started a
// packer run that could only ever fail with "no <match_id>_*.dm_91 files" and
// leave a stub .packer.log in the demo directory for the operator to wonder at.
static int demo_stage2_flush(void) {
    if (g_fin_acc.count <= 0) {
        int published = g_fin_acc.published;
        demo_acc_reset();
        return published;
    }

    // PASS 1: the extremes, over every cuttable POV. These only locate the
    // cohorts; they are deliberately NOT the window.
    int cuttable = 0, min_first = 0, max_last = 0;
    for (int i = 0; i < g_fin_acc.count; i++) {
        demo_pov_t* p = &g_fin_acc.povs[i];
        if (!p->cuttable) {
            continue;
        }
        if (cuttable == 0 || p->first_ms < min_first) {
            min_first = p->first_ms;
        }
        if (cuttable == 0 || p->last_ms > max_last) {
            max_last = p->last_ms;
        }
        cuttable++;
    }

    // PASS 2: the window itself, computed over the CORE COHORT only - the POVs
    // that start within DEMO_OUTLIER_WINDOW_MS of the earliest start, and the
    // POVs that end within DEMO_OUTLIER_WINDOW_MS of the latest end. A
    // mid-match joiner or an early leaver is thereby excluded from DEFINING the
    // window rather than being allowed to define it for everyone (see
    // DEMO_OUTLIER_WINDOW_MS).
    //
    // Both cohorts are non-empty by construction: the POV that achieved
    // min_first is always within the start cohort, and the POV that achieved
    // max_last is always within the end cohort. So win_start/win_end are always
    // set whenever cuttable > 0.
    //
    // PASS 2b: how many cuttable POVs can actually BE trimmed to that window,
    // i.e. cover it. This - not the raw cuttable count - is the number of
    // clock-aligned files the match will ship.
    int win_start = 0, win_end = 0, aligned = 0;
    if (cuttable > 0) {
        int have_start = 0, have_end = 0;
        for (int i = 0; i < g_fin_acc.count; i++) {
            demo_pov_t* p = &g_fin_acc.povs[i];
            if (!p->cuttable) {
                continue;
            }
            if (p->first_ms <= min_first + DEMO_OUTLIER_WINDOW_MS && (!have_start || p->first_ms > win_start)) {
                win_start  = p->first_ms;
                have_start = 1;
            }
            if (p->last_ms >= max_last - DEMO_OUTLIER_WINDOW_MS && (!have_end || p->last_ms < win_end)) {
                win_end  = p->last_ms;
                have_end = 1;
            }
        }
        for (int i = 0; i < g_fin_acc.count; i++) {
            demo_pov_t* p = &g_fin_acc.povs[i];
            if (p->cuttable && p->first_ms <= win_start && p->last_ms >= win_end) {
                aligned++;
            }
        }
    }

    // With fewer than two POVs that can be cut to the shared window there is
    // nothing to intersect: a lone file already trivially "shares" its window
    // with itself, and re-cutting it to its own range would only cost time and
    // lose its first snapshot to the rewritten gamestate. This is the coverage
    // count from pass 2b, not the raw cuttable count, because a match of one
    // real POV plus one mid-match joiner has two cuttable POVs and still no
    // shared window worth cutting to.
    if (aligned < 2) {
        for (int i = 0; i < g_fin_acc.count; i++) {
            demo_pov_publish(&g_fin_acc.povs[i], "countdown-trimmed, no shared window needed");
        }
        DebugPrint("demo: match %s finalised with %d file(s), %d cuttable, %d covering "
                   "[%d,%d]: fewer than 2 clock-alignable POVs, no shared-window cut\n",
                   g_fin_acc.match_id, g_fin_acc.count, cuttable, aligned, win_start, win_end);
        int published = g_fin_acc.published;
        demo_acc_reset();
        return published;
    }

    // Applied to the outlier-resistant window, not to the naive intersection:
    // the whole point of pass 2 is that one straggler must not be able to push
    // the match onto a window that is (nearly) empty.
    if (win_end - win_start < DEMO_MIN_WINDOW_MS) {
        // Accepted degraded outcome, not an error: the POVs barely overlap, so a
        // shared window would be meaningless. Ship the stage-1 files as they are
        // and say so loudly - whatever builds the .qlmatch must be able to tell
        // this apart from a normal pack.
        DebugPrint("demo: match %s shared window [%d,%d] is only %d ms over %d of %d POVs "
                   "(< %d ms): NOT clock-aligned, shipping stage-1 files as-is\n",
                   g_fin_acc.match_id, win_start, win_end, win_end - win_start, aligned, cuttable,
                   DEMO_MIN_WINDOW_MS);
        for (int i = 0; i < g_fin_acc.count; i++) {
            demo_pov_publish(&g_fin_acc.povs[i], "window too short");
        }
        int published = g_fin_acc.published;
        demo_acc_reset();
        return published;
    }

    DebugPrint("demo: match %s shared window [%d,%d] (%d ms) over %d of %d cuttable POVs "
               "(extremes [%d,%d], outlier margin %d ms)\n",
               g_fin_acc.match_id, win_start, win_end, win_end - win_start, aligned, cuttable, min_first,
               max_last, DEMO_OUTLIER_WINDOW_MS);

    // PASS 3: the cut itself.
    for (int i = 0; i < g_fin_acc.count; i++) {
        demo_pov_t* p = &g_fin_acc.povs[i];
        if (!p->cuttable) {
            demo_pov_publish(p, "copy-through, not clock-aligned");
            continue;
        }
        // The outlier case. Its own range does not cover the shared window, so
        // it CANNOT be trimmed to it - demo_cut would silently hand back
        // whatever part of the window it does have, which is a file that claims
        // to be aligned and is not. Publish its stage-1 file instead: a correct,
        // countdown-trimmed, honestly-unaligned demo, exactly like the !cuttable
        // copy-through path above.
        if (p->first_ms > win_start || p->last_ms < win_end) {
            DebugPrint("demo: slot %d POV [%d,%d] does not cover the shared window [%d,%d] "
                       "(%s by %d ms); excluded from the shared cut, shipping its stage-1 "
                       "file as-is\n",
                       p->slot, p->first_ms, p->last_ms, win_start, win_end,
                       (p->first_ms > win_start) ? "joined late" : "left early",
                       (p->first_ms > win_start) ? (p->first_ms - win_start) : (win_end - p->last_ms));
            demo_pov_publish(p, "outside the shared window, stage-1 file kept");
            continue;
        }

        char dir[DEMO_LONG_PATH];
        char cut[DEMO_LONG_PATH];
        // Never purge a directory name demo_stage_dir could not build in full:
        // snprintf truncation would leave a valid-looking path to some OTHER
        // directory, and demo_purge_dir unlinks what it finds.
        if (!demo_stage_dir(dir, sizeof(dir), p->final_path, "s2")) {
            DebugPrint("demo: stage-2 scratch path too long for %s\n", p->final_path);
            demo_pov_publish(p, "stage 2 skipped, stage-1 file kept");
            continue;
        }
        // -1: a stage-1 output is a single clean clock epoch by construction
        // (demo_stage1 refuses to publish anything else), so there is no stale
        // epoch for the window to open in and no sequence to pin it to.
        if (!demo_cut_into(p->source, dir, win_start, win_end, -1, cut, sizeof(cut))) {
            demo_purge_dir(dir);
            demo_pov_publish(p, "stage 2 failed, stage-1 file kept");
            continue;
        }

        // The stage-2 directory sits next to final_path (same filesystem), so
        // this rename is atomic and copy-free, unlike demo_publish's.
        if (rename(cut, p->final_path)) {
            DebugPrint("demo: could not rename %s into place\n", cut);
            demo_purge_dir(dir);
            demo_pov_publish(p, "stage 2 rename failed, stage-1 file kept");
            continue;
        }
        demo_purge_dir(dir);
        demo_purge_dir(p->stage_dir);
        p->source[0]    = '\0';
        p->stage_dir[0] = '\0';
        g_fin_acc.published++;
        DebugPrint("demo: finalised %s (slot %d, window [%d,%d])\n", p->final_path, p->slot, win_start,
                   win_end);

        // Diagnostic re-scan of the SHIPPED file (not the pre-rename scratch
        // copy): the alignment guarantee this whole stage exists for is that
        // every clock-aligned POV starts/ends at the same server time, but that
        // rests on every client sharing one snapshot grid - true on the real
        // corpus this was tested against, not something demo_cut() or this code
        // enforces structurally. A client on a different snaps rate could land
        // its nearest snapshot to win_start/win_end a frame away from its
        // siblings. Failure here is not a reason to undo a publish that already
        // succeeded - it is the live-verification signal, logged loudly instead
        // of silently assumed.
        //
        // The index build is folded into this same step: demo_write_index walks
        // the shipped file with demo_index(), which is demo_scan()'s walk with
        // the per-snapshot rows kept, and hands the scan back here. So the
        // shipped file is still read exactly once, and the two things that must
        // describe the same bytes (the alignment check and the index) provably
        // come from one pass over them. An index failure is reported by
        // demo_write_index itself and does not disturb the alignment check,
        // which only needs the scan.
        demo_scan_t shipped;
        demo_write_index(p, &shipped);
        if (shipped.first_ms >= 0) {
            p->shipped_first_ms = shipped.first_ms;
            p->shipped_last_ms  = shipped.last_ms;
        } else {
            DebugPrint("demo: post-cut re-scan of %s failed; cannot verify shared clock\n", p->final_path);
        }
    }

    // Cross-check every POV that was actually stage-2 cut against the first one:
    // they were all cut to the identical [win_start, win_end], so if the
    // shared-snapshot-grid assumption above holds they must all report the same
    // shipped_first_ms/shipped_last_ms. Exact equality, not a tolerance window -
    // demo_cut() selects existing snapshots rather than resampling, so two POVs
    // on the same grid land on the exact same millisecond, and "off by one
    // frame" is precisely the case worth knowing about, not rounding away.
    {
        int ref_first = -1, ref_last = -1, ref_slot = -1, mismatches = 0, checked = 0;
        for (int i = 0; i < g_fin_acc.count; i++) {
            demo_pov_t* p = &g_fin_acc.povs[i];
            if (p->shipped_first_ms < 0) {
                continue; // not stage-2 cut, or its re-scan failed above.
            }
            checked++;
            if (ref_slot < 0) {
                ref_first = p->shipped_first_ms;
                ref_last  = p->shipped_last_ms;
                ref_slot  = p->slot;
                continue;
            }
            if (p->shipped_first_ms != ref_first || p->shipped_last_ms != ref_last) {
                DebugPrint("demo: match %s clock mismatch: slot %d shipped [%d,%d], "
                           "slot %d (reference) shipped [%d,%d] - NOT actually clock-aligned "
                           "despite both being cut to the same window\n",
                           g_fin_acc.match_id, p->slot, p->shipped_first_ms, p->shipped_last_ms, ref_slot,
                           ref_first, ref_last);
                mismatches++;
            }
        }
        if (checked >= 2 && mismatches == 0) {
            DebugPrint("demo: match %s: %d shipped POV(s) confirmed sharing [%d,%d]\n", g_fin_acc.match_id,
                       checked, ref_first, ref_last);
        }
    }

    if (g_fin_acc.dropped) {
        DebugPrint("demo: match %s had %d segment(s) published outside the shared window "
                   "(more than %d segments in one match)\n",
                   g_fin_acc.match_id, g_fin_acc.dropped, DEMO_MAX_POVS_PER_MATCH);
    }
    int published = g_fin_acc.published;
    demo_acc_reset();
    return published;
}

// STAGE 1. Turns one capture into a countdown-trimmed file and parks it in the
// accumulator until the match's last job arrives.
static void demo_stage1(const demo_finalize_job_t* job) {
    if (g_fin_acc.count >= DEMO_MAX_POVS_PER_MATCH) {
        // No room to hold this one for stage 2. Publishing it untrimmed is
        // strictly better than dropping it, and the count is logged at flush.
        g_fin_acc.dropped++;
        if (demo_publish(job->raw_path, job->final_path)) {
            g_fin_acc.published++;
            unlink(job->raw_path);
        }
        return;
    }

    demo_pov_t* p = &g_fin_acc.povs[g_fin_acc.count];
    memset(p, 0, sizeof(*p));
    p->shipped_first_ms = -1;
    p->shipped_last_ms  = -1;
    p->live_ms          = -1;
    p->slot             = job->slot;
    p->steam_id         = job->steam_id;
    snprintf(p->match_guid, sizeof(p->match_guid), "%s", job->match_guid);
    snprintf(p->final_path, sizeof(p->final_path), "%s", job->final_path);
    snprintf(p->source, sizeof(p->source), "%s", job->raw_path);
    g_fin_acc.count++;

    char err[256];
    demo_scan_t scan;
    if (demo_scan(job->raw_path, job->arm_seq, &scan, err, (int)sizeof(err)) != 0) {
        DebugPrint("demo: slot %d scan of %s failed (%s); copying untrimmed\n", job->slot, job->raw_path, err);
        return; // p stays cuttable=0, source=capture: published as-is at flush.
    }

    // A multi-gamestate file breaks demo_cut()'s "start <= t <= end" message
    // selection, which has no notion of gamestate boundaries (its
    // GameStateIndex is pinned to 0) - hitting one means a stranded or
    // hand-recovered file, copy it through rather than cut it wrongly.
    // Recorded even when the cut below fails: it is a property of the capture,
    // not of the trim, and a copy-through POV still gets an index.
    p->live_ms = scan.live_ms;

    if (scan.gamestate_count != 1) {
        DebugPrint("demo: slot %d %s has %d gamestate(s); copying untrimmed\n",
                   job->slot, job->raw_path, scan.gamestate_count);
        return;
    }
    if (scan.arm_ms < 0) {
        DebugPrint("demo: slot %d no snapshot at/after arm_seq %d in %s "
                   "(%d snapshots, [%d,%d]); copying untrimmed\n",
                   job->slot, job->arm_seq, job->raw_path, scan.snapshot_count, scan.first_ms, scan.last_ms);
        return;
    }
    // A raw capture stays open and keeps accumulating across back-to-back
    // matches on the same map with no client reconnect (see the "known
    // limitation" comment on demo_disarm_match below), and a soft server
    // respawn between those matches resets the server clock without ever
    // sending a new gamestate - so scan.clock_resets (over the WHOLE file)
    // is routinely non-zero on a perfectly healthy capture: a stale, already-
    // consumed match's tail sits before our own arm point, discarded by the
    // cut regardless of what it contains. Only a reset AT OR AFTER arm_ms -
    // inside the region the cut below will actually select from - means two
    // clock epochs really do overlap our window and the cut would be wrong.
    if (scan.clock_resets_since_arm != 0) {
        DebugPrint("demo: slot %d %s has %d clock reset(s) at/after arm_seq %d "
                   "(%d total); copying untrimmed\n",
                   job->slot, job->raw_path, scan.clock_resets_since_arm, job->arm_seq, scan.clock_resets);
        return;
    }

    char dir[DEMO_LONG_PATH];
    char cut[DEMO_LONG_PATH];
    // See the matching comment in demo_stage2_flush: a truncated scratch path
    // must never reach demo_purge_dir.
    if (!demo_stage_dir(dir, sizeof(dir), job->raw_path, "s1")) {
        DebugPrint("demo: stage-1 scratch path too long for %s; copying untrimmed\n", job->raw_path);
        return;
    }
    // job->arm_seq is what keeps a stale pre-arm epoch (see above) out of the
    // cut: its times can sit anywhere relative to our match's - BELOW arm_ms,
    // above last_ms, or straddling the whole window - so no [start_ms, end_ms]
    // pair can exclude it, and the epoch that comes FIRST in the file is the one
    // a time-only selection opens on. Passing the sequence the match armed at
    // pins the opening edge to the right epoch outright (democut.h's start_seq).
    //
    // end = scan.last_ms (this file's own true last snapshot) rather than
    // DEMO_CUT_TO_END/INT32_MAX all the same: with the opening edge pinned, the
    // closing one only has to stop at the real end of the data, and a concrete
    // bound keeps the cut from running on into anything a later epoch might add.
    // Normally the window opens exactly at the arm instant. A mid-match joiner's
    // does not: they were not in the countdown, so their POV starts
    // sv_demoCutJoinLead ms before the moment they joined, clamped to the start
    // of their own clock epoch (never further back than the data this epoch has).
    int start_ms  = scan.arm_ms;
    int start_seq = scan.arm_epoch_seq >= 0 ? scan.arm_epoch_seq : job->arm_seq;
    if (job->join_lead_ms > 0) {
        start_ms -= job->join_lead_ms;
        if (scan.arm_epoch_firstms >= 0 && start_ms < scan.arm_epoch_firstms) {
            start_ms = scan.arm_epoch_firstms;
        }
    }
    if (!demo_cut_into(job->raw_path, dir, start_ms, scan.last_ms, start_seq, cut, sizeof(cut))) {
        demo_purge_dir(dir);
        DebugPrint("demo: slot %d stage 1 failed for %s; copying untrimmed\n", job->slot, job->raw_path);
        return;
    }

    demo_scan_t s1;
    if (demo_scan(cut, -1, &s1, err, (int)sizeof(err)) != 0 || s1.first_ms < 0) {
        DebugPrint("demo: slot %d stage-1 output %s unreadable (%s); copying untrimmed\n", job->slot, cut,
                   err);
        demo_purge_dir(dir);
        return;
    }
    // Defense in depth: if a pre-arm epoch's numeric range still overlapped
    // ours despite the last_ms bound above (e.g. a THIRD epoch sitting
    // between them), the cut output itself would show it as a clock reset or
    // an extra gamestate. Refuse to publish a cut that isn't actually clean
    // rather than trust the bound alone.
    if (s1.gamestate_count != 1 || s1.clock_resets != 0) {
        DebugPrint("demo: slot %d stage-1 output %s has %d gamestate(s) and %d clock reset(s) "
                   "despite the arm/last_ms bound; copying untrimmed\n",
                   job->slot, cut, s1.gamestate_count, s1.clock_resets);
        demo_purge_dir(dir);
        return;
    }

    snprintf(p->source, sizeof(p->source), "%s", cut);
    snprintf(p->stage_dir, sizeof(p->stage_dir), "%s", dir);
    p->first_ms = s1.first_ms;
    p->last_ms  = s1.last_ms;
    p->cuttable = 1;

    // The capture is dead weight from here on, and holding both copies for every
    // POV until the match flushes would double peak demo-directory usage for no
    // benefit.
    unlink(job->raw_path);

    DebugPrint("demo: slot %d stage 1: arm_seq %d -> %d ms, asked from %d ms%s, kept [%d,%d] of "
               "[%d,%d]%s\n",
               job->slot, job->arm_seq, scan.arm_ms, start_ms,
               (job->join_lead_ms > 0) ? " (mid-match join lead)" : "", s1.first_ms, s1.last_ms,
               scan.first_ms, scan.last_ms,
               (scan.live_ms >= 0) ? " (match went live inside the kept range)" : "");
}

// sv_demoCut's whole job: a capture no match ever armed, trimmed down to
// [match went live, end of file] and published back over its own path.
//
// Deliberately NOT run through demo_stage1()/g_fin_acc: those exist to align
// several POVs of one match onto a shared window, which has no meaning here -
// there is no match, no arm_seq and no second POV to agree with. Kept as its
// own small function instead of teaching demo_stage1 a "no match" mode so a
// trim job can never accidentally touch match bookkeeping it has nothing to do
// with (see the dispatch in demo_finalize_main(), which routes trim_only jobs
// here before the match_id/g_fin_acc logic even looks at them).
//
// arm_seq 0, not -1: passing a negative arm_seq to demo_scan() is the
// documented way to SKIP the arm_ms/live_ms lookup entirely (see democut.h),
// which is the opposite of what this needs. 0 is a valid netchan sequence
// number and every message in the file has a sequence at or above it, so this
// reads as "count from the very start of the capture" - the correct stand-in
// for "no arm point, the whole file is fair game".
static void demo_trim_only(const demo_finalize_job_t* job) {
    char err[256];
    demo_scan_t scan;
    if (demo_scan(job->raw_path, 0, &scan, err, (int)sizeof(err)) != 0) {
        DebugPrint("demo: sv_demoCut scan of %s failed (%s); leaving it uncut\n", job->raw_path, err);
        return;
    }
    if (scan.gamestate_count != 1) {
        // A stranded or hand-recovered file - same guard as demo_stage1, same
        // reason: demo_cut()'s selection has no notion of gamestate boundaries.
        DebugPrint("demo: sv_demoCut: %s has %d gamestate(s); leaving it uncut\n", job->raw_path,
                   scan.gamestate_count);
        return;
    }
    if (scan.live_ms < 0) {
        // Never went live: pure warmup, or a spectator/observer capture with no
        // match in it at all. Nothing a cut would keep - same disposal as an
        // unclaimed capture nothing was ever going to want.
        if (unlink(job->raw_path) == 0) {
            DebugPrint("demo: sv_demoCut: %s never went live; removed\n", job->raw_path);
        }
        return;
    }

    // Only a reset INSIDE [live_ms, last_ms] makes this uncuttable. A reset
    // BEFORE live_ms belongs to a stale epoch the cut now skips outright, since
    // live_seq pins the window's opening edge past it (demo_cut's start_seq).
    // Counting those too - which is what checking clock_resets_since_arm with
    // arm_seq 0 amounted to, arm_ms being the very first snapshot there - left
    // every capture that outlived a map_restart uncut.
    if (scan.clock_resets_since_live != 0) {
        DebugPrint("demo: sv_demoCut: %s has %d clock reset(s) at/after going live; leaving it uncut\n",
                   job->raw_path, scan.clock_resets_since_live);
        return;
    }

    char dir[DEMO_LONG_PATH];
    char cut[DEMO_LONG_PATH];
    if (!demo_stage_dir(dir, sizeof(dir), job->raw_path, "tc")) {
        DebugPrint("demo: sv_demoCut: stage path too long for %s; leaving it uncut\n", job->raw_path);
        return;
    }
    if (!demo_cut_into(job->raw_path, dir, scan.live_ms, scan.last_ms, scan.live_seq, cut, sizeof(cut))) {
        demo_purge_dir(dir);
        DebugPrint("demo: sv_demoCut: cut of %s failed; leaving it uncut\n", job->raw_path);
        return;
    }

    if (demo_publish(cut, job->raw_path)) {
        DebugPrint("demo: sv_demoCut: %s trimmed to [%d,%d] of [%d,%d]\n", job->raw_path, scan.live_ms,
                   scan.last_ms, scan.first_ms, scan.last_ms);
    } else {
        DebugPrint("demo: sv_demoCut: could not publish cut of %s in place; leaving it uncut\n",
                   job->raw_path);
    }
    demo_purge_dir(dir);
}

static void* demo_finalize_main(void* unused) {
    (void)unused;

    // Keep the engine's asynchronous signal handling on the main thread. The
    // four faults a thread raises against itself stay unblocked (blocking those
    // is undefined), and SIGABRT with them so the engine still prints a
    // backtrace - the same set upstream's own writer thread keeps.
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGSEGV);
    sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGFPE);
    sigdelset(&all, SIGILL);
    sigdelset(&all, SIGABRT);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    for (;;) {
        pthread_mutex_lock(&demo_fin_lock);
        while (demo_fin_head == demo_fin_tail) {
            pthread_cond_wait(&demo_fin_cond, &demo_fin_lock);
        }
        demo_finalize_job_t* job = demo_fin_queue[demo_fin_tail % DEMO_FINALIZE_QUEUE_SIZE];
        demo_fin_tail++;
        pthread_mutex_unlock(&demo_fin_lock);

        if (!job) {
            continue;
        }

        if (job->trim_only) {
            // Never let a trim job reach the match_id/g_fin_acc logic below: it
            // has no match_id, and the two share this queue - an unarmed
            // client's disconnect can land here mid-match, between two of that
            // match's own stage-1 jobs.
            demo_trim_only(job);
            free(job);
            continue;
        }

        // A match's jobs are contiguous in this queue: the game thread is the
        // only producer and only appends a match's end marker once every one of
        // its segments has already been pushed. Seeing a different match_id
        // therefore means the previous match's marker never made it (queue full,
        // or its deadline path could not allocate), so flush the old one now
        // rather than leaking its stage-1 files onto disk forever.
        if (g_fin_acc.count > 0 && strcmp(g_fin_acc.match_id, job->match_id) != 0) {
            DebugPrint("demo: match %s had no end marker; flushing it before %s\n", g_fin_acc.match_id,
                       job->match_id);
            demo_stage2_flush();
        }
        if (g_fin_acc.count == 0) {
            snprintf(g_fin_acc.match_id, sizeof(g_fin_acc.match_id), "%s", job->match_id);
        }

        if (job->raw_path[0] && job->final_path[0]) {
            demo_stage1(job);
        }
        if (job->last_for_match) {
            int published = demo_stage2_flush();
            DebugPrint("demo: match %s finalised, %d file(s) shipped\n", job->match_id, published);
#ifndef NOPY
            // Nothing shipped means there is nothing for a consumer to pack or
            // archive; see demo_stage2_flush for why firing anyway is worse than
            // staying quiet.
            if (published > 0) {
                DemoMatchFinalizedDispatcher(job->match_id);
            }
#endif
        }
        free(job);
    }
    return NULL; // unreachable: the finalize thread lives for the process.
}

// Started lazily, on the game thread, the first time a match is armed, and then
// lives for the process. A parked thread waiting on a condvar costs nothing.
static void demo_finalize_ensure(void) {
    if (demo_fin_started) {
        return;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, demo_finalize_main, NULL)) {
        DebugPrint("demo: could not start finalize thread; captures will stay uncut\n");
        return;
    }
    pthread_detach(th);
    demo_fin_started = 1;
    DebugPrint("demo: finalize thread started\n");
}

// ---------------------------------------------------------------------------
// Game thread: match state and segment bookkeeping.
//
// One "segment" here is one file upstream opened for one slot: everything from
// a gamestate to the next gamestate, disconnect, map change or explicit
// Demo_Request(slot, -1).
//
// Segments are held in a flat table rather than one record per slot, because a
// slot's NEXT segment can be open while its PREVIOUS one is still on its way
// back through the completion queue. Demo_CaptureBody closes and reopens inside
// a single call on a re-gamestate, so Demo_GetPath(slot) already names the new
// file while the writer thread has not finished with the old one; keying on the
// slot alone would overwrite the old record and its job would never be built,
// leaving the match waiting on a segment that can no longer be accounted for.
// g_cur[slot] points at the one segment that is open right now, or -1.
// ---------------------------------------------------------------------------

// Same order as DEMO_MAX_POVS_PER_MATCH: this is the same population, seen one
// stage earlier.
#define DEMO_MAX_PENDING 128

// Minimum seconds between two "table is full" lines out of demo_seg_open - see
// the comment at that DebugPrint for why it cannot be left unrated.
#define DEMO_SEG_FULL_LOG_INTERVAL_S 60

typedef struct {
    char path[520];       // exactly what Demo_GetPath(slot) reported
    char final_path[512]; // where we will ship it
    char match_id[64];
    time_t seed_at;
    int32_t arm_seq; // netchan outgoingSequence at the arm instant, -1 if none
    int slot;
    int armed; // 1 once bound to a match_id
    // Read at the arm instant (svs->clients[slot].steam_id; 0 for a bot) and
    // the match's CS_MATCH_GUID as known so far - demo_match_guid_capture fills
    // it in for every armed segment once the game goes live. Game thread only.
    uint64_t steam_id;
    char match_guid[DEMO_MATCH_GUID_SIZE];
    // Bound at a mid-match join rather than at the countdown, so its shipped POV
    // starts sv_demoCutJoinLead seconds before arm_seq instead of at it.
    int joined_late;
    // At least one match was cut out of this capture through a snapshot and has
    // already shipped. What is still on disk under s->path is the whole file -
    // warmup and every match in it - so at completion it is removed rather than
    // trimmed and kept, which would republish a match a second time.
    int served_match;
    // A match was LIVE at some point while this segment was open. Only matters
    // for a segment that never got armed: that combination is a spectator's
    // camera follow of a real match, and sv_demoCut throws it away rather than
    // trimming and keeping it. Without the flag it is indistinguishable from a
    // capture whose match was cancelled, which is kept.
    int saw_live;
    int used;
} demo_seg_t;

static demo_seg_t g_seg[DEMO_MAX_PENDING];
static int g_cur[MAX_DEMO_CLIENTS]; // index into g_seg, or -1
static int g_cur_init;

// Per slot: this file put a Demo_Request(slot, -1) in to close the match's
// segment at game_end, and owes upstream a reset to 0 ("follow sv_demoRecord")
// once that close has landed. Without the reset the override would outlive the
// match and suppress this client's next-map capture; with an immediate reset
// the close itself would never happen, because the very next message would see
// sv_demoRecord set and keep the file open. DemoMatch_Frame pays the debt as
// soon as the slot stops recording; a disconnect clears it for free (upstream
// resets the override in Demo_ClientDisconnect).
static int g_close_pending[MAX_DEMO_CLIENTS];

// Per slot: the match a snapshot of this slot's capture is still being taken
// for, or seg < 0 when there is none.
//
// The completion carries the COPY's path rather than the capture's, so
// demo_seg_find cannot place it and this record is what does. It holds the
// match's own fields rather than reading them back off the segment, because the
// segment is unbound the moment the copy is REQUESTED: the next match can arm it
// while the copy is still being written, and a countdown two seconds after the
// last one is not a case worth being fragile about.
//
// One at a time per slot, which is also what keeps a second disarm from stacking
// another copy on top of an unfinished one.
typedef struct {
    int seg; // g_seg index the copy was taken from, or -1 for "nothing pending"
    char capture_path[520]; // that segment's file, to tell a recycled record apart
    char final_path[512];
    char match_id[64];
    time_t seed_at;
    int32_t arm_seq;
    int join_lead_ms;
    uint64_t steam_id;
    char match_guid[DEMO_MATCH_GUID_SIZE];
} demo_pending_snap_t;
static demo_pending_snap_t g_snap[MAX_DEMO_CLIENTS];

// Per-snapshot discriminator, for the same reason demo_seg_build_final has one:
// two matches of the same slot can otherwise collide inside a second.
static uint32_t g_snap_seq;

static int g_armed;
// The armed match has been seen live (game_start). A match that ends was
// always live first, so this only gates the abort path: a countdown that
// falls apart cancels the arm as if it never happened, rather than
// finalising a "match" nothing was played in.
static int g_live;
static char g_match_id[64];
static char g_map[64];
// The armed match's CS_MATCH_GUID, lower-case, or "" until a valid one is read.
// See demo_match_guid_capture.
static char g_match_guid[DEMO_MATCH_GUID_SIZE];
// The guid the last finished match carried, kept across the disarm. A
// CS_MATCH_GUID equal to it is the previous game's still in the configstring,
// not this one's - see demo_match_guid_capture.
static char g_prev_match_guid[DEMO_MATCH_GUID_SIZE];
static uint32_t g_seg_seq; // ever-increasing per-segment discriminator

// A match that has been disarmed but whose segments have not all come back
// through the completion queue yet. Its end marker is pushed when outstanding
// reaches 0, or when the deadline passes.
typedef struct {
    char match_id[64];
    int outstanding;
    time_t deadline;
    int used;
} demo_closing_t;

static demo_closing_t g_closing[DEMO_MAX_CLOSING];

static void demo_cur_init(void) {
    if (g_cur_init) {
        return;
    }
    for (int i = 0; i < MAX_DEMO_CLIENTS; i++) {
        g_cur[i]      = -1;
        g_snap[i].seg = -1;
    }
    g_cur_init = 1;
}

static void demo_cvars_ensure(void) {
    if (!Cvar_FindVar) {
        return;
    }
    if (!fs_homepath) {
        fs_homepath = Cvar_FindVar("fs_homepath");
    }
    if (!sv_demoDir) {
        sv_demoDir = Cvar_FindVar("sv_demoDir");
    }
    if (!sv_demoRecord) {
        sv_demoRecord = Cvar_FindVar("sv_demoRecord");
    }
    if (!mapname) {
        mapname = Cvar_FindVar("mapname");
    }
    // Ours to register: this is a plain sv_-family demo knob (like
    // sv_demoRecord/sv_demoDir, which demos.c owns), not something an operator
    // config line introduces on its own. demos.c itself stays untouched
    // upstream code (see the comment above demo_mkdir_p), so it is registered
    // from here instead.
    if (!sv_demoCut) {
        sv_demoCut = Cvar_Get("sv_demoCut", "0", CVAR_ARCHIVE);
    }
    // How many seconds before a mid-match join that client's shipped POV starts.
    // Only ever consulted for a client who was NOT in the game at the countdown
    // (see demo_seg_track); everyone who was starts at the countdown itself.
    if (!sv_demoCutJoinLead) {
        sv_demoCutJoinLead = Cvar_Get("sv_demoCutJoinLead", "15", CVAR_ARCHIVE);
    }

    // Once per process, on the first call that can actually resolve the demo
    // directory: clear stage directories orphaned by a previous run. Deliberately
    // NOT gated on sv_demoCut - an operator who turned the
    // feature off after a crash still wants the leftovers gone. Same placement as
    // upstream's own .part sweep in Demo_Init(), and cheap for the same reason:
    // one bounded directory walk, on the game thread, at startup.
    static int stage_swept;
    if (!stage_swept && fs_homepath && fs_homepath->string[0]) {
        stage_swept = 1;
        demo_stage_sweep();
    }
}

// Capture is upstream's alone: this file only ever attributes and post-
// processes what sv_demoRecord already writes. When the cvar is off there is
// nothing to attribute, so the match machinery stays idle too.
static int demo_upstream_records(void) {
    demo_cvars_ensure();
    return sv_demoRecord && sv_demoRecord->integer != 0;
}

// sv_demoCut: everything this file does is behind it. On, a capture bound to a
// match ships as the match-named two-stage cut, and a capture no match claimed
// is trimmed down to [went-live, end] (or deleted when no match ever went live
// inside it). Off, every capture stays exactly as upstream wrote it.
static int demo_cut_enabled(void) {
    demo_cvars_ensure();
    return sv_demoCut && sv_demoCut->integer != 0;
}

static int demo_slot_connected(int slot) {
    return svs && svs->clients && svs->clients[slot].state >= CS_CONNECTED;
}

// How long a mid-match joiner's POV reaches back before their join, in ms.
static int demo_join_lead_ms(void) {
    demo_cvars_ensure();
    int seconds = sv_demoCutJoinLead ? sv_demoCutJoinLead->integer : DEMO_JOIN_LEAD_DEFAULT_S;
    if (seconds < 0) {
        seconds = DEMO_JOIN_LEAD_DEFAULT_S;
    }
    if (seconds > DEMO_JOIN_LEAD_MAX_S) {
        seconds = DEMO_JOIN_LEAD_MAX_S;
    }
    return seconds * 1000;
}

// Is this slot IN the game right now, as opposed to spectating it?
//
// The distinction is the whole reason a spectator does not get a match POV: with
// sv_demoRecord set upstream records every connected client, and before this a
// duel with two spectators shipped four POVs - two of them a camera follow, which
// is dead weight in the .qlmatch and useless for analysis or restore.
//
// Read off the GAME module's own session team (the same field game_events.c's
// CheckTeams polls), not off client_t: TEAM_SPECTATOR is a gameplay fact the
// server-side client record does not carry. Every gametype agrees on it - duel
// players are TEAM_FREE, team games RED/BLUE, and only a spectator is
// TEAM_SPECTATOR - so no gametype special-casing is needed here.
//
// Returns 1 when the game module is not loaded (level/level->clients NULL during
// a module reload, see common.h). That is deliberately the permissive answer: a
// wrong "spectator" would DELETE a real player's capture, while a wrong "playing"
// only ships a POV nobody wanted.
static int demo_slot_playing(int slot) {
    if (!level || !level->clients) {
        return 1;
    }
    int maxclients = level->maxclients;
    if (maxclients > MAX_DEMO_CLIENTS) {
        maxclients = MAX_DEMO_CLIENTS;
    }
    if (slot < 0 || slot >= maxclients) {
        return 1;
    }
    gclient_t* client = &level->clients[slot];
    if (client->pers.connected != CON_CONNECTED) {
        return 0;
    }
    return client->sess.sessionTeam != TEAM_SPECTATOR;
}

// The sequence the client's NEXT message will carry: Netchan_Transmit consumes
// the current value and then increments, which is also why Demo_CaptureBody can
// compare it against gamestateMessageNum to spot a gamestate. This has to be
// sampled at the arm instant and nowhere else - the same instant of a match
// carries a DIFFERENT sequence number in every POV (measured pre-port on eight
// real captures of one match: the countdown message is seq 1009 in p0, 669 in
// p1, 3481 in p2, 8032 in p3-p7, all at server time 201725), so there is no
// server-wide sequence to recover it from later.
static int32_t demo_slot_seq(int slot) {
    if (!demo_slot_connected(slot)) {
        return -1;
    }
    return (int32_t)svs->clients[slot].netchan.outgoingSequence;
}

// Builds this segment's shipped path. Called once per segment, at the moment it
// is bound to a match, which is the first moment g_match_id/g_map are
// authoritative for it (a client can connect long before the match it will be
// recorded into even has an id).
//
// seg_time + seg_id are a per-SEGMENT discriminator. The name is otherwise
// unique only per (match, map, slot, client name), but one slot can legitimately
// open several segments inside a single match - a re-gamestate closes one and
// opens the next, and a client can disconnect and another take the same slot.
// Without a discriminator those would name the same file, silently overwriting
// the already-finalised output and racing the finalize thread (its work on
// segment 1 can take seconds, and its closing unlink() would then delete segment
// 2's freshly written capture). seed_at alone is not enough - time() has
// one-second resolution and both cases can happen inside the same second - so a
// process-wide counter closes that window.
static void demo_seg_build_final(demo_seg_t* s, const char* client_name) {
    demo_cvars_ensure();
    if (!fs_homepath || !fs_homepath->string[0]) {
        s->final_path[0] = '\0';
        return;
    }

    char pov[256];
    demo_build_pov_name(pov, sizeof(pov), s->match_id, g_map, s->slot, client_name);

    static const char dm_ext[] = ".dm_91";
    const size_t dm_ext_len    = sizeof(dm_ext) - 1;
    size_t povlen              = strlen(pov);
    if (povlen >= dm_ext_len && !strcmp(pov + povlen - dm_ext_len, dm_ext)) {
        pov[povlen - dm_ext_len] = '\0'; // splice before the extension, not after it.
    }

    const char* subdir = (sv_demoDir && sv_demoDir->string[0]) ? sv_demoDir->string : "demos";
    snprintf(s->final_path, sizeof(s->final_path), "%s/%s/%s_%ld_%u.dm_91", fs_homepath->string, subdir, pov,
             (long)s->seed_at, (unsigned)++g_seg_seq);
}

// Binds an open segment to the currently-armed match and reports it to Python.
static void demo_seg_arm(demo_seg_t* s) {
    if (s->armed || !g_armed) {
        return;
    }
    snprintf(s->match_id, sizeof(s->match_id), "%s", g_match_id);
    s->arm_seq  = demo_slot_seq(s->slot);
    s->steam_id = demo_slot_connected(s->slot) ? svs->clients[s->slot].steam_id : 0;
    snprintf(s->match_guid, sizeof(s->match_guid), "%s", g_match_guid);

    const char* name = demo_slot_connected(s->slot) ? svs->clients[s->slot].name : "";
    demo_seg_build_final(s, name);
    if (!s->final_path[0]) {
        DebugPrint("demo: slot %d has no fs_homepath; segment %s will not be published\n", s->slot, s->path);
        // Never bound to a match and never will be (demo_seg_arm is not
        // retried) - its completion takes the ordinary unclaimed path instead.
        s->match_id[0] = '\0';
        return;
    }
    s->armed = 1;

    DebugPrint("demo: slot %d armed at seq %d, %s -> %s\n", s->slot, (int)s->arm_seq, s->path, s->final_path);
#ifndef NOPY
    DemoRecordingStartedDispatcher(s->slot, s->final_path, name);
#endif
}

// Per-frame bookkeeping for the one segment a slot has open, whether it was just
// opened or has been open for a while. Two jobs, both of which only mean anything
// while a match is armed:
//
//   - Remember that a live match passed through this segment, so an unarmed one
//     can be recognised as a spectator's follow-cam at completion time.
//   - Bind a client who JOINS the game after the countdown. demo_arm_match only
//     binds the players who were in the game when the match armed, so this is the
//     only path a substitute coming off the bench gets a POV at all - and their
//     POV cannot start at the countdown they were not in, so it starts
//     sv_demoCutJoinLead seconds before the join instead.
//
// Deliberately NOT a team-change hook: the engine's own team plumbing has several
// entry points (SetTeam, ExecuteTeamChange, admin puts, a spectator auto-joining
// on a slot opening up) and a poll over the slots this loop already walks cannot
// miss any of them. Cost is one field read per connected slot per frame.
static void demo_seg_track(demo_seg_t* s) {
    if (!g_armed) {
        return;
    }
    if (g_live) {
        s->saw_live = 1;
    }
    if (s->armed || !demo_slot_playing(s->slot)) {
        return;
    }
    s->joined_late = 1;
    demo_seg_arm(s);
    if (s->armed) {
        DebugPrint("demo: slot %d joined %s after it armed; its POV starts %d ms earlier\n", s->slot,
                   g_match_id, demo_join_lead_ms());
    }
}

// A new segment for this slot. Returns its index in g_seg, or -1 when the table
// is full (in which case upstream simply keeps the file under its own name -
// still a valid demo, just not part of any .qlmatch).
//
// Idempotent per (slot, path): an already-tracked segment for the same open file
// is adopted rather than duplicated. That is not belt-and-braces - the engine
// calls ClientConnect (firstTime false) for every connected client on a plain
// map_restart, which is NOT a map change and does NOT give them a fresh
// gamestate, so DemoMatch_OnClientConnect drops g_cur[slot] while upstream's
// file for that slot stays open under the same name. Without this check the next
// frame opened a SECOND record for that one file, armed it as a second POV of
// the live match, and then waited DEMO_CLOSE_TIMEOUT_S for a completion that can
// never arrive (one file, one completion) - measured live on 2026-09-17: 8
// segments for 4 files, the match finalising 30 s late.
static int demo_seg_open(int slot, const char* path) {
    for (int i = 0; i < DEMO_MAX_PENDING; i++) {
        if (g_seg[i].used && g_seg[i].slot == slot && !strcmp(g_seg[i].path, path)) {
            return i;
        }
    }
    for (int i = 0; i < DEMO_MAX_PENDING; i++) {
        if (g_seg[i].used) {
            continue;
        }
        demo_seg_t* s = &g_seg[i];
        memset(s, 0, sizeof(*s));
        snprintf(s->path, sizeof(s->path), "%s", path);
        s->slot    = slot;
        s->seed_at = time(NULL);
        s->arm_seq = -1;
        s->used    = 1;
        return i;
    }
    // DemoMatch_Frame retries this every frame for as long as the slot keeps
    // recording, and DebugPrint is an unconditional printf in release builds too
    // (src/server/dllmain.c) - unrated this is dozens of lines per second, per
    // affected slot, indefinitely. "The table is full" is a standing condition
    // someone should look at, not news that needs sub-second freshness, so rate
    // limit it: a standing condition earns one line a minute, not one a frame.
    static time_t full_logged_at;
    time_t now = time(NULL);
    if (now - full_logged_at >= DEMO_SEG_FULL_LOG_INTERVAL_S) {
        full_logged_at = now;
        DebugPrint("demo: %d segments already awaiting completion; not tracking %s\n", DEMO_MAX_PENDING, path);
    }
    return -1;
}

// The completion carries the ".part" name on the failure paths, so match on the
// tracked path being a prefix of it rather than on equality.
static demo_seg_t* demo_seg_find(int slot, const char* path) {
    for (int i = 0; i < DEMO_MAX_PENDING; i++) {
        demo_seg_t* s = &g_seg[i];
        if (!s->used || s->slot != slot) {
            continue;
        }
        size_t n = strlen(s->path);
        if (n && !strncmp(s->path, path, n)) {
            return s;
        }
    }
    return NULL;
}

static demo_closing_t* demo_closing_find(const char* match_id) {
    for (int i = 0; i < DEMO_MAX_CLOSING; i++) {
        if (g_closing[i].used && !strcmp(g_closing[i].match_id, match_id)) {
            return &g_closing[i];
        }
    }
    return NULL;
}

// The payload-less job that rides the queue behind every one of this match's
// segment jobs, so the finalize thread reaches it only once they are all done.
static void demo_mark_match_end(const char* match_id) {
    demo_finalize_job_t* job = (demo_finalize_job_t*)calloc(1, sizeof(*job));
    if (!job) {
        DebugPrint("demo: out of memory marking %s finished\n", match_id);
        return;
    }
    snprintf(job->match_id, sizeof(job->match_id), "%s", match_id);
    job->arm_seq        = -1;
    job->last_for_match = 1;
    if (demo_finalize_push(job) != 0) {
        DebugPrint("demo: finalize queue full, no match-end marker for %s\n", match_id);
        free(job);
    }
}

static void demo_closing_release(demo_closing_t* c) {
    demo_mark_match_end(c->match_id);
    c->used = 0;
}

static void demo_closing_add(const char* match_id, int outstanding) {
    if (outstanding <= 0) {
        demo_mark_match_end(match_id);
        return;
    }
    for (int i = 0; i < DEMO_MAX_CLOSING; i++) {
        if (!g_closing[i].used) {
            snprintf(g_closing[i].match_id, sizeof(g_closing[i].match_id), "%s", match_id);
            g_closing[i].outstanding = outstanding;
            g_closing[i].deadline    = time(NULL) + DEMO_CLOSE_TIMEOUT_S;
            g_closing[i].used        = 1;
            return;
        }
    }
    // More than DEMO_MAX_CLOSING matches draining at once means something is
    // very wrong upstream of here; force the oldest out rather than losing this
    // one's marker entirely. The finalize thread's own "different match_id with
    // no end marker" path then flushes whatever that match had.
    DebugPrint("demo: %d matches already closing; forcing %s out early\n", DEMO_MAX_CLOSING,
               g_closing[0].match_id);
    demo_closing_release(&g_closing[0]);
    demo_closing_add(match_id, outstanding);
}

static void demo_closing_account(const char* match_id) {
    demo_closing_t* c = demo_closing_find(match_id);
    if (!c) {
        return; // still armed, or already released.
    }
    if (--c->outstanding <= 0) {
        demo_closing_release(c);
    }
}

// A match whose last segment never came back must not strand its .qlmatch.
static void demo_closing_deadlines(void) {
    time_t now = 0;
    for (int i = 0; i < DEMO_MAX_CLOSING; i++) {
        if (!g_closing[i].used) {
            continue;
        }
        if (!now) {
            now = time(NULL);
        }
        if (now >= g_closing[i].deadline) {
            DebugPrint("demo: match %s still had %d segment(s) unaccounted for after %d s; "
                       "finalising what did arrive\n",
                       g_closing[i].match_id, g_closing[i].outstanding, DEMO_CLOSE_TIMEOUT_S);
            demo_closing_release(&g_closing[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Match lifecycle. Driven by game_events.c's own polling of the engine state
// (the same crossings that fire game_countdown / game_start / game_end into
// Python), so no plugin is involved: with sv_demoRecord and sv_demoCut both
// set, matches are found, cut and finalised by this file alone.
// ---------------------------------------------------------------------------

// The moment a match arms is the moment its identity is fixed: the same UTC
// stamp convention a consumer globs for ("{match_id}_*.dm_91", see the naming
// note at the top), deliberately distinct from sv_demoNameFormat's default
// "%Y%m%d-%H%M%S" raw names.
static void demo_match_id_now(char* out, size_t n) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, n, "%Y%m%dT%H%M%SZ", &tm);
}

// Where a mid-capture snapshot lands, before the cut turns it into a POV.
//
// One shared staging directory rather than one per capture, so nothing has to
// clean an empty directory up afterwards: the files inside are unlinked by the
// same paths that already dispose of a finished capture (demo_stage1 after a
// successful cut, demo_pov_publish after a copy-through). Dot-prefixed so
// neither a consumer's "{match_id}_*.dm_91" glob nor a plain listing of the demo
// directory ever sees one, and the file name itself starts "sn_" rather than
// with the match_id, so a consumer globbing "{match_id}_*.dm_91" cannot pick up
// a snapshot even if it recurses. Prefixed ".sn_" specifically so the startup sweep
// in demo_stage_sweep_dir treats it like the other staging directories and
// clears out whatever a crash mid-finalize orphaned in it.
static int demo_snapshot_path(char* out, size_t out_len, const char* match_id, int slot) {
    demo_cvars_ensure();
    if (!fs_homepath || !fs_homepath->string[0]) {
        return 0;
    }
    const char* subdir = (sv_demoDir && sv_demoDir->string[0]) ? sv_demoDir->string : "demos";
    int written        = snprintf(out, out_len, "%s/%s/.sn_cache/sn_%s_s%02d_%u.dm_91", fs_homepath->string,
                                  subdir, match_id, slot, (unsigned)++g_snap_seq);
    return (written > 0 && (size_t)written < out_len);
}

// End this segment's part in the match that is closing, without ending the
// recording if that can be helped.
//
// Preferred: copy the capture as it stands (Demo_Snapshot) and leave it running.
// The capture then survives the map_restart a server does to get from one match
// to the next on the same map - a restart sends no fresh gamestate, and a .dm_91
// has to begin at one - so the NEXT match still has an open capture to arm. That
// is the whole of the "back-to-back matches record nothing" hole this used to
// have; see the note above demo_disarm_match.
//
// Fallback, when no copy can be had (no writeable path, ring full, or one
// already in flight for this slot): close it, which is what this always did.
// -1, not 0: 0 means "follow sv_demoRecord", which is set on any server this
// machinery runs on and would leave the segment open past the end of the match,
// so its file would never be cut, indexed or packed. -1 closes it on this
// client's next outgoing message, which is this frame or the next; DemoMatch_Frame
// then owes upstream the reset back to 0 - see g_close_pending.
static void demo_seg_close_for_match(demo_seg_t* s) {
    demo_pending_snap_t* pend = &g_snap[s->slot];
    char snap[DEMO_SNAPSHOT_PATH_MAX];

    if (pend->seg < 0 && demo_snapshot_path(snap, sizeof(snap), s->match_id, s->slot) &&
        Demo_Snapshot(s->slot, snap)) {
        // Everything the cut will need, taken now rather than read back off the
        // segment when the copy lands - by then the next match may already have
        // armed this very segment for itself.
        pend->seg          = (int)(s - g_seg);
        pend->seed_at      = s->seed_at;
        pend->arm_seq      = s->arm_seq;
        pend->join_lead_ms = s->joined_late ? demo_join_lead_ms() : 0;
        pend->steam_id     = s->steam_id;
        snprintf(pend->match_guid, sizeof(pend->match_guid), "%s", s->match_guid);
        snprintf(pend->capture_path, sizeof(pend->capture_path), "%.*s", (int)sizeof(s->path) - 1, s->path);
        snprintf(pend->final_path, sizeof(pend->final_path), "%s", s->final_path);
        snprintf(pend->match_id, sizeof(pend->match_id), "%s", s->match_id);

        DebugPrint("demo: slot %d snapshotting %s for %s; capture stays open\n", s->slot, s->path,
                   s->match_id);

        // Unbound immediately, so the next countdown can arm it. served_match is
        // deliberately NOT set here: only a copy that actually lands means this
        // capture's content has shipped, and until then its own completion must
        // still be allowed to trim and keep it.
        s->armed         = 0;
        s->arm_seq       = -1;
        s->joined_late   = 0;
        s->match_id[0]   = '\0';
        s->final_path[0] = '\0';
        return;
    }

    DebugPrint("demo: slot %d could not snapshot %s; closing it for %s instead\n", s->slot, s->path,
               s->match_id);
    Demo_Request(s->slot, -1);
    g_close_pending[s->slot] = 1;
}

// A Demo_Snapshot copy came back. The capture itself was never interrupted and
// its segment was handed back at request time; all that settles here is the
// match's claim, cut out of the copy instead of out of the file.
static void demo_on_snapshot(const demo_finished_t* done) {
    demo_pending_snap_t* pend = &g_snap[done->slot];
    if (pend->seg < 0) {
        return; // no copy was asked for; nothing this file can do with one.
    }

    char match_id[64];
    snprintf(match_id, sizeof(match_id), "%s", pend->match_id);

    if (done->failed || done->discarded) {
        // The match loses this POV. The capture is left alone on purpose: it is
        // still recording, may already be armed for the next match, and its own
        // completion can still trim and keep it (served_match was not set).
        DebugPrint("demo: slot %d snapshot for %s %s; no POV from this capture\n", done->slot, match_id,
                   done->discarded ? "held only a gamestate" : "failed");
        pend->seg = -1;
        demo_closing_account(match_id);
        return;
    }

    demo_finalize_job_t* job = (demo_finalize_job_t*)calloc(1, sizeof(*job));
    if (!job) {
        DebugPrint("demo: out of memory finalising %s\n", done->path);
        unlink(done->path);
    } else {
        snprintf(job->raw_path, sizeof(job->raw_path), "%s", done->path);
        snprintf(job->final_path, sizeof(job->final_path), "%s", pend->final_path);
        snprintf(job->match_id, sizeof(job->match_id), "%s", match_id);
        job->seed_at      = pend->seed_at;
        job->arm_seq      = pend->arm_seq;
        job->join_lead_ms = pend->join_lead_ms;
        job->slot         = done->slot;
        job->steam_id     = pend->steam_id;
        snprintf(job->match_guid, sizeof(job->match_guid), "%s", pend->match_guid);
        if (demo_finalize_push(job) != 0) {
            DebugPrint("demo: finalize queue full, leaving %s in place\n", done->path);
            free(job);
        }
    }

    // The capture this came out of has now shipped its content, so when it does
    // eventually end there is nothing left in it worth keeping. Matched on the
    // path as well as the index: the record may have been recycled for a
    // different capture while the copy was being written.
    demo_seg_t* s = &g_seg[pend->seg];
    if (s->used && s->slot == done->slot && !strcmp(s->path, pend->capture_path)) {
        s->served_match = 1;
    }
    pend->seg = -1;

    demo_closing_account(match_id);
}

// ---------------------------------------------------------------------------
// Public entry points (declared in demo_match.h). Game thread only.
// ---------------------------------------------------------------------------

void DemoMatch_OnClientConnect(int slot) {
    if (slot < 0 || slot >= MAX_DEMO_CLIENTS) {
        return;
    }
    demo_cur_init();
    // Whatever the previous occupant of this slot had open is already closed
    // (Demo_ClientDisconnect) and stays in the pending table under its own path
    // until its completion arrives - only the "currently open" pointer and the
    // close debt are stale, and both belong to that occupant, not this one
    // (upstream reset the request override itself on their disconnect).
    g_cur[slot]           = -1;
    g_close_pending[slot] = 0;
    // Nothing to request: with sv_demoRecord set, upstream opens this client's
    // segment at their gamestate on its own, and without it there is nothing
    // for this file to attribute.
}

void DemoMatch_Frame(void) {
    demo_cur_init();
    demo_cvars_ensure();
    if (svs && svs->clients) {
        for (int slot = 0; slot < MAX_DEMO_CLIENTS; slot++) {
            const char* p = Demo_IsRecording(slot) ? Demo_GetPath(slot) : NULL;
            if (!p) {
                g_cur[slot] = -1; // closed; its completion carries its own path.
                if (g_close_pending[slot]) {
                    // The game_end close has landed; hand the slot back to
                    // sv_demoRecord so its next gamestate opens a fresh
                    // upstream segment. See g_close_pending for why this
                    // cannot happen at the moment the -1 goes in.
                    g_close_pending[slot] = 0;
                    Demo_Request(slot, 0);
                }
                continue;
            }
            int idx = g_cur[slot];
            if (idx >= 0 && g_seg[idx].used && !strcmp(g_seg[idx].path, p)) {
                demo_seg_track(&g_seg[idx]); // same segment as last frame, but the player may have moved
                continue;
            }
            idx         = demo_seg_open(slot, p);
            g_cur[slot] = idx;
            if (idx >= 0) {
                demo_seg_track(&g_seg[idx]); // no-op unless a match is armed right now
            }
        }
    }
    demo_closing_deadlines();
}

void DemoMatch_OnFinished(const demo_finished_t* done) {
    if (!done || done->slot < 0 || done->slot >= MAX_DEMO_CLIENTS) {
        return;
    }
    demo_cur_init();

    if (done->snapshot) {
        // A copy of a capture that is STILL recording, not a segment that ended.
        // Placed by the slot's pending-snapshot record, since done->path names
        // the copy and demo_seg_find would not match it against any capture.
        demo_on_snapshot(done);
        return;
    }

    demo_seg_t* s = demo_seg_find(done->slot, done->path);
    if (!s) {
        return; // a segment we never tracked; upstream's own to keep.
    }
    if (g_cur[done->slot] >= 0 && &g_seg[g_cur[done->slot]] == s) {
        g_cur[done->slot] = -1;
    }
    if (g_snap[done->slot].seg >= 0 && &g_seg[g_snap[done->slot].seg] == s) {
        // A copy is still owed against a record that is wiped below. Only
        // reachable when a completion was dropped on overflow - the copy is
        // queued ahead of the close, so normally it has long since landed - and
        // leaving it standing would both block this slot's next snapshot and
        // leave its match waiting out demo_closing_deadlines for nothing.
        DebugPrint("demo: slot %d snapshot for %s never came back; releasing it\n", done->slot,
                   g_snap[done->slot].match_id);
        char lost[64];
        snprintf(lost, sizeof(lost), "%s", g_snap[done->slot].match_id);
        g_snap[done->slot].seg = -1;
        demo_closing_account(lost);
    }

    int armed         = s->armed;
    // Already cut and shipped through a snapshot, so what is on disk is the
    // whole capture, every match in it included. Checked ahead of spectated:
    // a served capture has saw_live set too, and the two want opposite logs.
    int served        = !s->armed && s->served_match;
    int spectated     = !s->armed && !s->served_match && s->saw_live;
    int join_lead_ms  = s->joined_late ? demo_join_lead_ms() : 0;
    char match_id[64];
    snprintf(match_id, sizeof(match_id), "%s", s->match_id);

    if (armed && !done->discarded && !done->failed) {
        demo_finalize_job_t* job = (demo_finalize_job_t*)calloc(1, sizeof(*job));
        if (!job) {
            DebugPrint("demo: out of memory finalising %s\n", done->path);
        } else {
            snprintf(job->raw_path, sizeof(job->raw_path), "%s", done->path);
            snprintf(job->final_path, sizeof(job->final_path), "%s", s->final_path);
            snprintf(job->match_id, sizeof(job->match_id), "%s", s->match_id);
            job->seed_at      = s->seed_at;
            job->arm_seq      = s->arm_seq;
            job->join_lead_ms = join_lead_ms;
            job->slot         = done->slot;
            job->steam_id     = s->steam_id;
            snprintf(job->match_guid, sizeof(job->match_guid), "%s", s->match_guid);
            if (demo_finalize_push(job) != 0) {
                DebugPrint("demo: finalize queue full, leaving %s in place\n", done->path);
                free(job);
            }
        }
    } else if (armed) {
        DebugPrint("demo: slot %d segment %s %s; nothing to cut for match %s\n", done->slot, done->path,
                   done->discarded ? "held only a gamestate" : "failed in the writer", match_id);
    } else if (served && !done->discarded && !done->failed && demo_cut_enabled()) {
        // Every match this capture carried has already been cut out of its own
        // snapshot and shipped. What is left under this name is the full file -
        // warmup, both matches, the lot - which is exactly what sv_demoCut
        // exists not to keep, and trimming it would ship a match twice.
        if (unlink(done->path) == 0) {
            DebugPrint("demo: sv_demoCut: slot %d capture %s already shipped as match POVs; removed\n",
                       done->slot, done->path);
        } else {
            DebugPrint("demo: sv_demoCut: could not remove spent capture %s\n", done->path);
        }
    } else if (spectated && !done->discarded && !done->failed && demo_cut_enabled()) {
        // A live match ran through this capture and its client never entered the
        // game: a spectator's camera follow. Not trimmed and kept like the other
        // unclaimed captures below - discarded, which is the whole point of not
        // binding spectators in the first place. Done here rather than on the
        // finalize thread because it needs no scan: the decision was already made
        // while the segment was open.
        if (unlink(done->path) == 0) {
            DebugPrint("demo: sv_demoCut: slot %d spectated %s without playing; %s removed\n", done->slot,
                       match_id[0] ? match_id : "a match", done->path);
        } else {
            DebugPrint("demo: sv_demoCut: could not remove spectator capture %s\n", done->path);
        }
    } else if (!done->discarded && !done->failed && demo_cut_enabled()) {
        // Unclaimed by any match. sv_demoCut still owes it a trim: cut it down
        // to [went-live, end] if a match went live inside it (a capture from a
        // cancelled arm, or one that outlived its match's close), delete it if
        // one never did (a warmup-only connection). Both are the finalize
        // thread's call - it is the one that can afford the scan.
        demo_finalize_job_t* job = (demo_finalize_job_t*)calloc(1, sizeof(*job));
        if (!job) {
            DebugPrint("demo: out of memory trimming %s\n", done->path);
        } else {
            snprintf(job->raw_path, sizeof(job->raw_path), "%s", done->path);
            job->seed_at   = s->seed_at;
            job->slot      = done->slot;
            job->trim_only = 1;
            demo_finalize_ensure(); // may be the first job ever queued, if no match has armed yet
            if (demo_finalize_push(job) != 0) {
                DebugPrint("demo: finalize queue full, leaving %s in place\n", done->path);
                free(job);
            }
        }
    }

    memset(s, 0, sizeof(*s));

    if (armed) {
        demo_closing_account(match_id);
    }
}

// Back-to-back matches on the same map, with no reconnect and no map load in
// between, used to record nothing for the second one: a valid .dm_91 has to
// start at a gamestate, ending the match closed the capture, and the map_restart
// that starts the next match sends no new gamestate for a new capture to begin
// at. The fix is not to close it. demo_seg_close_for_match takes a copy of the
// still-open capture instead (Demo_Snapshot), the match is cut out of the copy,
// and the capture itself runs straight through the restart into the next match,
// which arms the very same segment record again.
//
// keep_capturing says whether the capture has a future worth preserving. A match
// ending on a live server does; DemoMatch_OnCloseAll does not - there the files
// are being finalised anyway, so the old close is both correct and cheaper.
static void demo_disarm_match(int keep_capturing) {
    demo_cur_init();
    if (!g_armed) {
        return;
    }
    char match_id[64];
    snprintf(match_id, sizeof(match_id), "%s", g_match_id);

    if (g_match_guid[0]) {
        snprintf(g_prev_match_guid, sizeof(g_prev_match_guid), "%s", g_match_guid);
    }
    g_armed         = 0;
    g_live          = 0;
    g_match_id[0]   = '\0';
    g_map[0]        = '\0';
    g_match_guid[0] = '\0';

    // Every segment of this match that has not already come back through the
    // completion queue, whether it is still open or merely still in flight.
    int outstanding = 0;
    for (int i = 0; i < DEMO_MAX_PENDING; i++) {
        demo_seg_t* s = &g_seg[i];
        if (!s->used || !s->armed || strcmp(s->match_id, match_id) != 0) {
            continue;
        }
        outstanding++;
        if (g_cur[s->slot] >= 0 && &g_seg[g_cur[s->slot]] == s) {
            if (keep_capturing) {
                demo_seg_close_for_match(s);
            } else {
                // See demo_seg_close_for_match for why -1 and not 0.
                Demo_Request(s->slot, -1);
                g_close_pending[s->slot] = 1;
            }
        }
    }

    // The marker cannot go out yet: those segments' jobs have to reach the
    // finalize thread ahead of it. See DemoMatch_OnFinished for where the count
    // comes down, and demo_closing_deadlines for the backstop.
    demo_closing_add(match_id, outstanding);
    DebugPrint("demo: disarmed %s, %d segment(s) still closing\n", match_id, outstanding);
}

// The arm never happened: unbind every segment it claimed and forget the match.
// For a countdown that fell apart before the match went live - there is nothing
// to finalise, no marker to send and no event to fire, and the captures keep
// recording under sv_demoRecord exactly as if the countdown had never started
// (their completions take the unclaimed-trim path in DemoMatch_OnFinished).
static void demo_cancel_match(void) {
    demo_cur_init();
    if (!g_armed) {
        return;
    }
    int unbound = 0;
    for (int i = 0; i < DEMO_MAX_PENDING; i++) {
        demo_seg_t* s = &g_seg[i];
        if (!s->used || !s->armed || strcmp(s->match_id, g_match_id) != 0) {
            continue;
        }
        s->armed         = 0;
        s->arm_seq       = -1;
        s->joined_late   = 0; // the next arm decides this again from scratch
        s->match_id[0]   = '\0';
        s->final_path[0] = '\0';
        unbound++;
    }
    DebugPrint("demo: match %s cancelled before going live; %d segment(s) unbound\n", g_match_id, unbound);
    g_armed         = 0;
    g_live          = 0;
    g_match_id[0]   = '\0';
    g_map[0]        = '\0';
    g_match_guid[0] = '\0';
}

static void demo_arm_match(const char* match_id, const char* map) {
    demo_cur_init();
    if (g_armed) {
        // Re-arm without an end in between: close a live match out first; an
        // arm that never went live never happened.
        if (g_live) {
            demo_disarm_match(1);
        } else {
            demo_cancel_match();
        }
    }
    demo_cvars_ensure();
    Demo_SanitiseName(g_match_id, sizeof(g_match_id), match_id);
    Demo_SanitiseName(g_map, sizeof(g_map), map);
    demo_finalize_ensure();
    g_armed         = 1;
    g_live          = 0;
    g_match_guid[0] = '\0'; // read at game_start; see demo_match_guid_capture

    if (!svs || !svs->clients) {
        DebugPrint("demo: armed %s on %s (engine not ready; slots bind as they open)\n", g_match_id, g_map);
        return;
    }

    // Bind the PLAYERS already being captured. Under sv_demoRecord that capture
    // covers every connected client, spectators included, but a spectator's POV
    // is a camera follow: dead weight in the .qlmatch and useless for analysis,
    // so it is not made part of the match (and sv_demoCut discards it at
    // completion - see DemoMatch_OnFinished). Someone who joins the game later
    // still gets a POV; demo_seg_track binds them when they do.
    //
    // No Demo_Request here: capture is the cvar's, this file only attributes it.
    for (int slot = 0; slot < MAX_DEMO_CLIENTS; slot++) {
        if (!demo_slot_connected(slot) || !demo_slot_playing(slot)) {
            continue;
        }
        int idx = g_cur[slot];
        if (idx >= 0 && g_seg[idx].used) {
            demo_seg_arm(&g_seg[idx]);
        }
    }
    DebugPrint("demo: armed %s on %s\n", g_match_id, g_map);
}

// CS_MATCH_GUID, lower-cased, into out (DEMO_MATCH_GUID_SIZE bytes). Returns 1
// for a well-formed UUID (8-4-4-4-12 hex digits), 0 with out = "" for anything
// else, an empty configstring included. Game thread only: it reads
// sv.configstrings. raw receives what the configstring actually held, for the
// log line of a caller that got 0.
static int demo_match_guid_read(char* out, char* raw, size_t raw_len) {
    out[0] = '\0';
    raw[0] = '\0';
    if (!SV_GetConfigstring) {
        return 0;
    }
    SV_GetConfigstring(CS_MATCH_GUID, raw, (int)raw_len);
    if (strlen(raw) != DEMO_MATCH_GUID_LEN) {
        return 0;
    }
    char guid[DEMO_MATCH_GUID_SIZE];
    for (int i = 0; i < DEMO_MATCH_GUID_LEN; i++) {
        char c = raw[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') {
                return 0;
            }
        } else if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
        guid[i] = c;
    }
    guid[DEMO_MATCH_GUID_LEN] = '\0';
    snprintf(out, DEMO_MATCH_GUID_SIZE, "%s", guid);
    return 1;
}

// Ties the armed match to the engine's own game id, for the snapshot index.
// Read at game_start and again at game_end because when the engine sets
// CS_MATCH_GUID relative to those two crossings is not established; a valid
// value read later replaces an earlier different one (logged), so the guid
// stored is the one the game ended with - the same one the telemetry plugin
// reads at match_end. A value equal to the previous finished game's is that
// game's guid still in the configstring and is not written. Copied into every
// segment still armed for the match; demo_seg_arm copies it into any that arm
// later. Game thread only - the finalize thread sees the value only through its
// jobs.
static void demo_match_guid_capture(const char* when) {
    if (!g_armed) {
        return;
    }
    char guid[DEMO_MATCH_GUID_SIZE];
    char raw[128];
    if (!demo_match_guid_read(guid, raw, sizeof(raw))) {
        if (raw[0]) {
            DebugPrint("demo: %s CS_MATCH_GUID at %s is not a UUID (\"%s\"); ignored\n", g_match_id, when, raw);
        } else if (!g_match_guid[0]) {
            DebugPrint("demo: %s has no CS_MATCH_GUID at %s\n", g_match_id, when);
        }
        return;
    }
    if (g_prev_match_guid[0] && !strcmp(g_prev_match_guid, guid)) {
        DebugPrint("demo: CS_MATCH_GUID %s equals the previous game's; not written\n", guid);
        return;
    }
    if (g_match_guid[0]) {
        if (!strcmp(g_match_guid, guid)) {
            return;
        }
        DebugPrint("demo: %s CS_MATCH_GUID changed by %s: was %s, now %s\n", g_match_id, when, g_match_guid, guid);
    }
    snprintf(g_match_guid, sizeof(g_match_guid), "%s", guid);
    for (int i = 0; i < DEMO_MAX_PENDING; i++) {
        demo_seg_t* s = &g_seg[i];
        if (s->used && s->armed && !strcmp(s->match_id, g_match_id)) {
            snprintf(s->match_guid, sizeof(s->match_guid), "%s", g_match_guid);
        }
    }
    DebugPrint("demo: %s is match_guid %s (read at %s)\n", g_match_id, g_match_guid, when);
}

// The countdown is the moment the match's identity is fixed and its segments
// bind - the shipped files start here (stage 1 cuts at the arm instant).
void DemoMatch_OnGameCountdown(void) {
    if (!demo_cut_enabled() || !demo_upstream_records()) {
        return;
    }
    char match_id[64];
    demo_match_id_now(match_id, sizeof(match_id));
    demo_cvars_ensure();
    const char* map = (mapname && mapname->string[0]) ? mapname->string : "unknown";
    demo_arm_match(match_id, map);
}

// The armed match went live. Also the late-arm fallback: a game that reaches
// live without this file having armed at its countdown (sv_demoCut flipped on
// mid-countdown, or a start with no countdown at all) is still worth cutting -
// from here instead of the countdown, which is all the information there is.
void DemoMatch_OnGameStart(void) {
    if (g_armed) {
        g_live = 1;
        demo_match_guid_capture("game_start");
        return;
    }
    if (!demo_cut_enabled() || !demo_upstream_records()) {
        return;
    }
    char match_id[64];
    demo_match_id_now(match_id, sizeof(match_id));
    demo_cvars_ensure();
    const char* map = (mapname && mapname->string[0]) ? mapname->string : "unknown";
    demo_arm_match(match_id, map);
    g_live = 1;
    demo_match_guid_capture("game_start");
}

// Both engine paths to a match ending land here: intermission queued, and a
// live game abandoned back to warmup (forfeit, admin stop).
void DemoMatch_OnGameEnd(void) {
    if (!g_armed) {
        return;
    }
    // Before the disarm, so the snapshot taken there still carries the guid.
    demo_match_guid_capture("game_end");
    demo_disarm_match(1);
}

// A countdown that fell apart (player left, ready state lost) before the match
// went live. Nothing was played, so the arm is taken back rather than
// finalised - without this, the next real match's arm would flush a "match"
// consisting of warmup.
void DemoMatch_OnCountdownCancelled(void) {
    if (g_armed && !g_live) {
        demo_cancel_match();
    }
}

void DemoMatch_OnCloseAll(void) {
    // Upstream's Demo_CloseAll() finalises every open segment, so their
    // completions are on their way; all this has to do is stop the match from
    // waiting for anything else. A match that never went live (a map change
    // voted mid-countdown) is cancelled, not finalised.
    if (g_armed && !g_live) {
        demo_cancel_match();
        return;
    }
    demo_disarm_match(0);
}
