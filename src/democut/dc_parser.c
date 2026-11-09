// Message-level parser + re-encoder for .dm_91. See dc_parser.h.
//
// This file is the one place where "what the cut keeps" is decided, and every
// ordering decision in it is a deliberate copy of what uberdemotools did before
// it, which is in turn id Tech 3's CL_Parse* with an output side attached. The
// three that actually matter, and which a well-meaning cleanup would break:
//
//  1. The window test runs at the END of a message, against _inServerTime -
//     i.e. against the time of the snapshot this message just carried (or the
//     last one, for a message that carries none). So the first message the cut
//     writes is decided after its content has already been parsed, which is
//     exactly why that first message's content is DROPPED and replaced by a
//     synthesized gamestate: a .dm_91 must open on a gamestate, and the state
//     to synthesize it from is only complete once that message has been read.
//
//  2. Output messages keep their ORIGINAL sequence numbers. The snapshot delta
//     chain is expressed as "this many messages back", so renumbering would
//     silently repoint every delta. The gamestate therefore occupies the
//     sequence number of the message it replaced.
//
//  3. Server command sequence numbers are renumbered from 0 (the synthesized
//     gamestate takes 0). The client rejects a gap ("Server command overflow"),
//     and the original numbers are mid-stream values with no relation to a
//     freshly opened segment.
//
// GPLv2, same as minqlxtended and the id Tech 3 code this derives from.

#include "dc_parser.h"

#include <stdlib.h>
#include <string.h>

#define DC_TOKEN_MAX   3
#define DC_TOKEN_CHARS DC_BIG_INFO_STRING

static void dc_set_err(dc_parser_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>

static void dc_set_err(dc_parser_t *p, const char *fmt, ...) {
    if (p->errSet) {
        return; // keep the first, most specific error
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof(p->err), fmt, ap);
    va_end(ap);
    p->errSet = 1;
}

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static dc_entityState_t *dc_entity(dc_parser_t *p, int index) {
    return &p->parseEntities[index & (DC_MAX_PARSE_ENTITIES - 1)];
}

static dc_snapshot_t *dc_client_snapshot(dc_parser_t *p, int index) {
    return &p->snapshots[index & DC_PACKET_MASK];
}

// Quake 3 command tokenizer: whitespace separated, a double quote opens a token
// that runs to the next double quote (quotes stripped). Enough for the only
// commands this file has to understand - "cs" and the "bcs0/1/2" chunks that
// carry an oversized one.
static int dc_tokenize(const char *text, char tokens[DC_TOKEN_MAX][DC_TOKEN_CHARS]) {
    int count = 0;
    const char *s = text;

    while (count < DC_TOKEN_MAX) {
        while (*s && (unsigned char)*s <= ' ') {
            s++;
        }
        if (!*s) {
            break;
        }
        int len = 0;
        if (*s == '"') {
            s++;
            while (*s && *s != '"') {
                if (len < DC_TOKEN_CHARS - 1) {
                    tokens[count][len++] = *s;
                }
                s++;
            }
            if (*s == '"') {
                s++;
            }
        } else {
            while (*s && (unsigned char)*s > ' ') {
                if (len < DC_TOKEN_CHARS - 1) {
                    tokens[count][len++] = *s;
                }
                s++;
            }
        }
        tokens[count][len] = '\0';
        count++;
    }
    return count;
}

static int dc_parse_int(const char *s, int *out) {
    char *end = NULL;
    long v;
    if (!s || !*s) {
        return 0;
    }
    v = strtol(s, &end, 10);
    if (end == s || (end && *end != '\0')) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static void dc_free_config_strings(dc_parser_t *p) {
    for (int i = 0; i < DC_MAX_CONFIGSTRINGS; i++) {
        free(p->configStrings[i]);
        p->configStrings[i] = NULL;
    }
}

static int dc_store_config_string(dc_parser_t *p, int index, const char *value) {
    if (index < 0 || index >= DC_MAX_CONFIGSTRINGS) {
        return 0;
    }
    char *copy = strdup(value ? value : "");
    if (!copy) {
        return 0;
    }
    free(p->configStrings[index]);
    p->configStrings[index] = copy;
    return 1;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

dc_parser_t *dc_parser_create(void) {
    dc_parser_t *p = (dc_parser_t *)calloc(1, sizeof(dc_parser_t));
    if (!p) {
        return NULL;
    }
    p->inServerMessageSequence       = -1;
    p->inServerCommandSequence       = -1;
    p->inReliableSequenceAcknowledge = -1;
    p->inClientNum                   = -1;
    p->inChecksumFeed                = -1;
    p->inServerTime                  = INT32_MIN;
    p->inLastSnapshotMessageNumber   = INT32_MIN;
    p->inGameStateIndex              = -1;
    p->cutStartSeq                   = -1;
    return p;
}

void dc_parser_destroy(dc_parser_t *p) {
    if (!p) {
        return;
    }
    if (p->outFile) {
        fclose(p->outFile);
        p->outFile = NULL;
    }
    dc_free_config_strings(p);
    free(p);
}

void dc_parser_set_cut(dc_parser_t *p, int start_ms, int end_ms, int start_seq, const char *out_path) {
    p->cutActive   = 1;
    p->cutStartMs  = start_ms;
    p->cutEndMs    = end_ms;
    p->cutStartSeq = start_seq;
    snprintf(p->outPath, sizeof(p->outPath), "%s", out_path);
}

// Resets everything a fresh gamestate invalidates. Mirrors UDT's
// ResetForGamestateMessage with one deliberate omission: the CURRENT message's
// own sequence number is left alone (UDT sets it to -1 here, mid-message, which
// would stamp a nonsense header on a cut that opened on this very message).
static void dc_reset_for_gamestate(dc_parser_t *p) {
    p->inServerCommandSequence       = -1;
    p->inReliableSequenceAcknowledge = -1;
    p->inClientNum                   = -1;
    p->inChecksumFeed                = -1;
    p->inParseEntitiesNum            = 0;
    p->inServerTime                  = INT32_MIN;
    p->inLastSnapshotMessageNumber   = INT32_MIN;

    p->outServerCommandSequence = 0;
    p->outSnapshotsWritten      = 0;

    memset(p->baselines, 0, sizeof(p->baselines));
    memset(p->snapshots, 0, sizeof(p->snapshots));
    dc_free_config_strings(p);
    p->bigConfigString[0] = '\0';
}

static int dc_should_write(const dc_parser_t *p) {
    return p->cutActive && p->outWriteMessage;
}

// ---------------------------------------------------------------------------
// output
// ---------------------------------------------------------------------------

static void dc_write_gamestate(dc_parser_t *p) {
    static const dc_entityState_t nullState;

    dc_msg_init(&p->outMsg, p->outMsgData, (int)sizeof(p->outMsgData));

    dc_msg_write_long(&p->outMsg, p->inReliableSequenceAcknowledge);

    dc_msg_write_byte(&p->outMsg, DC_SVC_GAMESTATE);
    dc_msg_write_long(&p->outMsg, p->outServerCommandSequence);
    p->outServerCommandSequence++;

    for (int i = 0; i < DC_MAX_CONFIGSTRINGS; i++) {
        const char *cs = p->configStrings[i];
        if (cs == NULL || cs[0] == '\0') {
            continue;
        }
        dc_msg_write_byte(&p->outMsg, DC_SVC_CONFIGSTRING);
        dc_msg_write_short(&p->outMsg, i);
        dc_msg_write_big_string(&p->outMsg, cs);
    }

    for (int i = 0; i < DC_MAX_GENTITIES; i++) {
        const dc_entityState_t *newState = &p->baselines[i];
        // Same "is this slot used at all" test the engine and UDT use: a
        // baseline that is bit-for-bit the null state carries no information.
        if (memcmp(&nullState, newState, sizeof(nullState)) == 0) {
            continue;
        }
        dc_msg_write_byte(&p->outMsg, DC_SVC_BASELINE);
        dc_msg_write_delta_entity(&p->outMsg, &nullState, newState, 1);
    }

    dc_msg_write_byte(&p->outMsg, DC_SVC_EOF);

    dc_msg_write_long(&p->outMsg, p->inClientNum);
    dc_msg_write_long(&p->outMsg, p->inChecksumFeed);

    dc_msg_write_byte(&p->outMsg, DC_SVC_EOF);
}

static int dc_flush_message(dc_parser_t *p) {
    const int32_t seq    = (int32_t)p->inServerMessageSequence;
    const int32_t length = (int32_t)p->outMsg.cursize;

    if (p->outMsg.overflowed) {
        dc_set_err(p, "output message overflowed (%d bytes) at sequence %d", length, seq);
        return 0;
    }
    if (length <= 0 || length > DC_MAX_MSGLEN) {
        dc_set_err(p, "output message length %d out of range at sequence %d", length, seq);
        return 0;
    }
    if (fwrite(&seq, 4, 1, p->outFile) != 1 || fwrite(&length, 4, 1, p->outFile) != 1 ||
        fwrite(p->outMsg.data, (size_t)length, 1, p->outFile) != 1) {
        dc_set_err(p, "write error on %s", p->outPath);
        return 0;
    }
    p->outMessageCount++;
    return 1;
}

static void dc_write_trailer_and_close(dc_parser_t *p) {
    if (!p->outFile) {
        return;
    }
    const int32_t marker = -1;
    fwrite(&marker, 4, 1, p->outFile);
    fwrite(&marker, 4, 1, p->outFile);
    if (fclose(p->outFile) != 0) {
        dc_set_err(p, "close error on %s", p->outPath);
    }
    p->outFile = NULL;
}

// UDT's WriteBigConfigStringCommand: a config string too long for one server
// command goes out as a bcs0/bcs1*/bcs2 chain the client reassembles.
static void dc_write_big_config_string_command(dc_parser_t *p, const char *csIndex, const char *csData) {
    const unsigned maxLengthPerCmd = DC_MAX_STRING_CHARS - 2;
    const unsigned perCmdOverhead  = 8 + (unsigned)strlen(csIndex);
    const unsigned dataLength      = (unsigned)strlen(csData);
    unsigned maxDataLength;
    unsigned outputChunks = 0;

    if (perCmdOverhead >= maxLengthPerCmd) {
        return; // absurd index length; nothing sane to emit
    }
    maxDataLength = maxLengthPerCmd - perCmdOverhead;

    for (unsigned i = 2;; ++i) {
        const unsigned perCmdData = (dataLength + i - 1) / i;
        if (perCmdData + perCmdOverhead <= maxLengthPerCmd) {
            outputChunks = i;
            break;
        }
        if (i > DC_BIG_INFO_STRING) {
            return; // cannot happen for a real config string; do not spin
        }
    }

    unsigned dataOffset = 0;
    for (unsigned i = 0; i < outputChunks; ++i) {
        const char *bcsIdx = (i == 0) ? "0" : ((i == outputChunks - 1) ? "2" : "1");
        char chunk[DC_MAX_STRING_CHARS];
        // Sized past MAX_STRING_CHARS so the compose below can never truncate:
        // the chunk size is picked so the result fits, and the check after it
        // turns a violated assumption into a dropped command rather than a
        // half-written one on the wire.
        char command[DC_MAX_STRING_CHARS + 64];
        unsigned take = (bcsIdx[0] == '2') ? (dataLength > dataOffset ? dataLength - dataOffset : 0) : maxDataLength;

        if (dataOffset >= dataLength) {
            take = 0;
        } else if (dataOffset + take > dataLength) {
            take = dataLength - dataOffset;
        }
        if (take >= sizeof(chunk)) {
            take = sizeof(chunk) - 1;
        }
        memcpy(chunk, csData + dataOffset, take);
        chunk[take] = '\0';

        snprintf(command, sizeof(command), "bcs%s %s \"%s\"", bcsIdx, csIndex, chunk);
        if (strlen(command) >= DC_MAX_STRING_CHARS) {
            continue; // cannot happen with the chunking above; never emit it if it does
        }

        dc_msg_write_byte(&p->outMsg, DC_SVC_SERVERCOMMAND);
        dc_msg_write_long(&p->outMsg, p->outServerCommandSequence);
        dc_msg_write_string(&p->outMsg, command);
        p->outServerCommandSequence++;

        dataOffset += maxDataLength;
    }
}

// ---------------------------------------------------------------------------
// parsing
// ---------------------------------------------------------------------------

static int dc_parse_gamestate(dc_parser_t *p, dc_msg_t *msg) {
    static const dc_entityState_t nullState;

    dc_reset_for_gamestate(p);

    p->inServerCommandSequence = dc_msg_read_long(msg);

    for (;;) {
        const int command = dc_msg_read_byte(msg);
        if (command == DC_SVC_EOF || command == -1) {
            break;
        }
        if (command == DC_SVC_CONFIGSTRING) {
            const int index = dc_msg_read_short(msg);
            if (index < 0 || index >= DC_MAX_CONFIGSTRINGS) {
                dc_set_err(p, "gamestate config string index out of range: %d", index);
                return 0;
            }
            const char *value = dc_msg_read_big_string(msg);
            if (!dc_store_config_string(p, index, value)) {
                dc_set_err(p, "out of memory storing config string %d", index);
                return 0;
            }
        } else if (command == DC_SVC_BASELINE) {
            const int newIndex = dc_msg_read_bits(msg, DC_GENTITYNUM_BITS);
            if (newIndex < 0 || newIndex >= DC_MAX_GENTITIES) {
                dc_set_err(p, "gamestate baseline number out of range: %d", newIndex);
                return 0;
            }
            dc_msg_read_delta_entity(msg, &nullState, &p->baselines[newIndex], newIndex);
        } else {
            dc_set_err(p, "unrecognized gamestate command byte: %d", command);
            return 0;
        }
        if (msg->readOverrun) {
            dc_set_err(p, "truncated gamestate message");
            return 0;
        }
    }

    p->inClientNum    = dc_msg_read_long(msg);
    p->inChecksumFeed = dc_msg_read_long(msg);

    p->inGameStateIndex++;

    if (p->cb.on_gamestate) {
        p->cb.on_gamestate(p, p->cb.user);
    }
    return 1;
}

static int dc_parse_command_string(dc_parser_t *p, dc_msg_t *msg) {
    static char tokens[DC_TOKEN_MAX][DC_TOKEN_CHARS];
    // Same capacity as p->bigConfigString: the bcs2 branch copies the whole
    // reassembled big config string back in here, and gcc's format-truncation
    // check (debug builds, -O0) measures against the source buffer's size.
    char commandString[3 * DC_BIG_INFO_STRING];
    int csIndex        = -1;
    int isConfigString = 0;
    const char *csValue = NULL;

    const int commandSequence = dc_msg_read_long(msg);
    const char *raw           = dc_msg_read_string(msg);

    // Already seen (the server resends unacknowledged commands): the engine
    // ignores it outright, and so must the cut, or the renumbered output would
    // contain a duplicate sequence number.
    if (p->inServerCommandSequence >= commandSequence) {
        return 1;
    }
    p->inServerCommandSequence = commandSequence;

    snprintf(commandString, sizeof(commandString), "%s", raw);

    for (int pass = 0; pass < 2; pass++) {
        const int tokenCount = dc_tokenize(commandString, tokens);
        const char *name     = (tokenCount > 0) ? tokens[0] : "";

        if (tokenCount == 3 && strcmp(name, "cs") == 0) {
            int index = -1;
            if (dc_parse_int(tokens[1], &index) && index >= 0 && index < DC_MAX_CONFIGSTRINGS) {
                isConfigString = 1;
                csIndex        = index;
                csValue        = tokens[2];
                if (!dc_store_config_string(p, index, tokens[2])) {
                    dc_set_err(p, "out of memory storing config string %d", index);
                    return 0;
                }
            }
        } else if (tokenCount == 3 && strcmp(name, "bcs0") == 0) {
            snprintf(p->bigConfigString, sizeof(p->bigConfigString), "cs %s \"%s", tokens[1], tokens[2]);
        } else if (tokenCount == 3 && strcmp(name, "bcs1") == 0) {
            const size_t have = strlen(p->bigConfigString);
            snprintf(p->bigConfigString + have, sizeof(p->bigConfigString) - have, "%s", tokens[2]);
        } else if (tokenCount == 3 && strcmp(name, "bcs2") == 0) {
            const size_t have = strlen(p->bigConfigString);
            snprintf(p->bigConfigString + have, sizeof(p->bigConfigString) - have, "%s\"", tokens[2]);
            // Re-tokenise the reassembled "cs <idx> "<value>"" on the second
            // pass, exactly as UDT's `goto tokenize` does, so the config string
            // table sees the whole value and the output re-splits it itself.
            snprintf(commandString, sizeof(commandString), "%s", p->bigConfigString);
            continue;
        }
        break;
    }

    if (p->cb.on_command) {
        p->cb.on_command(p, commandSequence, commandString, isConfigString ? csIndex : -1, csValue, p->cb.user);
    }

    if (dc_should_write(p)) {
        const int length = (int)strlen(commandString);
        if (isConfigString && length >= DC_MAX_STRING_CHARS) {
            char idxText[16];
            snprintf(idxText, sizeof(idxText), "%d", csIndex);
            dc_write_big_config_string_command(p, idxText, p->configStrings[csIndex] ? p->configStrings[csIndex] : "");
        } else if (length < DC_MAX_STRING_CHARS) {
            dc_msg_write_byte(&p->outMsg, DC_SVC_SERVERCOMMAND);
            dc_msg_write_long(&p->outMsg, p->outServerCommandSequence);
            dc_msg_write_string(&p->outMsg, commandString);
            p->outServerCommandSequence++;
        } else {
            // Too long and not a config string: nothing legal to emit, so hold
            // the slot with a no-op rather than desynchronising the stream.
            dc_msg_write_byte(&p->outMsg, DC_SVC_NOP);
        }
    }
    return 1;
}

static int dc_delta_entity(dc_parser_t *p, dc_msg_t *msg, dc_snapshot_t *frame, int newnum,
                           const dc_entityState_t *old, int unchanged) {
    dc_entityState_t *state = dc_entity(p, p->inParseEntitiesNum);

    if (unchanged) {
        *state = *old;
    } else {
        dc_msg_read_delta_entity(msg, old, state, newnum);
        if (msg->readOverrun) {
            dc_set_err(p, "truncated entity delta (entity %d)", newnum);
            return 0;
        }
    }

    if (state->number == DC_MAX_GENTITIES - 1) {
        return 1; // delta-removed: not added to the frame
    }

    p->inParseEntitiesNum++;
    frame->numEntities++;
    return 1;
}

static int dc_parse_packet_entities(dc_parser_t *p, dc_msg_t *msg, const dc_snapshot_t *oldframe,
                                    dc_snapshot_t *newframe) {
    int oldnum, newnum;
    int oldindex                 = 0;
    const dc_entityState_t *oldstate = NULL;
    int guard                    = 0;

    newframe->parseEntitiesNum = p->inParseEntitiesNum;
    newframe->numEntities      = 0;

    if (!oldframe || oldindex >= oldframe->numEntities) {
        oldnum = 99999;
    } else {
        oldstate = dc_entity(p, oldframe->parseEntitiesNum + oldindex);
        oldnum   = oldstate->number;
    }

    for (;;) {
        // A truncated/garbled message makes the reader return the same value
        // forever; MAX_GENTITIES distinct entity numbers is already more than
        // any real snapshot can name, so anything past it is corruption.
        if (++guard > DC_MAX_GENTITIES + 1) {
            dc_set_err(p, "packet entities did not terminate (message %d)", p->inServerMessageSequence);
            return 0;
        }
        newnum = dc_msg_read_bits(msg, DC_GENTITYNUM_BITS);
        if (newnum == DC_MAX_GENTITIES - 1) {
            break;
        }
        if (msg->readOverrun) {
            dc_set_err(p, "truncated packet entities (message %d)", p->inServerMessageSequence);
            return 0;
        }

        while (oldnum < newnum) {
            if (!dc_delta_entity(p, msg, newframe, oldnum, oldstate, 1)) {
                return 0;
            }
            oldindex++;
            if (!oldframe || oldindex >= oldframe->numEntities) {
                oldnum = 99999;
            } else {
                oldstate = dc_entity(p, oldframe->parseEntitiesNum + oldindex);
                oldnum   = oldstate->number;
            }
        }

        if (oldnum == newnum) {
            if (!dc_delta_entity(p, msg, newframe, newnum, oldstate, 0)) {
                return 0;
            }
            oldindex++;
            if (!oldframe || oldindex >= oldframe->numEntities) {
                oldnum = 99999;
            } else {
                oldstate = dc_entity(p, oldframe->parseEntitiesNum + oldindex);
                oldnum   = oldstate->number;
            }
            continue;
        }

        if (oldnum > newnum) {
            if (!dc_delta_entity(p, msg, newframe, newnum, &p->baselines[newnum], 0)) {
                return 0;
            }
            continue;
        }
    }

    while (oldnum != 99999) {
        if (!dc_delta_entity(p, msg, newframe, oldnum, oldstate, 1)) {
            return 0;
        }
        oldindex++;
        if (!oldframe || oldindex >= oldframe->numEntities) {
            oldnum = 99999;
        } else {
            oldstate = dc_entity(p, oldframe->parseEntitiesNum + oldindex);
            oldnum   = oldstate->number;
        }
    }

    return 1;
}

static void dc_emit_packet_entities(dc_parser_t *p, const dc_snapshot_t *from, const dc_snapshot_t *to) {
    static const dc_entityState_t nullState;
    int oldindex = 0, newindex = 0;
    int oldnum, newnum;
    const dc_entityState_t *oldent = NULL;
    const dc_entityState_t *newent = NULL;
    const int fromNumEntities      = from ? from->numEntities : 0;

    while (newindex < to->numEntities || oldindex < fromNumEntities) {
        if (newindex >= to->numEntities) {
            newnum = 9999;
        } else {
            newent = dc_entity(p, to->parseEntitiesNum + newindex);
            newnum = newent->number;
        }

        if (oldindex >= fromNumEntities) {
            oldnum = 9999;
        } else {
            oldent = dc_entity(p, from->parseEntitiesNum + oldindex);
            oldnum = oldent->number;
        }

        if (newnum == oldnum) {
            dc_msg_write_delta_entity(&p->outMsg, oldent, newent, 0);
            oldindex++;
            newindex++;
            continue;
        }
        if (newnum < oldnum) {
            const dc_entityState_t *baseline = (newnum >= 0 && newnum < DC_MAX_GENTITIES)
                                                   ? &p->baselines[newnum]
                                                   : &nullState;
            dc_msg_write_delta_entity(&p->outMsg, baseline, newent, 1);
            newindex++;
            continue;
        }
        // newnum > oldnum: the old entity is gone from this frame.
        dc_msg_write_delta_entity(&p->outMsg, oldent, NULL, 1);
        oldindex++;
    }

    dc_msg_write_bits(&p->outMsg, DC_MAX_GENTITIES - 1, DC_GENTITYNUM_BITS);
}

static int dc_parse_snapshot(dc_parser_t *p, dc_msg_t *msg) {
    dc_snapshot_t newSnap;
    const dc_snapshot_t *oldSnap = NULL;
    int deltaNum;

    memset(&newSnap, 0, sizeof(newSnap));

    p->inServerTime = dc_msg_read_long(msg);

    newSnap.serverCommandNum = p->inServerCommandSequence;
    newSnap.serverTime       = p->inServerTime;
    newSnap.messageNum       = p->inServerMessageSequence;

    deltaNum              = dc_msg_read_byte(msg);
    newSnap.deltaWireNum  = deltaNum;
    newSnap.deltaNum      = deltaNum ? (newSnap.messageNum - deltaNum) : -1;
    newSnap.snapFlags     = dc_msg_read_byte(msg);

    if (newSnap.deltaNum <= 0) {
        newSnap.valid = 1; // uncompressed frame
        oldSnap       = NULL;
    } else {
        const dc_snapshot_t *candidate = dc_client_snapshot(p, newSnap.deltaNum);
        oldSnap                        = candidate;
        if (candidate->valid && candidate->messageNum == newSnap.deltaNum &&
            p->inParseEntitiesNum - candidate->parseEntitiesNum <= DC_MAX_PARSE_ENTITIES - 128) {
            newSnap.valid = 1;
        }
        // else: left invalid, exactly like the engine. The message is still
        // fully read below so the bitstream stays in sync; it just does not
        // become a usable frame.
    }

    {
        const int areaMaskLength = dc_msg_read_byte(msg);
        if (areaMaskLength < 0 || areaMaskLength > (int)sizeof(newSnap.areamask)) {
            dc_set_err(p, "invalid areamask size %d (message %d)", areaMaskLength, newSnap.messageNum);
            return 0;
        }
        newSnap.areamaskLen = areaMaskLength;
        dc_msg_read_data(msg, newSnap.areamask, areaMaskLength);
    }

    dc_msg_read_delta_playerstate(msg, oldSnap ? &oldSnap->ps : NULL, &newSnap.ps);
    if (msg->readOverrun) {
        dc_set_err(p, "truncated playerstate (message %d)", newSnap.messageNum);
        return 0;
    }

    // oldSnap is passed through even when it failed the validity test above:
    // the engine does the same, and the delta bitstream is self-describing, so
    // this only decides which entities get carried over - not how many bits are
    // consumed. Substituting NULL here would silently change the contents of
    // frames the engine would have built differently.
    if (!dc_parse_packet_entities(p, msg, oldSnap, &newSnap)) {
        return 0;
    }

    // Did we write enough snapshots already? A delta reference the output does
    // not contain has to be turned into a full snapshot, or the client drops
    // the frame ("Delta frame too old") and every entity in it.
    if (p->outSnapshotsWritten < deltaNum) {
        deltaNum = 0;
        oldSnap  = NULL;
    }

    if (!newSnap.valid) {
        return 1;
    }

    *dc_client_snapshot(p, newSnap.messageNum) = newSnap;

    if (newSnap.messageNum == p->inLastSnapshotMessageNumber) {
        return 1; // don't report or write the same snapshot twice
    }
    p->inLastSnapshotMessageNumber = newSnap.messageNum;

    if (p->cb.on_snapshot) {
        p->cb.on_snapshot(p, dc_client_snapshot(p, newSnap.messageNum), p->cb.user);
    }

    if (dc_should_write(p)) {
        const dc_snapshot_t *stored = dc_client_snapshot(p, newSnap.messageNum);
        dc_msg_write_byte(&p->outMsg, DC_SVC_SNAPSHOT);
        dc_msg_write_long(&p->outMsg, stored->serverTime);
        dc_msg_write_byte(&p->outMsg, deltaNum);
        dc_msg_write_byte(&p->outMsg, stored->snapFlags);
        dc_msg_write_byte(&p->outMsg, stored->areamaskLen);
        dc_msg_write_data(&p->outMsg, stored->areamask, stored->areamaskLen);
        dc_msg_write_delta_playerstate(&p->outMsg, oldSnap ? &oldSnap->ps : NULL, &stored->ps);
        dc_emit_packet_entities(p, deltaNum ? oldSnap : NULL, stored);
        p->outSnapshotsWritten++;
        p->outSnapshotCount++;
    }

    return 1;
}

int dc_parser_parse_message(dc_parser_t *p, const dc_byte *data, int len, int seq) {
    dc_msg_t msg;
    int cutEndedHere = 0;

    p->inServerMessageSequence = seq;

    dc_msg_init(&msg, (dc_byte *)data, len);
    msg.cursize = len;
    dc_msg_begin_reading(&msg);

    dc_msg_init(&p->outMsg, p->outMsgData, (int)sizeof(p->outMsgData));

    p->inReliableSequenceAcknowledge = dc_msg_read_long(&msg);
    if (dc_should_write(p)) {
        dc_msg_write_long(&p->outMsg, p->inReliableSequenceAcknowledge);
    }

    for (;;) {
        if (msg.readcount > msg.cursize) {
            dc_set_err(p, "read past the end of message %d", seq);
            return -1;
        }
        if (msg.readcount == msg.cursize) {
            break;
        }

        const int command = dc_msg_read_byte(&msg);
        if (command == DC_SVC_EOF || command == -1) {
            break;
        }

        switch (command) {
        case DC_SVC_NOP:
            if (dc_should_write(p)) {
                dc_msg_write_byte(&p->outMsg, DC_SVC_NOP);
            }
            break;
        case DC_SVC_SERVERCOMMAND:
            if (!dc_parse_command_string(p, &msg)) {
                return -1;
            }
            break;
        case DC_SVC_GAMESTATE:
            if (p->outWriteMessage) {
                // A second gamestate ends the segment the cut is producing: the
                // clock and the entity/config state both restart, so nothing
                // after it belongs to the same file. UDT stopped writing here
                // WITHOUT a trailer (ResetForGamestateMessage clears
                // _outWriteMessage before the end-of-message check can see it),
                // which leaves a demo the engine plays but never sees end.
                // Finish it properly instead; the production path refuses to
                // cut a multi-gamestate file at all, so this can only ever be a
                // hand-recovered one.
                dc_write_trailer_and_close(p);
                p->outWriteMessage      = 0;
                p->outWriteFirstMessage = 0;
                p->cutActive            = 0;
                cutEndedHere            = 1;
            }
            if (!dc_parse_gamestate(p, &msg)) {
                return -1;
            }
            break;
        case DC_SVC_SNAPSHOT:
            if (!dc_parse_snapshot(p, &msg)) {
                return -1;
            }
            break;
        default:
            dc_set_err(p, "unrecognized server message command byte %d in message %d", command, seq);
            return -1;
        }

        if (msg.readOverrun) {
            dc_set_err(p, "truncated message %d", seq);
            return -1;
        }
    }

    if (dc_should_write(p)) {
        dc_msg_write_byte(&p->outMsg, DC_SVC_EOF);
    }

    // ---- the window test, at the end of the message and against this
    // ---- message's own server time. See the file header, point 1.
    if (p->cutActive) {
        const int gameTime = p->inServerTime;

        // The sequence bound is checked ONLY here, on the opening edge, and
        // deliberately not on the closing one: a capture can hold a stale,
        // already-consumed epoch whose server times overlap the window that is
        // wanted (a map_restart resets the server clock without sending a fresh
        // gamestate, so both epochs sit in one gamestate-0 file). Time alone
        // cannot tell those two epochs apart, and the earlier one comes first,
        // so a time-only test opens the cut in the WRONG epoch and the output
        // carries a backward clock jump. The block sequence number is
        // monotonic over the whole file and is exactly what the caller sampled
        // at the instant it cares about, so it is the one bound that can.
        if (p->inGameStateIndex == 0 && !p->outWriteMessage && gameTime >= p->cutStartMs &&
            gameTime <= p->cutEndMs && (p->cutStartSeq < 0 || seq >= p->cutStartSeq)) {
            p->outWriteMessage      = 1;
            p->outWriteFirstMessage = 1;
        } else if ((p->inGameStateIndex == 0 && p->outWriteMessage && gameTime > p->cutEndMs) ||
                   (p->inGameStateIndex > 0 && p->outWriteMessage)) {
            dc_write_trailer_and_close(p);
            p->outWriteMessage      = 0;
            p->outWriteFirstMessage = 0;
            p->cutActive            = 0;
            return p->errSet ? -1 : 0; // the only cut is done: stop parsing
        }
    }

    if (p->outWriteFirstMessage) {
        p->outFile = fopen(p->outPath, "wb");
        if (!p->outFile) {
            dc_set_err(p, "cannot create %s", p->outPath);
            return -1;
        }
        dc_write_gamestate(p);
        if (!dc_flush_message(p)) {
            return -1;
        }
        p->outWriteFirstMessage = 0;
    } else if (p->outWriteMessage) {
        if (!dc_flush_message(p)) {
            return -1;
        }
    }

    return cutEndedHere ? 0 : 1;
}

void dc_parser_finish(dc_parser_t *p) {
    if (p->cutActive && p->outWriteMessage) {
        dc_write_trailer_and_close(p);
        p->outWriteMessage      = 0;
        p->outWriteFirstMessage = 0;
    }
    if (p->outFile) {
        fclose(p->outFile);
        p->outFile = NULL;
    }
}
