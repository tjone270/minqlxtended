/*
 *  MinHook - The Minimalistic API Hooking Library for x64/x86
 *  Copyright (C) 2009-2017 Tsuda Kageyu.
 *  All rights reserved.
 *
 *  Modifications copyright (C) 2026 Thomas Jones <me@thomasjones.id.au>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *  TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *  PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
 *  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(A) (sizeof(A) / sizeof((A)[0]))
#endif

// The 32-bit decoder went with the 32-bit target. Quake Live's dedicated server is x86-64
// only, and both common.h and the Makefile refuse anything else.
#if !defined(_M_X64) && !defined(__x86_64__)
#error minqlxtended is x86-64 only
#endif

#include "HDE/hde64.h"
typedef hde64s HDE;
#define HDE_DISASM(code, hs) hde64_disasm(code, hs)

#include "trampoline.h"

//-------------------------------------------------------------------------
BOOL CreateTrampolineFunction(PTRAMPOLINE ct) {
#if defined(_M_X64) || defined(__x86_64__)
    CALL_ABS call = {
        0xFF, 0x15, 0x00000002, // FF15 00000002: CALL [RIP+8]
        0xEB, 0x08,             // EB 08:         JMP +10
        0x0000000000000000ULL   // Absolute destination address
    };
    JMP_ABS jmp = {
        0xFF, 0x25, 0x00000000, // FF25 00000000: JMP [RIP+6]
        0x0000000000000000ULL   // Absolute destination address
    };
    JCC_ABS jcc = {
        0x70, 0x0E,             // 7* 0E:         J** +16
        0xFF, 0x25, 0x00000000, // FF25 00000000: JMP [RIP+6]
        0x0000000000000000ULL   // Absolute destination address
    };
#else
    CALL_REL call = {
        0xE8,      // E8 xxxxxxxx: CALL +5+xxxxxxxx
        0x00000000 // Relative destination address
    };
    JMP_REL jmp = {
        0xE9,      // E9 xxxxxxxx: JMP +5+xxxxxxxx
        0x00000000 // Relative destination address
    };
    JCC_REL jcc = {
        0x0F, 0x80, // 0F8* xxxxxxxx: J** +6+xxxxxxxx
        0x00000000  // Relative destination address
    };
#endif

    UINT8 oldPos      = 0;
    UINT8 newPos      = 0;
    ULONG_PTR jmpDest = 0;     // Destination address of an internal jump.
    BOOL finished     = FALSE; // Is the function completed?
#if defined(_M_X64) || defined(__x86_64__)
    UINT8 instBuf[16];
#endif

    ct->nIP = 0;

    do {
        HDE hs;
        UINT copySize;
        LPVOID pCopySrc;
        ULONG_PTR pOldInst = (ULONG_PTR)ct->pTarget + oldPos;
        ULONG_PTR pNewInst = (ULONG_PTR)ct->pTrampoline + newPos;

        copySize = HDE_DISASM((LPVOID)pOldInst, &hs);
        if (hs.flags & F_ERROR) {
            return FALSE;
        }

        pCopySrc = (LPVOID)pOldInst;
        if (oldPos >= sizeof(jmp)) {
            // The trampoline function is long enough.
            // Complete the function with the jump to the target function.
#if defined(_M_X64) || defined(__x86_64__)
            jmp.address = (UINT64)pOldInst;
#else
            jmp.operand = (UINT32)(pOldInst - (pNewInst + sizeof(jmp)));
#endif
            pCopySrc = &jmp;
            copySize = sizeof(jmp);

            finished = TRUE;
        }
#if defined(_M_X64) || defined(__x86_64__)
        else if ((hs.modrm & 0xC7) == 0x05) {
            // Instructions using RIP relative addressing. (ModR/M = 00???101B)

            // Modify the RIP relative address.
            PUINT32 pRelAddr;

            // Avoid using memcpy to reduce the footprint.
#ifndef _MSC_VER
            memcpy(instBuf, (LPBYTE)pOldInst, copySize);
#else
            __movsb(instBuf, (LPBYTE)pOldInst, copySize);
#endif
            pCopySrc = instBuf;

            // Relative address is stored at (instruction length - immediate value length - 4).
            pRelAddr = (PUINT32)(instBuf + hs.len - ((hs.flags & 0x3C) >> 2) - 4);

            // The relocated instruction still has to reach the datum it read in the original,
            // over a 32-bit displacement, and simple_hook.c's allocator can fail to place the
            // pool within 2 GB. Check instead of truncating.
            INT64 delta = (pOldInst + hs.len + (INT32)hs.disp.disp32) - (pNewInst + hs.len);
            if (delta < INT32_MIN || delta > INT32_MAX) {
                return FALSE;
            }

            *pRelAddr = (UINT32)delta;

            // Complete the function if JMP (FF /4).
            if (hs.opcode == 0xFF && hs.modrm_reg == 4) {
                finished = TRUE;
            }
        }
#endif
        else if (hs.opcode == 0xE8) {
            // Direct relative CALL
            ULONG_PTR dest = pOldInst + hs.len + (INT32)hs.imm.imm32;
#if defined(_M_X64) || defined(__x86_64__)
            call.address = (UINT64)dest;
#else
            call.operand = (UINT32)(dest - (pNewInst + sizeof(call)));
#endif
            pCopySrc = &call;
            copySize = sizeof(call);
        } else if ((hs.opcode & 0xFD) == 0xE9) {
            // Direct relative JMP (EB or E9)
            ULONG_PTR dest = pOldInst + hs.len;

            if (hs.opcode == 0xEB) { // isShort jmp
                dest += (INT8)hs.imm.imm8;
            } else {
                dest += (INT32)hs.imm.imm32;
            }

            /*
             * Simply copy an internal jump. The window is sizeof(jmp), since it has to cover
             * the bytes Hook() is about to destroy and Hook() stamps a whole JMP_ABS where
             * upstream stamps a five-byte relative one. The bytes between sizeof(jmp) and
             * `difference` are interior bytes of the one instruction straddling the boundary.
             */
            if ((ULONG_PTR)ct->pTarget <= dest && dest < ((ULONG_PTR)ct->pTarget + sizeof(jmp))) {
                if (jmpDest < dest) {
                    jmpDest = dest;
                }
            } else {
#if defined(_M_X64) || defined(__x86_64__)
                jmp.address = (UINT64)dest;
#else
                jmp.operand = (UINT32)(dest - (pNewInst + sizeof(jmp)));
#endif
                pCopySrc = &jmp;
                copySize = sizeof(jmp);

                // Exit the function If it is not in the branch
                finished = (pOldInst >= jmpDest);
            }
        } else if ((hs.opcode & 0xF0) == 0x70 || (hs.opcode & 0xFC) == 0xE0 || (hs.opcode2 & 0xF0) == 0x80) {
            // Direct relative Jcc
            ULONG_PTR dest = pOldInst + hs.len;

            if ((hs.opcode & 0xF0) == 0x70       // Jcc
                || (hs.opcode & 0xFC) == 0xE0) { // LOOPNZ/LOOPZ/LOOP/JECXZ
                dest += (INT8)hs.imm.imm8;
            } else {
                dest += (INT32)hs.imm.imm32;
            }

            // Simply copy an internal jump.
            if ((ULONG_PTR)ct->pTarget <= dest && dest < ((ULONG_PTR)ct->pTarget + sizeof(jmp))) {
                if (jmpDest < dest) {
                    jmpDest = dest;
                }
            } else if ((hs.opcode & 0xFC) == 0xE0) {
                // LOOPNZ/LOOPZ/LOOP/JCXZ/JECXZ to the outside are not supported.
                return FALSE;
            } else {
                UINT8 cond = ((hs.opcode != 0x0F ? hs.opcode : hs.opcode2) & 0x0F);
#if defined(_M_X64) || defined(__x86_64__)
                // Invert the condition in x64 mode to simplify the conditional jump logic.
                jcc.opcode  = 0x71 ^ cond;
                jcc.address = (UINT64)dest;
#else
                jcc.opcode1 = 0x80 | cond;
                jcc.operand = (UINT32)(dest - (pNewInst + sizeof(jcc)));
#endif
                pCopySrc = &jcc;
                copySize = sizeof(jcc);
            }
        } else if ((hs.opcode & 0xFE) == 0xC2) {
            // RET (C2 or C3)

            // Complete the function if not in a branch.
            finished = (pOldInst >= jmpDest);
        }

        // Can't alter the instruction length in a branch.
        if (pOldInst < jmpDest && copySize != hs.len) {
            return FALSE;
        }

        // Trampoline function is too large.
        if ((newPos + copySize) > TRAMPOLINE_MAX_SIZE) {
            return FALSE;
        }

        // Trampoline function has too many instructions.
        if (ct->nIP >= ARRAYSIZE(ct->oldIPs)) {
            return FALSE;
        }

        ct->oldIPs[ct->nIP] = oldPos;
        ct->newIPs[ct->nIP] = newPos;
        ct->nIP++;

        // Avoid using memcpy to reduce the footprint.
#ifndef _MSC_VER
        memcpy((LPBYTE)ct->pTrampoline + newPos, pCopySrc, copySize);
#else
        __movsb((LPBYTE)ct->pTrampoline + newPos, pCopySrc, copySize);
#endif
        newPos += copySize;
        oldPos += hs.len;
    } while (!finished);

    return TRUE;
}
