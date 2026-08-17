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

import logging
from typing import Any, Self

import minqlxtended
import redis
import re
import time

# Reached as `minqlxtended.database`, so the package does not re-export these. The list
# holds `import *` and a type checker to the two names a plugin is meant to see.
__all__ = (
    "AbstractDatabase",
    "Redis",
)

_re_permission_key = re.compile(r"^minqlx:players:(?P<steam_id>\d+):permission$")

class AbstractDatabase:
    def __init__(self, plugin: Any) -> None:
        self.plugin = plugin

    @property
    def logger(self) -> logging.Logger:
        return minqlxtended.get_logger(self.plugin)

    def set_permission(self, player, level):
        """Abstract method. Should set the permission of a player.

        :param player: The player in question.
        :param level: The permission level to set.
        :raises: NotImplementedError

        """
        raise NotImplementedError("The base plugin can't do database actions.")

    def get_permission(self, player):
        """Abstract method. Should return the permission of a player.

        :returns: int
        :raises: NotImplementedError

        """
        raise NotImplementedError("The base plugin can't do database actions.")

    def has_permission(self, player: Any, level: int = 5) -> bool:
        """Abstract method. Should return whether the player has at least *level*, which
        runs 0 to 5, where 0 is always True.

        :returns: bool
        :raises: NotImplementedError

        """
        raise NotImplementedError("The base plugin can't do database actions.")

    def set_flag(self, player: Any, flag: str, value: bool = True) -> None:
        """Abstract method. Should set specified player flag to value.

        :raises: NotImplementedError

        """
        raise NotImplementedError("The base plugin can't do database actions.")

    def clear_flag(self, player: Any, flag: str) -> None:
        """Should clear specified player flag."""
        return self.set_flag(player, flag, False)

    def get_flag(self, player: Any, flag: str, default: bool = False) -> bool:
        """Abstract method. Should return specified player flag

        :returns: bool
        :raises: NotImplementedError

        """
        raise NotImplementedError("The base plugin can't do database actions.")

    def get_flags(self, players, flag, default=False):
        """Abstract method. Should return the same flag for many players at once.

        :returns: dict -- steam_id -> bool
        :raises: NotImplementedError

        """
        raise NotImplementedError("The base plugin can't do database actions.")

    def connect(self):
        """Abstract method. Should return a connection to the database.

        :raises: NotImplementedError

        """
        raise NotImplementedError("The base plugin can't do database actions.")

    def close(self):
        """Abstract method. If the database has a connection state, this method should
        close the connection.

        :raises: NotImplementedError

        """
        raise NotImplementedError("The base plugin can't do database actions.")

class Redis(AbstractDatabase):
    """A subclass of :class:`minqlxtended.AbstractDatabase` providing support for Redis."""

    # We only use the instance-level ones if we override the URI from the config.
    _conn = None
    _pool = None
    _pass = ""

    # get_permission runs on the game thread for every permissioned command, where a Redis
    # round-trip is around 150µs. Class-level, so every plugin's database instance shares
    # one cache of steam_id -> (level, expires_at). set_permission evicts, so !setperm is
    # visible immediately and the expiry only picks up edits made outside the process.
    _permissions: dict[int, tuple[int, float]] = {}
    _permission_cache_time = None

    # Expired entries are only ignored at read time, so unbounded the dict grows by one
    # tuple per distinct visitor for the life of the process, into a permanent generation
    # _core's gc.freeze() never rescans. Only sv_maxclients entries ever matter.
    _PERMISSION_CACHE_MAX = 512

    @classmethod
    def _prune_permissions(cls, now):
        """Evict expired permission entries, then the soonest-to-expire if still over."""
        expired = [sid for sid, entry in Redis._permissions.items() if entry[1] <= now]
        for steam_id in expired:
            del Redis._permissions[steam_id]

        target = cls._PERMISSION_CACHE_MAX * 3 // 4
        over = len(Redis._permissions) - target
        if over > 0:
            by_expiry = sorted(Redis._permissions.items(), key=lambda item: item[1][1])
            for steam_id, _ in by_expiry[:over]:
                del Redis._permissions[steam_id]

    # Without this, an outage costs a full traceback per permissioned chat line.
    _OUTAGE_LOG_INTERVAL = 30.0
    # None until the first one is logged. time.monotonic() counts from boot on Linux, so a
    # seed of 0.0 would silence warnings for the first half minute of uptime.
    _last_outage_log: float | None = None

    def _log_outage(self, what, exc):
        now = time.monotonic()
        if (Redis._last_outage_log is not None
                and now - Redis._last_outage_log < Redis._OUTAGE_LOG_INTERVAL):
            return
        Redis._last_outage_log = now
        self.logger.warning("Redis is not answering; %s failed: %s", what, exc)

    @classmethod
    def _permission_ttl(cls):
        # Memoised on Redis itself so a subclass doesn't end up with its own
        # copy that a reset of the base class can't reach.
        if Redis._permission_cache_time is None:
            value = minqlxtended.get_cvar("qlx_permissionCacheTime")
            try:
                Redis._permission_cache_time = float(value) if value else 30.0
            except (TypeError, ValueError):
                Redis._permission_cache_time = 30.0
        return Redis._permission_cache_time

    @classmethod
    def invalidate_permission_cache(cls, steam_id=None):
        """Drop a cached permission level, or the whole cache when given nothing."""
        if steam_id is None:
            Redis._permissions.clear()
        else:
            Redis._permissions.pop(int(steam_id), None)

    def __contains__(self, key: str) -> bool:
        return self.r.exists(key)

    def __getitem__(self, key: str) -> Any:
        res = self.r.get(key)
        if res is None:
            raise KeyError(f"The key '{key}' is not present in the database.")
        else:
            return res

    def __setitem__(self, key: str, item: Any) -> None:
        res = self.r.set(key, item)
        if res is False:
            raise RuntimeError("The database assignment failed.")
        self._evict_if_permission_key(key)

    def __delitem__(self, key: str) -> None:
        res = self.r.delete(key)
        self._evict_if_permission_key(key)
        if res == 0:
            raise KeyError(f"The key '{key}' is not present in the database.")

    # The forwarded redis-py commands that write a key. `self.db.set(...)` is the same
    # object plugins use for everything else, so a permission written that way has to
    # evict the same as one written through the mapping interface.
    _EVICTING_COMMANDS = frozenset(("set", "setex", "setnx", "getset", "mset", "delete", "unlink"))

    @classmethod
    def _evict_if_permission_key(cls, key):
        """Plugins write permissions through the mapping interface as often as
        through set_permission, so both paths have to invalidate."""
        match = _re_permission_key.match(key) if isinstance(key, str) else None
        if match:
            cls.invalidate_permission_cache(match.group("steam_id"))

    @classmethod
    def _evict_from_args(cls, args):
        """Evict for every key among a write command's positional arguments."""
        for arg in args:
            if isinstance(arg, dict):  # mset takes a mapping
                for key in arg:
                    cls._evict_if_permission_key(key)
            else:
                cls._evict_if_permission_key(arg)

    def __enter__(self) -> Self:
        """Usable with `with`; the connection is closed on the way out."""
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def __getattr__(self, attr: str) -> Any:
        # Anything not defined here is forwarded to the connection, so `self.db.zadd(...)`
        # works. Dunder lookups are refused, or copy, pickle and the interpreter's protocol
        # probes would open a connection to ask redis whether it has a __deepcopy__.
        if attr.startswith("__") and attr.endswith("__"):
            raise AttributeError(attr)

        forwarded = getattr(self.r, attr)
        if attr in Redis._EVICTING_COMMANDS and callable(forwarded):
            def evicting(*args, **kwargs):
                result = forwarded(*args, **kwargs)
                Redis._evict_from_args(args)
                return result
            return evicting

        return forwarded

    def __dir__(self) -> list[str]:
        """Include the forwarded connection's attributes, so completion and dir() show
        what `__getattr__` will actually answer to."""
        own = set(super().__dir__())
        try:
            return sorted(own | set(dir(self.r)))
        except Exception:
            # Never connected, or the connection is gone. Our own names are still true.
            return sorted(own)

    @property
    def r(self):
        return self.connect()

    def set_permission(self, player, level):
        """Sets the permission of a player.

        :param player: The player in question.
        :type player: minqlxtended.Player

        """
        if isinstance(player, minqlxtended.Player):
            steam_id = player.steam_id
        else:
            steam_id = player

        # __setitem__ evicts through _evict_if_permission_key, so the cache is already
        # dealt with. Write through, don't update, so a failed write can't leave the cache
        # claiming a level the database doesn't have.
        self[f"minqlx:players:{steam_id}:permission"] = level

    def get_permission(self, player):
        """Gets the permission of a player.

        :param player: The player in question.
        :type player: minqlxtended.Player, int
        :returns: int

        """
        if isinstance(player, minqlxtended.Player):
            steam_id = player.steam_id
        elif isinstance(player, int):
            steam_id = player
        elif isinstance(player, str):
            steam_id = int(player)
        else:
            raise ValueError("Invalid player. Use either a minqlxtended.Player instance or a SteamID64.")

        # If it's the owner, treat it like a 5. Ahead of the cache, since a cvar decides
        # the owner and the database has no say in it.
        if steam_id == minqlxtended.owner():
            return 5

        # The cache is class-level and keyed by steam_id alone, so an instance pointed at
        # another host sits it out rather than trading levels with the configured database.
        cacheable = self.__dict__.get("_conn") is None
        ttl = self._permission_ttl() if cacheable else 0
        now = time.monotonic()
        if ttl > 0:
            cached = Redis._permissions.get(steam_id)
            if cached is not None and cached[1] > now:
                return cached[0]

        key = f"minqlx:players:{steam_id}:permission"
        try:
            perm = self[key]
        except KeyError:
            perm = "0"
        except redis.exceptions.RedisError as e:
            # Fail closed, and don't cache it. Serving the last known level while the
            # database is unreachable would keep granting commands an admin may have
            # just taken away.
            self._log_outage("a permission lookup", e)
            return 0

        try:
            level = int(perm)
        except (TypeError, ValueError):
            self.logger.warning("Ignoring non-numeric permission %r for %s.", perm, steam_id)
            level = 0

        if ttl > 0:
            if len(Redis._permissions) >= Redis._PERMISSION_CACHE_MAX:
                Redis._prune_permissions(now)
            Redis._permissions[steam_id] = (level, now + ttl)
        return level

    def has_permission(self, player, level=5):
        """Checks if the player has higher than or equal to *level*.

        :param player: The player in question.
        :type player: minqlxtended.Player
        :param level: The permission level to check for.
        :type level: int
        :returns: bool

        """
        return self.get_permission(player) >= level

    def set_flag(self, player, flag, value=True):
        """Sets specified player flag

        :param player: The player in question.
        :type player: minqlxtended.Player
        :param flag: The flag to set.
        :type flag: string
        :param value: (optional, default=True) Value to set
        :type value: bool

        """
        if isinstance(player, minqlxtended.Player):
            key = f"minqlx:players:{player.steam_id}:flags:{flag}"
        else:
            key = f"minqlx:players:{player}:flags:{flag}"

        self[key] = 1 if value else 0

    def get_flag(self, player, flag, default=False):
        """Gets the specified player flag

        :param player: The player in question.
        :type player: minqlxtended.Player
        :param flag: The flag to get
        :type flag: string
        :param default: (optional, default=False) The value to return if the flag is unknown
        :type default: bool

        """
        if isinstance(player, minqlxtended.Player):
            key = f"minqlx:players:{player.steam_id}:flags:{flag}"
        else:
            key = f"minqlx:players:{player}:flags:{flag}"

        try:
            return bool(int(self[key]))
        except KeyError:
            return default
        except redis.exceptions.RedisError as e:
            self._log_outage("a flag lookup", e)
            return default

    def get_flags(self, players, flag, default=False):
        """Read the same flag for many players in a single round-trip.

        One MGET, where :meth:`get_flag` is one GET per player.

        :param players: The players to look up.
        :type players: list of minqlxtended.Player (or SteamID64s)
        :param flag: The flag to get.
        :type flag: string
        :param default: (optional, default=False) The value to use for players whose
            flag is unset or unreadable.
        :type default: bool
        :returns: dict -- SteamID64 -> bool, in the order the players were given.

        """
        # A getattr, so anything carrying a steam_id works and bare SteamID64s fall
        # through unchanged.
        steam_ids = [getattr(p, "steam_id", p) for p in players]
        if not steam_ids:
            return {}

        keys = [f"minqlx:players:{steam_id}:flags:{flag}" for steam_id in steam_ids]
        try:
            values = self.r.mget(keys)
        except redis.exceptions.RedisError as e:
            self._log_outage("a flag lookup", e)
            return {steam_id: default for steam_id in steam_ids}

        result = {}
        for steam_id, value in zip(steam_ids, values):
            if value is None:
                result[steam_id] = default
                continue
            try:
                result[steam_id] = bool(int(value))
            except (TypeError, ValueError):
                result[steam_id] = default
        return result

    def connect(self, host=None, database=0, unix_socket=False, password=None, protocol=3):
        """Returns a connection to a Redis database.

        With *host* None the config settings apply, the rest of the arguments are ignored,
        and the connection is shared by every plugin. Passing *host* gives an unshared
        connection. Later calls return the first call's connection until it is closed.

        :param host: The host name. If no port is specified, it will use 6379. Ex.: ``localhost:1234``.
        :type host: str
        :param database: The database number that should be used.
        :type database: int
        :param unix_socket: Whether or not *host* should be interpreted as a unix socket path.
        :type unix_socket: bool
        :param password: The password to the database as defined in Redis's `requirepass` directive.
        :type password: str
        :param protocol: The RESP protocol to use when connecting to the Redis database. Accepts either 2 or 3.
        :type protocol: int
        :raises: RuntimeError

        """

        # Fast path, since self.r calls this with no host on every mapping access. An
        # instance that overrode the host must get its own connection back: handing it
        # Redis._conn would redirect its reads and writes to the default database.
        if not host:
            if self._conn:
                return self._conn
            if Redis._conn:
                return Redis._conn

        if not host: # use the configuration defined in CVARs
            address = minqlxtended.get_cvar("qlx_redisAddress")
            unix_socket = bool(int(minqlxtended.get_cvar("qlx_redisUnixSocket")))
            database = int(minqlxtended.get_cvar("qlx_redisDatabase"))
            Redis._pass = minqlxtended.get_cvar("qlx_redisPassword")
            protocol = int(minqlxtended.get_cvar("qlx_redisProtocol"))
            password = Redis._pass
        else: # connect to a specific, non-shared database
            address = host

        address = address.split(":")
        # redis-py defaults every one of these to "wait forever", so a host that stops
        # answering without resetting the connection blocks the frame loop until the process
        # is killed. initialize_cvars creates the cvar, but connect() is reachable from a
        # plugin before that, so read it defensively. "0" restores redis-py's behaviour.
        try:
            timeout = float(minqlxtended.get_cvar("qlx_redisTimeout") or 0.5)
        except (TypeError, ValueError):
            timeout = 0.5
        timeout = timeout if timeout > 0 else None
        connection_kwargs = {
            "db": database,
            "password": password,
            "decode_responses": True,
            "socket_timeout": timeout,
            "socket_connect_timeout": timeout,
            # Off so qlx_redisTimeout is the whole budget: redis-py reads this as
            # Retry(NoBackoff(), 1) and retries once, doubling the worst case. The connection
            # is dropped on error anyway, so the next command reconnects.
            "retry_on_timeout": False,
            "health_check_interval": 30,
        }
        # Only the transport actually in use. redis-py>=5 forwards these straight to the
        # connection class, and a TCP Connection rejects unix_socket_path and vice versa.
        # socket_keepalive lives on Connection alone.
        if unix_socket:
            connection_kwargs["unix_socket_path"] = address[0]
        else:
            connection_kwargs["host"] = address[0]
            connection_kwargs["port"] = int(address[1]) if len(address) > 1 else 6379
            connection_kwargs["socket_keepalive"] = True

        # redis-py >= 5.0 is required; StrictRedis is deprecated there and `protocol` is
        # only understood from that version on.
        connection_kwargs["protocol"] = protocol
        redis_instance = redis.Redis

        # redis-py's own default is 2**31. Every caller is either the game thread or a
        # @thread worker, so a pool that large only ever hides a leak.
        connection_kwargs["max_connections"] = 32

        if not host:
            if not Redis._conn:
                if unix_socket:
                    Redis._conn = redis_instance(**connection_kwargs)
                else:
                    Redis._pool = redis.ConnectionPool(**connection_kwargs)
                    Redis._conn = redis_instance(connection_pool=Redis._pool, decode_responses=True)
            return Redis._conn

        # Out of the instance dict, the way close() reads it. _conn is a class attribute, so
        # `self._conn` is Redis._conn on an instance that never overrode the host, and testing
        # that would hand this call the shared connection to the configured database.
        if self.__dict__.get("_conn") is None:
            if unix_socket:
                self._conn = redis_instance(**connection_kwargs)
            else:
                self._pool = redis.ConnectionPool(**connection_kwargs)
                self._conn = redis_instance(connection_pool=self._pool, decode_responses=True)
        return self._conn


    def close(self):
        """Close this instance's own connection, which it only has if it was pointed at a
        host other than the configured one. The shared default connection is untouched;
        use :meth:`close_shared` for that.

        """
        # Read out of the instance dict directly. _conn and _pool are class attributes, so on
        # an instance that never overrode the host `self._conn` is Redis._conn, and going
        # through the attribute would disconnect the shared pool, in-use connections included.
        conn = self.__dict__.get("_conn")
        pool = self.__dict__.get("_pool")
        if conn is None:
            return

        self._conn = None
        if pool is not None:
            pool.disconnect()
            self._pool = None

    @classmethod
    def close_shared(cls):
        """Close the connection every plugin shares. Nothing reopens it implicitly, so
        this is for shutdown rather than housekeeping."""
        if Redis._conn:
            Redis._conn = None
            if Redis._pool:
                Redis._pool.disconnect()
                Redis._pool = None
