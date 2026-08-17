#!/bin/bash
cd "$(dirname "$0")"
export LD_PRELOAD=$LD_PRELOAD:./minqlxtended.x64.so
# zmq_stats_enable is no longer needed for the built-in events, which come out of the game
# module. It stays on for plugins that hook the raw stats feed; drop it if none of yours do.
LD_LIBRARY_PATH="./linux64:$LD_LIBRARY_PATH" exec ./qzeroded.x64 +set zmq_stats_enable 1 "$@"
