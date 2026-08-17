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

from __future__ import annotations

import minqlxtended
from typing import Any, Mapping

from ._commands import center_print as _center_print
from ._commands import re_color_tag as _re_color_code
from ._enums import (ConnectionState, EntityEffect, EntityFlag, Holdable, Key,
                     ModelIndex, Mod, PersistantIndex, Powerup, Privilege, ServerFlag,
                     StatIndex, Team, Weapon)

__all__ = (
    "AbstractDummyPlayer",
    "DEFAULT_FLIGHT",
    "NO_AMMO",
    "NO_KEYS",
    "NO_POWERUPS",
    "NO_WEAPONS",
    "NonexistentPlayerError",
    "Player",
    "RconDummyPlayer",
)

_DUMMY_USERINFO = (
    "ui_singlePlayerActive\\0\\cg_autoAction\\1\\cg_autoHop\\0"
    "\\cg_predictItems\\1\\model\\bitterman/sport_blue\\headmodel\\crash/red"
    "\\handicap\\100\\cl_anonymous\\0\\color1\\4\\color2\\23\\sex\\male"
    "\\teamtask\\0\\rate\\25000\\country\\NO"
)

#: What the flight holdable starts at when it is granted.
DEFAULT_FLIGHT = minqlxtended.Flight((16000, 16000, 1200, 0))

# The empty ones, for building a loadout from nothing:
#     player.weapons = minqlxtended.NO_WEAPONS._replace(rl=True)   # only a rocket launcher
NO_WEAPONS = minqlxtended.Weapons((False,) * len(minqlxtended.Weapons._fields))
NO_AMMO = minqlxtended.Weapons((0,) * len(minqlxtended.Weapons._fields))
NO_POWERUPS = minqlxtended.Powerups((0,) * len(minqlxtended.Powerups._fields))
NO_KEYS = minqlxtended.Keys((False,) * len(minqlxtended.Keys._fields))


# Which powerup slot each Powerups field is.
_POWERUP_SLOTS = (
    Powerup.QUAD,
    Powerup.BATTLESUIT,
    Powerup.HASTE,
    Powerup.INVIS,
    Powerup.REGEN,
    Powerup.INVULNERABILITY,
)

# The four ps.stats slots a Flight is, in field order.
_FLIGHT_SLOTS = (
    StatIndex.CUR_FLIGHT_FUEL,
    StatIndex.MAX_FLIGHT_FUEL,
    StatIndex.FLIGHT_THRUST,
    StatIndex.FLIGHT_REFUEL,
)


def _set_bit(value: int, bit: int, on: bool) -> int:
    """*value* with *bit* set or cleared."""
    return (value | bit) if on else (value & ~int(bit))


def _as_struct(value: Any, kind: type, what: str) -> Any:
    if not isinstance(value, kind):
        raise TypeError(f"{what} must be a minqlxtended.{kind.__name__}, got {value!r}")

    return value


class NonexistentPlayerError(Exception):
    """An exception that is raised when a player that disconnected is being used
    as if the player were still present.
    """

    pass


class Player:
    """A player on the server.

    Five attributes are a snapshot taken when the instance was built: :attr:`name`,
    :attr:`team`, :attr:`steam_id`, :attr:`privileges` and :attr:`connection_state`. Call
    :meth:`~.Player.update` for their current values, which raises
    :exc:`minqlxtended.NonexistentPlayerError` if the player has disconnected. Everything
    else reads the engine on every access.

    The snapshot is safe to hand to an :func:`minqlxtended.thread` worker; reading those
    five live would race the game thread reusing the client slot.

    """

    def __init__(self, client_id: int, info: minqlxtended.PlayerInfo | None = None) -> None:
        self._valid = True
        self._info: Any

        # Pass your own info when you're building the whole player list, and for dummies.
        if info:
            self._id = client_id
            self._info = info
        else:
            self._id = client_id
            self._info = minqlxtended.player_info(client_id)
            if not self._info:
                self._invalidate(f"Tried to initialize a Player instance of nonexistant player {client_id}.")

        self._userinfo = None
        self._steam_id = self._info.steam_id

        # The name field in the client struct isn't filled in yet while a player is still
        # connecting, so fall back to parsing it out of the userinfo.
        if self._info.name:
            self._name = self._info.name
        else:
            self._userinfo = minqlxtended.parse_infostring(self._info.userinfo)
            if "name" in self._userinfo:
                self._name = self._userinfo["name"]
            else:  # No name at all, which odd userinfo during connection can do.
                self._name = ""

    def __repr__(self) -> str:
        if not self._valid:
            return f"{self.__class__.__name__}(INVALID:'{self.clean_name}':{self.steam_id})"

        return f"{self.__class__.__name__}({self._id}:'{self.clean_name}':{self.steam_id})"

    def __str__(self) -> str:
        return self.name

    def __contains__(self, key: str) -> bool:
        return key in self._cvars()

    def __getitem__(self, key: str) -> str:
        return self._cvars()[key]

    def __eq__(self, other: object) -> bool:
        if isinstance(other, type(self)):
            return self.steam_id == other.steam_id
        else:
            return self.steam_id == other

    def __hash__(self) -> int:
        return hash(self.steam_id)

    def update(self) -> None:
        """Re-read the player's information, invalidating the instance if they
        disconnected. Name and Steam ID still read after that; anything else raises.

        :raises: minqlxtended.NonexistentPlayerError
        """

        info = minqlxtended.player_info(self._id)

        if not info or self._steam_id != info.steam_id:
            self._invalidate()

        self._info = info
        self._userinfo = None

        if self._info.name:
            self._name = self._info.name
        else:
            self._userinfo = minqlxtended.parse_infostring(self._info.userinfo)
            if "name" in self._userinfo:
                self._name = self._userinfo["name"]
            else:
                self._name = ""

    def _invalidate(self, e: str = "The player does not exist anymore. Did the player disconnect?") -> None:
        self._valid = False
        raise NonexistentPlayerError(e)

    def _cvars(self) -> dict[str, str]:
        """The parsed userinfo itself, uncopied. Callers in this class must not mutate it.

        :attr:`cvars` is the copy, for callers outside the class.
        """
        if not self._valid:
            self._invalidate()

        if self._userinfo is None:
            self._userinfo = minqlxtended.parse_infostring(self._info.userinfo)

        return self._userinfo

    def _cvar(self, key: str, default: str = "") -> str:
        """The userinfo value for *key*, or *default* when the client has not sent one."""
        return self._cvars().get(key, default)

    @property
    def cvars(self) -> dict[str, str]:
        return self._cvars().copy()

    @cvars.setter
    def cvars(self, new_cvars: Mapping[str, object]) -> None:
        if not self._valid:
            self._invalidate()

        # Through format_infostring, which the userinfo hook also serialises with.
        minqlxtended.client_command(
            self.id, f'userinfo "{minqlxtended.format_infostring(new_cvars)}"')

        self._userinfo = None

    def _set_userinfo(self, **changes: Any) -> None:
        """Change some userinfo keys and send the result back to the client."""
        new = self.cvars
        new.update(changes)
        self.cvars = new

    @property
    def steam_id(self) -> int:
        return self._steam_id

    @property
    def id(self) -> int:
        return self._id

    @property
    def entity(self) -> minqlxtended.Entity:
        """This player's :class:`minqlxtended.Entity`, a live view onto g_entities."""
        return minqlxtended.Entity(self.id)

    @property
    def gclient(self) -> minqlxtended.GameClient:
        """The game module's state for this player, as a
        :class:`minqlxtended.GameClient`."""
        return minqlxtended.GameClient(self.id)

    @property
    def connection(self) -> minqlxtended.Client:
        """The server's connection record, as a :class:`minqlxtended.Client`.

        The engine's `client_t`. :attr:`gclient` is the game module's `gclient_t`.

        """
        return minqlxtended.Client(self.id)

    @property
    def ip(self) -> str:
        """The address this player connected from. Read from the connection itself."""
        return self.connection.ip

    @property
    def clan(self) -> str:
        """The clan tag, from the configstring cache.

        QL has no clan support of its own; the scoreboard displays whatever tag the
        configstring carries.
        """
        return minqlxtended.player_configstring_variables(self.id).get("cn", "")

    @clan.setter
    def clan(self, tag: str) -> None:
        """Set the clan tag.

        Dirty-checked: setting the tag it already has does nothing. To force the
        set_configstring dispatchers to run, call
        ``minqlxtended.set_configstring(minqlxtended.CS_PLAYERS + player.id, player.configstring)``.
        """
        minqlxtended.update_player_configstring_variables(self.id, {"xcn": tag, "cn": tag})

    @property
    def configstring(self) -> str:
        """The player's raw configstring, from the cache. Use
        :attr:`configstring_variables` for it parsed into a dict."""
        return minqlxtended.player_configstring(self.id)

    @property
    def configstring_variables(self) -> Mapping[str, str]:
        """The player's configstring, parsed. Read-only: it is the shared parse every
        other reader sees, so copy it before changing anything."""
        return minqlxtended.player_configstring_variables(self.id)

    def update_configstring(self, changes: Mapping[str, str | None]) -> bool:
        """Read-modify-write this player's own configstring, for everyone.

        Goes through the server's table, so every client sees the change and it survives
        later writes. A value of None removes the key. Returns False if nothing changed.
        """
        return minqlxtended.update_player_configstring_variables(self.id, changes)

    @property
    def name(self) -> str:
        return self._name + "^7"

    @name.setter
    def name(self, value: str) -> None:
        self._set_userinfo(name=value)

    @property
    def clean_name(self) -> str:
        """Removes color tags from the name."""
        return _re_color_code.sub("", self.name)

    @property
    def qport(self) -> int:
        """The port the client multiplexes on behind a NAT.

        From the netchan. The userinfo copy is whatever the client chose to tell us.

        """
        return self.connection.netchan.qport

    @property
    def team(self) -> Team:
        return Team.from_index(self._info.team)

    @team.setter
    def team(self, new_team: str) -> None:
        self.put(new_team)

    @property
    def colors(self) -> tuple[float, float]:
        # Float, since the userinfo values are not always integral.
        return float(self._cvar("color1", "0")), float(self._cvar("color2", "0"))

    @colors.setter
    def colors(self, value: tuple[float, float]) -> None:
        c1, c2 = value
        self._set_userinfo(color1=c1, color2=c2)

    @property
    def model(self):
        return self._cvar("model")

    @model.setter
    def model(self, value):
        self._set_userinfo(model=value)

    @property
    def headmodel(self):
        return self._cvar("headmodel")

    @headmodel.setter
    def headmodel(self, value):
        self._set_userinfo(headmodel=value)

    @property
    def handicap(self):
        return self._cvar("handicap", "100")

    @handicap.setter
    def handicap(self, value):
        self._set_userinfo(handicap=value)

    @property
    def autohop(self):
        return bool(int(self._cvar("cg_autoHop", "0")))

    @autohop.setter
    def autohop(self, value):
        self._set_userinfo(cg_autoHop=int(value))

    @property
    def autoaction(self):
        return bool(int(self._cvar("cg_autoAction", "0")))

    @autoaction.setter
    def autoaction(self, value):
        self._set_userinfo(cg_autoAction=int(value))

    @property
    def predictitems(self):
        return bool(int(self._cvar("cg_predictItems", "0")))

    @predictitems.setter
    def predictitems(self, value):
        self._set_userinfo(cg_predictItems=int(value))

    @property
    def connection_state(self) -> ConnectionState:
        """How far through connecting the player is.

        *free* and *zombie* are a slot being released, *connected* and *primed* are still
        loading, and *active* is in-game. Check
        ``player.connection_state == "active"`` to require in-game.

        """
        return ConnectionState.from_index(self._info.connection_state)

    @property
    def state(self) -> minqlxtended.PlayerState | None:
        """This player's engine state, or None if they have no game client."""
        return minqlxtended.player_state(self.id)

    @property
    def _live_state(self) -> minqlxtended.PlayerState:
        """:attr:`state`, but raises instead of answering None."""
        state = minqlxtended.player_state(self.id)
        if state is None:
            raise minqlxtended.EngineStateError(
                f"no game client in slot {self.id}.")
        return state

    @property
    def _live_entity(self) -> minqlxtended.Entity:
        """:attr:`entity`, but raises for a slot with nobody in it."""
        entity = minqlxtended.Entity(self.id)
        if entity.client is None:
            raise minqlxtended.EngineStateError(
                f"no game client in slot {self.id}.")

        return entity

    @property
    def _live_stats(self) -> minqlxtended.PlayerStats:
        """:attr:`stats`, but raises instead of answering None. See :attr:`_live_state`."""
        stats = minqlxtended.player_stats(self.id)
        if stats is None:
            raise minqlxtended.EngineStateError(
                f"no game client in slot {self.id}.")
        return stats

    @property
    def privileges(self) -> Privilege:
        """The engine's privilege level, as a :class:`Privilege`.

        Distinct from the permission level the permission plugin keeps in the database,
        the one :meth:`Plugin.db.has_permission` reads. No privileges is
        :attr:`Privilege.NONE`; test with ``== Privilege.NONE``, since every member is a
        non-empty string and so always truthy.

        """
        priv = self._info.privileges
        try:
            return Privilege.from_level(priv)
        except ValueError:
            minqlxtended.get_logger().warning(
                "Player %s has unknown privilege level %r; treating as no privileges.",
                self.steam_id, priv)
            return Privilege.NONE

    @privileges.setter
    def privileges(self, value: Privilege | None) -> None:
        # None means "take their privileges away".
        try:
            privilege = Privilege.NONE if value is None else Privilege(value)
        except ValueError:
            levels = ", ".join(repr(str(p)) for p in Privilege)
            raise ValueError(f"Invalid privilege level: {value!r}. Valid levels are {levels}.") from None

        self.gclient.sess.privileges = privilege.level

    @property
    def country(self) -> str:
        return self._cvar("country")

    @country.setter
    def country(self, value: str) -> None:
        self._set_userinfo(country=value)

    @property
    def valid(self) -> bool:
        return self._valid

    @property
    def stats(self) -> minqlxtended.PlayerStats | None:
        """This player's match statistics, or None if they have no game client."""
        return minqlxtended.player_stats(self.id)

    @stats.setter
    def stats(self, value: minqlxtended.PlayerStats) -> None:
        _as_struct(value, minqlxtended.PlayerStats, "stats")
        expanded                    = self.gclient.expanded_stats
        expanded.num_kills          = value.kills
        expanded.num_deaths         = value.deaths
        expanded.total_damage_dealt = value.damage_dealt
        expanded.total_damage_taken = value.damage_taken

    @property
    def ping(self) -> int:
        """The server's own round-trip measurement.

        Read from the connection. `PlayerStats.ping` is the game module's separate copy in
        `ps.ping`, and the two can disagree.
        """
        return self.connection.ping

    @property
    def position(self) -> minqlxtended.Vector3:
        """Where the player is, as a :class:`minqlxtended.Vector3`.

        Assign a Vector3 or any three-item sequence. To change one axis::
            player.position = player.position._replace(z=100)
        """
        return self._live_state.position

    @position.setter
    def position(self, value: Any) -> None:
        self.gclient.ps.origin = value

    @property
    def velocity(self) -> minqlxtended.Vector3:
        """How fast the player is moving, as a :class:`minqlxtended.Vector3`.

        Assign a Vector3 or any three-item sequence, the way :attr:`position` does.
        """
        return self._live_state.velocity

    @velocity.setter
    def velocity(self, value: Any) -> None:
        self.gclient.ps.velocity = value

    @property
    def weapons(self) -> minqlxtended.Weapons:
        """Which weapons the player is carrying, as a :class:`minqlxtended.Weapons`.

        To change some of them, or to build a set from nothing::
            player.weapons = player.weapons._replace(rl=True, rg=True)
            player.weapons = minqlxtended.NO_WEAPONS._replace(rl=True)
        """
        return self._live_state.weapons

    @weapons.setter
    def weapons(self, value: minqlxtended.Weapons) -> None:
        _as_struct(value, minqlxtended.Weapons, "weapons")
        held = 0
        for weapon, carried in zip(Weapon, value):
            if not isinstance(carried, bool):
                raise TypeError(f"weapons.{weapon.short} must be a bool, got {carried!r}")
            if carried:
                held |= 1 << weapon

        self.gclient.ps.stats[StatIndex.WEAPONS] = held

    @property
    def weapon(self) -> Weapon:
        """The weapon the player is currently holding, as a :class:`Weapon`::
            player.weapon = minqlxtended.Weapon.ROCKET_LAUNCHER
            player.weapon = minqlxtended.Weapon.from_short("rl")

        """
        return Weapon(self.gclient.ps.weapon)

    @weapon.setter
    def weapon(self, value: Weapon) -> None:
        try:
            weapon = Weapon(value)
        except ValueError:
            shorts = ", ".join(repr(w.short) for w in Weapon)
            raise ValueError(
                f"Invalid weapon: {value!r}. Use a Weapon member, or Weapon.from_short() for {shorts}.") from None

        self.gclient.ps.weapon = weapon

    @property
    def ammo(self) -> minqlxtended.Weapons:
        """How much ammunition the player has, as a :class:`minqlxtended.Weapons` of
        counts instead of flags::
            player.ammo = player.ammo._replace(rl=25)

        """
        return self._live_state.ammo

    @ammo.setter
    def ammo(self, value: minqlxtended.Weapons) -> None:
        _as_struct(value, minqlxtended.Weapons, "ammo")

        ammo = self.gclient.ps.ammo
        for weapon, count in zip(Weapon, value):
            ammo[weapon] = count

    @property
    def powerups(self) -> minqlxtended.Powerups:
        """Time remaining on each powerup, as a :class:`minqlxtended.Powerups`.

        .. note::
            Milliseconds, both reading and writing, so convert at the call site::

                player.powerups = player.powerups._replace(quad=30 * 1000)

        """
        return self._live_state.powerups

    @powerups.setter
    def powerups(self, value: minqlxtended.Powerups) -> None:
        _as_struct(value, minqlxtended.Powerups, "powerups")

        powerups = self.gclient.ps.powerups
        now      = minqlxtended.level.time
        expiry   = now - (now % 1000)

        for slot, remaining in zip(_POWERUP_SLOTS, value):
            powerups[slot] = expiry + remaining if remaining else 0

    @property
    def keys(self) -> minqlxtended.Keys:
        """Which keys the player holds, as a :class:`minqlxtended.Keys`."""
        return self._live_state.keys

    @keys.setter
    def keys(self, value: minqlxtended.Keys) -> None:
        _as_struct(value, minqlxtended.Keys, "keys")

        held = 0
        for key, carried in zip(Key, value):
            if not isinstance(carried, bool):
                raise TypeError(f"keys.{key.name.lower()} must be a bool, got {carried!r}")
            if carried:
                held |= 1 << key

        self.gclient.ps.stats[StatIndex.KEY] = held

    @property
    def holdable(self) -> Holdable | None:
        """The holdable item the player is carrying, as a :class:`Holdable`, or None.

        Assigning None takes it away. Nothing else falsy does.
        """
        held = self._live_state.holdable
        if held is None:
            return None

        try:
            return Holdable(held)
        except ValueError:
            minqlxtended.get_logger().warning(
                "Player %s is holding %r, which names nothing we know; "
                "reporting it as no holdable.", self.steam_id, held)
            return None

    @holdable.setter
    def holdable(self, value: Holdable | None) -> None:
        if value is None:
            self._set_holdable(0)
            return

        try:
            value = Holdable(value)
        except ValueError:
            items = ", ".join(repr(str(h)) for h in Holdable)
            raise ValueError(f"Invalid holdable item: {value!r}. Valid items are {items}.") from None

        self._set_holdable(value.model_index)
        if value == Holdable.FLIGHT:
            # The engine leaves whatever the last carrier had in the flight fields, so a
            # freshly granted holdable has to be given its starting values here.
            self._set_flight(DEFAULT_FLIGHT)

    def _set_holdable(self, model_index: int) -> None:
        """Put a model index in the holdable slot, keeping the kamikaze effect with it.

        The client draws the skull from an effect bit rather than the slot, so the two
        move together.
        """
        ps          = self.gclient.ps
        ps.e_flags  = _set_bit(ps.e_flags, EntityEffect.KAMIKAZE,
                               model_index == ModelIndex.KAMIKAZE)
        ps.stats[StatIndex.HOLDABLE_ITEM] = model_index

    def drop_holdable(self) -> None:
        minqlxtended.drop_holdable(self.id)

    @property
    def flight(self) -> minqlxtended.Flight:
        """The player's flight parameters, as a :class:`minqlxtended.Flight`.

        Reading is meaningless unless the player carries the flight holdable. Assigning
        grants them the holdable first::
            player.flight = player.flight._replace(fuel=8000)

        :attr:`DEFAULT_FLIGHT` is what a freshly granted one starts at.
        """
        return self._live_state.flight

    @flight.setter
    def flight(self, value: minqlxtended.Flight) -> None:
        _as_struct(value, minqlxtended.Flight, "flight")

        if self._live_state.holdable != Holdable.FLIGHT:
            self.holdable = Holdable.FLIGHT

        self._set_flight(value)

    def _set_flight(self, value: minqlxtended.Flight) -> None:
        """The four ps.stats slots, without the granting the setter above does."""
        stats = self.gclient.ps.stats
        for slot, parameter in zip(_FLIGHT_SLOTS, value):
            stats[slot] = parameter

    @property
    def noclip(self):
        return self.gclient.noclip

    @noclip.setter
    def noclip(self, value):
        self.gclient.noclip = bool(value)

    @property
    def god(self):
        return bool(self._live_entity.flags & EntityFlag.GODMODE)

    @god.setter
    def god(self, value):
        entity       = self._live_entity
        entity.flags = _set_bit(entity.flags, EntityFlag.GODMODE, bool(value))

    @property
    def notarget(self):
        return bool(self._live_entity.flags & EntityFlag.NOTARGET)

    @notarget.setter
    def notarget(self, value):
        entity       = self._live_entity
        entity.flags = _set_bit(entity.flags, EntityFlag.NOTARGET, bool(value))

    @property
    def flags(self):
        return self._live_entity.flags

    @flags.setter
    def flags(self, value):
        self._live_entity.flags = int(value)

    @property
    def health(self):
        return self._live_entity.health

    @health.setter
    def health(self, value):
        # gentity_t.health. stats[StatIndex.HEALTH] is the clamped copy the HUD
        # reads, and it's rewritten from this one every frame.
        self._live_entity.health = value

    @property
    def armor(self):
        return self.gclient.ps.stats[StatIndex.ARMOR]

    @armor.setter
    def armor(self, value):
        self.gclient.ps.stats[StatIndex.ARMOR] = value

    @property
    def speed(self):
        return self.gclient.ps.speed

    @speed.setter
    def speed(self, value):
        self.gclient.ps.speed = value

    @property
    def gravity(self):
        return self.gclient.ps.gravity

    @gravity.setter
    def gravity(self, value):
        self.gclient.ps.gravity = value

    def invulnerability(self, time: int) -> None:
        """Makes the player invulnerable for a while. Write-only; PlayerState doesn't carry it.

        :param time: How long to be invulnerable for, in milliseconds.
        :type time: int
        :raises: ValueError -- if *time* is not positive.
        """
        if time <= 0:
            raise ValueError("time needs to be a positive integer.")

        self.gclient.invulnerability_time = minqlxtended.level.time + time

    @property
    def userinfo(self) -> str | None:
        """The player's raw userinfo string. Use :attr:`cvars` for it parsed into a dict."""
        return minqlxtended.get_userinfo(self.id)

    @property
    def expanded_stats(self) -> minqlxtended.PlayerExpandedStats | None:
        """Live per-match stats as a ``PlayerExpandedStats``. Per-weapon members are
        ``Weapons`` sequences, e.g. ``player.expanded_stats.shots_hit.rg``."""
        return minqlxtended.player_expanded_stats(self.id)

    @property
    def demo_status(self) -> minqlxtended.DemoStatus:
        """Demo state as a ``DemoStatus`` of ``(recording, requested, path)``. While
        recording, the bytes are in ``path + ".part"``; the final size comes with
        ``demo_finished``. Game thread only, so marshal with
        :func:`minqlxtended.next_frame`."""
        return minqlxtended.demo_status(self.id)

    def record_demo(self) -> bool:
        """Records this player regardless of ``sv_demoRecord``. A demo has to begin at a
        gamestate, so for a player already in the game this takes effect at their next one.
        Game thread only, so marshal with :func:`minqlxtended.next_frame`.

        :returns: bool -- True if a demo is already being written, False if it's only queued.
        """
        return minqlxtended.start_demo(self.id)

    def stop_demo(self) -> bool:
        """Finalises any open demo, firing ``demo_finished``, and suppresses recording for
        this slot until they disconnect even if ``sv_demoRecord`` is on. Game thread only,
        so marshal with :func:`minqlxtended.next_frame`.

        :returns: bool -- True if a demo was actually being written.
        """
        return minqlxtended.stop_demo(self.id)

    @property
    def is_alive(self) -> bool:
        return self._live_state.is_alive

    @is_alive.setter
    def is_alive(self, value: Any) -> None:
        if not isinstance(value, bool):
            raise ValueError("is_alive needs to be a boolean.")

        cur = self.is_alive
        if cur and value is False:
            self.slay_with_mod(Mod.SUICIDE)
        elif not cur and value is True:
            minqlxtended.player_spawn(self.id)

    @property
    def is_frozen(self) -> bool:
        return self._live_state.is_frozen

    @property
    def is_bot(self):
        """Whether this is a bot.

        Reads SVF_BOT from r.sv_flags, the test the engine uses.

        .. warning::
            Not valid during a ``player_connect`` handler. ``SVF_BOT`` is set inside
            ``ClientConnect``, which runs after that event, so a bot reports as human.
            Use the ``is_bot`` argument the event passes.

        """
        return bool(self.entity.r.sv_flags & ServerFlag.BOT)

    @property
    def score(self) -> int:
        return self._live_stats.score

    @score.setter
    def score(self, value: int) -> None:
        self.gclient.ps.persistant[PersistantIndex.ROUND_SCORE] = value

    @property
    def channel(self):
        return minqlxtended.TellChannel(self)

    def center_print(self, msg: str) -> None:
        return _center_print(self.id, msg)

    def tell(self, msg, **kwargs):
        return minqlxtended.Plugin.tell(msg, self, **kwargs)

    def send_server_command(self, cmd: str) -> bool:
        """Send this player a server command. See
        :func:`minqlxtended.send_server_command`, which this calls with their client id."""
        return minqlxtended.send_server_command(self.id, cmd)

    def client_command(self, cmd: str) -> bool:
        """Make this player's client run *cmd*, as though they had typed it.

        Goes through the ``client_command`` event and any plugins hooking it. See
        :func:`minqlxtended.client_command`.
        """
        return minqlxtended.client_command(self.id, cmd)

    def play_sound(self, sound_path: str) -> None:
        """Play a sound file to this player alone.

        :raises: ValueError -- if *sound_path* is empty or names a music file.
        """
        return minqlxtended.Plugin.play_sound(sound_path, self)

    def play_music(self, music_path: str) -> None:
        """Play a music file to this player alone.

        :raises: ValueError -- if *music_path* is empty or names a sound file.
        """
        return minqlxtended.Plugin.play_music(music_path, self)

    def stop_sound(self) -> None:
        """Stop whatever sounds this player has going."""
        return minqlxtended.Plugin.stop_sound(self)

    def stop_music(self) -> None:
        """Stop whatever music this player has going."""
        return minqlxtended.Plugin.stop_music(self)

    def send_configstring(self, index: int, value: str) -> None:
        """Send this player a configstring only they see.

        The server's table is untouched, so a later server-side write to *index* reaches
        this player and overwrites it. See
        :meth:`minqlxtended.Plugin.send_configstring_to`. Unrelated to
        :attr:`configstring`, the ``CS_PLAYERS`` entry describing this player.
        """
        return minqlxtended.Plugin.send_configstring_to(self.id, index, value)

    def send_configstring_overrides(self, index: int,
                                    overrides: Mapping[str, str | None]) -> bool:
        """Send this player a configstring with *overrides* applied to the server's value.

        The read-modify-write form of :meth:`send_configstring`, for infostrings: it
        replaces keys rather than appending them. Returns False if nothing would change.
        See :meth:`minqlxtended.Plugin.send_player_configstring`.
        """
        return minqlxtended.Plugin.send_player_configstring(self.id, index, overrides)

    def kick(self, reason: str = "") -> None:
        return minqlxtended.Game.kick(self, reason)

    def ban(self) -> None:
        return minqlxtended.Game.ban(self)

    def tempban(self) -> None:
        return minqlxtended.Game.tempban(self)

    def addadmin(self) -> None:
        return minqlxtended.Game.addadmin(self)

    def addmod(self) -> None:
        return minqlxtended.Game.addmod(self)

    def demote(self) -> None:
        return minqlxtended.Game.demote(self)

    def mute(self) -> None:
        return minqlxtended.Game.mute(self)

    def unmute(self) -> None:
        return minqlxtended.Game.unmute(self)

    def put(self, team: str) -> None:
        return minqlxtended.Game.put(self, team)

    def addscore(self, score: int) -> None:
        return minqlxtended.Game.addscore(self, score)

    def switch(self, other_player: "Player") -> None:
        return minqlxtended.Game.switch(self, other_player)

    def slap(self, damage: int = 0) -> None:
        return minqlxtended.Game.slap(self, damage)

    def slay(self) -> None:
        return minqlxtended.Game.slay(self)

    def slay_with_mod(self, mod: int) -> bool:
        return minqlxtended.slay_with_mod(self.id, mod)

    @classmethod
    def all_players(cls) -> list["Player"]:
        return [cls(i, info=info) for i, info in enumerate(minqlxtended.players_info()) if info]


class AbstractDummyPlayer(Player):
    """A player that occupies no client slot, for dispatching commands that didn't come
    from one. The console, usually.

    Anything that reads a client slot (position, health, weapons, stats, the
    configstring, the entity) raises NonexistentPlayerError.
    """
    def __init__(self, name: str = "DummyPlayer") -> None:
        info = minqlxtended.PlayerInfo((
            -1, name, ConnectionState.CONNECTED.index, _DUMMY_USERINFO, -1,
            Team.SPECTATOR.index, Privilege.NONE.level))
        super().__init__(-1, info=info)

    @property
    def id(self):
        raise minqlxtended.NonexistentPlayerError("Dummy players do not have client IDs.")

    @property
    def steam_id(self):
        raise NotImplementedError("steam_id property needs to be implemented.")

    @property
    def ip(self):
        return "127.0.0.1"

    @property
    def ping(self):
        return -1

    @property
    def qport(self):
        return -1

    @property
    def is_bot(self):
        return False

    def update(self):
        pass

    @property
    def channel(self):
        raise NotImplementedError("channel property needs to be implemented.")

    def tell(self, msg, **kwargs):
        raise NotImplementedError("tell() needs to be implemented.")


class RconDummyPlayer(AbstractDummyPlayer):
    def __init__(self):
        super().__init__(name=self.__class__.__name__)

    @property
    def steam_id(self):
        return minqlxtended.owner()

    @property
    def channel(self):
        return minqlxtended.CONSOLE_CHANNEL

    def tell(self, msg, **kwargs):
        self.channel.reply(msg, **kwargs)
