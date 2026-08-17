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

"""Fill in the OFF column of the X-macro field lists in engine_fields.h.

    python3 tools/gen_field_offsets.py     # from the repository root, needs a C compiler

Rewrites the header in place, idempotently. The offsets are compiled out of the real headers
so that reordering a field fails the build naming the Python attribute, rather than silently
returning its neighbour. This only checks engine_fields.h against quake_common.h; what pins
quake_common.h to the game module is its disassembly-derived _Static_asserts, not generated.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
HEADER = os.path.join(REPO, "src", "python", "engine_fields.h")

# The struct each list describes. Add a list here and its offsets get generated too.
LISTS = {
    "ROUNDSTATE_FIELDS": "roundState_t",
    "LEVEL_FIELDS": "level_locals_t",
    "ENTITYSTATE_FIELDS": "entityState_t",
    "ENTITYSHARED_FIELDS": "entityShared_t",
    "ENTITY_FIELDS": "gentity_t",
    "PLAYERSTATE_FIELDS": "playerState_t",
    "TEAMSTATE_FIELDS": "playerTeamState_t",
    "PERSISTANT_FIELDS": "clientPersistant_t",
    "SESSION_FIELDS": "clientSession_t",
    "RACEINFO_FIELDS": "raceInfo_t",
    "EXPANDEDSTATS_FIELDS": "expandedStatObj_t",
    "GAMECLIENT_FIELDS": "gclient_t",
    "NETCHAN_FIELDS": "netchan_t",
    "CLIENT_FIELDS": "client_t",
    "CVAR_FIELDS": "cvar_t",
    "ITEM_FIELDS": "gitem_t",
    "SERVER_FIELDS": "server_t",
    "SERVERSTATIC_FIELDS": "serverStatic_t",
}

# X(KIND, FIELD, NAME, OFF, DOC). DOC is unparsed, being free text to end of line. FIELD
# allows a dot so a flattened member like `pos.trType` works in both offsetof and p->FIELD.
ROW = re.compile(
    r"""^(?P<indent>\s*)X\(\s*
        (?P<kind>\w+)\s*,\s*
        (?P<field>[\w.]+)\s*,\s*
        (?P<name>\w+)\s*,\s*
        (?P<off>\d+)\s*,\s*
        (?P<rest>.*)$""",
    re.VERBOSE,
)


def parse_lists(text):
    """Map list name -> [(field, name), ...] in declaration order."""
    found = {}
    current = None
    for line in text.splitlines():
        opened = re.match(r"^#define (\w+)\(X\)", line)
        if opened and opened.group(1) in LISTS:
            current = opened.group(1)
            found[current] = []
            continue
        if current is None:
            continue
        row = ROW.match(line)
        if row:
            found[current].append((row.group("field"), row.group("name")))
        # A line that neither continues the macro nor matches a row ends the list.
        elif not line.rstrip().endswith("\\") and line.strip():
            current = None
    return found


def compute_offsets(lists, cc, includes):
    """Compile a throwaway program that prints offsetof for every field."""
    src = ['#include <stddef.h>', '#include <stdio.h>', '#include "engine/quake_common.h"',
           "int main(void) {"]
    for list_name, fields in lists.items():
        struct = LISTS[list_name]
        for field, _ in fields:
            src.append(
                f'    printf("{list_name}|{field}|%zu\\n", offsetof({struct}, {field}));'
            )
    src.append("    return 0;")
    src.append("}")

    with tempfile.TemporaryDirectory() as tmp:
        csrc = os.path.join(tmp, "offsets.c")
        exe = os.path.join(tmp, "offsets")
        with open(csrc, "w", encoding="utf-8") as f:
            f.write("\n".join(src) + "\n")

        cmd = [cc, "-w", "-I", os.path.join(REPO, "src")] + includes + ["-o", exe, csrc]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr)
            raise SystemExit(
                "offset harness failed to compile - a field name in engine_fields.h "
                "probably does not exist in the struct it claims to describe"
            )

        out = subprocess.run([exe], capture_output=True, text=True, check=True).stdout

    offsets = {}
    for line in out.splitlines():
        if line.strip():
            list_name, field, value = line.split("|")
            offsets[(list_name, field)] = int(value)
    return offsets


def rewrite(text, offsets):
    lines = text.splitlines(keepends=True)
    current = None
    changes = []
    for i, line in enumerate(lines):
        opened = re.match(r"^#define (\w+)\(X\)", line)
        if opened and opened.group(1) in LISTS:
            current = opened.group(1)
            continue
        if current is None:
            continue
        row = ROW.match(line)
        if not row:
            if not line.rstrip().endswith("\\") and line.strip():
                current = None
            continue

        want = offsets[(current, row.group("field"))]
        if int(row.group("off")) == want:
            continue

        # Rebuild the row, keeping the column alignment the file is written in.
        old_off = row.group("off")
        head = line[: row.start("off")]
        tail = line[row.end("off"):]
        pad = len(old_off) - len(str(want))
        if pad > 0:
            head = head + " " * pad
        elif pad < 0:
            head = head[:pad] if head.endswith(" " * -pad) else head
        lines[i] = head + str(want) + tail
        changes.append(f"{current}.{row.group('field')}: {old_off} -> {want}")

    return "".join(lines), changes


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if the file would change, without writing it")
    args = ap.parse_args()

    with open(HEADER, encoding="utf-8", newline="") as f:
        text = f.read()

    lists = parse_lists(text)
    missing = set(LISTS) - set(lists)
    if missing:
        raise SystemExit(f"no rows found for: {', '.join(sorted(missing))}")

    # A list LISTS does not name gets no offsets, and its rows land in the previous list.
    unregistered = sorted(set(re.findall(r"^#define (\w+)\(X\)", text, re.MULTILINE))
                          - set(LISTS))
    if unregistered:
        raise SystemExit(
            f"engine_fields.h defines {', '.join(unregistered)}, which LISTS does not name. Add it "
            "to LISTS in tools/gen_field_offsets.py with the struct it describes.")

    offsets = compute_offsets(lists, args.cc, [])
    updated, changes = rewrite(text, offsets)

    for name, fields in lists.items():
        print(f"{name}: {len(fields)} fields")

    if not changes:
        print("offsets already correct")
        return 0

    if args.check:
        for change in changes:
            print(f"would update {change}")
        return 1

    with open(HEADER, "w", encoding="utf-8", newline="") as f:
        f.write(updated)
    for change in changes:
        print(f"updated {change}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
