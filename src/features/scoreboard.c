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
#include <string.h>

#include "scoreboard.h"

/* See scoreboard.h for what this works around. */

#define SCOREBOARD_MAX_CLIENTS 64 // level->sortedClients is a fixed 64 entries

// ps.persistant[8..13]: gametype-specific scoreboard columns (captures, defends, ...).
// Taken as a range since the meaning varies but all of them are blankable.
#define SB_PERS_FIRST 8
#define SB_PERS_COUNT 6

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

static struct {
    qboolean armed;
    qboolean sorted_changed;
    int saved_sorted[SCOREBOARD_MAX_CLIENTS];
    int saved_numConnected;
    sb_saved_client_t light[SCOREBOARD_MAX_CLIENTS];
    int num_light;
} sb;

static cvar_t* qlx_scoreboardTrim;
static cvar_t* qlx_scoreboardFullPerTeam;
static cvar_t* qlx_scoreboardLightPerTeam;
static cvar_t* sb_gametype;

static struct {
    unsigned trimmed;   // scoreboards we altered
    unsigned lightened; // entries whose stats we zeroed
    unsigned omitted;   // entries we dropped
    int worst_team;     // largest single-team headcount seen
} sb_stats;

void Scoreboard_Init(void) {
    if (!Cvar_Get) {
        return;
    }
    qlx_scoreboardTrim         = Cvar_Get("qlx_scoreboardTrim", "1", CVAR_ARCHIVE);
    qlx_scoreboardFullPerTeam  = Cvar_Get("qlx_scoreboardFullPerTeam", "8", CVAR_ARCHIVE);
    qlx_scoreboardLightPerTeam = Cvar_Get("qlx_scoreboardLightPerTeam", "12", CVAR_ARCHIVE);
}

// Ranks up to here keep every stat the builder would normally emit.
static int full_per_team(void) { return cvar_clamped(qlx_scoreboardFullPerTeam, 8, 0, SCOREBOARD_MAX_CLIENTS); }

// Ranks past full_per_team() up to here keep their row with the stats blanked. A value
// below the full cutoff would blank nobody and drop everyone instead, so it is raised.
static int light_per_team(void) {
    int full  = full_per_team();
    int light = cvar_clamped(qlx_scoreboardLightPerTeam, 12, 0, SCOREBOARD_MAX_CLIENTS);
    return (light < full) ? full : light;
}

// Gametypes whose per-client entry carries sess.sessionTeam, the field ranked on. FFA, Duel, Race
// and gametype 7 have no team column at all. Red Rover (12) is excluded despite being team based:
// it emits ps.localPersistant[0] as its team column. Verify what that slot holds before adding it.
static qboolean team_gametype(void) {
    if (!sb_gametype) {
        if (!Cvar_FindVar) {
            return qfalse;
        }
        sb_gametype = Cvar_FindVar("g_gametype");
        if (!sb_gametype) {
            return qfalse;
        }
    }

    switch (sb_gametype->integer) {
    case 3:  // Team Deathmatch
    case 4:  // Clan Arena
    case 5:  // Capture the Flag
    case 6:  // One Flag CTF
    case 8:  // Harvester
    case 9:  // Freeze Tag
    case 10: // Domination
    case 11: // Attack & Defend
        return qtrue;
    default:
        return qfalse;
    }
}

static qboolean enabled(void) {
    return (qlx_scoreboardTrim && qlx_scoreboardTrim->integer && level && level->clients) ? qtrue : qfalse;
}

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

void Scoreboard_BeginTrim(void) {
    // EndTrim already unwound anything a previous call left armed; this covers re-entry.
    if (sb.armed) {
        restore();
    }
    sb.num_light      = 0;
    sb.sorted_changed = qfalse;

    if (!enabled() || !team_gametype()) {
        return;
    }

    int connected = level->numConnectedClients;
    if (connected <= 0 || connected > SCOREBOARD_MAX_CLIENTS) {
        return;
    }

    int full  = full_per_team();
    int light = light_per_team();

    int trimmed[SCOREBOARD_MAX_CLIENTS];
    int rank[TEAM_NUM_TEAMS] = {0};
    int kept                 = 0;

    memcpy(sb.saved_sorted, level->sortedClients, sizeof(sb.saved_sorted));
    sb.saved_numConnected = connected;

    for (int i = 0; i < connected; i++) {
        // Bounded by maxclients, which is narrower than the array. lighten() writes through
        // this index. game_events.c clamps the same way.
        int client_num = sb.saved_sorted[i];
        if (client_num < 0 || client_num >= level->maxclients || client_num >= SCOREBOARD_MAX_CLIENTS) {
            trimmed[kept++] = client_num; // not ours to interpret; pass it through
            continue;
        }

        // Spectators and TEAM_FREE are never ranked and never trimmed.
        team_t team = level->clients[client_num].sess.sessionTeam;
        if (team != TEAM_RED && team != TEAM_BLUE) {
            trimmed[kept++] = client_num;
            continue;
        }

        int place = ++rank[team];
        if (place > sb_stats.worst_team) {
            sb_stats.worst_team = place;
        }

        if (place > light) {
            sb_stats.omitted++;
            continue; // dropped: never copied into the trimmed list
        }
        if (place > full) {
            lighten(client_num);
        }
        trimmed[kept++] = client_num;
    }

    if (kept != connected) {
        memcpy(level->sortedClients, trimmed, sizeof(int) * (size_t)kept);
        level->numConnectedClients = kept;
        sb.sorted_changed          = qtrue;
    }

    if (sb.num_light || sb.sorted_changed) {
        sb.armed = qtrue;
        sb_stats.trimmed++;
    }
}

// SelectScoreboardMessage runs the intermission statistics messages off the same fields
// after this, so the trim is undone here to keep those values correct. My_SV_SendServerCommand
// calls it before dispatching, so plugins never see the blanked values either.
void Scoreboard_NoteCommand(const char* cmd) {
    if (!sb.armed || !cmd) {
        return;
    }
    // Whole-word, via the shared helper: a prefix match would fire on any command that
    // merely starts with "scores".
    if (!cmd_word_is(cmd, "scores") && !cmd_word_is(cmd, "smscores")) {
        return;
    }
    restore();
}

void Scoreboard_EndTrim(void) {
    if (sb.armed) {
        restore();
    }
}

void Scoreboard_Reset(void) {
    // Not restore(): level->clients points at a fresh game, so writing the old values
    // back would corrupt it.
    sb.armed          = qfalse;
    sb.sorted_changed = qfalse;
    sb.num_light      = 0;
    sb_gametype       = NULL; // re-resolved on the new map
    memset(&sb_stats, 0, sizeof(sb_stats));
}

void Scoreboard_Report(void) {
    qboolean on = (qlx_scoreboardTrim && qlx_scoreboardTrim->integer) ? qtrue : qfalse;
    ENGINE_PRINTF("Scoreboard trim: %s. Full stats to %d per team, blanked to %d, dropped beyond.\n",
               on ? "on" : "off", full_per_team(), light_per_team());

    // Resolves g_gametype as a side effect, so it has to run before that is read.
    qboolean eligible = team_gametype();
    ENGINE_PRINTF("Gametype %d is %seligible.\n", sb_gametype ? sb_gametype->integer : -1, eligible ? "" : "not ");
    ENGINE_PRINTF("Since the last map: %u scoreboards trimmed, %u entries blanked, %u dropped. "
               "Largest team seen %d.\n",
               sb_stats.trimmed, sb_stats.lightened, sb_stats.omitted, sb_stats.worst_team);

    if (!level || !level->clients) {
        return;
    }
    ENGINE_PRINTF("Connected %d, currently trimmed: %s.\n", level->numConnectedClients, sb.armed ? "yes" : "no");
}
