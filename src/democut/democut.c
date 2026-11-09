// democut - public implementation of demo_scan / demo_index / demo_cut.
// See democut.h for the contract, the clock and the failure modes.
//
// GPLv2, same as minqlxtended.

#include "democut.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dc_parser.h"
#include "dc_protocol.h"

// ---------------------------------------------------------------------------
// shared walk
// ---------------------------------------------------------------------------

typedef struct {
    demo_scan_t *scan;
    int armSeq;

    int haveFirst;
    int pendingLive;
    int seenArm;

    // First snapshot of the clock epoch being walked right now: its block
    // sequence and its server time. Re-seeded at every backward clock jump, so
    // when arm_ms is found these describe the epoch the arm instant lives in.
    int epochSeq;
    int epochMs;

    // Current block, set by the walker before each message so the snapshot
    // callback can stamp an index row with it.
    long long curOff;
    int curLen;
    int curSeq;

    // demo_index() only.
    int collectRows;
    demo_snap_t *rows;
    int rowCount;
    int rowCap;
    int oom;
} dc_scan_state_t;

static void dc_err(char *err_buf, int err_buf_len, const char *fmt, ...) {
    if (!err_buf || err_buf_len <= 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, (size_t)err_buf_len, fmt, ap);
    va_end(ap);
}

// Returns 1 when a raw config string value carries "\time\0" exactly - QL's
// "the match is in progress" marker. Returns 0 for any other "\time\" value and
// for a value with no "\time\" key at all. See DEMO_CS_WARMUP_INDEX in
// democut.h for the three values this key actually takes.
static int dc_cs_time_is_match_live(const char *cs) {
    static const char key[] = "\\time\\";
    if (cs == NULL) {
        return 0;
    }
    const char *p = strstr(cs, key);
    if (p == NULL) {
        return 0;
    }
    p += sizeof(key) - 1;
    return (p[0] == '0' && (p[1] == '\0' || p[1] == '\\')) ? 1 : 0;
}

static int dc_rows_add(dc_scan_state_t *st, long long off, int t, int len, int delta) {
    if (st->oom) {
        return 0;
    }
    if (st->rowCount == st->rowCap) {
        // ~40 Hz snapshots, so 4096 rows is roughly the first 100 seconds; a
        // long match doubles a handful of times and stops there.
        const int newCap       = (st->rowCap == 0) ? 4096 : (st->rowCap * 2);
        demo_snap_t *const buf = (demo_snap_t *)realloc(st->rows, (size_t)newCap * sizeof(demo_snap_t));
        if (buf == NULL) {
            st->oom = 1;
            return 0;
        }
        st->rows   = buf;
        st->rowCap = newCap;
    }
    demo_snap_t *const row = &st->rows[st->rowCount++];
    row->off               = off;
    row->t                 = t;
    row->len               = len;
    row->delta             = delta;
    return 1;
}

static void dc_on_gamestate(dc_parser_t *p, void *user) {
    dc_scan_state_t *st = (dc_scan_state_t *)user;
    st->scan->gamestate_count++;
    if (st->scan->gamestate_count == 1) {
        st->scan->client_num = p->inClientNum;
    }
}

static void dc_on_command(dc_parser_t *p, int seq, const char *text, int csIndex, const char *csValue,
                          void *user) {
    dc_scan_state_t *st = (dc_scan_state_t *)user;
    (void)seq;
    (void)text;

    if (p->inGameStateIndex != 0) {
        return; // past the leading gamestate: a different clock entirely
    }
    if (csIndex != DEMO_CS_WARMUP_INDEX || st->scan->live_ms >= 0) {
        return;
    }
    if (st->armSeq < 0 || st->curSeq < st->armSeq) {
        return;
    }
    if (dc_cs_time_is_match_live(csValue)) {
        // Commands are parsed before the snapshot inside one server message, so
        // a config string update seen here is already in effect for this
        // message's own snapshot - which is the one whose time gets reported.
        st->pendingLive = 1;
    }
}

static void dc_on_snapshot(dc_parser_t *p, const dc_snapshot_t *snap, void *user) {
    dc_scan_state_t *st = (dc_scan_state_t *)user;
    demo_scan_t *s      = st->scan;
    const int t         = snap->serverTime;

    if (p->inGameStateIndex != 0) {
        return; // only gamestate 0 contributes times
    }

    if (!st->haveFirst) {
        s->first_ms  = t;
        st->haveFirst = 1;
        st->epochSeq  = st->curSeq;
        st->epochMs   = t;
    } else if (t < s->last_ms) {
        st->epochSeq = st->curSeq; // a new clock epoch starts on this snapshot
        st->epochMs  = t;
        s->clock_resets++;
        // arm_ms already set means this backward jump sits inside
        // [arm_ms, ...] - the exact region a cut would be asked to select from.
        // A jump still ahead of arm_ms is stale, pre-arm data the cut discards
        // regardless and must not veto it.
        if (s->arm_ms >= 0) {
            s->clock_resets_since_arm++;
        }
        // Same reasoning, for a caller that cuts from live_ms instead of arm_ms
        // (demo_match.c's trim-only path, which has no arm instant).
        if (s->live_ms >= 0) {
            s->clock_resets_since_live++;
        }
    }
    s->last_ms = t;
    s->snapshot_count++;

    if (s->arm_ms < 0 && st->armSeq >= 0 && st->curSeq >= st->armSeq) {
        s->arm_ms            = t;
        s->arm_epoch_seq     = st->epochSeq;
        s->arm_epoch_firstms = st->epochMs;
    }
    if (st->pendingLive && s->live_ms < 0) {
        s->live_ms      = t;
        s->live_seq     = st->curSeq;
        st->pendingLive = 0;
    }

    if (st->collectRows) {
        dc_rows_add(st, st->curOff, t, st->curLen, snap->deltaWireNum);
    }
}

// collectRows != 0 turns on the per-snapshot index rows.
static int dc_walk(const char *who, const char *in_path, int arm_seq, demo_scan_t *out,
                   dc_scan_state_t *st, int collectRows, dc_parser_t *parser,
                   char *err_buf, int err_buf_len) {
    FILE *fp          = NULL;
    dc_byte *buffer   = NULL;
    long long filePos = 0;
    int rc            = 0;

    if (err_buf && err_buf_len > 0) {
        err_buf[0] = '\0';
    }

    memset(out, 0, sizeof(*out));
    out->first_ms              = -1;
    out->last_ms               = -1;
    out->arm_ms                = -1;
    out->live_ms               = -1;
    out->live_seq              = -1;
    out->arm_epoch_seq         = -1;
    out->arm_epoch_firstms     = -1;
    out->client_num            = -1;
    out->clock_resets_since_arm  = (arm_seq >= 0) ? 0 : -1;
    out->clock_resets_since_live = (arm_seq >= 0) ? 0 : -1;

    memset(st, 0, sizeof(*st));
    st->scan        = out;
    st->armSeq      = arm_seq;
    st->curSeq      = -1;
    st->collectRows = collectRows;

    fp = fopen(in_path, "rb");
    if (fp == NULL) {
        dc_err(err_buf, err_buf_len, "%s: cannot open %s", who, in_path);
        return 1;
    }

    buffer = (dc_byte *)malloc(DC_MAX_MSGLEN);
    if (buffer == NULL) {
        fclose(fp);
        dc_err(err_buf, err_buf_len, "%s: out of memory", who);
        return 1;
    }

    parser->cb.user         = st;
    parser->cb.on_gamestate = dc_on_gamestate;
    parser->cb.on_command   = dc_on_command;
    parser->cb.on_snapshot  = dc_on_snapshot;

    for (;;) {
        const long long msgOff = filePos;
        int32_t header[2];

        if (fread(header, sizeof(int32_t), 2, fp) != 2) {
            break; // clean end of file, or a capture truncated by a crash
        }
        if (header[0] == -1 && header[1] == -1) {
            break; // the writer's explicit end-of-demo marker
        }
        const int seq = (int)header[0];
        const int len = (int)header[1];
        if (len <= 0 || len > DC_MAX_MSGLEN) {
            dc_err(err_buf, err_buf_len, "%s: bad block length %d at message %d in %s", who, len,
                   out->message_count, in_path);
            rc = 1;
            break;
        }
        if (fread(buffer, 1, (size_t)len, fp) != (size_t)len) {
            break; // truncated tail: keep whatever we parsed up to here
        }
        filePos = msgOff + 8 + (long long)len;

        st->curOff = msgOff;
        st->curLen = len;
        st->curSeq = seq;

        const int step = dc_parser_parse_message(parser, buffer, len, seq);
        out->message_count++;
        if (step < 0) {
            dc_err(err_buf, err_buf_len, "%s: %s (message %d of %s)", who,
                   parser->errSet ? parser->err : "parse error", out->message_count, in_path);
            rc = 1;
            break;
        }
        if (st->oom) {
            dc_err(err_buf, err_buf_len, "%s: out of memory collecting snapshot %d of %s", who,
                   out->snapshot_count, in_path);
            rc = 1;
            break;
        }
        if (step == 0) {
            break; // the cut finished; nothing past here is wanted
        }
    }

    dc_parser_finish(parser);
    if (rc == 0 && parser->errSet) {
        dc_err(err_buf, err_buf_len, "%s: %s", who, parser->err);
        rc = 1;
    }

    free(buffer);
    fclose(fp);
    return rc;
}

// ---------------------------------------------------------------------------
// demo_scan
// ---------------------------------------------------------------------------

static int dc_scan_common(const char *who, const char *in_path, int arm_seq, demo_scan_t *out,
                          dc_scan_state_t *st, int collectRows, char *err_buf, int err_buf_len) {
    memset(st, 0, sizeof(*st));
    if (in_path == NULL || out == NULL) {
        dc_err(err_buf, err_buf_len, "%s: NULL argument", who);
        return 1;
    }

    dc_parser_t *parser = dc_parser_create();
    if (parser == NULL) {
        dc_err(err_buf, err_buf_len, "%s: out of memory", who);
        return 1;
    }

    int rc = dc_walk(who, in_path, arm_seq, out, st, collectRows, parser, err_buf, err_buf_len);
    dc_parser_destroy(parser);

    if (rc == 0 && out->snapshot_count == 0) {
        dc_err(err_buf, err_buf_len, "%s: no snapshot found in %s (%d messages)", who, in_path,
               out->message_count);
        rc = 1;
    }
    return rc;
}

int demo_scan(const char *in_path, int arm_seq, demo_scan_t *out, char *err_buf, int err_buf_len) {
    dc_scan_state_t st;
    const int rc = dc_scan_common("demo_scan", in_path, arm_seq, out, &st, 0, err_buf, err_buf_len);
    free(st.rows);
    return rc;
}

// ---------------------------------------------------------------------------
// demo_index
// ---------------------------------------------------------------------------

int demo_index(const char *in_path, demo_index_result_t *out, char *err_buf, int err_buf_len) {
    dc_scan_state_t st;

    if (err_buf && err_buf_len > 0) {
        err_buf[0] = '\0';
    }
    if (out == NULL) {
        dc_err(err_buf, err_buf_len, "demo_index: NULL argument");
        return 1;
    }
    memset(out, 0, sizeof(*out));

    // arm_seq is -1: arm_ms/live_ms are a property of the RAW capture (the
    // sequence number the capture side stamped), not of a finalised file, and
    // the index is only ever built for finalised files.
    const int rc = dc_scan_common("demo_index", in_path, -1, &out->scan, &st, 1, err_buf, err_buf_len);
    if (rc != 0) {
        free(st.rows);
        memset(out, 0, sizeof(*out));
        return rc;
    }

    out->snaps         = st.rows;
    out->count         = st.rowCount;
    out->delta_unknown = 0;

    // Belt and braces: the rows come off the same callback as snapshot_count in
    // the same walk, so a mismatch is impossible without a code change - which
    // is exactly when a caller most wants to be told rather than to ship a
    // silently short index.
    if (out->count != out->scan.snapshot_count) {
        dc_err(err_buf, err_buf_len, "demo_index: collected %d row(s) but counted %d snapshot(s) in %s",
               out->count, out->scan.snapshot_count, in_path);
        demo_index_free(out);
        return 1;
    }
    return 0;
}

void demo_index_free(demo_index_result_t *out) {
    if (out == NULL) {
        return;
    }
    free(out->snaps);
    out->snaps = NULL;
    out->count = 0;
}

// ---------------------------------------------------------------------------
// demo_cut
// ---------------------------------------------------------------------------

// UDT's FormatTimeForFileName: minutes unpadded, seconds zero-padded to two.
// Kept identical so an operator grepping older logs still recognises the name.
static void dc_format_time(char *out, size_t out_len, int timeMs) {
    const char *sign = "";
    if (timeMs < 0) {
        timeMs = -timeMs;
        sign   = "-";
    }
    const int secondsTotal = timeMs / 1000;
    const int minutes      = secondsTotal / 60;
    const int seconds      = secondsTotal - (minutes * 60);
    snprintf(out, out_len, "%s%d%02d", sign, minutes, seconds);
}

static void dc_file_stem(const char *path, char *out, size_t out_len) {
    const char *sep  = strrchr(path, '/');
    const char *leaf = sep ? sep + 1 : path;
    snprintf(out, out_len, "%s", leaf);
    char *dot = strrchr(out, '.');
    if (dot != NULL && dot != out) {
        *dot = '\0';
    }
}

int demo_cut(const char *in_path, const char *out_folder, int start_ms, int end_ms, int start_seq,
             char *err_buf, int err_buf_len) {
    if (err_buf && err_buf_len > 0) {
        err_buf[0] = '\0';
    }
    if (in_path == NULL || out_folder == NULL) {
        dc_err(err_buf, err_buf_len, "demo_cut: NULL argument");
        return 1;
    }

    // The end_ms = -1 trap, made loud on purpose: see democut.h. A window that
    // selects nothing used to come back as success with an empty output folder.
    if (start_ms >= end_ms) {
        dc_err(err_buf, err_buf_len,
               "invalid range: start_ms (%d) must be < end_ms (%d); note end_ms=-1 does NOT mean "
               "\"to end of file\" -- pass a real end server-time (e.g. INT32_MAX)",
               start_ms, end_ms);
        return 1;
    }

    char stem[512];
    char startText[32];
    char endText[32];
    char outPath[1024];

    dc_file_stem(in_path, stem, sizeof(stem));
    dc_format_time(startText, sizeof(startText), start_ms);
    dc_format_time(endText, sizeof(endText), end_ms);

    const int written = snprintf(outPath, sizeof(outPath), "%s/%s_CUT_%s_%s.dm_91", out_folder, stem,
                                 startText, endText);
    if (written <= 0 || (size_t)written >= sizeof(outPath)) {
        dc_err(err_buf, err_buf_len, "demo_cut: output path too long for %s in %s", in_path, out_folder);
        return 1;
    }

    dc_parser_t *parser = dc_parser_create();
    if (parser == NULL) {
        dc_err(err_buf, err_buf_len, "demo_cut: out of memory");
        return 1;
    }
    dc_parser_set_cut(parser, start_ms, end_ms, start_seq, outPath);

    demo_scan_t scan;
    dc_scan_state_t st;
    int rc = dc_walk("demo_cut", in_path, -1, &scan, &st, 0, parser, err_buf, err_buf_len);
    const int outMessages  = parser->outMessageCount;
    const int outSnapshots = parser->outSnapshotCount;
    free(st.rows);
    dc_parser_destroy(parser);

    if (rc != 0) {
        remove(outPath);
        return rc;
    }
    if (outMessages == 0) {
        dc_err(err_buf, err_buf_len,
               "demo_cut: window [%d,%d] (from seq %d) selected no message of %s (its own range is "
               "[%d,%d], %d snapshot(s), %d gamestate(s))",
               start_ms, end_ms, start_seq, in_path, scan.first_ms, scan.last_ms, scan.snapshot_count,
               scan.gamestate_count);
        remove(outPath);
        return 1;
    }
    if (outSnapshots == 0) {
        // A gamestate with nothing after it is a file the engine opens and then
        // sits on forever. Treat it as a failed cut rather than shipping it.
        dc_err(err_buf, err_buf_len,
               "demo_cut: window [%d,%d] (from seq %d) of %s produced a gamestate but no snapshot",
               start_ms, end_ms, start_seq, in_path);
        remove(outPath);
        return 1;
    }
    return 0;
}
