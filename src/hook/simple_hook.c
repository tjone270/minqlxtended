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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common.h"
#include "protect.h"
#include "trampoline.h"

// Linux 4.17. Older kernels ignore the flag and treat the address as a hint, so
// reserve_pool_near checks what mmap actually returned instead of trusting it.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define JUMP_SIZE sizeof(JMP_ABS)

// 64 slots is 4096 bytes, one page, and anything smaller rounds up to the same.
#define TRMPS_ARRAY_SIZE 64
#define POOL_SIZE (MEMORY_SLOT_SIZE * TRMPS_ARRAY_SIZE)
const uint8_t NOP = 0x90;

// CreateTrampolineFunction rewrites RIP-relative operands in the relocated prologue, and those
// are 32 bits, so a trampoline more than 2 GB from its target cannot be built. Two pools, since
// the engine and qagame sit far too far apart for one to serve both.
#define POOL_COUNT 2

// Under the 2 GB a 32-bit displacement covers, with room for the distance from the hooked
// function to whatever it reads at the far end of its own module.
#define POOL_REACH ((pint)0x60000000)

// A megabyte apart, out to a gigabyte either way, bounded so a full address space fails
// instead of spinning.
#define POOL_PROBE_STEP ((pint)0x100000)
#define POOL_PROBES 1024

typedef struct {
    pint base; // 0 when the pool holds no mapping
    int next;  // next free slot
} trmp_pool_t;

static trmp_pool_t pools[POOL_COUNT];
static trmp_pool_t* last_pool; // what seek_hook_slot rewinds

static int in_reach(pint pool, pint target) {
    pint low  = pool < target ? pool : target;
    pint high = pool < target ? target : pool;

    return (high - low) < POOL_REACH;
}

static pint reserve_pool_near(pint target) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0;
    }

    pint anchor = target & ~((pint)page_size - 1);

    for (int i = 1; i <= POOL_PROBES; i++) {
        pint step     = (pint)i * POOL_PROBE_STEP;
        pint hints[2] = {anchor + step, anchor - step};

        for (int d = 0; d < 2; d++) {
            if (d && anchor < step) {
                continue; // walking down would wrap past zero
            }

            // Read-execute is the pool's resting state; Hook() makes it writable only while
            // a trampoline is going in.
            void* at = mmap((void*)hints[d], POOL_SIZE, PROT_READ | PROT_EXEC,
                            MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED_NOREPLACE, -1, 0);
            if (at == MAP_FAILED) {
                continue;
            }

            if ((pint)at == hints[d]) {
                return (pint)at;
            }

            munmap(at, POOL_SIZE); // the kernel ignored the flag and placed it elsewhere
        }
    }

    return 0;
}

static trmp_pool_t* pool_for(pint target) {
    for (int i = 0; i < POOL_COUNT; i++) {
        if (pools[i].base && in_reach(pools[i].base, target)) {
            return &pools[i];
        }
    }

    for (int i = 0; i < POOL_COUNT; i++) {
        // Reclaiming is for the map change that maps qagame out of the old pool's reach. A zero
        // cursor means no hook has gone in since HookVm last rewound, which is seen only at the
        // start of the next rebuild with the old module already gone. Don't install a hook
        // mid-map (see the TODO in Hook), or this unmaps live trampolines.
        if (pools[i].base && !pools[i].next) {
            munmap((void*)pools[i].base, POOL_SIZE);
            pools[i].base = 0;
        }

        if (!pools[i].base) {
            pools[i].base = reserve_pool_near(target);
            pools[i].next = 0;
            if (!pools[i].base) {
                return NULL;
            }

            // Anonymous, so a fault inside it backtraces to an address in nothing named.
            DebugPrint("Trampoline pool for %p: %p\n", (void*)target, (void*)pools[i].base);
            return &pools[i];
        }
    }

    return NULL;
}

int Hook(void* target, void* replacement, void** func_ptr) {
    TRAMPOLINE ct;
    int res;

    trmp_pool_t* pool = pool_for((pint)target);
    if (!pool) {
        return -12; // no trampoline pool within reach of the target.
    }

    // TODO: Implement a way to add and remove hooks.
    if (pool->next >= TRMPS_ARRAY_SIZE) {
        return -3;
    }

    void* trmp = (void*)(pool->base + (pint)pool->next * MEMORY_SLOT_SIZE);

    ct.pTarget     = target;
    ct.pDetour     = replacement;
    ct.pTrampoline = trmp;

    // The pool is never both writable and executable. Resealing happens before any failure return
    // below: a pool left unwritable costs one hook, one left unexecutable takes down every
    // trampoline already in it. HookVm rebuilds the qagame pool on a live server, where a Python
    // worker can be part-way into a call through it, so it raises vm_rehooking; see common.h.
    if (mprotect((void*)pool->base, POOL_SIZE, PROT_READ | PROT_WRITE)) {
        return errno;
    }

    int built  = CreateTrampolineFunction(&ct);
    int sealed = mprotect((void*)pool->base, POOL_SIZE, PROT_READ | PROT_EXEC);

    if (!built) {
        return -11;
    }
    if (sealed) {
        return errno;
    }

    // The prologue patch (JMP + NOP padding) writes `difference` bytes from target
    // and may straddle a page boundary, so protect every page the write touches.
    int difference = ct.oldIPs[ct.nIP - 1];

    // CreateTrampolineFunction stops early on a RIP-relative FF /4 jump, an unconditional
    // jump out of the function, or a RET, so it can relocate fewer bytes than the JMP_ABS
    // about to go over the prologue. Writing anyway would point the trampoline's tail jump
    // into the middle of that JMP_ABS, executing an address immediate as code.
    if (difference < (int)JUMP_SIZE) { // cast: JUMP_SIZE is a sizeof, so unsigned
        return -13; // prologue too short to patch safely.
    }

    page_protection_t saved;
    res = MakeWritable((pint)target, (size_t)difference, &saved);
    if (res) {
        return res;
    }

    PJMP_ABS pJmp = (PJMP_ABS)target;
    pJmp->opcode0 = 0xFF;
    pJmp->opcode1 = 0x25;
    pJmp->dummy   = 0;
    pJmp->address = (pint)replacement;

    for (int i = JUMP_SIZE; i < difference; i++) {
        *((uint8_t*)target + i) = NOP;
    }

    // Accounted for before the protection goes back, so an installed hook is recorded as one.
    *func_ptr = trmp;
    pool->next++;
    last_pool = pool;

    return RestoreProtection(&saved);
}

int seek_hook_slot(int offset) {
    if (!last_pool) {
        return 0;
    }

    // The upper bound is off by one for a positive offset landing on the last slot, which
    // is harmless: HookVm is the only caller and only ever rewinds.
    if ((last_pool->next + offset < 0) || (last_pool->next + offset >= TRMPS_ARRAY_SIZE)) {
        return 0;
    }

    last_pool->next += offset;
    return 1;
}
