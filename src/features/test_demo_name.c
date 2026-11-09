// Standalone harness for Demo_SanitiseName, the player-name part of a demo filename.
//
// Build (from the repository root):
//     gcc -std=gnu11 -O2 -Wall -Wextra -Isrc -o test_demo_name src/features/test_demo_name.c src/features/demo_name.c
//
// Usage:
//     ./test_demo_name            runs every case below, exits non-zero on a failure
//
// Not part of the Makefile on purpose, like src/democut's harnesses: it has its
// own main() and must not link into the .so.

#include <stdio.h>
#include <string.h>

#include "features/demo_name.h"

typedef struct {
    const char *what;
    const char *in;
    size_t n;
    const char *want;
} case_t;

static const case_t cases[] = {
    {"plain ascii", "Vasya", 64, "Vasya"},
    {"colour codes stripped, space to underscore", "^1Red^7 Name", 64, "Red_Name"},
    {"ascii punctuation dropped", "a.b/c\\d:e*f?g\"h<i>j|k", 64, "abcdefghijk"},
    {"path traversal dropped", "../etc", 64, "etc"},
    {"empty name", "", 64, "x"},
    {"nothing usable", "!!!", 64, "x"},
    // "Петров" in UTF-8: every Cyrillic letter is a two-byte sequence.
    {"cyrillic kept", "\xD0\x9F\xD0\xB5\xD1\x82\xD1\x80\xD0\xBE\xD0\xB2", 64,
     "\xD0\x9F\xD0\xB5\xD1\x82\xD1\x80\xD0\xBE\xD0\xB2"},
    {"mixed latin and cyrillic", "Vasya_\xD0\x9F\xD0\xB5\xD1\x82\xD1\x80\xD0\xBE\xD0\xB2", 64,
     "Vasya_\xD0\x9F\xD0\xB5\xD1\x82\xD1\x80\xD0\xBE\xD0\xB2"},
    {"colour code before cyrillic", "^3\xD0\x9F\xD0\xB5", 64, "\xD0\x9F\xD0\xB5"},
    // U+4E2D (three bytes) and U+1F600 (four bytes).
    {"three-byte sequence kept", "\xE4\xB8\xAD", 64, "\xE4\xB8\xAD"},
    {"four-byte sequence kept", "\xF0\x9F\x98\x80", 64, "\xF0\x9F\x98\x80"},
    {"stray continuation byte dropped", "\x80" "abc", 64, "abc"},
    {"truncated sequence at the end dropped", "ab\xD0", 64, "ab"},
    {"truncated sequence in the middle dropped", "ab\xD0" "cd", 64, "abcd"},
    {"overlong form dropped", "\xC0\xAF", 64, "x"},
    {"surrogate dropped", "\xED\xA0\x80", 64, "x"},
    {"above U+10FFFF dropped", "\xF4\x90\x80\x80", 64, "x"},
    // A buffer of 4 holds one two-byte character plus NUL; the second must not be cut in half.
    {"never a cut sequence at the buffer end", "\xD0\x9F\xD0\xB5", 4, "\xD0\x9F"},
    {"ascii fills the buffer exactly", "abcdef", 4, "abc"},
    {"null source", NULL, 64, "x"},
};

int main(void) {
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char out[64];
        memset(out, 0x7f, sizeof(out));
        Demo_SanitiseName(out, cases[i].n, cases[i].in);
        if (strcmp(out, cases[i].want) != 0) {
            printf("FAIL %s: got \"%s\" want \"%s\"\n", cases[i].what, out, cases[i].want);
            failed++;
        }
    }
    printf("%s: %zu cases, %d failed\n", failed ? "FAIL" : "OK", sizeof(cases) / sizeof(cases[0]), failed);
    return failed ? 1 : 0;
}
