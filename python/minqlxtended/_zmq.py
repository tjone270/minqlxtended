# minqlxtended - Extends Quake Live's dedicated server with extra functionality and scripting.
# Copyright (C) 2015 Mino <mino@minomino.org>

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

"""Subscribes to the ZMQ stats protocol and calls the stats event dispatcher when
we get stats from it. It polls the ZMQ socket approx. every 0.25 seconds."""

import minqlxtended
import json
import zmq

class StatsListener():
    def __init__(self):
        if not bool(int(minqlxtended.get_cvar("zmq_stats_enable"))):
            self.done = True
            return

        stats = minqlxtended.get_cvar("zmq_stats_ip")
        port = minqlxtended.get_cvar("zmq_stats_port")
        if not port:
            port = minqlxtended.get_cvar("net_port")
        self.address = "tcp://{}:{}".format("127.0.0.1" if not stats else stats, port)
        self.password = minqlxtended.get_cvar("zmq_stats_password")

        # Initialize socket, connect, and subscribe.
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.SUB)
        if self.password:
            self.socket.plain_username = b"stats"
            self.socket.plain_password = self.password.encode()
        self.socket.zap_domain = b"stats"
        self.socket.connect(self.address)
        self.socket.setsockopt_string(zmq.SUBSCRIBE, "")

        self.done = False
        self._in_progress = False

    @minqlxtended.delay(0.25)
    def keep_receiving(self):
        """Receives until self.done is set to True. Works by scheduling this
        to be called every 0.25 seconds. If we get an exception, we try
        to reconnect and continue.

        """
        try:
            if self.done:
                return
            while True: # Will throw an expcetion if no more data to get.
                stats = json.loads(self.socket.recv(zmq.NOBLOCK).decode(errors="ignore"))
                minqlxtended.EVENT_DISPATCHERS["stats"].dispatch(stats)

                if stats["TYPE"] == "MATCH_STARTED":
                    self._in_progress = True
                    minqlxtended.EVENT_DISPATCHERS["game_start"].dispatch(stats["DATA"])
                elif stats["TYPE"] == "ROUND_OVER":
                    minqlxtended.EVENT_DISPATCHERS["round_end"].dispatch(stats["DATA"])
                elif stats["TYPE"] == "MATCH_REPORT":
                    # MATCH_REPORT event goes off with a map change and map_restart,
                    # but we really only want it for when the game actually ends.
                    # We use a variable instead of Game().state because by the
                    # time we get the event, the game is probably gone.
                    if self._in_progress:
                        minqlxtended.EVENT_DISPATCHERS["game_end"].dispatch(stats["DATA"])
                    self._in_progress = False
                elif stats["TYPE"] == "PLAYER_DEATH":
                    # Dead player.
                    sid = int(stats["DATA"]["VICTIM"]["STEAM_ID"])
                    if sid:
                        player = minqlxtended.Plugin.player(sid)
                    else: # It's a bot. Forced to use name as an identifier.
                        player = minqlxtended.Plugin.player(stats["DATA"]["VICTIM"]["NAME"])

                    # Killer player.
                    if not stats["DATA"]["KILLER"]:
                        player_killer = None
                    else:
                        sid_killer = int(stats["DATA"]["KILLER"]["STEAM_ID"])
                        if sid_killer:
                            player_killer = minqlxtended.Plugin.player(sid_killer)
                        else: # It's a bot. Forced to use name as an identifier.
                            player_killer = minqlxtended.Plugin.player(stats["DATA"]["KILLER"]["NAME"])

                    minqlxtended.EVENT_DISPATCHERS["death"].dispatch(player, player_killer, stats["DATA"])
                    if player_killer:
                        minqlxtended.EVENT_DISPATCHERS["kill"].dispatch(player, player_killer, stats["DATA"])
                elif stats["TYPE"] == "PLAYER_SWITCHTEAM":
                    # No idea why they named it "KILLER" here, but whatever.
                    sid = int(stats["DATA"]["KILLER"]["STEAM_ID"])
                    if sid:
                        player = minqlxtended.Plugin.player(sid)
                    else: # It's a bot. Forced to use name as an identifier.
                        player = minqlxtended.Plugin.player(stats["DATA"]["KILLER"]["NAME"])
                    old_team = stats["DATA"]["KILLER"]["OLD_TEAM"].lower()
                    new_team = stats["DATA"]["KILLER"]["TEAM"].lower()
                    if old_team != new_team:
                        res = minqlxtended.EVENT_DISPATCHERS["team_switch"].dispatch(player, old_team, new_team)
                        if res is False and player is not None:
                            player.put(old_team)

        except zmq.error.Again:
            pass
        except Exception:
            minqlxtended.log_exception()
            # Reconnect, just in case. GC will clean up for us. Preserve match
            # state and never let a failed reconnect escape and kill the poll.
            in_progress = self._in_progress
            try:
                self.__init__()
                self._in_progress = in_progress
            except Exception:
                minqlxtended.log_exception()
        finally:
            # Always reschedule (unless stopped) so a transient error can't
            # permanently stop the stats listener.
            if not self.done:
                self.keep_receiving()

    def stop(self):
        self.done = True
