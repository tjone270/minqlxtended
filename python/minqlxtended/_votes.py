# minqlxtended - Extends Quake Live's dedicated server with extra functionality and scripting.
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

from __future__ import annotations

import itertools
import typing
from typing import Any, Callable

import minqlxtended

from ._commands import Command
from ._enums import Priority

__all__ = (
    "CUSTOM_VOTES",
    "CustomVote",
    "CustomVoteManager",
)


class CustomVote(typing.NamedTuple):
    """What a custom vote callback returns to start its vote.

    ``display`` is the text players see on the vote HUD. ``execute`` is a vote string the
    engine runs when the vote passes, or a callable invoked at that same moment.

    """

    display: str
    execute: str | Callable[[], Any]


class _FrameworkOwner:
    """Stands in as ``Command.plugin`` for the framework's executor command.

    ``CommandInvoker.remove_all_for_plugin`` matches on the owner's type name, so no
    unload sweep takes the executor away.

    """

    name = "minqlxtended"

    def __str__(self) -> str:
        return self.name


class CustomVoteManager:
    """The custom vote registry: one instance, :data:`minqlxtended.CUSTOM_VOTES`.

    Register a vote name with :meth:`minqlxtended.Plugin.add_vote`.
    :class:`minqlxtended._events.VoteCalledDispatcher` hands a called vote here once every
    plugin hook has run, so policy hooks keep their veto. Names are unique across every
    plugin, and registering one twice raises.

    """

    #: The console command the engine executes when a callable vote passes. The token
    #: after it names the stored callable.
    EXECUTE_COMMAND = "qlx_custom_vote"

    def __init__(self) -> None:
        self._votes: dict[str, tuple[Any, Callable[..., Any], str, str]] = {}
        self._pending: dict[int, tuple[str, Callable[[], Any]]] = {}
        self._tokens = itertools.count()
        self._owner = _FrameworkOwner()
        self._installed = False

    @property
    def votes(self) -> dict[str, tuple[Any, str, str]]:
        """The registered votes: ``{name: (plugin, usage, description)}``, for help output."""
        return {name: (entry[0], entry[2], entry[3])
                for name, entry in self._votes.items()}

    def add_vote(self, plugin: Any, name: str, callback: Callable[..., Any],
                 usage: str = "", description: str = "") -> None:
        lowered = name.lower()
        if lowered in self._votes:
            raise ValueError(f"Custom vote '{lowered}' is already registered.")

        minqlxtended._events._check_handler_signature(
            callback, ["caller", "args"], "A custom vote")

        self._install()
        self._votes[lowered] = (plugin, callback, usage, description)

    def remove_vote(self, name: str) -> None:
        if self._votes.pop(name.lower(), None) is None:
            raise ValueError(f"Custom vote '{name.lower()}' is not registered.")

    def remove_all_for_plugin(self, plugin_name: str) -> int:
        """Drop every vote *plugin_name* registered, and its pending callables.

        The counterpart of ``COMMANDS.remove_all_for_plugin``, called from the same places.

        """
        doomed = [name for name, entry in self._votes.items()
                  if type(entry[0]).__name__ == plugin_name]
        for name in doomed:
            del self._votes[name]

        stale = [token for token, (owner_name, _) in self._pending.items()
                 if owner_name == plugin_name]
        for token in stale:
            del self._pending[token]

        return len(doomed)

    def dispatch(self, caller: Any, vote: str, args: str) -> Any:
        """Run the registered callback for *vote*; None when the name isn't ours.

        Every handled outcome returns False. The engine knows nothing of these names and
        would tell the caller the vote is invalid.

        """
        entry = self._votes.get(vote.lower())
        if entry is None:
            return None

        plugin, callback = entry[0], entry[1]

        if minqlxtended.Plugin.is_vote_active():
            caller.tell("A vote is already in progress.")
            return False

        try:
            result = callback(caller, args)
        except:
            minqlxtended.log_exception(plugin)
            return False

        if result is None:
            # Declined: the callback has already told the caller what was wrong.
            return False

        if not isinstance(result, CustomVote):
            minqlxtended.get_logger(plugin).warning(
                "Custom vote '%s' returned %r instead of a CustomVote or None.",
                vote.lower(), result)
            return False

        token = None
        if callable(result.execute):
            token = next(self._tokens)
            self._trim_pending()
            self._pending[token] = (type(plugin).__name__, result.execute)
            vote_string = f"{self.EXECUTE_COMMAND} {token}"
        else:
            vote_string = result.execute

        if minqlxtended.Plugin.callvote(vote_string, result.display, caller=caller):
            # Announce the vote and cast the caller's own yes.
            minqlxtended.client_command(caller.id, "vote yes")
            minqlxtended.CHAT_CHANNEL.reply(f"{caller.name}^7 called a vote.")
        elif token is not None:
            # The callback started a vote of its own between our check and here, so the
            # engine refused this one and the token will never be executed.
            self._pending.pop(token, None)

        return False

    def _trim_pending(self) -> None:
        """Keep at most one older callable before a new vote adds its own.

        A passed vote's string runs three seconds after the result, so one token can still
        be waiting out that window.

        """
        while len(self._pending) > 1:
            del self._pending[next(iter(self._pending))]

    def _install(self) -> None:
        if self._installed:
            return

        minqlxtended.add_console_command(self.EXECUTE_COMMAND)
        minqlxtended.COMMANDS.add_command(
            Command(plugin=self._owner, name=self.EXECUTE_COMMAND,
                    handler=self._execute_pending, permission=0, channels=("console",),
                    exclude_channels=(), client_cmd_pass=False, client_cmd_perm=0,
                    prefix=False, usage=""),
            Priority.NORMAL)

        minqlxtended.EVENT_DISPATCHERS["vote_ended"].add_hook(
            "minqlxtended", self._handle_vote_ended)

        minqlxtended.EVENT_DISPATCHERS["map"].add_hook(
            "minqlxtended", self._handle_map)
        self._installed = True

    def _execute_pending(self, player, msg, channel):
        try:
            token = int(msg[1])
        except (IndexError, ValueError):
            return

        entry = self._pending.pop(token, None)
        if entry is not None:
            entry[1]()

    def _handle_map(self, mapname, factory):
        self._pending.clear()

    def _handle_vote_ended(self, votes, vote, args, passed):
        if passed or vote != self.EXECUTE_COMMAND:
            return

        try:
            self._pending.pop(int(args), None)
        except ValueError:
            pass


CUSTOM_VOTES = CustomVoteManager()
