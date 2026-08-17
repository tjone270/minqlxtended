/*
Copyright (C) 2015 Mino <mino@minomino.org>
Copyright (C) 2022-2026 Thomas Jones <me@thomasjones.id.au>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef PATCHES_H
#define PATCHES_H

void patch_vm(void);

#define VM_SEARCH_START ((pint)qagame + 0xB000)
#define VM_SEARCH_LENGTH 0xB0000

#define STR_BAD_USERINFO_KICK "Dropping client %i for a deformed userinfo: %s\n"
#define STR_BAD_USERINFO_KICK_FIX "Dropping client with deformed userinfo: %s\n"
#define STR_BAD_USERINFO_KEEP "Found client %i with deformed userinfo, but cowardly refusing to kick: %s\n"
#define STR_BAD_USERINFO_KEEP_FIX "Found client with deformed userinfo, but cowardly refusing to kick: %s\n"

// Cmd_CallVote_f becomes the trampoline once hooked.
#define ADDR_VOTE_CLIENTKICK_FIX (Cmd_CallVote_f_addr + 0x11C8)
#define PTRN_VOTE_CLIENTKICK_FIX "\x39\xFE\x0F\x8D\x90\x00\x00\x00\x48\x69\xD6\xF8\x0B\x00\x00\x48\x01\xD0\x90\x90\x90\x0\x0\x0\x0\x0\x0\x0\x0f\x85\x76\x00\x00\x00\x90\x90\x90\x90"
#define MASK_VOTE_CLIENTKICK_FIX "XXXXXXXXXXXXXXXXXXXXX-------XXXXXXXXXX"

// The site's pre-patch bytes, read out of qagamex64.so.
#define PRE_VOTE_CLIENTKICK_FIX                                                                    \
    "\x31\xd2\xeb\x09\x83\xc2\x01\x48\x05\xf8\x0b\x00\x00\x39\xfa\x0f\x8d\x83\x00\x00\x00\x83\xb8" \
    "\x50\x02\x00\x00\x02\x75\xe6\x3b\xb0\x88\x00\x00\x00\x75\xde"

#endif /* PATCHES_H */
