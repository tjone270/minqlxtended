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

"""Generate the two files that describe `_minqlxtended` to everything outside it.

  * python/_minqlxtended.pyi        the type stub
  * python/minqlxtended/__init__.py the `from _minqlxtended import (...)` block, between
                                    its GENERATED markers

    python3 tools/gen_stub.py            # from the repository root; rewrites what is stale
    python3 tools/gen_stub.py --check    # exit 1 if either is out of date

`_minqlxtended` is built in through PyImport_AppendInittab, so it exists only inside a running
qzeroded and the stub is the only description of it anything else can read. Constants, struct
sequences and engine types are all read back out of the source. Signatures are not: format
strings give arity but no argument names, so SIGNATURES below is hand-maintained and render()
checks it against the method table.

The package must not republish the constants _enums.py owns, so the import block names what
it takes, using _enums.OWNED_PREFIXES and OWNED_NAMES.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
EMBED = os.path.join(REPO, "src", "python", "python_embed.c")
OBJECTS = os.path.join(REPO, "src", "python", "python_objects.c")
FIELDS = os.path.join(REPO, "src", "python", "engine_fields.h")
# Top-level, not a submodule of the package. Put python/ on MYPYPATH and this answers.
OUT = os.path.join(REPO, "python", "_minqlxtended.pyi")

INIT = os.path.join(REPO, "python", "minqlxtended", "__init__.py")
ENUMS = os.path.join(REPO, "python", "minqlxtended", "_enums.py")
INIT_BEGIN = "# --- BEGIN GENERATED ENGINE IMPORTS (tools/gen_stub.py) ---"
INIT_END = "# --- END GENERATED ENGINE IMPORTS ---"

# Field kind -> what the accessor hands back. A kind added to python_objects.c needs a row.
KIND_TYPES = {
    "INT": "int",
    "INT_RO": "int",
    "UINT": "int",
    "UINT_RO": "int",
    "U64": "int",
    "BYTE": "int",
    "SCHAR": "int",
    "FLOAT": "float",
    "BOOL": "bool",
    "BOOL_RO": "bool",
    "CHARBUF": "str",
    "CHARPTR": "str | None",
    "VEC3": "Vector3",
    "INTARR": "IntArray",
    "WEAPONS": "Weapons",
    "ENTREF": "Entity | None",
    "ENTREFARR": "tuple[Entity | None, ...]",
    # The raw address, or None. See QLX_GETSET_FNPTR in python_objects.c.
    "FNPTR": "int | None",
}

# Kinds whose QLX_SETTER_* expands to NULL, so the stub emits them as read-only properties.
READONLY_KINDS = {"CHARPTR", "ENTREFARR", "INT_RO", "UINT_RO", "BOOL_RO"}

# Which X-macro list backs which Python type. Checked against names[] in python_objects.c.
ENGINE_TYPES = [
    # (python type, field list, constructor signature or None for a singleton)
    ("Level", "LEVEL_FIELDS", None),
    # No field list: its attributes are all in EXTRA_ATTRS. See engine_fields.h.
    ("MatchState", None, None),
    ("RoundStateView", "ROUNDSTATE_FIELDS", None),
    ("EntityState", "ENTITYSTATE_FIELDS", None),
    ("EntityShared", "ENTITYSHARED_FIELDS", None),
    ("Entity", "ENTITY_FIELDS", "number: int"),
    ("PlayerStateView", "PLAYERSTATE_FIELDS", None),
    ("TeamState", "TEAMSTATE_FIELDS", None),
    ("Persistant", "PERSISTANT_FIELDS", None),
    ("Session", "SESSION_FIELDS", None),
    ("RaceInfo", "RACEINFO_FIELDS", None),
    ("ExpandedStats", "EXPANDEDSTATS_FIELDS", None),
    ("GameClient", "GAMECLIENT_FIELDS", "client_id: int"),
    ("Netchan", "NETCHAN_FIELDS", None),
    ("Client", "CLIENT_FIELDS", "client_id: int"),
    ("Cvar", "CVAR_FIELDS", "name: str"),
    ("Item", "ITEM_FIELDS", "number: int"),
    ("Server", "SERVER_FIELDS", None),
    ("ServerStatic", "SERVERSTATIC_FIELDS", None),
]

# Registered types no field list backs, written by hand in render(). The value is what an
# iterator yields, or None for IntArray.
HAND_WRITTEN_TYPES = {
    "IntArray": None,
    "EntityIterator": "Entity",
    "ItemIterator": "Item",
    "CvarIterator": "Cvar",
}

# Attributes hand-written in python_objects.c. Parsed back out and checked against this.
EXTRA_ATTRS = {
    "Level": {
        "round": "RoundStateView",
        # Aliases for fields that already appear under their own name.
        "num_playing": "int",
        "vote_caller": "int",
        "pause_begin": "int",
        "round_state": "int",
    },
    "Entity": {
        "number": "int",
        "s": "EntityState",
        "r": "EntityShared",
        "client": "GameClient | None",
    },
    "GameClient": {
        "id": "int",
        "ps": "PlayerStateView",
        "pers": "Persistant",
        "sess": "Session",
        "race": "RaceInfo",
        "expanded_stats": "ExpandedStats",
    },
    "Persistant": {"team_state": "TeamState"},
    "Client": {
        "id": "int",
        "netchan": "Netchan",
        # From the netchan, so where the client really is; the userinfo "ip" can say anything.
        "ip": "str",
        "port": "int",
        "address": "str",
        "address_type": "int",
    },
    "Item": {"number": "int"},
    "Cvar": {"string": "str", "value": "float", "integer": "int"},
    # All of MatchState: six separate globals rather than a struct, so no field list.
    "MatchState": {
        "unpause_time": "int",
        "pause_caller": "int",
        "paused_by_server": "bool",
        "timeouts_used": "IntArray",
        "team_locked": "IntArray",
        "auto_action_state": "int",
    },
}

# qlx_<prefix>_getset -> its Python type. Only tables carrying hand-written rows belong here.
GETSET_TABLES = {
    "qlx_level": "Level",
    "qlx_entity": "Entity",
    "qlx_gameclient": "GameClient",
    "qlx_client": "Client",
    "qlx_item": "Item",
    "qlx_cvar": "Cvar",
    "qlx_matchstate": "MatchState",
}

# QLX_<prefix>_EXTRA_ROWS -> the view that takes those rows. Any of the seven can carry one.
EXTRA_ROW_MACROS = {
    "PERS": "Persistant",
}

# The one hand-maintained part: what each native function takes and returns.
SIGNATURES = {
    "player_info": "(client_id: int, /) -> PlayerInfo | None",
    "players_info": "() -> list[PlayerInfo | None]",
    "get_userinfo": "(client_id: int, /) -> str | None",
    "send_server_command": "(client_id: int | None, cmd: str, /) -> bool",
    "client_command": "(client_id: int, cmd: str, /) -> bool",
    "console_command": "(cmd: str, /) -> None",
    "get_cvar": "(name: str, /) -> str | None",
    "set_cvar": "(name: str, value: str, flags: int = ..., force: bool = ...) -> Cvar",
    "set_cvar_limit": "(name: str, value: str, minimum: str, maximum: str, flags: int = ..., /) -> None",
    "kick": "(client_id: int, reason: str | None, /) -> None",
    "console_print": "(text: str, /) -> None",
    "get_configstring": "(index: int, /) -> str",
    "set_configstring": "(index: int, value: str, /) -> None",
    "force_vote": "(pass_it: bool, /) -> bool",
    "add_console_command": "(name: str, /) -> None",
    "register_handler": "(event: str, handler: Callable[..., Any] | None, /) -> None",
    "player_state": "(client_id: int, /) -> PlayerState | None",
    "player_stats": "(client_id: int, /) -> PlayerStats | None",
    "drop_holdable": "(client_id: int, /) -> bool",
    "callvote": "(vote: str, display: str, time: int = ..., caller_id: int = ..., /) -> None",
    "player_spawn": "(client_id: int, /) -> bool",
    "destroy_kamikaze_timers": "() -> bool",
    "spawn_item": "(item_id: int, x: int, y: int, z: int, /) -> bool",
    "add_event": "(entity_id: int, event: int, event_parm: int = ..., /) -> None",
    "remove_dropped_items": "() -> bool",
    "slay_with_mod": "(client_id: int, mod: int, /) -> bool",
    "replace_items": "(entity: int | str, item: int | str, /) -> bool",
    "dev_print_items": "() -> None",
    "force_weapon_respawn_time": "(respawn_time: int, /) -> bool",
    "player_expanded_stats": "(client_id: int, /) -> PlayerExpandedStats | None",
    "start_demo": "(client_id: int, /) -> bool",
    "stop_demo": "(client_id: int, /) -> bool",
    "demo_status": "(client_id: int, /) -> DemoStatus",
    "reliable_status": "() -> ReliableStatus",
    "drop_item": "(client_id: int, item_id: int, angle: float = ..., /) -> int | None",
    "remove_entity": "(entity_id: int, /) -> bool",
    "spawn_entity": ("(classname: str, keys: dict[str, str | int | float | "
                     "Sequence[float]] | None = ..., /) -> Entity | None"),
    "link_entity": "(entity_id: int, /) -> bool",
    "unlink_entity": "(entity_id: int, /) -> bool",
    # Pre-wrapped: the renderer writes each of these out as it stands.
    "entities": ("(inuse: bool = ..., etype: int | None = ..., start: int = ...,\n"
                 "             stop: int = ..., classname: str | None = ...) "
                 "-> Iterator[Entity]"),
    "items": "() -> Iterator[Item]",
    "cvar": "(name: str, /) -> Cvar | None",
    "cvars": "() -> Iterator[Cvar]",
}

# Exported by the C module but not republished by the package. They stay in the stub.
UNPUBLISHED = {
    # _configstring.py imports this directly; the package's configstring() wraps it.
    "get_configstring",
}

HEADER = '''"""Type stub for the `_minqlxtended` C extension.

GENERATED by tools/gen_stub.py. Don't edit it. Re-run that after changing the method
table, the struct sequence descriptors, the PyModule_AddIntMacro block in python_embed.c,
or the field lists in engine_fields.h.

The module is registered through PyImport_AppendInittab and only exists inside a running
qzeroded, so this file is the only description of it anything outside one can read.
"""

from typing import Any, Callable, Final, Iterator, Sequence, SupportsIndex, overload

__version__: str
DEBUG: bool
'''


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_method_table(source):
    """Every name in the PyMethodDef table. Spans to the entry's closing brace, because
    a METH_KEYWORDS entry puts its cast and function on the following line."""
    return set(re.findall(r'\{"(\w+)",[^}]{0,200}?PyMinqlxtended_\w+', source, re.DOTALL))


def parse_constants(source):
    """Every PyModule_AddIntMacro name, in declaration order."""
    return re.findall(r"PyModule_AddIntMacro\(module,\s*([A-Za-z_0-9]+)\)", source)


def parse_struct_sequences(source):
    """[(python name, [field names])], in declaration order."""
    arrays = {}
    for match in re.finditer(
            r"static PyStructSequence_Field (\w+)_fields\[\]\s*=\s*\{(.*?)\};",
            source, re.DOTALL):
        arrays[match.group(1)] = re.findall(r'\{"(\w+)"', match.group(2))

    out = []
    for match in re.finditer(
            r'static PyStructSequence_Desc (\w+)_desc\s*=\s*\{\s*"(\w+)"', source):
        fields = arrays.get(match.group(1))
        if fields is not None:
            out.append((match.group(2), fields))
    return out


GETSET_ROW = re.compile(r'\{"(\w+)",\s*\(getter\)\w+,\s*(NULL|\(setter\)\w+)')


def _getset_rows(text):
    """{attribute: whether it can be assigned} for the hand-written rows in *text*."""
    return {name: setter != "NULL" for name, setter in GETSET_ROW.findall(text)}


def _parse_getset_rows(objects_source):
    """{python type: {attribute: writable}} for the rows written by hand.

    A text parse sees only those, the rest being X-macro expansions. Hand-written rows with
    no entry in GETSET_TABLES/EXTRA_ROW_MACROS are an error: they would be silently absent
    from the stub."""
    found = {}

    def add(source_name, rows, mapping, where):
        type_name = mapping.get(source_name)
        if type_name is None:
            if rows:
                raise SystemExit(
                    f"{source_name} in python_objects.c has hand-written rows "
                    f"({', '.join(sorted(rows))}) but no Python type in {where}.\n"
                    "Add it in tools/gen_stub.py.")
            return
        found.setdefault(type_name, {}).update(rows)

    for match in re.finditer(
            r"static PyGetSetDef (\w+)_getset\[\]\s*=\s*\{(.*?)\{NULL\}\}",
            objects_source, re.DOTALL):
        add(match.group(1), _getset_rows(match.group(2)), GETSET_TABLES, "GETSET_TABLES")

    # The views take their extra rows through a macro argument instead of a table.
    for match in re.finditer(r"#define QLX_(\w+)_EXTRA_ROWS(.*?)(?=\n\n|\n#define|\nQLX_)",
                             objects_source, re.DOTALL):
        add(match.group(1), _getset_rows(match.group(2)), EXTRA_ROW_MACROS,
            "EXTRA_ROW_MACROS")

    return found


def parse_registered_types(objects_source):
    """The names[] array in PyMinqlxtended_AddObjectTypes, as a set of type names."""
    match = re.search(r"const char\*\s*names\[\]\s*=\s*\{(.*?)\};", objects_source, re.S)
    if not match:
        raise SystemExit(
            "python_objects.c has no names[] array in PyMinqlxtended_AddObjectTypes, so the "
            "registered types cannot be read. Fix the pattern in tools/gen_stub.py.")
    return set(re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)"', match.group(1)))


def parse_extra_getsets(objects_source):
    """{python type: {attribute names}} for the rows written by hand."""
    return {name: set(rows) for name, rows in _parse_getset_rows(objects_source).items()}


def parse_owned(enums_source):
    """(prefixes, names) that _enums.py says its enums speak for. A text parse, since
    `_enums` reads `_minqlxtended` and so cannot be imported outside a running qzeroded."""
    block = re.search(r"^OWNED_PREFIXES\s*=\s*\{(.*?)^\}", enums_source, re.DOTALL | re.MULTILINE)
    names = re.search(r"^OWNED_NAMES\s*=\s*frozenset\(\((.*?)\)\)",
                      enums_source, re.DOTALL | re.MULTILINE)
    if not block or not names:
        raise SystemExit(
            f"could not find OWNED_PREFIXES/OWNED_NAMES in {ENUMS}; if they were renamed, "
            "update parse_owned() in tools/gen_stub.py")

    prefixes = tuple(re.findall(r'"(\w+_)"\s*:', block.group(1)))
    exact = frozenset(re.findall(r'"(\w+)"', names.group(1)))
    if not prefixes or not exact:
        raise SystemExit(f"OWNED_PREFIXES/OWNED_NAMES parsed empty from {ENUMS}")
    return prefixes, exact


def parse_field_lists(source):
    """{list name: [(kind, python name)]}, skipping the macro definitions."""
    lists = {}
    current = None
    row = re.compile(r"^\s*X\(\s*(\w+)\s*,\s*[\w.]+\s*,\s*(\w+)\s*,\s*\d+\s*,")
    for line in source.split("\n"):
        start = re.match(r"^#define (\w+)\(X\)", line)
        if start:
            current = start.group(1)
            lists[current] = []
            continue
        if current is None:
            continue
        match = row.match(line)
        if match:
            lists[current].append((match.group(1), match.group(2)))
        elif not line.strip().endswith("\\"):
            current = None
    return lists


def _attribute(name, attr_type, writable):
    """A class-body attribute, as a property when the engine gives no way to write it."""
    if writable:
        return [f"    {name}: {attr_type}"]
    return ["    @property", f"    def {name}(self) -> {attr_type}: ..."]


def render(embed_source, fields_source, objects_source):
    constants = parse_constants(embed_source)
    structseqs = parse_struct_sequences(embed_source)
    field_lists = parse_field_lists(fields_source)

    # The hand-written getsets, checked against EXTRA_ATTRS.
    found = _parse_getset_rows(objects_source)
    for type_name in sorted(set(found) | set(EXTRA_ATTRS)):
        attrs = set(found.get(type_name, {}))
        declared = set(EXTRA_ATTRS.get(type_name, {}))
        missing = sorted(attrs - declared)
        extra = sorted(declared - attrs)
        if missing or extra:
            raise SystemExit(
                f"EXTRA_ATTRS is out of step with the getsets in python_objects.c for {type_name}\n"
                f"  no type given for: {', '.join(missing) or 'none'}\n"
                f"  no such getset:    {', '.join(extra) or 'none'}\n"
                "Add or remove them in tools/gen_stub.py.")

    # The mapping --check was blind to: a registered type with no entry here was simply absent.
    registered = parse_registered_types(objects_source)
    known = {name for name, _, _ in ENGINE_TYPES} | set(HAND_WRITTEN_TYPES)
    missing = sorted(registered - known)
    extra = sorted(known - registered)
    if missing or extra:
        raise SystemExit(
            "ENGINE_TYPES is out of step with names[] in python_objects.c\n"
            f"  no stub entry for: {', '.join(missing) or 'none'}\n"
            f"  not registered:    {', '.join(extra) or 'none'}\n"
            "Add or remove them in tools/gen_stub.py.")

    out = [HEADER]

    out.append("\n# --- struct sequences ---------------------------------------------------------\n")
    out.append("# Snapshots, taken when you ask for them. Tuple subclasses: index them, unpack")
    out.append("# them, or read the fields by name. _replace() gives a copy with fields changed.\n")
    for name, seq_fields in structseqs:
        out.append(f"class {name}(tuple[Any, ...]):")
        out.append("    _fields: Final[tuple[str, ...]]")
        out.append("    n_fields: Final[int]")
        out.append("    n_sequence_fields: Final[int]")
        out.append("    n_unnamed_fields: Final[int]")
        out.append("    def __init__(self, sequence: Any, /) -> None: ...")
        for field in seq_fields:
            out.extend(_attribute(field, "Any", False))
        out.append(f"    def _replace(self, **fields: Any) -> \"{name}\": ...")
        out.append("")

    out.append("\n# --- live engine views --------------------------------------------------------\n")
    out.append("# Not snapshots: reading an attribute dereferences the engine there and then, and")
    out.append("# assigning writes straight through. Game thread only. An attribute written as a")
    out.append("# property is read-only; the engine offers no way to set it.\n")
    out.append("class IntArray:")
    out.append("    def __len__(self) -> int: ...")
    out.append("    @overload")
    out.append("    def __getitem__(self, index: SupportsIndex, /) -> int: ...")
    out.append("    @overload")
    out.append("    def __getitem__(self, index: slice, /) -> list[int]: ...")
    out.append("    def __setitem__(self, index: int, value: int) -> None: ...")
    out.append("    def __iter__(self) -> Iterator[int]: ...")
    out.append("")

    # Not constructible from Python; each comes off the function that hands it back.
    for name in sorted(n for n, yields in HAND_WRITTEN_TYPES.items() if yields):
        out.append(f"class {name}:")
        out.append(f"    def __iter__(self) -> \"{name}\": ...")
        out.append(f"    def __next__(self) -> {HAND_WRITTEN_TYPES[name]}: ...")
        out.append("")

    for type_name, list_name, ctor in ENGINE_TYPES:
        if list_name is not None and list_name not in field_lists:
            raise SystemExit(
                f"{type_name} claims the field list {list_name}, which engine_fields.h does not define.\n"
                "Fix the name in ENGINE_TYPES in tools/gen_stub.py.")
        rows = field_lists.get(list_name, [])
        out.append(f"class {type_name}:")
        # PyArg_ParseTupleAndKeywords with a kwlist, so the argument can be passed by name.
        if ctor:
            out.append(f"    def __init__(self, {ctor}) -> None: ...")
        generated = {attr for _, attr in rows}
        for kind, attr in rows:
            if kind not in KIND_TYPES:
                raise SystemExit(
                    f"no Python type for the field kind {kind}, seen on {type_name}.{attr}\n"
                    "Add a row to KIND_TYPES in tools/gen_stub.py.")
            out.extend(_attribute(attr, KIND_TYPES[kind], kind not in READONLY_KINDS))
        for attr, attr_type in sorted(EXTRA_ATTRS.get(type_name, {}).items()):
            if attr not in generated:
                out.extend(_attribute(attr, attr_type,
                                      found.get(type_name, {}).get(attr, True)))
        if type_name == "Cvar":
            out.append("    def set(self, value: str, force: bool = ...) -> None: ...")
        if not rows and not ctor and type_name not in EXTRA_ATTRS:
            out.append("    ...")
        out.append("")

    out.append("\n# --- singletons ---------------------------------------------------------------\n")
    out.append("level: Level")
    out.append("match_state: MatchState")
    out.append("server: Server")
    out.append("server_static: ServerStatic")
    out.append("")

    out.append("\n# --- exceptions ---------------------------------------------------------------\n")
    out.append("class EngineStateError(RuntimeError):")
    out.append("    \"\"\"The engine is not in a state where that question has an answer.\"\"\"")
    out.append("")

    # The part that can rot: a method-table entry with no signature would be absent.
    table = parse_method_table(embed_source)
    missing = sorted(table - set(SIGNATURES))
    extra = sorted(set(SIGNATURES) - table)
    if missing or extra:
        raise SystemExit(
            "SIGNATURES is out of step with the method table in python_embed.c\n"
            f"  no signature for: {', '.join(missing) or 'none'}\n"
            f"  no such function: {', '.join(extra) or 'none'}\n"
            "Add or remove them in tools/gen_stub.py.")

    out.append("\n# --- functions ----------------------------------------------------------------\n")
    for name in sorted(SIGNATURES):
        out.append(f"def {name}{SIGNATURES[name]}: ...")
    out.append("")

    out.append("\n# --- constants ----------------------------------------------------------------\n")
    out.append("# All int. The package rebinds most of these to enum members in _enums.py;")
    out.append("# what the C module itself exports is a plain int.\n")
    for name in constants:
        out.append(f"{name}: int")
    out.append("")

    return "\n".join(out)


def _wrap(names, indent="    ", width=94):
    """*names* as comma-separated lines that fit inside an import parenthesis."""
    lines, current = [], indent
    for name in names:
        piece = name + ","
        if len(current) + len(piece) + 1 > width and current != indent:
            lines.append(current.rstrip())
            current = indent
        current += piece + " "
    if current.strip():
        lines.append(current.rstrip())
    return lines


def render_init_imports(embed_source, enums_source):
    """The `from _minqlxtended import (...)` block the package uses, as text."""
    prefixes, exact = parse_owned(enums_source)

    def owned(name):
        return name in exact or name.startswith(prefixes)

    constants = [c for c in parse_constants(embed_source) if not owned(c)]
    structseqs = [name for name, _ in parse_struct_sequences(embed_source)]
    # IntArray joins them: several MatchState and Level attributes are typed as one.
    types = [name for name, _, _ in ENGINE_TYPES] + ["IntArray"]

    out = [INIT_BEGIN]
    out.append("from _minqlxtended import (  # noqa: F401")
    out.append("    # Functions.")
    out.extend(_wrap(sorted(set(SIGNATURES) - UNPUBLISHED)))
    out.append("    # Struct sequences. Snapshots, taken when you ask for them.")
    out.extend(_wrap(sorted(structseqs)))
    out.append("    # Live engine views, and the singletons among them.")
    out.extend(_wrap(sorted(types) + ["level", "match_state", "server", "server_static"]))
    out.append("    # Errors.")
    out.extend(_wrap(["EngineStateError"]))
    out.append("    # Constants the enums in _enums.py do not supersede: the configstring")
    out.append("    # indices, which are two families under one prefix, and the MAX_* bounds.")
    out.extend(_wrap(sorted(constants)))
    out.append(")")
    out.append(INIT_END)
    return "\n".join(out)


def splice_init(existing, block):
    """*existing* with the generated region replaced by *block*."""
    start = existing.find(INIT_BEGIN)
    end = existing.find(INIT_END)
    if start < 0 or end < 0:
        raise SystemExit(
            f"{INIT} has no {INIT_BEGIN} / {INIT_END} markers; the generated import block cannot be placed.")
    return existing[:start] + block + existing[end + len(INIT_END):]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if either output is out of date instead of writing it")
    args = parser.parse_args()

    for path in (EMBED, FIELDS, OBJECTS, INIT, ENUMS):
        if not os.path.exists(path):
            sys.exit(f"{path} is missing; run this from the repository root")

    embed, fields, objects, enums = read(EMBED), read(FIELDS), read(OBJECTS), read(ENUMS)

    outputs = [
        (OUT, "_minqlxtended.pyi", render(embed, fields, objects)),
        (INIT, "minqlxtended/__init__.py",
         splice_init(read(INIT), render_init_imports(embed, enums))),
    ]

    stale = [(path, label, text) for path, label, text in outputs
             if (read(path) if os.path.exists(path) else None) != text]

    if not stale:
        print("_minqlxtended.pyi and minqlxtended/__init__.py are up to date")
        return 0

    if args.check:
        for _, label, _ in stale:
            print(f"{label} is out of date; run tools/gen_stub.py")
        return 1

    for path, label, text in stale:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        written = len(text.split("\n"))
        print(f"wrote {label} ({written} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
