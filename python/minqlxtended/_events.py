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
import difflib
import inspect
import logging
from typing import Any, Callable, override

from ._enums import Priority, Return

__all__ = (
    "EVENT_DISPATCHERS",
    "EventDispatcher",
    "EventDispatcherManager",
)

# EVENTS

def _handler_parameters(callable_):
    """The parameter names *callable_* takes, or None if they can't be determined.

    None means "don't check": builtins and some C callables have no introspectable
    signature, and a handler with ``*args`` or ``**kwargs`` accepts whatever it is passed.
    """
    try:
        parameters = list(inspect.signature(callable_).parameters.values())
    except (TypeError, ValueError):
        return None

    for parameter in parameters:
        if parameter.kind in (parameter.VAR_POSITIONAL, parameter.VAR_KEYWORD):
            return None

    return [parameter.name for parameter in parameters]


def _check_handler_signature(handler, expected, description, hint=""):
    """Refuse a handler that cannot be called with what it will be passed.

    Without this the mismatch surfaces as a TypeError the first time the event fires.

    :param handler: The handler being registered.
    :param expected: The parameter names it will be passed, in order. None skips the check.
    :type expected: list
    :param description: What is doing the passing, for the message.
    :type description: str
    :param hint: Appended to the message. Where to look, when there is somewhere to look.
    :type hint: str
    :raises: TypeError
    """
    if expected is None:
        return

    try:
        signature = inspect.signature(handler)
    except (TypeError, ValueError):
        return

    try:
        signature.bind(*([None] * len(expected)))
    except TypeError:
        plural = "" if len(expected) == 1 else "s"
        names = ", ".join(expected) or "none"
        where = getattr(handler, "__qualname__", repr(handler))
        raise TypeError(
            f"{description} passes {len(expected)} argument{plural} ({names}), which handler "
            f"'{where}{signature}' cannot accept.{' ' + hint if hint else ''}") from None


class EventDispatcher:
    """The base event dispatcher. Each event should inherit this and provides a way
    to hook into events by registering an event handler.

    """
    name: str = ""
    no_debug = ("frame", "set_configstring", "stats", "server_command", "death", "kill", "command",
                "console_print", "damage", "weapon_fired", "cvar_changed")
    need_zmq_stats_enabled = False

    gated_handler: str | None = None
    gated_dispatch_fn: str | None = None

    def __init__(self):
        self.name = type(self).name
        self.plugins = {}
        # The position each plugin name first hooked this event, kept for the dispatcher's
        # lifetime, so a reloaded plugin keeps its slot within its priority level.
        self._plugin_order = {}
        self._handler_chain = ()
        # Gated events start disarmed. register_handlers() skips them, so the engine slot
        # is NULL until the first hook arrives.
        self._gate_armed = False
        self._no_debug = self.name in self.no_debug
        # What this event passes its handlers, read off its own dispatch(). None for a
        # dispatcher that doesn't override dispatch.
        self._handler_params = _handler_parameters(type(self).dispatch)
        if self._handler_params:
            self._handler_params = self._handler_params[1:]  # drop `self`
        # Handlers already warned about an unrecognised return value, so a chat filter
        # returning one does not warn per message. Cleared when the chain is rebuilt, so a
        # reloaded plugin is told again.
        self._warned_returns = set()
        # Per-dispatch state, saved and restored by dispatch() so a re-entering handler
        # cannot clobber the dispatch it interrupted.
        self.args = ()
        self.kwargs = {}
        self.return_value = True

    def dispatch(self, *args, **kwargs):
        """Calls all the handlers that have been registered when hooking this event.

        Subclasses override this with explicit arguments and call ``super().dispatch()``.

        Handlers have several options for return values that can affect the flow:
            - minqlxtended.Return.NONE or None -- Continue execution normally.
            - minqlxtended.Return.STOP -- Stop any further handlers from being called.
            - minqlxtended.Return.STOP_EVENT -- Let handlers process it, but stop the event
                at the engine-level.
            - minqlxtended.Return.STOP_ALL -- Stop handlers **and** the event.
            - Any other value -- Passed on to :func:`self.handle_return`, which warns about
                the unknown value. Override it to give an event its own return values.

        :param args: Any arguments.
        :param kwargs: Any keyword arguments.

        """
        prev_args = self.args
        prev_kwargs = self.kwargs
        prev_return_value = self.return_value

        self.args = args
        self.kwargs = kwargs
        if not self._no_debug:
            logger = minqlxtended.get_logger()
            if logger.isEnabledFor(logging.DEBUG):
                dbgstr = f"{self.name}{args}"
                if len(dbgstr) > 100:
                    dbgstr = dbgstr[0:99] + ")"
                logger.debug(dbgstr)

        self.return_value = True
        try:
            for plugin, handler in self._handler_chain:
                try:
                    res = handler(*self.args, **self.kwargs)
                    if res == Return.NONE or res is None:
                        continue
                    elif res == Return.STOP:
                        return self.return_value
                    elif res == Return.STOP_EVENT:
                        self.return_value = False
                    elif res == Return.STOP_ALL:
                        return False
                    else: # Got an unknown return value.
                        return_handler = self.handle_return(handler, res)
                        if return_handler is not None:
                            return return_handler
                except:
                    minqlxtended.log_exception(plugin)
                    continue

            return self.return_value
        finally:
            self.args = prev_args
            self.kwargs = prev_kwargs
            self.return_value = prev_return_value

    def _rebuild_chain(self):
        """Rebuilds the flattened (plugin, handler) snapshot iterated by :meth:`dispatch`.
        Must be called whenever self.plugins is mutated. The immutable snapshot also
        keeps a dispatch in progress safe from hooks being added or removed mid-event.
        """
        for plugin in self.plugins:
            if plugin not in self._plugin_order:
                self._plugin_order[plugin] = len(self._plugin_order)
        order = sorted(self.plugins, key=self._plugin_order.__getitem__)

        chain = []
        for priority in Priority:
            for plugin in order:
                for handler in self.plugins[plugin][priority]:
                    chain.append((plugin, handler))
        self._handler_chain = tuple(chain)
        self._warned_returns.clear()
        self._update_gate(bool(chain))

    def _update_gate(self, wanted):
        """Arm or disarm the engine handler slot for a gated event, if this is one.

        The only place the gate moves. :meth:`add_hook` and :meth:`remove_hook` both reach
        it through :meth:`_rebuild_chain`, so no 0<->1 crossing escapes.
        """
        if self.gated_handler is None or wanted == self._gate_armed:
            return

        handler = getattr(minqlxtended._handlers, self.gated_dispatch_fn) if wanted else None
        minqlxtended.register_handler(self.gated_handler, handler)
        self._gate_armed = wanted

    def handle_return(self, handler: Callable[..., Any], value: Any) -> Any:
        """Handle an unknown return value.

        Anything but None stops the event and is passed on to the C-level handlers.
        Override this to give an event its own return values. Editing *self.args* and
        *self.kwargs* changes what later handlers are passed, and *self.return_value* is
        what reaches the C-level handler if the event is not stopped.
        """
        name = getattr(handler, "__qualname__", repr(handler))
        if name in self._warned_returns:
            return

        self._warned_returns.add(name)
        minqlxtended.get_logger().warning(
            "Handler '%s' returned a bare %r for '%s', which this event does not "
            "understand. Return None (or Return.NONE) to do nothing, Return.STOP_EVENT "
            "to cancel the event, or Return.STOP_ALL for both.",
            name, value, self.name)

    def add_hook(self, plugin: str, handler: Callable[..., Any],
                 priority: int = Priority.NORMAL) -> None:
        """Hook the event, so the handler is called with the event's arguments whenever it
        takes place.

        :param plugin: The name of the plugin that's hooking the event.
        :type plugin: str
        :param handler: The handler to be called when the event takes place.
        :type handler: callable
        :param priority: The priority of the hook. Determines the order the handlers are called in.
        :type priority: minqlxtended.Priority
        :raises: ValueError

        """
        if priority not in Priority:
            levels = ", ".join(p.name for p in Priority)
            raise ValueError(f"'{priority}' is an invalid priority level. Valid levels are {levels}.")

        if self.need_zmq_stats_enabled and not bool(int(minqlxtended.require_cvar("zmq_stats_enable"))):
            raise AssertionError(f"{self.name} hook requires zmq_stats_enabled cvar to have nonzero value")

        _check_handler_signature(
            handler, self._handler_params, f"Event '{self.name}'",
            "Several event signatures changed. See the upgrade notes in README.md.")

        if plugin not in self.plugins:
            # One list per priority level, indexed by the Priority value.
            self.plugins[plugin] = tuple([] for _ in Priority)
        else:
            # Check if we've already registered this handler.
            for i in range(len(self.plugins[plugin])):
                for hook in self.plugins[plugin][i]:
                    if handler == hook:
                        raise ValueError("The event has already been hooked with the same handler and priority.")

        self.plugins[plugin][priority].append(handler)
        self._rebuild_chain()

    def remove_hook(self, plugin: str, handler: Callable[..., Any],
                    priority: int = Priority.NORMAL) -> None:
        """Removes a hooked event.

        :param plugin: The plugin that hooked the event.
        :type plugin: minqlxtended.Plugin
        :param handler: The handler used when hooked.
        :type handler: callable
        :param priority: The priority of the hook when hooked.
        :type priority: minqlxtended.Priority
        :raises: ValueError
        """

        if plugin in self.plugins:
            for hook in self.plugins[plugin][priority]:
                if handler == hook:
                    self.plugins[plugin][priority].remove(handler)
                    self._rebuild_chain()
                    return

        raise ValueError("The event has not been hooked with the handler provided")

class EventDispatcherManager:
    """Holds all the event dispatchers and provides a way to access the dispatcher
    instances by accessing it like a dictionary using the event name as a key.
    Only one dispatcher can be used per event.

    """
    def __init__(self):
        self._dispatchers = {}

    def _suggest(self, key):
        """Event names a plugin author plausibly meant by *key*, best first."""
        key = str(key)
        parts = set(key.split("_"))
        suggestions = [name for name in self._dispatchers
                       if name in key or set(name.split("_")) <= parts]
        suggestions.sort(key=len, reverse=True)

        for name in difflib.get_close_matches(key, self._dispatchers, n=3):
            if name not in suggestions:
                suggestions.append(name)

        return suggestions[:3]

    def __getitem__(self, key):
        try:
            return self._dispatchers[key]
        except KeyError:
            # A plain dict KeyError names the event you typed
            suggestions = self._suggest(key)
            names = " or ".join(f"'{s}'" for s in suggestions)
            hint = f" Did you mean {names}?" if suggestions else ""
            raise KeyError(f"Unknown event '{key}'.{hint} See minqlxtended.EVENT_DISPATCHERS for "
                           f"all {len(self._dispatchers)} of them.") from None

    def __contains__(self, key):
        return key in self._dispatchers

    def __iter__(self):
        return iter(self._dispatchers)

    def __len__(self):
        return len(self._dispatchers)

    def keys(self):
        """The name of every registered event."""
        return self._dispatchers.keys()

    def items(self):
        """Every ``(name, dispatcher)`` pair."""
        return self._dispatchers.items()

    def values(self):
        """Every registered dispatcher."""
        return self._dispatchers.values()

    def remove_all_hooks(self, plugin_name: str) -> int:
        """Drop every hook registered under *plugin_name*, across every event.

        For unwinding a plugin that failed partway through construction. Its hooks are live
        by then and nothing else can reach them, so loading it again would stack a second
        set on top.

        :param plugin_name: The plugin's name, as :attr:`minqlxtended.Plugin.name` gives it.
        :type plugin_name: str
        :returns: int -- how many hooks were removed.

        """
        removed = 0
        for dispatcher in self._dispatchers.values():
            handlers = dispatcher.plugins.pop(plugin_name, None)
            if handlers is None:
                continue

            removed += sum(len(by_priority) for by_priority in handlers)
            dispatcher._rebuild_chain()

        return removed

    def add_dispatcher(self, dispatcher: type["EventDispatcher"]) -> None:
        if dispatcher.name in self:
            raise ValueError("Event name already taken.")
        elif not issubclass(dispatcher, EventDispatcher):
            raise ValueError("Cannot add an event dispatcher not based on EventDispatcher.")

        self._dispatchers[dispatcher.name] = dispatcher()

    def remove_dispatcher(self, dispatcher: type["EventDispatcher"]) -> None:
        if dispatcher.name not in self:
            raise ValueError("Event name not found.")

        del self._dispatchers[dispatcher.name]

    def remove_dispatcher_by_name(self, event_name: str) -> None:
        if event_name not in self:
            raise ValueError("Event name not found.")

        del self._dispatchers[event_name]

# EVENT DISPATCHERS

class ConsolePrintDispatcher(EventDispatcher):
    """Event that goes off whenever the console prints something, including
    those with :func:`minqlxtended.console_print`.

    """
    name = "console_print"

    @override
    def dispatch(self, text):
        return super().dispatch(text)

    @override
    def handle_return(self, handler, value):
        """If a string was returned, continue execution, but we edit the
        string that's being printed along the chain of handlers.

        """
        if isinstance(value, str):
            self.args = (value,)
            self.return_value = value
        else:
            return super().handle_return(handler, value)

class CommandDispatcher(EventDispatcher):
    """Event that goes off when a command is executed. This can be used
    to for instance keep a log of all the commands admins have used.

    ``caller`` is the player who ran it, or None from the console. ``command`` is the
    :class:`minqlxtended.Command` object that matched. ``args`` is the raw line as typed,
    prefix and command name included; the vote events use the same parameter name for the
    argument tail with the name stripped.

    """
    name = "command"

    @override
    def dispatch(self, caller, command, args):
        return super().dispatch(caller, command, args)

class ClientCommandDispatcher(EventDispatcher):
    """Event that triggers with any client command. This overlaps with
    other events, such as "chat".

    The handler chain runs before the command invoker sees the line, so a handler here
    can cancel a ``!command`` outright or rewrite the text the parser reads. This is the
    event to hook to gate commands typed into chat, since "chat" runs its handlers after
    the command has already executed.

    """
    name = "client_command"

    @override
    def dispatch(self, player, cmd):
        ret = super().dispatch(player, cmd)
        if ret is False:
            return False

        # A handler may have rewritten the command. handle_return stores the edit in the
        # return value as well as self.args, which is only valid mid-dispatch.
        if isinstance(ret, str):
            cmd = ret

        if minqlxtended.COMMANDS.handle_input(player, cmd, minqlxtended.ClientCommandChannel(player)) is False:
            return False

        return ret

    @override
    def handle_return(self, handler, value):
        """If a string was returned, continue execution, but we edit the
        command that's being executed along the chain of handlers.

        """
        if isinstance(value, str):
            player, cmd = self.args
            self.args = (player, value)
            self.return_value = value
        else:
            return super().handle_return(handler, value)

class ServerCommandDispatcher(EventDispatcher):
    """Event that triggers with any server command sent by the server,
    including :func:`minqlxtended.send_server_command`. Can be cancelled.

    """
    name = "server_command"

    @override
    def dispatch(self, player, cmd):
        return super().dispatch(player, cmd)

    @override
    def handle_return(self, handler, value):
        """If a string was returned, continue execution, but we edit the
        command that's being sent along the chain of handlers.

        """
        if isinstance(value, str):
            player, cmd = self.args
            self.args = (player, value)
            self.return_value = value
        else:
            return super().handle_return(handler, value)

class FrameEventDispatcher(EventDispatcher):
    """Event that triggers every frame. Cannot be cancelled.

    """
    name = "frame"

    @override
    def dispatch(self):
        return super().dispatch()

class SetConfigstringDispatcher(EventDispatcher):
    """Event that triggers when the server tries to set a configstring. You can
    stop this event and use :func:`minqlxtended.set_configstring` to modify it, but a
    more elegant way to do it is simply returning the new configstring in
    the handler, and the modified one will go down the plugin chain instead.

    """
    name = "set_configstring"

    @override
    def dispatch(self, index, value):
        return super().dispatch(index, value)

    @override
    def handle_return(self, handler, value):
        """If a string was returned, continue execution, but we edit the
        configstring to the returned string. This allows multiple handlers
        to edit the configstring along the way before it's actually
        set by the QL engine.

        """
        if isinstance(value, str):
            index, old_value = self.args
            self.args = (index, value)
            self.return_value = value
        else:
            return super().handle_return(handler, value)

class ChatEventDispatcher(EventDispatcher):
    """Event that triggers with the "say" command. If the handler cancels it,
    the message will also be cancelled.

    Private messages raise it too. ``recipient`` is the player who was told, or None for
    anything said openly; ``channel`` addresses the *speaker* either way, since this is
    also where `!commands` are read from and a command must answer whoever typed it.

    Commands go first. A ``!command`` has already run and replied by the time any chat
    handler is called, so a handler here can neither cancel it nor rewrite the text the
    parser read; hook "client_command" or "command" for that. A command returning
    ``Return.STOP_EVENT`` or ``Return.STOP_ALL`` also stops this event.

    """
    name = "chat"

    @override
    def dispatch(self, player, msg, channel, recipient=None):
        ret = minqlxtended.COMMANDS.handle_input(player, msg, channel)
        if ret is False: # Stop event if told to.
            return False

        return super().dispatch(player, msg, channel, recipient)


class UnloadDispatcher(EventDispatcher):
    """Event that triggers whenever a plugin is unloaded. Cannot be cancelled."""
    name = "unload"

    @override
    def dispatch(self, plugin):
        return super().dispatch(plugin)

class PlayerConnectDispatcher(EventDispatcher):
    """Event that triggers whenever a player tries to connect. If the event
    is not stopped, it will let the player connect as usual. If it is stopped
    it will either display a generic ban message, or whatever string is returned
    by the handler.

    ``is_bot`` is the engine's own flag. **Prefer it to ``player.is_bot`` in a handler for
    this event.** This fires before the game module's ``ClientConnect`` has run, so the
    ``SVF_BOT`` bit ``Player.is_bot`` reads is unset and reports every bot as a human.
    """
    name = "player_connect"

    @override
    def dispatch(self, player, is_bot):
        return super().dispatch(player, is_bot)

    @override
    def handle_return(self, handler, value):
        """If a string was returned, stop execution of event, disallow
        the player from connecting, and display the returned string as
        a message to the player trying to connect.

        """
        if isinstance(value, str):
            return value
        else:
            return super().handle_return(handler, value)

class PlayerLoadedDispatcher(EventDispatcher):
    """Event that triggers whenever a player connects *and* finishes loading.
    This means it'll trigger later than the "X connected" messages in-game,
    and it will also trigger when a map changes and players finish loading it.

    """
    name = "player_loaded"

    @override
    def dispatch(self, player):
        return super().dispatch(player)

class PlayerDisconnectDispatcher(EventDispatcher):
    """Event that triggers whenever a player disconnects. Cannot be cancelled."""
    name = "player_disconnect"

    @override
    def dispatch(self, player, reason):
        return super().dispatch(player, reason)

class PlayerSpawnDispatcher(EventDispatcher):
    """Event that triggers when a player spawns. Cannot be cancelled."""
    name = "player_spawn"

    @override
    def dispatch(self, player):
        return super().dispatch(player)

class StatsDispatcher(EventDispatcher):
    """Event that triggers whenever the server sends stats over ZMQ."""
    name = "stats"
    need_zmq_stats_enabled = True

    @override
    def dispatch(self, stats):
        return super().dispatch(stats)

class VoteCalledDispatcher(EventDispatcher):
    """Event that goes off whenever a player tries to call a vote. Note that
    this goes off even if it's a vote command that is invalid. Use vote_started
    if you only need votes that actually go through. Use this one for custom votes.

    """
    name = "vote_called"

    @override
    def dispatch(self, player, vote, args):
        res = super().dispatch(player, vote, args)
        if res is False:
            return False

        consumed = minqlxtended.CUSTOM_VOTES.dispatch(player, vote, args)
        if consumed is not None:
            return consumed

        return res

class VoteStartedDispatcher(EventDispatcher):
    """Event that goes off whenever a vote starts. ``caller`` is None when the engine
    started the vote itself, or when :meth:`minqlxtended.Plugin.callvote` was given no
    caller.
    """
    name = "vote_started"

    @override
    def dispatch(self, caller, vote, args):
        return super().dispatch(caller, vote, args)

class VoteEndedDispatcher(EventDispatcher):
    """Event that goes off whenever a vote either passes or fails."""
    name = "vote_ended"

    @override
    def dispatch(self, votes, vote, args, passed):
        return super().dispatch(votes, vote, args, passed)

class VoteDispatcher(EventDispatcher):
    """Event that goes off whenever someone tries to vote either yes or no."""
    name = "vote"

    @override
    def dispatch(self, player, yes):
        return super().dispatch(player, yes)

class GameCountdownDispatcher(EventDispatcher):
    """Event that goes off when the countdown before a game starts."""
    name = "game_countdown"

    @override
    def dispatch(self):
        return super().dispatch()

class GameStartDispatcher(EventDispatcher):
    """Event that goes off when a game starts."""
    name = "game_start"

    @override
    def dispatch(self):
        return super().dispatch()

class GameEndDispatcher(EventDispatcher):
    """Event that goes off when a game ends. ``aborted`` is True when the match stopped
    without a result: a forfeit, a map change mid-game, or an admin ending it.

    Scores and per-player stats are not passed: read them off :class:`minqlxtended.Game`
    and :attr:`minqlxtended.Player.expanded_stats`, which are live at the time this fires.
    """
    name = "game_end"

    @override
    def dispatch(self, aborted):
        return super().dispatch(aborted)

class RoundCountdownDispatcher(EventDispatcher):
    """Event that goes off when the countdown before a round starts."""
    name = "round_countdown"

    @override
    def dispatch(self, round_number):
        return super().dispatch(round_number)

class RoundStartDispatcher(EventDispatcher):
    """Event that goes off when a round starts."""
    name = "round_start"

    @override
    def dispatch(self, round_number):
        return super().dispatch(round_number)

class RoundEndDispatcher(EventDispatcher):
    """Event that goes off when a round ends. ``round_number`` uses the same numbering as
    :class:`RoundStartDispatcher`, ``winning_team`` is a team name or None on a draw, and
    ``time`` is how long the round lasted, in milliseconds.
    """
    name = "round_end"

    @override
    def dispatch(self, round_number, winning_team, time):
        return super().dispatch(round_number, winning_team, time)

class TeamSwitchDispatcher(EventDispatcher):
    """For when a player switches teams. If cancelled,
    simply put the player back in the old team.

    If possible, consider using team_switch_attempt for a cleaner
    solution if you need to cancel the event."""
    name = "team_switch"

    @override
    def dispatch(self, player, old_team, new_team):
        return super().dispatch(player, old_team, new_team)

class TeamSwitchAttemptDispatcher(EventDispatcher):
    """For when a player attempts to join a team. Prevents the switch when cancelled.

    Comes from the SetTeam hook, so it fires for admin puts, duel-queue promotion and
    follow-cycling as well as for a player typing `team X`.

    ``new_team`` is the team being asked for, with no promise it's granted. The Join Match
    button sends "any", meaning "pick one"; SetTeam ignores `red`/`blue` and picks a team
    itself while the level is in warmup, and refuses outright when the arena or team is
    full or the teams would go out of balance. Hook ``team_switch`` for what happened.

    ``target`` is SetTeam's raw argument, and the only way to tell `follow1`/`follow2`
    (both of which land on spectator) from an ordinary spectate.

    """
    name = "team_switch_attempt"

    @override
    def dispatch(self, player, old_team, new_team, target):
        return super().dispatch(player, old_team, new_team, target)

class MapDispatcher(EventDispatcher):
    """Event that goes off when a map is loaded, even if the same map is loaded again."""
    name = "map"

    @override
    def dispatch(self, mapname, factory):
        return super().dispatch(mapname, factory)

class NewGameDispatcher(EventDispatcher):
    """Event that goes off when the game module is initialized. This happens when new maps are loaded,
    a game is aborted, a game ends but stays on the same map, or when the game itself starts.

    """
    name = "new_game"

    @override
    def dispatch(self):
        return super().dispatch()

class KillDispatcher(EventDispatcher):
    """Event that goes off when one player kills another. Suicides and deaths caused by
    the map don't raise it; hook ``death`` for those. ``mod`` is the means of death as a
    :class:`minqlxtended.MeansOfDeath`, e.g. ``MeansOfDeath.GAUNTLET``.
    """
    name = "kill"

    @override
    def dispatch(self, victim, killer, mod):
        return super().dispatch(victim, killer, mod)

class DeathDispatcher(EventDispatcher):
    """Event that goes off when someone dies, however it happened. ``killer`` is None when
    nobody was responsible (lava, a trigger_hurt, falling) and is the victim themselves on
    a suicide.
    """
    name = "death"

    @override
    def dispatch(self, victim, killer, mod):
        return super().dispatch(victim, killer, mod)

class WeaponFiredDispatcher(EventDispatcher):
    """Event that goes off every time a player fires a weapon.

    ``weapon`` is a :class:`minqlxtended.Weapon`, and None if the engine named one this
    build doesn't know. Ask the member for :attr:`~minqlxtended.Weapon.short` if you want
    the abbreviation. Raised after the shot, so it can't be cancelled.

    Like ``damage``, this event is **gated**: the engine doesn't call into Python for it
    until something hooks it. Roughly twenty a second per player with the lightning gun,
    so keep handlers cheap.

    """
    name = "weapon_fired"
    gated_handler = "weapon_fired"
    gated_dispatch_fn = "handle_weapon_fired"

    @override
    def dispatch(self, player, weapon):
        return super().dispatch(player, weapon)

class ObjectiveDispatcher(EventDispatcher):
    """Event that goes off when a player's objective counter goes up: a flag capture, a
    return, an assist or a defend.

    Derived from the frame poll off ``pers.team_state``, so it catches these however the
    game module awarded them.

    ``kind`` is always a :class:`minqlxtended.Objective`, and
    :attr:`~minqlxtended.Objective.UNKNOWN` if the engine named a counter this build
    doesn't know. ``count`` is the counter's new total, so it's the same number the
    scoreboard shows.

    """
    name = "objective"

    @override
    def dispatch(self, player, kind, count):
        return super().dispatch(player, kind, count)

class DamageDispatcher(EventDispatcher):
    """Event that goes off every time a player takes damage, from any source.

    ``attacker`` is None when no client was responsible (lava, a trigger_hurt, falling) and
    is the target themselves on self-damage. ``dflags`` is a raw bitfield of
    :class:`minqlxtended.DamageFlag` and ``mod`` the means of death as a
    :class:`minqlxtended.MeansOfDeath`.

    Raised after the damage has been applied, so the target's health already reflects it.
    Can't be cancelled: the engine applies armour, powerups and scoring internally.

    This event is **gated**: the engine doesn't call into Python for it at all until
    something hooks it. It fires more often than any other event, so keep handlers cheap,
    and hook ``death`` instead where that will do.

    """
    name = "damage"
    gated_handler = "damage"
    gated_dispatch_fn = "handle_player_damage"

    @override
    def dispatch(self, target, attacker, damage, dflags, mod):
        return super().dispatch(target, attacker, damage, dflags, mod)

class CvarChangedDispatcher(EventDispatcher):
    """Event that goes off when a write changes a cvar's live value, whoever wrote it: the
    console, the game module, a vote, or a plugin.

    ``new_value`` is the text the engine kept, so a range-flagged cvar reports what it was
    clamped to. No event fires for a cvar's creation, for a write that left the value as it
    was, or for an unforced write to a latched cvar. The ``new_game`` refresh picks those
    up.

    Raised after the write, so it can't be cancelled, and always on the game thread; a
    write made off the main thread is re-raised on the next frame. Gated like ``damage``.

    """
    name = "cvar_changed"
    gated_handler = "cvar_changed"
    gated_dispatch_fn = "handle_cvar_changed"

    @override
    def dispatch(self, name, old_value, new_value):
        return super().dispatch(name, old_value, new_value)

class UserinfoDispatcher(EventDispatcher):
    """Event for clients changing their userinfo.

    ``changed`` is only the keys whose values differ from what the player had.
    ``infostring`` is the whole thing the client sent, for the cases where the diff isn't
    enough: spotting a key being removed, say, or logging what actually arrived.

    Returning a dict rewrites the change. Those are merged across handlers, so two plugins
    editing different keys both take effect and ``changed`` grows to include what earlier
    handlers asked for.

    """
    name = "userinfo"

    @override
    def dispatch(self, player, changed, infostring):
        return super().dispatch(player, changed, infostring)

    @override
    def handle_return(self, handler, value):
        """Takes a returned dictionary and applies it to the current userinfo.

        Merged into what's already there, so two plugins changing different keys both take
        effect and later handlers see every key that changed.

        """
        if isinstance(value, dict):
            player, changed, infostring = self.args
            merged = dict(changed)
            merged.update(value)
            self.args = (player, merged, infostring)
            self.return_value = merged
        else:
            return super().handle_return(handler, value)

class KamikazeUseDispatcher(EventDispatcher):
    """Event that goes off when player uses kamikaze item."""
    name = "kamikaze_use"

    @override
    def dispatch(self, player):
        return super().dispatch(player)

class KamikazeExplodeDispatcher(EventDispatcher):
    """Event that goes off when kamikaze explodes."""
    name = "kamikaze_explode"

    @override
    def dispatch(self, player, is_used_on_demand):
        return super().dispatch(player, is_used_on_demand)


class ItemPickupDispatcher(EventDispatcher):
    """Event that goes off when a player picks an item up. Only successful pickups raise
    it, rather than on every touch of an item trigger. ``item_name`` is the entity classname, e.g.
    "item_armor_body" or "weapon_rocketlauncher".

    """
    name = "item_pickup"

    @override
    def dispatch(self, player, item_name):
        return super().dispatch(player, item_name)


class DemoFinishedDispatcher(EventDispatcher):
    """Event that goes off when a server-side demo has been written and closed. Carries a
    client id rather than a :class:`minqlxtended.Player`, since the player may have left."""
    name = "demo_finished"

    @override
    def dispatch(self, client_id, path, size, discarded, failed):
        return super().dispatch(client_id, path, size, discarded, failed)


EVENT_DISPATCHERS = EventDispatcherManager()
EVENT_DISPATCHERS.add_dispatcher(ConsolePrintDispatcher)
EVENT_DISPATCHERS.add_dispatcher(CommandDispatcher)
EVENT_DISPATCHERS.add_dispatcher(ClientCommandDispatcher)
EVENT_DISPATCHERS.add_dispatcher(ServerCommandDispatcher)
EVENT_DISPATCHERS.add_dispatcher(FrameEventDispatcher)
EVENT_DISPATCHERS.add_dispatcher(SetConfigstringDispatcher)
EVENT_DISPATCHERS.add_dispatcher(ChatEventDispatcher)
EVENT_DISPATCHERS.add_dispatcher(UnloadDispatcher)
EVENT_DISPATCHERS.add_dispatcher(PlayerConnectDispatcher)
EVENT_DISPATCHERS.add_dispatcher(PlayerLoadedDispatcher)
EVENT_DISPATCHERS.add_dispatcher(PlayerDisconnectDispatcher)
EVENT_DISPATCHERS.add_dispatcher(PlayerSpawnDispatcher)
EVENT_DISPATCHERS.add_dispatcher(KamikazeUseDispatcher)
EVENT_DISPATCHERS.add_dispatcher(KamikazeExplodeDispatcher)
EVENT_DISPATCHERS.add_dispatcher(DemoFinishedDispatcher)
EVENT_DISPATCHERS.add_dispatcher(StatsDispatcher)
EVENT_DISPATCHERS.add_dispatcher(VoteCalledDispatcher)
EVENT_DISPATCHERS.add_dispatcher(VoteStartedDispatcher)
EVENT_DISPATCHERS.add_dispatcher(VoteEndedDispatcher)
EVENT_DISPATCHERS.add_dispatcher(VoteDispatcher)
EVENT_DISPATCHERS.add_dispatcher(GameCountdownDispatcher)
EVENT_DISPATCHERS.add_dispatcher(GameStartDispatcher)
EVENT_DISPATCHERS.add_dispatcher(GameEndDispatcher)
EVENT_DISPATCHERS.add_dispatcher(RoundCountdownDispatcher)
EVENT_DISPATCHERS.add_dispatcher(RoundStartDispatcher)
EVENT_DISPATCHERS.add_dispatcher(RoundEndDispatcher)
EVENT_DISPATCHERS.add_dispatcher(TeamSwitchDispatcher)
EVENT_DISPATCHERS.add_dispatcher(TeamSwitchAttemptDispatcher)
EVENT_DISPATCHERS.add_dispatcher(MapDispatcher)
EVENT_DISPATCHERS.add_dispatcher(NewGameDispatcher)
EVENT_DISPATCHERS.add_dispatcher(KillDispatcher)
EVENT_DISPATCHERS.add_dispatcher(DeathDispatcher)
EVENT_DISPATCHERS.add_dispatcher(DamageDispatcher)
EVENT_DISPATCHERS.add_dispatcher(CvarChangedDispatcher)
EVENT_DISPATCHERS.add_dispatcher(ObjectiveDispatcher)
EVENT_DISPATCHERS.add_dispatcher(WeaponFiredDispatcher)
EVENT_DISPATCHERS.add_dispatcher(UserinfoDispatcher)
EVENT_DISPATCHERS.add_dispatcher(ItemPickupDispatcher)
