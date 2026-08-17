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
import functools
import random
import logging
import threading
import typing
from typing import Any, Callable, Iterable, Mapping, Sequence

from ._commands import center_print as _center_print
from ._core import _QUOTED_FORBIDDEN, _check_command_value
from ._core import set_cvar_limit_once as _set_cvar_limit_once
from ._core import set_cvar_once as _set_cvar_once
from ._enums import Priority

if typing.TYPE_CHECKING:
    from ._core import TimerHandle
    from ._player import Player

__all__ = ("Identifier", "Plugin")


def _deliver_result(plugin, callback, result):
    callback(result)


class Identifier(typing.NamedTuple):
    """What :meth:`Plugin.resolve_identifier` gives back."""

    #: Their SteamID64, whether or not they are connected.
    steam_id: int
    #: Their name if they are connected, otherwise the steam id as a string.
    name: str
    #: The :class:`minqlxtended.Player`, or None if they are not connected.
    player: minqlxtended.Player | None


class Plugin:
    """The base plugin class.

    Every plugin must inherit this or a subclass of this. The *database* static variable
    must be a subclass of :class:`minqlxtended.database.AbstractDatabase`, which requires
    only the permission methods. Any use of the database past those is database-specific.

    The cvar ``qlx_perm_<command>`` overrides the permission level a command's chat form
    demands, and ``qlx_ccmd_perm_<command>`` the same for its client-command form. Both key
    on the command's primary name.

    Hooks and commands are registered from the handler with :func:`minqlxtended.hook` and
    :func:`minqlxtended.command`, or from ``__init__`` with :meth:`add_hook` and
    :meth:`add_command`. The two are interchangeable and can be mixed. Use the methods when
    the registration depends on something only known at runtime, like a cvar or the
    gametype.

    ::

        class my_plugin(minqlxtended.Plugin):

            @minqlxtended.hook("chat")
            def handle_chat(self, player, msg, channel, recipient):
                ...

            @minqlxtended.command("hello", usage="<name>")
            def cmd_hello(self, player, msg, channel):
                ...

    The admin commands (``lock``, ``pause``, ``tempban``, ``slap`` and the rest) live on
    :class:`minqlxtended.Game`, and thirteen of them are also on
    :class:`minqlxtended.Player`.

    .. warning::
        Blocking operations in code called by commands or events make players lag. Helper
        decorators like :func:`minqlxtended.thread` can be useful.

    """

    # Static dictionary of plugins currently loaded for the purpose of inter-plugin communication.
    _loaded_plugins: dict[str, Plugin] = {}

    # Populated by __new__, which is where registration happens so that a plugin
    # defining __init__ without super().__init__() still gets its decorated handlers.
    _hooks: list[tuple[str, Callable[..., Any], int]]
    _commands: list[Any]
    _tasks: set[Any]
    # The database driver class the plugin should use.
    # Set by late_init() to whichever driver qlx_database names.
    database: type | None = None

    # Set by unload_plugin. _run_frame_task checks it before delivering queued work,
    # so nothing fires into an instance that has been unloaded or replaced by a reload.
    _unloaded: bool = False

    # What this class declared with @minqlxtended.hook, @minqlxtended.command,
    # @minqlxtended.vote and minqlxtended.setting, filled in per subclass by
    # __init_subclass__. Plugin itself declares nothing.
    _declared_hooks = ()
    _declared_commands = ()
    _declared_votes = ()
    _declared_settings = ()

    def __init_subclass__(cls, **kwargs: Any) -> None:
        """Collect what the subclass declared with the registration decorators.

        Runs once per class. See :func:`minqlxtended._core._collect_declarations` for how
        an inherited declaration and an overridden decorated method resolve.

        """
        super().__init_subclass__(**kwargs)
        cls._declared_hooks = minqlxtended._core._collect_declarations(
            cls, minqlxtended._core._HOOK_ATTR)
        cls._declared_commands = minqlxtended._core._collect_declarations(
            cls, minqlxtended._core._COMMAND_ATTR)
        cls._declared_votes = minqlxtended._core._collect_declarations(
            cls, minqlxtended._core._VOTE_ATTR)
        cls._declared_settings = minqlxtended._core._collect_settings(cls)

    def __new__(cls, *args, **kwargs):
        """Register whatever the class declared with the decorators.

        Registration lives here because a plugin may define ``__init__`` without calling
        ``super().__init__()``. The handlers are live before the subclass' ``__init__`` body
        runs, and nothing can dispatch in between: plugins are constructed on the game
        thread, the only thread that raises events.

        """
        self = super().__new__(cls)
        self._hooks = []
        self._commands = []
        self._tasks = set()

        for attribute, entries in cls._declared_hooks:
            handler = getattr(self, attribute)
            for event, priority in entries:
                self.add_hook(event, handler, priority)

        for attribute, entries in cls._declared_commands:
            handler = getattr(self, attribute)
            for entry in entries:
                self.add_command(handler=handler, **entry)

        for attribute, entries in cls._declared_votes:
            handler = getattr(self, attribute)
            for entry in entries:
                self.add_vote(entry["name"], handler, entry["usage"], entry["description"])

        for declared_setting in cls._declared_settings:
            declared_setting.initialise(self)

        return self

    # No __init__. __new__ above creates _hooks and _commands from the decorated handlers,
    # so re-initialising them here would clobber registrations unload_plugin has to replay.

    def __str__(self) -> str:
        return self.name

    @functools.cached_property
    def db(self) -> Any:
        """The database instance, built on first use and cached."""
        if not self.database:
            raise RuntimeError(f"Plugin '{self.name}' does not have a database driver.")

        return self.database(self)

    @property
    def name(self) -> str:
        """The name of the plugin."""
        return self.__class__.__name__

    @property
    def is_loaded(self) -> bool:
        """Whether this instance is the live, loaded one.

        False once :func:`minqlxtended.unload_plugin` has run on it, including the unload
        half of a reload. A worker started with :func:`minqlxtended.thread` cannot be
        cancelled, so have any loop that shouldn't outlive its plugin check this and end
        itself.

        """
        return not self._unloaded and Plugin._loaded_plugins.get(self.name) is self

    @property
    def plugins(self) -> dict[str, "Plugin"]:
        """A dictionary containing plugin names as keys and plugin instances
        as values of all currently loaded plugins.

        """
        return self._loaded_plugins.copy()

    @property
    def hooks(self) -> list[tuple[str, Callable[..., Any], int]]:
        """A list of all the hooks this plugin has."""
        return self._hooks.copy()

    @property
    def commands(self) -> list["minqlxtended.Command"]:
        """A list of all the commands this plugin has registered."""
        return self._commands.copy()

    @property
    def game(self) -> minqlxtended.Game | None:
        """A Game instance."""
        try:
            return minqlxtended.Game()
        except minqlxtended.NonexistentGameError:
            return None

    @property
    def logger(self) -> logging.Logger:
        """An instance of :class:`logging.Logger`, but initialized for this plugin."""
        return minqlxtended.get_logger(self)

    def add_hook(self, event: str, handler: Callable[..., Any],
                 priority: int = Priority.NORMAL) -> None:
        """Hook an event, so *handler* is called every time it is raised.

        Everything registered here comes off again when the plugin is unloaded.

        :param event: The event name, as :data:`minqlxtended.EVENT_DISPATCHERS` keys it.
        :type event: str
        :param handler: Called with the event's arguments. Its signature is checked
            against the event's at registration.
        :param priority: Where in the handler chain this sits.
        :type priority: minqlxtended.Priority
        :raises KeyError: if *event* is not a known event name.
        :raises ValueError: if *priority* is not a valid level, or this handler is already
            hooked to the event at this priority.
        :raises AssertionError: if the event needs ZeroMQ stats and ``zmq_stats_enable``
            is zero.

        """
        # Register first, record second. A bad event name, a duplicate handler or a
        # zmq-gated event all raise out of add_hook, and a hook recorded but never
        # registered makes unload_plugin's replay raise for good.
        minqlxtended.EVENT_DISPATCHERS[event].add_hook(self.name, handler, priority)
        self._hooks.append((event, handler, priority))

    def remove_hook(self, event: str, handler: Callable[..., Any],
                    priority: int = Priority.NORMAL) -> None:
        """Unhook a handler registered with :meth:`add_hook`.

        The event, the handler and the priority must all match the registration.

        :param event: The event name.
        :type event: str
        :param handler: The handler that was registered.
        :param priority: The priority it was registered at.
        :type priority: minqlxtended.Priority
        :raises KeyError: if *event* is not a known event name.
        :raises ValueError: if the handler is not hooked to that event at that priority.

        """
        minqlxtended.EVENT_DISPATCHERS[event].remove_hook(self.name, handler, priority)
        # Tolerate a hook the plugin registered directly with the dispatcher. It came off
        # the dispatcher above, and that's all unload needs.
        try:
            self._hooks.remove((event, handler, priority))
        except ValueError:
            pass

    def add_command(
        self,
        name: str | Sequence[str],
        handler: Callable[..., Any],
        permission: int = 0,
        channels: Iterable[Any] | None = None,
        exclude_channels: Iterable[Any] = (),
        priority: int = Priority.NORMAL,
        client_cmd_pass: bool = False,
        client_cmd_perm: int = 5,
        prefix: bool = True,
        usage: str = "",
    ) -> None:
        """Register a command for this plugin.

        Takes the same arguments as the :func:`minqlxtended.command` decorator. Everything
        registered here comes off again when the plugin is unloaded::

            self.add_command(("teamsize", "ts"), self.cmd_teamsize, permission=2,
                             usage="<size>")

        The handler is called as ``handler(player, msg, channel)``, *msg* being the whole
        line split on whitespace. Returning :data:`minqlxtended.Return.USAGE` prints
        *usage* back to the caller.

        :param name: The command name, or a list of names for a command with aliases. The
            first is the primary one, which the permission cvars key on.
        :type name: str, list, tuple
        :param handler: Called as ``handler(player, msg, channel)``.
        :param permission: The level a player needs to run it from chat. 0 lets anyone.
            ``qlx_perm_<command>`` overrides it.
        :type permission: int
        :param channels: The channels this command answers in, as channel objects or their
            names. None or empty means all of them.
        :type channels: list, tuple, set
        :param exclude_channels: Channels this command never answers in. Takes precedence
            over *channels*.
        :type exclude_channels: list, tuple, set
        :param priority: Where in the dispatch order this sits when several commands share
            a name.
        :type priority: minqlxtended.Priority
        :param client_cmd_pass: Whether the client command still reaches the engine once
            the handler has run. False swallows it, sparing the player an "unknown cmd".
        :type client_cmd_pass: bool
        :param client_cmd_perm: The level a player needs to run it as a client command.
            ``qlx_ccmd_perm_<command>`` overrides it.
        :type client_cmd_perm: int
        :param prefix: Whether the typed name has to carry ``qlx_commandPrefix``.
        :type prefix: bool
        :param usage: The argument hint, e.g. ``"<size>"``.
        :type usage: str
        :raises ValueError: if *priority* is not a valid level, if the handler's signature
            isn't ``(player, msg, channel)``, or if the command is already registered.

        """
        cmd = minqlxtended.Command(self, name, handler, permission, channels, exclude_channels,
                                   client_cmd_pass, client_cmd_perm, prefix, usage)
        # Register first, record second, for the same reason as add_hook.
        minqlxtended.COMMANDS.add_command(cmd, priority)
        self._commands.append(cmd)

    def remove_command(self, name: Any, handler: Callable[..., Any]) -> None:
        """Unregister a command registered with :meth:`add_command`.

        A *name* and *handler* pair matching nothing is a no-op.

        :param name: Any one of the command's names, or the list it was registered with.
        :type name: str, list, tuple
        :param handler: The handler it was registered with.

        """
        # Command.names is always a lowercased list, so normalise the argument the same
        # way Command.__init__ does before comparing.
        if isinstance(name, (list, tuple)):
            names = [n.lower() for n in name]
        else:
            names = [name.lower()]

        # Any one of a command's names identifies it, and the whole registration goes,
        # aliases included.
        for cmd in self._commands[:]:
            if cmd.handler == handler and not set(names).isdisjoint(cmd.names):
                minqlxtended.COMMANDS.remove_command(cmd)
                self._commands.remove(cmd)

    def delay(self, interval: float, func: Callable[..., Any], *args: Any, **kwargs: Any) -> TimerHandle:
        """Call *func* once, *interval* seconds from now, and hand back the cancel.

        Cancellable through the returned handle, and cancelled when the plugin is unloaded.
        Runs on the game thread.

        :param interval: Seconds before the call.
        :type interval: int, float
        :param func: What to call. Further positional and keyword arguments are passed on.
        :returns: :class:`minqlxtended.TimerHandle`

        """
        return self._schedule_timer(interval, func, args, kwargs, repeating=False)

    def repeat(self, interval: float, func: Callable[..., Any], *args: Any, **kwargs: Any) -> TimerHandle:
        """Call *func* every *interval* seconds, the first call one interval from now.

        Cancelled through the returned handle, or at unload. Each fire carries up to a
        frame of slack, so don't use it for timekeeping.

        :param interval: Seconds between calls.
        :type interval: int, float
        :param func: What to call. Further positional and keyword arguments are passed on.
        :returns: :class:`minqlxtended.TimerHandle`
        :raises ValueError: if *interval* is not above zero.

        """
        if interval <= 0:
            raise ValueError("A repeat interval must be above zero.")
        return self._schedule_timer(interval, func, args, kwargs, repeating=True)

    def _schedule_timer(self, interval, func, args, kwargs, repeating):
        handle = minqlxtended.TimerHandle(interval, func, args, kwargs,
                                          repeating=repeating, owner=self)
        self._tasks.add(handle)
        handle._schedule()
        return handle

    def add_vote(self, name: str | Sequence[str], callback: Callable[..., Any],
                 usage: str = "", description: str = "") -> None:
        """Register a custom vote by name, server-wide.

        When a player calls the vote, *callback* runs as ``callback(caller, args)`` once
        every ``vote_called`` hook has had its say, so policy plugins can still veto it.
        Validate *args* in the callback, tell the caller about any problem and return None,
        or return a :class:`minqlxtended.CustomVote` to start the vote::

            self.add_vote("lgammo", self.vote_lgammo, usage="<count>",
                          description="Set starting lightning gun ammo.")

            def vote_lgammo(self, caller, args):
                try:
                    count = int(args)
                except ValueError:
                    caller.tell("Usage: /cv lgammo <count>")
                    return None
                return minqlxtended.CustomVote(
                    display=f"lg ammo {count}",
                    execute=lambda: self.set_cvar("g_startingAmmo_lg", count))

        The :func:`minqlxtended.vote` decorator spells the same registration on the
        callback itself.

        A vote name is unique across every plugin, and a second registration raises
        ValueError. Everything registered here is removed when the plugin is unloaded.

        :param name: The vote's name, what players put after ``/cv``, or a list of
            names for a vote with aliases.
        :type name: str, list, tuple
        :param callback: Called as ``callback(caller, args)`` on every attempt.
        :param usage: The argument hint for help listings, e.g. ``"<count>"``.
        :type usage: str
        :param description: One line for help listings.
        :type description: str
        :raises ValueError: if the name is already registered.

        """
        names = [name] if isinstance(name, str) else name
        for vote_name in names:
            minqlxtended.CUSTOM_VOTES.add_vote(self, vote_name, callback, usage, description)

    def remove_vote(self, name: str | Sequence[str]) -> None:
        """Unregister a custom vote registered with :meth:`add_vote`.

        Takes the names in the same shape :meth:`add_vote` takes them. Each alias is
        registered on its own, so naming one leaves the rest live. A vote is keyed by name
        alone, so this can unregister a vote another plugin owns.

        :param name: The vote's name, or the list of names it was registered with.
        :type name: str, list, tuple
        :raises ValueError: if a name is not registered.

        """
        names = [name] if isinstance(name, str) else name
        for vote_name in names:
            minqlxtended.CUSTOM_VOTES.remove_vote(vote_name)

    def run_in_thread(self, func: Callable[..., Any], *args: Any,
                      then: Callable[[Any], Any] | None = None, **kwargs: Any) -> threading.Thread | None:
        """Run ``func(*args, **kwargs)`` on a worker thread and hand what it returns
        to *then* on the game thread.

        Do the blocking work in *func*, then touch the engine from *then*. The delivery is
        dropped if the plugin has been unloaded by the time the result is in, and an
        exception out of *func* is logged with *then* left uncalled.

        :param func: What to run off-thread.
        :param then: Called on the game thread as ``then(result)``. Optional.
        :returns: The worker ``threading.Thread``, or None when already on a worker
            thread and the work ran inline, as :func:`minqlxtended.thread` does.

        """
        @minqlxtended.thread
        def _run_in_thread_worker():
            try:
                result = func(*args, **kwargs)
            except:
                minqlxtended.log_exception(self)
                return
            if then is not None:
                minqlxtended._handlers._queue_next_frame(
                    _deliver_result, (self, then, result), {})

        spawned = _run_in_thread_worker()
        return spawned if isinstance(spawned, threading.Thread) else None

    def unload(self) -> None:
        """Called as this instance is unloaded, while its state is still intact."""

    @classmethod
    def get_cvar(cls, name: str, return_type: type = str, default: Any = None) -> Any:
        """Gets the value of a cvar, converted to *return_type*.

        :param name: The name of the cvar.
        :type name: str
        :param return_type: The type the cvar should be returned in.
            Supported types: str, int, float, bool, list, set, tuple
        :param default: Returned as-is when the cvar is not set. It does not go through
            *return_type*, so pass it in the shape you want back. Defaults to None.
        :raises: ValueError -- if *return_type* is not one of the supported types, or if the
            cvar is set to something *return_type* cannot parse. The message names the cvar
            and the value in that case.

        """
        res = minqlxtended.get_cvar(name)
        if res is None:
            # Answered before the conversions below, or get_cvar("qlx_logs", int) surfaces
            # as a TypeError out of int(None) without naming the missing cvar.
            return default

        # Compared with `is`. These are type objects and identity is what's meant.
        if return_type is str:
            return res
        elif return_type is list:
            return [s.strip() for s in res.split(",")]
        elif return_type is set:
            return {s.strip() for s in res.split(",")}
        elif return_type is tuple:
            return tuple(s.strip() for s in res.split(","))
        elif return_type not in (int, float, bool):
            raise ValueError(f"Invalid return type: {return_type}")

        # An operator typo reaches here as int("") or int("0.5"). Left bare it propagates
        # through setting.refresh into _refresh_settings(), which shares a try with the
        # new_game dispatch, so one bad cvar stops new_game reaching any plugin.
        try:
            if return_type is int:
                return int(res)
            elif return_type is float:
                return float(res)
            return bool(int(res))
        except ValueError:
            raise ValueError(
                f"Cvar '{name}' is set to '{res}', which is not a valid {return_type.__name__}.") from None

    @classmethod
    def set_cvar(cls, name: str, value: Any, flags: int = 0) -> bool:
        """Sets a cvar. If the cvar exists, it will be set as if set from the console,
        otherwise create it.

        :param name: The name of the cvar.
        :type name: str
        :param value: The value of the cvar.
        :type value: Anything with an __str__ method.
        :param flags: The flags to set if, and only if, the cvar does not exist and has to be created.
        :type flags: int
        :returns: True if a new cvar was created, False if an existing cvar was set.
        :rtype: bool

        """
        # str(), since the engine setter takes a string and this is documented to take
        # anything with an __str__.
        if cls.get_cvar(name) is None:
            minqlxtended.set_cvar(name, str(value), flags)
            return True
        else:
            # Through the engine setter rather than a formatted console_command. A value
            # containing a quote would truncate or run as a further command.
            minqlxtended.set_cvar(name, str(value))
            return False

    @classmethod
    def set_cvar_limit(cls, name: str, value: Any, minimum: Any, maximum: Any,
                       flags: int = 0) -> bool:
        """Sets a cvar with upper and lower limits. If the cvar exists, it will be set
        as if set from the console, otherwise create it.

        :param name: The name of the cvar.
        :type name: str
        :param value: The value of the cvar.
        :type value: int, float
        :param minimum: The minimum value of the cvar.
        :type value: int, float
        :param maximum: The maximum value of the cvar.
        :type value: int, float
        :param flags: The flags to set if, and only if, the cvar does not exist and has to be created.
        :type flags: int
        :returns: True if a new cvar was created, False if an existing cvar was set.
        :rtype: bool

        .. note::
            *minimum* and *maximum* reach the engine only when this creates the cvar. On one
            that already exists, such as anything in ``server.cfg``, the value is set and the
            bounds are dropped, so :class:`minqlxtended.setting` clamps on the way out.

        """
        # As in set_cvar: the engine setters take strings, and this one is documented to
        # take int/float for all three of value, minimum and maximum.
        if cls.get_cvar(name) is None:
            minqlxtended.set_cvar_limit(name, str(value), str(minimum), str(maximum), flags)
            return True
        else:
            minqlxtended.set_cvar(name, str(value))
            return False

    @classmethod
    def set_cvar_once(cls, name: str, value: Any, flags: int = 0) -> bool:
        """Sets a cvar. If the cvar exists, do nothing.

        :param name: The name of the cvar.
        :type name: str
        :param value: The value of the cvar.
        :type value: Anything with an __str__ method.
        :param flags: The flags to set if, and only if, the cvar does not exist and has to be created.
        :type flags: int
        :returns: True if a new cvar was created, False if it already existed and was left alone.
        :rtype: bool

        """
        return _set_cvar_once(name, value, flags)

    @classmethod
    def set_cvar_limit_once(cls, name: str, value: Any, minimum: Any, maximum: Any,
                            flags: int = 0) -> bool:
        """Sets a cvar with upper and lower limits. If the cvar exists, do nothing.

        :param name: The name of the cvar.
        :type name: str
        :param value: The value of the cvar.
        :type value: int, float
        :param minimum: The minimum value of the cvar.
        :type value: int, float
        :param maximum: The maximum value of the cvar.
        :type value: int, float
        :param flags: The flags to set if, and only if, the cvar does not exist and has to be created.
        :type flags: int
        :returns: True if a new cvar was created, False if it already existed and was left alone.
        :rtype: bool

        """
        return _set_cvar_limit_once(name, value, minimum, maximum, flags)

    @classmethod
    def players(cls) -> list["Player"]:
        """Get a list of all the players on the server."""
        return minqlxtended.Player.all_players()

    @classmethod
    def player(cls, name: Any, player_list: list["Player"] | None = None) -> Player | None:
        """Get a Player instance from the name, client ID,
        or Steam ID. Assumes [0, MAX_CLIENTS) to be a client ID
        and [64, inf) to be a Steam ID.

        A Steam ID or a name that matches nobody returns None; a client ID in range whose
        slot is empty raises. :meth:`resolve_player` covers both.

        :raises: minqlxtended.NonexistentPlayerError -- if *name* is a client ID and that
            slot holds nobody.

        """
        # In case 'name' isn't a string.
        if isinstance(name, minqlxtended.Player):
            return name
        elif isinstance(name, int) and 0 <= name < minqlxtended.MAX_CLIENTS:
            return minqlxtended.Player(name)

        # `is None`, since an explicitly empty list means "search nobody" and a truthiness
        # test would turn that into "search everybody".
        if player_list is None:
            players = cls.players()
        else:
            players = player_list

        if isinstance(name, int) and name >= minqlxtended.MAX_CLIENTS:
            for p in players:
                if p.steam_id == name:
                    return p
        else:
            cid = cls.client_id(name, players)
            if cid is not None:
                for p in players:
                    if p.id == cid:
                        return p

        return None

    @classmethod
    def msg(cls, msg: Any, chat_channel: Any = None, **kwargs: Any) -> None:
        """Send a message to the chat, or any other channel.

        Defaults to the main chat channel.

        :param chat_channel: The channel to send to. None means the main chat channel.
        :type chat_channel: minqlxtended.AbstractChannel

        """
        if chat_channel is None:
            chat_channel = minqlxtended.CHAT_CHANNEL

        if not isinstance(chat_channel, minqlxtended.AbstractChannel):
            raise ValueError(f"Invalid channel: {chat_channel!r}. Pass a channel object, not a name.")

        chat_channel.reply(msg, **kwargs)

    @classmethod
    def clean_text(cls, text: str) -> str:
        """Removes color tags from text."""
        return minqlxtended.re_color_tag.sub("", text)

    @classmethod
    def client_id(cls, name: Any, player_list: list["Player"] | None = None) -> int | None:
        """Get a player's client id from the name, client ID,
        Player instance, or Steam ID. Assumes [0, MAX_CLIENTS) to be
        a client ID and [64, inf) to be a Steam ID.

        """
        if isinstance(name, int) and 0 <= name < minqlxtended.MAX_CLIENTS:
            return name
        elif isinstance(name, minqlxtended.Player):
            return name.id

        # `is None`, since an explicitly empty list means "search nobody" and a truthiness
        # test would turn that into "search everybody".
        if player_list is None:
            players = cls.players()
        else:
            players = player_list

        # Check Steam ID first, then name.
        if isinstance(name, int) and name >= minqlxtended.MAX_CLIENTS:
            for p in players:
                if p.steam_id == name:
                    return p.id
        else:
            clean = cls.clean_text(name).lower()
            for p in players:
                if p.clean_name.lower() == clean:
                    return p.id

        return None

    @classmethod
    def find_player(cls, name: str, player_list: list["Player"] | None = None) -> list["Player"]:
        """Find a player based on part of a players name.

        :param name: A part of someone's name.
        :type name: str
        :returns: A list of players that had that in their names.

        """
        # `is None`, since an explicitly empty list means "search nobody" and a truthiness
        # test would turn that into "search everybody".
        if player_list is None:
            players = cls.players()
        else:
            players = player_list

        if not name:
            return players

        # Hoisted, as in client_id. The search term doesn't change per player.
        needle = cls.clean_text(name).lower()
        res = []
        for p in players:
            if needle in p.clean_name.lower():
                res.append(p)

        return res

    @classmethod
    def teams(cls, player_list: list["Player"] | None = None) -> dict[Any, list["Player"]]:
        """Get a dictionary with the teams as keys and players as values."""
        # `is None`, since an explicitly empty list means "search nobody" and a truthiness
        # test would turn that into "search everybody".
        if player_list is None:
            players = cls.players()
        else:
            players = player_list

        res: dict[Any, list[Player]] = {team: [] for team in minqlxtended.Team}

        for p in players:
            res[p.team].append(p)

        return res

    @classmethod
    def center_print(cls, msg: Any) -> None:
        """Print a message in the middle of everybody's screen.

        Broadcast only; :meth:`Player.center_print` addresses one client. The two take
        their arguments the other way round from each other, so check which you have.
        """
        return _center_print(None, msg)

    # A single reliable command cannot exceed the engine's 1022-character limit, so a
    # longer configstring goes as bcs0/bcs1/bcs2 chunks the client reassembles. Leaves room
    # for the `bcs0 <index> ""` wrapper.
    BIG_CONFIGSTRING_CHUNK = 999

    @classmethod
    def send_configstring_to(cls, client_id: int, index: int, value: str) -> None:
        """Send a configstring to one client, whatever its length.

        Overrides what the client holds for `index` without touching the server's own
        configstring table. Anything over :data:`BIG_CONFIGSTRING_CHUNK` is split the same
        way `SV_SetConfigstring` splits it.

        Nothing here is remembered. A later server-side write to `index` reaches this
        client too and overwrites what it was sent, so an override that has to survive must
        be re-applied.

        :param client_id: The client to send to.
        :type client_id: int
        :param index: The configstring index.
        :type index: int
        :param value: The configstring value.
        :type value: str
        :raises ValueError: if *value* contains a quote, which would close the one this
            writes and leave the client the front half of the string.
        """
        _check_command_value(value, "Configstring value", _QUOTED_FORBIDDEN)
        chunk_size = cls.BIG_CONFIGSTRING_CHUNK
        if len(value) <= chunk_size:
            minqlxtended.send_server_command(client_id, f'cs {index} "{value}"\n')
            return

        # The same split SV_SetConfigstring makes, with bcs2 carrying the tail rather than
        # following it as an empty terminator. One fewer reliable command per send, on a
        # ring that holds 64 per client.
        offsets = range(0, len(value), chunk_size)
        last = len(offsets) - 1
        for i, offset in enumerate(offsets):
            if i == 0:
                command = "bcs0"
            elif i == last:
                command = "bcs2"
            else:
                command = "bcs1"
            minqlxtended.send_server_command(
                client_id, f'{command} {index} "{value[offset:offset + chunk_size]}"\n')

    @classmethod
    def plugin(cls, name: str) -> Plugin | None:
        """The loaded plugin instance with this name, or None.

        Prefer this over ``self.plugins[name]``: it copies no dict, it is indifferent to
        load order, and it picks up a reload.

        :param name: The plugin's name, as in its module and class name.
        :type name: str
        :returns: minqlxtended.Plugin or None
        """
        return cls._loaded_plugins.get(name)

    @classmethod
    def send_player_configstring(cls, client_id: int, index: int,
                                 overrides: Mapping[str, str | None]) -> bool:
        """Give one client a modified view of a configstring.

        Reads the server's current value, applies *overrides*, and sends the result to that
        client alone, chunked by :meth:`send_configstring_to` when it's too long for a
        single reliable command. The server's own configstring is untouched.

        Prefer this over concatenating ``\\key\\value`` onto the existing string: that
        appends a duplicate key on every call, growing the configstring without bound until
        it is truncated.

        :param client_id: The client to send to.
        :type client_id: int
        :param index: The configstring index.
        :type index: int
        :param overrides: Keys to set for this client. A value of None removes the key.
        :type overrides: dict
        :returns: bool -- False if the client's view would be identical to the
            server's, in which case nothing is sent.
        """
        # configstring_variables() is a read-only view onto the shared parse and
        # apply_variable_changes copies before merging, so private overrides cannot leak.
        variables, dirty = minqlxtended.apply_variable_changes(
            minqlxtended.configstring_variables(index), overrides)
        if not dirty:
            return False

        cls.send_configstring_to(client_id, index, minqlxtended.format_infostring(variables))
        return True

    # RESOLVING PLAYER ARGUMENTS
    #
    # Nearly every admin command takes "<id>", meaning either a client id or a SteamID64.
    # Use these rather than hand-rolling it around int() and player(). player() answers a
    # miss two ways and falls through to a by-name search, so "!slap 999" looks for a
    # player called "999".

    INVALID_ID = "Invalid ID. Use either a client ID or a SteamID64."
    INVALID_CLIENT_ID = "Invalid client ID."

    @classmethod
    def _reply(cls, target: Any, message: str) -> None:
        """Send a message to a channel or a player, whichever we were handed.

        Duck-typed on purpose. Channels have ``reply()``, players have ``tell()``, and
        callers pass RconDummyPlayer and other Player-likes an ``isinstance`` check would
        send down the wrong branch.
        """
        if target is None:
            return
        reply = getattr(target, "reply", None)
        if reply is not None:
            reply(message)
        else:
            target.tell(message)

    @classmethod
    def resolve_player(cls, value: Any, target: Any = None,
                       message: str | None = None) -> Player | None:
        """Resolve a client id to a connected player.

        :param value: The raw argument, e.g. ``msg[1]``.
        :param target: A channel or player to report failure to. None stays silent.
        :param message: Overrides the default failure message.
        :returns: minqlxtended.Player or None
        """
        if message is None:
            message = cls.INVALID_CLIENT_ID

        try:
            client_id = int(value)
        except (TypeError, ValueError):
            cls._reply(target, message)
            return None

        if not 0 <= client_id < minqlxtended.MAX_CLIENTS:
            cls._reply(target, message)
            return None

        # ValueError as well: MAX_CLIENTS is the engine's ceiling of 64 while player_info
        # bounds on sv_maxclients, so an id between the two raises out of the C module.
        try:
            player = cls.player(client_id)
        except (minqlxtended.NonexistentPlayerError, ValueError):
            player = None

        if player is None:
            cls._reply(target, message)
        return player

    @classmethod
    def resolve_identifier(cls, value: Any, target: Any = None,
                           message: str | None = None) -> Identifier | None:
        """Resolve "<id>" to a player, whether it is a client id or a SteamID64.

        A value below :data:`MAX_CLIENTS` is a client id and must name a connected player.
        Anything larger is a SteamID64 and need not be connected.

        :param value: The raw argument, e.g. ``msg[1]``.
        :param target: A channel or player to report failure to. None stays silent.
        :param message: Overrides the default failure message.
        :returns: minqlxtended.Identifier or None -- ``(steam_id, name, player)``, or
            None if it couldn't be resolved.
        """
        try:
            ident = int(value)
        except (TypeError, ValueError):
            cls._reply(target, message or cls.INVALID_ID)
            return None

        if ident >= minqlxtended.MAX_CLIENTS:
            # A SteamID64 can still belong to someone connected, and callers need the
            # Player to act on them as well as to write to the database.
            player = cls.player(ident)
            return Identifier(ident, player.name if player is not None else str(ident), player)

        if ident < 0:
            cls._reply(target, message or cls.INVALID_ID)
            return None

        # Same two as resolve_player: an empty slot raises NonexistentPlayerError, and an
        # id past sv_maxclients raises ValueError from player_info.
        try:
            player = cls.player(ident)
        except (minqlxtended.NonexistentPlayerError, ValueError):
            player = None

        if player is None:
            cls._reply(target, message or cls.INVALID_CLIENT_ID)
            return None

        return Identifier(player.steam_id, player.name, player)

    # COALESCED OUTPUT
    #
    # Every message is a reliable command, and each client has a 64-slot ring before the
    # engine drops it with "a reliable command was cycled out". reliable.c merges
    # consecutive *broadcast* prints, so a loop that tells N players costs N commands.

    @classmethod
    def reply_lines(cls, recipient: Any, lines: Sequence[str]) -> None:
        """Send several lines as a single message.

        ``ChatChannel.reply`` splits on newlines and re-joins up to the engine's length
        limit, so joining first collapses N reliable commands into one or two. Call
        ``channel.reply(msg, limit=N)`` directly for custom wrapping.

        :param recipient: A player, or any channel with a ``reply``.
        :param lines: The lines to send. Empty entries are kept, so a blank line
            still separates blocks.
        :type lines: list
        """
        lines = [str(line) for line in lines]
        if not lines:
            return

        cls._reply(recipient, "\n".join(lines))

    @classmethod
    def tell_many(cls, players: Iterable["Player"], message: str, limit: int = 100) -> None:
        """Send the same message to several players.

        Broadcasts instead when *players* covers everyone currently connected, which turns
        N reliable commands into one.

        :param players: The players to send to.
        :type players: list
        :param message: The message.
        :type message: str
        """
        players = list(players)
        if not players:
            return

        connected = cls.players()
        if len(players) == len(connected) and set(players) == set(connected):
            cls.msg(message, limit=limit)
            return

        for player in players:
            player.tell(message, limit=limit)

    @classmethod
    def tell(cls, msg: Any, recipient: Any, **kwargs: Any) -> None:
        """Send a tell (private message) to someone.

        :param msg: The message to be sent.
        :type msg: str
        :param recipient: The player that should receive the message.
        :type recipient: str/int/minqlxtended.Player

        .. note::
            Returns None. The send is queued for the next frame, so success isn't knowable
            here: an unresolvable recipient raises a logged ValueError on that later frame.
            Call :meth:`client_id` first to check.
        """
        minqlxtended.TellChannel(recipient).reply(msg, **kwargs)

    # VOTES
    #
    # Read from the level. The engine mirrors these into the CS_VOTE_* configstrings a
    # frame later, so gating on those races the vote being detected. Both keep answering
    # for a server with no level rather than raising.

    @classmethod
    def is_intermission(cls) -> bool:
        """Whether the match is over and the end-of-match screen is up.

        The `vote` command means something else here: one of three map panels rather than
        yes or no. See :meth:`is_vote_active`."""
        try:
            return minqlxtended.level.intermission_time != 0
        except minqlxtended.EngineStateError:
            return False

    @classmethod
    def is_vote_active(cls) -> bool:
        """Whether voteTime is set.

        .. note::
            True for the whole of every intermission as well. BeginIntermission borrows
            voteTime for the map vote, so pair this with :meth:`is_intermission` when you
            mean a yes/no vote. :meth:`callvote` relies on the broad reading to refuse a
            vote while the map vote is up.
        """
        try:
            return minqlxtended.level.vote_time != 0
        except minqlxtended.EngineStateError:
            return False

    @classmethod
    def current_vote_count(cls) -> tuple[int, int] | None:
        """The running vote's tally, or None when no vote is running.

        .. note::
            A vote that *passes* reports one short: the frame that tips it over the line
            never writes the deciding vote down.
        """
        try:
            level = minqlxtended.level
            if not level.vote_time:
                return None

            return level.vote_yes, level.vote_no
        except minqlxtended.EngineStateError:
            return None

    @classmethod
    def callvote(cls, vote: str, display: str, caller: Any = None,
                 time: int = 30) -> bool:
        if not cls.is_vote_active():
            # The caller goes to the engine's own pendingVoteCaller, since vote_started is
            # raised from the frame poll and reads it back from there.
            minqlxtended.callvote(vote, display, time,
                                  caller.id if caller is not None else -1)
            return True

        return False

    @classmethod
    def force_vote(cls, pass_it: bool) -> bool:
        if pass_it is True or pass_it is False:
            return minqlxtended.force_vote(pass_it)

        raise ValueError("pass_it must be either True or False.")

    @classmethod
    def cointoss(cls) -> None:
        cls.msg(f"^3The coin is: ^5{'TAILS' if random.randint(0, 1) else 'HEADS'}^7")

    @classmethod
    def change_map(cls, new_map: str, factory: str | None = None) -> None:
        """Load a map, optionally with a different factory.

        Forwards to :meth:`minqlxtended.Game.change_map`. That's a classmethod, so this
        works with no live game.
        """
        minqlxtended.Game.change_map(new_map, factory)

    # Both raise ValueError for bad input, like the admin commands on Game.

    @classmethod
    def play_sound(cls, sound_path: str, player: Any = None) -> None:
        """Play a sound file, to one player or to everybody.

        :raises: ValueError -- if *sound_path* is empty, names a music file, or carries a
            character a command line can't hold.

        """
        if not sound_path:
            raise ValueError("sound_path must not be empty.")
        if "music/" in sound_path.lower():
            raise ValueError(
                f"{sound_path!r} is a music file; use play_music for those.")

        minqlxtended.send_server_command(
            player.id if player else None,
            f"playSound {_check_command_value(sound_path, 'Sound path')}")

    @classmethod
    def play_music(cls, music_path: str, player: Any = None) -> None:
        """Play a music file, to one player or to everybody.

        :raises: ValueError -- if *music_path* is empty, names a sound file, or carries a
            character a command line can't hold.

        """
        if not music_path:
            raise ValueError("music_path must not be empty.")
        if "sound/" in music_path.lower():
            raise ValueError(
                f"{music_path!r} is a sound file; use play_sound for those.")

        minqlxtended.send_server_command(
            player.id if player else None,
            f"playMusic {_check_command_value(music_path, 'Music path')}")

    @classmethod
    def stop_sound(cls, player: Any = None) -> None:
        minqlxtended.send_server_command(player.id if player else None, "clearSounds")

    @classmethod
    def stop_music(cls, player: Any = None) -> None:
        minqlxtended.send_server_command(player.id if player else None, "stopMusic")

    # ADMIN COMMANDS

    # The twenty-five admin commands live on Game, and thirteen of them are on Player too,
    # e.g. self.game.lock("red") and player.tempban(). The whole-map item functions
    # (destroy_kamikaze_timers, remove_dropped_items, replace_items,
    # force_weapon_respawn_time) are module functions. Call them directly.
