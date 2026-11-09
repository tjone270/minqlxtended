// Standalone harness for democut's reading side (demo_scan + demo_index).
//
// Build (from democut/):
//     gcc -std=gnu11 -O2 -Wall -Wextra -o test_scan *.c
//     # ...except the other harness, which has its own main():
//     gcc -std=gnu11 -O2 -Wall -Wextra -o test_scan \
//         test_scan.c democut.c dc_parser.c dc_msg_read.c dc_msg_write.c \
//         dc_fields.c dc_huffman.c
//
// Usage:
//     ./test_scan <demo.dm_91> [arm_seq]        human-readable
//     ./test_scan --json <demo.dm_91> [arm_seq] one line of JSON
//
// The JSON form is what the UDT differ consumes: it prints exactly the fields
// the acceptance criteria compare (first_ms/last_ms/snapshot_count and the
// index's own rows), so "does the new cutter agree with the old one" is a
// textual diff of two of these, not a judgement call.
//
// Beyond printing, this runs the data-integrity checks that must hold for ANY
// .dm_91, and exits non-zero if one fails:
//
//   1. demo_index()'s row count equals demo_scan()'s snapshot_count, and the
//      two walks agree on first_ms/last_ms/gamestate_count. They come from one
//      shared walker, so a disagreement means that walker grew a branch.
//   2. Every row's byte offset is inside the file, offsets are strictly
//      increasing, and off + 8 + len never runs past the end. This is the
//      property a seeker depends on: the index is a byte map, and a row that
//      points outside the file silently breaks playback rather than the index.
//   3. No row reports delta = -1 ("could not be determined"). The value is read
//      straight out of the decoded message now, so anything else is a bug.
//   4. The first row of a file with no clock reset is a full snapshot
//      (delta == 0) when the file is a cut output - reported, not enforced,
//      because a raw capture legitimately starts mid-delta-chain.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "democut.h"

static long long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    const long long n = ftell(f);
    fclose(f);
    return n;
}

// The scalar fields come from the demo_scan() run (it is the only one given an
// arm_seq, so arm_ms/live_ms/clock_resets_since_arm only exist there); the rows
// come from demo_index(), which always walks with arm_seq = -1. Mixing them the
// other way round silently prints -1 for every arm-derived field.
static void print_json(const char *path, const demo_scan_t *s, const demo_index_result_t *idx,
                       int arm_seq) {
    printf("{\"file\":\"%s\",\"arm_seq\":%d,", path, arm_seq);
    printf("\"first_ms\":%d,\"last_ms\":%d,\"arm_ms\":%d,\"live_ms\":%d,", s->first_ms, s->last_ms,
           s->arm_ms, s->live_ms);
    printf("\"live_seq\":%d,\"arm_epoch_seq\":%d,\"arm_epoch_firstms\":%d,", s->live_seq,
           s->arm_epoch_seq, s->arm_epoch_firstms);
    printf("\"gamestate_count\":%d,\"clock_resets\":%d,\"clock_resets_since_arm\":%d,", s->gamestate_count,
           s->clock_resets, s->clock_resets_since_arm);
    printf("\"clock_resets_since_live\":%d,", s->clock_resets_since_live);
    printf("\"snapshot_count\":%d,\"message_count\":%d,\"client_num\":%d,", s->snapshot_count,
           s->message_count, s->client_num);
    printf("\"delta_unknown\":%d,\"snapshots\":[", idx->delta_unknown);
    for (int i = 0; i < idx->count; i++) {
        const demo_snap_t *r = &idx->snaps[i];
        printf("%s{\"t\":%d,\"off\":%lld,\"len\":%d,\"delta\":%d}", i ? "," : "", r->t, r->off, r->len,
               r->delta);
    }
    printf("]}\n");
}

int main(int argc, char **argv) {
    int json    = 0;
    int argBase = 1;

    if (argc > 1 && strcmp(argv[1], "--json") == 0) {
        json    = 1;
        argBase = 2;
    }
    if (argc <= argBase) {
        fprintf(stderr, "usage: %s [--json] <demo.dm_91> [arm_seq]\n", argv[0]);
        return 2;
    }

    const char *path = argv[argBase];
    const int arm_seq = (argc > argBase + 1) ? atoi(argv[argBase + 1]) : -1;

    char err[512];
    demo_scan_t scan;
    if (demo_scan(path, arm_seq, &scan, err, (int)sizeof(err)) != 0) {
        fprintf(stderr, "demo_scan failed: %s\n", err);
        return 1;
    }

    demo_index_result_t idx;
    if (demo_index(path, &idx, err, (int)sizeof(err)) != 0) {
        fprintf(stderr, "demo_index failed: %s\n", err);
        return 1;
    }

    int failures = 0;

    // (1) the two entry points must describe the same file. demo_index runs the
    // walk with arm_seq = -1, so arm_ms/live_ms are not comparable; everything
    // else is.
    if (idx.count != scan.snapshot_count) {
        fprintf(stderr, "FAIL: demo_index has %d row(s), demo_scan counted %d snapshot(s)\n", idx.count,
                scan.snapshot_count);
        failures++;
    }
    if (idx.scan.first_ms != scan.first_ms || idx.scan.last_ms != scan.last_ms ||
        idx.scan.gamestate_count != scan.gamestate_count ||
        idx.scan.message_count != scan.message_count || idx.scan.client_num != scan.client_num) {
        fprintf(stderr,
                "FAIL: demo_scan says [%d,%d] %d gs %d msgs client %d, demo_index says [%d,%d] %d gs "
                "%d msgs client %d\n",
                scan.first_ms, scan.last_ms, scan.gamestate_count, scan.message_count, scan.client_num,
                idx.scan.first_ms, idx.scan.last_ms, idx.scan.gamestate_count, idx.scan.message_count,
                idx.scan.client_num);
        failures++;
    }

    // (2) the index is a byte map of this exact file.
    const long long size = file_size(path);
    if (size < 0) {
        fprintf(stderr, "FAIL: cannot stat %s\n", path);
        failures++;
    } else {
        long long prevOff = -1;
        for (int i = 0; i < idx.count; i++) {
            const demo_snap_t *r = &idx.snaps[i];
            if (r->off < 0 || r->off + 8 + (long long)r->len > size) {
                fprintf(stderr, "FAIL: row %d points at [%lld,%lld) but the file is %lld bytes\n", i,
                        r->off, r->off + 8 + (long long)r->len, size);
                failures++;
                break;
            }
            if (r->off < prevOff) {
                fprintf(stderr, "FAIL: row %d offset %lld goes backwards (previous %lld)\n", i, r->off,
                        prevOff);
                failures++;
                break;
            }
            if (r->len <= 0) {
                fprintf(stderr, "FAIL: row %d has length %d\n", i, r->len);
                failures++;
                break;
            }
            prevOff = r->off;
        }
    }

    // (3) delta is always a real wire value now.
    if (idx.delta_unknown != 0) {
        fprintf(stderr, "FAIL: %d row(s) report delta = -1\n", idx.delta_unknown);
        failures++;
    }
    for (int i = 0; i < idx.count; i++) {
        if (idx.snaps[i].delta < 0 || idx.snaps[i].delta > 255) {
            fprintf(stderr, "FAIL: row %d has delta %d, which is not a wire byte\n", i, idx.snaps[i].delta);
            failures++;
            break;
        }
    }

    if (json) {
        print_json(path, &scan, &idx, arm_seq);
    } else {
        printf("scan  messages=%d snapshots(gs0)=%d gamestates=%d clock_resets=%d\n", scan.message_count,
               scan.snapshot_count, scan.gamestate_count, scan.clock_resets);
        printf("scan  first_ms=%d last_ms=%d span_ms=%d\n", scan.first_ms, scan.last_ms,
               scan.last_ms - scan.first_ms);
        printf("scan  arm_seq=%d -> arm_ms=%d (offset from first: %d ms), clock_resets_since_arm=%d\n",
               arm_seq, scan.arm_ms, (scan.arm_ms < 0) ? -1 : scan.arm_ms - scan.first_ms,
               scan.clock_resets_since_arm);
        printf("scan  arm epoch starts at seq=%d, %d ms\n", scan.arm_epoch_seq, scan.arm_epoch_firstms);
        printf("scan  live_ms=%d live_seq=%d (cs %d warmup->live at/after arm), "
               "clock_resets_since_live=%d\n",
               scan.live_ms, scan.live_seq, DEMO_CS_WARMUP_INDEX, scan.clock_resets_since_live);
        printf("scan  client_num=%d\n", scan.client_num);
        printf("index rows=%d delta_unknown=%d", idx.count, idx.delta_unknown);
        if (idx.count > 0) {
            int fullSnapshots = 0;
            for (int i = 0; i < idx.count; i++) {
                if (idx.snaps[i].delta == 0) {
                    fullSnapshots++;
                }
            }
            printf(" first_row={t=%d off=%lld len=%d delta=%d} full_snapshots=%d", idx.snaps[0].t,
                   idx.snaps[0].off, idx.snaps[0].len, idx.snaps[0].delta, fullSnapshots);
        }
        printf("\n");
        printf(failures ? "RESULT: %d integrity check(s) FAILED\n" : "RESULT: OK (%d failures)\n",
               failures);
    }

    demo_index_free(&idx);
    return failures ? 1 : 0;
}
