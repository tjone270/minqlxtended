# minqlxtended - Extends Quake Live's dedicated server with extra functionality and scripting.
# Copyright (C) 2015 Mino <mino@minomino.org>
# Copyright (C) 2022-2026 Thomas Jones <me@thomasjones.id.au>

# This file is part of minqlxtended.

# minqlxtended is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# minqlxtended is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with minqlxtended. If not, see <http://www.gnu.org/licenses/>.

"""minqlxtended - extends Quake Live's dedicated server with extra functionality and scripting.

Everything a plugin needs is reachable from this one namespace: ``minqlxtended.Plugin``,
``minqlxtended.Player``, ``minqlxtended.hook``, the constant enums (``Return``, ``Priority``,
``Team``, ``Weapon`` and the rest of :mod:`minqlxtended._enums`), and the live engine views
(``minqlxtended.level``, ``Entity``, ``GameClient``, ``Cvar``).

The ``handle_*`` C-ABI functions, the concrete dispatcher classes and the configstring
cache internals aren't published here. Import them from the submodule that owns them if
you really want one.
"""

import sys as _sys

if _sys.version_info < (3, 12):
    _running = ".".join(str(part) for part in _sys.version_info[:3])
    raise RuntimeError(
        f"minqlxtended requires Python 3.12 or later; this is {_running}. It links libpython "
        "directly, so rebuild against the interpreter you mean to run: "
        "`make PYTHON=python3.12`.")

import _minqlxtended
import re as _re

__version__ = _minqlxtended.__version__

_match = _re.search(r"([0-9]+)\.([0-9]+)\.([0-9]+)", __version__)
__version_info__ = tuple(int(part) for part in _match.groups()) if _match else (999, 999, 999)

# Generated. Run `python3 tools/gen_stub.py` after touching the method table, the struct
# sequence descs, the PyModule_AddIntMacro block or engine_fields.h; CI runs it with
# --check. Do not edit between the markers.
# --- BEGIN GENERATED ENGINE IMPORTS (tools/gen_stub.py) ---
from _minqlxtended import (  # noqa: F401
    # Functions.
    add_console_command, add_event, callvote, client_command, console_command, console_print,
    cvar, cvars, demo_status, destroy_kamikaze_timers, dev_print_items, drop_holdable,
    drop_item, entities, force_vote, force_weapon_respawn_time, get_cvar, get_userinfo,
    items, kick, link_entity, player_expanded_stats, player_info, player_spawn, player_state,
    player_stats, players_info, register_handler, reliable_status, remove_dropped_items,
    remove_entity, replace_items, reset_player_weapon_stats, send_server_command,
    set_configstring, set_cvar, set_cvar_limit, slay_with_mod, spawn_entity, spawn_item,
    start_demo, stop_demo, unlink_entity,
    # Struct sequences. Snapshots, taken when you ask for them.
    DemoStatus, Flight, Keys, PlayerExpandedStats, PlayerInfo, PlayerState, PlayerStats,
    Powerups, ReliableStatus, StatHoldables, StatPowerups, Vector3, Weapons,
    # Live engine views, and the singletons among them.
    Client, Cvar, Entity, EntityShared, EntityState, ExpandedStats, GameClient, IntArray,
    Item, Level, MatchState, Netchan, Persistant, PlayerStateView, RaceInfo, RoundStateView,
    Server, ServerStatic, Session, TeamState, level, match_state, server, server_static,
    # Errors.
    EngineStateError,
    # Constants the enums in _enums.py do not supersede: the configstring
    # indices, which are two families under one prefix, and the MAX_* bounds.
    CS_ADVERT_DELAY, CS_AD_SCORES, CS_ALLREADY_TIME, CS_ARMORINFO, CS_ATMOSEFFECT, CS_AUTHOR,
    CS_AUTHOR2, CS_BEST_ITEMCONTROL_PLYR, CS_BLUETEAMBASE, CS_BOTINFO, CS_CLIENTNUM1STPLAYER,
    CS_CLIENTNUM2NDPLAYER, CS_CUSTOM_SETTINGS, CS_DEBUGFLAGS, CS_DISABLE_LOADOUT,
    CS_DISABLE_VOTE_UI, CS_DMGTHROUGHDEPTH, CS_ENABLEBREATH, CS_FLAGSTATUS, CS_FREECAM,
    CS_GAME_VERSION, CS_GENERIC_COUNT_BLUE, CS_GENERIC_COUNT_RED,
    CS_INFECTED_SURVIVOR_MINSPEED, CS_INTERMISSION, CS_ITEMS, CS_LAST_GENERIC,
    CS_LEVEL_START_TIME, CS_LOCATIONS, CS_MATCH_GUID, CS_MAX, CS_MESSAGE, CS_MODELS,
    CS_MODEL_OVERRIDE, CS_MOST_ACCURATE_PLYR, CS_MOST_DAMAGEDEALT_PLYR,
    CS_MOST_VALUABLE_DEFENSIVE_PLYR, CS_MOST_VALUABLE_OFFENSIVE_PLYR, CS_MOST_VALUABLE_PLYR,
    CS_MOTD, CS_MUSIC, CS_NEXTMAP, CS_PAUSE_END_TIME, CS_PAUSE_START_TIME, CS_PLAYERINFO,
    CS_PLAYERS, CS_PLAYER_CYLINDERS, CS_PMOVEINFO, CS_PRACTICE, CS_RACE_POINTS,
    CS_REDTEAMBASE, CS_ROTATIONMAPS, CS_ROTATIONVOTES, CS_ROUND_START_TIME, CS_ROUND_WARMUP,
    CS_ROUND_WINNER, CS_SCORE1STPLAYER, CS_SCORE2NDPLAYER, CS_SCORES1, CS_SCORES1PLAYER,
    CS_SCORES2, CS_SCORES2PLAYER, CS_SERVERINFO, CS_SHADERSTATE, CS_SOUNDS,
    CS_STARTING_WEAPONS, CS_STEAM_ID, CS_STEAM_WORKSHOP_IDS, CS_SYSTEMINFO,
    CS_TEAMCOUNT_BLUE, CS_TEAMCOUNT_RED, CS_TIMEOUTS_BLUE, CS_TIMEOUTS_RED, CS_VOTE_NO,
    CS_VOTE_STRING, CS_VOTE_TIME, CS_VOTE_YES, CS_WARMUP, CS_WEAPONINFO, MAX_CLIENTS,
    MAX_CONFIGSTRINGS, MAX_GENTITIES,
)
# --- END GENERATED ENGINE IMPORTS ---

# tools/gen_stub.py reads its OWNED_PREFIXES, OWNED_NAMES and is_owned() to work out what
# the import above may contain, so keep those three at `minqlxtended._enums`.
from ._enums import (  # noqa: F401
    ConnectionState, CvarFlag, DamageFlag, DemoRequest, EntityEffect, EntityEvent,
    EntityFlag, EntityType, GameState, Gametype, Holdable, Key, MeansOfDeath, Mod,
    ModelIndex, MoverState, Objective, PersistantIndex, Powerup, Priority, Privilege,
    Return, RoundState, SayMode, ServerFlag, ServerState, StatIndex, Team, TrajectoryType,
    Weapon,
)

from ._core import (  # noqa: F401
    DEFAULT_PLUGINS, MIN_SWITCH_INTERVAL, MapTitles, SPAWN_POINT_CLASSNAMES,
    PluginLoadError, PluginUnloadError, TimerHandle,
    command, delay, format_infostring, get_logger, handle_exception, hook, initialize,
    initialize_cvars, late_init, load_plugin, load_preset_plugins, log_exception,
    map_titles, next_frame, owner, parse_infostring, perf_trampoline,
    plugins_version, queued_handler, require_cvar,
    reload_plugin, set_map_subtitles, setting,
    set_plugins_version, spawn_points, starting_weapon_bit, stats_listener, thread,
    threading_excepthook, toggle_starting_weapon, unload_plugin, uptime, vote,
)
from ._configstring import (  # noqa: F401
    apply_variable_changes, configstring, configstring_variables, player_configstring,
    player_configstring_variables, update_configstring_variables,
    update_player_configstring_variables,
)
from ._mapinfo import (  # noqa: F401
    ArenaInfo, FactoryInfo, MapEntity, MapInfo, MapSource,
    factories, factory_info, installed_maps, map_entities, map_info, map_supports,
    map_supports_factory, map_worldspawn, refresh_map_cache,
)
from ._plugin import Identifier, Plugin  # noqa: F401
from ._game import Game, NonexistentGameError  # noqa: F401
from ._events import EVENT_DISPATCHERS, EventDispatcher, EventDispatcherManager  # noqa: F401
from ._commands import (  # noqa: F401
    AbstractChannel, BLUE_TEAM_CHAT_CHANNEL, BlueTeamChatChannel, CHAT_CHANNEL, COMMANDS,
    CONSOLE_CHANNEL, ChatChannel, ClientCommandChannel, Command, CommandInvoker,
    ConsoleChannel, FREE_CHAT_CHANNEL, FreeChatChannel, MAX_MSG_LENGTH,
    RED_TEAM_CHAT_CHANNEL, RedTeamChatChannel, SPECTATOR_CHAT_CHANNEL,
    SpectatorChatChannel, TellChannel, re_color_tag,
)
from ._votes import CUSTOM_VOTES, CustomVote, CustomVoteManager  # noqa: F401
from ._handlers import (  # noqa: F401
    NEXT_FRAME_TASKS_MAX, frame_tasks, next_frame_tasks, redirect_print, register_handlers,
)
from ._player import (  # noqa: F401
    AbstractDummyPlayer, DEFAULT_FLIGHT, NO_AMMO, NO_KEYS, NO_POWERUPS, NO_WEAPONS,
    NonexistentPlayerError, Player, RconDummyPlayer,
)
from ._zmq import StatsListener  # noqa: F401
