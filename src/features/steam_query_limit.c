/*
Copyright (C) 2026 mgaertne

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

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "engine/quake_common.h"
#include "features/steam_query_limit.h"

// Defaults of the cvars. They follow the limits ioquake3 applies to its status requests.
#define DEFAULT_RATE 1.0         // Queries per second and source.
#define DEFAULT_BURST 10.0       // Queries a source can send at once.
#define DEFAULT_GLOBAL_RATE 10.0 // Queries per second from all sources together.

#define CVAR_RATE "qlx_steamQueryRate"
#define CVAR_BURST "qlx_steamQueryBurst"
#define CVAR_GLOBAL_RATE "qlx_steamQueryGlobalRate"

#define LIMITS_REFRESH_INTERVAL_MS 5000
#define SUMMARY_INTERVAL_MS 60000

#define TABLE_BITS 11
#define TABLE_SIZE (1 << TABLE_BITS)

// Every connectionless packet starts with four 0xFF bytes. The Steam queries go on with their opcode.
static const unsigned char QUERY_HEADER[4] = {0xFF, 0xFF, 0xFF, 0xFF};
static const char QUERY_OPCODES[]          = "TUV"; // A2S_INFO, A2S_PLAYER, A2S_RULES

typedef struct {
    double rate; // A rate of 0 disables the limit.
    double burst;
    double global_rate;
} query_limits_t;

typedef struct {
    double tokens;
    uint64_t last_refill_ms;
} bucket_t;

typedef struct {
    unsigned char source[4];
    int in_use;
    bucket_t bucket;
} slot_t;

static query_limits_t limits = {DEFAULT_RATE, DEFAULT_BURST, DEFAULT_GLOBAL_RATE};
static uint64_t limits_refreshed_ms;
static int limits_loaded;

static slot_t slots[TABLE_SIZE];
static bucket_t global_bucket = {DEFAULT_GLOBAL_RATE, 0};

static uint64_t dropped;
static uint64_t last_summary_ms;

static uint64_t NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// The global bucket holds at least one token, so rates below 1 per second still let a query through.
static double GlobalCapacity(void) {
    return limits.global_rate > 1.0 ? limits.global_rate : 1.0;
}

static double RefilledTokens(const bucket_t* bucket, double rate, double capacity, uint64_t now_ms) {
    double elapsed_secs = now_ms > bucket->last_refill_ms ? (double)(now_ms - bucket->last_refill_ms) / 1000.0 : 0.0;
    double tokens       = bucket->tokens + elapsed_secs * rate;
    return tokens < capacity ? tokens : capacity;
}

static int TryTakeToken(bucket_t* bucket, double rate, double capacity, uint64_t now_ms) {
    bucket->tokens         = RefilledTokens(bucket, rate, capacity, now_ms);
    bucket->last_refill_ms = now_ms;
    if (bucket->tokens < 1.0) {
        return 0;
    }

    bucket->tokens -= 1.0;
    return 1;
}

static unsigned SlotIndex(const unsigned char* source) {
    uint32_t address = ((uint32_t)source[0] << 24) | ((uint32_t)source[1] << 16) | ((uint32_t)source[2] << 8) | source[3];
    return (uint32_t)(address * 0x9E3779B1u) >> (32 - TABLE_BITS);
}

static int SourceAllows(const unsigned char* source, uint64_t now_ms) {
    if (limits.rate <= 0.0) {
        return 1;
    }

    slot_t* slot = &slots[SlotIndex(source)];
    // A source that meets a slot another source still uses shares its bucket, so colliding sources
    // are limited together and can never reset each other's limit. A slot whose bucket is full has
    // been idle and is taken over.
    int takes_over = !slot->in_use || (memcmp(slot->source, source, 4) != 0 && RefilledTokens(&slot->bucket, limits.rate, limits.burst, now_ms) >= limits.burst);
    if (takes_over) {
        memcpy(slot->source, source, 4);
        slot->in_use                = 1;
        slot->bucket.tokens         = limits.burst;
        slot->bucket.last_refill_ms = now_ms;
    }

    return TryTakeToken(&slot->bucket, limits.rate, limits.burst, now_ms);
}

static int GlobalAllows(uint64_t now_ms) {
    if (limits.global_rate <= 0.0) {
        return 1;
    }

    return TryTakeToken(&global_bucket, limits.global_rate, GlobalCapacity(), now_ms);
}

// Reads a cvar as a non-negative number. A missing cvar or anything else gives the default.
static double CvarValue(const char* name, double default_value) {
    cvar_t* cvar = Cvar_FindVar(name);
    if (cvar == NULL || cvar->string == NULL) {
        return default_value;
    }

    char* end;
    double value = strtod(cvar->string, &end);
    if (end == cvar->string || *end != '\0' || !isfinite(value) || value < 0.0) {
        return default_value;
    }

    return value;
}

static void RefreshLimits(uint64_t now_ms) {
    if (limits_loaded && now_ms - limits_refreshed_ms < LIMITS_REFRESH_INTERVAL_MS) {
        return;
    }

    limits.rate         = CvarValue(CVAR_RATE, DEFAULT_RATE);
    limits.burst        = CvarValue(CVAR_BURST, DEFAULT_BURST);
    limits.global_rate  = CvarValue(CVAR_GLOBAL_RATE, DEFAULT_GLOBAL_RATE);
    limits_refreshed_ms = now_ms;
    limits_loaded       = 1;
}

// One line a minute at most, so a flood does not flood the log as well.
static void ReportDroppedPackets(uint64_t now_ms) {
    if (now_ms - last_summary_ms < SUMMARY_INTERVAL_MS) {
        return;
    }

    last_summary_ms = now_ms;
    if (dropped > 0) {
        DebugPrint("Dropped %llu Steam query packets that exceeded the rate limit.\n", (unsigned long long)dropped);
        dropped = 0;
    }
}

static int IsSteamQuery(const msg_t* msg) {
    if (msg == NULL || msg->data == NULL) {
        return 0;
    }

    // Do not trust a message that claims to be larger than its buffer.
    if (msg->cursize <= (int)sizeof(QUERY_HEADER) || msg->cursize > msg->maxsize) {
        return 0;
    }

    return memcmp(msg->data, QUERY_HEADER, sizeof(QUERY_HEADER)) == 0 && strchr(QUERY_OPCODES, msg->data[sizeof(QUERY_HEADER)]) != NULL;
}

void SteamQueryLimit_Init(void) {
    char value[32];

    snprintf(value, sizeof(value), "%g", DEFAULT_RATE);
    Cvar_Get(CVAR_RATE, value, 0);
    snprintf(value, sizeof(value), "%g", DEFAULT_BURST);
    Cvar_Get(CVAR_BURST, value, 0);
    snprintf(value, sizeof(value), "%g", DEFAULT_GLOBAL_RATE);
    Cvar_Get(CVAR_GLOBAL_RATE, value, 0);
}

int SteamQueryLimit_Allow(const netadr_t* from, const msg_t* msg) {
    // Only IPv4 sources are limited. Other address types, like the loopback, are not forged.
    if (from == NULL || from->type != NA_IP || !IsSteamQuery(msg)) {
        return 1;
    }

    uint64_t now_ms = NowMs();
    RefreshLimits(now_ms);

    // The source limit comes first, so a source that is over its limit cannot drain the global one.
    int allowed = SourceAllows(from->ip, now_ms) && GlobalAllows(now_ms);
    if (!allowed) {
        dropped++;
    }
    ReportDroppedPackets(now_ms);

    return allowed;
}
