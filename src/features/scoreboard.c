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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "scoreboard.h"

/* See scoreboard.h for what this works around. */

#define SCOREBOARD_MAX_CLIENTS 64 // level->sortedClients is a fixed 64 entries

// SV_SendServerCommand throws away anything this long or longer, without printing anything.
#define SB_ENGINE_LIMIT 1023

// ps.persistant[8..13]: gametype-specific scoreboard columns (captures, defends, ...).
// Taken as a range since the meaning varies but all of them are blankable.
#define SB_PERS_FIRST 8
#define SB_PERS_COUNT 6

// A row we can't read, so we guess how wide it is.
#define SB_OPAQUE_COST 64

// A bit of room for a header that is still getting wider.
#define SB_HEADER_MARGIN 16

enum {
    SB_BUILDER_TDM = 0, // TeamDeathmatchScoreboardMessage, "scores_tdm"
    SB_BUILDER_CA,      // ClanArenaScoreboardMessage,      "scores_ca"
    SB_BUILDER_CTF,     // CaptureTheFlagScoreboardMessage, "scores_ctf"
    SB_BUILDER_FT,      // FreezeTagScoreboardMessage,      "scores_ft"
    SB_BUILDER_SMALL,   // SmallScoreboardMessage,          "smscores"
    SB_BUILDER_COUNT,
    SB_BUILDER_NONE = -1
};

static const char* const sb_builder_name[SB_BUILDER_COUNT] = {"scores_tdm", "scores_ca", "scores_ctf", "scores_ft",
                                                              "smscores"};

// How many fields each builder prints before the rows. The row count is always third from last.
static const int sb_header_fields[SB_BUILDER_COUNT] = {31, 3, 37, 31, 3};

// What we assume until we have measured a real header: every field at seven digits and a sign.
static const int sb_header_seed[SB_BUILDER_COUNT] = {289, 36, 343, 288, 35};

enum { SB_TIER_FULL = 0, SB_TIER_LIGHT = 1, SB_TIER_DROP = 2 };
enum { SB_KIND_TEAM = 0, SB_KIND_SPEC, SB_KIND_OPAQUE };

typedef struct {
    int client; // index into level->clients
    int enterTime;
    int accuracy_shots;
    int numKills;
    int numDeaths;
    int numTeamKills;
    int numTeamKilled;
    int totalDamageDealt;
    int persistant[SB_PERS_COUNT];
} sb_saved_client_t;

typedef struct {
    int client;
    int cost[2]; // SB_TIER_FULL, SB_TIER_LIGHT
    short rank;  // 1-based within its team; 0 for anything not ranked
    unsigned char team;
    unsigned char kind;
    unsigned char tier;
} sb_entry_t;

static struct {
    qboolean armed;
    qboolean sorted_changed;
    int saved_sorted[SCOREBOARD_MAX_CLIENTS];
    int saved_numConnected;
    sb_saved_client_t light[SCOREBOARD_MAX_CLIENTS];
    int num_light;

    qboolean in_fallback; // this pass is the rebuild, so it can't ask for another
    qboolean retry;       // the pass that just ran sent nothing worth having
    int retry_builder;    // which builder the rebuild has to be sized for

    qboolean have_prediction; // we worked out the widths, so we can check what came back
    int builder;
    int predicted_body;
    int kept;

    int header[SB_BUILDER_COUNT]; // widest header seen this map, 0 before we have seen one
} sb;

static cvar_t* qlx_scoreboardTrim;
static cvar_t* qlx_scoreboardFullPerTeam;
static cvar_t* qlx_scoreboardLightPerTeam;
static cvar_t* qlx_scoreboardMinPerTeam;
static cvar_t* qlx_scoreboardBudget;
static cvar_t* sb_gametype;
static cvar_t* sb_startingWeapons;
static cvar_t* sb_forceSmall;

static struct {
    unsigned built;     // scoreboards the game module handed us
    unsigned trimmed;   // scoreboards we altered
    unsigned lightened; // entries whose stats we zeroed
    unsigned omitted;   // entries we dropped
    unsigned fallback;  // times the stub came back while we were trimming
    unsigned oversize;  // scoreboards too long for the engine to send
    unsigned rebuilt;   // scoreboards we threw away and built again
    unsigned drift;     // rows that came back did not match the rows we asked for
    int worst_team;     // largest single-team headcount seen
    int last_len;       // bytes in the last scoreboard command
    int last_rows;      // rows we planned for it
    int last_builder;
} sb_stats;

void Scoreboard_Init(void) {
    if (!Cvar_Get) {
        return;
    }
    qlx_scoreboardTrim         = Cvar_Get("qlx_scoreboardTrim", "1", CVAR_ARCHIVE);
    qlx_scoreboardFullPerTeam  = Cvar_Get("qlx_scoreboardFullPerTeam", "8", CVAR_ARCHIVE);
    qlx_scoreboardLightPerTeam = Cvar_Get("qlx_scoreboardLightPerTeam", "12", CVAR_ARCHIVE);
    qlx_scoreboardMinPerTeam   = Cvar_Get("qlx_scoreboardMinPerTeam", "3", CVAR_ARCHIVE);
    qlx_scoreboardBudget       = Cvar_Get("qlx_scoreboardBudget", "1022", CVAR_ARCHIVE);
}

// Ranks up to here keep every stat the builder would normally emit.
static int full_per_team(void) { return cvar_clamped(qlx_scoreboardFullPerTeam, 8, 0, SCOREBOARD_MAX_CLIENTS); }

// Ranks past full_per_team() up to here keep their row, with the stats blanked. Set below the
// full cutoff it would blank no one and drop everyone, so we raise it.
static int light_per_team(void) {
    int full  = full_per_team();
    int light = cvar_clamped(qlx_scoreboardLightPerTeam, 12, 0, SCOREBOARD_MAX_CLIENTS);
    return (light < full) ? full : light;
}

// How far down we are willing to cut a team when nothing else has worked.
static int min_per_team(void) {
    int light = light_per_team();
    int min   = cvar_clamped(qlx_scoreboardMinPerTeam, 3, 1, SCOREBOARD_MAX_CLIENTS);
    return (min > light) ? light : min;
}

// How many bytes the whole command can be. Go over what the engine allows and it is thrown away.
static int budget_bytes(void) { return cvar_clamped(qlx_scoreboardBudget, 1022, 256, SB_ENGINE_LIMIT - 1); }

static cvar_t* find_cvar(cvar_t** cache, const char* name) {
    if (!*cache && Cvar_FindVar) {
        *cache = Cvar_FindVar(name);
    }
    return *cache;
}

// Which builder SelectScoreboardMessage will use. We only handle the team gametypes. Red Rover
// (12) is left out because it prints ps.localPersistant[0] as its team column.
static int builder_for_gametype(void) {
    if (!find_cvar(&sb_gametype, "g_gametype")) {
        return SB_BUILDER_NONE;
    }
    if (find_cvar(&sb_forceSmall, "g_forceSmallScoreboardMessage") && sb_forceSmall->integer) {
        return SB_BUILDER_SMALL;
    }

    switch (sb_gametype->integer) {
    case 3: // Team Deathmatch
        return SB_BUILDER_TDM;
    case 4: // Clan Arena
        return SB_BUILDER_CA;
    case 5:  // Capture the Flag
    case 6:  // One Flag CTF
    case 8:  // Harvester
    case 10: // Domination
    case 11: // Attack & Defend
        return SB_BUILDER_CTF;
    case 9: // Freeze Tag
        return SB_BUILDER_FT;
    default:
        return SB_BUILDER_NONE;
    }
}

static qboolean enabled(void) {
    return (qlx_scoreboardTrim && qlx_scoreboardTrim->integer && level && level->clients) ? qtrue : qfalse;
}

static int header_budget(int builder) {
    int measured = sb.header[builder];
    return measured ? measured + SB_HEADER_MARGIN : sb_header_seed[builder];
}

// Width of one %i, sign included.
static int dec_len(int v) {
    unsigned u;
    int n;

    if (v < 0) {
        n = 2;
        u = (unsigned)(-(long)v);
    } else {
        n = 1;
        u = (unsigned)v;
    }
    while (u >= 10) {
        u /= 10;
        n++;
    }
    return n;
}

// Every field is printed as " %i", so one byte of separator plus the digits.
#define SB_FIELD(v) (cost += 1 + dec_len((v)))

// The same as STAT_GetBestWeapon: most damage dealt, ties go to the lower index, otherwise the
// highest weapon in g_startingWeapons.
static int sb_best_weapon(const gclient_t* client) {
    int starting = find_cvar(&sb_startingWeapons, "g_startingWeapons") ? sb_startingWeapons->integer : 0;
    int fallback = 1;
    int best     = 0;
    int most     = 0;

    for (int w = 2; w < 15; w++) {
        if (starting & (1 << (w - 1))) {
            fallback = w;
        }
    }
    for (int w = 1; w < 15; w++) {
        int damage = client->expandedStats.damageDealt[w];
        if (most < damage) {
            most = damage;
            best = w;
        }
    }
    return best ? best : fallback;
}

static int pers_at(const gclient_t* client, int slot, qboolean light) {
    if (light && slot >= SB_PERS_FIRST && slot < SB_PERS_FIRST + SB_PERS_COUNT) {
        return 0;
    }
    return client->ps.persistant[slot];
}

// Exactly how wide the row the builder prints will be. If this ever gets out of step with
// qagame, the row-count mismatch counter picks it up.
static int entry_cost(int client_num, int builder, qboolean light) {
    const gclient_t* c = &level->clients[client_num];
    int cost           = 0;

    int ping    = (c->pers.connected == CON_CONNECTING) ? -1 : (c->ps.ping < 1000 ? c->ps.ping : 999);
    int minutes = light ? 0 : (level->time - c->pers.enterTime) / 60000;
    int shots   = light ? 0 : c->accuracy_shots;
    int alive   = (c->ps.pm_type == 0);
    int kills   = light ? 0 : c->expandedStats.numKills;
    int deaths  = light ? 0 : c->expandedStats.numDeaths;
    int tkills  = light ? 0 : c->expandedStats.numTeamKills;
    int tkilled = light ? 0 : c->expandedStats.numTeamKilled;
    int damage  = light ? 0 : c->expandedStats.totalDamageDealt;

    if (builder == SB_BUILDER_SMALL) {
        SB_FIELD(client_num);
        SB_FIELD(c->ps.persistant[0]);
        SB_FIELD(ping);
        SB_FIELD(minutes);
        SB_FIELD(pers_at(c, 13, light));
        SB_FIELD(alive);
        SB_FIELD(kills);
        SB_FIELD(deaths);
        return cost;
    }

    // The nine fields every team builder starts with.
    int best = sb_best_weapon(c);
    SB_FIELD(client_num);
    SB_FIELD(c->sess.sessionTeam);
    SB_FIELD(c->ps.persistant[0]);
    SB_FIELD(ping);
    SB_FIELD(minutes);
    SB_FIELD(kills);
    SB_FIELD(deaths);
    SB_FIELD(shots ? (c->accuracy_hits * 100) / shots : 0);
    SB_FIELD(best);

    switch (builder) {
    case SB_BUILDER_TDM:
        SB_FIELD(pers_at(c, 8, light));
        SB_FIELD(pers_at(c, 9, light));
        SB_FIELD(pers_at(c, 12, light));
        SB_FIELD(tkills);
        SB_FIELD(tkilled);
        SB_FIELD(damage);
        break;

    case SB_BUILDER_CA: {
        // Comes from shotsFired/shotsHit, which lighten() leaves alone.
        int fired = c->expandedStats.shotsFired[best];
        SB_FIELD(fired > 0 ? (int)(((double)c->expandedStats.shotsHit[best] * 100.0) / (double)fired) : 0);
        SB_FIELD(damage);
        SB_FIELD(pers_at(c, 8, light));
        SB_FIELD(pers_at(c, 9, light));
        SB_FIELD(pers_at(c, 12, light));
        SB_FIELD(c->ps.persistant[2] == 0 ? (c->ps.persistant[7] == 0) : 0);
        SB_FIELD(alive);
        break;
    }

    case SB_BUILDER_CTF:
        SB_FIELD(pers_at(c, 8, light));
        SB_FIELD(pers_at(c, 9, light));
        SB_FIELD(pers_at(c, 12, light));
        SB_FIELD(pers_at(c, 10, light));
        SB_FIELD(pers_at(c, 11, light));
        SB_FIELD(pers_at(c, 13, light));
        SB_FIELD(c->ps.persistant[2] == 0 ? (c->ps.persistant[7] == 0) : 0);
        SB_FIELD(alive);
        break;

    case SB_BUILDER_FT:
        SB_FIELD(pers_at(c, 8, light));
        SB_FIELD(pers_at(c, 9, light));
        SB_FIELD(pers_at(c, 12, light));
        SB_FIELD(pers_at(c, 11, light));
        SB_FIELD(tkills);
        SB_FIELD(tkilled);
        SB_FIELD(damage);
        SB_FIELD(alive);
        break;

    default:
        break;
    }
    return cost;
}

#undef SB_FIELD

// Blanks the columns a light entry drops, remembering what was there. Score, ping, team, alive
// and ready are left alone so the row still sorts, and shotsFired/shotsHit with them: Clan
// Arena's per-weapon accuracy reads those through STAT_GetBestWeapon.
static void lighten(int client_num) {
    gclient_t* client    = &level->clients[client_num];
    sb_saved_client_t* s = &sb.light[sb.num_light++];

    s->client           = client_num;
    s->enterTime        = client->pers.enterTime;
    s->accuracy_shots   = client->accuracy_shots;
    s->numKills         = client->expandedStats.numKills;
    s->numDeaths        = client->expandedStats.numDeaths;
    s->numTeamKills     = client->expandedStats.numTeamKills;
    s->numTeamKilled    = client->expandedStats.numTeamKilled;
    s->totalDamageDealt = client->expandedStats.totalDamageDealt;
    for (int i = 0; i < SB_PERS_COUNT; i++) {
        s->persistant[i] = client->ps.persistant[SB_PERS_FIRST + i];
    }

    client->pers.enterTime                 = level->time; // (level->time - enterTime) / 60000 == 0
    client->accuracy_shots                 = 0;           // the builders guard the divide and emit 0
    client->expandedStats.numKills         = 0;
    client->expandedStats.numDeaths        = 0;
    client->expandedStats.numTeamKills     = 0;
    client->expandedStats.numTeamKilled    = 0;
    client->expandedStats.totalDamageDealt = 0;
    for (int i = 0; i < SB_PERS_COUNT; i++) {
        client->ps.persistant[SB_PERS_FIRST + i] = 0;
    }

    sb_stats.lightened++;
}

static void restore(void) {
    for (int i = 0; i < sb.num_light; i++) {
        const sb_saved_client_t* s = &sb.light[i];
        gclient_t* client          = &level->clients[s->client];

        client->pers.enterTime                 = s->enterTime;
        client->accuracy_shots                 = s->accuracy_shots;
        client->expandedStats.numKills         = s->numKills;
        client->expandedStats.numDeaths        = s->numDeaths;
        client->expandedStats.numTeamKills     = s->numTeamKills;
        client->expandedStats.numTeamKilled    = s->numTeamKilled;
        client->expandedStats.totalDamageDealt = s->totalDamageDealt;
        for (int j = 0; j < SB_PERS_COUNT; j++) {
            client->ps.persistant[SB_PERS_FIRST + j] = s->persistant[j];
        }
    }
    sb.num_light = 0;

    if (sb.sorted_changed) {
        memcpy(level->sortedClients, sb.saved_sorted, sizeof(sb.saved_sorted));
        level->numConnectedClients = sb.saved_numConnected;
        sb.sorted_changed          = qfalse;
    }

    sb.armed = qfalse;
}

static int cost_of(const sb_entry_t* e, int tier) { return (tier == SB_TIER_DROP) ? 0 : e->cost[tier]; }

static void demote(sb_entry_t* e, int tier, int* total) {
    *total -= cost_of(e, e->tier) - cost_of(e, tier);
    e->tier = (unsigned char)tier;
}

// Lowest rank first, taking turns between the teams so one side doesn't lose more than the other.
static void shed_team(sb_entry_t* entries, int n, int* total, int budget, int floor_rank, int pinned, int tier) {
    int highest = 0;

    for (int i = 0; i < n; i++) {
        if (entries[i].kind == SB_KIND_TEAM && entries[i].rank > highest) {
            highest = entries[i].rank;
        }
    }

    for (int rank = highest; rank > floor_rank && *total > budget; rank--) {
        for (int team = 0; team < TEAM_NUM_TEAMS && *total > budget; team++) {
            for (int i = 0; i < n; i++) {
                sb_entry_t* e = &entries[i];
                if (e->kind != SB_KIND_TEAM || e->rank != rank || e->team != team) {
                    continue;
                }
                if (e->client != pinned && e->tier < tier) {
                    demote(e, tier, total);
                }
                break;
            }
        }
    }
}

// Spectators sort to the end, so working backwards takes the least interesting ones first.
static void shed_spectators(sb_entry_t* entries, int n, int* total, int budget, int pinned, int tier) {
    for (int i = n - 1; i >= 0 && *total > budget; i--) {
        sb_entry_t* e = &entries[i];
        if (e->kind == SB_KIND_SPEC && e->client != pinned && e->tier < tier) {
            demote(e, tier, total);
        }
    }
}

// Work out how wide every row is, cut rows back until the command fits, then rewrite the list
// the builder is about to walk.
static void plan(gentity_t* ent, int builder, qboolean allow_blank) {
    sb_entry_t entries[SCOREBOARD_MAX_CLIENTS];
    // The stub prints minutes, kills and deaths too, so anything we blank shows up there.
    if (builder == SB_BUILDER_SMALL) {
        allow_blank = qfalse;
    }

    int rank[TEAM_NUM_TEAMS] = {0};
    int n                    = 0;
    int opaque               = 0;
    int pinned               = -1;

    int connected = level->numConnectedClients;
    if (connected <= 0 || connected > SCOREBOARD_MAX_CLIENTS) {
        return;
    }

    if (ent && ent->client) {
        ptrdiff_t slot = ent->client - level->clients;
        if (slot >= 0 && slot < level->maxclients && slot < SCOREBOARD_MAX_CLIENTS) {
            pinned = (int)slot;
        }
    }

    memcpy(sb.saved_sorted, level->sortedClients, sizeof(sb.saved_sorted));
    sb.saved_numConnected = connected;

    int total = header_budget(builder);
    for (int i = 0; i < connected; i++) {
        sb_entry_t* e = &entries[n++];

        // Bounded by maxclients, which is narrower than the array, and reached through by
        // lighten() and entry_cost(). game_events.c clamps the same way.
        int client_num = sb.saved_sorted[i];
        e->client      = client_num;
        e->tier        = SB_TIER_FULL;
        e->rank        = 0;
        e->team        = TEAM_FREE;

        if (client_num < 0 || client_num >= level->maxclients || client_num >= SCOREBOARD_MAX_CLIENTS) {
            e->kind    = SB_KIND_OPAQUE; // we don't know what this is, so pass it straight through
            e->cost[0] = SB_OPAQUE_COST;
            e->cost[1] = SB_OPAQUE_COST;
            opaque++;
            total += SB_OPAQUE_COST;
            continue;
        }

        e->cost[SB_TIER_FULL]  = entry_cost(client_num, builder, qfalse);
        e->cost[SB_TIER_LIGHT] = entry_cost(client_num, builder, qtrue);
        total += e->cost[SB_TIER_FULL];

        team_t team = level->clients[client_num].sess.sessionTeam;
        if (team != TEAM_RED && team != TEAM_BLUE) {
            e->kind = SB_KIND_SPEC; // spectators are never ranked
            continue;
        }

        e->kind = SB_KIND_TEAM;
        e->team = (unsigned char)team;
        e->rank = (short)++rank[team];
        if (e->rank > sb_stats.worst_team) {
            sb_stats.worst_team = e->rank;
        }
    }

    int budget = budget_bytes();
    if (allow_blank) {
        shed_spectators(entries, n, &total, budget, pinned, SB_TIER_LIGHT);
        shed_team(entries, n, &total, budget, full_per_team(), pinned, SB_TIER_LIGHT);
    }
    shed_team(entries, n, &total, budget, light_per_team(), pinned, SB_TIER_DROP);
    shed_spectators(entries, n, &total, budget, pinned, SB_TIER_DROP);
    shed_team(entries, n, &total, budget, min_per_team(), pinned, SB_TIER_DROP);

    int trimmed[SCOREBOARD_MAX_CLIENTS];
    int kept = 0;
    int body = 0;

    for (int i = 0; i < n; i++) {
        sb_entry_t* e = &entries[i];
        if (e->tier == SB_TIER_DROP) {
            sb_stats.omitted++;
            continue;
        }
        if (e->tier == SB_TIER_LIGHT) {
            lighten(e->client);
        }
        body += cost_of(e, e->tier);
        trimmed[kept++] = e->client;
    }

    if (kept != connected) {
        memcpy(level->sortedClients, trimmed, sizeof(int) * (size_t)kept);
        level->numConnectedClients = kept;
        sb.sorted_changed          = qtrue;
    }

    sb.builder         = builder;
    sb.kept            = kept;
    sb.predicted_body  = body;
    sb.have_prediction = opaque ? qfalse : qtrue; // an opaque row makes the total unverifiable

    if (sb.num_light || sb.sorted_changed) {
        sb.armed = qtrue;
        sb_stats.trimmed++;
    }
}

static void begin(gentity_t* ent, qboolean fallback) {
    // EndTrim already unwound anything a previous call left armed; this covers re-entry.
    if (sb.armed) {
        restore();
    }
    sb.num_light       = 0;
    sb.sorted_changed  = qfalse;
    sb.have_prediction = qfalse;
    sb.in_fallback     = fallback;

    if (!enabled()) {
        return;
    }

    int builder = fallback ? sb.retry_builder : builder_for_gametype();
    if (builder < 0 || builder >= SB_BUILDER_COUNT) {
        return;
    }
    plan(ent, builder, fallback ? qfalse : qtrue);
}

void Scoreboard_BeginTrim(gentity_t* ent) { begin(ent, qfalse); }

// Rebuilds a scoreboard that never made it to the client. Nothing is blanked, and the rows are
// sized for whichever builder wrote the one we threw away.
void Scoreboard_BeginFallback(gentity_t* ent) { begin(ent, qtrue); }

// Taken from the name, so the counters stay right for gametypes we don't handle.
static int builder_from_command(const char* cmd) {
    for (int i = 0; i < SB_BUILDER_COUNT; i++) {
        if (cmd_word_is(cmd, sb_builder_name[i])) {
            return i;
        }
    }
    return SB_BUILDER_NONE;
}

// Reads the nth whitespace-separated integer after a command's first word.
static qboolean nth_int(const char* cmd, int n, int* out) {
    const char* p = cmd;
    char* end;

    while (*p && *p != ' ') {
        p++;
    }
    for (int i = 0; i <= n; i++) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            return qfalse;
        }
        if (i == n) {
            break;
        }
        while (*p && *p != ' ') {
            p++;
        }
    }

    long v = strtol(p, &end, 10);
    if (end == p) {
        return qfalse;
    }
    *out = (int)v;
    return qtrue;
}

/*
 * Every scoreboard comes through here as the game module wrote it, before Python sees it. The
 * statistics messages read the same fields straight afterwards, so this is where we put them
 * back. Returning qtrue tells the caller to throw the command away; we build another one.
 */
qboolean Scoreboard_FilterCommand(const char* cmd) {
    if (!cmd) {
        return qfalse;
    }

    // Whole word for the stub, prefix for the rest. There is no plain "scores" command.
    qboolean stub = cmd_word_is(cmd, "smscores");
    if (!stub && strncmp(cmd, "scores_", 7)) {
        return qfalse;
    }

    // SB_BUILDER_NONE for scores_ffa and the like. We count those but can't resize them.
    int len   = (int)strlen(cmd);
    int built = builder_from_command(cmd);

    if (!sb.in_fallback) {
        sb_stats.built++;
    }
    sb_stats.last_len     = len;
    sb_stats.last_rows    = sb.have_prediction ? sb.kept : -1;
    sb_stats.last_builder = built;

    qboolean doomed = (len >= SB_ENGINE_LIMIT);
    if (doomed) {
        sb_stats.oversize++;
    }
    if (stub && sb.armed) {
        sb_stats.fallback++;
    }

    if (sb.have_prediction && built != SB_BUILDER_NONE) {
        int emitted;
        if (nth_int(cmd, sb_header_fields[built] - 3, &emitted) && emitted != sb.kept) {
            sb_stats.drift++;
        }
        // Only the builder we sized rows for tells us anything about its own header.
        if (built == sb.builder) {
            int header = len - sb.predicted_body;
            if (header > sb.header[built] && header < SB_ENGINE_LIMIT) {
                sb.header[built] = header;
            }
        }
    }

    if (!sb.in_fallback && enabled() && built != SB_BUILDER_NONE && (doomed || (stub && sb.armed))) {
        sb_stats.rebuilt++;
        sb.retry         = qtrue;
        sb.retry_builder = built;
        if (sb.armed) {
            restore();
        }
        return qtrue;
    }

    if (sb.armed) {
        restore();
    }
    return qfalse;
}

void Scoreboard_EndTrim(void) {
    if (sb.armed) {
        restore();
    }
    sb.in_fallback = qfalse;
}

qboolean Scoreboard_TakeRetry(void) {
    qboolean retry = sb.retry;
    sb.retry       = qfalse;
    return retry;
}

void Scoreboard_Reset(void) {
    // Not restore(): level->clients points at a fresh game, so writing the old values
    // back would corrupt it.
    sb.armed           = qfalse;
    sb.sorted_changed  = qfalse;
    sb.num_light       = 0;
    sb.in_fallback     = qfalse;
    sb.retry           = qfalse;
    sb.retry_builder   = SB_BUILDER_NONE;
    sb.have_prediction = qfalse;
    memset(sb.header, 0, sizeof(sb.header)); // measure the headers again
    sb_gametype        = NULL;               // re-resolved on the new map
    sb_startingWeapons = NULL;
    sb_forceSmall      = NULL;
    memset(&sb_stats, 0, sizeof(sb_stats));
}

static const char* builder_name(int builder) {
    return (builder >= 0 && builder < SB_BUILDER_COUNT) ? sb_builder_name[builder] : "a scoreboard we do not model";
}

void Scoreboard_Report(void) {
    qboolean on = (qlx_scoreboardTrim && qlx_scoreboardTrim->integer) ? qtrue : qfalse;
    ENGINE_PRINTF("Scoreboard trim: %s. Budget %d bytes, full stats to %d per team, rows to %d, floor %d.\n",
                  on ? "on" : "off", budget_bytes(), full_per_team(), light_per_team(), min_per_team());

    // Resolves g_gametype as a side effect, so it has to run before that is read.
    int builder = builder_for_gametype();
    ENGINE_PRINTF("Gametype %d builds %s. g_forceSmallScoreboardMessage %d.\n",
                  sb_gametype ? sb_gametype->integer : -1, builder_name(builder),
                  sb_forceSmall ? sb_forceSmall->integer : 0);

    ENGINE_PRINTF("Since the last map: %u scoreboards, %u trimmed, %u entries blanked, %u rows dropped.\n",
                  sb_stats.built, sb_stats.trimmed, sb_stats.lightened, sb_stats.omitted);
    ENGINE_PRINTF("  %u fell back to the stub, %u too long for the engine, %u rebuilt, %u row-count mismatches.\n",
                  sb_stats.fallback, sb_stats.oversize, sb_stats.rebuilt, sb_stats.drift);

    if (sb_stats.last_len) {
        int known = (sb_stats.last_builder >= 0 && sb_stats.last_builder < SB_BUILDER_COUNT);
        if (sb_stats.last_rows < 0) {
            ENGINE_PRINTF("Last scoreboard: %s, %d bytes, not costed.\n", builder_name(sb_stats.last_builder),
                          sb_stats.last_len);
        } else {
            ENGINE_PRINTF("Last scoreboard: %s, %d bytes, header %d %s, %d rows planned.\n",
                          builder_name(sb_stats.last_builder), sb_stats.last_len,
                          known ? header_budget(sb_stats.last_builder) : 0,
                          (known && sb.header[sb_stats.last_builder]) ? "measured" : "assumed", sb_stats.last_rows);
        }
    }

    if (!level || !level->clients) {
        return;
    }
    ENGINE_PRINTF("Connected %d, largest team seen %d.\n", level->numConnectedClients, sb_stats.worst_team);
}

// Works out the widths for the current roster without changing anything, so a live capture can
// be compared against what we expected.
void Scoreboard_Verify(void) {
    if (!level || !level->clients) {
        ENGINE_PRINTF("No level loaded.\n");
        return;
    }

    int builder = builder_for_gametype();
    if (builder == SB_BUILDER_NONE) {
        ENGINE_PRINTF("Gametype %d builds a scoreboard we do not model.\n", sb_gametype ? sb_gametype->integer : -1);
        return;
    }

    int connected = level->numConnectedClients;
    if (connected <= 0 || connected > SCOREBOARD_MAX_CLIENTS) {
        return;
    }

    int head  = header_budget(builder);
    int full  = 0;
    int light = 0;

    ENGINE_PRINTF("%s: header %d (%s), budget %d.\n", sb_builder_name[builder], head,
                  sb.header[builder] ? "measured" : "assumed", budget_bytes());
    for (int i = 0; i < connected; i++) {
        int client_num = level->sortedClients[i];
        if (client_num < 0 || client_num >= level->maxclients || client_num >= SCOREBOARD_MAX_CLIENTS) {
            ENGINE_PRINTF("  slot %2d: client %d out of range, assumed %d bytes.\n", i, client_num, SB_OPAQUE_COST);
            full += SB_OPAQUE_COST;
            light += SB_OPAQUE_COST;
            continue;
        }
        int a = entry_cost(client_num, builder, qfalse);
        int b = entry_cost(client_num, builder, qtrue);
        full += a;
        light += b;
        ENGINE_PRINTF("  slot %2d: client %2d team %d, %d bytes full, %d blanked.\n", i, client_num,
                      level->clients[client_num].sess.sessionTeam, a, b);
    }
    ENGINE_PRINTF("Total %d bytes untrimmed, %d fully blanked, against %d.\n", head + full, head + light,
                  budget_bytes());
}
