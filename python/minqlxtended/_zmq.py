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

"""Subscribes to the ZMQ stats protocol and raises the raw ``stats`` event for each
message. Receiving and parsing happen on their own thread; the handler is called on the
main thread, so it is free to touch engine state.

``zmq_stats_enable 1`` is only needed for the raw feed: game_start, game_end, round_end,
team_switch, kill and death all come out of the game module itself."""

from __future__ import annotations

import minqlxtended
import threading
import atexit
import json
import time
import zmq

from zmq.utils.monitor import recv_monitor_message

__all__ = ("StatsListener",)

# connect() only rejects an address libzmq cannot parse, so a failure repeats identically.
_RECONNECT_DELAY = 0.25
_RECONNECT_DELAY_MAX = 30.0

# By name, since the handshake events need libzmq 4.3.
_HANDSHAKE_FAILED = frozenset(
    value for value in (
        getattr(zmq, name, None) for name in (
            "EVENT_HANDSHAKE_FAILED_AUTH",
            "EVENT_HANDSHAKE_FAILED_PROTOCOL",
            "EVENT_HANDSHAKE_FAILED_NO_DETAIL",
        )
    ) if value is not None)

_MONITOR_EVENTS = zmq.EVENT_CONNECTED | zmq.EVENT_DISCONNECTED
for _flag in _HANDSHAKE_FAILED:
    _MONITOR_EVENTS |= _flag


class StatsListener(threading.Thread):
    def __init__(self) -> None:
        super().__init__(name="minqlxtended-stats", daemon=True)
        # stop() gets called from other threads, so an Event rather than a plain flag.
        self._done = threading.Event()
        # Read from other threads through `connected`, set only from the listener's own.
        self._connected = threading.Event()
        self._auth_reported = False


        # Set whether or not ZMQ is enabled, so reading listener.address on a disabled
        # listener doesn't raise AttributeError.
        self.address = None
        self.password = None

        if not bool(int(minqlxtended.require_cvar("zmq_stats_enable"))):
            self._done.set()
            return

        stats = minqlxtended.get_cvar("zmq_stats_ip")
        port = minqlxtended.get_cvar("zmq_stats_port")
        if not port:
            port = minqlxtended.get_cvar("net_port")
        self.address = f"tcp://{stats if stats else '127.0.0.1'}:{port}"
        self.password = minqlxtended.get_cvar("zmq_stats_password")

    @property
    def done(self) -> bool:
        """Whether the listener is finished: stopped, never started, or its thread died."""
        return self._done.is_set()

    @property
    def connected(self) -> bool:
        """Whether the stats socket currently has a peer. libzmq reconnects underneath, so a
        publisher that went away leaves the thread alive and ``done`` clear."""
        return self._connected.is_set()

    def start(self) -> None:
        if self.done:
            return
        super().start()
        # Stop and join before interpreter teardown; a daemon thread caught in
        # finalisation hangs.
        atexit.register(self.stop)

    def run(self) -> None:
        # The context and socket live on this thread only; pyzmq sockets aren't
        # thread-safe.
        context = zmq.Context()
        socket = monitor = None
        delay = _RECONNECT_DELAY
        reported_failure = False
        try:
            while not self.done:
                if socket is None:
                    try:
                        socket, monitor = self._connect(context)
                    except Exception:
                        if reported_failure:
                            minqlxtended.get_logger().debug(
                                "The stats listener still cannot use %s.", self.address)
                        else:
                            reported_failure = True
                            minqlxtended.log_exception()
                        time.sleep(delay)
                        delay = min(delay * 2, _RECONNECT_DELAY_MAX)
                        continue
                    delay = _RECONNECT_DELAY
                    reported_failure = False

                self._drain_monitor(monitor)

                try:
                    data = socket.recv()  # Wakes every 250ms (RCVTIMEO) to check self.done.
                except zmq.error.Again:
                    continue
                except Exception:
                    minqlxtended.log_exception()
                    # Reconnect, just in case.
                    self._teardown(socket, monitor)
                    socket = monitor = None
                    continue

                try:
                    stats = json.loads(data.decode(errors="ignore"))
                except Exception:
                    minqlxtended.log_exception()
                    continue

                # Handlers read and write engine state, so dispatch on the main thread.
                self._dispatch(stats)
        except Exception:
            minqlxtended.log_exception()
        finally:
            self._teardown(socket, monitor)
            context.term()
            # The thread is gone either way, so `done` has to say so.
            if not self.done:
                minqlxtended.get_logger().error(
                    "The stats listener stopped. No further stats events will be raised.")
            self._done.set()

    def _drain_monitor(self, monitor):
        """Whatever the monitor has to say, without blocking. Each transition is reported
        once; libzmq retries on its own, and that is a tick per backoff interval."""
        if monitor is None:
            return

        logger = minqlxtended.get_logger()
        while True:
            try:
                event = recv_monitor_message(monitor, flags=zmq.NOBLOCK)["event"]
            except zmq.error.Again:
                return
            except Exception:
                minqlxtended.log_exception()
                return

            if event == zmq.EVENT_CONNECTED:
                if not self._connected.is_set():
                    self._connected.set()
                    logger.info("The stats feed at %s is connected.", self.address)
            elif event == zmq.EVENT_DISCONNECTED:
                if self._connected.is_set():
                    self._connected.clear()
                    logger.warning(
                        "The stats feed at %s went away. No stats events will be raised "
                        "until it is back.", self.address)
            elif event in _HANDSHAKE_FAILED:
                self._connected.clear()
                if not self._auth_reported:
                    self._auth_reported = True
                    logger.error("The stats feed at %s refused us. Check zmq_stats_password "
                                 "against that server's.", self.address)

    def _teardown(self, socket, monitor):
        """Close a socket and its monitor. disable_monitor first: an inproc PAIR socket left
        attached keeps context.term() waiting."""
        self._connected.clear()
        for close in (lambda: socket.disable_monitor(),
                      lambda: monitor.close(linger=0),
                      lambda: socket.close(linger=0)):
            try:
                close()
            except Exception:
                pass

    def _connect(self, context):
        socket = context.socket(zmq.SUB)
        monitor = None
        try:
            if self.password:
                socket.plain_username = b"stats"
                socket.plain_password = self.password.encode()
            socket.zap_domain = b"stats"
            socket.setsockopt(zmq.RCVTIMEO, 250)
            # Before connect, or the first connection's events are missed.
            monitor = socket.get_monitor_socket(_MONITOR_EVENTS)
            socket.connect(self.address)
            socket.setsockopt_string(zmq.SUBSCRIBE, "")
        except Exception:
            # Otherwise both live until the traceback's reference cycle is collected.
            self._teardown(socket, monitor)
            raise
        return socket, monitor

    @minqlxtended.next_frame
    def _dispatch(self, stats):
        minqlxtended.EVENT_DISPATCHERS["stats"].dispatch(stats)

    def stop(self) -> None:
        self._done.set()
        if self.is_alive() and threading.current_thread() is not self:
            self.join(timeout=1)
