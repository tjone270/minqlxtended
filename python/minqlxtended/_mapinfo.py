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

"""What is installed on this server: the maps, their .arena declarations, their BSP
entities, and the factories. Read from the pk3 files themselves, off the same search
paths the engine mounts.

Three search locations, in ascending precedence:
    <fs_basepath>/baseq3          the base game data
    <qlx_workshopPath>/<id>/      Steam Workshop items (derived from fs_basepath as
                                  ../../workshop/content/282440 when the cvar is empty)
    <fs_homepath>/baseq3          the server admin's own overrides

Within one directory pk3s apply in case-insensitive filename order, then that directory's
loose .arena and .factories files (in it or in its scripts/ subdirectory). Within one
source the consolidated ``arenas.txt`` and ``factories.txt`` are read first and per-map
files layer over them. :attr:`MapInfo.sources` lists every pk3 carrying a map, winner
first.

Queries within 30 seconds of the last scan are dictionary lookups. The first query after
that walks the search paths again on the calling thread, re-reading only the pk3s whose
size or mtime changed, so call these from a :func:`minqlxtended.thread` worker.
late_init() warms the cache in the background when ``qlx_mapinfoScan`` is on.

Maps that exist only as loose files outside any pk3 are not seen.
"""

from __future__ import annotations

import minqlxtended

import collections
import json
import os
import struct
import threading
import time
import types
import typing
import zipfile

from ._core import thread
from ._enums import Gametype

__all__ = (
    "ArenaInfo", "FactoryInfo", "MapEntity", "MapInfo", "MapSource",
    "factories", "factory_info", "installed_maps", "map_entities", "map_info",
    "map_supports", "map_supports_factory", "map_worldspawn", "refresh_map_cache",
)

_MAX_ENTITY_LUMP = 16 * 1024 * 1024
_STAT_TTL = 30.0
_ENTITY_CACHE_SIZE = 8

class MapSource(typing.NamedTuple):
    """One pk3 that carries a map. A map can have several; precedence decides which wins."""

    pk3_path: str
    #: The Steam Workshop item the pk3 belongs to, from its directory name, or None.
    workshop_id: int | None
    #: Whether the pk3 also carries the map's .aas file, i.e. bots can play it.
    has_aas: bool
    #: Whether the pk3 carries a levelshot for the map.
    has_levelshot: bool


class ArenaInfo(typing.NamedTuple):
    """One map's arena entry, from arenas.txt or a per-map .arena file."""

    map: str
    longname: str
    author: str
    #: The raw space-separated "type" tokens, lowercased, unknown ones included.
    type_tokens: tuple[str, ...]
    #: The tokens this build recognises, as :class:`Gametype` members, in declared order.
    gametypes: tuple[Gametype, ...]
    fraglimit: int | None
    timelimit: int | None
    #: Every key/value pair as parsed, for the keys not modelled above. A read-only view of
    #: what the cache holds.
    raw: typing.Mapping[str, str]
    #: Where the entry came from: "<pk3>:<member>" or a loose file path.
    source: str


class FactoryInfo(typing.NamedTuple):
    """One factory, from factories.txt or a .factories file. Both are JSON."""

    id: str
    title: str
    author: str
    description: str
    #: The factory's base gametype, or None when the file's token isn't one we recognise.
    basegt: Gametype | None
    #: The raw basegt token, kept even when unrecognised.
    basegt_raw: str
    #: The cvars the factory applies, values as strings. A read-only view of what the
    #: cache holds.
    cvars: typing.Mapping[str, str]
    #: Where the factory came from: "<pk3>:<member>" or a loose file path.
    source: str


class MapEntity(typing.NamedTuple):
    """One entity from a BSP's entities lump."""

    classname: str
    #: The parsed "origin" key, or None when the entity has none (or it doesn't parse).
    origin: tuple[float, float, float] | None
    #: Every key/value pair, classname and origin included, keys lowercased. A read-only
    #: view of what the cache holds; every reader of the map gets the same one.
    keys: typing.Mapping[str, str]


class MapInfo(typing.NamedTuple):
    """What :func:`installed_maps` and :func:`map_info` provide.

    Everything here comes from the zip directories and the small declaration members. The
    entity list needs a BSP read: see :func:`map_entities` and :func:`map_worldspawn`.
    """

    #: The short map name, lowercased: "longestyard".
    name: str
    #: Every pk3 carrying maps/<name>.bsp, the winning one first.
    sources: tuple[MapSource, ...]
    #: The winning arena entry, or None when nothing installed declares this map.
    arena: ArenaInfo | None
    #: The workshop item providing the map, when any source is one.
    workshop_id: int | None
    #: Whether any source carries the .aas file; the filesystem serves it from wherever it is.
    has_aas: bool


# PARSERS

def _logger():
    return minqlxtended.get_logger()


def _tokenize_info(text: str) -> tuple[list[str], bool]:
    """Split idTech3 info-file text into tokens: braces, quoted strings and bare words.

    Returns the tokens and whether the text ended inside a quote.
    """
    tokens: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n\0":
            i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
        elif c == '"':
            j = text.find('"', i + 1)
            if j < 0:
                return tokens, True
            tokens.append(text[i + 1:j])
            i = j + 1
        elif c in "{}":
            tokens.append(c)
            i += 1
        else:
            j = i
            while j < n and text[j] not in ' \t\r\n\0{}"':
                j += 1
            tokens.append(text[i:j])
            i = j
    return tokens, False


def _parse_info_blocks(text: str, source: str) -> list[dict[str, str]]:
    """Parse ``{ key "value" ... }`` blocks: the .arena format and the BSP entity string.

    Keys are lowercased, a duplicate key's later value wins, and a block the text ends in
    the middle of is dropped with a warning while the complete blocks before it are kept.
    """
    tokens, truncated = _tokenize_info(text)
    blocks: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    pending_key: str | None = None
    for token in tokens:
        if token == "{":
            if current is not None:
                blocks.append(current)
            current = {}
            pending_key = None
        elif token == "}":
            if current is not None:
                blocks.append(current)
            current = None
            pending_key = None
        elif current is not None:
            if pending_key is None:
                pending_key = token.lower()
            else:
                current[pending_key] = token
                pending_key = None
    if truncated or current is not None:
        _logger().warning("Truncated info text in %s; kept %d complete block(s).", source, len(blocks))
    return blocks


_GAMETYPE_TOKEN_ALIASES: dict[str, Gametype] = {gt.value: gt for gt in Gametype}
_GAMETYPE_TOKEN_ALIASES.update({
    "oneflag": Gametype.ONE_FLAG,
    "1fctf": Gametype.ONE_FLAG,
    "harvester": Gametype.HARVESTER,
    "overload": Gametype.OVERLOAD,
    "obelisk": Gametype.OVERLOAD,  # Team Arena's name for it
    "freezetag": Gametype.FREEZE_TAG,
    "dm": Gametype.FFA,
    "tourney": Gametype.DUEL,  # Quake 3 names from here down
    "team": Gametype.TDM,
    "single": Gametype.FFA,
})


def _gametype_from_token(token: str) -> Gametype | None:
    return _GAMETYPE_TOKEN_ALIASES.get(token.strip().lower())


def _int_or_none(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return None


def _arena_from_block(block: dict[str, str], source: str) -> ArenaInfo | None:
    mapname = block.get("map", "").lower()
    if not mapname:
        return None
    type_tokens = tuple(block.get("type", "").lower().split())
    gametypes = []
    for token in type_tokens:
        gt = _gametype_from_token(token)
        if gt is not None and gt not in gametypes:
            gametypes.append(gt)
    return ArenaInfo(
        map=mapname,
        longname=block.get("longname", ""),
        author=block.get("author", ""),
        type_tokens=type_tokens,
        gametypes=tuple(gametypes),
        fraglimit=_int_or_none(block.get("fraglimit")),
        timelimit=_int_or_none(block.get("timelimit")),
        raw=types.MappingProxyType(block),
        source=source,
    )


def _parse_arena(text: str, source: str) -> dict[str, ArenaInfo]:
    """Every map's entry from one .arena file, keyed by lowercased map name."""
    arenas: dict[str, ArenaInfo] = {}
    for block in _parse_info_blocks(text, source):
        arena = _arena_from_block(block, source)
        if arena is not None:
            arenas[arena.map] = arena
    return arenas


def _parse_factories(text: str, source: str) -> dict[str, FactoryInfo]:
    """Every factory from one .factories file, keyed by id.

    The format is JSON: a list of factory objects, or a single one.
    """
    try:
        data = json.loads(text)
    except ValueError as e:
        _logger().warning("Unparseable .factories file %s: %s", source, e)
        return {}
    if isinstance(data, dict):
        data = [data]
    if not isinstance(data, list):
        _logger().warning("Unexpected .factories structure in %s: %s.", source, type(data).__name__)
        return {}

    factories_found: dict[str, FactoryInfo] = {}
    for entry in data:
        if not isinstance(entry, dict) or not isinstance(entry.get("id"), str) or not entry["id"]:
            _logger().warning("Skipping a factory entry with no usable id in %s.", source)
            continue
        factory_id = entry["id"]
        basegt_raw = entry.get("basegt", entry.get("gametype", ""))
        if not isinstance(basegt_raw, str):
            basegt_raw = str(basegt_raw)
        cvars = entry.get("cvars")
        if not isinstance(cvars, dict):
            cvars = {}
        factories_found[factory_id] = FactoryInfo(
            id=factory_id,
            title=entry["title"] if isinstance(entry.get("title"), str) and entry["title"] else factory_id,
            author=entry["author"] if isinstance(entry.get("author"), str) else "",
            description=entry["description"] if isinstance(entry.get("description"), str) else "",
            basegt=_gametype_from_token(basegt_raw),
            basegt_raw=basegt_raw,
            cvars=types.MappingProxyType({str(k): str(v) for k, v in cvars.items()}),
            source=source,
        )
    return factories_found


def _read_bsp_entities(fileobj, source: str) -> str:
    """The entities lump of an IBSP file, as text.

    Reads forward only, since a zip member decompresses forward: header, lump directory,
    then discard up to the lump.
    """
    header = fileobj.read(8)
    if len(header) < 8:
        _logger().warning("%s is too short to be a BSP.", source)
        return ""
    magic, version = struct.unpack("<4si", header)
    if magic != b"IBSP":
        _logger().warning("%s is not an IBSP file (magic %r).", source, magic)
        return ""
    if version != 47:
        _logger().warning("%s has unexpected BSP version %d; attempting to read it anyway.", source, version)
    directory = fileobj.read(17 * 8)
    if len(directory) < 17 * 8:
        _logger().warning("%s ends inside its lump directory.", source)
        return ""
    offset, length = struct.unpack_from("<ii", directory, 0)
    if offset < 8 + 17 * 8 or length <= 0:
        _logger().warning("%s has no readable entities lump (offset %d, length %d).", source, offset, length)
        return ""
    if length > _MAX_ENTITY_LUMP:
        _logger().warning("%s claims a %d byte entities lump; reading the first %d.", source, length, _MAX_ENTITY_LUMP)
        length = _MAX_ENTITY_LUMP

    remaining = offset - (8 + 17 * 8)
    while remaining > 0:
        skipped = fileobj.read(min(remaining, 1 << 20))
        if not skipped:
            _logger().warning("%s ends before its entities lump.", source)
            return ""
        remaining -= len(skipped)

    data = fileobj.read(length)
    return data.split(b"\0", 1)[0].decode("latin-1")


def _entities_from_text(text: str, source: str) -> list[MapEntity]:
    entities = []
    for block in _parse_info_blocks(text, source):
        origin = None
        raw_origin = block.get("origin", "").split()
        if len(raw_origin) == 3:
            try:
                origin = (float(raw_origin[0]), float(raw_origin[1]), float(raw_origin[2]))
            except ValueError:
                pass
        entities.append(MapEntity(classname=block.get("classname", ""), origin=origin,
                                  keys=types.MappingProxyType(block)))
    return entities


# SCANNER AND CACHE


class _Candidate(typing.NamedTuple):
    """One file the scanner would read, in fold order: ascending precedence."""

    path: str
    kind: str  # "pk3", "arena" or "factories"
    workshop_id: int | None
    mtime_ns: int
    size: int


class _ProviderRecord(typing.NamedTuple):
    """What one candidate contributed, cached against its stat signature."""

    mtime_ns: int
    size: int
    maps: dict[str, tuple[bool, bool]]  # name -> (has_aas, has_levelshot)
    arenas: dict[str, ArenaInfo]
    factories: dict[str, FactoryInfo]


_EMPTY_MAPS: dict[str, tuple[bool, bool]] = {}


def _workshop_root() -> str | None:
    override = minqlxtended.get_cvar("qlx_workshopPath")
    if override:
        return override
    basepath = minqlxtended.get_cvar("fs_basepath")
    if not basepath:
        return None
    # steamcmd puts the server in steamapps/common/<name> and workshop items in
    # steamapps/workshop/content/<appid>, so two levels up and across.
    return os.path.normpath(os.path.join(basepath, "..", "..", "workshop", "content", "282440"))


def _stat_candidate(path: str, kind: str, workshop_id: int | None) -> _Candidate | None:
    try:
        st = os.stat(path)
    except OSError:
        return None
    return _Candidate(path, kind, workshop_id, st.st_mtime_ns, st.st_size)


_STOCK_ARENAS = "arenas.txt"
_STOCK_FACTORIES = "factories.txt"
_STOCK_INFO = frozenset((_STOCK_ARENAS, _STOCK_FACTORIES))


def _loose_order(name: str) -> tuple[int, str]:
    """Consolidated declarations before per-map ones, so a per-map file wins."""
    lower = name.lower()
    return (0 if lower in _STOCK_INFO else 1, lower)


def _dir_candidates(directory: str, workshop_id: int | None = None) -> list[_Candidate]:
    """One directory's pk3s in the engine's order, then its loose override files."""
    try:
        names = os.listdir(directory)
    except OSError:
        return []
    out = []
    for name in sorted((n for n in names if n.lower().endswith(".pk3")), key=str.lower):
        candidate = _stat_candidate(os.path.join(directory, name), "pk3", workshop_id)
        if candidate is not None:
            out.append(candidate)
    for subdir in ("", "scripts"):
        try:
            loose = os.listdir(os.path.join(directory, subdir)) if subdir else names
        except OSError:
            continue
        for name in sorted(loose, key=_loose_order):
            lower = name.lower()
            if lower.endswith(".arena") or lower == _STOCK_ARENAS:
                kind = "arena"
            elif lower.endswith(".factories") or lower == _STOCK_FACTORIES:
                kind = "factories"
            else:
                continue
            candidate = _stat_candidate(os.path.join(directory, subdir, name), kind, workshop_id)
            if candidate is not None:
                out.append(candidate)
    return out


def _find_candidates() -> list[_Candidate]:
    candidates: list[_Candidate] = []
    basepath = minqlxtended.get_cvar("fs_basepath")
    if basepath:
        candidates.extend(_dir_candidates(os.path.join(basepath, "baseq3")))

    workshop = _workshop_root()
    if workshop:
        try:
            items = sorted(os.listdir(workshop))
        except OSError:
            items = []
        for item in items:
            item_dir = os.path.join(workshop, item)
            if os.path.isdir(item_dir):
                candidates.extend(_dir_candidates(item_dir, int(item) if item.isdigit() else None))

    homepath = minqlxtended.get_cvar("fs_homepath")
    if homepath and homepath != basepath:
        candidates.extend(_dir_candidates(os.path.join(homepath, "baseq3")))
    return candidates


def _map_member_name(member: str) -> str | None:
    """The map a zip member spells, or None: "maps/thunderstruck.bsp" -> "thunderstruck"."""
    if member.startswith("maps/") and member.endswith(".bsp"):
        stem = member[len("maps/"):-len(".bsp")]
        if stem and "/" not in stem:
            return stem
    return None


def _info_members(members: dict[str, str], stock: str, suffix: str) -> list[str]:
    """The consolidated declaration first, then the per-map ones in name order."""
    ordered = [name for name in ("scripts/" + stock,) if name in members]
    ordered.extend(sorted(name for name in members if name.endswith(suffix)))
    return ordered


def _read_pk3(candidate: _Candidate) -> _ProviderRecord:
    maps: dict[str, tuple[bool, bool]] = {}
    arenas: dict[str, ArenaInfo] = {}
    factories_found: dict[str, FactoryInfo] = {}
    with zipfile.ZipFile(candidate.path) as zf:
        members = {name.replace("\\", "/").lower(): name for name in zf.namelist()}
        for lowered in members:
            stem = _map_member_name(lowered)
            if stem is not None:
                has_aas = f"maps/{stem}.aas" in members
                has_levelshot = any(
                    f"levelshots/{stem}.{ext}" in members for ext in ("jpg", "tga", "png"))
                maps[stem] = (has_aas, has_levelshot)
        for lowered in _info_members(members, _STOCK_ARENAS, ".arena"):
            member = members[lowered]
            source = f"{candidate.path}:{member}"
            arenas.update(_parse_arena(zf.read(member).decode("utf-8", "replace"), source))
        for lowered in _info_members(members, _STOCK_FACTORIES, ".factories"):
            member = members[lowered]
            source = f"{candidate.path}:{member}"
            factories_found.update(_parse_factories(zf.read(member).decode("utf-8", "replace"), source))
    return _ProviderRecord(candidate.mtime_ns, candidate.size, maps, arenas, factories_found)


def _read_loose(candidate: _Candidate) -> _ProviderRecord:
    with open(candidate.path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if candidate.kind == "arena":
        arenas, factories_found = _parse_arena(text, candidate.path), {}
    else:
        arenas, factories_found = {}, _parse_factories(text, candidate.path)
    return _ProviderRecord(candidate.mtime_ns, candidate.size, _EMPTY_MAPS, arenas, factories_found)


class _MapCache:
    """The module singleton behind every public function. One lock covers the lot:
    a scan holds it, and concurrent callers wait for that scan instead of starting
    their own.
    """

    def __init__(self):
        self.lock = threading.Lock()
        self.records: dict[str, _ProviderRecord] = {}
        self.fingerprint: tuple = ()
        self.last_stat: float | None = None
        self.maps_by_name: dict[str, MapInfo] = {}
        self.factories_by_id: dict[str, FactoryInfo] = {}
        self.warned: set[tuple[str, int]] = set()
        self.entities: collections.OrderedDict[tuple[str, int, str], list[MapEntity]] = collections.OrderedDict()

    def clear(self) -> None:
        with self.lock:
            self.records.clear()
            self.fingerprint = ()
            self.last_stat = None
            self.maps_by_name.clear()
            self.factories_by_id.clear()
            self.warned.clear()
            self.entities.clear()

    # ---- Everything below runs with the lock held. ----

    def ensure_fresh(self, refresh: bool = False) -> None:
        with self.lock:
            now = time.monotonic()
            if not refresh and self.last_stat is not None and now - self.last_stat <= _STAT_TTL:
                return
            self._scan()
            self.last_stat = time.monotonic()

    def _scan(self) -> None:
        candidates = _find_candidates()
        fingerprint = tuple((c.path, c.mtime_ns, c.size) for c in candidates)
        if fingerprint == self.fingerprint:
            return

        for candidate in candidates:
            record = self.records.get(candidate.path)
            if record is not None and (record.mtime_ns, record.size) == (candidate.mtime_ns, candidate.size):
                continue
            try:
                if candidate.kind == "pk3":
                    record = _read_pk3(candidate)
                else:
                    record = _read_loose(candidate)
            except (OSError, zipfile.BadZipFile) as e:
                if (candidate.path, candidate.mtime_ns) not in self.warned:
                    self.warned.add((candidate.path, candidate.mtime_ns))
                    _logger().warning("Skipping unreadable %s: %s", candidate.path, e)
                record = _ProviderRecord(candidate.mtime_ns, candidate.size, _EMPTY_MAPS, {}, {})
            self.records[candidate.path] = record

        current_paths = {c.path for c in candidates}
        for path in [p for p in self.records if p not in current_paths]:
            del self.records[path]

        self._rebuild(candidates)
        self.fingerprint = fingerprint

    def _rebuild(self, candidates: list[_Candidate]) -> None:
        sources_by_map: dict[str, list[MapSource]] = {}
        arenas: dict[str, ArenaInfo] = {}
        factories_by_id: dict[str, FactoryInfo] = {}
        for candidate in candidates:
            record = self.records[candidate.path]
            for name, (has_aas, has_levelshot) in record.maps.items():
                sources_by_map.setdefault(name, []).append(
                    MapSource(candidate.path, candidate.workshop_id, has_aas, has_levelshot))
            arenas.update(record.arenas)
            factories_by_id.update(record.factories)

        maps_by_name = {}
        for name, sources in sources_by_map.items():
            sources.reverse()  # fold order is ascending precedence; the winner goes first
            workshop_id = next((s.workshop_id for s in sources if s.workshop_id is not None), None)
            maps_by_name[name] = MapInfo(
                name=name,
                sources=tuple(sources),
                arena=arenas.get(name),
                workshop_id=workshop_id,
                has_aas=any(s.has_aas for s in sources),
            )
        self.maps_by_name = maps_by_name
        self.factories_by_id = factories_by_id

    def entities_for(self, info: MapInfo) -> list[MapEntity]:
        source = info.sources[0]
        record = self.records.get(source.pk3_path)
        mtime_ns = record.mtime_ns if record is not None else 0
        key = (source.pk3_path, mtime_ns, info.name)
        with self.lock:
            cached = self.entities.get(key)
            if cached is not None:
                self.entities.move_to_end(key)
                return cached
        try:
            with zipfile.ZipFile(source.pk3_path) as zf:
                member = f"maps/{info.name}.bsp"
                for name in zf.namelist():
                    if name.replace("\\", "/").lower() == member:
                        member = name
                        break
                with zf.open(member) as f:
                    text = _read_bsp_entities(f, f"{source.pk3_path}:{member}")
        except (OSError, KeyError, zipfile.BadZipFile) as e:
            _logger().warning("Could not read the BSP for %s from %s: %s", info.name, source.pk3_path, e)
            return []
        parsed = _entities_from_text(text, f"{source.pk3_path}:maps/{info.name}.bsp")
        with self.lock:
            self.entities[key] = parsed
            self.entities.move_to_end(key)
            while len(self.entities) > _ENTITY_CACHE_SIZE:
                self.entities.popitem(last=False)
        return parsed


_cache = _MapCache()


# PUBLIC API


def installed_maps(refresh: bool = False) -> list[MapInfo]:
    """Every map installed on this server, sorted by name.

    Served from the cache; can read from disk when the cache is cold or a pk3 changed,
    so call it from a :func:`minqlxtended.thread` worker in a command handler.

    :param refresh: Rescan the search paths first, even if the cache looks fresh.
    :type refresh: bool
    :returns: list[MapInfo] -- the installed maps.
    """
    _cache.ensure_fresh(refresh)
    with _cache.lock:
        return sorted(_cache.maps_by_name.values(), key=lambda info: info.name)


def map_info(mapname: str, refresh: bool = False) -> MapInfo | None:
    """One installed map's details, or None when no pk3 carries it.

    Case-insensitive. Same I/O caveat as :func:`installed_maps`.

    :param mapname: The short map name, e.g. ``longestyard``.
    :type mapname: str
    :param refresh: Rescan the search paths first, even if the cache looks fresh.
    :type refresh: bool
    :returns: MapInfo | None
    """
    _cache.ensure_fresh(refresh)
    with _cache.lock:
        return _cache.maps_by_name.get(mapname.lower())


def map_worldspawn(mapname: str) -> dict[str, str]:
    """The worldspawn entity's key/value pairs from the map's BSP: ``message``,
    ``music``, ``author`` and whatever else the mapper set.

    Reads and parses the BSP on a cache miss, which is I/O; call it from a
    :func:`minqlxtended.thread` worker. Empty when the map is unknown or unreadable,
    with the reason logged.

    :param mapname: The short map name.
    :type mapname: str
    :returns: dict[str, str] -- the worldspawn keys, lowercased.
    """
    for entity in map_entities(mapname):
        if entity.classname == "worldspawn":
            return dict(entity.keys)
    return {}


def map_entities(mapname: str, classname: str | None = None) -> list[MapEntity]:
    """The map's entities as its BSP declares them, optionally only one classname.

    The map as shipped; :func:`minqlxtended.entities` is what's in the world right now.
    Reads and parses the BSP on a cache miss, so call it from a
    :func:`minqlxtended.thread` worker. Empty when the map is unknown or unreadable.

    :param mapname: The short map name.
    :type mapname: str
    :param classname: Only entities with this classname (case-insensitive), or None for all.
    :type classname: str | None
    :returns: list[MapEntity]
    """
    info = map_info(mapname)
    if info is None:
        return []
    entities = _cache.entities_for(info)
    if classname is not None:
        wanted = classname.lower()
        return [e for e in entities if e.classname.lower() == wanted]
    return list(entities)


def factories(refresh: bool = False) -> list[FactoryInfo]:
    """Every factory the installed declarations carry, sorted by id.

    Same I/O caveat as :func:`installed_maps`.

    :param refresh: Rescan the search paths first, even if the cache looks fresh.
    :type refresh: bool
    :returns: list[FactoryInfo]
    """
    _cache.ensure_fresh(refresh)
    with _cache.lock:
        return sorted(_cache.factories_by_id.values(), key=lambda factory: factory.id)


def factory_info(factory_id: str) -> FactoryInfo | None:
    """One factory's details by its id, or None when nothing installed declares it.

    :param factory_id: The factory id, e.g. ``ca``. Factory ids are case-sensitive.
    :type factory_id: str
    :returns: FactoryInfo | None
    """
    _cache.ensure_fresh()
    with _cache.lock:
        return _cache.factories_by_id.get(factory_id)


def map_supports(mapname: str, gametype: Gametype | str) -> bool | None:
    """Whether the map's arena entry declares support for a gametype.

    None when the map has no arena entry. Most workshop maps play fine in gametypes they
    never declared.

    :param mapname: The short map name.
    :type mapname: str
    :param gametype: A :class:`Gametype` member, or a type token as .arena files spell
        them ("ca", "tourney").
    :type gametype: Gametype | str
    :returns: bool | None
    """
    info = map_info(mapname)
    if info is None or info.arena is None:
        return None
    if isinstance(gametype, Gametype):
        return gametype in info.arena.gametypes
    resolved = _gametype_from_token(gametype)
    if resolved is not None:
        return resolved in info.arena.gametypes
    return gametype.strip().lower() in info.arena.type_tokens


def map_supports_factory(mapname: str, factory_id: str) -> bool | None:
    """Whether the map declares support for a factory's base gametype.

    None when the factory is unknown, its base gametype is unrecognised, or the map has
    no arena entry.

    :param mapname: The short map name.
    :type mapname: str
    :param factory_id: The factory id, e.g. ``ca``.
    :type factory_id: str
    :returns: bool | None
    """
    factory = factory_info(factory_id)
    if factory is None or factory.basegt is None:
        return None
    return map_supports(mapname, factory.basegt)


def refresh_map_cache(background: bool = True) -> None:
    """Rescan the search paths, re-reading any pk3 that changed.

    The queries refresh themselves on a timer, so call this only to pay the I/O somewhere
    convenient, such as after a workshop download.

    :param background: Do the work on a worker thread and return immediately. Pass False
        to do it here and now, on the calling thread.
    :type background: bool
    """
    if background:
        _refresh_worker()
    else:
        _cache.ensure_fresh(refresh=True)


@thread
def _refresh_worker():
    _cache.ensure_fresh(refresh=True)
