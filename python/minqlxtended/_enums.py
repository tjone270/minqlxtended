# minqlxtended - Extends Quake Live's dedicated server with extra functionality and scripting.
# Copyright (C) 2015 Mino <mino@minomino.org>
# Copyright (C) 2026 Thomas Jones <me@thomasjones.id.au>

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

"""Named constants for the values the API transacts in/out.

Properties that give you a string are ``StrEnum``, so ``player.team == "spectator"`` and
``game.state == "warmup"`` work, and are ``Team.SPECTATOR`` and ``GameState.WARMUP`` as
well. The numeric families are ``IntEnum`` and the bitfields ``IntFlag``, so arithmetic and
``|`` work on those. :class:`Return` is the one exception, for the reason in its docstring.

The ``StrEnum`` families put ``index`` and ``title`` properties over the ``str`` methods of
the same name, so ``str.index`` and ``str.title`` are unreachable through a member.
``Team.RED.index`` is the engine's number and ``Gametype.CTF.title`` is "Capture the Flag".

Values come from the C module rather than literals here. These are the only spelling:
write ``Return.STOP_ALL``, ``Priority.HIGH``, ``Weapon.RAILGUN``, ``CvarFlag.ARCHIVE``.
:data:`OWNED_PREFIXES` and :data:`OWNED_NAMES` say which constants the enums here speak
for, and so which ones the package doesn't republish.

Read ``_minqlxtended`` here rather than the package, so this module has no import-order
dependency on what the package has bound yet.
"""

from __future__ import annotations

import enum
from typing import Any, Self, TypeVar

import _minqlxtended

__all__: tuple[str, ...] = (
    "ConnectionState",
    "CvarFlag",
    "DamageFlag",
    "DemoRequest",
    "EntityEffect",
    "EntityEvent",
    "EntityFlag",
    "EntityType",
    "GameState",
    "Gametype",
    "Holdable",
    "Key",
    "MeansOfDeath",
    "Mod",
    "ModelIndex",
    "MoverState",
    "Objective",
    "PersistantIndex",
    "Powerup",
    "Priority",
    "Privilege",
    "Return",
    "RoundState",
    "SayMode",
    "ServerFlag",
    "ServerState",
    "StatIndex",
    "Team",
    "TrajectoryType",
    "Weapon",
)


def _lookup(cls: type[enum.Enum], table: dict[Any, Any], key: Any, noun: str = "index") -> Any:
    """``table[key]``, or a ValueError naming the family and the keys it does have.

    *noun* is what the key is, for the message.
    """
    try:
        return table[key]
    except (KeyError, TypeError):
        # Both directions in the message, so an off-by-one in a table shows up in the pairs.
        # `.name`, since `str()` of an IntEnum member is its number and prints the key twice.
        known = ", ".join(f"{k}={table[k].name}" for k in sorted(table))
        raise ValueError(f"{cls.__name__} has no member with {noun} {key!r}; it has {known}.") from None


def _from_prefix(name: str, prefix: str, base: Any = enum.IntEnum,
                 doc: str | None = None) -> Any:
    """Build an enum from every ``PREFIX_*`` int the C module exports.

    ``Any`` in and out: the members are read at import time, so no static type names them.
    """
    members = {}
    for const, value in vars(_minqlxtended).items():
        if const.startswith(prefix) and isinstance(value, int) and not isinstance(value, bool):
            members[const[len(prefix):]] = value

    if not members:
        raise RuntimeError(f"no {prefix}* constants found; is the engine module loaded?")

    built = base(name, members)
    if doc:
        built.__doc__ = doc
    return built


class Return(enum.Enum):
    """What an event handler returns to say what should happen next.

    Deliberately **not** an ``IntEnum``. With no integer conversion, ``False``, ``True``,
    ``0`` and ``1`` match no member; they fall through to
    :meth:`EventDispatcher.handle_return`, which reports the value as not understood.
    """

    #: Carry on to the next handler. Returning None does the same thing.
    NONE = _minqlxtended.RET_NONE
    #: Stop calling handlers.
    STOP = _minqlxtended.RET_STOP
    #: Let the remaining handlers run, but cancel the event.
    STOP_EVENT = _minqlxtended.RET_STOP_EVENT
    #: Stop the handlers *and* cancel the event.
    STOP_ALL = _minqlxtended.RET_STOP_ALL
    #: Command handlers only: print the command's usage string.
    USAGE = _minqlxtended.RET_USAGE


class Priority(enum.IntEnum):
    """Hook priority. Lower numbers run first, so ``HIGHEST`` is 0."""

    HIGHEST = _minqlxtended.PRI_HIGHEST
    HIGH = _minqlxtended.PRI_HIGH
    NORMAL = _minqlxtended.PRI_NORMAL
    LOW = _minqlxtended.PRI_LOW
    LOWEST = _minqlxtended.PRI_LOWEST


class Team(enum.StrEnum):
    """A player's team, as :attr:`Player.team` reports it."""

    FREE = "free"
    RED = "red"
    BLUE = "blue"
    SPECTATOR = "spectator"

    @property
    def index(self) -> int:  # type: ignore[override]
        """The ``TEAM_*`` number the engine uses for this team."""
        return _TEAM_INDICES[self]

    @classmethod
    def from_index(cls, index: int) -> Self:
        """The team the engine's *index* names."""
        return _lookup(cls, _TEAM_BY_INDEX, index)


class Privilege(enum.StrEnum):
    """The engine's admin level for a player, as :attr:`Player.privileges` reports it.

    Distinct from the permission level the permission plugin keeps in the database.
    :attr:`Player.privileges` reports no privileges as :attr:`NONE`, so test with
    ``== Privilege.NONE``. Every member is a non-empty string and so always truthy.
    """

    NONE = "none"
    MOD = "mod"
    ADMIN = "admin"
    ROOT = "root"
    BANNED = "banned"

    @property
    def level(self) -> int:
        """The ``privileges_t`` number the engine uses for this privilege."""
        return _PRIVILEGE_LEVELS[self]

    @classmethod
    def from_level(cls, level: int) -> Self:
        """The privilege the engine's *level* names."""
        return _lookup(cls, _PRIVILEGE_BY_LEVEL, level, "level")


class ConnectionState(enum.StrEnum):
    """How far through connecting a player is, as :attr:`Player.connection_state`."""

    FREE = "free"
    ZOMBIE = "zombie"
    CONNECTED = "connected"
    PRIMED = "primed"
    ACTIVE = "active"

    @property
    def index(self) -> int:  # type: ignore[override]
        """The ``clientState_t`` number for this state."""
        return _CONNECTION_STATE_INDICES[self]

    @classmethod
    def from_index(cls, index: int) -> Self:
        """The state the engine's *index* names."""
        return _lookup(cls, _CONNECTION_STATE_BY_INDEX, index)


class GameState(enum.StrEnum):
    """The match state, as :attr:`Game.state`."""

    WARMUP = "warmup"
    COUNTDOWN = "countdown"
    IN_PROGRESS = "in_progress"


class RoundState(enum.StrEnum):
    """Where a round-based gametype is in its cycle, as :attr:`Game.round_state`.

    Only round-based gametypes move through these; everything else sits in
    :attr:`PREGAME`.
    """

    PREGAME = "pregame"
    ROUND_WARMUP = "round_warmup"
    ROUND_SHUFFLE = "round_shuffle"
    ROUND_BEGUN = "round_begun"
    ROUND_OVER = "round_over"
    POSTGAME = "postgame"

    @property
    def index(self) -> int:  # type: ignore[override]
        """The ``roundStateState_t`` number for this state."""
        return _ROUND_STATE_INDICES[self]

    @classmethod
    def from_index(cls, index: int) -> Self:
        """The round state the engine's *index* names."""
        return _lookup(cls, _ROUND_STATE_BY_INDEX, index)


class Objective(enum.StrEnum):
    """What a player did to score, carried by the ``objective`` event."""

    CAPTURE = "capture"
    RETURN = "return"
    ASSIST = "assist"
    BASE_DEFENSE = "base_defense"
    CARRIER_DEFENSE = "carrier_defense"
    FRAG_CARRIER = "frag_carrier"
    #: Not an ``objective_t``. What the event carries when the engine reports a counter
    #: this build can't name, so the field is always an Objective.
    UNKNOWN = "unknown"

    @property
    def index(self) -> int:  # type: ignore[override]
        """The ``objective_t`` number for this objective.

        :attr:`UNKNOWN` has none and raises. ``objective_t`` ends at
        ``OBJ_FRAG_CARRIER``, so 6 is ``OBJ_COUNT``.
        """
        try:
            return _OBJECTIVE_INDICES[self]
        except KeyError:
            raise ValueError(
                f"Objective.{self.name} is not an objective_t. It is what the event carries when "
                "the engine names a counter this build does not know.") from None

    @classmethod
    def from_index(cls, index: int) -> Self:
        """The objective the engine's *index* names."""
        return _lookup(cls, _OBJECTIVE_BY_INDEX, index)


class Holdable(enum.StrEnum):
    """A holdable item, as :attr:`Player.holdable` reports it."""

    TELEPORTER = "teleporter"
    MEDKIT = "medkit"
    FLIGHT = "flight"
    KAMIKAZE = "kamikaze"
    PORTAL = "portal"
    INVULNERABILITY = "invulnerability"

    @property
    def model_index(self) -> enum.IntEnum:
        """The :class:`ModelIndex` this holdable is spawned from."""
        return ModelIndex[self.name]


class MeansOfDeath(enum.StrEnum):
    """How someone died, carried by the ``death`` and ``kill`` events as a name.

    The ``MOD_*`` integers are a separate family; :func:`minqlxtended.slay_with_mod` takes
    one of those, and :attr:`index` bridges the two.
    """

    UNKNOWN = "UNKNOWN"
    SHOTGUN = "SHOTGUN"
    GAUNTLET = "GAUNTLET"
    MACHINEGUN = "MACHINEGUN"
    GRENADE = "GRENADE"
    GRENADE_SPLASH = "GRENADE_SPLASH"
    ROCKET = "ROCKET"
    ROCKET_SPLASH = "ROCKET_SPLASH"
    PLASMA = "PLASMA"
    PLASMA_SPLASH = "PLASMA_SPLASH"
    RAILGUN = "RAILGUN"
    LIGHTNING = "LIGHTNING"
    BFG = "BFG"
    BFG_SPLASH = "BFG_SPLASH"
    WATER = "WATER"
    SLIME = "SLIME"
    LAVA = "LAVA"
    CRUSH = "CRUSH"
    TELEFRAG = "TELEFRAG"
    FALLING = "FALLING"
    SUICIDE = "SUICIDE"
    TARGET_LASER = "TARGET_LASER"
    TRIGGER_HURT = "TRIGGER_HURT"
    NAIL = "NAIL"
    CHAINGUN = "CHAINGUN"
    PROXIMITY_MINE = "PROXIMITY_MINE"
    KAMIKAZE = "KAMIKAZE"
    JUICED = "JUICED"
    GRAPPLE = "GRAPPLE"
    SWITCH_TEAMS = "SWITCH_TEAMS"
    THAW = "THAW"
    LIGHTNING_DISCHARGE = "LIGHTNING_DISCHARGE"
    HMG = "HMG"
    RAILGUN_HEADSHOT = "RAILGUN_HEADSHOT"

    @property
    def index(self) -> int:  # type: ignore[override]
        """The ``meansOfDeath_t`` number that ``slay_with_mod`` takes."""
        return _MEANS_OF_DEATH_INDICES[self]

    @classmethod
    def from_index(cls, index: int) -> Self:
        """The means of death the engine's *index* names."""
        return _lookup(cls, _MEANS_OF_DEATH_BY_INDEX, index)


class Gametype(enum.StrEnum):
    """A gametype by its short name, the form :attr:`Game.type_short` reports and plugins
    compare against.

    :attr:`Game.type` gives the long form; :attr:`title` is the same string.
    """

    FFA = "ffa"
    DUEL = "duel"
    RACE = "race"
    TDM = "tdm"
    CA = "ca"
    CTF = "ctf"
    ONE_FLAG = "1f"
    OVERLOAD = "ol"
    HARVESTER = "har"
    FREEZE_TAG = "ft"
    DOMINATION = "dom"
    ATTACK_AND_DEFEND = "ad"
    RED_ROVER = "rr"

    @property
    def index(self) -> int:  # type: ignore[override]
        """The ``g_gametype`` value for this gametype."""
        return _GAMETYPE_INDICES[self]

    @classmethod
    def from_index(cls, index: int) -> Self:
        """The gametype the engine's ``g_gametype`` *index* names."""
        return _lookup(cls, _GAMETYPE_BY_INDEX, index)

    @property
    def title(self) -> str:  # type: ignore[override]
        """The long name, e.g. "Capture the Flag"."""
        return _GAMETYPE_TITLES[self]

    @property
    def is_team_based(self) -> bool:
        """Whether players are split into red and blue. Red Rover and Overload are."""
        return self not in _NONTEAM_BASED


class Weapon(enum.IntEnum):
    """A weapon.

    Each member is bound to its ``WP_*`` constant. :attr:`short` is the two- or
    three-letter spelling the ``Weapons`` struct sequence uses, as in ``player.weapons.rl``.
    """

    GAUNTLET = _minqlxtended.WP_GAUNTLET
    MACHINEGUN = _minqlxtended.WP_MACHINEGUN
    SHOTGUN = _minqlxtended.WP_SHOTGUN
    GRENADE_LAUNCHER = _minqlxtended.WP_GRENADE_LAUNCHER
    ROCKET_LAUNCHER = _minqlxtended.WP_ROCKET_LAUNCHER
    LIGHTNING = _minqlxtended.WP_LIGHTNING
    RAILGUN = _minqlxtended.WP_RAILGUN
    PLASMAGUN = _minqlxtended.WP_PLASMAGUN
    BFG = _minqlxtended.WP_BFG
    GRAPPLING_HOOK = _minqlxtended.WP_GRAPPLING_HOOK
    NAILGUN = _minqlxtended.WP_NAILGUN
    PROX_LAUNCHER = _minqlxtended.WP_PROX_LAUNCHER
    CHAINGUN = _minqlxtended.WP_CHAINGUN
    HMG = _minqlxtended.WP_HMG
    HANDS = _minqlxtended.WP_HANDS

    @property
    def short(self) -> str:
        """The abbreviated name, e.g. "rl"."""
        return _WEAPON_SHORT[self]

    @classmethod
    def from_short(cls, short: str) -> Self:
        """The weapon the abbreviated *short* name spells."""
        return _lookup(cls, _WEAPON_BY_SHORT, short, "short name")


class DemoRequest(enum.IntEnum):
    """Whether a player is being recorded, as ``DemoStatus.requested``."""

    #: Recording because sv_demoRecord says so, rather than on request.
    DEFAULT = 0
    #: start_demo() was called for this player.
    RECORDING = 1
    #: Explicitly excluded, regardless of sv_demoRecord.
    EXCLUDED = -1


#: Cvar flags: ``CVAR_ARCHIVE``, ``CVAR_ROM`` and friends. ``IntFlag``, so they combine
#: with ``|`` and a combination reprs as its parts.
CvarFlag = _from_prefix("CvarFlag", "CVAR_", enum.IntFlag,
                        "Flags on a cvar. Combine with |.")

#: ``svFlags`` on an entity: ``SVF_BOT``, ``SVF_BROADCAST`` and so on. ``Entity(n).r.sv_flags``.
ServerFlag = _from_prefix("ServerFlag", "SVF_", enum.IntFlag,
                          "Server flags on an entity, from r.sv_flags. Combine with |.")

#: ``flags`` on an entity: ``FL_GODMODE``, ``FL_NOTARGET`` and so on. ``Entity(n).flags``.
EntityFlag = _from_prefix("EntityFlag", "FL_", enum.IntFlag,
                          "Game-module flags on an entity, from flags. Combine with |.")

#: ``eFlags``, on both ``Entity(n).s.e_flags`` and ``GameClient(n).ps.e_flags``.
EntityEffect = _from_prefix("EntityEffect", "EF_", enum.IntFlag,
                            "Entity effects, from e_flags. Combine with |.")

#: The ``dflags`` argument carried by the ``damage`` event.
DamageFlag = _from_prefix("DamageFlag", "DAMAGE_", enum.IntFlag,
                          "How a hit bypasses armour, protection or knockback. Combine with |.")

#: ``entityType_t``, what an entity is. ``Entity(n).s.e_type``.
EntityType = _from_prefix("EntityType", "ET_")

#: ``entity_event_t``, what :func:`minqlxtended.add_event` raises on an entity.
EntityEvent = _from_prefix("EntityEvent", "EV_")

#: ``serverState_t``, what the server is doing. ``minqlxtended.server.state``.
ServerState = _from_prefix("ServerState", "SS_")

#: ``meansOfDeath_t`` as the engine numbers them. :func:`minqlxtended.slay_with_mod` takes
#: one of these. :class:`MeansOfDeath` is the same concept as the names the ``death`` and
#: ``kill`` events report.
Mod = _from_prefix("Mod", "MOD_")

#: Every item in ``bg_itemlist``, by its ``MODELINDEX_*``. Named for the constants; the
#: engine module uses `Item` for the live ``gitem_t`` view.
ModelIndex = _from_prefix("ModelIndex", "MODELINDEX_")

#: ``trType_t``, how an entity's trajectory is evaluated. ``Entity(n).s.pos.type``.
TrajectoryType = _from_prefix("TrajectoryType", "TR_")

#: ``moverState_t``, where a door or platform is in its travel.
MoverState = _from_prefix("MoverState", "MOVER_")

#: The ``mode`` argument the ``chat`` event's G_Say hook is given.
SayMode = _from_prefix("SayMode", "SAY_")


# Indices into the three arrays on playerState_t. `GameClient(n).ps.stats`, `.persistant` and
# `.powerups` are fixed-length int views, and these name their slots.

#: ``statIndex_t``, indexing ``GameClient(n).ps.stats``. ``Entity(n).health`` is the
#: authoritative health; ``stats[StatIndex.HEALTH]`` is the clamped copy the HUD reads.
StatIndex = _from_prefix("StatIndex", "STAT_")

#: Indexing ``GameClient(n).ps.persistant``. ``ROUND_SCORE`` is the score, the rest are
#: medal counters. Named for what it indexes; ``Persistant`` is the live view of
#: ``clientPersistant_t``.
PersistantIndex = _from_prefix("PersistantIndex", "PERS_")

#: ``powerup_t``, indexing ``GameClient(n).ps.powerups``. Each slot is the level time the
#: powerup runs out at, or 0 for not held. ``Powerup`` is a slot number; ``Powerups``, the
#: struct sequence ``player.powerups`` gives, is a snapshot of six of them.
Powerup = _from_prefix("Powerup", "PW_")

#: ``keys_t``, the bits of ``ps.stats[StatIndex.KEY]``. Singular against the ``Keys`` struct
#: sequence, for the same reason ``Powerup`` is.
Key = _from_prefix("Key", "KEY_")


# Side tables. Declared after the enums because they key off the members.

_Member = TypeVar("_Member", bound=enum.Enum)


def _invert(indices: dict[_Member, int]) -> dict[int, _Member]:
    """The number -> member direction of an index table, checking it is one-to-one."""
    reverse = {number: member for member, number in indices.items()}
    if len(reverse) != len(indices):
        raise RuntimeError(f"two members share an index in {indices!r}")
    return reverse


_TEAM_INDICES = {
    Team.FREE: _minqlxtended.TEAM_FREE,
    Team.RED: _minqlxtended.TEAM_RED,
    Team.BLUE: _minqlxtended.TEAM_BLUE,
    Team.SPECTATOR: _minqlxtended.TEAM_SPECTATOR,
}
_TEAM_BY_INDEX = _invert(_TEAM_INDICES)

_PRIVILEGE_LEVELS = {
    Privilege.NONE: _minqlxtended.PRIV_NONE,
    Privilege.MOD: _minqlxtended.PRIV_MOD,
    Privilege.ADMIN: _minqlxtended.PRIV_ADMIN,
    Privilege.ROOT: _minqlxtended.PRIV_ROOT,
    Privilege.BANNED: _minqlxtended.PRIV_BANNED,
}
_PRIVILEGE_BY_LEVEL = _invert(_PRIVILEGE_LEVELS)

# meansOfDeath_t order, which is also the order the members are declared in.
_MEANS_OF_DEATH_INDICES = {member: i for i, member in enumerate(MeansOfDeath)}
_MEANS_OF_DEATH_BY_INDEX = _invert(_MEANS_OF_DEATH_INDICES)

# g_gametype order, likewise.
_GAMETYPE_INDICES = {member: i for i, member in enumerate(Gametype)}
_GAMETYPE_BY_INDEX = _invert(_GAMETYPE_INDICES)

# The clientState_t constants. Written out against the C module for the same reason
# _TEAM_INDICES is: declaration order here says nothing about the engine's numbering.
_CONNECTION_STATE_INDICES = {
    ConnectionState.FREE: _minqlxtended.CS_FREE,
    ConnectionState.ZOMBIE: _minqlxtended.CS_ZOMBIE,
    ConnectionState.CONNECTED: _minqlxtended.CS_CONNECTED,
    ConnectionState.PRIMED: _minqlxtended.CS_PRIMED,
    ConnectionState.ACTIVE: _minqlxtended.CS_ACTIVE,
}
_CONNECTION_STATE_BY_INDEX = _invert(_CONNECTION_STATE_INDICES)

# roundStateState_t order.
_ROUND_STATE_INDICES = {member: i for i, member in enumerate(RoundState)}
_ROUND_STATE_BY_INDEX = _invert(_ROUND_STATE_INDICES)

# objective_t order, matching game_events.h. UNKNOWN is left out; the C enum ends at
# OBJ_FRAG_CARRIER and then OBJ_COUNT, so index 6 is the count, and giving UNKNOWN that
# number would make `from_index(6)` answer instead of raising.
_OBJECTIVE_INDICES: dict[Objective, int] = {
    member: i for i, member in enumerate(Objective) if member is not Objective.UNKNOWN}
_OBJECTIVE_BY_INDEX = _invert(_OBJECTIVE_INDICES)

_GAMETYPE_TITLES = dict(zip(Gametype, (
    "Free for All",
    "Duel",
    "Race",
    "Team Deathmatch",
    "Clan Arena",
    "Capture the Flag",
    "One Flag",
    "Overload",
    "Harvester",
    "Freeze Tag",
    "Domination",
    "Attack and Defend",
    "Red Rover",
)))

_NONTEAM_BASED = frozenset((Gametype.FFA, Gametype.DUEL, Gametype.RACE))

# The abbreviations, in weapon_t order. These are from the Weapons struct sequence's field.
_WEAPON_SHORT = dict(zip(Weapon, (
    "g", "mg", "sg", "gl", "rl", "lg", "rg", "pg", "bfg", "gh", "ng", "pl", "cg", "hmg",
    "hands",
)))

_WEAPON_BY_SHORT = {short: weapon for weapon, short in _WEAPON_SHORT.items()}


# --------------------------------------------------------------------------------------
# Which engine constants these enums speak for. `tools/gen_stub.py` reads this to leave
# them out of the generated import block in `__init__.py`.
#
# Rebinding a name here shadows the old spelling rather than removing it. Delete the
# rebinding and `minqlxtended.RET_STOP_ALL` comes back as the plain int 3, which a handler
# can return and have silently ignored. Only naming the imports removes it.
# --------------------------------------------------------------------------------------

#: Prefixes of every ``_minqlxtended`` constant an enum here owns, with the enum that owns
#: it.
OWNED_PREFIXES = {
    "RET_": Return,
    "PRI_": Priority,
    "WP_": Weapon,
    "MOD_": Mod,
    "ET_": EntityType,
    "EV_": EntityEvent,
    "SS_": ServerState,
    "CVAR_": CvarFlag,
    "SVF_": ServerFlag,
    "FL_": EntityFlag,
    "EF_": EntityEffect,
    "STAT_": StatIndex,
    "PERS_": PersistantIndex,
    "PW_": Powerup,
    "KEY_": Key,
    "DAMAGE_": DamageFlag,
    "TEAM_": Team,
    "PRIV_": Privilege,
    "MODELINDEX_": ModelIndex,
    "TR_": TrajectoryType,
    "MOVER_": MoverState,
    "SAY_": SayMode,
}

#: The five ``clientState_t`` constants, which :class:`ConnectionState` owns. Named rather
#: than prefixed, since ``CS_`` is two families: these and the ~79 configstring indices.
#: ``CS_FREE`` and ``CS_SERVERINFO`` are both 0, so no single enum can hold the prefix.
OWNED_NAMES = frozenset((
    "CS_FREE", "CS_ZOMBIE", "CS_CONNECTED", "CS_PRIMED", "CS_ACTIVE",
))


def is_owned(name: str) -> bool:
    """Whether *name* is a ``_minqlxtended`` constant an enum here speaks for."""
    return name in OWNED_NAMES or any(
        name.startswith(prefix) for prefix in OWNED_PREFIXES)
