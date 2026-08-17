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

import random

from typing import Mapping

from ._core import _check_command_value
from ._enums import EntityEvent, GameState, Gametype, Mod, RoundState, Team

__all__ = ("Game", "NonexistentGameError")


def _team(value):
    """The :class:`Team` *value* names, or a ValueError that lists the real ones."""
    try:
        return Team(value)
    except ValueError:
        teams = ", ".join(repr(str(t)) for t in Team)
        raise ValueError(f"Invalid team: {value!r}. Valid teams are {teams}.") from None

def _client(player):
    """The client id *player* names, or a ValueError.

    Accepts whatever :meth:`Plugin.client_id` does: a Player, a client id, a SteamID64
    or a name.
    """
    cid = minqlxtended.Plugin.client_id(player)
    if cid is None:
        raise ValueError("Invalid player.")

    return cid

class NonexistentGameError(Exception):
    """An exception raised when accessing properties on an invalid game."""
    pass

def _cs0_vars() -> Mapping[str, str] | None:
    """Returns the parsed serverinfo configstring, or None when no game is active.

    Goes through the shared parse cache in `_configstring`, since configstring 0 is read
    on nearly every Game property access.
    """
    parsed = minqlxtended.configstring_variables(minqlxtended.CS_SERVERINFO)
    return parsed if parsed else None

class Game():
    """The game: what map is being played, whether it's in warmup, and so on. Also the
    methods for timeins, aborts and pauses."""
    def __init__(self) -> None:
        # No _valid flag. Game holds no per-instance engine state and re-reads the
        # configstring on every access, so there's nothing to invalidate.
        if not minqlxtended.configstring(minqlxtended.CS_SERVERINFO):
            raise NonexistentGameError("Tried to instantiate a game while no game is active.")

    def __repr__(self) -> str:
        try:
            return f"{self.__class__.__name__}({self.type_short}@{self.map})"
        except NonexistentGameError:
            return f"{self.__class__.__name__}(N/A@N/A)"

    def __str__(self) -> str:
        try:
            return f"{self.type} on {self.map}"
        except NonexistentGameError:
            return "Invalid game"

    def __contains__(self, key: str) -> bool:
        cvars = _cs0_vars()
        if cvars is None:
            raise NonexistentGameError("Invalid game. Is the server loading a new map?")

        return key in cvars

    def __getitem__(self, key: str) -> str:
        cvars = _cs0_vars()
        if cvars is None:
            raise NonexistentGameError("Invalid game. Is the server loading a new map?")

        return cvars[key]

    @property
    def cvars(self) -> dict[str, str]:
        """A dictionary of unprocessed cvars, for the ones with no attribute here."""
        cvars = _cs0_vars()
        if cvars is None:
            return dict(minqlxtended.parse_infostring(""))
        # A copy, so callers can't mutate the cached parse.
        return dict(cvars)

    @property
    def type(self) -> str:
        """The gametype's long name, e.g. "Capture the Flag"."""
        return self.type_short.title

    @property
    def type_short(self) -> Gametype:
        return Gametype.from_index(int(self["g_gametype"]))

    @property
    def is_team_based(self) -> bool:
        """Whether this gametype splits players into red and blue."""
        return self.type_short.is_team_based

    @property
    def map(self) -> str:
        """The short name of the map. Ex.: ``longestyard``."""
        return self["mapname"]

    @map.setter
    def map(self, value: str) -> None:
        self.change_map(value)

    @property
    def map_title(self) -> str:
        """The full name of the map. Ex.: ``Longest Yard``.

        Empty until the first map has loaded.
        """
        return minqlxtended.map_titles().title

    @property
    def map_subtitle1(self) -> str:
        """The map's subtitle. Usually either empty or has the author's name."""
        return minqlxtended.map_titles().subtitle1

    @property
    def map_subtitle2(self) -> str:
        """The map's second subtitle. Usually either empty or has the author's name."""
        return minqlxtended.map_titles().subtitle2

    @property
    def map_info(self) -> minqlxtended.MapInfo | None:
        """The current map as :func:`minqlxtended.map_info` reports it: its pk3 sources,
        .arena data and workshop id. None when the installed-map scan doesn't know the
        map. Reads from disk when that cache is cold, unlike everything else here.
        """
        return minqlxtended.map_info(self.map)

    @property
    def factory_info(self) -> minqlxtended.FactoryInfo | None:
        """The current factory as :func:`minqlxtended.factory_info` reports it, or None
        when no installed .factories file declares it. Reads from disk when the
        installed-map cache is cold, unlike everything else here.
        """
        return minqlxtended.factory_info(self.factory)

    @staticmethod
    def _level(attribute):
        try:
            return getattr(minqlxtended.level, attribute)
        except minqlxtended.EngineStateError:
            raise NonexistentGameError("Tried to read the level when no game is active.")

    @staticmethod
    def _set_level(attribute, value):
        """The write half of :meth:`_level`, converting the same way."""
        try:
            setattr(minqlxtended.level, attribute, value)
        except minqlxtended.EngineStateError:
            raise NonexistentGameError("Tried to write the level when no game is active.")

    @property
    def time(self) -> int:
        """The level clock in milliseconds. Stops advancing while the game is paused."""
        return self._level("time")

    @property
    def team_scores(self):
        """Scores as a 4-tuple, indexed by :attr:`minqlxtended.Team.index`.

        Straight off the game module's per-team array. CS_SCORES1 and CS_SCORES2 are "1st
        place" and "2nd place", which in a free-for-all are not teams.

        """
        return tuple(self._level("team_scores"))

    @property
    def round_number(self) -> int:
        """Which round a round-based gametype is on."""
        return self._level("round").round

    @property
    def round_state(self) -> RoundState:
        """The round state as a :class:`minqlxtended.RoundState`."""
        return RoundState.from_index(self._level("round_state"))

    @property
    def paused(self) -> bool:
        return self._level("time_pause_begin") != 0

    @property
    def players_playing(self) -> int:
        """How many clients are on a team, as the game module counts them."""
        return self._level("num_playing_clients")

    @property
    def players_ready(self) -> int:
        """How many clients have readied up during warmup."""
        return self._level("num_ready_clients")

    @property
    def forfeited(self) -> bool:
        """Whether the match ended without a result."""
        return self._level("match_forfeited")

    @property
    def unpause_time(self) -> int:
        """Level time the running timeout ends.

        0 while unpaused, and also during a pause with no timer on it, so read
        :attr:`paused` first to tell those apart.

        """
        return minqlxtended.match_state.unpause_time

    @property
    def pause_caller(self) -> int:
        """Client id that called the current pause.

        0 when the server called it, which is also client 0's id, so read
        :attr:`paused_by_server` before trusting it.

        """
        return minqlxtended.match_state.pause_caller

    @property
    def paused_by_server(self) -> bool:
        """Whether the server called the current pause, rather than a player."""
        return minqlxtended.match_state.paused_by_server

    @property
    def timeouts_used(self) -> tuple:
        """Timeouts each team has spent, indexed by :attr:`minqlxtended.Team.index`.

        The game module only ever writes the red and blue slots.

        """
        return tuple(minqlxtended.match_state.timeouts_used)

    @property
    def is_training_map(self) -> bool:
        """Whether the game module will let a single player start a match.

        Writable, and the same field as :attr:`minqlxtended.level.map_is_training_map`.

        """
        return self._level("map_is_training_map")

    @is_training_map.setter
    def is_training_map(self, value: bool) -> None:
        self._set_level("map_is_training_map", bool(value))

    @property
    def state(self) -> GameState:
        """The state of the game: *warmup*, *countdown* or *in_progress*.

        Read from ``level.warmup_time`` rather than the ``g_gameState`` cvar in
        configstring 0.
        """
        warmup = self._level("warmup_time")
        if warmup < 0:
            return GameState.WARMUP
        if warmup > 0:
            return GameState.COUNTDOWN

        return GameState.IN_PROGRESS

    @property
    def factory(self) -> str:
        return self["g_factory"]

    @factory.setter
    def factory(self, value: str) -> None:
        self.change_map(self.map, value)

    @property
    def factory_title(self) -> str:
        return self["g_factoryTitle"]

    @property
    def hostname(self) -> str:
        return self["sv_hostname"]

    @hostname.setter
    def hostname(self, value: str) -> None:
        minqlxtended.set_cvar("sv_hostname", str(value))

    @property
    def instagib(self) -> bool:
        return bool(int(self["g_instaGib"]))

    @instagib.setter
    def instagib(self, value: object) -> None:
        if isinstance(value, bool):
            minqlxtended.set_cvar("g_instaGib", str(int(value)))
        elif value == 0 or value == 1:
            minqlxtended.set_cvar("g_instaGib", "1" if value == 1 else "0")
        else:
            raise ValueError("instagib needs to be 0, 1, or a bool.")

    @property
    def loadout(self) -> bool:
        return bool(int(self["g_loadout"]))

    @loadout.setter
    def loadout(self, value: object) -> None:
        if isinstance(value, bool):
            minqlxtended.set_cvar("g_loadout", str(int(value)))
        elif value == 0 or value == 1:
            # See the instagib setter above for why this is not str(value).
            minqlxtended.set_cvar("g_loadout", "1" if value == 1 else "0")
        else:
            raise ValueError("loadout needs to be 0, 1, or a bool.")

    @property
    def maxclients(self) -> int:
        return int(self["sv_maxclients"])

    @maxclients.setter
    def maxclients(self, new_limit: int) -> None:
        minqlxtended.set_cvar("sv_maxclients", str(int(new_limit)))

    @property
    def timelimit(self) -> int:
        return int(self["timelimit"])

    @timelimit.setter
    def timelimit(self, new_limit: int) -> None:
        minqlxtended.set_cvar("timelimit", str(int(new_limit)))

    @property
    def fraglimit(self) -> int:
        return int(self["fraglimit"])

    @fraglimit.setter
    def fraglimit(self, new_limit: int) -> None:
        minqlxtended.set_cvar("fraglimit", str(int(new_limit)))

    @property
    def roundlimit(self) -> int:
        return int(self["roundlimit"])

    @roundlimit.setter
    def roundlimit(self, new_limit: int) -> None:
        minqlxtended.set_cvar("roundlimit", str(int(new_limit)))

    @property
    def roundtimelimit(self) -> int:
        return int(self["roundtimelimit"])

    @roundtimelimit.setter
    def roundtimelimit(self, new_limit: int) -> None:
        minqlxtended.set_cvar("roundtimelimit", str(int(new_limit)))

    @property
    def scorelimit(self) -> int:
        return int(self["scorelimit"])

    @scorelimit.setter
    def scorelimit(self, new_limit: int) -> None:
        minqlxtended.set_cvar("scorelimit", str(int(new_limit)))

    @property
    def capturelimit(self) -> int:
        return int(self["capturelimit"])

    @capturelimit.setter
    def capturelimit(self, new_limit: int) -> None:
        minqlxtended.set_cvar("capturelimit", str(int(new_limit)))

    @property
    def teamsize(self) -> int:
        return int(self["teamsize"])

    @teamsize.setter
    def teamsize(self, new_size: int) -> None:
        minqlxtended.set_cvar("teamsize", str(int(new_size)))

    @property
    def tags(self) -> list[str]:
        return self["sv_tags"].split(",") if "sv_tags" in self else []

    @tags.setter
    def tags(self, new_tags: object) -> None:
        if isinstance(new_tags, str):
            minqlxtended.set_cvar("sv_tags", new_tags)
        elif hasattr(new_tags, "__iter__"):
            minqlxtended.set_cvar("sv_tags", ",".join(new_tags))
        else:
            raise ValueError("tags need to be a string or an iterable returning strings.")

    @property
    def workshop_items(self) -> list[int]:
        return [int(i) for i in minqlxtended.configstring(minqlxtended.CS_STEAM_WORKSHOP_IDS).split()]

    @workshop_items.setter
    def workshop_items(self, new_items: object) -> None:
        if hasattr(new_items, "__iter__"):
            minqlxtended.set_configstring(minqlxtended.CS_STEAM_WORKSHOP_IDS, " ".join([str(i) for i in new_items]) + " ")
        else:
            raise ValueError("The value needs to be an iterable.")

    @classmethod
    def change_map(cls, new_map, factory=None):
        """Load *new_map*, optionally with a different factory.

        The one place the ``map`` command is built. Returns before the map has loaded: the
        command goes to the engine's command buffer. Hook ``map`` or ``new_game`` to act on
        the new level.

        :raises ValueError: if either name carries a character a command line can't hold.
        """
        _check_command_value(new_map, "Map name")
        if factory:
            checked = _check_command_value(factory, "Factory name")
            return minqlxtended.console_command(f"map {new_map} {checked}")

        return minqlxtended.console_command(f"map {new_map}")

    @classmethod
    def shuffle(cls):
        minqlxtended.console_command("forceshuffle")

    # ADMIN COMMANDS

    @classmethod
    def timeout(cls):
        return minqlxtended.console_command("timeout")

    @classmethod
    def timein(cls):
        return minqlxtended.console_command("timein")

    @classmethod
    def allready(cls):
        return minqlxtended.console_command("allready")

    @classmethod
    def pause(cls):
        return minqlxtended.console_command("pause")

    @classmethod
    def unpause(cls):
        return minqlxtended.console_command("unpause")

    @classmethod
    def _set_team_locked(cls, team, locked):
        """Lock or unlock *team*, or red and blue together when it is None.

        Writing :attr:`minqlxtended.match_state.team_locked` calls the game module's own
        setter, so clients are told as the console command tells them.

        """
        if team is None:
            # The engine's own no-argument behaviour. It does *not* consult the gametype
            # the way the one-team form below does; MP_LockOrUnlockTeams calls the setter
            # for red and blue directly when there's no argument to parse.
            teams = (Team.RED, Team.BLUE)
        else:
            teams = (_team(team),)
            cls._check_team_is_playable(teams[0])

        locks = minqlxtended.match_state.team_locked
        for one in teams:
            locks[one.index] = locked

    @staticmethod
    def _check_team_is_playable(team):
        """Raise if *team* is not one this gametype has.

        MP_TeamID refuses red and blue outside a team gametype and free inside one, and
        calling the setter directly skips that check. A lock is not cleared between maps,
        so one set in a free-for-all is still there on the next team map. Spectator is
        accepted in every gametype.

        """
        if team is Team.SPECTATOR:
            return

        cvars = _cs0_vars()
        if cvars is None or "g_gametype" not in cvars:
            return  # Between maps. Nothing to check it against, so don't invent an answer.

        is_team_based = Gametype.from_index(int(cvars["g_gametype"])).is_team_based
        wanted_team_based = team is not Team.FREE
        if is_team_based != wanted_team_based:
            usable = "'red', 'blue' or 'spectator'" if is_team_based else "'free' or 'spectator'"
            raise ValueError(f"{str(team)!r} is not a team in this gametype. Use {usable}.")

    @classmethod
    def lock(cls, team=None):
        """Lock *team* against joining, or red and blue when it is None.

        :raises EngineStateError: if the game module's setter did not resolve in this build.
        """
        cls._set_team_locked(team, True)

    @classmethod
    def unlock(cls, team=None):
        """Unlock *team*, or red and blue when it is None."""
        cls._set_team_locked(team, False)

    @classmethod
    def is_team_locked(cls, team) -> bool:
        """Whether *team* is locked against joining, as :meth:`lock` sets it.

        Read off the game module's own flag, so it agrees with the engine however the lock
        was made. A lock outlives a map change.

        :param team: The team, as :meth:`lock` takes it.
        :returns: bool
        :raises EngineStateError: if the flag did not resolve in this build.

        """
        return bool(minqlxtended.match_state.team_locked[_team(team).index])

    @property
    def locked_teams(self) -> tuple:
        """Every locked team, as a tuple of :class:`minqlxtended.Team`.

        Only red and blue gate joining; ``MP_AllowJoin`` never consults free or spectator,
        so a lock on either is recorded but inert.

        """
        locks = minqlxtended.match_state.team_locked
        return tuple(team for team in Team if locks[team.index])

    @classmethod
    def put(cls, player, team):
        return minqlxtended.console_command(f"put {_client(player)} {_team(team).value}")

    @classmethod
    def mute(cls, player):
        return minqlxtended.console_command(f"mute {_client(player)}")

    @classmethod
    def unmute(cls, player):
        return minqlxtended.console_command(f"unmute {_client(player)}")

    @classmethod
    def tempban(cls, player):
        return minqlxtended.console_command(f"tempban {_client(player)}")

    @classmethod
    def ban(cls, player):
        return minqlxtended.console_command(f"ban {_client(player)}")

    @classmethod
    def unban(cls, player):
        return minqlxtended.console_command(f"unban {_client(player)}")

    @classmethod
    def opsay(cls, msg):
        """Broadcast *msg* as an admin message.

        :raises ValueError: if *msg* carries a character a command line can't hold.
        """
        return minqlxtended.console_command(
            f"opsay {_check_command_value(msg, 'Message')}")

    @classmethod
    def addadmin(cls, player):
        return minqlxtended.console_command(f"addadmin {_client(player)}")

    @classmethod
    def addmod(cls, player):
        return minqlxtended.console_command(f"addmod {_client(player)}")

    @classmethod
    def demote(cls, player):
        return minqlxtended.console_command(f"demote {_client(player)}")

    @classmethod
    def abort(cls):
        return minqlxtended.console_command("map_restart")

    @classmethod
    def addscore(cls, player, score):
        return minqlxtended.console_command(
            f"addscore {_client(player)} {score}")

    @classmethod
    def addteamscore(cls, team, score):
        return minqlxtended.console_command(
            f"addteamscore {_team(team).value} {score}")

    @classmethod
    def setmatchtime(cls, time):
        return minqlxtended.console_command(f"setmatchtime {time}")

    @classmethod
    def kick(cls, player, reason=""):
        # Not console_command: a reason is arbitrary text and would need quoting.
        return minqlxtended.kick(_client(player), reason or None)

    @classmethod
    def slap(cls, player, damage=0):
        """Throw *player* into the air, taking *damage* health off them on the way.

        Says nothing to anyone, so announce it yourself if you want the server told.

        :param player: The player to slap.
        :param damage: Health to take. 0, the default, slaps them unharmed.
        :type damage: int
        :raises ValueError: if there is nobody alive in that slot.
        """
        client_id = _client(player)
        entity = minqlxtended.Entity(client_id)
        if not entity.inuse or entity.client is None or entity.health <= 0:
            raise ValueError("The player is currently not active.")

        # ps.velocity hands back a Vector3, which is a tuple, so read and write it whole.
        state = entity.client.ps
        x, y, z = state.velocity
        state.velocity = (
            x + random.uniform(-1, 1) * 200,
            y + random.uniform(-1, 1) * 200,
            z + 300,
        )

        entity.health -= damage
        if entity.health > 0:
            # The client picks a pain sound from the health it is handed. 99 is pain100_1.
            minqlxtended.add_event(client_id, EntityEvent.PAIN, 99)
        else:
            minqlxtended.add_event(client_id, EntityEvent.DEATH1, entity.s.number)

    @classmethod
    def slay(cls, player):
        """Kill *player* where they stand.

        Through G_Damage, so the ``death`` and ``kill`` events both fire.
        """
        minqlxtended.slay_with_mod(_client(player), Mod.SUICIDE)

    @classmethod
    def switch(cls, player, other_player):
        """Swap two players' teams. Both have to be on a team, and on different ones."""
        p1 = minqlxtended.Plugin.player(player)
        p2 = minqlxtended.Plugin.player(other_player)

        if not p1:
            raise ValueError("The first player is invalid.")
        elif not p2:
            raise ValueError("The second player is invalid.")

        if p1.team == p2.team:
            raise ValueError("Both players are on the same team.")

        t1, t2 = p1.team, p2.team
        cls.put(p1, t2)
        cls.put(p2, t1)

    # center_print isn't here. It needs no level and works between maps, where Plugin.game
    # is None. Use Plugin.center_print to broadcast, Player.center_print for one client.
