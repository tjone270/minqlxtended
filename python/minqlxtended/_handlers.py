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
import collections
import contextlib
import sched
import threading
from typing import Any, Callable

from ._enums import MeansOfDeath, Objective, SayMode, Team, Weapon

# Cache maintenance, imported by name. Not part of the public surface, and this module is
# their only caller.
from ._configstring import invalidate_configstrings, note_configstring

# The team chat channels. The map below is a module constant so it isn't rebuilt on every
# team message.
from ._commands import (BLUE_TEAM_CHAT_CHANNEL, FREE_CHAT_CHANNEL, RED_TEAM_CHAT_CHANNEL,
                        SPECTATOR_CHAT_CHANNEL)

# The handle_* functions below are the C ABI, so they are not exported.
# register_handlers() reaches them as module globals, and the two gated ones are resolved
# off this module by name.
__all__ = (
    "NEXT_FRAME_TASKS_MAX",
    "frame_tasks",
    "next_frame_tasks",
    "redirect_print",
    "register_handlers",
)

# LOW-LEVEL HANDLERS. The C code calls all of these; nothing in Python does.

def handle_rcon(cmd):
    """Console commands that are to be processed as regular pyminqlxtended
    commands as if the owner executes it. This allows the owner to
    interact with the Python part of minqlxtended without having to connect.

    """
    try:
        minqlxtended.COMMANDS.handle_input(minqlxtended.RconDummyPlayer(), cmd, minqlxtended.CONSOLE_CHANNEL)
    except:
        minqlxtended.log_exception()
        return True

def handle_console_command(cmd):
    """Console commands registered from Python via ``add_console_command()``, and the
    built-in ``pycmd``, are routed here and run as the owner, mirroring :func:`handle_rcon`.

    """
    try:
        minqlxtended.COMMANDS.handle_input(minqlxtended.RconDummyPlayer(), cmd, minqlxtended.CONSOLE_CHANNEL)
    except:
        minqlxtended.log_exception()
        return True

def handle_client_command(client_id, cmd):
    """Client commands are commands such as "say", "say_team", "scores",
    "disconnect" and so on.

    chat, vote_called, team_switch_attempt and userinfo all come from engine hooks, so
    nothing here derives them from the command text. Casting a vote is the permanent
    exception: qagame 1069 inlines the `vote` command into ClientCommand, leaving no
    function to hook.

    :param client_id: The client identifier.
    :type client_id: int
    :param cmd: The command being ran by the client.
    :type cmd: str

    """
    try:
        player = minqlxtended.Player(client_id)
        retval = minqlxtended.EVENT_DISPATCHERS["client_command"].dispatch(player, cmd)
        if retval is False:
            return False
        elif isinstance(retval, str):
            # Allow plugins to modify the command before passing it on.
            cmd = retval

        # The engine reads the same string through Cmd_TokenizeString, which strips the
        # quotes around a whole token, so `vote "yes"` and `"vote" yes` both count. A
        # half-quoted `"vote yes"` tokenises to one argument and is not the vote command.
        parts = cmd.split(None, 1)
        if (len(parts) == 2 and parts[0].casefold() in ("vote", '"vote"')
                and minqlxtended.Plugin.is_vote_active()
                and not minqlxtended.Plugin.is_intermission()):
            # qagame's Cmd_Vote_f (0x10044450) tests the first character against 'y', 'Y' and
            # '1' and counts everything else as a no, so `vote 3` and `vote maybe` are both
            # noes. Intermission is excluded above: there the same command picks one of three
            # map panels, and neither reading it as a yes/no nor letting a handler cancel it
            # is right.
            yes = parts[1].lstrip('"')[:1] in ("y", "Y", "1")
            if minqlxtended.EVENT_DISPATCHERS["vote"].dispatch(player, yes) is False:
                return False

        return cmd
    except:
        minqlxtended.log_exception()
        return True

def handle_server_command(client_id, cmd):
    try:
        # Runs for every server command sent, so the Player construction (a C player_info
        # call) is skipped entirely while nothing hooks the event.
        dispatcher = minqlxtended.EVENT_DISPATCHERS["server_command"]
        if dispatcher._handler_chain:
            try:
                player = minqlxtended.Player(client_id) if client_id >= 0 else None
            except minqlxtended.NonexistentPlayerError:
                return True

            retval = dispatcher.dispatch(player, cmd)
            if retval is False:
                return False
            elif isinstance(retval, str):
                cmd = retval

        return cmd
    except:
        minqlxtended.log_exception()
        return True

# Work to run on the main thread just before a frame, queued by @minqlxtended.next_frame.
frame_tasks = sched.scheduler()

# Bounded, since handle_frame is the only consumer and does not run while
# skipFrameDispatcher is set around SV_SpawnServer, while the ZMQ thread keeps queueing a
# task per stats message. The oldest are the most stale by the time frames resume.
NEXT_FRAME_TASKS_MAX = 8192
next_frame_tasks: collections.deque[tuple[Callable[..., Any], tuple, dict]] = (
    collections.deque(maxlen=NEXT_FRAME_TASKS_MAX))
_next_frame_dropped = 0


def _queue_next_frame(func, args, kwargs):
    """Queue one task for the next frame, counting what a full deque throws away.

    The bounded deque discards from the far end, and handle_frame reports the count. The
    counter takes no lock, so a concurrent producer can lose an increment and undercount.

    """
    global _next_frame_dropped
    if len(next_frame_tasks) == next_frame_tasks.maxlen:
        _next_frame_dropped += 1
    next_frame_tasks.append((func, args, kwargs))

def _run_frame_task(func, args, kwargs):
    # A task queued by a plugin carries the instance as args[0], the `self` of a bound
    # method routed through @next_frame or @delay. Once unload_plugin has marked it the work
    # is dropped, since a worker it left behind can queue tasks long after the unload.
    if args and isinstance(args[0], minqlxtended.Plugin) and args[0]._unloaded:
        minqlxtended.get_logger().debug(
            "Dropping task %s queued by unloaded plugin '%s'.",
            getattr(func, "__name__", func), args[0].name)
        return
    func(*args, **kwargs)


def handle_frame():
    """This will be called every frame. To allow threads to call stuff from the
    main thread, tasks can be scheduled using the :func:`minqlxtended.next_frame` decorator
    and have it be executed here.

    """

    # Tasks queued since the last frame, run straight from the deque. Only this thread
    # pops and producers only append, so counting up front is safe, and stopping at that
    # count defers a task that queues another to the next frame.
    global _next_frame_dropped
    if _next_frame_dropped:
        dropped, _next_frame_dropped = _next_frame_dropped, 0
        minqlxtended.get_logger().warning(
            "Dropped %d next_frame task(s): the queue hit its %d-task limit, which "
            "usually means frames stopped dispatching while a thread kept queueing.",
            dropped, NEXT_FRAME_TASKS_MAX)

    for _ in range(len(next_frame_tasks)):
        func, args, kwargs = next_frame_tasks.popleft()
        try:
            _run_frame_task(func, args, kwargs)
        except:
            minqlxtended.log_exception()

    while True:
        # An exception in one scheduled task is logged; the rest still run.
        try:
            frame_tasks.run(blocking=False)
            break
        except:
            minqlxtended.log_exception()
            continue
    try:
        minqlxtended.EVENT_DISPATCHERS["frame"].dispatch()
    except:
        minqlxtended.log_exception()
        return True


_first_game = True

# A match can end two ways: the engine queues intermission, or warmupTime falls back below
# zero on an abandoned match. Both paths are in game_events.c, and this latches so only one
# game_end comes out.
_game_ended = False

def handle_spawn_server():
    """Called from My_SV_SpawnServer, immediately before the engine wipes the
    configstring table.

    The new map's writes reach handle_set_configstring long before handle_new_game runs,
    so the cache has to be dropped here.

    """
    try:
        invalidate_configstrings()
    except:
        minqlxtended.log_exception()
        return True

def handle_new_game(is_restart):
    try:
        if is_restart:
            # Only on map_restart, where G_InitGame dispatches new_game without going
            # through SV_SpawnServer so nothing else has dropped the cache. On a real map
            # change SV_SpawnServer has already re-warmed it through the set_configstring
            # hook, and clearing again would throw away a full table.
            invalidate_configstrings()

        # A new game means a new match, so the last one's ending doesn't apply.
        # GameEvents_Reset clears the C side's counterparts at the same two points.
        global _game_ended
        _game_ended = False

        # This is called early in the launch process, so it's a good place to initialize
        # minqlxtended stuff that needs QLDS to be initialized.
        global _first_game
        if _first_game:
            # Clear the flag first so a failure inside late_init isn't retried on every
            # subsequent new_game (which would re-run non-idempotent setup).
            _first_game = False
            minqlxtended.late_init()

        minqlxtended.set_map_subtitles()

        # Before either dispatch, so map and new_game handlers both read fresh values. A
        # latched cvar takes its new value during the map load without a Cvar_Set2 write.
        minqlxtended._core._refresh_settings()

        if not is_restart:
            minqlxtended.EVENT_DISPATCHERS["map"].dispatch(
                minqlxtended.get_cvar("mapname"),
                minqlxtended.get_cvar("g_factory"))

        minqlxtended.EVENT_DISPATCHERS["new_game"].dispatch()
    except:
        # Wrapped whole, like every other handler here: NewGameDispatcher reports a NULL
        # return as a one-line DebugError, so an escaping exception loses its traceback.
        minqlxtended.log_exception()
        return True

def handle_set_configstring(index, value):
    """Called whenever the server tries to set a configstring. Can return
    False to stop the event.

    """
    try:
        res = minqlxtended.EVENT_DISPATCHERS["set_configstring"].dispatch(index, value)
        if res is False:
            return False
        replaced = isinstance(res, str)
        if replaced:
            value = res

        note_configstring(index, value, may_be_truncated=replaced)
        return res
    except:
        minqlxtended.log_exception()
        return True

# game_countdown, game_start, round_countdown, round_start and the abandoned-match game_end
# come off level_locals_t in game_events.c, beside round_end and the vote events. Those
# fields are the source the configstrings are built from.


def handle_player_connect(client_id, is_bot):
    """This will be called whenever a player tries to connect. If the dispatcher
    returns False, the player is refused and shown "You are banned from this server."

    That string is hardcoded in ClientConnectDispatcher and can't be changed. Return a
    string instead of False to refuse with your own message.

    :param client_id: The client identifier.
    :type client_id: int
    :param is_bot: Whether or not the player is a bot.
    :type is_bot: bool

    """
    try:
        player = minqlxtended.Player(client_id)
        # is_bot comes from the engine, since Player.is_bot reads SVF_BOT off the entity
        # and the game module has not set it yet at this point.
        return minqlxtended.EVENT_DISPATCHERS["player_connect"].dispatch(player, bool(is_bot))
    except:
        minqlxtended.log_exception()
        return True

def handle_player_loaded(client_id):
    """This will be called whenever a player has connected and finished loading,
    meaning it'll go off a bit later than the usual "X connected" messages.
    This will not trigger on bots.

    :param client_id: The client identifier.
    :type client_id: int

    """
    try:
        player = minqlxtended.Player(client_id)
        return minqlxtended.EVENT_DISPATCHERS["player_loaded"].dispatch(player)
    except:
        minqlxtended.log_exception()
        return True

def handle_player_disconnect(client_id, reason):
    """This will be called whenever a player disconnects.

    :param client_id: The client identifier.
    :type client_id: int

    """
    try:
        player = minqlxtended.Player(client_id)
        return minqlxtended.EVENT_DISPATCHERS["player_disconnect"].dispatch(player, reason)
    except:
        minqlxtended.log_exception()
        return True

def handle_player_spawn(client_id):
    """Called when a player spawns. Note that a spectator going in free spectate mode
    makes the client spawn, so you'll want to check for that if you only want "actual"
    spawns.

    """
    try:
        player = minqlxtended.Player(client_id)
        return minqlxtended.EVENT_DISPATCHERS["player_spawn"].dispatch(player)
    except:
        minqlxtended.log_exception()
        return True

def handle_kamikaze_use(client_id):
    """This will be called whenever player uses kamikaze item.

    :param client_id: The client identifier.
    :type client_id: int

    """
    try:
        player = minqlxtended.Player(client_id)
        return minqlxtended.EVENT_DISPATCHERS["kamikaze_use"].dispatch(player)
    except:
        minqlxtended.log_exception()
        return True

def handle_kamikaze_explode(client_id, is_used_on_demand):
    """This will be called whenever kamikaze explodes.

    :param client_id: The client identifier.
    :type client_id: int
    :param is_used_on_demand: Non-zero if kamikaze is used on demand.
    :type is_used_on_demand: int


    """
    try:
        player = minqlxtended.Player(client_id)
        return minqlxtended.EVENT_DISPATCHERS["kamikaze_explode"].dispatch(player, True if is_used_on_demand else False)
    except:
        minqlxtended.log_exception()
        return True

def handle_player_death(victim_id, killer_id, mod):
    """Called from the player_die hook whenever a player dies, for any reason.

    Always raises ``death``. ``kill`` follows only when another player was responsible,
    so a suicide or a drop into lava produces a single event.

    :param victim_id: The client that died.
    :type victim_id: int
    :param killer_id: The client responsible, or -1 when nobody was.
    :type killer_id: int
    :param mod: Raw means of death; see :meth:`minqlxtended.MeansOfDeath.from_index`.
    :type mod: int

    """
    try:
        victim = minqlxtended.Player(victim_id)
        killer = minqlxtended.Player(killer_id) if killer_id >= 0 else None
        mod_name = _named(MeansOfDeath.from_index, mod, MeansOfDeath.UNKNOWN)

        minqlxtended.EVENT_DISPATCHERS["death"].dispatch(victim, killer, mod_name)

        if killer is not None and killer_id != victim_id:
            minqlxtended.EVENT_DISPATCHERS["kill"].dispatch(victim, killer, mod_name)
    except:
        minqlxtended.log_exception()
        return True

def handle_vote_called(client_id, vote, args):
    """Called from the Cmd_CallVote_f hook when a player calls a vote.

    Fires for every attempt, including votes the engine goes on to reject as invalid or
    disallowed. Hook ``vote_started`` if you only want the ones that run. Returning False
    stops the vote from being called at all.

    :param client_id: The client calling the vote.
    :type client_id: int
    :param vote: The vote command, e.g. "map" or "kick".
    :type vote: str
    :param args: The rest of the command, or "" when the vote takes no arguments.
    :type args: str

    """
    try:
        player = minqlxtended.Player(client_id)
        return minqlxtended.EVENT_DISPATCHERS["vote_called"].dispatch(player, vote, args)
    except:
        minqlxtended.log_exception()
        return True

def _split_vote_string(vote_string):
    """Split a raw level->voteString into its command and the rest."""
    parts = vote_string.split(None, 1)
    if not parts:
        return "", ""
    return parts[0], parts[1] if len(parts) > 1 else ""

def handle_vote_started(caller_id, vote_string):
    """Called from the frame poll when level->voteTime goes non-zero.

    :param caller_id: The client that called it, or -1 when the engine or a plugin did.
    :type caller_id: int
    :param vote_string: The raw vote string, e.g. "map campgrounds ca".
    :type vote_string: str

    """
    try:
        caller = minqlxtended.Player(caller_id) if caller_id >= 0 else None
        vote, args = _split_vote_string(vote_string)

        return minqlxtended.EVENT_DISPATCHERS["vote_started"].dispatch(caller, vote, args)
    except:
        minqlxtended.log_exception()
        return True

def handle_vote_ended(passed, vote_string, yes, no):
    """Called from the frame poll when level->voteTime falls back to zero.

    The tallies are the ones cached while the vote was still live: the engine clears
    CS_VOTE_YES and CS_VOTE_NO as it resolves, so reading them here would report 0-0.

    :param passed: Whether the vote passed.
    :type passed: bool
    :param vote_string: The raw vote string the vote ran with.
    :type vote_string: str
    :param yes: Votes in favour.
    :type yes: int
    :param no: Votes against.
    :type no: int

    """
    try:
        vote, args = _split_vote_string(vote_string)

        return minqlxtended.EVENT_DISPATCHERS["vote_ended"].dispatch(
            (yes, no), vote, args, bool(passed))
    except:
        minqlxtended.log_exception()
        return True

# SetTeam's argument to the team it actually results in. The engine accepts the full word
# and the single letter, and anything it does not recognise means "pick one for me".
# follow1/follow2 map to spectator: SetTeam seats them there and then arranges the follow.
_TEAM_TARGETS = {
    "f": Team.FREE, "free": Team.FREE,
    "r": Team.RED, "red": Team.RED,
    "b": Team.BLUE, "blue": Team.BLUE,
    "s": Team.SPECTATOR, "spectator": Team.SPECTATOR,
    "follow1": Team.SPECTATOR, "follow2": Team.SPECTATOR,
}

# Which channel a team message goes to. Spectators are the default instead of an entry,
# which is also what the engine does with anyone not on a playing team.
_TEAM_CHAT_CHANNELS = {
    Team.FREE: FREE_CHAT_CHANNEL,
    Team.RED: RED_TEAM_CHAT_CHANNEL,
    Team.BLUE: BLUE_TEAM_CHAT_CHANNEL,
}


def _named(describe, raw, default):
    """``describe(raw)``, or *default* with a warning if it names nothing.

    Engine numbers reach these handlers unvalidated. One this build doesn't know is
    replaced rather than taking the event down with it.

    """
    try:
        return describe(raw)
    except ValueError:
        minqlxtended.get_logger().warning(
            "The engine reported %r, which names nothing this build knows; "
            "the event will carry %r instead.", raw, default)
        return default

def handle_chat(client_id, target_id, mode, msg):
    """Called from the G_Say hook when a player says something.

    The message arrives exactly as the engine has it, quotes and colour codes intact, and
    returning False stops it being said at all.

    Private messages raise this too, with the person who was told passed as *recipient*.
    The channel for a tell addresses the *speaker*: the chat dispatcher runs `!commands`
    through this same channel, and a command must answer whoever typed it.

    :param client_id: The player speaking.
    :type client_id: int
    :param target_id: The recipient for a tell, or -1.
    :type target_id: int
    :param mode: The raw G_Say mode; see :class:`minqlxtended.SayMode`.
    :type mode: int
    :param msg: The message.
    :type msg: str

    """
    try:
        player = minqlxtended.Player(client_id)
        recipient = None

        if mode == SayMode.TELL:
            # Cmd_Tell_f cannot produce a tell without a target, so this is the engine doing
            # something unmodelled. Falling through would report it as said out loud.
            if target_id < 0:
                minqlxtended.get_logger().warning(
                    "SayMode.TELL from client %d with no recipient; reporting it as a "
                    "tell with recipient None.", client_id)
            else:
                recipient = minqlxtended.Player(target_id)

            channel = minqlxtended.TellChannel(player)
        elif mode == SayMode.TEAM:
            channel = _TEAM_CHAT_CHANNELS.get(player.team, SPECTATOR_CHAT_CHANNEL)
        else:
            channel = minqlxtended.CHAT_CHANNEL

        return minqlxtended.EVENT_DISPATCHERS["chat"].dispatch(player, msg, channel,
                                                               recipient)
    except:
        minqlxtended.log_exception()
        return True

def handle_team_switch_attempt(client_id, old_team, target):
    """Called from the SetTeam hook when a player tries to join a team.

    Returning False prevents the switch outright. This fires for a player typing `team X`,
    and equally for admin puts, duel-queue promotion and follow-cycling.

    *new_team* is what was asked for, with no promise. SetTeam ignores `red` and `blue` and
    picks a team itself while level.warmupTime is not negative, and refuses outright when
    the arena or team is full, when the teams would go out of balance, or when MP_AllowJoin
    says no. `team_switch` is the event for what actually happened.

    :param client_id: The player switching.
    :type client_id: int
    :param old_team: Raw team_t they are on now.
    :type old_team: int
    :param target: SetTeam's raw argument, e.g. "red", "s", "follow1".
    :type target: str

    """
    try:
        player = minqlxtended.Player(client_id)
        old = _named(Team.from_index, old_team, Team.FREE)
        # Anything the engine does not recognise means "put me somewhere".
        new = _TEAM_TARGETS.get(target.lower(), "any")

        # Exactly SetTeam's own test, `newTeam == oldTeam && newTeam != TEAM_SPECTATOR`.
        # Spectator to spectator is not a no-op to the engine: it is how follow1, follow2
        # and `team s` cycle the follow target. That is the case `target` is carried for.
        if new == old and new != Team.SPECTATOR:
            return True

        # The raw argument goes through as well: follow1 and follow2 both land on
        # spectator, and only this tells them apart from an ordinary spectate.
        return minqlxtended.EVENT_DISPATCHERS["team_switch_attempt"].dispatch(
            player, old, new, target)
    except:
        minqlxtended.log_exception()
        return True

def handle_weapon_fired(client_id, weapon):
    """Called from the FireWeapon hook on every shot.

    **Not** registered by :func:`register_handlers`. Like ``damage``, the slot is armed by
    the dispatcher only while the event has hooks.

    :param client_id: The player firing.
    :type client_id: int
    :param weapon: Raw weapon_t value; see :class:`minqlxtended.Weapon`.
    :type weapon: int

    """
    try:
        dispatcher = minqlxtended.EVENT_DISPATCHERS["weapon_fired"]
        # As with damage: a dispatch already in flight when the last hook goes away must
        # not pay for a Player.
        if not dispatcher._handler_chain:
            return True

        # The Weapon member itself. Ask it for `.short` if that's
        # the spelling you want.
        return dispatcher.dispatch(minqlxtended.Player(client_id),
                                   _named(Weapon, weapon, None))
    except:
        minqlxtended.log_exception()
        return True


def handle_cvar_changed(name, old_value, new_value):
    """Called from the Cvar_Set2 hook for a write that changed a cvar's live value.

    Not registered by :func:`register_handlers`: like ``damage``, the slot is armed by the
    dispatcher only while the event has hooks. An off-main-thread write is queued for the
    next frame, so handlers always run on the game thread.

    """
    try:
        if threading.current_thread() is not threading.main_thread():
            _queue_next_frame(_dispatch_cvar_changed, (name, old_value, new_value), {})
            return True
        return _dispatch_cvar_changed(name, old_value, new_value)
    except:
        minqlxtended.log_exception()
        return True


def _dispatch_cvar_changed(name, old_value, new_value):
    try:
        dispatcher = minqlxtended.EVENT_DISPATCHERS["cvar_changed"]
        # A deferred dispatch can outlive the last hook; nothing left to tell.
        if not dispatcher._handler_chain:
            return True

        return dispatcher.dispatch(name, old_value, new_value)
    except:
        minqlxtended.log_exception()
        return True

def handle_userinfo(client_id, infostring):
    """Called from the SV_UpdateUserinfo_f hook when a client changes its userinfo.

    Raises ``userinfo`` with only the keys that actually changed. A handler returning a
    dict has it merged and the whole infostring handed back to the engine to re-tokenise.
    Returning False drops the change entirely.

    :param client_id: The client.
    :type client_id: int
    :param infostring: The raw infostring the client sent.
    :type infostring: str

    """
    try:
        player = minqlxtended.Player(client_id)
        new_info = minqlxtended.parse_infostring(infostring)
        old_info = player.cvars

        changed = {k: v for k, v in new_info.items()
                   if k not in old_info or old_info[k] != v}
        if not changed:
            return True

        ret = minqlxtended.EVENT_DISPATCHERS["userinfo"].dispatch(player, changed, infostring)
        if ret is False:
            return False

        if isinstance(ret, dict):
            new_info.update(ret)
            return minqlxtended.format_infostring(_infostring_safe(new_info))

        return True
    except:
        minqlxtended.log_exception()
        return True

def _infostring_safe(variables):
    """*variables* with the characters the infostring format can't carry removed.

    format_infostring raises on any of them, which would lose a handler's whole edit over a
    character it never supplied. The C layer strips the same set out of the replacement
    before re-tokenising it (src/server/hooks.c).

    """
    forbidden = str.maketrans("", "", "".join(minqlxtended._core._INFOSTRING_FORBIDDEN))
    cleaned = {}
    for key, value in variables.items():
        key, value = str(key), str(value)
        safe_key, safe_value = key.translate(forbidden), value.translate(forbidden)
        if safe_key != key or safe_value != value:
            minqlxtended.get_logger().warning(
                "Dropped characters the infostring format can't carry from userinfo key %r.", key)
        cleaned[safe_key] = safe_value

    return cleaned

def handle_objective(client_id, kind, count):
    """Called from the frame poll when a player's objective counter goes up.

    :param client_id: The client that scored it.
    :type client_id: int
    :param kind: objective_t index; see :meth:`minqlxtended.Objective.from_index`.
    :type kind: int
    :param count: The counter's new total, so a jump of two arrives as one event.
    :type count: int

    """
    try:
        player = minqlxtended.Player(client_id)
        name = _named(Objective.from_index, kind, Objective.UNKNOWN)

        return minqlxtended.EVENT_DISPATCHERS["objective"].dispatch(player, name, count)
    except:
        minqlxtended.log_exception()
        return True

def handle_player_damage(target_id, attacker_id, damage, dflags, mod):
    """Called from the G_Damage hook every time a player takes damage.

    **Not** registered by :func:`register_handlers`. The engine slot is armed by
    :class:`minqlxtended.DamageDispatcher` only while the event has hooks, since this runs
    on every point of damage dealt on the server.

    :param target_id: The client that was damaged.
    :type target_id: int
    :param attacker_id: The client responsible, or -1 when nobody was.
    :type attacker_id: int
    :param damage: How much damage was applied.
    :type damage: int
    :param dflags: Raw damage bitfield; see :class:`minqlxtended.DamageFlag`.
    :type dflags: int
    :param mod: Raw means of death; see :meth:`minqlxtended.MeansOfDeath.from_index`.
    :type mod: int

    """
    try:
        dispatcher = minqlxtended.EVENT_DISPATCHERS["damage"]
        # The slot is disarmed on the last unhook, but a dispatch already in flight on the
        # game thread can still land here. Bail before paying for the Player objects.
        if not dispatcher._handler_chain:
            return True

        target = minqlxtended.Player(target_id)
        attacker = minqlxtended.Player(attacker_id) if attacker_id >= 0 else None

        return dispatcher.dispatch(target, attacker, damage, dflags,
                                   _named(MeansOfDeath.from_index, mod, MeansOfDeath.UNKNOWN))
    except:
        minqlxtended.log_exception()
        return True

def handle_round_countdown(round_number):
    """Called when the game module moves the round state to ROUND_WARMUP.

    :param round_number: Which round is about to start.
    :type round_number: int

    """
    try:
        return minqlxtended.EVENT_DISPATCHERS["round_countdown"].dispatch(round_number)
    except:
        minqlxtended.log_exception()
        return True

def handle_round_start(round_number):
    """Called when the game module moves the round state to ROUND_BEGUN.

    :param round_number: Which round this is.
    :type round_number: int

    """
    try:
        return minqlxtended.EVENT_DISPATCHERS["round_start"].dispatch(round_number)
    except:
        minqlxtended.log_exception()
        return True

def handle_round_end(round_number, winning_team, time):
    """Called when the game module moves the round state to ROUND_OVER.

    The round number is the one ``round_start`` reported, held by game_events.c rather than
    re-read, so the two events always agree about the round they are both describing.

    :param round_number: Which round just ended.
    :type round_number: int
    :param winning_team: Raw team_t of the team that gained a point, or TEAM_FREE on a draw.
    :type winning_team: int
    :param time: How long the round lasted, in milliseconds.
    :type time: int

    """
    try:
        winner = _named(Team.from_index, winning_team, Team.FREE)
        if winner == Team.FREE:
            # Neither team scored, so nobody took the round.
            winner = None

        return minqlxtended.EVENT_DISPATCHERS["round_end"].dispatch(round_number, winner, time)
    except:
        minqlxtended.log_exception()
        return True

def handle_game_countdown():
    """Called when warmupTime goes positive: players have readied up and the match is
    counting down."""
    try:
        return minqlxtended.EVENT_DISPATCHERS["game_countdown"].dispatch()
    except:
        minqlxtended.log_exception()
        return True

def handle_game_start():
    """Called when warmupTime reaches zero, which is the match starting."""
    try:
        return minqlxtended.EVENT_DISPATCHERS["game_start"].dispatch()
    except:
        minqlxtended.log_exception()
        return True

def handle_game_end(aborted):
    """Called when a match ends, from whichever of the two paths gets there first: the
    game module queueing intermission, or warmupTime falling back below zero because the
    match was abandoned. Only the first one through raises the event.

    :param aborted: True when the match ended without a result.
    :type aborted: bool

    """
    global _game_ended
    try:
        if _game_ended:
            return True
        _game_ended = True

        return minqlxtended.EVENT_DISPATCHERS["game_end"].dispatch(bool(aborted))
    except:
        minqlxtended.log_exception()
        return True

def handle_team_switch(client_id, old_team, new_team):
    """Called when a player's team actually changes, whatever caused it: joining, an admin
    put, autobalance, or being dropped into spectator.

    Returning False puts the player back where they were. Note that unlike
    ``team_switch_attempt`` this happens after the fact, so there is a brief moment
    where the player really is on the new team.

    :param client_id: The client that moved.
    :type client_id: int
    :param old_team: Raw team_t they were on.
    :type old_team: int
    :param new_team: Raw team_t they are on now.
    :type new_team: int

    """
    try:
        player = minqlxtended.Player(client_id)
        old_name = _named(Team.from_index, old_team, Team.FREE)
        new_name = _named(Team.from_index, new_team, Team.FREE)

        res = minqlxtended.EVENT_DISPATCHERS["team_switch"].dispatch(player, old_name, new_name)
        if res is False:
            player.put(old_name)
            return False
    except:
        minqlxtended.log_exception()
        return True

def handle_item_pickup(client_id, item_name):
    """Called when a player successfully picks an item up.

    :param client_id: The client that picked it up.
    :type client_id: int
    :param item_name: The item entity's classname, e.g. "item_armor_body".
    :type item_name: str

    """
    try:
        player = minqlxtended.Player(client_id)
        return minqlxtended.EVENT_DISPATCHERS["item_pickup"].dispatch(player, item_name)
    except:
        minqlxtended.log_exception()
        return True

def handle_demo_finished(client_id, path, size, discarded, failed):
    """Called once the demo writer has closed a segment.

    :param client_id: The client slot the demo was recorded from.
    :type client_id: int
    :param path: Where the demo ended up on disk.
    :type path: str
    :param size: Size of the demo in bytes.
    :type size: int
    :param discarded: True if the segment held only a gamestate and was deleted again,
                      in which case nothing exists at ``path``.
    :type discarded: bool
    :param failed: True if the writer hit an error, leaving an incomplete ``.part``.
    :type failed: bool

    """
    try:
        return minqlxtended.EVENT_DISPATCHERS["demo_finished"].dispatch(client_id, path, size, discarded, failed)
    except:
        minqlxtended.log_exception()
        return True

def handle_console_print(text):
    """Called whenever the server prints something to the console and when rcon is used."""
    try:
        if not text:
            return

        # Log console output. Removes the need to have stdout logs in addition to minqlxtended.log.
        minqlxtended.get_logger().debug(text.rstrip("\n"))

        res = minqlxtended.EVENT_DISPATCHERS["console_print"].dispatch(text)
        if res is False:
            return False

        # The redirect channel gets what the handlers settled on, same as the console.
        out = res if isinstance(res, str) else text

        if _redirect_stack:
            # Innermost only: an inner block takes the output for its duration and the outer
            # one resumes after it.
            _redirect_stack[-1][1].append(out)

        return out
    except:
        minqlxtended.log_exception()
        return True

#: The open blocks, innermost last, each holding the channel it replies on and what has
#: printed since it opened.
_redirect_stack: list[tuple[Any, list[str]]] = []

@contextlib.contextmanager
def redirect_print(channel):
    """Redirects print output to a channel. Useful for commands that execute console commands
    and want to redirect the output to the channel instead of letting it go to the console.

    .. code-block:: python
        def cmd_echo(self, player, msg, channel):
            with minqlxtended.redirect_print(channel):
                minqlxtended.console_command(f"echo {' '.join(msg)}")

    Everything printed while the block is open is captured, whatever printed it, and the
    channel gets it in one reply when the block closes. A console command that reloads the
    game module runs a frame or more after the `with` has closed and escapes capture.

    Nesting works: an inner block takes the output for its own duration and the outer one
    resumes afterwards.

    Process-global, like the thing being redirected. Two of these open on different threads
    at once interleave into one buffer; this is a game-thread API.

    """
    if not isinstance(channel, minqlxtended.AbstractChannel):
        raise ValueError("The redirection channel must be an instance of minqlxtended.AbstractChannel.")

    entry = (channel, [])
    _redirect_stack.append(entry)
    try:
        yield
    finally:
        # By identity: two blocks on the same channel that have captured the same thing so
        # far compare equal, and list.remove() would take the outer one.
        for i, open_block in enumerate(_redirect_stack):
            if open_block is entry:
                del _redirect_stack[i]
                break

        if entry[1]:
            channel.reply("".join(entry[1]))

def register_handlers():
    minqlxtended.register_handler("rcon", handle_rcon)
    minqlxtended.register_handler("custom_command", handle_console_command)
    minqlxtended.register_handler("client_command", handle_client_command)
    minqlxtended.register_handler("server_command", handle_server_command)
    minqlxtended.register_handler("frame", handle_frame)
    minqlxtended.register_handler("new_game", handle_new_game)
    minqlxtended.register_handler("spawn_server", handle_spawn_server)
    minqlxtended.register_handler("set_configstring", handle_set_configstring)
    minqlxtended.register_handler("player_connect", handle_player_connect)
    minqlxtended.register_handler("player_loaded", handle_player_loaded)
    minqlxtended.register_handler("player_disconnect", handle_player_disconnect)
    minqlxtended.register_handler("player_spawn", handle_player_spawn)
    minqlxtended.register_handler("console_print", handle_console_print)

    minqlxtended.register_handler("kamikaze_use", handle_kamikaze_use)
    minqlxtended.register_handler("kamikaze_explode", handle_kamikaze_explode)

    minqlxtended.register_handler("demo_finished", handle_demo_finished)

    minqlxtended.register_handler("player_death", handle_player_death)
    minqlxtended.register_handler("round_countdown", handle_round_countdown)
    minqlxtended.register_handler("round_start", handle_round_start)
    minqlxtended.register_handler("round_end", handle_round_end)
    minqlxtended.register_handler("game_countdown", handle_game_countdown)
    minqlxtended.register_handler("game_start", handle_game_start)
    minqlxtended.register_handler("game_end", handle_game_end)
    minqlxtended.register_handler("team_switch", handle_team_switch)
    minqlxtended.register_handler("item_pickup", handle_item_pickup)
    minqlxtended.register_handler("vote_called", handle_vote_called)
    minqlxtended.register_handler("vote_started", handle_vote_started)
    minqlxtended.register_handler("vote_ended", handle_vote_ended)
    minqlxtended.register_handler("objective", handle_objective)
    minqlxtended.register_handler("chat", handle_chat)
    minqlxtended.register_handler("team_switch_attempt", handle_team_switch_attempt)
    minqlxtended.register_handler("userinfo", handle_userinfo)

    # `damage` and `weapon_fired` are missing from this list because they're gated; their
    # dispatchers arm the slot only while the event has hooks. See
    # EventDispatcher.gated_handler.
