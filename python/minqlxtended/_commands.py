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
import threading
import re
from typing import Any

from ._core import _QUOTED_FORBIDDEN, _check_command_value
from ._enums import Priority, Return, Team

__all__ = (
    "AbstractChannel",
    "BLUE_TEAM_CHAT_CHANNEL",
    "BlueTeamChatChannel",
    "CHAT_CHANNEL",
    "COMMANDS",
    "CONSOLE_CHANNEL",
    "ChatChannel",
    "ClientCommandChannel",
    "Command",
    "CommandInvoker",
    "ConsoleChannel",
    "FREE_CHAT_CHANNEL",
    "FreeChatChannel",
    "MAX_MSG_LENGTH",
    "RED_TEAM_CHAT_CHANNEL",
    "RedTeamChatChannel",
    "SPECTATOR_CHAT_CHANNEL",
    "SpectatorChatChannel",
    "TellChannel",
    "re_color_tag",
)

MAX_MSG_LENGTH = 1000


def center_print(client_id, msg):
    """Print in the middle of a client's screen, or everyone's when *client_id* is None.

    Not exported. Use :meth:`Plugin.center_print` to broadcast and
    :meth:`Player.center_print` for one client; those two take their arguments the other
    way round. Only the quote is refused; the message sits inside quotes this writes.

    :raises ValueError: if *msg* contains a quote.
    """
    checked = _check_command_value(msg, "Centerprint", _QUOTED_FORBIDDEN)
    return minqlxtended.send_server_command(client_id, f'cp "{checked}"')

re_color_tag = re.compile(r"\^[0-7]")

# COMMANDS

class Command:
    """An input-triggered command: its names, usage, permissions and handler."""
    def __init__(self, plugin, name, handler, permission, channels, exclude_channels, client_cmd_pass, client_cmd_perm, prefix, usage):
        if not (channels is None or hasattr(channels, "__iter__")):
            raise ValueError("'channels' must be a finite iterable or None.")
        elif not (exclude_channels is None or hasattr(exclude_channels, "__iter__")):
            raise ValueError("'exclude_channels' must be a finite iterable or None.")
        # Same check as the event handlers in _events.py: a wrong signature otherwise waits
        # until someone types the command to show itself.
        minqlxtended._events._check_handler_signature(
            handler, ["player", "msg", "channel"], "A command")

        self.plugin = plugin # Instance of the owner.

        # Allow a command to have alternative names. Both forms are lowercased because the
        # name index and handle_input both lowercase before comparing.
        if isinstance(name, (list, tuple)):
            names = [n.lower() for n in name]
        else:
            names = [name.lower()]

        if not names:
            raise ValueError("A command needs at least one name.")

        #: Every name this command answers to, the first being the primary one.
        self.names = names
        self.handler = handler
        self.permission = permission
        self.channels = list(channels) if channels is not None else []
        self.exclude_channels = list(exclude_channels) if exclude_channels is not None else []
        self.client_cmd_pass = client_cmd_pass
        self.client_cmd_perm = client_cmd_perm
        self.prefix = prefix
        self.usage = usage

    @property
    def name(self) -> str:
        """The primary name, as a string. See :attr:`names` for all of them."""
        return self.names[0]

    @property
    def aliases(self) -> list[str]:
        """The other names this command answers to."""
        return self.names[1:]

    def __repr__(self) -> str:
        owner = self.plugin.name if self.plugin is not None else None
        handler = getattr(self.handler, "__name__", self.handler)
        return f"{type(self).__name__}({self.name!r} -> {owner}.{handler})"

    def __eq__(self, other: object) -> bool:
        """Two commands are the same registration if they answer to the same names with
        the same handler."""
        if not isinstance(other, Command):
            return NotImplemented

        return self.names == other.names and self.handler == other.handler

    def __hash__(self) -> int:
        return hash((tuple(self.names), self.handler))

    def execute(self, player: Any, msg: str, channel: Any) -> Any:
        logger = minqlxtended.get_logger(self.plugin)
        # %-args instead of format(), so we don't interpolate anything (or call
        # channel.__str__) unless the record is really going to be emitted.
        logger.debug("%s executed: %s @ %s -> %s", player.steam_id, self.name, self.plugin.name, channel)
        return self.handler(player, msg.split(), channel)

    def is_eligible_name(self, name: str, prefix: str | None = None) -> bool:
        """Whether *name*, as the player typed it, addresses this command.

        Not on the dispatch path; handle_input matches through the name index instead.
        """
        if self.prefix:
            if prefix is None:
                prefix = minqlxtended.get_cvar("qlx_commandPrefix") or ""
            if not name.startswith(prefix):
                return False
            name = name[len(prefix):]

        return name.lower() in self.names

    def is_eligible_channel(self, channel: Any) -> bool:
        """Whether this command should execute in *channel*. Exclude takes precedence."""
        if channel in self.exclude_channels:
            return False
        elif not self.channels or channel.name in self.channels:
            return True
        else:
            return False

    _warned_permission_cvars: set[str] = set()

    @classmethod
    def _permission_cvar(cls, name, value, default):
        """Parse a permission-override cvar, falling back to the registered default.

        Warns once per cvar instead of on every chat line, since the value is read for
        every command match.
        """
        try:
            return int(value)
        except ValueError:
            if name not in cls._warned_permission_cvars:
                cls._warned_permission_cvars.add(name)
                minqlxtended.get_logger().warning(
                    "Cvar '%s' is set to '%s', which is not a permission level. "
                    "Using the command's default of %s.", name, value, default)
            return default

    def is_eligible_player(self, player: Any, is_client_cmd: bool) -> bool:
        """Check if a player has the rights to execute the command."""
        # Check if config overrides permission.
        perm = self.permission
        client_cmd_perm = self.client_cmd_perm

        if is_client_cmd:
            cvar_client_cmd = minqlxtended.get_cvar("qlx_ccmd_perm_" + self.name)
            if cvar_client_cmd:
                client_cmd_perm = self._permission_cvar(
                    "qlx_ccmd_perm_" + self.name, cvar_client_cmd, client_cmd_perm)
        else:
            cvar = minqlxtended.get_cvar("qlx_perm_" + self.name)
            if cvar:
                perm = self._permission_cvar("qlx_perm_" + self.name, cvar, perm)

        if (player.steam_id == minqlxtended.owner() or
            (not is_client_cmd and perm == 0) or
            (is_client_cmd and client_cmd_perm == 0)):
            return True

        player_perm = self.plugin.db.get_permission(player)
        if is_client_cmd:
            return player_perm >= client_cmd_perm
        else:
            return player_perm >= perm

class CommandInvoker:
    """Holds every registered command and runs the eligible ones on each line of input."""
    def __init__(self):
        # One list per priority level, indexed by the Priority value.
        self._commands = tuple([] for _ in Priority)

        self._index_prefixed = {}
        self._index_raw = {}
        self._seq = 0

    @property
    def commands(self):
        c = []
        for cmds in self._commands:
            c.extend(cmds)

        return c

    def _index_for(self, command):
        return self._index_prefixed if command.prefix else self._index_raw

    def _index_add(self, command, priority):
        index = self._index_for(command)
        # One number per command, so all of a command's aliases share a position.
        self._seq += 1
        for name in command.names:
            entry = index.get(name, ()) + ((priority, self._seq, command),)
            index[name] = tuple(sorted(entry, key=lambda item: (item[0], item[1])))

    def _index_remove(self, command):
        index = self._index_for(command)
        for name in command.names:
            entry = index.get(name)
            if entry is None:
                continue

            entry = tuple(item for item in entry if item[2] is not command)
            if entry:
                index[name] = entry
            else:
                del index[name]

    def _eligible_by_name(self, name, prefix):
        """The commands registered under the name a player typed, in dispatch order.

        *name* must already be lowercased; command names are stored that way.
        """
        raw = self._index_raw.get(name)
        stripped = None

        if prefix is not None and name.startswith(prefix):
            stripped = self._index_prefixed.get(name[len(prefix):])

        if not stripped:
            return raw or ()
        if not raw:
            return stripped

        return sorted(stripped + raw, key=lambda item: (item[0], item[1]))

    def add_command(self, command: Command, priority: int) -> None:
        if priority not in Priority:
            levels = ", ".join(p.name for p in Priority)
            raise ValueError(f"'{priority}' is an invalid priority level. Valid levels are {levels}.")

        if self.is_registered(command):
            raise ValueError("Attempted to add an already registered command.")

        self._commands[priority].append(command)
        self._index_add(command, priority)

    def remove_command(self, command: Command) -> None:
        if not self.is_registered(command):
            raise ValueError("Attempted to remove a command that was never added.")
        else:
            for priority_level in self._commands:
                for cmd in priority_level:
                    if cmd == command:
                        priority_level.remove(cmd)
                        self._index_remove(cmd)
                        return

    def remove_all_for_plugin(self, plugin_name: str) -> int:
        """Drop every command registered by *plugin_name*.

        The counterpart to :meth:`minqlxtended.EventDispatcherManager.remove_all_hooks`

        :param plugin_name: The plugin's name, as :attr:`minqlxtended.Plugin.name` gives it.
        :type plugin_name: str
        :returns: int -- how many commands were removed.
        """
        doomed = [cmd for cmd in self.commands
                  if type(cmd.plugin).__name__ == plugin_name]
        for cmd in doomed:
            self.remove_command(cmd)

        return len(doomed)

    def is_registered(self, command: Command) -> bool:
        """Check if a command is already registed.

        Commands are unique by (names, handler); Command.__eq__ compares that pair.
        """
        for priority_level in self._commands:
            for cmd in priority_level:
                if command == cmd:
                    return True

        return False

    def handle_input(self, player: Any, msg: str, channel: Any) -> Any:
        if not msg.strip():
            return

        name = msg.strip().split(" ", 1)[0].lower()
        is_client_cmd = channel == "client_command"
        pass_through = True
        prefix = minqlxtended.get_cvar("qlx_commandPrefix")
        if prefix is not None:
            prefix = prefix.lower()

        for _, _, cmd in self._eligible_by_name(name, prefix):
            # A raising handler, or an eligibility check that raises with the database down,
            # is this command's failure alone. The rest of the line still runs.
            try:
                if not (cmd.is_eligible_channel(channel) and cmd.is_eligible_player(player, is_client_cmd)):
                    continue

                if is_client_cmd and pass_through:
                    pass_through = cmd.client_cmd_pass

                if minqlxtended.EVENT_DISPATCHERS["command"].dispatch(player, cmd, msg) is False:
                    return pass_through

                res = cmd.execute(player, msg, channel)
                if res == Return.STOP:
                    return pass_through
                elif res == Return.STOP_EVENT:
                    pass_through = False
                elif res == Return.STOP_ALL:
                    # C-level dispatchers expect False if it shouldn't go to the engine.
                    return False
                elif res == Return.USAGE:
                    if cmd.usage:
                        channel.reply(f"^7Usage: ^6{name} {cmd.usage}")
                    else:
                        logger = minqlxtended.get_logger(None)
                        logger.warning("Command '%s' with handler '%s' returned Return.USAGE, "
                            "but was registered without a usage string, so the player was told "
                            "nothing.", cmd.name, cmd.handler.__name__)
                elif isinstance(res, threading.Thread):
                    logger = minqlxtended.get_logger(None)
                    logger.warning("Command '%s' with handler '%s' is decorated with "
                        "@minqlxtended.thread, so its return value is lost. Validate on the "
                        "calling thread and move the work into a threaded helper.",
                        cmd.name, cmd.handler.__name__)
                elif res is not None and res != Return.NONE:
                    logger = minqlxtended.get_logger(None)
                    logger.warning("Command '%s' with handler '%s' returned an unknown return value: %s",
                                   cmd.name, cmd.handler.__name__, res)
            except:
                minqlxtended.log_exception(cmd.plugin)
                continue

        return pass_through

# CHANNELS

class AbstractChannel:
    """A chat channel: where a message came from, and where a reply goes.

    Subclasses implement reply(). Comparing two channels compares their __repr__(), while
    the channel list a command registers with is matched against __str__(). For a web
    interface with several users at once, set "name" to "webinterface" and have __repr__()
    return "webinterface user1".
    """
    def __init__(self, name):
        self._name = name

    def __str__(self):
        return self.name

    def __repr__(self):
        return str(self)

    def __eq__(self, other):
        if isinstance(other, str):
            return self.name == other
        else:
            return repr(self) == repr(other)

    def __hash__(self):
        return hash(self.name)

    @property
    def name(self):
        return self._name

    def reply(self, msg: Any, limit: int = 100, delimiter: str = " ") -> None:
        """Send *msg* to whoever this channel reaches.

        Every channel takes the same arguments, whether or not it uses them.
        """
        raise NotImplementedError()

    def split_long_lines(self, msg: str, limit: int = 100,
                         delimiter: str = " ") -> list[str]:
        res = []

        while msg:
            i = msg.find("\n")
            if 0 <= i <= limit:
                res.append(msg[:i])
                msg = msg[i+1:]
                continue

            if len(msg) < limit:
                if msg:
                    res.append(msg)
                break

            length = 0
            while True:
                i = msg[length:].find(delimiter)
                if i == -1 or i+length > limit:
                    if not length:
                        # No delimiter within the limit: hard-wrap at the limit
                        # without consuming (dropping) a character.
                        res.append(msg[:limit])
                        msg = msg[limit:]
                    else:
                        res.append(msg[:length-1])
                        msg = msg[length+len(delimiter)-1:]
                    break
                else:
                    length += i+1

        return res

class ChatChannel(AbstractChannel):
    """A channel for chat to and from the server. Name and audience are constructor
    arguments."""
    DEFAULT_FMT = "print \"{}\n\"\n"

    def __init__(self, name="chat", fmt=None, team=None):
        super().__init__(name)
        self.fmt = self.DEFAULT_FMT if fmt is None else fmt
        self.team = team

    def _targets(self):
        """The client ids this channel reaches, or None to broadcast."""
        if self.team is None:
            return None

        return [p.id for p in minqlxtended.Player.all_players() if p.team == self.team]

    @minqlxtended.next_frame
    def reply(self, msg, limit=100, delimiter=" "):
        # TODO: rcon can print quotes to clients using NET_OutOfBandPrint. Maybe we should too?
        msg = str(msg).replace("\"", "'")
        last_color = ""
        targets = self._targets()

        split_msgs = self.split_long_lines(msg, limit, delimiter)
        joined_msgs = []
        for s in split_msgs:
            if not len(joined_msgs):
                joined_msgs.append(s)
            else:
                s_new = joined_msgs[-1] + "\n" + s
                if len(s_new.encode(errors="replace")) > MAX_MSG_LENGTH:
                    joined_msgs.append(s)
                else:
                    joined_msgs[-1] = s_new

        for s in joined_msgs:
            if targets is None:
                minqlxtended.send_server_command(None, self.fmt.format(last_color + s))
            else:
                for cid in targets:
                    minqlxtended.send_server_command(cid, self.fmt.format(last_color + s))

            find = re_color_tag.findall(s)
            if find:
                last_color = find[-1]

class RedTeamChatChannel(ChatChannel):
    """A channel for in-game chat to and from the red team."""
    def __init__(self):
        super().__init__("red_team_chat", team=Team.RED)

class BlueTeamChatChannel(ChatChannel):
    """A channel for in-game chat to and from the blue team."""
    def __init__(self):
        super().__init__("blue_team_chat", team=Team.BLUE)

class FreeChatChannel(ChatChannel):
    """A channel for in-game chat to and from the free team. The free team
    is the team all FFA players are in.

    """
    def __init__(self):
        super().__init__("free_chat", team=Team.FREE)

class SpectatorChatChannel(ChatChannel):
    """A channel for in-game chat to and from the spectator team."""
    def __init__(self):
        super().__init__("spectator_chat", team=Team.SPECTATOR)

class TellChannel(ChatChannel):
    """A channel for private in-game messages."""
    def __init__(self, player):
        super().__init__("tell")
        self.recipient = player

    def __repr__(self):
        return f"tell {minqlxtended.Plugin.player(self.recipient).steam_id}"

    def _targets(self):
        cid = minqlxtended.Plugin.client_id(self.recipient)
        if cid is None:
            raise ValueError(f"Invalid recipient: {self.recipient!r}.")

        return [cid]

class ConsoleChannel(AbstractChannel):
    """A channel that prints to the console."""
    def __init__(self):
        super().__init__("console")

    def reply(self, msg, limit=100, delimiter=" "):
        # limit and delimiter are accepted and ignored; the console wraps its own output.
        minqlxtended.console_print(str(msg))

class ClientCommandChannel(AbstractChannel):
    """Wraps a TellChannel, but with its own name."""
    def __init__(self, player):
        super().__init__("client_command")
        self.recipient = player
        self.tell_channel = TellChannel(player)

    def __repr__(self):
        return f"client_command {minqlxtended.Plugin.player(self.recipient).id}"

    def reply(self, msg, limit=100, delimiter=" "):
        self.tell_channel.reply(msg, limit, delimiter)


# MODULE CONSTANTS

COMMANDS = CommandInvoker()
CHAT_CHANNEL = ChatChannel()
RED_TEAM_CHAT_CHANNEL = RedTeamChatChannel()
BLUE_TEAM_CHAT_CHANNEL = BlueTeamChatChannel()
FREE_CHAT_CHANNEL = FreeChatChannel()
SPECTATOR_CHAT_CHANNEL = SpectatorChatChannel()
CONSOLE_CHANNEL = ConsoleChannel()
