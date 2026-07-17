#include "trampoline.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(_M_X64)
typedef uint64_t pint;
typedef int64_t sint;
// Must be >= the MEMORY_SLOT_SIZE used by trampoline.c (relocated body up to
// TRAMPOLINE_MAX_SIZE plus a JMP_ABS relay), else adjacent slots overlap.
#define WORST_CASE 64
#define JUMP_SIZE sizeof(JMP_ABS)
#elif defined(__i386) || defined(_M_IX86)
typedef uint32_t pint;
typedef int32_t sint;
#define WORST_CASE 32
#define JUMP_SIZE sizeof(JMP_REL)
#endif

#define TRMPS_ARRAY_SIZE 30
const uint8_t NOP = 0x90;

static void* trmps;
static int last_trmp = 0; // trmp[TRMPS_ARRAY_SIZE]

static void initializeTrampolines(void) {
    trmps = mmap(NULL, (WORST_CASE * TRMPS_ARRAY_SIZE),
                 PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (trmps == MAP_FAILED) {
        trmps = NULL; // Keep the !trmps guard meaningful and allow a later retry.
    }
}

int Hook(void* target, void* replacement, void** func_ptr) {
    TRAMPOLINE ct;
    int res, page_size;

    // Check if our trampoline pool has been initialized. If not, do so.
    if (!trmps) {
        initializeTrampolines();
        if (!trmps) {
            return -12; // mmap failed; cannot allocate trampoline pool.
        }
    } else { // TODO: Implement a way to add and remove hooks.
        if (last_trmp + 1 > TRMPS_ARRAY_SIZE) {
            return -3;
        }
    }

    void* trmp = (void*)((pint)trmps + last_trmp * WORST_CASE);

    ct.pTarget     = target;
    ct.pDetour     = replacement;
    ct.pTrampoline = trmp;

    if (!CreateTrampolineFunction(&ct)) {
        return -11;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size == -1) {
        return errno;
    }
    // The prologue patch (JMP + NOP padding) writes `difference` bytes from target
    // and may straddle a page boundary, so protect every page the write touches.
    int difference     = ct.oldIPs[ct.nIP - 1];
    pint protect_start = (pint)target & ~(pint)(page_size - 1);
    pint protect_end   = ((pint)target + difference + page_size - 1) & ~(pint)(page_size - 1);
    res = mprotect((void*)protect_start, protect_end - protect_start, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (res) {
        return errno;
    }

#if defined(__x86_64__) || defined(_M_X64)
    PJMP_ABS pJmp = (PJMP_ABS)target;
    pJmp->opcode0 = 0xFF;
    pJmp->opcode1 = 0x25;
    pJmp->dummy   = 0;
    pJmp->address = (pint)replacement;
#else
    PJMP_REL pJmp = (PJMP_REL)target;
    pJmp->opcode  = 0xE9;
    pJmp->operand = (pint)replacement - ((pint)target + sizeof(JMP_REL));
#endif

    for (int i = JUMP_SIZE; i < difference; i++) {
        *((uint8_t*)target + i) = NOP;
    }

    *func_ptr = trmp;

    last_trmp++;
    return 0;
}

int seek_hook_slot(int offset) {

    if ((last_trmp + offset < 0) || (last_trmp + offset >= TRMPS_ARRAY_SIZE)) {
        return 0;
    }

    last_trmp += offset;
    return 1;
}
