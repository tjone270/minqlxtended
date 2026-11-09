#ifndef DEMO_MATCH_H
#define DEMO_MATCH_H

#include "features/demos.h"

// Per-match orchestration layered on top of upstream's own demos.c capture.
//
// Capture is sv_demoRecord's alone: with it set, upstream records every
// connected client from their own gamestate and hands each finished file back
// through Demo_PollFinished(). Nothing in here touches a message, a FILE* or
// the writer thread, and nothing in here asks for a slot to be recorded.
//
// What this file adds, all behind sv_demoCut, is knowing where the matches are
// inside those captures and what to do with that:
//   - arm/disarm a match on the engine's own state transitions (game_events.c
//     calls the DemoMatch_OnGame*() entries below - no plugin involved);
//   - bind only the clients who are actually IN the match: a spectator's capture
//     is discarded rather than shipped, and a client who joins the game after the
//     countdown is bound then, with their POV starting sv_demoCutJoinLead
//     seconds before the join;
//   - attribute each segment to a match_id and name the shipped file after the
//     match rather than the wall clock;
//   - run the two-stage cut + snapshot index over each finished segment on a
//     dedicated finalize thread, and trim (or delete) captures no match
//     claimed;
//   - fire demo_recording_started / demo_match_finalized into Python.
//
// EVERY function below is game-thread only, exactly like the Demo_* API it sits
// on. See the threading note at the top of demo_match.c.

// A client just connected. Pure bookkeeping: drops the previous occupant's
// stale per-slot state. Capture for the new client is upstream's own doing.
void DemoMatch_OnClientConnect(int slot);

// Called once per game frame, before upstream's own completion drain, so a
// segment opened during the last frame is noticed while its client is still
// connected and its netchan sequence still readable. Also where the match-close
// deadline runs and post-match Demo_Request overrides are handed back.
void DemoMatch_Frame(void);

// One finished segment, handed straight from upstream's completion queue.
// Called from inside upstream's existing drain loop (hooks.c) rather than from
// a second Demo_PollFinished() loop of our own: that queue is single-consumer,
// so a second drainer would steal an arbitrary subset of the completions.
void DemoMatch_OnFinished(const demo_finished_t* done);

// Map change. Beside upstream's Demo_CloseAll(), which finalises the segments;
// this only closes the match out so the files still get cut and indexed.
void DemoMatch_OnCloseAll(void);

// Match lifecycle, called by game_events.c off the engine-state crossings that
// also fire the corresponding Python events. All no-ops unless sv_demoCut and
// sv_demoRecord are both set (except OnGameEnd/OnCountdownCancelled, which
// close out whatever an earlier call armed).
void DemoMatch_OnGameCountdown(void);
void DemoMatch_OnGameStart(void);
void DemoMatch_OnGameEnd(void);
void DemoMatch_OnCountdownCancelled(void);

#endif /* DEMO_MATCH_H */
