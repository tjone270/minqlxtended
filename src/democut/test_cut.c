// Standalone harness for democut's writing side (demo_cut).
//
// Build (from democut/):
//     gcc -std=gnu11 -O2 -Wall -Wextra -o test_cut \
//         test_cut.c democut.c dc_parser.c dc_msg_read.c dc_msg_write.c \
//         dc_fields.c dc_huffman.c
//
// Usage:
//     ./test_cut <in.dm_91> <out_folder> <start_ms> [end_ms] [start_seq]
//
// start_seq is the optional lower bound on the block sequence number at which
// the window may open (-1, the default, for none). Pass the arm sequence to
// reproduce what production asks for; leave it off to reproduce a time-only
// selection, including the two-epoch failure the checks below describe.
//
// out_folder must already exist and must be this call's own, exactly as
// demo_cut()'s contract requires (see democut.h) - the harness verifies it
// contains precisely one file afterwards, which is the same check
// demo_match.c's demo_only_output() makes in production.
//
// end_ms defaults to INT32_MAX rather than -1 on purpose: -1 is the trap
// documented in democut.h (it never meant "to end of file"), and a harness that
// reproduced it by default would keep that mistake alive. A large sentinel does
// what people expect, because the window test stops at the last snapshot that
// actually exists.
//
// After cutting, the output is re-scanned and its own range/snapshot count
// printed, so a cut can be checked without a second tool. The checks are:
// exactly one gamestate, the shipped first_ms inside the requested window, the
// shipped last_ms not past end_ms, and no server-clock reset that the source did
// not already have. That last qualifier matters - a raw capture can hold two
// clock epochs, and a time-only window that reaches the earlier one correctly
// yields an output that contains the reset; production never asks for that,
// because it pins the opening edge with start_seq and gates the rest on
// demo_scan's clock_resets_since_arm.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "democut.h"

static int only_output(const char *dir, char *out, size_t out_len) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "cannot open %s\n", dir);
        return 0;
    }
    int found = 0;
    struct dirent *e;
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
        fprintf(stderr, "expected exactly 1 cut output in %s, found %d\n", dir, found);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 6) {
        fprintf(stderr, "usage: %s <in.dm_91> <out_folder> <start_ms> [end_ms] [start_seq]\n", argv[0]);
        return 2;
    }

    const int start_ms = atoi(argv[3]);
    const int end_ms   = (argc >= 5) ? atoi(argv[4]) : 2147483647;
    // Optional, and -1 by default, so the harness keeps exercising the plain
    // time-only selection unless a test explicitly pins the opening edge.
    const int start_seq = (argc >= 6) ? atoi(argv[5]) : -1;

    char err[512];

    // The source's own range first: several of the checks below are only
    // meaningful relative to what went in. In particular a raw capture can
    // legitimately contain two server-clock epochs (a soft respawn between
    // back-to-back matches resets the clock without a new gamestate), and a
    // window that spans the reset produces an output that contains it too -
    // correct behaviour, not a cutter bug. Production never asks for such a
    // window; it gates on demo_scan's clock_resets_since_arm first.
    demo_scan_t src;
    if (demo_scan(argv[1], -1, &src, err, (int)sizeof(err)) != 0) {
        fprintf(stderr, "demo_scan of the input failed: %s\n", err);
        return 1;
    }

    if (demo_cut(argv[1], argv[2], start_ms, end_ms, start_seq, err, (int)sizeof(err)) != 0) {
        fprintf(stderr, "demo_cut failed: %s\n", err);
        return 1;
    }

    char cut[1024];
    if (!only_output(argv[2], cut, sizeof(cut))) {
        return 1;
    }

    demo_scan_t scan;
    if (demo_scan(cut, -1, &scan, err, (int)sizeof(err)) != 0) {
        fprintf(stderr, "demo_cut produced %s but it does not scan: %s\n", cut, err);
        return 1;
    }

    printf("demo_cut OK: %s\n", cut);
    printf("  source    [%d,%d] %d snapshot(s), %d gamestate(s), %d clock reset(s)\n", src.first_ms,
           src.last_ms, src.snapshot_count, src.gamestate_count, src.clock_resets);
    printf("  requested [%d,%d]\n", start_ms, end_ms);
    printf("  shipped   [%d,%d] %d snapshot(s), %d message(s), %d gamestate(s), %d clock reset(s), "
           "client_num %d\n",
           scan.first_ms, scan.last_ms, scan.snapshot_count, scan.message_count, scan.gamestate_count,
           scan.clock_resets, scan.client_num);

    int failures = 0;
    if (scan.gamestate_count != 1) {
        fprintf(stderr, "FAIL: a cut output must contain exactly 1 gamestate, got %d\n",
                scan.gamestate_count);
        failures++;
    }
    if (scan.first_ms < start_ms || scan.first_ms > end_ms) {
        fprintf(stderr, "FAIL: first shipped snapshot %d is outside the requested [%d,%d]\n",
                scan.first_ms, start_ms, end_ms);
        failures++;
    }
    if (scan.last_ms > end_ms) {
        fprintf(stderr, "FAIL: last shipped snapshot %d is past the requested end %d\n", scan.last_ms,
                end_ms);
        failures++;
    }
    if (scan.clock_resets != 0) {
        if (src.clock_resets == 0) {
            fprintf(stderr, "FAIL: the cut output has %d backwards clock jump(s) but its source had none\n",
                    scan.clock_resets);
            failures++;
        } else {
            printf("  note: %d backwards clock jump(s) carried over from the source (which has %d) - "
                   "the requested window spans a server-clock reset\n",
                   scan.clock_resets, src.clock_resets);
        }
    }
    return failures ? 1 : 0;
}
