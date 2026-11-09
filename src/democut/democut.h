// democut - a self-contained .dm_91 (Quake Live protocol 91) reader and cutter.
//
// This is the public surface src/features/demo_match.c is written against. It
// replaces a 1.3 MB vendored C++ copy of uberdemotools plus its extern "C"
// bridge; the API, its units and its failure modes are deliberately unchanged
// from that bridge, because demo_match.c's two-stage cut depends on every one
// of them. What DID change is that everything behind it is now plain C
// (gnu11), built with the same gcc the rest of minqlxtended is, with no C++
// toolchain and no third-party tree involved.
//
// Licensing: GPLv2, like minqlxtended itself. The protocol tables, the Huffman
// coder and the delta encoders are ports of id Software's Quake III Arena
// source (via wolfcamql, the Quake Live-capable engine that plays these files);
// the cut's message selection rules are a port of what the previously vendored
// uberdemotools did, which is itself id Tech 3's CL_Parse* with an output side.
//
// ===========================================================================
// THE CLOCK. Read this before touching a start/end value anywhere.
// ===========================================================================
//
// start_ms/end_ms are in the demo's *server time* clock: milliseconds since the
// map loaded on the server that recorded it. This is NOT wall-clock/epoch time
// and NOT "milliseconds since this recording started" - a real per-player
// capture from this pipeline had its first snapshot at 176550 and its last at
// 703000, i.e. the file begins 176.55 s of server uptime in, not at 0. The
// numbers to feed demo_cut() come from demo_scan() on the same file (first_ms /
// last_ms / arm_ms), never from wall time; job->seed_at in demo_match.c is wall
// time and is deliberately never used for this.
//
// end_ms = -1 does NOT mean "cut to the end of the file". It never did: the
// vendored cutter silently queued zero cuts for it and still reported success,
// leaving an empty output folder with nothing logged, which cost a live test
// run once. demo_cut() rejects start_ms >= end_ms outright (non-zero return,
// message in err_buf) so that failure mode cannot come back silently. To cut to
// the end of a file, pass that file's own last_ms from demo_scan(), or a large
// sentinel such as INT32_MAX - the window test clamps to the last snapshot that
// actually exists.
//
// ===========================================================================
// WHAT A CUT ACTUALLY PRODUCES
// ===========================================================================
//
// A .dm_91 is a delta chain whose first message MUST be a gamestate, so a cut
// cannot simply copy bytes from the middle of a file. demo_cut() therefore
// walks the input, keeps the accumulated config strings and entity baselines,
// and at the first message whose server time falls inside [start_ms, end_ms]
// writes a SYNTHESIZED gamestate carrying that state - replacing that message's
// own content, which is why the first snapshot of a cut output is the one from
// the SECOND in-window message, not the first. Everything after it is
// re-encoded message by message until the first message past end_ms.
//
// Three properties of that output matter to callers:
//   - Message sequence numbers are the input's own, unchanged. A snapshot's
//     delta reference is relative ("this many messages back"), so renumbering
//     would repoint every one of them.
//   - Server command sequence numbers ARE renumbered from 0, because the
//     synthesized gamestate restarts that counter and the client refuses a gap.
//   - The first snapshot written is forced to be a full (non-delta) one, since
//     the frame it was delta-encoded against is not in the output.

#ifndef DEMOCUT_H
#define DEMOCUT_H

#ifdef __cplusplus
extern "C" {
#endif

// Cuts [start_ms, end_ms] out of in_path (a .dm_91) into a new file under
// out_folder, named "<input stem>_CUT_<mmss>_<mmss>.dm_91". out_folder must be
// a directory the caller can treat as exclusively this call's own, so there is
// never more than one candidate output file to find afterwards - demo_match.c
// creates a per-invocation scratch directory for exactly this reason (a prior
// design globbed a shared folder for "the newest .dm_91" and raced).
//
// Returns 0 on success. On failure returns non-zero and writes a human-readable
// message into err_buf (truncated to err_buf_len). Producing no output file at
// all counts as a failure here, deliberately: the caller cannot tell an empty
// scratch directory apart from a cut that selected nothing, and silently
// shipping the untrimmed capture instead is the wrong default.
//
// start_seq is a lower bound on the BLOCK SEQUENCE NUMBER at which the window
// may open (pass -1 for none, i.e. time alone decides). It exists because
// [start_ms, end_ms] on its own cannot identify a point in a real capture: a
// map_restart resets the server clock WITHOUT sending a fresh gamestate, so one
// gamestate-0 file can hold two clock epochs whose numeric ranges overlap, and
// the stale earlier one comes first - a time-only test opens the cut there and
// the output carries a backward clock jump (seen live on 2026-09-17: every POV
// of a match shipped untrimmed and unindexed for exactly this reason). Block
// sequence numbers are monotonic over the whole file, so a caller that sampled
// one at the instant it wants (demo_match.c's arm_seq, or demo_scan's live_seq)
// can pin the opening edge exactly. The CLOSING edge is still time-only: a
// reset after the opening edge is a genuinely unusable window, and callers gate
// on clock_resets_since_arm / clock_resets_since_live for that.
int demo_cut(const char *in_path, const char *out_folder,
             int start_ms, int end_ms, int start_seq,
             char *err_buf, int err_buf_len);

// ---------------------------------------------------------------------------
// Reading a .dm_91's own server-time clock.
//
// The capture side (upstream demos.c) never decodes message content: all it
// knows about a message is the netchan outgoingSequence it stamped into the
// file's 8-byte per-block header. demo_scan() closes that gap - it walks the
// framing itself, decodes each message, and reports the server times it finds,
// keyed by that same sequence number. The correlation is exact, not heuristic:
// the sequence number the caller reads out of the block header IS the
// messageNum the snapshot inside that block is stamped with.
// ---------------------------------------------------------------------------

// Everything demo_scan reports is scoped to the demo's FIRST gamestate, because
// that is the only one demo_cut() addresses. This is not a theoretical
// distinction: real per-POV captures have been seen with two gamestates, whose
// server-time clock jumps backwards mid-file (1077875 -> 7625) when the level
// reloads; treating such a file as one continuous clock silently produces a
// negative span. Callers must check gamestate_count and refuse to cut when it
// is not 1. The normal capture path can only ever produce 1, so anything else
// means a stranded or hand-recovered file.
typedef struct demo_scan_s {
    // Server time (ms) of the first / last snapshot of gamestate 0.
    // -1 when there is no snapshot at all.
    int first_ms;
    int last_ms;

    // Server time (ms) of the first snapshot carried by a message whose
    // sequence number is >= the arm_seq passed in - i.e. "what the server clock
    // read at the instant this POV was armed". -1 if arm_seq was negative or no
    // message at/after it carried a snapshot.
    int arm_ms;

    // Server time (ms) of the first snapshot at/after config string
    // DEMO_CS_WARMUP_INDEX is set to "\time\0" (QL's "match in progress"),
    // considering only updates at/after arm_seq. In other words: the server
    // clock at the instant the countdown ended and the match went live.
    // -1 when no such update happens at/after arm_seq - normal for a file that
    // was already in progress when it started (cs 5 then only appears in the
    // gamestate, never as a command) and for a pure-warmup file. Diagnostic
    // only - see demo_match.c for why the two-stage cut deliberately does NOT
    // move its window to this time.
    int live_ms;

    // Block sequence number of the message that carried the live_ms snapshot,
    // i.e. demo_cut()'s start_seq for a cut that starts at live_ms. -1 whenever
    // live_ms is -1.
    int live_seq;

    // The clock epoch arm_ms lives in: the block sequence number and the server
    // time of that epoch's FIRST snapshot. Both -1 when arm_ms is -1.
    //
    // This is the pair to hand demo_cut() when the window should open EARLIER
    // than arm_ms - a caller wanting "from a few seconds before this instant"
    // (demo_match.c's mid-match joiner) cannot pass arm_seq as start_seq,
    // because that is precisely the point it needs to reach back past. Passing
    // arm_epoch_seq instead still excludes every earlier clock epoch (which is
    // all start_seq was ever protecting against) while leaving the time bound
    // free to move anywhere inside this one. arm_epoch_firstms is how far back
    // that can go: the epoch's own first snapshot, so a caller can clamp.
    //
    // For a window that starts exactly at arm_ms this pair is equivalent to
    // arm_seq: server time is monotonic within one epoch, so the first message
    // at/after arm_epoch_seq whose time is >= arm_ms IS the arm message.
    int arm_epoch_seq;
    int arm_epoch_firstms;

    // Number of gamestate messages in the whole file. 1 for anything the normal
    // capture path produces; anything else means first_ms/last_ms/arm_ms only
    // describe the leading gamestate and demo_cut() must not be used.
    int gamestate_count;

    // How many times the server-time clock jumped BACKWARDS between two
    // consecutive snapshots inside gamestate 0, counted over the WHOLE file.
    // Non-zero here is common and often harmless: a raw capture stays open
    // across back-to-back matches on the same map with no client reconnect, and
    // a soft server respawn between them resets the server clock WITHOUT
    // sending a new gamestate - so a raw capture can legitimately hold a stale,
    // already-consumed match's tail, then a clock reset, then the current
    // match's clean data. That leading pre-arm portion is discarded by the cut
    // regardless, so a reset confined to it must not veto the cut. Kept for
    // diagnostics; gate the cut on clock_resets_since_arm instead.
    int clock_resets;

    // Same count, but only for resets detected AT OR AFTER arm_ms was found
    // (i.e. within the region demo_cut() will actually be asked to select from).
    // -1 when arm_seq was negative. THIS is the field to check being 0 before
    // trusting a cut of [arm_ms, last_ms]: the cut selects messages by a plain
    // "start <= t <= end" comparison with no notion of where in the file a
    // message sits, so a reset inside the scanned window means two clock epochs
    // really do overlap the requested range and the cut would be silently wrong
    // rather than failing.
    int clock_resets_since_arm;

    // Same, counted from live_ms instead of arm_ms - for the caller that cuts
    // [live_ms, last_ms] (demo_match.c's trim-only path) and therefore only
    // cares about resets inside THAT region. -1 when arm_seq was negative.
    int clock_resets_since_live;

    // Diagnostics. snapshot_count covers gamestate 0 only; message_count is the
    // whole file.
    int snapshot_count;
    int message_count;

    // The client number of the player who recorded the demo, straight off the
    // FIRST gamestate message. -1 when the file has no gamestate at all. This
    // is the demo's own idea of "whose POV is this", which is not necessarily
    // the server slot the capture side named the file after.
    int client_num;
} demo_scan_t;

// QL's warmup/in-progress config string (the spec's "cs 5"). Confirmed against
// real per-POV captures: index 5 is the only config string carrying a "\time\"
// key, and it goes "\time\-1" (no match) -> "\time\<absolute start time>"
// (countdown running) -> "\time\0" (in progress). Note the countdown value is a
// large positive number, so "not -1" is NOT the same as "live"; only an exact 0
// means the match is running.
#define DEMO_CS_WARMUP_INDEX 5

// Walks in_path once and fills *out. arm_seq may be negative to skip the
// arm_ms/live_ms lookups. Returns 0 on success, non-zero with a message in
// err_buf otherwise.
int demo_scan(const char *in_path, int arm_seq, demo_scan_t *out,
              char *err_buf, int err_buf_len);

// ---------------------------------------------------------------------------
// The per-POV snapshot byte index (index/<demo stem>.snaps.json).
//
// demo_index() is demo_scan()'s walk with the per-snapshot rows kept instead of
// thrown away. Both go through ONE internal walker, so there is exactly one
// implementation of "decode this file" and the two can never disagree about
// what a file contains.
// ---------------------------------------------------------------------------

typedef struct demo_snap_s {
    // Byte offset, from the start of the file, of this message's OWN 8-byte
    // (int32 seq, int32 len) header - not of the payload behind it. This is the
    // spec's "index_framing": "with_header" convention. The value comes from
    // the walker's own read loop, which is also what hands the message to the
    // parser, so offset and server time cannot desync.
    long long off;

    // Server time (ms) of the snapshot this message carries. Same clock as
    // demo_scan()'s first_ms/last_ms and demo_cut()'s start/end.
    int t;

    // The header's own len field: payload bytes following the 8-byte header. So
    // the next message's header starts at off + 8 + len.
    int len;

    // The svc_snapshot "deltaNum" byte as the file actually carries it: how
    // many messages BACK the snapshot this one is delta-compressed against
    // sits. 0 means a full/keyframe snapshot that can be decoded with no
    // history - the only rows a seeker can jump straight to. It is the RELATIVE
    // wire value on purpose: an absolute message number would be meaningless
    // after a cut renumbers nothing but drops everything before its gamestate.
    //
    // Never -1 from this implementation. The field keeps the "-1 means could
    // not be determined" contract the vendored bridge had (it had to recover
    // this value from uberdemotools' internals, which could fail), so consumers
    // written against that contract keep working; delta_unknown below is
    // therefore always 0 now.
    int delta;
} demo_snap_t;

typedef struct demo_index_result_s {
    // Everything demo_scan() would have reported for the same file.
    demo_scan_t scan;

    // One row per snapshot of gamestate 0, in file order. NULL when count is 0.
    // Owned by the caller: release with demo_index_free().
    demo_snap_t *snaps;
    int count; // always equals scan.snapshot_count on success

    // How many rows got delta = -1. Always 0 with this implementation, which
    // reads the byte directly out of the message it just decoded.
    int delta_unknown;
} demo_index_result_t;

// Walks in_path once and fills *out, including a row per snapshot.
// Returns 0 on success, non-zero with a message in err_buf otherwise.
// On success the caller MUST call demo_index_free(out) exactly once.
int demo_index(const char *in_path, demo_index_result_t *out,
               char *err_buf, int err_buf_len);

// Releases out->snaps and zeroes the pointer/count. Safe on a zeroed struct and
// safe to call twice.
void demo_index_free(demo_index_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEMOCUT_H */
