// Message-level parser for .dm_91, plus the re-encoder the cut needs.
//
// This is a port of what uberdemotools' udtBaseParser did for this project,
// which is itself id Tech 3's CL_Parse* with an output side bolted on. The
// order of operations is preserved deliberately, because it is what decides
// which messages a cut keeps: see dc_parser_parse_message() for the full
// annotated sequence.
//
// GPLv2, same as minqlxtended and the id Tech 3 code it derives from.

#ifndef DC_PARSER_H
#define DC_PARSER_H

#include <stdio.h>

#include "dc_msg.h"
#include "dc_protocol.h"

typedef struct dc_parser_s dc_parser_t;

typedef struct {
    void *user;
    // Fired after a complete gamestate message has been parsed.
    void (*on_gamestate)(dc_parser_t *p, void *user);
    // Fired for every server command, after it has been applied to the parser's
    // config string table. csIndex is -1 for anything that is not a "cs".
    void (*on_command)(dc_parser_t *p, int seq, const char *text, int csIndex, const char *csValue, void *user);
    // Fired once per VALID snapshot, after it has been stored in the ring.
    void (*on_snapshot)(dc_parser_t *p, const dc_snapshot_t *snap, void *user);
} dc_parser_cb_t;

struct dc_parser_s {
    // --- input state (mirrors udtBaseParser's _in* members) ---
    int inServerMessageSequence;
    int inServerCommandSequence;
    int inReliableSequenceAcknowledge;
    int inClientNum;
    int inChecksumFeed;
    int inServerTime;
    int inLastSnapshotMessageNumber;
    int inGameStateIndex; // -1 until the first gamestate; 0 for the first
    int inParseEntitiesNum;

    char *configStrings[DC_MAX_CONFIGSTRINGS]; // NULL = never set
    dc_entityState_t baselines[DC_MAX_GENTITIES];
    dc_snapshot_t snapshots[DC_PACKET_BACKUP];
    dc_entityState_t parseEntities[DC_MAX_PARSE_ENTITIES];

    // Holds "cs <index> \"<value>\"" while a bcs0/bcs1*/bcs2 chain is being
    // reassembled. Three times BIG_INFO_STRING so the worst case the tokenizer
    // can hand it (two maximum-length tokens plus the wrapper) still fits
    // without truncation.
    char bigConfigString[3 * DC_BIG_INFO_STRING];

    dc_parser_cb_t cb;

    // --- output state, only used while a cut is running ---
    int cutActive;
    int cutStartMs;
    int cutEndMs;
    // Lower bound on the block sequence number at which the window may OPEN,
    // or -1 for "time alone decides". See dc_parser_set_cut.
    int cutStartSeq;
    int outWriteMessage;
    int outWriteFirstMessage;
    int outServerCommandSequence;
    int outSnapshotsWritten;
    int outSnapshotCount;  // snapshots actually emitted, for the caller's stats
    int outMessageCount;   // messages actually emitted, gamestate included
    FILE *outFile;
    char outPath[1024];
    dc_msg_t outMsg;
    dc_byte outMsgData[DC_MAX_MSGLEN];

    char err[512];
    int errSet;
};

dc_parser_t *dc_parser_create(void);
void dc_parser_destroy(dc_parser_t *p);

// Arms the single cut this parser can perform. Must be called before the first
// message. out_path is created lazily, when the first in-window message is
// reached, exactly like UDT (so a cut that selects nothing leaves no file).
//
// start_seq bounds WHERE in the file the window may open: no message whose own
// block sequence number is below it can start the cut, however well its server
// time fits [start_ms, end_ms]. Pass -1 for the old time-only behaviour. See
// democut.h's demo_cut() for why a caller that knows the exact instant it wants
// must pass it.
void dc_parser_set_cut(dc_parser_t *p, int start_ms, int end_ms, int start_seq, const char *out_path);

// Feeds one length-prefixed block's payload. seq is the block's own sequence
// number from the .dm_91 framing.
// Returns 1 to keep going, 0 when parsing should stop (the cut finished), -1 on
// error (p->err holds why).
int dc_parser_parse_message(dc_parser_t *p, const dc_byte *data, int len, int seq);

// Closes an in-progress cut properly (trailer + close). Safe to call always.
void dc_parser_finish(dc_parser_t *p);

#endif /* DC_PARSER_H */
