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


"""A cache for the engine's configstring table.

Every configstring write goes through the hooked ``SV_SetConfigstring``, which dispatches
into Python before the engine's own copy is updated, so a cached entry is authoritative.

**Undispatched indices are never cached.** ``CS_BOTINFO`` and the ``CS_ROUND_START_TIME``
block are rewritten every frame, so the hook skips them and reads of those always hit the
engine. See ``_UNDISPATCHED`` below.

**Parsed results are shared.** ``configstring_variables`` hands back a read-only view of
the dict it keeps. Call ``update_configstring_variables`` to change a key.
"""

from __future__ import annotations

import types
from typing import Mapping

import minqlxtended

from _minqlxtended import get_configstring as _get_configstring

__all__ = (
    "apply_variable_changes",
    "configstring",
    "configstring_variables",
    "player_configstring",
    "player_configstring_variables",
    "update_configstring_variables",
    "update_player_configstring_variables",
)

_UNDISPATCHED = frozenset([minqlxtended.CS_BOTINFO]) | frozenset(
    range(minqlxtended.CS_ROUND_START_TIME, minqlxtended.CS_PAUSE_END_TIME)
)

_TRUNCATION_SAFE_CHARS = 1023
_TRUNCATION_LIMIT_BYTES = 4095

# index -> raw string. Absent means "not read yet"; present means authoritative.
_raw: dict[int, str] = {}
# index -> parsed dict, built on first variables() call.
_parsed: dict[int, Mapping[str, str]] = {}

def configstring(index: int, cached: bool = True) -> str:
    """The configstring at `index`.

    :param index: The configstring index.
    :type index: int
    :param cached: pass False to bypass the cache and hit the engine, which you want
        before a read-modify-write. See the module docstring.
    :returns: str
    """
    if not cached or index in _UNDISPATCHED:
        value = _get_configstring(index)
        if index not in _UNDISPATCHED:
            _note(index, value)
        return value

    try:
        return _raw[index]
    except KeyError:
        value = _get_configstring(index)
        _raw[index] = value
        return value


def configstring_variables(index: int, cached: bool = True) -> Mapping[str, str]:
    """The configstring at `index`, parsed as an infostring.

    The mapping is read-only and shared with every other reader, so wrap it in ``dict()``
    if you want something you can change.

    :returns: A read-only mapping of the variables.
    """
    value = configstring(index, cached=cached)
    if index in _UNDISPATCHED:
        return types.MappingProxyType(minqlxtended.parse_infostring(value))
    try:
        return _parsed[index]
    except KeyError:
        # Cache the view itself. Nearly every Game property access lands here, and callers
        # rely on the same parse handing back the same object.
        parsed = types.MappingProxyType(minqlxtended.parse_infostring(value))
        _parsed[index] = parsed
        return parsed


def apply_variable_changes(variables: Mapping[str, str],
                           changes: Mapping[str, str | None],
                           ) -> tuple[dict[str, str], bool]:
    """Merge `changes` into `variables`, returning the result and whether anything moved.

    Works on a copy, so a cached parse is safe to pass in.

    :param variables: The variables to start from. Not modified.
    :param changes: Keys to set. A value of None removes the key.
    :type changes: dict
    :returns: (dict, bool) -- the merged variables, and True if anything changed.
    """
    merged = dict(variables)
    dirty = False
    for key, value in changes.items():
        if value is None:
            if key in merged:
                del merged[key]
                dirty = True
        else:
            value = str(value)
            if merged.get(key) != value:
                merged[key] = value
                dirty = True

    return merged, dirty


def update_configstring_variables(index: int, changes: Mapping[str, str | None]) -> bool:
    """Apply `changes` to the infostring at `index` and write it back.

    Re-reads the engine first, so a key another writer changed since the cache was filled
    survives.

    :param changes: Keys to set. A value of None removes the key.
    :type changes: dict
    :returns: bool -- True if a write was issued, False if nothing changed.
    """
    current = _get_configstring(index)
    _note(index, current)

    variables, dirty = apply_variable_changes(minqlxtended.parse_infostring(current), changes)
    if not dirty:
        return False

    minqlxtended.set_configstring(index, minqlxtended.format_infostring(variables))
    return True


def player_configstring(client_id: int, cached: bool = True) -> str:
    """The configstring for a player slot."""
    return configstring(minqlxtended.CS_PLAYERS + client_id, cached=cached)


def player_configstring_variables(client_id: int, cached: bool = True) -> Mapping[str, str]:
    """The parsed configstring for a player slot. Read-only; see
    :func:`configstring_variables`."""
    return configstring_variables(minqlxtended.CS_PLAYERS + client_id, cached=cached)


def update_player_configstring_variables(
        client_id: int, changes: Mapping[str, str | None]) -> bool:
    """Read-modify-write a player's configstring."""
    return update_configstring_variables(minqlxtended.CS_PLAYERS + client_id, changes)


# CACHE MAINTENANCE


def _note(index: int, value: str) -> None:
    """Record the value now in the engine. The only writer into the cache."""
    if index in _UNDISPATCHED:
        return
    if _raw.get(index) != value:
        _raw[index] = value
        _parsed.pop(index, None)


def note_configstring(index: int, value: str, may_be_truncated: bool = False) -> None:
    """Called from `handle_set_configstring` once the dispatchers have had their
    say, with the value that is going through to the engine.

    :param may_be_truncated: True when a handler replaced the value, in which
        case the engine may have written a shortened copy of it. Drop the entry
        then, so the next read goes to the engine.
    """
    if (may_be_truncated and len(value) > _TRUNCATION_SAFE_CHARS
            and len(value.encode("utf-8", "ignore")) > _TRUNCATION_LIMIT_BYTES):
        invalidate_configstring(index)
        return
    _note(index, value)


def invalidate_configstrings() -> None:
    """Drop everything. `SV_SpawnServer` wipes the configstring table on every
    map load, so the cache has to go with it."""
    _raw.clear()
    _parsed.clear()


def invalidate_configstring(index: int) -> None:
    """Drop a single entry, when you have reason to believe a write bypassed the
    dispatcher."""
    _raw.pop(index, None)
    _parsed.pop(index, None)


