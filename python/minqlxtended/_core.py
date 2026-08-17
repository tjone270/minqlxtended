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
import minqlxtended.database

import typing
import subprocess
import functools
import threading
import traceback
import importlib
import importlib.util
import datetime
import os.path
import logging
import atexit
import queue
import shlex
import sys
import os
import gc

from logging.handlers import QueueHandler, QueueListener, RotatingFileHandler
from types import ModuleType
from typing import Any, Callable, Mapping, Sequence

from ._enums import Priority, Weapon

__all__ = (
    "DEFAULT_PLUGINS", "MIN_SWITCH_INTERVAL",
    # Plugin-authoring declarations and decorators.
    "TimerHandle", "command", "delay", "hook", "next_frame", "setting", "thread", "vote",
    # Infostrings and starting weapons.
    "format_infostring", "parse_infostring", "starting_weapon_bit",
    "toggle_starting_weapon",
    # Logging and diagnostics.
    "get_logger", "handle_exception", "log_exception", "perf_trampoline",
    "queued_handler", "threading_excepthook", "uptime",
    # Server identity and state.
    "MapTitles", "map_titles", "owner", "plugins_version", "set_map_subtitles",
    "set_plugins_version",
    "stats_listener",
    # Spawn points, for the entity-surgery natives' flagship use.
    "SPAWN_POINT_CLASSNAMES", "spawn_points",
    # Plugin loading. Called by the plugin_manager plugin as well as from here.
    "PluginLoadError", "PluginUnloadError", "load_plugin", "load_preset_plugins",
    "reload_plugin", "unload_plugin",
    # Startup. Called from C.
    "initialize", "initialize_cvars", "late_init", "require_cvar",
)

DEFAULT_PLUGINS = ("plugin_manager", "essentials", "motd", "permission", "ban", "silence", "clan", "names", "log", "workshop")


class MapTitles(typing.NamedTuple):
    """What :func:`map_titles` returns: the map's own strings, before we append to them."""

    title: str
    subtitle1: str
    subtitle2: str


# Replaced wholesale by set_map_subtitles on every map load. Initialised empty so reading
# before the first map load answers instead of raising.
_map_titles = MapTitles("", "", "")

# Replaced by set_plugins_version() during plugin loading.
_plugins_version = "NOT_SET"

# Low point for the switch interval applied in late_init.
MIN_SWITCH_INTERVAL = 0.00005  # 50us; below this the preemption overhead is the problem.

# HELPERS

_INFOSTRING_FORBIDDEN = {"\\": "a backslash", ";": "a semicolon", '"': "a quote"}

def parse_infostring(infostring: str) -> dict[str, str]:
    """Parse the Quake Live infostring format into a dict.

    A trailing key with no value is dropped with a warning.

    :param infostring: The infostring, with or without its leading backslash.
    :type infostring: str
    :returns: dict -- the variables, in the order they appeared.
    """

    res: dict[str, str] = {}
    if not infostring.strip():
        return res

    vars = infostring.lstrip("\\").split("\\")
    try:
        for i in range(0, len(vars), 2):
            res[vars[i]] = vars[i + 1]
    except IndexError:
        # Log and return incomplete dict.
        logger = minqlxtended.get_logger()
        logger.warning("Uneven number of keys and values: %s", infostring)

    return res


def format_infostring(variables: Mapping[str, object]) -> str:
    """Format a dict into the Quake Live infostring format.

    Values are coerced with ``str()``.

    :param variables: The variables to convert.
    :type variables: dict
    :returns: str -- the infostring.
    :raises: ValueError -- if a key or value contains a character the format can't carry.
    """

    parts = []
    for k, v in variables.items():
        key, value = str(k), str(v)
        for character, description in _INFOSTRING_FORBIDDEN.items():
            # Named rather than counted, since the caller has to find it in their data.
            if character in key:
                raise ValueError(
                    f"Infostring key {key!r} contains {description}, which the format can't carry.")
            if character in value:
                raise ValueError(
                    f"Infostring value for key {key!r} contains {description}, which the format can't "
                    "carry.")
        parts.append("\\" + key + "\\" + value)

    return "".join(parts)


_COMMAND_FORBIDDEN = {'"': "a quote", ";": "a semicolon", "\n": "a newline",
                      "\r": "a carriage return"}
_QUOTED_FORBIDDEN = {'"': "a quote"}


def _check_command_value(value: str, name: str, forbidden: Mapping[str, str] | None = None) -> str:
    """Return *value*, or raise if it would break the command line it is going into.

    ``opsay hi; quit`` is two commands, and the second stops the server.

    :param value: The text about to be interpolated.
    :param name: What the text is, for the message.
    :param forbidden: Which characters to refuse, defaulting to
        :data:`_COMMAND_FORBIDDEN`. Pass :data:`_QUOTED_FORBIDDEN` when the caller writes
        the quotes around it.
    :returns: str -- *value* unchanged.
    :raises ValueError: naming the character and what carried it.
    """
    for character, description in (forbidden or _COMMAND_FORBIDDEN).items():
        if character in value:
            raise ValueError(f"{name} {value!r} contains {description}, which a command can't carry.")

    return value


def starting_weapon_bit(weapon: Weapon) -> int:
    """The ``g_startingWeapons`` bit for a :class:`minqlxtended.Weapon`.

    :param weapon: The weapon.
    :type weapon: minqlxtended.Weapon
    :returns: int -- The bit to test, set or clear in ``g_startingWeapons``.
    """

    return 1 << (weapon - 1)


def toggle_starting_weapon(value: int, weapon: Weapon) -> tuple[int, bool]:
    """Flip one weapon in a ``g_startingWeapons`` value.

    :param value: The current ``g_startingWeapons`` value.
    :type value: int
    :param weapon: The weapon to flip.
    :type weapon: minqlxtended.Weapon
    :returns: tuple -- ``(new_value, enabled)``, where *enabled* is True if the
        weapon is now switched on.
    """

    bit = starting_weapon_bit(weapon)
    if value & bit:
        return value & ~bit, False
    return value | bit, True

_logger_cache: dict[str, logging.Logger] = {}


def get_logger(plugin: Any = None) -> logging.Logger:
    """A logger for your plugin, writing to both the server console and a file.

    :param plugin: The plugin that is using the logger.
    :type plugin: minqlxtended.Plugin
    :returns: logging.Logger -- The logger in question.
    """
    name = "minqlxtended." + str(plugin) if plugin else "minqlxtended"
    logger = _logger_cache.get(name)
    if logger is None:
        logger = logging.getLogger(name)
        _logger_cache[name] = logger

    return logger


_queue_listeners: list[QueueListener] = []
_log_atexit_registered = False


class _QueuedHandler(QueueHandler):
    """A :class:`~logging.handlers.QueueHandler` that owns the listener thread servicing
    it, and stops that thread when the handler is closed. See :func:`queued_handler`.
    """

    def __init__(self, log_queue, listener):
        super().__init__(log_queue)
        self._listener = listener

    def close(self):
        listener, self._listener = self._listener, None
        if listener is not None:
            try:
                listener.stop()
            except Exception:
                pass
            try:
                _queue_listeners.remove(listener)
            except ValueError:
                pass

        super().close()


def queued_handler(*handlers, respect_handler_level=True):
    """Puts *handlers* behind a queue serviced by a listener thread and returns a single
    handler to attach in their place. Emitting a record then costs a queue append.

    :param handlers: The handlers that should do the actual I/O, on the listener thread.
    :param respect_handler_level: Keep each handler's own level, instead of emitting
        every record the queue receives to all of them.
    :returns: logging.Handler -- The handler to add to your logger.

    """
    log_queue = queue.SimpleQueue()
    listener = QueueListener(log_queue, *handlers, respect_handler_level=respect_handler_level)
    listener.start()
    _queue_listeners.append(listener)

    return _QueuedHandler(log_queue, listener)


def _stop_queue_listeners():
    """Drains and stops every listener started by :func:`queued_handler`. Idempotent."""
    for listener in _queue_listeners[:]:
        try:
            listener.stop()
        except Exception:
            pass

    _queue_listeners.clear()


def _resolve_log_level(raw, default=logging.DEBUG):
    """Turns a qlx_logLevel value (a level name or a number) into a level. Returns
    (level, problem) so the caller can report a bad value once the logger is up.
    """
    if raw is None:
        return default, None

    raw = str(raw).strip()
    if not raw:
        return default, None

    try:
        return int(raw), None
    except ValueError:
        pass

    level = logging.getLevelName(raw.upper())
    if isinstance(level, int):
        return level, None

    return default, raw


def _configure_logger():
    global _log_atexit_registered

    level, bad_level = _resolve_log_level(minqlxtended.get_cvar("qlx_logLevel"))

    logging.logThreads = False
    logging.logProcesses = False
    logging.logMultiprocessing = False

    logger = logging.getLogger("minqlxtended")
    logger.setLevel(level)

    # Idempotent: drop any handlers left by a previous (possibly failed) init so we
    # don't stack duplicate file/console handlers if late_init runs more than once.
    for handler in logger.handlers[:]:
        logger.removeHandler(handler)
        try:
            handler.close()
        except Exception:
            pass

    # File
    file_path = os.path.join(require_cvar("fs_homepath"), "minqlxtended.log")
    maxlogs = minqlxtended.Plugin.get_cvar("qlx_logs", int)
    maxlogsize = minqlxtended.Plugin.get_cvar("qlx_logsSize", int)
    file_fmt = logging.Formatter("(%(asctime)s) [%(levelname)s @ %(name)s.%(funcName)s] %(message)s", "%H:%M:%S")
    file_handler = RotatingFileHandler(file_path, encoding="utf-8", maxBytes=maxlogsize, backupCount=maxlogs)
    file_handler.setLevel(level)
    file_handler.setFormatter(file_fmt)

    # Console
    console_fmt = logging.Formatter("[%(name)s.%(funcName)s] %(levelname)s: %(message)s", "%H:%M:%S")
    console_handler = logging.StreamHandler()
    console_handler.setLevel(logging.INFO)
    console_handler.setFormatter(console_fmt)

    # Both handlers sit behind a queue serviced by a listener thread, so emitting a record
    # never does I/O on the game thread. respect_handler_level is required or the INFO
    # console handler emits every DEBUG record.
    logger.addHandler(queued_handler(file_handler, console_handler))

    if not _log_atexit_registered:
        atexit.register(_stop_queue_listeners)
        _log_atexit_registered = True

    if bad_level:
        logger.error("qlx_logLevel is set to '%s', which is not a level name or number. Using DEBUG.", bad_level)

    logger.debug("============================= minqlxtended run @ %s =============================",
                 datetime.datetime.now())


def log_exception(plugin: Any = None) -> None:
    """
    Logs an exception using :func:`get_logger`. Call this in an except block.

    :param plugin: The plugin that is using the logger.
    :type plugin: minqlxtended.Plugin
    """
    # TODO: Remove plugin arg and make it automatic.
    logger = get_logger(plugin)
    e = traceback.format_exc().rstrip("\n")
    for line in e.split("\n"):
        logger.error(line)


def handle_exception(exc_type, exc_value, exc_traceback):
    """A handler for unhandled exceptions."""
    # TODO: If exception was raised within a plugin, detect it and pass to log_exception()
    logger = get_logger(None)
    e = "".join(traceback.format_exception(exc_type, exc_value, exc_traceback)).rstrip("\n")
    for line in e.split("\n"):
        logger.error(line)


def threading_excepthook(args):
    handle_exception(args.exc_type, args.exc_value, args.exc_traceback)


_init_time = datetime.datetime.now()


def uptime() -> datetime.timedelta:
    """Returns a :class:`datetime.timedelta` instance of the time since initialized."""
    return datetime.datetime.now() - _init_time


def perf_trampoline(action: str = "") -> str:
    """Turn the interpreter's perf trampoline on or off. Backs the ``qlx_pyperf`` command.

    :param action: ``"on"``, ``"off"``, or empty to report the current state.
    :type action: str
    :returns: str -- one line, already phrased for the console.

    """
    # Absent on an interpreter built without perf-trampoline support, and/or on non-Linux.
    if not hasattr(sys, "activate_stack_trampoline"):
        return "This interpreter was built without perf trampoline support."

    action = action.strip().lower()
    if action not in ("", "on", "off"):
        return "Usage: qlx_pyperf [on|off]"

    try:
        if action == "on":
            sys.activate_stack_trampoline("perf")
            return ("Perf trampoline on. Python frames are now visible to "
                    "`perf record -g -p <pid>`; turn it off when you're done.")
        if action == "off":
            sys.deactivate_stack_trampoline()
            return "Perf trampoline off."
    except Exception as e:
        return f"Perf trampoline unavailable: {type(e).__name__}: {e}"

    return f"Perf trampoline is {'on' if sys.is_stack_trampoline_active() else 'off'}."


def owner() -> int | None:
    """Returns the SteamID64 of the owner. This is set in the config."""
    try:
        sid = int(minqlxtended.get_cvar("qlx_owner") or "")
    except (ValueError, TypeError):
        logger = minqlxtended.get_logger()
        logger.error("Failed to parse the Owner Steam ID. Make sure it's in SteamID64 format.")
        return None
    # -1 is the default sentinel meaning "no owner configured"; not an error.
    if sid == -1:
        return None
    return sid


_stats = None


def stats_listener() -> Any:
    """Returns the :class:`minqlxtended.StatsListener` instance used to listen for stats."""
    return _stats


def require_cvar(name: str) -> str:
    """A cvar that must have a value by the time it is read, or a clear error saying it
    does not."""
    value = minqlxtended.get_cvar(name)
    if not value:
        raise RuntimeError(
            f"The cvar '{name}' is unset or empty. minqlxtended cannot continue without it; it "
            "is normally set by initialize_cvars().")

    return value


def _cvar_bool(name: str, default: bool) -> bool:
    """A cvar read as a boolean flag, falling back to *default*."""
    raw = minqlxtended.get_cvar(name)
    if raw is None or not raw.strip():
        return default

    try:
        return bool(int(raw))
    except ValueError:
        get_logger().error("%s is set to '%s', which is not a number. Using %d.",
                           name, raw, default)
        return default


def set_cvar_once(name: str, value: Any, flags: int = 0) -> bool:
    # str() because the engine setters take strings; callers pass ints and floats.
    if minqlxtended.get_cvar(name) is None:
        minqlxtended.set_cvar(name, str(value), flags)
        return True

    return False


def set_cvar_limit_once(name: str, value: Any, minimum: Any, maximum: Any,
                        flags: int = 0) -> bool:
    if minqlxtended.get_cvar(name) is None:
        minqlxtended.set_cvar_limit(name, str(value), str(minimum), str(maximum), flags)
        return True

    return False


def set_plugins_version(path: str) -> None:
    args_version = shlex.split("git describe --long --tags --dirty --always")
    args_branch = shlex.split("git rev-parse --abbrev-ref HEAD")

    # We keep environment variables, but remove LD_PRELOAD to avoid a warning the OS might throw.
    env = dict(os.environ)
    env.pop("LD_PRELOAD", None)
    try:
        # Get the version using git describe.
        p = subprocess.Popen(args_version, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=path, env=env)
        p.wait(timeout=1)
        if p.returncode != 0:
            _set_plugins_version("NOT_SET")
            return

        assert p.stdout is not None  # opened with stdout=PIPE
        version = p.stdout.read().decode().strip()

        # Get the branch using git rev-parse.
        p = subprocess.Popen(args_branch, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=path, env=env)
        p.wait(timeout=1)
        if p.returncode != 0:
            _set_plugins_version(version)
            return

        assert p.stdout is not None  # opened with stdout=PIPE
        branch = p.stdout.read().decode().strip()
    except (FileNotFoundError, subprocess.TimeoutExpired):
        _set_plugins_version("NOT_SET")
        return

    _set_plugins_version(f"{version}-{branch}")


def plugins_version() -> str:
    """The version string for the loaded plugin set.

    "NOT_SET" until set_plugins_version() has run, which is on the map-load path.
    """
    return _plugins_version


def _set_plugins_version(value: str) -> None:
    global _plugins_version
    _plugins_version = value


def map_titles() -> MapTitles:
    """The map's title and its two subtitles, as they were before we appended to them.

    Empty strings until the first :func:`set_map_subtitles`, which runs on map load.

    :returns: MapTitles -- a named tuple of (title, subtitle1, subtitle2).
    """
    return _map_titles


def set_map_subtitles() -> None:
    global _map_titles
    _map_titles = MapTitles(
        minqlxtended.configstring(minqlxtended.CS_MESSAGE),
        minqlxtended.configstring(minqlxtended.CS_AUTHOR),
        minqlxtended.configstring(minqlxtended.CS_AUTHOR2),
    )

    cs = minqlxtended.configstring(minqlxtended.CS_AUTHOR)
    if cs:
        cs += " - "
    minqlxtended.set_configstring(
        minqlxtended.CS_AUTHOR,
        cs + f"Running minqlxtended ^6{minqlxtended.__version__}^7 with plugins ^6{plugins_version()}^7.",
    )
    cs = minqlxtended.configstring(minqlxtended.CS_AUTHOR2)
    if cs:
        cs += " - "
    minqlxtended.set_configstring(
        minqlxtended.CS_AUTHOR2, cs + "Check ^6http://github.com/tjone270/minqlxtended^7 for more details."
    )


SPAWN_POINT_CLASSNAMES = ("info_player_deathmatch", "team_CTF_redspawn", "team_CTF_bluespawn",
                          "team_CTF_redplayer", "team_CTF_blueplayer")


def spawn_points(classnames: Sequence[str] = SPAWN_POINT_CLASSNAMES) -> list[minqlxtended.Entity]:
    """Every live spawn-point entity on the current map.

    :param classnames: The classnames to collect. Defaults to all five spawn kinds:
        deathmatch spawns plus the CTF respawn and initial-spawn variants per team.
    :type classnames: Sequence[str]
    :returns: list[Entity] -- game thread only, like every entity view.
    """
    out: list[minqlxtended.Entity] = []
    for classname in classnames:
        out.extend(minqlxtended.entities(classname=classname))
    return out


# DECORATORS


def next_frame(func: Callable[..., Any]) -> Callable[..., None]:
    """Delay a function call until the start of the next frame.

    .. note::
        Everything queued during a frame runs together at the top of the next one, ahead
        of any :func:`delay` callbacks due in that frame. A task that queues another one
        leaves it for the frame after.

    :param func: The function to be called.
    :type func: callable
    """

    @functools.wraps(func)
    def f(*args, **kwargs):
        minqlxtended._handlers._queue_next_frame(func, args, kwargs)

    return f


def delay(time: float) -> Callable[[Callable[..., Any]], Callable[..., None]]:
    """Delay a function call a certain amount of time.

    The call lands shortly after the timer expires, later if a plugin is blocking.
    Nothing comes back, so the call cannot be cancelled; :meth:`minqlxtended.Plugin.delay`
    schedules the same way and returns a cancellable :class:`TimerHandle`.

    :param time: The number of seconds before the function should be called.
    :type time: float

    """

    def wrap(func):
        @functools.wraps(func)
        def f(*args, **kwargs):
            minqlxtended.frame_tasks.enter(
                time, 0, minqlxtended._handlers._run_frame_task, (func, args, kwargs))

        return f

    return wrap


class TimerHandle:
    """A scheduled call that can be cancelled: what :meth:`minqlxtended.Plugin.delay`
    and :meth:`minqlxtended.Plugin.repeat` return.

    :meth:`cancel` is idempotent. :attr:`pending` says whether a call is still coming.
    """

    def __init__(self, interval, func, args, kwargs, *, repeating, owner=None):
        self._interval = interval
        self._func = func
        self._args = args
        self._kwargs = kwargs
        self._repeating = repeating
        # The plugin that made this, when one did.
        self._owner = owner
        self._event = None
        self._cancelled = False

    def __repr__(self):
        repeating = "every " if self._repeating else ""
        func = getattr(self._func, "__name__", self._func)
        return f"TimerHandle({repeating}{self._interval:g}s -> {func})"

    @property
    def pending(self):
        """Whether a call is still coming: scheduled now, or a repeat between fires."""
        return not self._cancelled and (self._event is not None or self._repeating)

    def _schedule(self):
        if self._cancelled or (self._owner is not None and self._owner._unloaded):
            self._done()
            return
        self._event = minqlxtended.frame_tasks.enter(self._interval, 0, self._fire, ())

    def _fire(self):
        self._event = None
        if self._owner is not None and self._owner._unloaded:
            self._done()
            return
        if self._repeating:
            minqlxtended._handlers._queue_next_frame(self._schedule, (), {})
        else:
            self._done()
        self._func(*self._args, **self._kwargs)

    def _done(self):
        self._event = None
        self._cancelled = True
        if self._owner is not None:
            self._owner._tasks.discard(self)

    def cancel(self):
        """Stop the call from coming. Idempotent, and a no-op once a one-shot has run."""
        if self._event is not None:
            try:
                minqlxtended.frame_tasks.cancel(self._event)
            except ValueError:
                # Already popped by the scheduler.
                pass
        self._done()


_thread_count = 0
_thread_name = "minqlxtendedthread"


def thread(func=None, force=False):
    """Starts a thread with the function passed as its target. Calling one decorated
    function from inside another runs it inline, unless *force* is set.

    Usable both ways around::

        @minqlxtended.thread
        def slow(): ...

        @minqlxtended.thread(force=True)
        def always_its_own_thread(): ...

    :param func: The function to be ran in a thread.
    :type func: callable
    :param force: Force it to create a new thread even if already in one created by this decorator.
    :type force: bool
    :returns: The ``threading.Thread`` when one is spawned, or the wrapped function's own
        return value when it runs inline because we are already in such a thread.

    """
    if func is None:
        return functools.partial(thread, force=force)

    @functools.wraps(func)
    def f(*args, **kwargs):
        if not force and threading.current_thread().name.endswith(_thread_name):
            return func(*args, **kwargs)
        else:
            global _thread_count
            name = func.__name__ + f"-{str(_thread_count)}-{_thread_name}"
            t = threading.Thread(target=func, name=name, args=args, kwargs=kwargs, daemon=True)
            t.start()
            _thread_count += 1

            return t

    return f


# DECLARATIVE REGISTRATION
#
# These let a handler carry its own registration instead of naming it in `__init__`.
# `add_hook`, `add_command` and `add_vote` remain for registration that depends on a cvar
# or a gametype.

# Where a decorated function keeps its pending registrations. Lists, because stacking the
# decorator is how one handler answers several events or a command gains a second spelling.
_HOOK_ATTR = "_minqlxtended_hooks"
_COMMAND_ATTR = "_minqlxtended_commands"
_VOTE_ATTR = "_minqlxtended_votes"


def _mark(func, attr, entry):
    """Append *entry* to *func*'s registration list under *attr*.

    Always writes a fresh list. functools.wraps copies `__dict__` by reference, so
    appending to an inherited list would register the entry twice.
    """
    existing = func.__dict__.get(attr, ())
    func.__dict__[attr] = list(existing) + [entry]
    return func


def hook(event, priority=Priority.NORMAL):
    """Hook an event from the handler itself, instead of calling
    :meth:`minqlxtended.Plugin.add_hook` in the plugin's ``__init__``.

    ::

        class my_plugin(minqlxtended.Plugin):

            @minqlxtended.hook("chat")
            def handle_chat(self, player, msg, channel, recipient):
                ...

    Registered when the plugin is constructed and removed on unload, interchangeably with
    ``add_hook``. Stack the decorator to answer more than one event with the same handler.

    :param event: The event to hook, as in :data:`minqlxtended.EVENT_DISPATCHERS`.
    :type event: str
    :param priority: The priority of the hook, which decides the order handlers run in.
    :type priority: minqlxtended.Priority

    """

    def wrap(func):
        return _mark(func, _HOOK_ATTR, (event, priority))

    return wrap


def command(name, permission=0, channels=None, exclude_channels=(),
            priority=Priority.NORMAL, client_cmd_pass=False, client_cmd_perm=5,
            prefix=True, usage=""):
    """Register a command from the handler itself, instead of calling
    :meth:`minqlxtended.Plugin.add_command` in the plugin's ``__init__``.

    ::

        class my_plugin(minqlxtended.Plugin):

            @minqlxtended.command(("sv_fps", "svfps"), permission=5, usage="<integer>")
            def cmd_svfps(self, player, msg, channel):
                ...

    Takes every argument :meth:`minqlxtended.Plugin.add_command` does apart from the
    handler. Registered when the plugin is constructed and removed on unload.

    :param name: The name of the command, or a list of names for a command with aliases.
    :type name: str, list, tuple

    """

    def wrap(func):
        return _mark(func, _COMMAND_ATTR, {
            "name": name,
            "permission": permission,
            "channels": channels,
            "exclude_channels": exclude_channels,
            "priority": priority,
            "client_cmd_pass": client_cmd_pass,
            "client_cmd_perm": client_cmd_perm,
            "prefix": prefix,
            "usage": usage,
        })

    return wrap


def vote(name, usage="", description=""):
    """Register a custom vote from its callback, instead of calling
    :meth:`minqlxtended.Plugin.add_vote` in the plugin's ``__init__``.

    ::

        class my_plugin(minqlxtended.Plugin):

            @minqlxtended.vote("lgammo", usage="<count>", description="Set starting LG ammo.")
            def vote_lgammo(self, caller, args):
                ...

    Takes every argument :meth:`minqlxtended.Plugin.add_vote` does apart from the
    callback. Registered when the plugin is constructed and removed on unload.

    :param name: The vote's name, or a list of names for a vote with aliases.
    :type name: str, list, tuple
    :param usage: The argument hint for help listings, e.g. ``"<count>"``.
    :type usage: str
    :param description: One line for help listings.
    :type description: str

    """

    def wrap(func):
        return _mark(func, _VOTE_ATTR, {
            "name": name,
            "usage": usage,
            "description": description,
        })

    return wrap


class setting:
    """A cvar a plugin declares once, at class level, and reads as an attribute.

    ::

        class my_plugin(minqlxtended.Plugin):
            _qlx_votepassThreshold = minqlxtended.setting("qlx_votepassThreshold", 0.33,
                                                          minimum=0, maximum=1)

            def handle_vote_ended(self, votes, vote, args, passed):
                if yes / total > self._qlx_votepassThreshold:
                    ...

    Reading the attribute is a cached parse of the cvar, refreshed on every ``new_game``;
    assigning writes through :meth:`minqlxtended.Plugin.set_cvar` and re-reads. The value
    is parsed to the default's type, or to *type* when given. *minimum* and *maximum* go
    through ``set_cvar_limit_once``, which bounds only a cvar this creates, so the value
    read back is clamped here as well.

    :param name: The cvar's name.
    :type name: str
    :param default: The value the cvar is created with, and what its value parses to.
        A bool is stored as ``1``/``0``, a list, set or tuple as a comma-separated list.
    :param type: Parse type override; the types :meth:`minqlxtended.Plugin.get_cvar` takes.
    :param minimum: Lower bound. Needs *maximum*.
    :param maximum: Upper bound. Needs *minimum*.
    :param flags: The flags the cvar is created with, as in ``set_cvar_once``.
    :type flags: int

    """

    def __init__(self, name, default, *, type=None, minimum=None, maximum=None, flags=0):
        self.name = name
        self.default = default
        self._type = type if type is not None else default.__class__
        if self._type not in (str, int, float, bool, list, set, tuple):
            raise ValueError(f"Invalid setting type: {self._type}")
        if (minimum is None) != (maximum is None):
            raise ValueError("A bounded setting needs both minimum and maximum.")
        self.minimum = minimum
        self.maximum = maximum
        self.flags = flags
        self.attr = None
        self._slot = None

    def __set_name__(self, owner, name):
        self.attr = name
        self._slot = "_setting_" + name

    def __repr__(self):
        return f"setting({self.name!r}, {self.default!r})"

    @staticmethod
    def _format(value):
        if isinstance(value, bool):
            return "1" if value else "0"
        if isinstance(value, (list, set, tuple)):
            return ", ".join(str(item) for item in value)
        return str(value)

    def initialise(self, instance):
        if self.minimum is not None:
            instance.set_cvar_limit_once(
                self.name, self._format(self.default), self.minimum, self.maximum, self.flags)
        else:
            instance.set_cvar_once(self.name, self._format(self.default), self.flags)
        self.refresh(instance)

    def refresh(self, instance):
        try:
            value = instance.get_cvar(self.name, self._type, self.default)
        except ValueError:
            get_logger().warning(
                "%s is not a valid %s; using the default %r.", self.name, self._type.__name__, self.default)
            value = self.default

        if self.minimum is not None and isinstance(value, (int, float)) and not isinstance(value, bool):
            if value < self.minimum:
                get_logger().warning("%s is below its minimum of %r; using %r.", self.name, self.minimum, self.minimum)
                value = self.minimum
            elif value > self.maximum:
                get_logger().warning("%s is above its maximum of %r; using %r.", self.name, self.maximum, self.maximum)
                value = self.maximum

        instance.__dict__[self._slot] = value

    def __get__(self, instance, owner=None):
        if instance is None:
            return self
        try:
            return instance.__dict__[self._slot]
        except KeyError:
            self.refresh(instance)
            return instance.__dict__[self._slot]

    def __set__(self, instance, value):
        instance.set_cvar(self.name, self._format(value))
        self.refresh(instance)


def _collect_settings(cls):
    declared = {}
    for klass in reversed(cls.__mro__):
        for attribute, value in vars(klass).items():
            if isinstance(value, setting):
                declared[attribute] = value
    return tuple(declared.values())


def _refresh_settings():
    for plugin_instance in minqlxtended.Plugin._loaded_plugins.values():
        for declared in type(plugin_instance)._declared_settings:
            try:
                declared.refresh(plugin_instance)
            except Exception:
                log_exception(plugin_instance)


def _on_cvar_changed(name, old_value, new_value):
    lowered = name.lower()
    for plugin_instance in minqlxtended.Plugin._loaded_plugins.values():
        for declared in type(plugin_instance)._declared_settings:
            if declared.name.lower() == lowered:
                declared.refresh(plugin_instance)


def _collect_declarations(cls, attr):
    """Gather every decorated method on *cls* into ``[(attribute_name, [entry, ...])]``."""
    declared = {}
    for klass in reversed(cls.__mro__):
        for attribute, value in vars(klass).items():
            entries = getattr(value, attr, None) if callable(value) else None
            if entries:
                declared[attribute] = entries

    return tuple(declared.items())


# CONFIG AND PLUGIN LOADING

class PluginLoadError(Exception):
    pass


class PluginUnloadError(Exception):
    pass


def _refreeze_gc():
    if not gc.get_freeze_count():
        return
    gc.unfreeze()
    gc.collect()
    gc.freeze()


def load_preset_plugins() -> None:
    plugins_temp = []
    for p in minqlxtended.Plugin.get_cvar("qlx_plugins", list):
        if p == "DEFAULT":
            plugins_temp += list(DEFAULT_PLUGINS)
        else:
            plugins_temp.append(p)

    plugins = []
    for p in plugins_temp:
        if p not in plugins:
            plugins.append(p)

    plugins_path = os.path.abspath(require_cvar("qlx_pluginsPath"))

    if os.path.isdir(plugins_path):
        loaded = minqlxtended.Plugin._loaded_plugins
        plugins = [p for p in plugins if p not in loaded]
        for p in plugins:
            # Isolate per-plugin failures so one bad preset plugin doesn't abort the
            # rest of the load (and, via late_init, cause it to be retried on every map).
            try:
                load_plugin(p)
            except Exception:
                log_exception()
    else:
        raise (PluginLoadError(f"Cannot find the plugins directory '{os.path.abspath(plugins_path)}'."))


def _import_plugin_module(name: str, source: str) -> ModuleType:
    """The plugin's module, executed against what is on disk."""
    try:
        os.remove(importlib.util.cache_from_source(source))
    except OSError:
        pass
    importlib.invalidate_caches()

    if name in sys.modules:
        return importlib.reload(sys.modules[name])
    return importlib.import_module(name)


def load_plugin(plugin: str) -> None:
    logger = get_logger(None)
    logger.info("Loading plugin '%s'...", plugin)
    plugins = minqlxtended.Plugin._loaded_plugins
    plugins_path = os.path.abspath(require_cvar("qlx_pluginsPath"))
    plugins_dir = os.path.basename(plugins_path)
    source = os.path.join(plugins_path, plugin + ".py")

    if not os.path.isfile(source):
        raise PluginLoadError("No such plugin exists.")
    elif plugin in plugins:
        return reload_plugin(plugin)
    try:
        module = _import_plugin_module(f"{plugins_dir}.{plugin}", source)

        if not hasattr(module, plugin):
            raise (PluginLoadError("The plugin needs to have a class with the exact name as the file, minus the .py."))

        plugin_class = getattr(module, plugin)
        if issubclass(plugin_class, minqlxtended.Plugin):
            plugins[plugin] = plugin_class()
        else:
            raise (PluginLoadError("Attempted to load a plugin that is not a subclass of 'minqlxtended.Plugin'."))
    except:
        log_exception(plugin)
        hooks = minqlxtended.EVENT_DISPATCHERS.remove_all_hooks(plugin)
        commands = minqlxtended.COMMANDS.remove_all_for_plugin(plugin)
        minqlxtended.CUSTOM_VOTES.remove_all_for_plugin(plugin)
        if hooks or commands:
            logger.warning("Unregistered %d hook(s) and %d command(s) left behind by "
                           "'%s', which failed to load.", hooks, commands, plugin)
        raise
    _refreeze_gc()


def unload_plugin(plugin: str) -> None:
    logger = get_logger(None)
    logger.info("Unloading plugin '%s'...", plugin)
    plugins = minqlxtended.Plugin._loaded_plugins
    if plugin in plugins:
        instance = plugins[plugin]
        try:
            minqlxtended.EVENT_DISPATCHERS["unload"].dispatch(plugin)

            # The instance's own teardown, while its state is intact.
            try:
                instance.unload()
            except:
                log_exception(plugin)

            instance._unloaded = True

            del plugins[plugin]

            for task in list(instance._tasks):
                task.cancel()

            try:
                # Unhook its hooks.
                for hook in instance.hooks:
                    instance.remove_hook(*hook)

                # Unregister commands.
                for cmd in instance.commands:
                    instance.remove_command(cmd.names, cmd.handler)
            finally:
                minqlxtended.CUSTOM_VOTES.remove_all_for_plugin(plugin)
                hooks = minqlxtended.EVENT_DISPATCHERS.remove_all_hooks(plugin)
                commands = minqlxtended.COMMANDS.remove_all_for_plugin(plugin)

            if hooks or commands:
                logger.warning("Unregistered %d hook(s) and %d command(s) that '%s' had "
                               "registered outside its own bookkeeping.", hooks, commands, plugin)
        except:
            log_exception(plugin)
            raise
        _refreeze_gc()
    else:
        raise (PluginUnloadError("Attempted to unload a plugin that is not loaded."))


def reload_plugin(plugin: str) -> None:
    try:
        unload_plugin(plugin)
    except PluginUnloadError:
        pass

    try:
        # load_plugin re-executes the module itself, so there is nothing to do here
        # beyond dropping the instance above.
        load_plugin(plugin)
    except:
        log_exception(plugin)
        raise


def initialize_cvars() -> None:
    # Core
    set_cvar_once("qlx_owner", "-1")
    set_cvar_once("qlx_plugins", ", ".join(DEFAULT_PLUGINS))
    set_cvar_once("qlx_pluginsPath", "minqlxtended-plugins")
    set_cvar_once("qlx_database", "Redis")
    set_cvar_once("qlx_commandPrefix", "!")
    set_cvar_once("qlx_logs", "2")
    set_cvar_once("qlx_logsSize", str(3 * 10**6))  # 3 MB
    set_cvar_once("qlx_logLevel", "DEBUG")
    set_cvar_once("qlx_pythonSwitchInterval", "0.0005")
    set_cvar_once("qlx_mapinfoScan", "1")
    set_cvar_once("qlx_workshopPath", "")
    # Redis
    set_cvar_once("qlx_permissionCacheTime", "30")
    set_cvar_once("qlx_redisAddress", "127.0.0.1")
    set_cvar_once("qlx_redisDatabase", "0")
    set_cvar_once("qlx_redisProtocol", "3")
    set_cvar_once("qlx_redisUnixSocket", "0")
    set_cvar_once("qlx_redisPassword", "")
    set_cvar_once("qlx_redisTimeout", "0.5")


# MAIN


def initialize() -> None:
    minqlxtended.register_handlers()


def late_init() -> None:
    """Initialization that runs after QLDS has finished its own."""
    minqlxtended.initialize_cvars()

    # Keeps minqlxtended.setting caches in step with cvar writes as they happen.
    minqlxtended.EVENT_DISPATCHERS["cvar_changed"].add_hook("minqlxtended", _on_cvar_changed)

    # Get the plugins path, which is where plugins_version() reads the version from.
    plugins_path = os.path.abspath(require_cvar("qlx_pluginsPath"))
    set_plugins_version(plugins_path)

    # Initialize the logger now that we have fs_basepath.
    _configure_logger()
    logger = get_logger()
    # Set our own exception handler so that we can log them if unhandled.
    sys.excepthook = handle_exception

    threading.excepthook = threading_excepthook

    # Caps how long the game thread can sit waiting for the GIL.
    # CPython's default of 5ms is a fifth of a 25ms frame.
    try:
        switch_interval = minqlxtended.Plugin.get_cvar("qlx_pythonSwitchInterval", float)
    except (TypeError, ValueError):
        logger.error("qlx_pythonSwitchInterval is not a number. Leaving the interpreter default in place.")
    else:
        if switch_interval > 0:
            if switch_interval < MIN_SWITCH_INTERVAL:
                logger.warning(
                    "qlx_pythonSwitchInterval of %s is too small to be useful; using %s.", switch_interval, MIN_SWITCH_INTERVAL
                )
                switch_interval = MIN_SWITCH_INTERVAL
            sys.setswitchinterval(switch_interval)

    db_name = minqlxtended.get_cvar("qlx_database")
    drivers = {
        attr.lower(): candidate
        for attr, candidate in vars(minqlxtended.database).items()
        if isinstance(candidate, type)
        and issubclass(candidate, minqlxtended.database.AbstractDatabase)
        and candidate is not minqlxtended.database.AbstractDatabase
    }
    driver = drivers.get(db_name.strip().lower()) if db_name else None
    if driver is not None:
        minqlxtended.Plugin.database = driver
    else:
        logger.error(
            "qlx_database is set to '%s', which is not a known database driver. Plugins that "
            "use a database will fail. Available drivers: %s.",
            db_name,
            ", ".join(sorted(drivers)) or "none",
        )

    # Add the plugins path to PATH so that we can load plugins later.
    sys.path.append(os.path.dirname(plugins_path))

    logger.info("Loading preset plugins...")
    try:
        load_preset_plugins()
    except Exception:
        log_exception()

    if _cvar_bool("qlx_mapinfoScan", True):
        try:
            minqlxtended.refresh_map_cache()
        except Exception:
            log_exception()

    if _cvar_bool("zmq_stats_enable", False):
        global _stats
        try:
            _stats = minqlxtended.StatsListener()
            logger.info("Stats listener started on %s.", _stats.address)
            # Receives on its own thread; dispatches on the main thread.
            _stats.start()
        except Exception:
            log_exception()

    gc.collect()
    gc.freeze()

    logger.info("We're good to go!")
