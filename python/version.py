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

"""Prints the version string that make stamps into MINQLXTENDED_VERSION.

    python3 python/version.py        # release
    python3 python/version.py -d     # debug

Set MINQLXTENDED_VERSION in the environment to override it entirely.
"""

import os
import sys

from subprocess import CalledProcessError, check_output, DEVNULL


def print_usage():
    print(f"Usage: {sys.argv[0]} [-d]")


def describe():
    try:
        version = check_output(
            ["git", "describe", "--long", "--tags", "--dirty", "--always"],
            stderr=DEVNULL).decode().strip()
        branch = check_output(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            stderr=DEVNULL).decode().strip()
    except (OSError, CalledProcessError):
        return None, None

    return version, branch


def main(argv):
    if len(argv) > 1 or (len(argv) == 1 and argv[0] != "-d"):
        print_usage()
        return 1

    debug = argv == ["-d"]

    override = os.environ.get("MINQLXTENDED_VERSION")
    if override:
        print(override)
        return 0

    version, branch = describe()
    if version is None:
        print("NOT_SET_debug" if debug else "NOT_SET")
    elif debug:
        print(f"{version}_debug-{branch}")
    else:
        print(f"{version}-{branch}")

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
