minqlxtended
======
minqlxtended is a further extension to [MinoMino's](https://github.com/MinoMino) [minqlx](https://github.com/MinoMino/minqlx) modification to the Quake Live Dedicated Server.

minqlxtended is tested on the latest LTS revision of Ubuntu Server, and needs **Python 3.12 or later**. Any current Ubuntu LTS ships a new enough one.

minqlxtended powers [The Purgery](https://thepurgery.com).

Not backwards compatible
========================
**Plugins written for minqlx, or for minqlxtended before v1.0.0, won't load on this version without alteration.**

- The six events that used to come from the ZMQ stats feed have new signatures. They now come out of the game module, so `zmq_stats_enable 1` is no longer mandatory.
- Every constant family is now an enum. `WP_RAILGUN` is `Weapon.RAILGUN`, `RET_STOP_ALL` is `Return.STOP_ALL`, etc.
- Twenty-two engine functions that each wrote a single field are replaced with direct property access. In practice you can still use the object to do mostly everything (e.g. Player.xyz)
- A handler whose signature doesn't fit its event is refused at registration time.

See [Upgrading](https://github.com/tjone270/minqlxtended/wiki/Upgrading) for the full list and what to change each one to.

Installation
============
- Ensure system is completely up-to-date:
```
sudo apt update
sudo apt upgrade -y
```

- Install Python 3:
```
sudo apt-get -y install python3 python3-dev python3-pip
```

- Install Redis, Git and build-essential:
```
sudo apt-get -y install redis-server git build-essential
```

- Clone this repository and compile minqlxtended
```
git clone https://github.com/tjone270/minqlxtended.git
cd minqlxtended
make
```

minqlxtended links `libpython` directly, so it has to be built against the same interpreter the server will run. `make` uses whatever `python3` resolves to. To target a different one:
```
make PYTHON=python3.14
```
Newer interpreters are worth trying, since much of minqlxtended's per-frame work is Python and the interpreter's own speed impacts the frame budget.

- Copy everything from `minqlxtended/bin` into your Quake Live Dedicated Server's installation folder (not the baseq3 folder, but it's parent.):

- Clone the plugins repository and get/build Python dependencies. Assuming you're in the directory with all the server files (where you extracted the above files) do:
```
git clone https://github.com/tjone270/minqlxtended-plugins.git
python3 -m pip install -r minqlxtended-plugins/requirements.txt
```

**IMPORTANT**: Don't be running the above using `sudo` or within the system context. Follow best practices and install these under the service account user you'll be executing `qzeroded` with, these packages will be local to that user and won't interfere with Ubuntu's built-in Python packages.

- Redis should work right off the bat, though switching it to a UNIX socket is faster. See Configuration below.

- You're almost there. Now simply edit the scripts you use to launch the server, but make it point to `run_server_x64_minqlxtended.sh` instead of `run_server_x64.sh`.

Configuration
=============
minqlxtended is configured with cvars, like `qzeroded` itself, so `server.cfg` or `+set` on the command line both work. Everything has a default except `qlx_owner`, which must hold your SteamID64. The listed owner operates outside the permission system and can execute any command, raw Python included.

The [Configuration](https://github.com/tjone270/minqlxtended/wiki/Configuration) wiki page lists every cvar: the core and database settings, logging, server-side demo recording, the reliable command guard and the team scoreboard trim. For plugin cvars see the [plugins repository](https://github.com/tjone270/minqlxtended-plugins).

What's new in v1.0.0
====================
- **Events come from the game module.** `game_start`, `game_end`, `round_end`, `team_switch`, `kill` and `death` are read out of the engine, so you don't need `zmq_stats_enable 1` any more. The ZMQ listener is only there for plugins that want to hook the raw `stats` event.
- **New events.** `damage`, `weapon_fired`, `item_pickup`, `objective`, `cvar_changed`, `demo_finished`, and the vote lifecycle. `damage` and `weapon_fired` are gated, so they don't run until a plugin hooks them, as they're extremely frequent.
- **Live views onto engine memory.** `minqlxtended.level`, `Entity`, `GameClient`, `Client`, `Item`, `Cvar`, `server` and `match_state` read and write the engine's own structs directly. Approximately 1,100 attributes across eighteen structs.
- **Entities can be spawned, moved and removed** at runtime.
- **The installed-map scan.** `installed_maps()`, `map_info()`, `factories()` and friends tell you what's installed and what gametypes each map declares.
- **Server-side demo recording**, the reliable command guard and the team scoreboard trim, all described above.
- **Two diagnostic console commands.** `qlx_prof` measures how much of the frame budget minqlxtended is using, and `qlx_pyperf` turns on CPython's perf trampoline so `perf` can name Python functions.

The [wiki](https://github.com/tjone270/minqlxtended/wiki) covers all of it: [Writing Plugins](https://github.com/tjone270/minqlxtended/wiki/Writing-Plugins) and [Events](https://github.com/tjone270/minqlxtended/wiki/Events) to get started, [Engine Views](https://github.com/tjone270/minqlxtended/wiki/Engine-Views) for `level` and `Entity`, [Upgrading](https://github.com/tjone270/minqlxtended/wiki/Upgrading) to port an existing plugin, and [Internals](https://github.com/tjone270/minqlxtended/wiki/Internals) for the engine offsets and `qlx_prof`.

The wiki is where those pages live; edit them there.

Usage
=====
Once you've configured the above CVARs and launched the server, you will quickly recognise if for instance your database configuration is wrong, as it will start printing a bunch of errors in the server console when someone connects. If you only see stuff like the following, then you know it's working like it should:
```
[minqlxtended.late_init] INFO: Loading preset plugins...
[minqlxtended.load_plugin] INFO: Loading plugin 'xxx'...
[minqlxtended.load_plugin] INFO: Loading plugin 'yyy'...
[minqlxtended.load_plugin] INFO: Loading plugin 'zzz'...
[minqlxtended.late_init] INFO: Stats listener started on tcp://127.0.0.1:?????.
[minqlxtended.late_init] INFO: We're good to go!
```

To confirm minqlxtended recognises you as the owner, try connecting to the server and type `!myperm` in chat.
If it tells you that you have permission level 0, the `qlx_owner` CVAR has not been set correctly (must use the SteamID64 number beginning with 765). Otherwise you should be good to go. As the owner, you are allowed to type commands directly into the console instead of having to use chat. You can now go ahead and add other admins too with `!setperm`.

[See here for a full command list.](https://github.com/tjone270/minqlxtended/wiki/Command-List)

Working on minqlxtended
=======================
Four build targets, each with its own object directory:

```
make              # bin/minqlxtended.x64.so, plus the Python package as bin/minqlxtended.zip
make debug        # -O0 -g, and DEBUG defined
make nopy         # no embedded interpreter; the hooks and the demo recorder only
make nopy_debug
make clean
```

Editing something under `src/engine/` rebuilds everything that includes it. `EXTRA_CFLAGS` adds flags without replacing the ones the Makefile sets. CI builds with `make EXTRA_CFLAGS=-Werror`.

Two tool scripts generate the _minqlxtended.pyi file and the field offsets in engine_fields.h.

```
python3 tools/gen_stub.py            # python/_minqlxtended.pyi, from the C method table
python3 tools/gen_field_offsets.py   # src/python/engine_fields.h, from quake_common.h
```

CI runs both with `--check`. Regenerate after changing the method table, a struct-sequence description, the `PyModule_AddIntMacro` block, or any struct in `quake_common.h`.

A third script checks facts the tree spells out in two places and compares them: the configstring skip list against the Python cache, the writable structs against their size asserts, and the natives and views against the game-module gate they need.

```
python3 tools/check_consistency.py   # report; --check exits non-zero on drift
```

Linting and type checking:

```
python3 -m ruff check .
python3 -m mypy python/minqlxtended --python-version 3.12
```

mypy reads `python/_minqlxtended.pyi` for the C module, so a stale stub will result in type errors in the package.
