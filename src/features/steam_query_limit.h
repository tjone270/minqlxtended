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

#ifndef STEAM_QUERY_LIMIT_H
#define STEAM_QUERY_LIMIT_H

#include "engine/quake_common.h"

/*
 * Rate limit for the server queries (A2S_INFO, A2S_PLAYER, A2S_RULES) that SV_HandleSteamPacket
 * hands to Steam. Steam answers a query with a packet that is much larger than the request, and
 * the request is a single UDP packet with an easily forged source address, so an unlimited server
 * can be abused to flood someone else. A dropped query is reported to the server as handled, so
 * it never reaches Steam. Nothing but those three queries is ever limited.
 *
 * There is one token bucket per IPv4 source and one for all sources together. The buckets of the
 * sources live in a fixed table, so a flood from many forged addresses cannot make memory grow.
 * Everything runs on the game thread, which is where SV_HandleSteamPacket is called from.
 *
 * Cvars (0 disables a limit, they are read again every five seconds):
 *   qlx_steamQueryRate        queries per second and source (default 1)
 *   qlx_steamQueryBurst       queries a source can send at once (default 10)
 *   qlx_steamQueryGlobalRate  queries per second from all sources together (default 10)
 */

// Registers the cvars with their defaults. A value that is already set, e.g. by server.cfg or
// the command line, is kept.
void SteamQueryLimit_Init(void);

// Returns 1 if the packet may go on to Steam and 0 if it has to be dropped.
int SteamQueryLimit_Allow(const netadr_t* from, const msg_t* msg);

#endif /* STEAM_QUERY_LIMIT_H */
