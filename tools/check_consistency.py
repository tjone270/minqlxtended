#!/usr/bin/env python3
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

"""Check the facts this codebase is forced to write down twice.

    python3 tools/check_consistency.py            # report
    python3 tools/check_consistency.py --check    # exit non-zero on drift

Needs only Python and the source tree. A check belongs here when the same fact is spelled out
in two places and nothing else compares them; one that needs a running server does not.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

HOOKS = os.path.join(REPO, "src", "server", "hooks.c")
ENGINE_FIELDS = os.path.join(REPO, "src", "python", "engine_fields.h")
QUAKE_COMMON = os.path.join(REPO, "src", "engine", "quake_common.h")
EMBED = os.path.join(REPO, "src", "python", "python_embed.c")
OBJECTS = os.path.join(REPO, "src", "python", "python_objects.c")
CONFIGSTRING = os.path.join(REPO, "python", "minqlxtended", "_configstring.py")

# NULL before the first map and for the whole of every reload, so an ungated read segfaults.
GAME_MODULE_GLOBALS = ("g_entities", "level", "bg_itemlist")

# Structs a Python setter writes into, each needing a size in quake_common.h taken from the
# binary. entityState_t/entityShared_t are absent: gentity_t embeds and bounds them.
NEED_END_BOUND = {
    "level_locals_t",
    "gentity_t",
    "gclient_t",
    "client_t",
    "server_t",
    "cvar_t",
    "gitem_t",
    "playerState_t",
    "roundState_t",
    "expandedStatObj_t",
    "netchan_t",
}


def blank_comments_and_strings(source):
    """*source* with comment and string contents replaced by spaces, so offsets still line up
    with the original and a brace scan sees only code."""
    out = list(source)
    i, n = 0, len(source)
    while i < n:
        if source[i] == "/" and i + 1 < n and source[i + 1] == "/":
            while i < n and source[i] != "\n":
                out[i] = " "
                i += 1
        elif source[i] == "/" and i + 1 < n and source[i + 1] == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i + 1 < n and not (source[i] == "*" and source[i + 1] == "/"):
                if source[i] != "\n":
                    out[i] = " "
                i += 1
            if i + 1 < n:
                out[i] = out[i + 1] = " "
            i += 2
        elif source[i] in "\"'":
            quote = source[i]
            i += 1
            while i < n and source[i] != quote:
                # A backslash escape swallows the character after it, so a \" stays inside.
                step = 2 if source[i] == "\\" else 1
                for j in range(i, min(i + step, n)):
                    if source[j] != "\n":
                        out[j] = " "
                i += step
            i += 1
        else:
            i += 1
    return "".join(out)


def c_function_bodies(source):
    """(name, body) for every top-level brace block. Struct initialisers come back too, and
    cost nothing: they hold no dereferences."""
    blanked = blank_comments_and_strings(source)
    bodies, depth, start, header_from = [], 0, None, 0
    for i, char in enumerate(blanked):
        if char == "{":
            if depth == 0:
                start = i
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0 and start is not None:
                header = " ".join(blanked[header_from:start].split())
                named = re.search(r"(\w+)\s*\([^()]*\)\s*$", header)
                bodies.append((named.group(1) if named else header[-40:] or "?",
                               blanked[start:i]))
                header_from, start = i + 1, None
    return bodies


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def check_configstring_skip_list(fail):
    """The indices My_SV_SetConfigstring bypasses, against the set the cache refuses to hold.
    A skipped configstring never reaches the cache's invalidation, so caching one serves a
    value that stopped being true frames ago."""
    hooks = read(HOOKS)
    match = re.search(
        r"if \(index == (\d+) \|\| \(index >= (\d+) && index < (\d+)\)\)", hooks)
    if not match:
        fail("could not find the skip test in My_SV_SetConfigstring; if its shape changed, "
             "update this check rather than deleting it")
        return

    single, low, high = (int(g) for g in match.groups())
    from_c = {single} | set(range(low, high))

    py = read(CONFIGSTRING)
    consts = dict(re.findall(r"^#define CS_(\w+) (\d+)", read(QUAKE_COMMON), re.M))
    names = re.search(r"_UNDISPATCHED = frozenset\(\[minqlxtended\.CS_(\w+)\]\) \| frozenset\(\s*"
                      r"range\(minqlxtended\.CS_(\w+), minqlxtended\.CS_(\w+)\)", py)
    if not names:
        fail("could not find _UNDISPATCHED in _configstring.py; if its shape changed, update "
             "this check rather than deleting it")
        return

    try:
        botinfo = int(consts[names.group(1)])
        py_low = int(consts[names.group(2)])
        py_high = int(consts[names.group(3)])
    except KeyError as exc:
        fail(f"_UNDISPATCHED names {exc}, which quake_common.h does not define")
        return

    from_py = {botinfo} | set(range(py_low, py_high))
    if from_c != from_py:
        fail(f"the configstring skip list has drifted: hooks.c skips {sorted(from_c)}, _configstring.py "
             f"declines to cache {sorted(from_py)}")


def check_writable_structs_have_an_end_bound(fail):
    """Every struct a generated setter writes into needs a size the binary agreed to.

    An offset assert only says a field is where we think it is; a size says the struct ends
    where we think it does. level_locals_t carried a phantom trailing field for exactly as
    long as it had no size assert."""
    header = read(QUAKE_COMMON)
    sized = set(re.findall(r"_Static_assert\(sizeof\((\w+)\) ==", header))
    for name in sorted(NEED_END_BOUND):
        if name not in sized:
            fail(f"{name} has no _Static_assert(sizeof(...)) in quake_common.h, and Python can "
                 "write into it")

    # A size on its own proves nothing if it was read off this header rather than the binary.
    for name in sorted(sized):
        assertion = re.search(rf"_Static_assert\(sizeof\({name}\) == \d+,\s*\"([^\"]*)\"",
                              header)
        if assertion and not re.search(r"0x[0-9a-fA-F]{5,}|build \d+", assertion.group(1)):
            fail(f"the sizeof assert for {name} cites no provenance; say which instruction or "
                 "build the figure came from, or it is a tautology over this header")


def check_natives_gate_the_game_module(fail):
    """Every native that dereferences a game-module global has to check it first."""
    source = read(EMBED)
    bodies = re.split(r"\nstatic PyObject\* (PyMinqlxtended_\w+)\(", source)
    offenders = []
    for i in range(1, len(bodies), 2):
        name, body = bodies[i], bodies[i + 1].split("\n}\n")[0]
        touches = re.search(r"\b(g_entities|level|bg_itemlist)\s*(->|\[)", body)
        if not touches:
            continue
        if "qlx_vm_ready()" in body or "qlx_live_client(" in body or "qlx_valid_client_id(" in body:
            continue
        offenders.append(name)

    if offenders:
        fail(f"these natives read a game-module global without gating it: {', '.join(sorted(offenders))}")


def check_views_gate_the_game_module(fail):
    """The same rule for the live views, which gate with a NULL test on the global itself
    rather than qlx_vm_ready() so the resolver can name what it wanted."""
    offenders = []
    for name, body in c_function_bodies(read(OBJECTS)):
        for global_name in GAME_MODULE_GLOBALS:
            if not re.search(rf"\b{global_name}\s*(->|\[)", body):
                continue
            if re.search(rf"!\s*{global_name}\b", body):
                continue
            offenders.append(f"{name} ({global_name})")

    if offenders:
        fail("these read a game-module global in python_objects.c without a NULL test on it "
             f"first: {', '.join(sorted(offenders))}")


def check_python_h_comes_first(fail):
    """Any TU reaching Python.h must reach it before every other include.

    It sets _POSIX_C_SOURCE and _XOPEN_SOURCE, so read second it redefines what features.h
    set. Any include counts, not just a system one: game_events.c reached one via profile.h.
    """
    entry = re.compile(r'^\s*#\s*include\s*[<"](Python\.h|python/pyminqlxtended\.h'
                       r'|pyminqlxtended\.h|python_objects\.h)[>"]')
    any_include = re.compile(r"^\s*#\s*include\s*[<\"]")
    offenders = []
    for directory, _dirs, names in os.walk(os.path.join(REPO, "src")):
        for name in sorted(names):
            if not name.endswith(".c"):
                continue
            path = os.path.join(directory, name)
            lines = read(path).splitlines()
            reaches = next((n for n, line in enumerate(lines, 1) if entry.match(line)), None)
            if reaches is None:
                continue
            first = next(n for n, line in enumerate(lines, 1) if any_include.match(line))
            if reaches != first:
                rel = os.path.relpath(path, REPO).replace("\\", "/")
                offenders.append(f"{rel} (line {reaches}, behind line {first})")

    if offenders:
        fail("these reach Python.h behind another include, so pyconfig.h can redefine what "
             f"features.h set: {', '.join(offenders)}")


CHECKS = (
    check_configstring_skip_list,
    check_python_h_comes_first,
    check_writable_structs_have_an_end_bound,
    check_natives_gate_the_game_module,
    check_views_gate_the_game_module,
)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if anything has drifted")
    args = parser.parse_args()

    failures = []
    for check in CHECKS:
        check(failures.append)

    for failure in failures:
        print(f"{os.path.basename(__file__)}: {failure}", file=sys.stderr)

    if failures:
        return 1 if args.check else 0

    print(f"{len(CHECKS)} consistency checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
