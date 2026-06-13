/*
Copyright (C) 1997-2001 id Software, Inc.
Copyright (C) 2015 Mino <mino@minomino.org>
Copyright (C) 2022-2025 Thomas Jones <me@thomasjones.id.au>

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


#ifndef QUAKE_COMMON_H
#define QUAKE_COMMON_H

#include <stdint.h>

#include "common.h"
#include "patterns.h"


#define MAX_CLIENTS 64
#define MAX_CHALLENGES 1024
#define MAX_MSGLEN 16384
#define MAX_NETCHAN_MSGLEN 32768 // [QL] netchan uses 32KB buffers (Q3 used MAX_MSGLEN=16KB)
#define MAX_PS_EVENTS 2
#define MAX_MAP_AREA_BYTES 32 // bit vector of area visibility
#define MAX_INFO_STRING 1024
#define MAX_RELIABLE_COMMANDS 64 // max string commands buffered for restransmit
#define MAX_STRING_CHARS 1024    // max length of a string passed to Cmd_TokenizeString
#define MAX_NAME_LENGTH 40       // [QL] max length of a client name (Q3 used 32)
#define MAX_QPATH 64             // max length of a quake game pathname
#define MAX_DOWNLOAD_WINDOW 8    // max of eight download frames
#define MAX_NETNAME 36
#define PACKET_BACKUP 32 // number of old messages that must be kept on client and
                         // server for delta compression and ping estimation
#define PACKET_MASK (PACKET_BACKUP - 1)
#define MAX_ENT_CLUSTERS 16
#define MAX_MODELS 256 // these are sent over the net as 8 bits
#define MAX_SOUNDS 256
#define MAX_LOCATIONS 64
#define MAX_CONFIGSTRINGS 1024
#define GENTITYNUM_BITS 10 // don't need to send any more
#define MAX_GENTITIES (1 << GENTITYNUM_BITS)
#define ENTITYNUM_NONE (MAX_GENTITIES - 1)
#define ENTITYNUM_WORLD (MAX_GENTITIES - 2)
#define ENTITYNUM_MAX_NORMAL (MAX_GENTITIES - 2)
#define MAX_ITEM_MODELS 4
#define MAX_SPAWN_VARS 64
#define MAX_SPAWN_VARS_CHARS 4096
#define BODY_QUEUE_SIZE 8

// configstrings
#define CS_SERVERINFO 0 // an info string with all the serverinfo cvars
#define CS_SYSTEMINFO 1 // an info string for server system to client system configuration
#define CS_MUSIC 2
#define CS_MESSAGE 3 // from the map worldspawn's message field
#define CS_MOTD 4    // g_motd string for server message of the day (not used in QL)
#define CS_WARMUP 5  // server time when the match will be restarted
#define CS_SCORES1 6 // 1st place score
#define CS_SCORES2 7 // 2nd place score
#define CS_VOTE_TIME 8
#define CS_VOTE_STRING 9
#define CS_VOTE_YES 10
#define CS_VOTE_NO 11
#define CS_GAME_VERSION 12
#define CS_LEVEL_START_TIME 13 // so the timer only shows the current level
#define CS_INTERMISSION 14     // when 1, fraglimit/timelimit has been hit and intermission will start in a second or two
#define CS_ITEMS 15            // string of 0's and 1's that tell the client which items are present/to load.
#define CS_BOTINFO 16          // internal debugging stuff (infostring to render bot_hud values on client screen)
#define CS_MODELS 17
#define CS_SOUNDS (CS_MODELS + MAX_MODELS)             // 273
#define CS_PLAYERS (CS_SOUNDS + MAX_SOUNDS)            // 529
#define CS_LOCATIONS (CS_PLAYERS + MAX_CLIENTS)        // 593
#define CS_LAST_GENERIC (CS_LOCATIONS + MAX_LOCATIONS) // 657
#define CS_FLAGSTATUS 658
#define CS_SCORES1PLAYER 659 // 1st place player's name
#define CS_SCORES2PLAYER 660 // 2nd place player's name
#define CS_ROUND_WARMUP 661
#define CS_ROUND_START_TIME 662
#define CS_TEAMCOUNT_RED 663
#define CS_TEAMCOUNT_BLUE 664
#define CS_SHADERSTATE 665
#define CS_NEXTMAP 666
#define CS_PRACTICE 667
#define CS_FREECAM 668
#define CS_PAUSE_START_TIME 669 // if this is non-zero, the game is paused
#define CS_PAUSE_END_TIME 670   // 0 = pause, !0 = timeout
#define CS_TIMEOUTS_RED 671     // TOs REMAINING
#define CS_TIMEOUTS_BLUE 672
#define CS_MODEL_OVERRIDE 673
#define CS_PLAYER_CYLINDERS 674
#define CS_DEBUGFLAGS 675
#define CS_ENABLEBREATH 676
#define CS_DMGTHROUGHDEPTH 677
#define CS_AUTHOR 678 // from the map worldspawn's author field
#define CS_AUTHOR2 679
#define CS_ADVERT_DELAY 680
#define CS_PMOVEINFO 681
#define CS_ARMORINFO 682
#define CS_WEAPONINFO 683
#define CS_PLAYERINFO 684
#define CS_SCORE1STPLAYER 685     // Score of the duel player on the left
#define CS_SCORE2NDPLAYER 686     // Score of the duel player on the right
#define CS_CLIENTNUM1STPLAYER 687 // ClientNum of the duel player on the left
#define CS_CLIENTNUM2NDPLAYER 688 // ClientNum of the duel player on the right
#define CS_ATMOSEFFECT 689        // unused, was per-map rain/snow effects
#define CS_MOST_DAMAGEDEALT_PLYR 690
#define CS_MOST_ACCURATE_PLYR 691
#define CS_REDTEAMBASE 692
#define CS_BLUETEAMBASE 693
#define CS_BEST_ITEMCONTROL_PLYR 694
#define CS_MOST_VALUABLE_OFFENSIVE_PLYR 695
#define CS_MOST_VALUABLE_DEFENSIVE_PLYR 696
#define CS_MOST_VALUABLE_PLYR 697
#define CS_GENERIC_COUNT_RED 698
#define CS_GENERIC_COUNT_BLUE 699
#define CS_AD_SCORES 700
#define CS_ROUND_WINNER 701
#define CS_CUSTOM_SETTINGS 702
#define CS_ROTATIONMAPS 703
#define CS_ROTATIONVOTES 704
#define CS_DISABLE_VOTE_UI 705
#define CS_ALLREADY_TIME 706
#define CS_INFECTED_SURVIVOR_MINSPEED 707
#define CS_RACE_POINTS 708
#define CS_DISABLE_LOADOUT 709
#define CS_MATCH_GUID 712         // also sent in the ZMQ stats
#define CS_STARTING_WEAPONS 713   // appears on the ESC menu in the client under Starting Weapons - bitmask of weapons identical to g_startingWeapons
#define CS_STEAM_ID 714           // the server's steam ID (64)
#define CS_STEAM_WORKSHOP_IDS 715 // space separated list of workshop IDs for the client to load
#define CS_MAX 716
#define RESERVED_CONFIGSTRINGS 2 // game can't modify below this, only the system can

// bit field limits
#define MAX_STATS 16
#define MAX_PERSISTANT 16
#define MAX_POWERUPS 16
#define MAX_WEAPONS 16

// Button flags
#define BUTTON_ATTACK 1
#define BUTTON_TALK 2         // displays talk balloon and disables actions
#define BUTTON_USE_HOLDABLE 4 // Mino: +button2
#define BUTTON_GESTURE 8      // Mino: +button3
#define BUTTON_WALKING 16
// Block of unused button flags, or at least flags I couldn't trigger.
// Q3 used them for bot commands, so probably unused in QL.
#define BUTTON_UNUSED1 32
#define BUTTON_UNUSED2 64
#define BUTTON_UNUSED3 128
#define BUTTON_UNUSED4 256
#define BUTTON_UNUSED5 512
#define BUTTON_UNUSED6 1024
#define BUTTON_UPMOVE 2048     // Mino: Not in Q3. I'm guessing it's for cg_autohop.
#define BUTTON_ANY 4096        // any key whatsoever
#define BUTTON_IS_ACTIVE 65536 // Mino: No idea what it is, but it goes off after a while of being
                               //   AFK, then goes on after being active for a while.

// eflags
#define EF_DEAD 0x00000001            // don't draw a foe marker over players with EF_DEAD
#define EF_TICKING 0x00000002         // used to make players play the prox mine ticking sound
#define EF_TELEPORT_BIT 0x00000004    // toggled every time the origin abruptly changes
#define EF_AWARD_EXCELLENT 0x00000008 // draw an excellent sprite
#define EF_PLAYER_EVENT 0x00000010
#define EF_BOUNCE 0x00000010         // for missiles
#define EF_BOUNCE_HALF 0x00000020    // for missiles
#define EF_AWARD_GAUNTLET 0x00000040 // draw a gauntlet sprite
#define EF_NODRAW 0x00000080         // may have an event, but no model (unspawned items)
#define EF_FIRING 0x00000100         // for lightning gun
#define EF_KAMIKAZE 0x00000200
#define EF_MOVER_STOP 0x00000400       // will push otherwise
#define EF_AWARD_CAP 0x00000800        // draw the capture sprite
#define EF_TALK 0x00001000             // draw a talk balloon
#define EF_CONNECTION 0x00002000       // draw a connection trouble sprite
#define EF_VOTED 0x00004000            // already cast a vote
#define EF_AWARD_IMPRESSIVE 0x00008000 // draw an impressive sprite
#define EF_AWARD_DEFEND 0x00010000     // draw a defend sprite
#define EF_AWARD_ASSIST 0x00020000     // draw a assist sprite
#define EF_AWARD_DENIED 0x00040000     // denied
#define EF_TEAMVOTED 0x00080000        // already cast a team vote

// gentity->flags
#define FL_GODMODE 0x00000010
#define FL_NOTARGET 0x00000020
#define FL_TEAMSLAVE 0x00000400 // not the first on the team
#define FL_NO_KNOCKBACK 0x00000800
#define FL_DROPPED_ITEM 0x00001000
#define FL_NO_BOTS 0x00002000       // spawn point not for bot use
#define FL_NO_HUMANS 0x00004000     // spawn point just for bots
#define FL_FORCE_GESTURE 0x00008000 // force gesture on client

// Damage flags (dflags parameter to G_Damage)
// Verified from qagamex64.so build 1069.
#define DAMAGE_RADIUS 0x00000001
#define DAMAGE_NO_ARMOR 0x00000002
#define DAMAGE_NO_KNOCKBACK 0x00000004
#define DAMAGE_NO_PROTECTION 0x00000008
#define DAMAGE_NO_TEAM_PROTECTION 0x00000010 // [QL]

// angle helpers
#define ANGLE2SHORT(x) ((int)((x) * 65536 / 360) & 65535)
#define SHORT2ANGLE(x) ((x) * (360.0 / 65536))

// entity->svFlags
// the server does not know how to interpret most of the values
// in entityStates (level eType), so the game must explicitly flag
// special server behaviors
#define SVF_NOCLIENT 0x00000001 // don't send entity to clients, even if it has effects
#define SVF_CLIENTMASK 0x00000002
#define SVF_BOT 0x00000008                // set if the entity is a bot
#define SVF_BROADCAST 0x00000020          // send to all connected clients
#define SVF_PORTAL 0x00000040             // merge a second pvs at origin2 into snapshots
#define SVF_USE_CURRENT_ORIGIN 0x00000080 // entity->r.currentOrigin instead of entity->s.origin
                                          // for link position (missiles and movers)
#define SVF_SINGLECLIENT 0x00000100       // only send to a single client (entityShared_t->singleClient)
#define SVF_NOSERVERINFO 0x00000200       // don't send CS_SERVERINFO updates to this client
                                          // so that it can be updated for ping tools without
                                          // lagging clients
#define SVF_CAPSULE 0x00000400            // use capsule for collision detection instead of bbox
#define SVF_NOTSINGLECLIENT 0x00000800    // send entity to everyone but one client
                                          // (entityShared_t->singleClient)

// pmove->pm_flags
// Verified from qagamex64.so build 1069 decompilation.
#define PMF_DUCKED 0x0001
#define PMF_JUMP_HELD 0x0002
#define PMF_FROZEN 0x0004 // [QL] freeze tag frozen
#define PMF_BACKWARDS_JUMP 0x0008
#define PMF_BACKWARDS_RUN 0x0010
#define PMF_RESPAWNED 0x0020
#define PMF_TIME_KNOCKBACK 0x0040
#define PMF_TIME_WATERJUMP 0x0080
#define PMF_PAUSED 0x0100 // [QL] game is paused
#define PMF_ZOOM 0x0200   // [QL]
#define PMF_TIME_LAND 0x0400
#define PMF_PROMODE 0x0800 // [QL] CPM physics active
#define PMF_FOLLOW 0x1000
#define PMF_SCOREBOARD 0x2000
#define PMF_INVULEXPAND 0x4000
#define PMF_GRAPPLE_PULL 0x8000
#define PMF_AIR_CONTROL 0x10000           // [QL] air control enabled
#define PMF_DOUBLE_JUMP 0x20000           // [QL] double jump enabled
#define PMF_NO_AUTOHOP 0x40000            // [QL] auto-hop disabled for this client
#define PMF_ZOMBIE 0x80000                // [QL] Red Rover zombie state
#define PMF_CROUCH_SLIDE 0x100000         // [QL] crouch slide active
#define PMF_CROUCH_SLIDE_ENABLED 0x200000 // [QL] crouch slide feature enabled
#define PMF_CIRCLE_STRAFE 0x400000        // [QL] circle strafe detection
#define PMF_AUTOHOP_BLOCK 0x800000        // [QL] auto-hop blocked temporarily
#define PMF_USE_ITEM 0x1000000            // [QL] use item
#define PMF_LOADOUT_LOCKED 0x2000000      // [QL] loadout locked during round
#define PMF_ALL_TIMES (PMF_TIME_WATERJUMP | PMF_TIME_LAND | PMF_TIME_KNOCKBACK)

// Item numbers
#define MODELINDEX_ARMORSHARD 1
#define MODELINDEX_ARMORYELLOW 2
#define MODELINDEX_ARMORRED 3
#define MODELINDEX_ARMORGREEN 4
#define MODELINDEX_HEALTH5 5
#define MODELINDEX_HEALTH25 6
#define MODELINDEX_HEALTH50 7
#define MODELINDEX_HEALTHMEGA 8
#define MODELINDEX_GAUNTLET 9
#define MODELINDEX_SHOTGUN 10
#define MODELINDEX_MACHINEGUN 11
#define MODELINDEX_GRENADELAUNCHER 12
#define MODELINDEX_ROCKETLAUNCHER 13
#define MODELINDEX_LIGHTNING 14
#define MODELINDEX_RAILGUN 15
#define MODELINDEX_PLASMAGUN 16
#define MODELINDEX_BFG10K 17
#define MODELINDEX_GRAPPLINGHOOK 18
#define MODELINDEX_SHELLS 19
#define MODELINDEX_BULLETS 20
#define MODELINDEX_GRENADES 21
#define MODELINDEX_CELLS 22
#define MODELINDEX_LIGHTNINGAMMO 23
#define MODELINDEX_ROCKETS 24
#define MODELINDEX_SLUGS 25
#define MODELINDEX_BFGAMMO 26
#define MODELINDEX_TELEPORTER 27
#define MODELINDEX_MEDKIT 28
#define MODELINDEX_QUADDAMAGE 29
#define MODELINDEX_BATTLESUIT 30
#define MODELINDEX_HASTE 31
#define MODELINDEX_INVISIBILITY 32
#define MODELINDEX_REGENERATION 33
#define MODELINDEX_FLIGHT 34
#define MODELINDEX_REDFLAG 35
#define MODELINDEX_BLUEFLAG 36
#define MODELINDEX_KAMIKAZE 37
#define MODELINDEX_PORTAL 38
#define MODELINDEX_INVULNERABILITY 39
#define MODELINDEX_NAILS 40
#define MODELINDEX_MINES 41
#define MODELINDEX_BELT 42
#define MODELINDEX_SCOUT 43
#define MODELINDEX_GUARD 44
#define MODELINDEX_DAMAGE 45
#define MODELINDEX_AMMOREGEN 46
#define MODELINDEX_DOMINATIONPOINT 47
#define MODELINDEX_REDSKULL 48
#define MODELINDEX_BLUESKULL 49
#define MODELINDEX_NAILGUN 50
#define MODELINDEX_PROXLAUNCHER 51
#define MODELINDEX_CHAINGUN 52
#define MODELINDEX_SPAWNARMOR 53
#define MODELINDEX_HEAVYMACHINEGUN 54
#define MODELINDEX_HEAVYBULLETS 55
#define MODELINDEX_AMMOPACK 56
#define MODELINDEX_KEYSILVER 57
#define MODELINDEX_KEYGOLD 58
#define MODELINDEX_KEYMASTER 59 // the master key doesn't have a model in-game but functionally
                                // works as if the holder has both silver and gold keys


typedef enum { qfalse,
               qtrue } qboolean;
typedef unsigned char byte;

typedef struct gentity_s gentity_t;
typedef struct gclient_s gclient_t;

typedef float vec_t;
typedef vec_t vec2_t[2];
typedef vec_t vec3_t[3];
typedef vec_t vec4_t[4];
typedef vec_t vec5_t[5];

typedef int fileHandle_t;

// The permission levels used by QL's admin commands.
typedef enum {
    PRIV_BANNED = 0xFFFFFFFF,
    PRIV_NONE   = 0x0,
    PRIV_MOD    = 0x1,
    PRIV_ADMIN  = 0x2,
    PRIV_ROOT   = 0x3,
} privileges_t;

// Vote type. As opposed to in Q3, votes are counted every frame.
typedef enum {
    VOTE_NONE,
    VOTE_PENDING,
    VOTE_YES,
    VOTE_NO,
    VOTE_FORCE_PASS,
    VOTE_FORCE_FAIL,
    VOTE_EXPIRED
} voteState_t;

typedef enum {
    CS_FREE,      // can be reused for a new connection
    CS_ZOMBIE,    // client has been disconnected, but don't reuse
                  // connection for a couple seconds
    CS_CONNECTED, // has been assigned to a client_t, but no gamestate yet
    CS_PRIMED,    // gamestate has been sent, but client hasn't sent a usercmd
    CS_ACTIVE     // client is fully in game
} clientState_t;

typedef enum {
    PREGAME       = 0x0,
    ROUND_WARMUP  = 0x1,
    ROUND_SHUFFLE = 0x2,
    ROUND_BEGUN   = 0x3,
    ROUND_OVER    = 0x4,
    POSTGAME      = 0x5,
} roundStateState_t;

typedef enum {
    STAT_HEALTH,
    STAT_HOLDABLE_ITEM,
    STAT_RUNE,
    STAT_WEAPONS,
    STAT_ARMOR,
    STAT_BSKILL,
    STAT_CLIENTS_READY,
    STAT_MAX_HEALTH,
    STAT_SPINUP,
    STAT_FLIGHT_THRUST,
    STAT_MAX_FLIGHT_FUEL,
    STAT_CUR_FLIGHT_FUEL,
    STAT_FLIGHT_REFUEL,
    STAT_QUADKILLS,
    STAT_ARMORTYPE,
    STAT_KEY,
    STAT_OTHER_HEALTH,
    STAT_OTHER_ARMOR,
} statIndex_t;

// QL's vmMain dispatch table ordering - differs from Q3!
// Verified from qagamex64.so build 1069 vmMain jump table at 0x3cff00.
typedef enum {
    GAME_SHUTDOWN,                // 0  G_ShutdownGame(int restart)
    GAME_RUN_FRAME,               // 1  G_RunFrame(int levelTime)
    GAME_REGISTER_CVARS,          // 2  G_RegisterCvars(void)  [QL addition]
    GAME_INIT,                    // 3  G_InitGame(int levelTime, int randomSeed, int restart)
    GAME_CONSOLE_COMMAND,         // 4  ConsoleCommand(void)
    GAME_CLIENT_USERINFO_CHANGED, // 5  ClientUserinfoChanged(int clientNum)
    GAME_CLIENT_THINK,            // 6  ClientThink(int clientNum)
    GAME_CLIENT_DISCONNECT,       // 7  ClientDisconnect(int clientNum)
    GAME_CLIENT_CONNECT,          // 8  ClientConnect(int clientNum, qboolean firstTime, qboolean isBot)
    GAME_CLIENT_COMMAND,          // 9  ClientCommand(int clientNum)
} gameExport_t;

typedef enum {
    PM_NORMAL       = 0x0,
    PM_NOCLIP       = 0x1,
    PM_SPECTATOR    = 0x2,
    PM_DEAD         = 0x3,
    PM_FREEZE       = 0x4,
    PM_INTERMISSION = 0x5,
    PM_TUTORIAL     = 0x6,
} pmtype_t;

typedef enum {
    EV_NONE                    = 0x0,
    EV_FOOTSTEP                = 0x1,
    EV_FOOTSTEP_METAL          = 0x2,
    EV_FOOTSPLASH              = 0x3,
    EV_FOOTWADE                = 0x4,
    EV_SWIM                    = 0x5,
    EV_FALL_SHORT              = 0x6,
    EV_FALL_MEDIUM             = 0x7,
    EV_FALL_FAR                = 0x8,
    EV_JUMP_PAD                = 0x9,
    EV_JUMP                    = 0xA,
    EV_WATER_TOUCH             = 0xB,
    EV_WATER_LEAVE             = 0xC,
    EV_WATER_UNDER             = 0xD,
    EV_WATER_CLEAR             = 0xE,
    EV_ITEM_PICKUP             = 0xF,
    EV_GLOBAL_ITEM_PICKUP      = 0x10,
    EV_NOAMMO                  = 0x11,
    EV_CHANGE_WEAPON           = 0x12,
    EV_DROP_WEAPON             = 0x13,
    EV_FIRE_WEAPON             = 0x14,
    EV_USE_ITEM0               = 0x15,
    EV_USE_ITEM1               = 0x16,
    EV_USE_ITEM2               = 0x17,
    EV_USE_ITEM3               = 0x18,
    EV_USE_ITEM4               = 0x19,
    EV_USE_ITEM5               = 0x1A,
    EV_USE_ITEM6               = 0x1B,
    EV_USE_ITEM7               = 0x1C,
    EV_USE_ITEM8               = 0x1D,
    EV_USE_ITEM9               = 0x1E,
    EV_USE_ITEM10              = 0x1F,
    EV_USE_ITEM11              = 0x20,
    EV_USE_ITEM12              = 0x21,
    EV_USE_ITEM13              = 0x22,
    EV_USE_ITEM14              = 0x23,
    EV_USE_ITEM15              = 0x24,
    EV_ITEM_RESPAWN            = 0x25,
    EV_ITEM_POP                = 0x26,
    EV_PLAYER_TELEPORT_IN      = 0x27,
    EV_PLAYER_TELEPORT_OUT     = 0x28,
    EV_GRENADE_BOUNCE          = 0x29,
    EV_GENERAL_SOUND           = 0x2A,
    EV_GLOBAL_SOUND            = 0x2B,
    EV_GLOBAL_TEAM_SOUND       = 0x2C,
    EV_BULLET_HIT_FLESH        = 0x2D,
    EV_BULLET_HIT_WALL         = 0x2E,
    EV_MISSILE_HIT             = 0x2F,
    EV_MISSILE_MISS            = 0x30,
    EV_MISSILE_MISS_METAL      = 0x31,
    EV_RAILTRAIL               = 0x32,
    EV_SHOTGUN                 = 0x33,
    EV_BULLET                  = 0x34,
    EV_PAIN                    = 0x35,
    EV_DEATH1                  = 0x36,
    EV_DEATH2                  = 0x37,
    EV_DEATH3                  = 0x38,
    EV_DROWN                   = 0x39,
    EV_OBITUARY                = 0x3A,
    EV_POWERUP_QUAD            = 0x3B,
    EV_POWERUP_BATTLESUIT      = 0x3C,
    EV_POWERUP_REGEN           = 0x3D,
    EV_POWERUP_ARMORREGEN      = 0x3E,
    EV_GIB_PLAYER              = 0x3F,
    EV_SCOREPLUM               = 0x40,
    EV_PROXIMITY_MINE_STICK    = 0x41,
    EV_PROXIMITY_MINE_TRIGGER  = 0x42,
    EV_KAMIKAZE                = 0x43,
    EV_OBELISKEXPLODE          = 0x44,
    EV_OBELISKPAIN             = 0x45,
    EV_INVUL_IMPACT            = 0x46,
    EV_JUICED                  = 0x47,
    EV_LIGHTNINGBOLT           = 0x48,
    EV_DEBUG_LINE              = 0x49,
    EV_TAUNT                   = 0x4A,
    EV_TAUNT_YES               = 0x4B,
    EV_TAUNT_NO                = 0x4C,
    EV_TAUNT_FOLLOWME          = 0x4D,
    EV_TAUNT_GETFLAG           = 0x4E,
    EV_TAUNT_GUARDBASE         = 0x4F,
    EV_TAUNT_PATROL            = 0x50,
    EV_FOOTSTEP_SNOW           = 0x51,
    EV_FOOTSTEP_WOOD           = 0x52,
    EV_ITEM_PICKUP_SPEC        = 0x53,
    EV_OVERTIME                = 0x54,
    EV_GAMEOVER                = 0x55,
    EV_MISSILE_MISS_DMGTHROUGH = 0x56,
    EV_THAW_PLAYER             = 0x57,
    EV_THAW_TICK               = 0x58,
    EV_SHOTGUN_KILL            = 0x59,
    EV_POI                     = 0x5A,
    EV_DEBUG_HITBOX            = 0x5B,
    EV_LIGHTNING_DISCHARGE     = 0x5C,
    EV_RACE_START              = 0x5D,
    EV_RACE_CHECKPOINT         = 0x5E,
    EV_RACE_FINISH             = 0x5F,
    EV_DAMAGEPLUM              = 0x60,
    EV_AWARD                   = 0x61,
    EV_INFECTED                = 0x62,
    EV_NEW_HIGH_SCORE          = 0x63,
    EV_NUM_ETYPES              = 0x64,
} entity_event_t;

typedef enum {
    IT_BAD,
    IT_WEAPON,   // EFX: rotate + upscale + minlight
    IT_AMMO,     // EFX: rotate
    IT_ARMOR,    // EFX: rotate + minlight
    IT_HEALTH,   // EFX: static external sphere + rotating internal
    IT_POWERUP,  // instant on, timer based
                 // EFX: rotate + external ring that rotates
    IT_HOLDABLE, // single use, holdable item
                 // EFX: rotate + bob
    IT_PERSISTANT_POWERUP,
    IT_TEAM
} itemType_t;

typedef enum {
    PW_NONE            = 0x0,
    PW_SPAWNARMOR      = 0x0,
    PW_REDFLAG         = 0x1,
    PW_BLUEFLAG        = 0x2,
    PW_NEUTRALFLAG     = 0x3,
    PW_QUAD            = 0x4,
    PW_BATTLESUIT      = 0x5,
    PW_HASTE           = 0x6,
    PW_INVIS           = 0x7,
    PW_REGEN           = 0x8,
    PW_FLIGHT          = 0x9,
    PW_INVULNERABILITY = 0xA,
    NOTPW_SCOUT        = 0xB,
    NOTPW_GUARD        = 0xC,
    NOTPW_DOUBLER      = 0xD,
    NOTPW_ARMORREGEN   = 0xE,
    PW_FREEZE          = 0xF,
    PW_NUM_POWERUPS    = 0x10,
} powerup_t;

typedef enum {
    H_NONE        = 0x0,
    H_MEGA        = 0x1,
    H_LARGE       = 0x2,
    H_MEDIUM      = 0x3,
    H_SMALL       = 0x4,
    H_NUM_HEALTHS = 0x5,
} healthPickup_t;

typedef enum {
    HI_NONE            = 0x0,
    HI_TELEPORTER      = 0x1,
    HI_MEDKIT          = 0x2,
    HI_KAMIKAZE        = 0x3,
    HI_PORTAL          = 0x4,
    HI_INVULNERABILITY = 0x5,
    HI_FLIGHT          = 0x6,
    HI_NUM_HOLDABLE    = 0x7,
} holdable_t;

typedef enum {
    WP_NONE             = 0x0,
    WP_GAUNTLET         = 0x1,
    WP_MACHINEGUN       = 0x2,
    WP_SHOTGUN          = 0x3,
    WP_GRENADE_LAUNCHER = 0x4,
    WP_ROCKET_LAUNCHER  = 0x5,
    WP_LIGHTNING        = 0x6,
    WP_RAILGUN          = 0x7,
    WP_PLASMAGUN        = 0x8,
    WP_BFG              = 0x9,
    WP_GRAPPLING_HOOK   = 0xA,
    WP_NAILGUN          = 0xB,
    WP_PROX_LAUNCHER    = 0xC,
    WP_CHAINGUN         = 0xD,
    WP_HMG              = 0xE,
    WP_HANDS            = 0xF,
    WP_NUM_WEAPONS      = 0x10,
} weapon_t;

typedef enum {
    WEAPON_READY    = 0x0,
    WEAPON_RAISING  = 0x1,
    WEAPON_DROPPING = 0x2,
    WEAPON_FIRING   = 0x3,
} weaponstate_t;

typedef enum {
    RUNE_NONE       = 0x0,
    RUNE_SCOUT      = 0x1,
    RUNE_GUARD      = 0x2,
    RUNE_DAMAGE     = 0x3,
    RUNE_ARMORREGEN = 0x4,
    RUNE_MAX        = 0x5,
} rune_t;

typedef enum {
    KEY_SILVER = 0x0,
    KEY_GOLD   = 0x1,
    KEY_MASTER = 0x2,
} keys_t;

typedef enum {
    TEAM_BEGIN, // Beginning a team game, spawn at base
    TEAM_ACTIVE // Now actively playing
} playerTeamStateState_t;

typedef enum {
    TEAM_FREE,
    TEAM_RED,
    TEAM_BLUE,
    TEAM_SPECTATOR,

    TEAM_NUM_TEAMS
} team_t;

// https://github.com/brugal/wolfcamql/blob/73e2d707e5dd1fb0fc50d4ad9f00940909c4b3ec/code/game/bg_public.h#L1142-L1188
// means of death
typedef enum {
    MOD_UNKNOWN,
    MOD_SHOTGUN,
    MOD_GAUNTLET,
    MOD_MACHINEGUN,
    MOD_GRENADE,
    MOD_GRENADE_SPLASH,
    MOD_ROCKET,
    MOD_ROCKET_SPLASH,
    MOD_PLASMA,
    MOD_PLASMA_SPLASH,
    MOD_RAILGUN,
    MOD_LIGHTNING,
    MOD_BFG,
    MOD_BFG_SPLASH,
    MOD_WATER,
    MOD_SLIME,
    MOD_LAVA,
    MOD_CRUSH,
    MOD_TELEFRAG,
    MOD_FALLING,
    MOD_SUICIDE,
    MOD_TARGET_LASER,
    MOD_TRIGGER_HURT,
    MOD_NAIL,
    MOD_CHAINGUN,
    MOD_PROXIMITY_MINE,
    MOD_KAMIKAZE,
    MOD_JUICED,
    MOD_GRAPPLE,
    MOD_SWITCH_TEAMS,
    MOD_THAW,
    MOD_LIGHTNING_DISCHARGE,
    MOD_HMG,
    MOD_RAILGUN_HEADSHOT
} meansOfDeath_t;

typedef enum {
    SPECTATOR_NOT,
    SPECTATOR_FREE,
    SPECTATOR_FOLLOW,
    SPECTATOR_SCOREBOARD
} spectatorState_t;

typedef enum {
    CON_DISCONNECTED,
    CON_CONNECTING,
    CON_CONNECTED
} clientConnected_t;

// movers are things like doors, plats, buttons, etc
typedef enum {
    MOVER_POS1,
    MOVER_POS2,
    MOVER_1TO2,
    MOVER_2TO1
} moverState_t;

enum {
    PERS_ROUND_SCORE      = 0x0,
    PERS_COMBOKILL_COUNT  = 0x1,
    PERS_RAMPAGE_COUNT    = 0x2,
    PERS_MIDAIR_COUNT     = 0x3,
    PERS_REVENGE_COUNT    = 0x4,
    PERS_PERFORATED_COUNT = 0x5,
    PERS_HEADSHOT_COUNT   = 0x6,
    PERS_ACCURACY_COUNT   = 0x7,
    PERS_QUADGOD_COUNT    = 0x8,
    PERS_FIRSTFRAG_COUNT  = 0x9,
    PERS_PERFECT_COUNT    = 0xA,
};

enum cvar_flags {
    CVAR_ARCHIVE      = 1,    // Saves to config file, used for system variables
    CVAR_USERINFO     = 2,    // Sent to server on connect or modification
    CVAR_SERVERINFO   = 4,    // Sent in response to front-end requests
    CVAR_SYSTEMINFO   = 8,    // Sent to all clients when changed
    CVAR_INIT         = 16,   // Allow setting from the command-line only, never from console
    CVAR_LATCH        = 32,   // Save the latched value until the next restart
    CVAR_ROM          = 64,   // Display purposes only, cannot be set by the user
    CVAR_USER_CREATED = 128,  // Created as a result of the 'set' command
    CVAR_TEMP         = 256,  // Can be set when cheats are disabled, but is not archived
    CVAR_CHEAT        = 512,  // Can not be changed if cheats are disabled
    CVAR_NORESTART    = 1024, // Do not clear on a cvar_restart
    CVAR_REPLICATED   = 2048,
    CVAR_MINMAX       = 4096,
    CVAR_OPTIONS      = 8192,
    CVAR_UNKNOWN3     = 16384,
    CVAR_UNKNOWN4     = 32768,
    CVAR_UNKNOWN5     = 65536,
    CVAR_UNKNOWN6     = 131072,
    CVAR_UNKNOWN7     = 262144,
    CVAR_UNKNOWN8     = 524288,
    CVAR_GAMERULE     = 1048576
};

// paramters for command buffer stuffing
typedef enum {
    EXEC_NOW,    // don't return until completed, a VM should NEVER use this,
                 // because some commands might cause the VM to be unloaded...
    EXEC_INSERT, // insert at current position, but don't run yet
    EXEC_APPEND  // add to end of the command buffer (normal case)
} cbufExec_t;

// Mino: Quite different from Q3. Not sure on everything.
typedef struct cvar_s {
    char *name;
    char *string;
    char *resetString;   // cvar_restart will reset to this value
    char *latchedString; // for CVAR_LATCH vars
    char *defaultString;
    char *minimumString;
    char *maximumString;
    int flags;
    qboolean modified;
    uint8_t _unknown2[4];
    int modificationCount; // incremented each time the cvar is changed
    float value;           // atof( string )
    int integer;           // atoi( string )
    uint8_t _unknown3[8];
    struct cvar_s *next;
    struct cvar_s *hashNext;
} cvar_t;

typedef struct {
    qboolean allowoverflow; // if false, do a Com_Error
    qboolean overflowed;    // set to true if the buffer size failed (with allowoverflow set)
    qboolean oob;           // set to true if the buffer size failed (with allowoverflow set)
    byte *data;
    int maxsize;
    int cursize;
    int readcount;
    int bit; // for bitwise reads and writes
} msg_t;

typedef struct __attribute__((aligned(4))) usercmd_s {
    int serverTime;     // +0
    int angles[3];      // +4
    int buttons;        // +16
    byte weapon;        // +20
    byte weaponPrimary; // +21
    byte fov;           // +22
    char forwardmove;   // +23
    char rightmove;     // +24
    char upmove;        // +25
    byte doubleTap;     // +26  unknown purpose, possibly dodge/double-tap indicator
    byte _pad;          // +27
} usercmd_t;            // 28 bytes

typedef enum {
    NS_CLIENT,
    NS_SERVER
} netsrc_t;

typedef enum {
    NA_BOT,
    NA_BAD, // an address lookup failed
    NA_LOOPBACK,
    NA_BROADCAST,
    NA_IP,
    NA_IPX,
    NA_BROADCAST_IPX
} netadrtype_t;

typedef enum {
    TR_STATIONARY,
    TR_INTERPOLATE, // non-parametric, but interpolate between snapshots
    TR_LINEAR,
    TR_LINEAR_STOP,
    TR_SINE, // value = base + sin( time / duration ) * delta
    TR_GRAVITY,
    TR_CUSTOM_GRAVITY // [QL] uses trajectory_t.gravity field
} trType_t;

typedef struct {
    netadrtype_t type;

    byte ip[4];
    byte ipx[10];

    unsigned short port;
} netadr_t;

// netchan_t - 65,596 bytes. Verified from qzeroded.x64 build 1069.
// [QL] uses 32KB buffers (MAX_NETCHAN_MSGLEN) instead of Q3's 16KB (MAX_MSGLEN).
typedef struct {
    netsrc_t sock; // +0

    int dropped; // +4   between last packet and previous

    netadr_t remoteAddress; // +8   (20 bytes)
    int qport;              // +28  qport value to write when transmitting

    // sequencing variables
    int incomingSequence; // +32
    int outgoingSequence; // +36

    // incoming fragment assembly buffer
    int fragmentSequence;                    // +40
    int fragmentLength;                      // +44
    byte fragmentBuffer[MAX_NETCHAN_MSGLEN]; // +48   (32768 bytes)

    // outgoing fragment buffer
    // we need to space out the sending of large fragmented messages
    qboolean unsentFragments;              // +32816
    int unsentFragmentStart;               // +32820
    int unsentLength;                      // +32824
    byte unsentBuffer[MAX_NETCHAN_MSGLEN]; // +32828 (32768 bytes)
} netchan_t;                               // 65,596 bytes

typedef struct cplane_s {
    vec3_t normal;
    float dist;
    byte type;
    byte signbits;
    byte pad[2];
} cplane_t;

// a trace is returned when a box is swept through the world
typedef struct {
    qboolean allsolid;
    qboolean startsolid;
    float fraction;
    vec3_t endpos;
    cplane_t plane;
    int surfaceFlags;
    int contents;
    int entityNum;
} trace_t;

// playerState_t is a full superset of entityState_t as it is used by players,
// so if a playerState_t is transmitted, the entityState_t can be fully derived
// from it.
// Total size: 592 bytes. Verified from qagamex64.so build 1069.
typedef struct playerState_s {
    int commandTime;         // +0
    int pm_type;             // +4
    int bobCycle;            // +8
    int pm_flags;            // +12
    int pm_time;             // +16
    vec3_t origin;           // +20
    vec3_t velocity;         // +32
    int weaponTime;          // +44
    int gravity;             // +48
    int speed;               // +52
    int delta_angles[3];     // +56
    int groundEntityNum;     // +68
    int legsTimer;           // +72
    int legsAnim;            // +76
    int torsoTimer;          // +80
    int torsoAnim;           // +84
    int movementDir;         // +88
    vec3_t grapplePoint;     // +92
    int eFlags;              // +104
    int eventSequence;       // +108
    int events[2];           // +112
    int eventParms[2];       // +120
    int externalEvent;       // +128
    int externalEventParm;   // +132
    int clientNum;           // +136
    int location;            // +140  [QL]
    int weapon;              // +144
    int weaponPrimary;       // +148  [QL]
    int weaponstate;         // +152
    int fov;                 // +156  [QL]
    vec3_t viewangles;       // +160
    int viewheight;          // +172
    int damageEvent;         // +176
    int damageYaw;           // +180
    int damagePitch;         // +184
    int damageCount;         // +188
    int stats[16];           // +192
    int persistant[16];      // +256
    int powerups[16];        // +320
    int ammo[16];            // +384
    int generic1;            // +448
    int loopSound;           // +452
    int jumppad_ent;         // +456
    int jumpTime;            // +460  [QL]
    int doubleJumped;        // +464  [QL]
    int crouchTime;          // +468  [QL]
    int crouchSlideTime;     // +472  [QL]
    char forwardmove;        // +476  [QL]
    char rightmove;          // +477  [QL]
    char upmove;             // +478  [QL]
    char _pad;               // +479  alignment padding
    int ping;                // +480
    int pmove_framecount;    // +484
    int jumppad_frame;       // +488
    int entityEventSequence; // +492
    int freezetime;          // +496  [QL] freeze tag
    int thawtime;            // +500  [QL]
    int thawClientNum_valid; // +504  [QL]
    int thawClientNum;       // +508  [QL]
    int respawnTime;         // +512  [QL]
    int localPersistant[16]; // +516  [QL] (64 bytes)
    int roundDamage;         // +580  [QL]
    int roundShots;          // +584  [QL]
    int roundHits;           // +588  [QL]
} playerState_t;             // 592 bytes

typedef struct __attribute__((aligned(8))) {
    playerState_t *ps;
    usercmd_t cmd;
    int tracemask;
    int debugLevel;
    int noFootsteps;
    qboolean gauntletHit;
    int numtouch;
    int touchents[32];
    vec3_t mins;
    vec3_t maxs;
    int watertype;
    int waterlevel;
    float xyspeed;
    float stepHeight;
    int stepTime;
    void (*trace)(trace_t *, const vec_t *, const vec_t *, const vec_t *, const vec_t *, int, int);
    int (*pointcontents)(const vec_t *, int);
    qboolean hookEnemy;
} pmove_t;

typedef struct {
    int areabytes;
    byte areabits[MAX_MAP_AREA_BYTES]; // portalarea visibility bits
    playerState_t ps;
    int num_entities;
    int first_entity; // into the circular sv_packet_entities[]
                      // the entities MUST be in increasing state number
                      // order, otherwise the delta compression will fail
    int messageSent;  // time the message was transmitted
    int messageAcked; // time the message was acked
    int messageSize;  // used to rate drop packets
} clientSnapshot_t;

typedef struct netchan_buffer_s {
    msg_t msg;
    byte msgBuffer[MAX_NETCHAN_MSGLEN];
    struct netchan_buffer_s *next;
} netchan_buffer_t;

// trajectory_t - 40 bytes (NOT 36 as in Q3 - QL adds gravity field)
typedef struct {
    trType_t trType; // +0
    int trTime;      // +4
    int trDuration;  // +8
    vec3_t trBase;   // +12
    vec3_t trDelta;  // +24
    float gravity;   // +36  [QL] used by TR_CUSTOM_GRAVITY
} trajectory_t;      // 40 bytes

typedef struct entityState_s {
    int number;
    int eType;
    int eFlags;
    trajectory_t pos;
    trajectory_t apos;
    int time;
    int time2;
    vec3_t origin;
    vec3_t origin2;
    vec3_t angles;
    vec3_t angles2;
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
    int health;
    int armor;
    int weapon;
    int location;
    int legsAnim;
    int torsoAnim;
    int generic1;
    int jumpTime;
    int doubleJumped;
} entityState_t;

typedef struct {
    entityState_t s;
    qboolean linked;
    int linkcount;
    int svFlags;
    int singleClient;
    qboolean bmodel;
    vec3_t mins;
    vec3_t maxs;
    int contents;
    vec3_t absmin;
    vec3_t absmax;
    vec3_t currentOrigin;
    vec3_t currentAngles;
    int ownerNum;
} entityShared_t;

typedef struct {
    entityState_t s;  // communicated by server to clients
    entityShared_t r; // shared by both the server system and game
} sharedEntity_t;

// client_t - 158,328 bytes (x64). Verified from qzeroded.x64 build 1069.
// Layout reconstructed from SV_Status_f loop stride (0x26A78) and field
// access patterns in SV_DirectConnect, SV_DropClient, SV_SendClientGameState,
// SV_CheckTimeouts, SV_ClientEnterWorld, SV_Netchan_Transmit.
typedef struct client_s {
    clientState_t state;            // +0
    char userinfo[MAX_INFO_STRING]; // +4       (1024 bytes)

    char reliableCommands[MAX_RELIABLE_COMMANDS][MAX_STRING_CHARS]; // +1028 (64*1024=65536)
    int reliableSequence;                                           // +66564
    int reliableAcknowledge;                                        // +66568
    int reliableSent;                                               // +66572
    int messageAcknowledge;                                         // +66576

    int gamestateMessageNum; // +66580
    int challenge;           // +66584

    usercmd_t lastUsercmd;                          // +66588  (28 bytes)
    int lastMessageNum;                             // +66616
    int lastClientCommand;                          // +66620
    char lastClientCommandString[MAX_STRING_CHARS]; // +66624  (1024 bytes)
    sharedEntity_t *gentity;                        // +67648  (8 bytes, ptr)
    char name[MAX_NAME_LENGTH];                     // +67656  [QL] 40 bytes (Q3 used 32)

    // --- UDP download fields (vestigial in QL - completely unreferenced) ---
    char downloadName[MAX_QPATH];                       // +67696  (64 bytes)
    fileHandle_t download;                              // +67760
    int downloadSize;                                   // +67764
    int downloadCount;                                  // +67768
    int downloadClientBlock;                            // +67772
    int downloadCurrentBlock;                           // +67776
    int downloadXmitBlock;                              // +67780
    unsigned char *downloadBlocks[MAX_DOWNLOAD_WINDOW]; // +67784  (64 bytes on x64)
    int downloadBlockSize[MAX_DOWNLOAD_WINDOW];         // +67848  (32 bytes)
    qboolean downloadEOF;                               // +67880
    int downloadSendTime;                               // +67884

    // --- Dead space: unreferenced region, likely remnant of removed features ---
    uint8_t _dead_space[3972]; // +67888

    int deltaMessage;                       // +71860
    int nextReliableTime;                   // +71864
    float floodCount;                       // +71868  [QL] token-bucket flood counter
    int lastPacketTime;                     // +71872
    int lastConnectTime;                    // +71876
    int nextSnapshotTime;                   // +71880
    qboolean rateDelayed;                   // +71884
    int timeoutCount;                       // +71888
    clientSnapshot_t frames[PACKET_BACKUP]; // +71892  (32*648=20736)
    int ping;                               // +92628
    int rate;                               // +92632
    int snapshotMsec;                       // +92636
    int pureAuthentic;                      // +92640
    qboolean gotCP;                         // +92644
    netchan_t netchan;                      // +92648  (65596 bytes)
    // implicit 4-byte padding for pointer alignment    // +158244
    netchan_buffer_t *netchan_start_queue; // +158248 (8 bytes, ptr)
    netchan_buffer_t **netchan_end_queue;  // +158256 (8 bytes, ptr)

    // --- Post-netchan fields (QL additions) ---
    uint8_t _reserved[40]; // +158264 unreferenced
    int rageQuit;          // +158304 [QL] set by game module via syscall
    int serverIdSequence;  // +158308 [QL] 7-bit server ID tracking
    int oldServerTime;     // +158312 [QL] map_restart time adjustment
    int _padding;          // +158316
    uint64_t steam_id;     // +158320 [QL]
} client_t;                // 158,328 bytes (x64)

//
// SERVER
//

// challenge_t - 696 bytes (0x2B8 stride). Verified from qzeroded.x64 build 1069.
// [QL] adds Steam authentication fields after the Q3 base fields.
typedef struct {
    netadr_t adr;         // +0    (20 bytes)
    int challenge;        // +20
    int time;             // +24   time the last packet was sent to the authorize server
    int pingTime;         // +28   time the challenge response was sent to client
    int firstTime;        // +32   time the adr was first used, for authorize timeout checks
    qboolean connected;   // +36
    uint64_t steamKey;    // +40   [QL] Steam session key
    byte authToken[512];  // +48   [QL] Steam authentication token
    int authTokenLen;     // +560  [QL] length of authToken
    char wasRefused[132]; // +564  [QL] refusal reason string
} challenge_t;            // 696 bytes

// serverStatic_t - cleared only when the game dll changes.
// Verified from qzeroded.x64 build 1069.
typedef struct {
    qboolean initialized;                   // +0    sv_init has completed
    int time;                               // +4    will be strictly increasing across level changes
    int snapFlagServerBit;                  // +8    ^= SNAPFLAG_SERVERCOUNT every SV_SpawnServer()
    int _padding;                           // +12
    client_t *clients;                      // +16   [sv_maxclients->integer]
    int numSnapshotEntities;                // +24   sv_maxclients->integer*PACKET_BACKUP*MAX_PACKET_ENTITIES
    int nextSnapshotEntities;               // +28   next snapshotEntities to use
    entityState_t *snapshotEntities;        // +32   [numSnapshotEntities]
    challenge_t challenges[MAX_CHALLENGES]; // +40   (1024 * 696 = 712,704 bytes)
    netadr_t redirectAddress;               // +712744
    netadr_t authorizeAddress;              // +712764
} serverStatic_t;

typedef struct svEntity_s {
    struct worldSector_s *worldSector;
    struct svEntity_s *nextEntityInWorldSector;

    entityState_t baseline; // for delta compression of initial sighting
    int numClusters;        // if -1, use headnode instead
    int clusternums[MAX_ENT_CLUSTERS];
    int lastCluster; // if all the clusters don't fit in clusternums
    int areanum, areanum2;
    int snapshotCounter; // used to prevent double adding from portal views
} svEntity_t;

typedef struct worldSector_s {
    int axis; // -1 = leaf node
    float dist;
    struct worldSector_s *children[2];
    svEntity_t *entities;
} worldSector_t;

typedef enum {
    SS_DEAD,    // no map loaded
    SS_LOADING, // spawning level entities
    SS_GAME     // actively running
} serverState_t;

typedef struct {
    serverState_t state;
    qboolean restarting;   // if true, send configstring changes during SS_LOADING
    int serverId;          // changes each server start
    int restartedServerId; // serverId before a map_restart
    int checksumFeed;      // the feed key that we use to compute the pure checksum strings
    // https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=475
    // the serverId associated with the current checksumFeed (always <= serverId)
    int checksumFeedServerId;
    int snapshotCounter; // incremented for each snapshot built
    int timeResidual;    // <= 1000 / sv_frame->value
    int nextFrameTime;   // when time > nextFrameTime, process world
    struct cmodel_s *models[MAX_MODELS];
    char *configstrings[MAX_CONFIGSTRINGS];
    svEntity_t svEntities[MAX_GENTITIES];

    char *entityParsePoint; // used during game VM init

    // the game virtual machine will update these on init and changes
    sharedEntity_t *gentities;
    int gentitySize;
    int num_entities; // current number, <= MAX_GENTITIES

    playerState_t *gameClients;
    int gameClientSize; // will be > sizeof(playerState_t) due to game private data

    int restartTime;
} server_t;

// playerTeamState_t - 48 bytes, 12 fields.
// Verified from qagamex64.so build 1069.
typedef struct {
    playerTeamStateState_t state; // +0
    int captures;                 // +4
    int basedefense;              // +8
    int carrierdefense;           // +12
    int flagrecovery;             // +16
    int fragcarrier;              // +20
    int assists;                  // +24
    int flagruntime;              // +28  [QL]
    int flagrunrelays;            // +32  [QL]
    int lasthurtcarrier;          // +36
    int lastreturnedflag;         // +40
    int lastfraggedcarrier;       // +44
} playerTeamState_t;              // 48 bytes

// expandedStatObj_t - 812 bytes, 62 fields.
// Verified from qagamex64.so build 1069 (ClientSpawn copies 812 bytes exactly).
typedef struct {
    unsigned int statId;            // +0
    int lastThinkTime;              // +4
    int teamJoinTime;               // +8
    int totalPlayTime;              // +12
    int serverRank;                 // +16
    qboolean serverRankIsTied;      // +20
    int teamRank;                   // +24
    qboolean teamRankIsTied;        // +28
    int numKills;                   // +32
    int numDeaths;                  // +36
    int numSuicides;                // +40
    int numTeamKills;               // +44
    int numTeamKilled;              // +48
    int numWeaponKills[16];         // +52   (64 bytes)
    int numWeaponDeaths[16];        // +116  (64 bytes)
    int shotsFired[16];             // +180  (64 bytes)
    int shotsHit[16];               // +244  (64 bytes)
    int damageDealt[16];            // +308  (64 bytes)
    int damageTaken[16];            // +372  (64 bytes)
    int powerups[16];               // +436  (64 bytes)
    int holdablePickups[7];         // +500  (28 bytes)
    int weaponPickups[16];          // +528  (64 bytes)
    int weaponUsageTime[16];        // +592  (64 bytes)
    int numCaptures;                // +656
    int numAssists;                 // +660
    int numDefends;                 // +664
    int numHolyShits;               // +668
    int totalDamageDealt;           // +672
    int totalDamageTaken;           // +676
    int previousHealth;             // +680
    int previousArmor;              // +684
    int numAmmoPickups;             // +688
    int numFirstMegaHealthPickups;  // +692
    int numMegaHealthPickups;       // +696
    int megaHealthPickupTime;       // +700
    int numHealthPickups;           // +704
    int numFirstRedArmorPickups;    // +708
    int numRedArmorPickups;         // +712
    int redArmorPickupTime;         // +716
    int numFirstYellowArmorPickups; // +720
    int numYellowArmorPickups;      // +724
    int yellowArmorPickupTime;      // +728
    int numFirstGreenArmorPickups;  // +732
    int numGreenArmorPickups;       // +736
    int greenArmorPickupTime;       // +740
    int numQuadDamagePickups;       // +744
    int numQuadDamageKills;         // +748
    int numBattleSuitPickups;       // +752
    int numRegenerationPickups;     // +756
    int numHastePickups;            // +760
    int numInvisibilityPickups;     // +764
    int numRedFlagPickups;          // +768
    int numBlueFlagPickups;         // +772
    int numNeutralFlagPickups;      // +776
    int numMedkitPickups;           // +780
    int numArmorPickups;            // +784
    int numDenials;                 // +788
    int killStreak;                 // +792
    int maxKillStreak;              // +796
    int xp;                         // +800
    int domThreeFlagsTime;          // +804
    int numMidairShotgunKills;      // +808
} expandedStatObj_t;                // 812 bytes

// client data that stays across multiple respawns, but is cleared
// on each level change or team change at ClientBegin()
// Total size: 248 bytes. Verified from qagamex64.so build 1069.
typedef struct __attribute__((aligned(8))) {
    clientConnected_t connected;  // +0
    usercmd_t cmd;                // +4    (28 bytes)
    qboolean localClient;         // +32
    qboolean initialSpawn;        // +36
    qboolean predictItemPickup;   // +40
    char netname[40];             // +44
    char country[24];             // +84   [QL]
    uint64_t steamId;             // +112  [QL] (8 bytes, aligned)
    int maxHealth;                // +120
    int voteCount;                // +124
    voteState_t voteState;        // +128  [QL]
    int complaints;               // +132  [QL]
    int complaintClient;          // +136  [QL]
    int complaintEndTime;         // +140  [QL]
    int damageFromTeammates;      // +144  [QL]
    int damageToTeammates;        // +148  [QL]
    qboolean ready;               // +152  [QL]
    int autoaction;               // +156  [QL]
    int timeouts;                 // +160  [QL]
    int enterTime;                // +164
    playerTeamState_t teamState;  // +168  (48 bytes)
    int damageResidual;           // +216  [QL]
    int inactivityTime;           // +220
    int inactivityWarning;        // +224
    int lastUserinfoUpdate;       // +228  [QL]
    int userInfoFloodInfractions; // +232  [QL]
    int lastMapVoteTime;          // +236  [QL]
    int lastMapVoteIndex;         // +240  [QL]
} clientPersistant_t;             // 248 bytes (4 bytes tail padding)

// client data that stays across multiple levels or tournament restarts
// this is achieved by writing all the data to cvar strings at game shutdown
// time and reading them back at connection time.  Anything added here
// MUST be dealt with in G_InitSessionData() / G_ReadSessionData() / G_WriteSessionData()
// Total size: 56 bytes. Verified from qagamex64.so build 1069.
typedef struct {
    team_t sessionTeam;              // +0
    int spectatorTime;               // +4
    spectatorState_t spectatorState; // +8
    int spectatorClient;             // +12
    int weaponPrimary;               // +16   [QL]
    int wins;                        // +20
    int losses;                      // +24
    qboolean teamLeader;             // +28
    privileges_t privileges;         // +32   [QL]
    int specOnly;                    // +36   [QL]
    int playQueue;                   // +40   [QL]
    qboolean updatePlayQueue;        // +44   [QL]
    int muted;                       // +48   [QL]
    int prevScore;                   // +52   [QL]
} clientSession_t;                   // 56 bytes

typedef struct gitem_s {
    char *classname;
    const char *pickup_sound;
    const char *world_model[4];
    const char *premium_model[4];
    const char *icon;
    const char *pickup_name;
    int quantity;
    itemType_t giType;
    int giTag;
    qboolean itemTimer;
    unsigned int maskGametypeRenderSkip;
    unsigned int maskGametypeForceSpawn;
} gitem_t;

typedef enum {
    ET_GENERAL,
    ET_PLAYER,
    ET_ITEM,
    ET_MISSILE,
    ET_MOVER,
    ET_BEAM,
    ET_PORTAL,
    ET_SPEAKER,
    ET_PUSH_TRIGGER,
    ET_TELEPORT_TRIGGER,
    ET_INVISIBLE,
    ET_GRAPPLE, // grapple hooked on wall
    ET_TEAM,

    ET_EVENTS // any of the EV_* events can be added freestanding
              // by setting eType to ET_EVENTS + eventNum
              // this avoids having to set eFlags and eventNum
} entityType_t;

struct gclient_s;

struct gentity_s {
    entityState_t s;
    entityShared_t r;
    struct gclient_s *client;
    qboolean inuse;
    char *classname;
    int spawnflags;
    qboolean neverFree;
    int flags;
    char *model;
    char *model2;
    int freetime;
    int eventTime;
    qboolean freeAfterEvent;
    qboolean unlinkAfterEvent;
    qboolean physicsObject;
    float physicsBounce;
    int clipmask;
    moverState_t moverState;
    int soundPos1;
    int sound1to2;
    int sound2to1;
    int soundPos2;
    int soundLoop;
    gentity_t *parent;
    gentity_t *nextTrain;
    gentity_t *prevTrain;
    vec3_t pos1;
    vec3_t pos2;
    char *message;
    char *cvar;
    char *tourPointTarget;
    char *tourPointTargetName;
    char *noise;
    int timestamp;
    float angle;
    char *target;
    char *targetname;
    char *targetShaderName;
    char *targetShaderNewName;
    gentity_t *target_ent;
    float speed;
    vec3_t movedir;
    int nextthink;
    void (*think)(gentity_t *);
    void (*framethink)(gentity_t *);
    void (*reached)(gentity_t *);
    void (*blocked)(gentity_t *, gentity_t *);
    void (*touch)(gentity_t *, gentity_t *);
    void (*use)(gentity_t *, gentity_t *, gentity_t *);
    void (*pain)(gentity_t *, gentity_t *, int);
    void (*die)(gentity_t *, gentity_t *, gentity_t *, int, int);
    int pain_debounce_time;
    int fly_sound_debounce_time;
    int health;
    qboolean takedamage;
    int damage;
    int damageFactor;
    int splashDamage;
    int splashRadius;
    int methodOfDeath;
    int splashMethodOfDeath;
    int count;
    gentity_t *enemy;
    gentity_t *activator;
    const char *team;
    gentity_t *teammaster;
    gentity_t *teamchain;
    int kamikazeTime;
    int kamikazeShockTime;
    int watertype;
    int waterlevel;
    int noise_index;
    int bouncecount;
    float wait;
    float random;
    int spawnTime;
    const gitem_t *item;
    int pickupCount;
};

// raceInfo_t - 552 bytes. Verified from qagamex64.so build 1069.
typedef struct {
    qboolean racingActive; // +0
    int startTime;         // +4
    int lastTime;          // +8
    int best_race[64];     // +12   (256 bytes)
    int current_race[64];  // +268  (256 bytes)
    int currentCheckPoint; // +524
    qboolean weaponUsed;   // +528
    // 4 bytes implicit padding for pointer alignment
    gentity_t *nextRacePoint;  // +536
    gentity_t *nextRacePoint2; // +544
} raceInfo_t;                  // 552 bytes

// this structure is cleared on each ClientSpawn() via memset(client, 0, 0xBF8),
// except for 'client->pers' and 'client->sess' which are saved/restored.
// Total size: 3064 bytes (0xBF8). Verified from qagamex64.so build 1069.
struct __attribute__((aligned(8))) gclient_s {
    playerState_t ps;          // +0     (592 bytes)
    clientPersistant_t pers;   // +592   (248 bytes)
    clientSession_t sess;      // +840   (56 bytes)
    qboolean noclip;           // +896
    int lastCmdTime;           // +900
    int buttons;               // +904
    int oldbuttons;            // +908
    int damage_armor;          // +912
    int damage_blood;          // +916
    vec3_t damage_from;        // +920
    qboolean damage_fromWorld; // +932
    int impressiveCount;       // +936
    int accuracyCount;         // +940
    int accuracy_shots;        // +944
    int accuracy_hits;         // +948
    int lastClientKilled;      // +952
    int lastKilledClient;      // +956
    int lastHurtClient[2];     // +960
    int lastHurtMod[2];        // +968
    int lastHurtTime[2];       // +976
    int lastKillTime;          // +984
    int lastGibTime;           // +988
    int rampageCounter;        // +992
    int revengeCounter[64];    // +996   (256 bytes)
    int respawnTime;           // +1252
    int rewardTime;            // +1256
    int airOutTime;            // +1260
    qboolean fireHeld;         // +1264
    // 4 bytes implicit padding for pointer alignment
    gentity_t *hook;            // +1272
    int switchTeamTime;         // +1280
    int timeResidual;           // +1284
    int timeResidualScout;      // +1288
    int timeResidualArmor;      // +1292
    int timeResidualHealth;     // +1296
    int timeResidualPingPOI;    // +1300
    int timeResidualSpecInfo;   // +1304
    qboolean healthRegenActive; // +1308
    qboolean armorRegenActive;  // +1312
    // 4 bytes implicit padding for pointer alignment
    gentity_t *persistantPowerup;    // +1320
    int portalID;                    // +1328
    int ammoTimes[16];               // +1332  (64 bytes)
    int invulnerabilityTime;         // +1396
    expandedStatObj_t expandedStats; // +1400  (812 bytes)
    int ignoreChatsTime;             // +2212
    int lastUserCmdTime;             // +2216
    qboolean freezePlayer;           // +2220
    int deferredSpawnTime;           // +2224
    int deferredSpawnCount;          // +2228
    raceInfo_t race;                 // +2232  (552 bytes)
    int shotgunDmg[64];              // +2784  (256 bytes) per-client SG damage accumulator for damage plums
    int round_shots;                 // +3040
    int round_hits;                  // +3044
    int round_damage;                // +3048
    qboolean queuedSpectatorFollow;  // +3052
    int queuedSpectatorClient;       // +3056
    // 4 bytes tail padding (struct alignment to 8-byte boundary)
}; // 3064 bytes total

typedef struct {
    roundStateState_t eCurrent;
    roundStateState_t eNext;
    int tNext;
    int startTime;
    int turn;
    int round;
    team_t prevRoundWinningTeam;
    qboolean touch;
    qboolean capture;
} roundState_t;

typedef struct __attribute__((aligned(8))) {
    struct gclient_s *clients;
    struct gentity_s *gentities;
    int gentitySize;
    int num_entities;
    int warmupTime;
    fileHandle_t logFile;
    int maxclients;
    int time;
    int frametime;
    int startTime;
    int teamScores[4];
    int nextTeamInfoTime;
    qboolean newSession;
    qboolean restarted;
    qboolean shufflePending;
    int shuffleReadyTime;
    int numConnectedClients;
    int numNonSpectatorClients;
    int numPlayingClients;
    int numReadyClients;
    int numReadyHumans;
    int numStandardClients;
    int sortedClients[64];
    int follow1;
    int follow2;
    int snd_fry;
    int warmupModificationCount;
    char voteString[1024];
    char voteDisplayString[1024];
    int voteExecuteTime;
    int voteTime;
    int voteYes;
    int voteNo;
    int pendingVoteCaller;
    qboolean spawning;
    int numSpawnVars;
    char *spawnVars[64][2];
    int numSpawnVarChars;
    char spawnVarChars[4096];
    int intermissionQueued;
    int intermissionTime;
    qboolean readyToExit;
    qboolean votingEnded;
    int exitTime;
    vec3_t intermission_origin;
    vec3_t intermission_angle;
    qboolean locationLinked;
    gentity_t *locationHead;
    int timePauseBegin;
    int timeOvertime;
    int timeInitialPowerupSpawn;
    int bodyQueIndex;
    gentity_t *bodyQue[8];
    int portalSequence;
    qboolean gameStatsReported;
    qboolean mapIsTrainingMap;
    int clientNum1stPlayer;
    int clientNum2ndPlayer;
    char scoreboardArchive1[1024];
    char scoreboardArchive2[1024];
    char firstScorer[40];
    char lastScorer[40];
    char lastTeamScorer[40];
    char firstFrag[40];
    vec3_t red_flag_origin;
    vec3_t blue_flag_origin;
    int spawnCount[4];
    int runeSpawns[5];
    int itemCount[60];
    int suddenDeathRespawnDelay;
    int suddenDeathRespawnDelayLastAnnounced;
    int numRedArmorPickups[4];
    int numYellowArmorPickups[4];
    int numGreenArmorPickups[4];
    int numMegaHealthPickups[4];
    int numQuadDamagePickups[4];
    int numBattleSuitPickups[4];
    int numRegenerationPickups[4];
    int numHastePickups[4];
    int numInvisibilityPickups[4];
    int quadDamagePossessionTime[4];
    int battleSuitPossessionTime[4];
    int regenerationPossessionTime[4];
    int hastePossessionTime[4];
    int invisibilityPossessionTime[4];
    int numFlagPickups[4];
    int numMedkitPickups[4];
    int flagPossessionTime[4];
    gentity_t *dominationPoints[5];
    int dominationPointCount;
    int dominationPointsTallied;
    int racePointCount;
    qboolean disableDropWeapon;
    qboolean teamShuffleActive;
    int lastTeamScores[4];
    int lastTeamRoundScores[4];
    team_t attackingTeam;
    roundState_t roundState;
    int lastTeamCountSent;
    int infectedConscript;
    int lastZombieSurvivor;
    int zombieScoreTime;
    int lastInfectionTime;
    char intermissionMapNames[3][1024];
    char intermissionMapTitles[3][1024];
    char intermissionMapConfigs[3][1024];
    int intermissionMapVotes[3];
    qboolean matchForfeited;
    int allReadyTime;
    qboolean notifyCvarChange;
    int notifyCvarChangeTime;
    int lastLeadChangeTime;
    int lastLeadChangeClient;
    int lastLeadChangeElapsedTime;
    int rrInfectedCounters[4]; // [QL] Red Rover infection tracking per-team
    int rrInfectedSpreadStart; // [QL] time when infection spread begins
    int rrSurvivorScoreTimer;  // [QL] timer for survivor score awarding
} level_locals_t;

// Some extra stuff that's not in the Q3 source. These are the commands you
// get when you type ? in the console. The array has a sentinel struct, so
// check "cmd" == NULL.
typedef struct {
    privileges_t needed_privileges;
    int unknown1;
    char *cmd; // The command name, e.g. "tempban".
    void (*admin_func)(gentity_t *ent);
    int unknown2;
    int unknown3;
    char *description; // Command description that gets printed when you do "?".
} adminCmd_t;

// A pointer to the qagame module in memory and its entry point.
extern void *qagame;
extern void *qagame_dllentry;

// Additional key struct pointers.
extern serverStatic_t *svs;
extern gentity_t *g_entities;
extern level_locals_t *level;
extern gitem_t *bg_itemlist;
extern int bg_numItems;
// Cvars.
extern cvar_t *sv_maxclients;

// Internal QL function pointer types.
typedef void(__cdecl *Com_Printf_ptr)(char *fmt, ...);
typedef void(__cdecl *Cmd_AddCommand_ptr)(char *cmd, void *func);
typedef char *(__cdecl *Cmd_Args_ptr)(void);
typedef char *(__cdecl *Cmd_Argv_ptr)(int arg);
typedef int(__cdecl *Cmd_Argc_ptr)(void);
typedef void(__cdecl *Cmd_TokenizeString_ptr)(const char *text_in);
typedef void(__cdecl *Cbuf_ExecuteText_ptr)(int exec_when, const char *text);
typedef cvar_t *(__cdecl *Cvar_FindVar_ptr)(const char *var_name);
typedef cvar_t *(__cdecl *Cvar_Get_ptr)(const char *var_name, const char *var_value, int flags);
typedef cvar_t *(__cdecl *Cvar_GetLimit_ptr)(const char *var_name, const char *var_value, const char *min, const char *max, int flag);
typedef cvar_t *(__cdecl *Cvar_Set2_ptr)(const char *var_name, const char *value, qboolean force);
typedef void(__cdecl *SV_SendServerCommand_ptr)(client_t *cl, const char *fmt, ...);
typedef void(__cdecl *SV_ExecuteClientCommand_ptr)(client_t *cl, const char *s, qboolean clientOK);
typedef void(__cdecl *SV_ClientEnterWorld_ptr)(client_t *client, usercmd_t *cmd);
typedef void(__cdecl *SV_Shutdown_ptr)(char *finalmsg);
typedef void(__cdecl *SV_Map_f_ptr)(void);
typedef void(__cdecl *SV_ClientThink_ptr)(client_t *cl, usercmd_t *cmd);
typedef void(__cdecl *SV_SetConfigstring_ptr)(int index, const char *value);
typedef void(__cdecl *SV_GetConfigstring_ptr)(int index, char *buffer, int bufferSize);
typedef void(__cdecl *SV_DropClient_ptr)(client_t *drop, const char *reason);
typedef void(__cdecl *FS_Startup_ptr)(const char *gameName);
typedef void(__cdecl *Sys_SetModuleOffset_ptr)(char *moduleName, void *offset);
typedef void(__cdecl *SV_LinkEntity_ptr)(sharedEntity_t *gEnt);
typedef void(__cdecl *SV_SpawnServer_ptr)(char *server, qboolean killBots);
typedef void(__cdecl *Cmd_ExecuteString_ptr)(const char *text);
typedef int(__cdecl *idSteamServer_DownloadItem_ptr)(uint64_t workshopId, qboolean defer);

// VM functions.
typedef void(__cdecl *G_RunFrame_ptr)(int time);
typedef void(__cdecl *G_AddEvent_ptr)(gentity_t *ent, int event, int eventParm);
typedef void(__cdecl *G_InitGame_ptr)(int levelTime, int randomSeed, int restart);
typedef int(__cdecl *CheckPrivileges_ptr)(gentity_t *ent, char *cmd);
typedef char *(__cdecl *ClientConnect_ptr)(int clientNum, qboolean firstTime, qboolean isBot);
typedef void(__cdecl *ClientSpawn_ptr)(gentity_t *ent);
typedef void(__cdecl *Cmd_CallVote_f_ptr)(gentity_t *ent);
typedef void(__cdecl *G_Damage_ptr)(gentity_t *targ, gentity_t *inflictor, gentity_t *attacker, vec3_t dir, vec3_t point, int damage, int dflags, int mod);
typedef void(__cdecl *Touch_Item_ptr)(gentity_t *ent, gentity_t *other, trace_t *trace);
typedef gentity_t *(__cdecl *LaunchItem_ptr)(gitem_t *item, vec3_t origin, vec3_t velocity);
typedef gentity_t *(__cdecl *Drop_Item_ptr)(gentity_t *ent, gitem_t *item, float angle);
typedef void(__cdecl *G_StartKamikaze_ptr)(gentity_t *ent);
typedef void(__cdecl *G_FreeEntity_ptr)(gentity_t *ed);
typedef qboolean(__cdecl *Sys_IsLANAddress_ptr)(netadr_t adr);


// Some of them are initialized by Initialize(), but not all of them necessarily.
extern Com_Printf_ptr Com_Printf;
extern Cmd_AddCommand_ptr Cmd_AddCommand;
extern Cmd_Args_ptr Cmd_Args;
extern Cmd_Argv_ptr Cmd_Argv;
extern Cmd_Argc_ptr Cmd_Argc;
extern Cmd_TokenizeString_ptr Cmd_TokenizeString;
extern Cbuf_ExecuteText_ptr Cbuf_ExecuteText;
extern Cvar_FindVar_ptr Cvar_FindVar;
extern Cvar_Get_ptr Cvar_Get;
extern Cvar_GetLimit_ptr Cvar_GetLimit;
extern Cvar_Set2_ptr Cvar_Set2;
extern SV_SendServerCommand_ptr SV_SendServerCommand;
extern SV_ExecuteClientCommand_ptr SV_ExecuteClientCommand;
extern SV_ClientEnterWorld_ptr SV_ClientEnterWorld;
extern SV_Shutdown_ptr SV_Shutdown; // Used to get svs pointer.
extern SV_Map_f_ptr SV_Map_f;       // Used to get Cmd_Argc
extern SV_SetConfigstring_ptr SV_SetConfigstring;
extern SV_GetConfigstring_ptr SV_GetConfigstring;
extern SV_DropClient_ptr SV_DropClient;
extern Sys_SetModuleOffset_ptr Sys_SetModuleOffset;
extern SV_SpawnServer_ptr SV_SpawnServer;
extern Cmd_ExecuteString_ptr Cmd_ExecuteString;
extern Sys_IsLANAddress_ptr Sys_IsLANAddress;
extern idSteamServer_DownloadItem_ptr idSteamServer_DownloadItem;

// VM functions.
extern G_RunFrame_ptr G_RunFrame;
extern G_AddEvent_ptr G_AddEvent;
extern G_InitGame_ptr G_InitGame;
extern CheckPrivileges_ptr CheckPrivileges;
extern ClientConnect_ptr ClientConnect;
extern ClientSpawn_ptr ClientSpawn;
extern Cmd_CallVote_f_ptr Cmd_CallVote_f;
extern G_Damage_ptr G_Damage;
extern Touch_Item_ptr Touch_Item;
extern LaunchItem_ptr LaunchItem;
extern Drop_Item_ptr Drop_Item;
extern G_StartKamikaze_ptr G_StartKamikaze;
extern G_FreeEntity_ptr G_FreeEntity;

// Server replacement functions for hooks.
void __cdecl My_Cmd_AddCommand(char *cmd, void *func);
void __cdecl My_Sys_SetModuleOffset(char *moduleName, void *offset);
#ifndef NOPY
void __cdecl My_SV_ExecuteClientCommand(client_t *cl, char *s, qboolean clientOK);
void __cdecl My_SV_SendServerCommand(client_t *cl, char *fmt, ...);
void __cdecl My_SV_ClientEnterWorld(client_t *client, usercmd_t *cmd);
void __cdecl My_SV_SetConfigstring(int index, char *value);
void __cdecl My_SV_DropClient(client_t *drop, const char *reason);
void __cdecl My_Com_Printf(char *fmt, ...);
void __cdecl My_SV_SpawnServer(char *server, qboolean killBots);
// VM replacement functions for hooks.
void __cdecl My_G_RunFrame(int time);
void __cdecl My_G_InitGame(int levelTime, int randomSeed, int restart);
char *__cdecl My_ClientConnect(int clientNum, qboolean firstTime, qboolean isBot);
void __cdecl My_ClientSpawn(gentity_t *ent);

void __cdecl My_G_StartKamikaze(gentity_t *ent);
#endif

// Custom commands added using Cmd_AddCommand during initialization.
void __cdecl SendServerCommand(void);    // "cmd"
void __cdecl CenterPrint(void);          // "cp"
void __cdecl RegularPrint(void);         // "p"
void __cdecl Slap(void);                 // "slap"
void __cdecl Slay(void);                 // "slay"
void __cdecl DownloadWorkshopItem(void); // "steam_downloadugcdefer"
void __cdecl StopFollowing(void);        // "stopfollowing"
#ifndef NOPY
// PyRcon gives the owner the ability to execute pyminqlxtended commands as if the
// owner executed them.
void __cdecl PyRcon(void);
// PyCommand is special. It'll serve as the handler for console commands added
// using Python. This means it can serve as the handler for a bunch of commands,
// and it'll take care of redirecting it to Python.
void __cdecl PyCommand(void);
void __cdecl RestartPython(void); // "pyrestart"
#endif

#endif /* QUAKE_COMMON_H */
