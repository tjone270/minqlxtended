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

// First, ahead of any system header: Python.h sets _POSIX_C_SOURCE and _XOPEN_SOURCE.
#include "python/pyminqlxtended.h"

#include "game_events.h"
#include "profile.h"
#include "engine/quake_common.h"

// Cached state. Game thread only, so no locking. GameEvents_Reset clears all of it, on map load
// and on map_restart.

// roundState.eCurrent as of last frame, so we only fire on the transitions.
static roundStateState_t last_round_state;

// The round number reported for round_start, held so round_end reports the same one. The engine's
// counter moves on some draw paths and not others, so re-reading it at the end would have the two
// disagree about the round they both describe.
static int current_round;

// level.warmupTime as of last frame. GameEvents_Reset seeds it rather than leaving it to the first
// poll; see CheckGameState. SetWarmupTime is the only writer of the g_gameState cvar and derives
// it from this field alone: <0 PRE_GAME, 0 IN_PROGRESS, >0 COUNT_DOWN.
static int last_warmup_time;

// g_gametype, resolved lazily and dropped on map load. Only needed to number rounds: Attack &
// Defend plays each round twice, once per side, and splits the pair number and the side across
// roundState.round and roundState.turn.
static cvar_t* ge_gametype;

#define GAMETYPE_ATTACK_AND_DEFEND 11

// level.time when the round started. roundState.startTime is when the *current* state
// began, so at ROUND_OVER it reports a duration of zero. Captured at ROUND_BEGUN instead.
static int round_begun_time;

// Team scores as of last frame. The round transition bumps the winning team's score before the
// state settles, so diffing these finds the winner. roundState.prevRoundWinningTeam will not
// serve: despite the name, only Freeze Tag touches it.
static int last_team_scores[TEAM_NUM_TEAMS];

// level->intermissionQueued as of last frame. A match ends when this goes non-zero, and
// that happens while the level is still standing.
static int last_intermission_queued;

// sess.sessionTeam per client slot. tracked[] tells a real change apart from the first
// sighting of a slot, so joining a server doesn't read as a switch out of TEAM_FREE.
static team_t last_team[MAX_CLIENTS];
static int team_tracked[MAX_CLIENTS];

// A cancelling team_switch handler puts the player back through the command buffer, so the revert
// lands a frame or more later and would otherwise read as a fresh switch. Holds the team the
// player is expected back on, or -1. The put can also be refused outright, by a locked team or a
// gametype that declines it, so revert_deadline[] is the level->time the latch expires at.
static int revert_pending[MAX_CLIENTS];
static int revert_deadline[MAX_CLIENTS];

// A Cbuf round trip takes a frame or two. Anything beyond this is a different switch.
#define REVERT_TIMEOUT_MS 3000

// Objective counters as of last frame, per client, from pers.teamState. Polled on the pass
// CheckTeams already makes. Indices are OBJ_* in game_events.h.
static int last_objective[MAX_CLIENTS][OBJ_COUNT];

// Vote state as of last frame. There's nothing to hook: in qagame 1069 the `vote` client command
// is inlined into ClientCommand and the resolution into G_RunFrame. The string and tallies are
// cached while the vote is live, since ClearVote clears CS_VOTE_STRING and every client's
// voteState and nothing about a finished vote is reliably readable afterwards.
static int last_vote_time;
static int last_vote_yes;
static int last_vote_no;
static char last_vote_string[MAX_STRING_CHARS];

void GameEvents_Reset(void) {
    last_round_state         = PREGAME;
    last_intermission_queued = 0;
    round_begun_time         = 0;
    current_round            = 0;

    // A fresh level is always PRE_GAME. Seeding 0 here, the value the match-start transition
    // moves *to*, would swallow game_start. See CheckGameState.
    last_warmup_time = -1;

    ge_gametype = NULL; // re-resolved on the new map, as scoreboard.c does with its copy

    last_vote_time      = 0;
    last_vote_yes       = 0;
    last_vote_no        = 0;
    last_vote_string[0] = '\0';

    for (int i = 0; i < TEAM_NUM_TEAMS; i++) {
        last_team_scores[i] = 0;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        last_team[i]       = TEAM_FREE;
        team_tracked[i]    = 0;
        revert_pending[i]  = -1;
        revert_deadline[i] = 0;

        for (int j = 0; j < OBJ_COUNT; j++) {
            last_objective[i][j] = 0;
        }
    }
}

// Reads the six counters into the order objective_t declares, so the loop below and the
// dispatcher agree without either knowing the struct.
static void ReadObjectives(const playerTeamState_t* ts, int out[OBJ_COUNT]) {
    out[OBJ_CAPTURE]         = ts->captures;
    out[OBJ_RETURN]          = ts->flagrecovery;
    out[OBJ_ASSIST]          = ts->assists;
    out[OBJ_BASE_DEFENSE]    = ts->basedefense;
    out[OBJ_CARRIER_DEFENSE] = ts->carrierdefense;
    out[OBJ_FRAG_CARRIER]    = ts->fragcarrier;
}

// Which round this is, read from the game module's counters rather than parsed out of
// CS_ROUND_WARMUP, whose `state` key is not roundState.eCurrent: AD_RoundStateTransition writes a
// literal 2 into it on the round-begun path, and that form has no `round` key at all.
static int RoundNumber(void) {
    if (!ge_gametype && Cvar_FindVar) {
        ge_gametype = Cvar_FindVar("g_gametype");
    }

    if (ge_gametype && ge_gametype->integer == GAMETYPE_ATTACK_AND_DEFEND) {
        // Two rounds per pair, one per side, and the pair is counted from zero.
        return level->roundState.round * 2 + 1 + level->roundState.turn;
    }

    return level->roundState.round;
}

static void CheckRoundState(void) {
    roundStateState_t current = level->roundState.eCurrent;

    if (current == last_round_state) {
        // Steady state. Keep the score baseline current so that when a round does end, the
        // only difference against it is the point the winning team just scored.
        for (int i = 0; i < TEAM_NUM_TEAMS; i++) {
            last_team_scores[i] = level->teamScores[i];
        }
        return;
    }

    last_round_state = current;

    // PREGAME and ROUND_SHUFFLE pass through without an event, and POSTGAME is the match
    // ending, which CheckIntermission reports.
    if (current == ROUND_WARMUP) {
        RoundCountdownDispatcher(RoundNumber());
        return;
    }

    if (current == ROUND_BEGUN) {
        round_begun_time = level->time;
        current_round    = RoundNumber();
        RoundStartDispatcher(current_round);
        return;
    }

    // No need to test the previous state: this is only reached when it differed, so it
    // cannot also have been ROUND_OVER.
    if (current != ROUND_OVER) {
        return;
    }

    // Whichever team gained a point took the round. Neither gaining one is a draw, which
    // every round-based gametype can produce.
    int winner = TEAM_FREE;
    for (int i = TEAM_RED; i <= TEAM_BLUE; i++) {
        if (level->teamScores[i] > last_team_scores[i]) {
            winner = i;
            break;
        }
    }

    RoundEndDispatcher(current_round, winner,
                       round_begun_time ? level->time - round_begun_time : 0);

    for (int i = 0; i < TEAM_NUM_TEAMS; i++) {
        last_team_scores[i] = level->teamScores[i];
    }
}

// The three states g_gameState names, from level.warmupTime. The baseline is seeded rather than
// learned: a match starts by way of a map_restart, and G_InitGame calls SetWarmupTime(0) before
// GameEvents_Reset runs, so a baseline learned from the first poll would already be 0 and
// game_start would never fire. Seeding -1 is safe; G_SpawnEntitiesFromString sets -1 first.
static void CheckGameState(void) {
    int now = level->warmupTime;

    if (now == last_warmup_time) {
        return;
    }

    int was          = last_warmup_time;
    last_warmup_time = now;

    // A crossing. warmupTime counts down while it is positive, so only the
    // moves between the three bands are events.
    if (now > 0 && was <= 0) {
        GameCountdownDispatcher();
    } else if (now == 0 && was != 0) {
        GameStartDispatcher();
    } else if (now < 0 && was == 0) {
        // In progress and then back to waiting for players, with no intermission queued: a
        // forfeit, or an admin ending it. Never a map change; both My_SV_SpawnServer and
        // My_G_InitGame reset the baseline first, so no frame ever observes that crossing.
        GameEndDispatcher(1);
    }
}

static void CheckIntermission(void) {
    int queued = level->intermissionQueued;
    int was    = last_intermission_queued;

    last_intermission_queued = queued;

    if (queued && !was) {
        // One of two paths to game_end, the other being the abandoned match in
        // CheckGameState above. Python's `_game_ended` latches so only one fires per match.
        GameEndDispatcher(level->matchForfeited ? 1 : 0);
    }
}

static void CheckVote(void) {
    // The end-of-match map vote borrows voteTime. BeginIntermission (qagame 0x10056d40) calls
    // ClearVote and then sets level.voteTime = level.time, so voteTime goes non-zero with
    // voteString already empty. Reported as a vote start, that is an empty vote_started at every
    // intermission with no vote_ended after it, the map change having reset this first.
    if (level->intermissionTime) {
        last_vote_time = level->voteTime;
        return;
    }

    int now = level->voteTime;

    if (now && !last_vote_time) {
        // pendingVoteCaller is the engine's own record, so a plugin-called vote reports
        // whatever callvote() was told and an engine-started one reports nobody.
        VoteStartedDispatcher(level->pendingVoteCaller, level->voteString);
    } else if (!now && last_vote_time) {
        // A non-zero voteExecuteTime distinguishes a pass. In qagame 1069 every resolution path
        // calls ClearVote, zeroing voteTime and voteExecuteTime together, and only the pass path
        // then sets voteExecuteTime = level.time + 3000.
        VoteEndedDispatcher(level->voteExecuteTime != 0,
                            last_vote_string, last_vote_yes, last_vote_no);
    }

    last_vote_time = now;

    if (now) {
        // Only refreshed while a vote is live, so the values survive into the frame that sees it
        // end. A passing vote's tally is one short: G_RunFrame writes level.voteYes/voteNo only
        // on the still-running branch, so the frame that tips it over the line never records the
        // deciding vote. An expiry reports the true count.
        last_vote_yes = level->voteYes;
        last_vote_no  = level->voteNo;
        strncpy(last_vote_string, level->voteString, sizeof(last_vote_string) - 1);
        last_vote_string[sizeof(last_vote_string) - 1] = '\0';
    }
}

static void CheckTeams(void) {
    if (!level->clients) {
        return;
    }

    int maxclients = level->maxclients;
    if (maxclients > MAX_CLIENTS) {
        maxclients = MAX_CLIENTS;
    }

    for (int i = 0; i < maxclients; i++) {
        gclient_t* client = &level->clients[i];

        if (client->pers.connected != CON_CONNECTED) {
            // Forget the slot so the next occupant gets a fresh baseline instead of
            // inheriting the previous player's team.
            team_tracked[i]   = 0;
            revert_pending[i] = -1;
            continue;
        }

        // Objective counters, on the same pass. team_tracked doubles as the baseline flag, so a
        // player connecting into a scored slot does not read as having just done all of it. A
        // counter climbing by more than one in a frame raises one event carrying the new total.
        int objectives[OBJ_COUNT];
        ReadObjectives(&client->pers.teamState, objectives);

        if (team_tracked[i]) {
            for (int j = 0; j < OBJ_COUNT; j++) {
                if (objectives[j] > last_objective[i][j]) {
                    ObjectiveDispatcher(i, j, objectives[j]);
                }
            }
        }

        for (int j = 0; j < OBJ_COUNT; j++) {
            last_objective[i][j] = objectives[j];
        }

        team_t current = client->sess.sessionTeam;

        if (!team_tracked[i]) {
            last_team[i]    = current;
            team_tracked[i] = 1;
            continue;
        }

        if (current == last_team[i]) {
            continue;
        }

        // Nothing came back within the window, so the put was refused or lost.
        if (revert_pending[i] >= 0 && level->time > revert_deadline[i]) {
            revert_pending[i] = -1;
        }

        if (revert_pending[i] >= 0) {
            int expected      = revert_pending[i];
            revert_pending[i] = -1;
            if (current == (team_t)expected) {
                // The cancelled switch has been undone. Absorb it silently.
                last_team[i] = current;
                continue;
            }
            // Something else won the race; treat it as a real switch and fall through.
        }

        team_t previous = last_team[i];
        last_team[i]    = current;

        if (!TeamSwitchDispatcher(i, previous, current)) {
            // Cancelled. The handler has queued a put back to `previous`; swallow that
            // when we see it instead of reporting it as another switch.
            revert_pending[i]  = previous;
            revert_deadline[i] = level->time + REVERT_TIMEOUT_MS;
        }
    }
}

void GameEvents_Frame(void) {
    // level is resolved in InitializeVm, after the VM is mapped. G_RunFrame cannot run
    // before that, but a NULL deref on the frame path would take the server down.
    if (!level) {
        return;
    }

    PROF_BEGIN(t_events);

    // Game state before the round state, so a plugin hooking both sees the match start
    // before the first round of it does.
    CheckGameState();
    CheckRoundState();
    CheckIntermission();
    CheckVote();
    CheckTeams();

    PROF_END(PROF_GAME_EVENTS, t_events);
}
