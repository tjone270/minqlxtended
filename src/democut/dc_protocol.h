/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// Quake Live protocol 91 (.dm_91) wire types, trimmed to what a demo cutter
// needs. Derived from id Tech 3's q_shared.h with the Quake Live additions
// wolfcamql's own q_shared.h carries (the engine that plays these very files);
// the network field tables in dc_msg_read.c / dc_msg_write.c index these
// structs by byte offset exactly as MSG_WriteDeltaEntity does upstream, so a
// member may NOT be reordered, resized or removed without editing those tables
// in lockstep.
//
// EVERY symbol here is prefixed dc_ / DC_ on purpose. This code is linked into
// minqlxtended.so, which is injected into the Quake Live dedicated server - a
// process that already exports its own MSG_*/Huff_*/entityState_t. An
// unprefixed duplicate would be an interposition hazard, not just a style
// problem.

#ifndef DC_PROTOCOL_H
#define DC_PROTOCOL_H

#include <stdint.h>

typedef uint8_t dc_byte;

#define DC_PROTOCOL 91

#define DC_GENTITYNUM_BITS  10
#define DC_MAX_GENTITIES    (1 << DC_GENTITYNUM_BITS)
#define DC_ENTITYNUM_NONE   (DC_MAX_GENTITIES - 1)

#define DC_MAX_CLIENTS      64
#define DC_MAX_STATS        16
#define DC_MAX_PERSISTANT   16
#define DC_MAX_POWERUPS     16
#define DC_MAX_WEAPONS      16
#define DC_MAX_PS_EVENTS    2

#define DC_MAX_CONFIGSTRINGS 1024
#define DC_MAX_STRING_CHARS  1024
#define DC_BIG_INFO_STRING   8192

// qcommon.h: MAX_MSGLEN 16384*2. Every .dm_91 block must fit in this, and the
// engine refuses to play one that does not.
#define DC_MAX_MSGLEN       16384 * 2

// The client's snapshot backup ring. A snapshot's wire deltaNum is "how many
// messages back" its reference sits, so it can never usefully exceed this.
#define DC_PACKET_BACKUP    32
#define DC_PACKET_MASK      (DC_PACKET_BACKUP - 1)

// Entity slots the client keeps across the snapshot ring. Same size id Tech 3
// uses (MAX_PARSE_ENTITIES); a snapshot references a contiguous window of it.
#define DC_MAX_PARSE_ENTITIES 2048

#define DC_MAX_MAP_AREA_BYTES 32

// svc_ops_e
#define DC_SVC_BAD           0
#define DC_SVC_NOP           1
#define DC_SVC_GAMESTATE     2
#define DC_SVC_CONFIGSTRING  3
#define DC_SVC_BASELINE      4
#define DC_SVC_SERVERCOMMAND 5
#define DC_SVC_DOWNLOAD      6
#define DC_SVC_SNAPSHOT      7
#define DC_SVC_EOF           8

#define DC_FLOAT_INT_BITS 13
#define DC_FLOAT_INT_BIAS (1 << (DC_FLOAT_INT_BITS - 1))

typedef float dc_vec3_t[3];

typedef struct {
    int trType;
    int trTime;
    int trDuration;
    dc_vec3_t trBase;
    dc_vec3_t trDelta;
    int gravity; // Quake Live addition (protocol 91 sends pos.gravity/apos.gravity)
} dc_trajectory_t;

typedef struct dc_entityState_s {
    int number;
    int eType;
    int eFlags;

    dc_trajectory_t pos;
    dc_trajectory_t apos;

    int time;
    int time2;

    dc_vec3_t origin;
    dc_vec3_t origin2;

    dc_vec3_t angles;
    dc_vec3_t angles2;

    int otherEntityNum;
    int otherEntityNum2;

    int groundEntityNum;

    int constantLight;
    int loopSound;

    int modelindex;
    int modelindex2;
    int clientNum;
    int frame;

    int solid;

    int event;
    int eventParm;

    int powerups;
    int weapon;
    int legsAnim;
    int torsoAnim;

    int generic1;

    // Quake Live protocol 90
    int jumpTime;
    int doubleJumped;

    // Quake Live protocol 91
    int health;
    int armor;
    int location;
} dc_entityState_t;

typedef struct dc_playerState_s {
    int commandTime;
    int pm_type;
    int bobCycle;
    int pm_flags;
    int pm_time;

    dc_vec3_t origin;
    dc_vec3_t velocity;
    int weaponTime;
    int gravity;
    int speed;
    int delta_angles[3];

    int groundEntityNum;

    int legsTimer;
    int legsAnim;

    int torsoTimer;
    int torsoAnim;

    int movementDir;

    dc_vec3_t grapplePoint;

    int eFlags;

    int eventSequence;
    int events[DC_MAX_PS_EVENTS];
    int eventParms[DC_MAX_PS_EVENTS];

    int externalEvent;
    int externalEventParm;
    int externalEventTime;

    int clientNum;
    int weapon;
    int weaponstate;

    dc_vec3_t viewangles;
    int viewheight;

    int damageEvent;
    int damageYaw;
    int damagePitch;
    int damageCount;

    int stats[DC_MAX_STATS];
    int persistant[DC_MAX_PERSISTANT];
    int powerups[DC_MAX_POWERUPS];
    int ammo[DC_MAX_WEAPONS];

    int generic1;
    int loopSound;
    int jumppad_ent;

    // Quake Live protocol 90
    int doubleJumped;
    int jumpTime;

    // Quake Live protocol 91
    int crouchTime;
    int crouchSlideTime;
    int location;
    int fov;
    int forwardmove;
    int rightmove;
    int upmove;
    int weaponPrimary;
} dc_playerState_t;

// One client snapshot as the reader resolves it: the wire deltaNum, plus the
// window into the parser's entity ring this snapshot's entities occupy. Kept
// separate from dc_entityState_t storage so a snapshot can be memcpy'd into the
// PACKET_BACKUP ring cheaply, exactly like idClientSnapshot.
typedef struct {
    int valid;
    int snapFlags;
    int serverTime;
    int messageNum;
    int deltaNum;     // absolute message number of the baseline, -1 = none
    int deltaWireNum; // the byte the file actually carries (0 = full snapshot)
    int areamaskLen;
    dc_byte areamask[DC_MAX_MAP_AREA_BYTES];
    int serverCommandNum;
    dc_playerState_t ps;
    int numEntities;
    int parseEntitiesNum;
} dc_snapshot_t;

typedef struct {
    const char *name;
    int offset;
    int bits; // 0 = float
} dc_netField_t;

extern const dc_netField_t dc_entityStateFields91[];
extern const int dc_entityStateFieldCount91;
extern const dc_netField_t dc_playerStateFields91[];
extern const int dc_playerStateFieldCount91;

#endif /* DC_PROTOCOL_H */
