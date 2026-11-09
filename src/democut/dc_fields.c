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

// The protocol-91 delta field tables. THE wire order: field i in this array is
// bit i of the delta record, so a reordering silently corrupts every demo
// rather than failing to build. Transcribed field-for-field from wolfcamql's
// qcommon/msg.c entityStateFieldsQldm91[] / playerStateFieldsQldm91[] - the
// tables the engine that plays these files uses - and independently cross-
// checked against uberdemotools' own EntityStateFields91/PlayerStateFields91,
// which agree entry for entry. Both are 58 entries long.
//
// "bits == 0" means the field is a float on the wire (13-bit biased integer or
// a full 32-bit float); anything else is that many bits of integer, negative
// meaning the reader sign-extends.

#include <stddef.h>

#include "dc_protocol.h"

#define ESF(x, b) {#x, (int)offsetof(dc_entityState_t, x), b}
#define PSF(x, b) {#x, (int)offsetof(dc_playerState_t, x), b}

const dc_netField_t dc_entityStateFields91[] = {
    ESF(pos.trTime, 32),
    ESF(pos.trBase[0], 0),
    ESF(pos.trBase[1], 0),
    ESF(pos.trDelta[0], 0),
    ESF(pos.trDelta[1], 0),
    ESF(pos.trBase[2], 0),
    ESF(apos.trBase[1], 0),
    ESF(pos.trDelta[2], 0),
    ESF(apos.trBase[0], 0),
    ESF(pos.gravity, 32),
    ESF(event, 10),
    ESF(angles2[1], 0),
    ESF(eType, 8),
    ESF(torsoAnim, 8),
    ESF(eventParm, 8),
    ESF(legsAnim, 8),
    ESF(groundEntityNum, DC_GENTITYNUM_BITS),
    ESF(pos.trType, 8),
    ESF(eFlags, 19),
    ESF(otherEntityNum, DC_GENTITYNUM_BITS),
    ESF(weapon, 8),
    ESF(clientNum, 8),
    ESF(angles[1], 0),
    ESF(pos.trDuration, 32),
    ESF(apos.trType, 8),
    ESF(origin[0], 0),
    ESF(origin[1], 0),
    ESF(origin[2], 0),
    ESF(solid, 24),
    ESF(powerups, DC_MAX_POWERUPS),
    ESF(modelindex, 8),
    ESF(otherEntityNum2, DC_GENTITYNUM_BITS),
    ESF(loopSound, 8),
    ESF(generic1, 8),
    ESF(origin2[2], 0),
    ESF(origin2[0], 0),
    ESF(origin2[1], 0),
    ESF(modelindex2, 8),
    ESF(angles[0], 0),
    ESF(time, 32),
    ESF(apos.trTime, 32),
    ESF(apos.trDuration, 32),
    ESF(apos.trBase[2], 0),
    ESF(apos.trDelta[0], 0),
    ESF(apos.trDelta[1], 0),
    ESF(apos.trDelta[2], 0),
    ESF(apos.gravity, 32),
    ESF(time2, 32),
    ESF(angles[2], 0),
    ESF(angles2[0], 0),
    ESF(angles2[2], 0),
    ESF(constantLight, 32),
    ESF(frame, 16),
    ESF(jumpTime, 32),     // Quake Live protocol 90
    ESF(doubleJumped, 1),  // Quake Live protocol 90
    ESF(health, 16),       // Quake Live protocol 91
    ESF(armor, 16),        // Quake Live protocol 91
    ESF(location, 8)       // Quake Live protocol 91
};

const int dc_entityStateFieldCount91 = (int)(sizeof(dc_entityStateFields91) / sizeof(dc_entityStateFields91[0]));

const dc_netField_t dc_playerStateFields91[] = {
    PSF(commandTime, 32),
    PSF(origin[0], 0),
    PSF(origin[1], 0),
    PSF(bobCycle, 8),
    PSF(velocity[0], 0),
    PSF(velocity[1], 0),
    PSF(viewangles[1], 0),
    PSF(viewangles[0], 0),
    PSF(weaponTime, -16),
    PSF(origin[2], 0),
    PSF(velocity[2], 0),
    PSF(legsTimer, 8),
    PSF(pm_time, -16),
    PSF(eventSequence, 16),
    PSF(torsoAnim, 8),
    PSF(movementDir, 4),
    PSF(events[0], 8),
    PSF(legsAnim, 8),
    PSF(events[1], 8),
    PSF(pm_flags, 24),
    PSF(groundEntityNum, DC_GENTITYNUM_BITS),
    PSF(weaponstate, 4),
    PSF(eFlags, 16),
    PSF(externalEvent, 10),
    PSF(gravity, 16),
    PSF(speed, 16),
    PSF(delta_angles[1], 16),
    PSF(externalEventParm, 8),
    PSF(viewheight, -8),
    PSF(damageEvent, 8),
    PSF(damageYaw, 8),
    PSF(damagePitch, 8),
    PSF(damageCount, 8),
    PSF(generic1, 8),
    PSF(pm_type, 8),
    PSF(delta_angles[0], 16),
    PSF(delta_angles[2], 16),
    PSF(torsoTimer, 12),
    PSF(eventParms[0], 8),
    PSF(eventParms[1], 8),
    PSF(clientNum, 8),
    PSF(weapon, 5),
    PSF(weaponPrimary, 8),
    PSF(viewangles[2], 0),
    PSF(grapplePoint[0], 0),
    PSF(grapplePoint[1], 0),
    PSF(grapplePoint[2], 0),
    PSF(jumppad_ent, 10),
    PSF(loopSound, 16),
    PSF(jumpTime, 32),        // Quake Live protocol 90
    PSF(doubleJumped, 1),     // Quake Live protocol 90
    PSF(crouchTime, 32),      // Quake Live protocol 91
    PSF(crouchSlideTime, 32), // Quake Live protocol 91
    PSF(location, 8),         // Quake Live protocol 91
    PSF(fov, 8),              // Quake Live protocol 91
    PSF(forwardmove, 8),      // Quake Live protocol 91
    PSF(rightmove, 8),        // Quake Live protocol 91
    PSF(upmove, 8)            // Quake Live protocol 91
};

const int dc_playerStateFieldCount91 = (int)(sizeof(dc_playerStateFields91) / sizeof(dc_playerStateFields91[0]));

#undef ESF
#undef PSF
