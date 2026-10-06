// crt_fdiv.cpp -- M3 stage LIBC, group B: the Pentium FDIV workaround (adj_fdiv.obj, testfdiv.obj) and the stack probe
// (chkstk.obj), rewritten.
//
// race.exe was compiled with /QIfdiv: every floating-point divide in the game's own code (482 sites) is
//     cmp dword ptr [__adjust_fdiv], 0 ; jne safe ; fdiv ...      safe: push operand; call __adj_fdiv_m32 (or
//     mov eax, code; call __adj_fdiv_r for a register divide)
// and __adjust_fdiv (0x5024b8) is set once, at start-up, by _fpmath (fpinit.obj, deferred) from __ms_p5_mp_test_fdiv:
// 1 only on a Pentium with the 1994 FDIV flaw (it divides 4195835 by 3145727 on every processor the process may run
// on). On every processor since it is 0, so the __adj_fdiv_* routines are never called in the game -- the inline
// fdiv runs -- except __adj_fpatan and __adj_fptan, which every fpatan / fptan site calls unconditionally (Car /
// AICar::apply_yaw_control, Tire::GetResultant, the camera, the replay jog control). __adj_fpatan is 3 bytes
// (fpatan; ret) with __adj_fptan right behind it, too short to hook: it stays original. The rewrites of the game's
// own functions treat the divides as plain divides (docs/PORTING.md rule 5); the original code still branches on
// __adjust_fdiv.
//
// The routines are Microsoft's hand-written assembly: they take their operands on the x87 stack (and __adj_fdiv_r an
// encoded instruction in eax: the register in bits 3..5, the form in bits 0..2), divide through fdiv_main_routine,
// which scales both operands by 15/16 (or 17/16) when the divisor's top mantissa bits hit one of the flawed table
// entries (0x5024d0) and fixes up denormal operands, and preserve every register but eax. So they are rewritten as
// naked assembly, instruction for instruction (generated from the disassembly, with the x87 register arithmetic
// emitted as bytes -- `fdivp st(1)` is DE F9 -- so no assembler can pick the other operand order), their constants
// read at their v1.0 addresses (0x5024d0 .. 0x50252d), their calls to each other by v1.0 address (through a pointer
// in memory: no register is free), and __adj_fdiv_r's 64-entry jump table (0x50252e, which holds the original's own
// code addresses) replaced by this file's table of the 64 rewritten entries. Each `int 6` (the forms the compiler
// never emits) is kept.
//
// None of them can be shadow-checked (the wrapper is C: it would disturb eax and the x87 stack), so their shadow
// entry is the rewrite itself (CRT_NO_SHADOW below: in shadow mode they simply run new). test/world_crt_io.cpp checks
// each against its original offline with __adjust_fdiv's routes forced: random and edge operands (the flawed divisor
// patterns, denormals, zeros, infinities, NaNs, the fprem paths that scale by 2^64), every eax code of __adj_fdiv_r
// with a full stack, under each precision and rounding control, comparing all eight registers, the tag word, the
// status word, the control word and the stack pointer.
//
// __chkstk (0x4cf3d0, the stack probe every function with more than a page of locals calls with the size in eax) is
// the same kind of routine: also naked, also new-only. (It has no entry of its own in the inventory: the map lists it
// as data.)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include "port.h"
#include "crt_types.h"

namespace crt_fdiv {   // (file-local helpers; one namespace per file so a harness can include all four)

#define IAT(slot, fn) (*(decltype(&fn) volatile*)(uintptr_t)(slot))
#define kLoadLibraryA   IAT(0x005d758c, LoadLibraryA)
#define kGetProcAddress IAT(0x005d75a0, GetProcAddress)

// A naked rewrite with an x87-stack or register calling convention: never run through PORT_FN's C shadow wrapper
// (in shadow mode its entry is the rewrite). A harness that registers the rewrites itself defines this as nothing.
#ifndef CRT_NO_SHADOW
#ifdef VP_FUZZ
#define CRT_NO_SHADOW(NEW)
#else
#define CRT_NO_SHADOW(NEW) \
    namespace { const bool VP_CAT(noshadow_, NEW) = (VP_CAT(port_, NEW).shadow = VP_CAT(port_, NEW).repl, true); }
#endif
#endif

// the routines' v1.0 entries, called through these (so a hooked rewrite is what runs)
static const uint32_t g_fdiv_main_routine = 0x004ce1e0, g_fdivp_sti_st = 0x004ce796, g_fdivrp_sti_st = 0x004ce7a9,
                      g_fprem_common = 0x004ce9e6, g_fprem1_common = 0x004cec9e, g_adj_fprem = 0x004cebec,
                      g_adj_fprem1 = 0x004ceea4, g_ms_p5_test_fdiv = 0x004d0840;

// ---- fdiv_main_routine (0x4ce1e0)
static __declspec(naked) void c_fdiv_main_routine() {
#ifdef VP_GCC
    __asm__ volatile(
        "fld tbyte ptr [esp + 0x10]\n\t"
        "fld tbyte ptr [esp + 4]\n\t"
        "L4ce1e8%=:\n\t"
        "mov eax, dword ptr [esp + 8]\n\t"
        "%{load%} add eax, eax\n\t"
        "jae L4ce27a%=\n\t"
        "xor eax, 0xe000000\n\t"
        "test eax, 0xe000000\n\t"
        "je L4ce203%=\n\t"
        ".byte 0xde\n\t"
        ".byte 0xf9\n\t"
        "ret\n\t"
        "L4ce203%=:\n\t"
        "shr eax, 0x1c\n\t"
        "cmp byte ptr [eax + 0x5024d0], 0\n\t"
        "jne L4ce212%=\n\t"
        ".byte 0xde\n\t"
        ".byte 0xf9\n\t"
        "ret\n\t"
        "L4ce212%=:\n\t"
        "mov eax, dword ptr [esp + 0xc]\n\t"
        "and eax, 0x7fff\n\t"
        "je L4ce284%=\n\t"
        "cmp eax, 0x7fff\n\t"
        "je L4ce284%=\n\t"
        "fnstcw word ptr [esp + 0x1c]\n\t"
        "mov eax, dword ptr [esp + 0x1c]\n\t"
        "or eax, 0x33f\n\t"
        "and eax, 0xf3ff\n\t"
        "mov dword ptr [esp + 0x20], eax\n\t"
        "fldcw word ptr [esp + 0x20]\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "and eax, 0x7fff\n\t"
        "cmp eax, 1\n\t"
        "je L4ce263%=\n\t"
        "fmul dword ptr ds:[0x5024e0]\n\t"
        "fxch st(1)\n\t"
        "fmul dword ptr ds:[0x5024e0]\n\t"
        "fxch st(1)\n\t"
        "fldcw word ptr [esp + 0x1c]\n\t"
        ".byte 0xde\n\t"
        ".byte 0xf9\n\t"
        "ret\n\t"
        "L4ce263%=:\n\t"
        "fmul dword ptr ds:[0x5024e4]\n\t"
        "fxch st(1)\n\t"
        "fmul dword ptr ds:[0x5024e4]\n\t"
        "fxch st(1)\n\t"
        "fldcw word ptr [esp + 0x1c]\n\t"
        ".byte 0xde\n\t"
        ".byte 0xf9\n\t"
        "ret\n\t"
        "L4ce27a%=:\n\t"
        "mov eax, dword ptr [esp + 4]\n\t"
        "or eax, dword ptr [esp + 8]\n\t"
        "jne L4ce287%=\n\t"
        "L4ce284%=:\n\t"
        ".byte 0xde\n\t"
        ".byte 0xf9\n\t"
        "ret\n\t"
        "L4ce287%=:\n\t"
        "mov eax, dword ptr [esp + 0xc]\n\t"
        "and eax, 0x7fff\n\t"
        "jne L4ce284%=\n\t"
        "fnstcw word ptr [esp + 0x1c]\n\t"
        "mov eax, dword ptr [esp + 0x1c]\n\t"
        "or eax, 0x33f\n\t"
        "and eax, 0xf3ff\n\t"
        "mov dword ptr [esp + 0x20], eax\n\t"
        "fldcw word ptr [esp + 0x20]\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "and eax, 0x7fff\n\t"
        "je L4ce2c8%=\n\t"
        "cmp eax, 0x7fff\n\t"
        "je L4ce2f0%=\n\t"
        "mov eax, dword ptr [esp + 0x14]\n\t"
        "%{load%} add eax, eax\n\t"
        "jae L4ce2f0%=\n\t"
        "jmp L4ce2d0%=\n\t"
        "L4ce2c8%=:\n\t"
        "mov eax, dword ptr [esp + 0x14]\n\t"
        "%{load%} add eax, eax\n\t"
        "jb L4ce2f0%=\n\t"
        "L4ce2d0%=:\n\t"
        "fxch st(1)\n\t"
        "fstp st(0)\n\t"
        "fld st(0)\n\t"
        "fmul dword ptr ds:[0x5024e8]\n\t"
        "fstp tbyte ptr [esp + 4]\n\t"
        "fld tbyte ptr [esp + 0x10]\n\t"
        "fxch st(1)\n\t"
        "fwait\n\t"
        "fldcw word ptr [esp + 0x1c]\n\t"
        "jmp L4ce1e8%=\n\t"
        "L4ce2f0%=:\n\t"
        "fldcw word ptr [esp + 0x1c]\n\t"
        ".byte 0xde\n\t"
        ".byte 0xf9\n\t"
        "ret"
        : :);
#else
    __asm {
        fld tbyte ptr [esp + 0x10]
        fld tbyte ptr [esp + 4]
L4ce1e8:
        mov eax, dword ptr [esp + 8]
        add eax, eax
        jae L4ce27a
        xor eax, 0xe000000
        test eax, 0xe000000
        je L4ce203
        _emit 0xde __asm _emit 0xf9   ; fdivp st(1)
        ret
L4ce203:
        shr eax, 0x1c
        cmp byte ptr [eax + 0x5024d0], 0
        jne L4ce212
        _emit 0xde __asm _emit 0xf9   ; fdivp st(1)
        ret
L4ce212:
        mov eax, dword ptr [esp + 0xc]
        and eax, 0x7fff
        je L4ce284
        cmp eax, 0x7fff
        je L4ce284
        fnstcw word ptr [esp + 0x1c]
        mov eax, dword ptr [esp + 0x1c]
        or eax, 0x33f
        and eax, 0xf3ff
        mov dword ptr [esp + 0x20], eax
        fldcw word ptr [esp + 0x20]
        mov eax, dword ptr [esp + 0x18]
        and eax, 0x7fff
        cmp eax, 1
        je L4ce263
        fmul dword ptr ds:[0x5024e0]
        fxch st(1)
        fmul dword ptr ds:[0x5024e0]
        fxch st(1)
        fldcw word ptr [esp + 0x1c]
        _emit 0xde __asm _emit 0xf9   ; fdivp st(1)
        ret
L4ce263:
        fmul dword ptr ds:[0x5024e4]
        fxch st(1)
        fmul dword ptr ds:[0x5024e4]
        fxch st(1)
        fldcw word ptr [esp + 0x1c]
        _emit 0xde __asm _emit 0xf9   ; fdivp st(1)
        ret
L4ce27a:
        mov eax, dword ptr [esp + 4]
        or eax, dword ptr [esp + 8]
        jne L4ce287
L4ce284:
        _emit 0xde __asm _emit 0xf9   ; fdivp st(1)
        ret
L4ce287:
        mov eax, dword ptr [esp + 0xc]
        and eax, 0x7fff
        jne L4ce284
        fnstcw word ptr [esp + 0x1c]
        mov eax, dword ptr [esp + 0x1c]
        or eax, 0x33f
        and eax, 0xf3ff
        mov dword ptr [esp + 0x20], eax
        fldcw word ptr [esp + 0x20]
        mov eax, dword ptr [esp + 0x18]
        and eax, 0x7fff
        je L4ce2c8
        cmp eax, 0x7fff
        je L4ce2f0
        mov eax, dword ptr [esp + 0x14]
        add eax, eax
        jae L4ce2f0
        jmp L4ce2d0
L4ce2c8:
        mov eax, dword ptr [esp + 0x14]
        add eax, eax
        jb L4ce2f0
L4ce2d0:
        fxch st(1)
        fstp st(0)
        fld st(0)
        fmul dword ptr ds:[0x5024e8]
        fstp tbyte ptr [esp + 4]
        fld tbyte ptr [esp + 0x10]
        fxch st(1)
        fwait
        fldcw word ptr [esp + 0x1c]
        jmp L4ce1e8
L4ce2f0:
        fldcw word ptr [esp + 0x1c]
        _emit 0xde __asm _emit 0xf9   ; fdivp st(1)
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h00() {    // st(0), op 0 (0x4ce304)
#ifdef VP_GCC
    __asm__ volatile(
        ".byte 0xd8\n\t"
        ".byte 0xf0\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : :);
#else
    __asm {
        _emit 0xd8 __asm _emit 0xf0   ; fdiv st(0)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h01() {    // st(0), op 1 (0x4ce30a)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h02() {    // st(0), op 2 (0x4ce30f)
#ifdef VP_GCC
    __asm__ volatile(
        ".byte 0xd8\n\t"
        ".byte 0xf8\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : :);
#else
    __asm {
        _emit 0xd8 __asm _emit 0xf8   ; fdivr st(0)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h03() {    // st(0), op 3 (0x4ce315)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h04() {    // st(0), op 4 (0x4ce31a)
#ifdef VP_GCC
    __asm__ volatile(
        ".byte 0xd8\n\t"
        ".byte 0xf0\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : :);
#else
    __asm {
        _emit 0xd8 __asm _emit 0xf0   ; fdiv st(0)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h05() {    // st(0), op 5 (0x4ce320)
#ifdef VP_GCC
    __asm__ volatile(
        ".byte 0xde\n\t"
        ".byte 0xf8\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : :);
#else
    __asm {
        _emit 0xde __asm _emit 0xf8   ; fdivp st(0)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h06() {    // st(0), op 6 (0x4ce326)
#ifdef VP_GCC
    __asm__ volatile(
        ".byte 0xd8\n\t"
        ".byte 0xf8\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : :);
#else
    __asm {
        _emit 0xd8 __asm _emit 0xf8   ; fdivr st(0)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h07() {    // st(0), op 7 (0x4ce32c)
#ifdef VP_GCC
    __asm__ volatile(
        ".byte 0xde\n\t"
        ".byte 0xf0\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : :);
#else
    __asm {
        _emit 0xde __asm _emit 0xf0   ; fdivrp st(0)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h08() {    // st(1), op 0 (0x4ce332)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fld tbyte ptr [esp + 0x20]
        fxch st(1)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h09() {    // st(1), op 1 (0x4ce34e)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h10() {    // st(1), op 2 (0x4ce353)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fld tbyte ptr [esp + 0xc]
        fxch st(1)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h11() {    // st(1), op 3 (0x4ce369)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h12() {    // st(1), op 4 (0x4ce36e)
#ifdef VP_GCC
    __asm__ volatile(
        "fxch st(1)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fxch st(1)
        fstp tbyte ptr [esp + 0xc]
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fld tbyte ptr [esp + 0x20]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h13() {    // st(1), op 5 (0x4ce38a)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h14() {    // st(1), op 6 (0x4ce39a)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fld tbyte ptr [esp + 0xc]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h15() {    // st(1), op 7 (0x4ce3ae)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h16() {    // st(2), op 0 (0x4ce3be)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(1)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(1)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "fxch st(2)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(1)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(1)
        fld tbyte ptr [esp + 0x20]
        fxch st(2)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h17() {    // st(2), op 1 (0x4ce3de)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h18() {    // st(2), op 2 (0x4ce3e3)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(1)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "fxch st(2)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(1)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(1)
        fld tbyte ptr [esp + 0xc]
        fxch st(2)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h19() {    // st(2), op 3 (0x4ce3fd)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h20() {    // st(2), op 4 (0x4ce402)
#ifdef VP_GCC
    __asm__ volatile(
        "fxch st(2)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(1)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(1)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fxch st(2)
        fstp tbyte ptr [esp + 0xc]
        fxch st(1)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(1)
        fld tbyte ptr [esp + 0x20]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h21() {    // st(2), op 5 (0x4ce422)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(1)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(1)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h22() {    // st(2), op 6 (0x4ce436)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(1)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(1)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(1)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(1)
        fld tbyte ptr [esp + 0xc]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h23() {    // st(2), op 7 (0x4ce44e)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(1)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(1)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(1)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h24() {    // st(3), op 0 (0x4ce462)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(2)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(2)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "fxch st(3)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(2)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(2)
        fld tbyte ptr [esp + 0x20]
        fxch st(3)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h25() {    // st(3), op 1 (0x4ce482)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h26() {    // st(3), op 2 (0x4ce487)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(2)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(2)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "fxch st(3)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(2)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(2)
        fld tbyte ptr [esp + 0xc]
        fxch st(3)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h27() {    // st(3), op 3 (0x4ce4a1)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h28() {    // st(3), op 4 (0x4ce4a6)
#ifdef VP_GCC
    __asm__ volatile(
        "fxch st(3)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(2)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(2)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fxch st(3)
        fstp tbyte ptr [esp + 0xc]
        fxch st(2)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(2)
        fld tbyte ptr [esp + 0x20]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h29() {    // st(3), op 5 (0x4ce4c6)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(2)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(2)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(2)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(2)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h30() {    // st(3), op 6 (0x4ce4da)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(2)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(2)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(2)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(2)
        fld tbyte ptr [esp + 0xc]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h31() {    // st(3), op 7 (0x4ce4f2)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(2)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(2)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(2)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(2)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h32() {    // st(4), op 0 (0x4ce506)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(3)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(3)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "fxch st(4)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(3)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(3)
        fld tbyte ptr [esp + 0x20]
        fxch st(4)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h33() {    // st(4), op 1 (0x4ce526)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h34() {    // st(4), op 2 (0x4ce52b)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(3)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(3)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "fxch st(4)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(3)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(3)
        fld tbyte ptr [esp + 0xc]
        fxch st(4)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h35() {    // st(4), op 3 (0x4ce545)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h36() {    // st(4), op 4 (0x4ce54a)
#ifdef VP_GCC
    __asm__ volatile(
        "fxch st(4)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(3)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(3)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fxch st(4)
        fstp tbyte ptr [esp + 0xc]
        fxch st(3)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(3)
        fld tbyte ptr [esp + 0x20]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h37() {    // st(4), op 5 (0x4ce56a)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(3)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(3)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(3)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(3)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h38() {    // st(4), op 6 (0x4ce57e)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(3)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(3)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(3)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(3)
        fld tbyte ptr [esp + 0xc]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h39() {    // st(4), op 7 (0x4ce596)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(3)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(3)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(3)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(3)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h40() {    // st(5), op 0 (0x4ce5aa)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(4)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(4)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "fxch st(5)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(4)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(4)
        fld tbyte ptr [esp + 0x20]
        fxch st(5)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h41() {    // st(5), op 1 (0x4ce5ca)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h42() {    // st(5), op 2 (0x4ce5cf)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(4)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(4)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "fxch st(5)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(4)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(4)
        fld tbyte ptr [esp + 0xc]
        fxch st(5)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h43() {    // st(5), op 3 (0x4ce5e9)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h44() {    // st(5), op 4 (0x4ce5ee)
#ifdef VP_GCC
    __asm__ volatile(
        "fxch st(5)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(4)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(4)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fxch st(5)
        fstp tbyte ptr [esp + 0xc]
        fxch st(4)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(4)
        fld tbyte ptr [esp + 0x20]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h45() {    // st(5), op 5 (0x4ce60e)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(4)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(4)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(4)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(4)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h46() {    // st(5), op 6 (0x4ce622)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(4)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(4)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(4)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(4)
        fld tbyte ptr [esp + 0xc]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h47() {    // st(5), op 7 (0x4ce63a)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(4)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(4)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(4)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(4)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h48() {    // st(6), op 0 (0x4ce64e)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(5)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(5)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "fxch st(6)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(5)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(5)
        fld tbyte ptr [esp + 0x20]
        fxch st(6)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h49() {    // st(6), op 1 (0x4ce66e)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h50() {    // st(6), op 2 (0x4ce673)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(5)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(5)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "fxch st(6)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(5)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(5)
        fld tbyte ptr [esp + 0xc]
        fxch st(6)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h51() {    // st(6), op 3 (0x4ce68d)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h52() {    // st(6), op 4 (0x4ce692)
#ifdef VP_GCC
    __asm__ volatile(
        "fxch st(6)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(5)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(5)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fxch st(6)
        fstp tbyte ptr [esp + 0xc]
        fxch st(5)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(5)
        fld tbyte ptr [esp + 0x20]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h53() {    // st(6), op 5 (0x4ce6b2)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(5)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(5)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(5)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(5)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h54() {    // st(6), op 6 (0x4ce6c6)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(5)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(5)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(5)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(5)
        fld tbyte ptr [esp + 0xc]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h55() {    // st(6), op 7 (0x4ce6de)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(5)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(5)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(5)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(5)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h56() {    // st(7), op 0 (0x4ce6f2)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(6)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(6)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "fxch st(7)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(6)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(6)
        fld tbyte ptr [esp + 0x20]
        fxch st(7)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h57() {    // st(7), op 1 (0x4ce712)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h58() {    // st(7), op 2 (0x4ce717)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(6)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(6)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "fxch st(7)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(6)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(6)
        fld tbyte ptr [esp + 0xc]
        fxch st(7)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h59() {    // st(7), op 3 (0x4ce731)
#ifdef VP_GCC
    __asm__ volatile(
        "add esp, 0x2c\n\t"
        "int 6"
        : :);
#else
    __asm {
        add esp, 0x2c
        int 6
    }
#endif
}
static __declspec(naked) void fdivr_h60() {    // st(7), op 4 (0x4ce736)
#ifdef VP_GCC
    __asm__ volatile(
        "fxch st(7)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(6)\n\t"
        "fld st(0)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0x20]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(6)\n\t"
        "fld tbyte ptr [esp + 0x20]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fxch st(7)
        fstp tbyte ptr [esp + 0xc]
        fxch st(6)
        fld st(0)
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0x20]
        call dword ptr [g_fdiv_main_routine]
        fxch st(6)
        fld tbyte ptr [esp + 0x20]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h61() {    // st(7), op 5 (0x4ce756)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp]\n\t"
        "fxch st(6)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(6)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp]
        fxch st(6)
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        fxch st(6)
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h62() {    // st(7), op 6 (0x4ce76a)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(6)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(6)\n\t"
        "fld tbyte ptr [esp + 0xc]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(6)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(6)
        fld tbyte ptr [esp + 0xc]
        add esp, 0x2c
        ret
    }
#endif
}
static __declspec(naked) void fdivr_h63() {    // st(7), op 7 (0x4ce782)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fxch st(6)\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "fxch st(6)\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        fstp tbyte ptr [esp + 0xc]
        fxch st(6)
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        fxch st(6)
        add esp, 0x2c
        ret
    }
#endif
}
// ---- fdivp_sti_st (0x4ce796)
static __declspec(naked) void c_fdivp_sti_st() {
#ifdef VP_GCC
    __asm__ volatile(
        "sub esp, 0x2c\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        sub esp, 0x2c
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        add esp, 0x2c
        ret
    }
#endif
}
// ---- fdivrp_sti_st (0x4ce7a9)
static __declspec(naked) void c_fdivrp_sti_st() {
#ifdef VP_GCC
    __asm__ volatile(
        "sub esp, 0x2c\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0x2c\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        sub esp, 0x2c
        fstp tbyte ptr [esp + 0xc]
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        add esp, 0x2c
        ret
    }
#endif
}
// ---- adj_fdiv_m32 (0x4ce7bc)
static __declspec(naked) void c_adj_fdiv_m32() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "mov eax, dword ptr [esp + 8]\n\t"
        "and eax, 0x7f800000\n\t"
        "cmp eax, 0x7f800000\n\t"
        "je L4ce800%=\n\t"
        "fnstsw ax\n\t"
        "and eax, 0x3800\n\t"
        "je L4ce7e3%=\n\t"
        "fld dword ptr [esp + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "pop eax\n\t"
        "ret 4\n\t"
        "L4ce7e3%=:\n\t"
        "fxch st(1)\n\t"
        "sub esp, 0xc\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fld dword ptr [esp + 0x14]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0xc\n\t"
        "pop eax\n\t"
        "ret 4\n\t"
        "L4ce800%=:\n\t"
        "fdiv dword ptr [esp + 8]\n\t"
        "pop eax\n\t"
        "ret 4"
        : : "i"(&g_fdivp_sti_st));
#else
    __asm {
        push eax
        mov eax, dword ptr [esp + 8]
        and eax, 0x7f800000
        cmp eax, 0x7f800000
        je L4ce800
        fnstsw ax
        and eax, 0x3800
        je L4ce7e3
        fld dword ptr [esp + 8]
        call dword ptr [g_fdivp_sti_st]
        pop eax
        ret 4
L4ce7e3:
        fxch st(1)
        sub esp, 0xc
        fstp tbyte ptr [esp]
        fld dword ptr [esp + 0x14]
        call dword ptr [g_fdivp_sti_st]
        fld tbyte ptr [esp]
        fxch st(1)
        add esp, 0xc
        pop eax
        ret 4
L4ce800:
        fdiv dword ptr [esp + 8]
        pop eax
        ret 4
    }
#endif
}
// ---- adj_fdiv_m64 (0x4ce808)
static __declspec(naked) void c_adj_fdiv_m64() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "mov eax, dword ptr [esp + 0xc]\n\t"
        "and eax, 0x7ff00000\n\t"
        "cmp eax, 0x7ff00000\n\t"
        "je L4ce84c%=\n\t"
        "fnstsw ax\n\t"
        "and eax, 0x3800\n\t"
        "je L4ce82f%=\n\t"
        "fld qword ptr [esp + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "pop eax\n\t"
        "ret 8\n\t"
        "L4ce82f%=:\n\t"
        "fxch st(1)\n\t"
        "sub esp, 0xc\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fld qword ptr [esp + 0x14]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0xc\n\t"
        "pop eax\n\t"
        "ret 8\n\t"
        "L4ce84c%=:\n\t"
        "fdiv qword ptr [esp + 8]\n\t"
        "pop eax\n\t"
        "ret 8"
        : : "i"(&g_fdivp_sti_st));
#else
    __asm {
        push eax
        mov eax, dword ptr [esp + 0xc]
        and eax, 0x7ff00000
        cmp eax, 0x7ff00000
        je L4ce84c
        fnstsw ax
        and eax, 0x3800
        je L4ce82f
        fld qword ptr [esp + 8]
        call dword ptr [g_fdivp_sti_st]
        pop eax
        ret 8
L4ce82f:
        fxch st(1)
        sub esp, 0xc
        fstp tbyte ptr [esp]
        fld qword ptr [esp + 0x14]
        call dword ptr [g_fdivp_sti_st]
        fld tbyte ptr [esp]
        fxch st(1)
        add esp, 0xc
        pop eax
        ret 8
L4ce84c:
        fdiv qword ptr [esp + 8]
        pop eax
        ret 8
    }
#endif
}
// ---- adj_fdiv_m16i (0x4ce854)
static __declspec(naked) void c_adj_fdiv_m16i() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "fnstsw ax\n\t"
        "and eax, 0x3800\n\t"
        "je L4ce86b%=\n\t"
        "fild word ptr [esp + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "pop eax\n\t"
        "ret 4\n\t"
        "L4ce86b%=:\n\t"
        "fxch st(1)\n\t"
        "sub esp, 0xc\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fild word ptr [esp + 0x14]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0xc\n\t"
        "pop eax\n\t"
        "ret 4"
        : : "i"(&g_fdivp_sti_st));
#else
    __asm {
        push eax
        fnstsw ax
        and eax, 0x3800
        je L4ce86b
        fild word ptr [esp + 8]
        call dword ptr [g_fdivp_sti_st]
        pop eax
        ret 4
L4ce86b:
        fxch st(1)
        sub esp, 0xc
        fstp tbyte ptr [esp]
        fild word ptr [esp + 0x14]
        call dword ptr [g_fdivp_sti_st]
        fld tbyte ptr [esp]
        fxch st(1)
        add esp, 0xc
        pop eax
        ret 4
    }
#endif
}
// ---- adj_fdiv_m32i (0x4ce888)
static __declspec(naked) void c_adj_fdiv_m32i() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "fnstsw ax\n\t"
        "and eax, 0x3800\n\t"
        "je L4ce89f%=\n\t"
        "fild dword ptr [esp + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "pop eax\n\t"
        "ret 4\n\t"
        "L4ce89f%=:\n\t"
        "fxch st(1)\n\t"
        "sub esp, 0xc\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fild dword ptr [esp + 0x14]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0xc\n\t"
        "pop eax\n\t"
        "ret 4"
        : : "i"(&g_fdivp_sti_st));
#else
    __asm {
        push eax
        fnstsw ax
        and eax, 0x3800
        je L4ce89f
        fild dword ptr [esp + 8]
        call dword ptr [g_fdivp_sti_st]
        pop eax
        ret 4
L4ce89f:
        fxch st(1)
        sub esp, 0xc
        fstp tbyte ptr [esp]
        fild dword ptr [esp + 0x14]
        call dword ptr [g_fdivp_sti_st]
        fld tbyte ptr [esp]
        fxch st(1)
        add esp, 0xc
        pop eax
        ret 4
    }
#endif
}
// ---- adj_fdivr_m32 (0x4ce8bc)
static __declspec(naked) void c_adj_fdivr_m32() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "mov eax, dword ptr [esp + 8]\n\t"
        "and eax, 0x7f800000\n\t"
        "cmp eax, 0x7f800000\n\t"
        "je L4ce900%=\n\t"
        "fnstsw ax\n\t"
        "and eax, 0x3800\n\t"
        "je L4ce8e3%=\n\t"
        "fld dword ptr [esp + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "pop eax\n\t"
        "ret 4\n\t"
        "L4ce8e3%=:\n\t"
        "fxch st(1)\n\t"
        "sub esp, 0xc\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fld dword ptr [esp + 0x14]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0xc\n\t"
        "pop eax\n\t"
        "ret 4\n\t"
        "L4ce900%=:\n\t"
        "fdivr dword ptr [esp + 8]\n\t"
        "pop eax\n\t"
        "ret 4"
        : : "i"(&g_fdivrp_sti_st));
#else
    __asm {
        push eax
        mov eax, dword ptr [esp + 8]
        and eax, 0x7f800000
        cmp eax, 0x7f800000
        je L4ce900
        fnstsw ax
        and eax, 0x3800
        je L4ce8e3
        fld dword ptr [esp + 8]
        call dword ptr [g_fdivrp_sti_st]
        pop eax
        ret 4
L4ce8e3:
        fxch st(1)
        sub esp, 0xc
        fstp tbyte ptr [esp]
        fld dword ptr [esp + 0x14]
        call dword ptr [g_fdivrp_sti_st]
        fld tbyte ptr [esp]
        fxch st(1)
        add esp, 0xc
        pop eax
        ret 4
L4ce900:
        fdivr dword ptr [esp + 8]
        pop eax
        ret 4
    }
#endif
}
// ---- adj_fdivr_m64 (0x4ce908)
static __declspec(naked) void c_adj_fdivr_m64() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "mov eax, dword ptr [esp + 0xc]\n\t"
        "and eax, 0x7ff00000\n\t"
        "cmp eax, 0x7ff00000\n\t"
        "je L4ce94c%=\n\t"
        "fnstsw ax\n\t"
        "and eax, 0x3800\n\t"
        "je L4ce92f%=\n\t"
        "fld qword ptr [esp + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "pop eax\n\t"
        "ret 8\n\t"
        "L4ce92f%=:\n\t"
        "fxch st(1)\n\t"
        "sub esp, 0xc\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fld qword ptr [esp + 0x14]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0xc\n\t"
        "pop eax\n\t"
        "ret 8\n\t"
        "L4ce94c%=:\n\t"
        "fdivr qword ptr [esp + 8]\n\t"
        "pop eax\n\t"
        "ret 8"
        : : "i"(&g_fdivrp_sti_st));
#else
    __asm {
        push eax
        mov eax, dword ptr [esp + 0xc]
        and eax, 0x7ff00000
        cmp eax, 0x7ff00000
        je L4ce94c
        fnstsw ax
        and eax, 0x3800
        je L4ce92f
        fld qword ptr [esp + 8]
        call dword ptr [g_fdivrp_sti_st]
        pop eax
        ret 8
L4ce92f:
        fxch st(1)
        sub esp, 0xc
        fstp tbyte ptr [esp]
        fld qword ptr [esp + 0x14]
        call dword ptr [g_fdivrp_sti_st]
        fld tbyte ptr [esp]
        fxch st(1)
        add esp, 0xc
        pop eax
        ret 8
L4ce94c:
        fdivr qword ptr [esp + 8]
        pop eax
        ret 8
    }
#endif
}
// ---- adj_fdivr_m16i (0x4ce954)
static __declspec(naked) void c_adj_fdivr_m16i() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "fnstsw ax\n\t"
        "and eax, 0x3800\n\t"
        "je L4ce96b%=\n\t"
        "fild word ptr [esp + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "pop eax\n\t"
        "ret 4\n\t"
        "L4ce96b%=:\n\t"
        "fxch st(1)\n\t"
        "sub esp, 0xc\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fild word ptr [esp + 0x14]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0xc\n\t"
        "pop eax\n\t"
        "ret 4"
        : : "i"(&g_fdivrp_sti_st));
#else
    __asm {
        push eax
        fnstsw ax
        and eax, 0x3800
        je L4ce96b
        fild word ptr [esp + 8]
        call dword ptr [g_fdivrp_sti_st]
        pop eax
        ret 4
L4ce96b:
        fxch st(1)
        sub esp, 0xc
        fstp tbyte ptr [esp]
        fild word ptr [esp + 0x14]
        call dword ptr [g_fdivrp_sti_st]
        fld tbyte ptr [esp]
        fxch st(1)
        add esp, 0xc
        pop eax
        ret 4
    }
#endif
}
// ---- adj_fdivr_m32i (0x4ce988)
static __declspec(naked) void c_adj_fdivr_m32i() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "fnstsw ax\n\t"
        "and eax, 0x3800\n\t"
        "je L4ce99f%=\n\t"
        "fild dword ptr [esp + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "pop eax\n\t"
        "ret 4\n\t"
        "L4ce99f%=:\n\t"
        "fxch st(1)\n\t"
        "sub esp, 0xc\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fild dword ptr [esp + 0x14]\n\t"
        "call dword ptr [%c0]\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "add esp, 0xc\n\t"
        "pop eax\n\t"
        "ret 4"
        : : "i"(&g_fdivrp_sti_st));
#else
    __asm {
        push eax
        fnstsw ax
        and eax, 0x3800
        je L4ce99f
        fild dword ptr [esp + 8]
        call dword ptr [g_fdivrp_sti_st]
        pop eax
        ret 4
L4ce99f:
        fxch st(1)
        sub esp, 0xc
        fstp tbyte ptr [esp]
        fild dword ptr [esp + 0x14]
        call dword ptr [g_fdivrp_sti_st]
        fld tbyte ptr [esp]
        fxch st(1)
        add esp, 0xc
        pop eax
        ret 4
    }
#endif
}
// ---- safe_fdiv (0x4ce9bc)
static __declspec(naked) void c_safe_fdiv() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "sub esp, 0x2c\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0x2c\n\t"
        "pop eax\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        push eax
        sub esp, 0x2c
        fstp tbyte ptr [esp]
        fstp tbyte ptr [esp + 0xc]
        call dword ptr [g_fdiv_main_routine]
        add esp, 0x2c
        pop eax
        ret
    }
#endif
}
// ---- safe_fdivr (0x4ce9d1)
static __declspec(naked) void c_safe_fdivr() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "sub esp, 0x2c\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0x2c\n\t"
        "pop eax\n\t"
        "ret"
        : : "i"(&g_fdiv_main_routine));
#else
    __asm {
        push eax
        sub esp, 0x2c
        fstp tbyte ptr [esp + 0xc]
        fstp tbyte ptr [esp]
        call dword ptr [g_fdiv_main_routine]
        add esp, 0x2c
        pop eax
        ret
    }
#endif
}
// ---- fprem_common (0x4ce9e6)
static __declspec(naked) void c_fprem_common() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "push ebx\n\t"
        "push ecx\n\t"
        "mov eax, dword ptr [esp + 0x16]\n\t"
        "xor eax, 0x700\n\t"
        "test eax, 0x700\n\t"
        "jne L4ceb80%=\n\t"
        "shr eax, 0xb\n\t"
        "and eax, 0xf\n\t"
        "cmp byte ptr [eax + 0x5024ec], 0\n\t"
        "je L4ceb80%=\n\t"
        "mov eax, dword ptr [esp + 0x16]\n\t"
        "and eax, 0x7fff0000\n\t"
        "cmp eax, 0x7fff0000\n\t"
        "je L4ceb80%=\n\t"
        "mov eax, dword ptr [esp + 0x2e]\n\t"
        "and eax, 0x7fff0000\n\t"
        "je L4ceb80%=\n\t"
        "cmp eax, 0x7fff0000\n\t"
        "je L4ceb80%=\n\t"
        "mov eax, dword ptr [esp + 0x2c]\n\t"
        "%{load%} add eax, eax\n\t"
        "jne L4ceb80%=\n\t"
        "mov eax, dword ptr [esp + 0x14]\n\t"
        "%{load%} add eax, eax\n\t"
        "jne L4ceb80%=\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "and eax, 0x7fff\n\t"
        "add eax, 0x3f\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "and ebx, 0x7fff\n\t"
        "%{load%} sub ebx, eax\n\t"
        "ja L4ceace%=\n\t"
        "L4cea70%=:\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "and eax, 0x7fff\n\t"
        "add eax, 0xa\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "and ebx, 0x7fff\n\t"
        "%{load%} sub ebx, eax\n\t"
        "js L4ceb80%=\n\t"
        "fld tbyte ptr [esp + 0x28]\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "and ebx, 0x7fff\n\t"
        "%{load%} mov ecx, ebx\n\t"
        "%{load%} sub ebx, eax\n\t"
        "and ebx, 7\n\t"
        "or ebx, 4\n\t"
        "%{load%} sub ecx, ebx\n\t"
        "%{load%} mov ebx, eax\n\t"
        "and ebx, 0x8000\n\t"
        "%{load%} or ecx, ebx\n\t"
        "mov dword ptr [esp + 0x18], ecx\n\t"
        "fld tbyte ptr [esp + 0x10]\n\t"
        "mov dword ptr [esp + 0x18], eax\n\t"
        "fxch st(1)\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xf8\n\t"
        "fstp tbyte ptr [esp + 0x28]\n\t"
        "fstp st(0)\n\t"
        "jmp L4cea70%=\n\t"
        "L4ceace%=:\n\t"
        "test edx, 2\n\t"
        "jne L4ceade%=\n\t"
        "fld tbyte ptr [esp + 0x10]\n\t"
        "fstp tbyte ptr [esp + 0x1c]\n\t"
        "L4ceade%=:\n\t"
        "fnstcw word ptr [esp + 0x34]\n\t"
        "mov eax, dword ptr [esp + 0x34]\n\t"
        "or eax, 0x33f\n\t"
        "mov dword ptr [esp + 0x38], eax\n\t"
        "fldcw word ptr [esp + 0x38]\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "and eax, 0x7fff\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "and ebx, 0x7fff\n\t"
        "%{load%} sub ebx, eax\n\t"
        "and ebx, 0x3f\n\t"
        "or ebx, 0x20\n\t"
        "add ebx, 1\n\t"
        "%{load%} mov ecx, ebx\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "and ebx, 0x7fff\n\t"
        "and eax, 0x8000\n\t"
        "%{load%} or ebx, eax\n\t"
        "mov dword ptr [esp + 0x18], ebx\n\t"
        "fld tbyte ptr [esp + 0x10]\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xe1\n\t"
        "fld tbyte ptr [esp + 0x28]\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xe1\n\t"
        "L4ceb38%=:\n\t"
        ".byte 0xd8\n\t"
        ".byte 0xd1\n\t"
        "fnstsw ax\n\t"
        "and eax, 0x100\n\t"
        "jne L4ceb45%=\n\t"
        ".byte 0xd8\n\t"
        ".byte 0xe1\n\t"
        "L4ceb45%=:\n\t"
        "fxch st(1)\n\t"
        "fmul qword ptr ds:[0x50251c]\n\t"
        "fxch st(1)\n\t"
        "sub ecx, 1\n\t"
        "jne L4ceb38%=\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "fstp tbyte ptr [esp + 0x28]\n\t"
        "fstp st(0)\n\t"
        "fld tbyte ptr [esp + 0x1c]\n\t"
        "fld tbyte ptr ds:[0x502524]\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xf8\n\t"
        "fstp st(0)\n\t"
        "fld tbyte ptr [esp + 0x28]\n\t"
        "fldcw word ptr [esp + 0x34]\n\t"
        "and ebx, 0x8000\n\t"
        "je L4ceb8a%=\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xe0\n\t"
        "jmp L4ceb8a%=\n\t"
        "L4ceb80%=:\n\t"
        "fld tbyte ptr [esp + 0x10]\n\t"
        "fld tbyte ptr [esp + 0x28]\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xf8\n\t"
        "L4ceb8a%=:\n\t"
        "test edx, 3\n\t"
        "je L4cebe8%=\n\t"
        "fnstsw word ptr [esp + 0x3c]\n\t"
        "test edx, 1\n\t"
        "je L4cebbd%=\n\t"
        "fnstcw word ptr [esp + 0x34]\n\t"
        "mov eax, dword ptr [esp + 0x34]\n\t"
        "or eax, 0x300\n\t"
        "mov dword ptr [esp + 0x38], eax\n\t"
        "fldcw word ptr [esp + 0x38]\n\t"
        "fmul qword ptr ds:[0x50250c]\n\t"
        "fldcw word ptr [esp + 0x34]\n\t"
        "L4cebbd%=:\n\t"
        "mov eax, dword ptr [esp + 0x3c]\n\t"
        "fxch st(1)\n\t"
        "fstp st(0)\n\t"
        "fld tbyte ptr [esp + 0x1c]\n\t"
        "fxch st(1)\n\t"
        "and eax, 0x4300\n\t"
        "sub esp, 0x1c\n\t"
        "fnstenv [esp]\n\t"
        "and dword ptr [esp + 4], 0xbcff\n\t"
        "or dword ptr [esp + 4], eax\n\t"
        "fldenv [esp]\n\t"
        "add esp, 0x1c\n\t"
        "L4cebe8%=:\n\t"
        "pop ecx\n\t"
        "pop ebx\n\t"
        "pop eax\n\t"
        "ret"
        : :);
#else
    __asm {
        push eax
        push ebx
        push ecx
        mov eax, dword ptr [esp + 0x16]
        xor eax, 0x700
        test eax, 0x700
        jne L4ceb80
        shr eax, 0xb
        and eax, 0xf
        cmp byte ptr [eax + 0x5024ec], 0
        je L4ceb80
        mov eax, dword ptr [esp + 0x16]
        and eax, 0x7fff0000
        cmp eax, 0x7fff0000
        je L4ceb80
        mov eax, dword ptr [esp + 0x2e]
        and eax, 0x7fff0000
        je L4ceb80
        cmp eax, 0x7fff0000
        je L4ceb80
        mov eax, dword ptr [esp + 0x2c]
        add eax, eax
        jne L4ceb80
        mov eax, dword ptr [esp + 0x14]
        add eax, eax
        jne L4ceb80
        mov eax, dword ptr [esp + 0x18]
        and eax, 0x7fff
        add eax, 0x3f
        mov ebx, dword ptr [esp + 0x30]
        and ebx, 0x7fff
        sub ebx, eax
        ja L4ceace
L4cea70:
        mov eax, dword ptr [esp + 0x18]
        and eax, 0x7fff
        add eax, 0xa
        mov ebx, dword ptr [esp + 0x30]
        and ebx, 0x7fff
        sub ebx, eax
        js L4ceb80
        fld tbyte ptr [esp + 0x28]
        mov eax, dword ptr [esp + 0x18]
        mov ebx, dword ptr [esp + 0x30]
        and ebx, 0x7fff
        mov ecx, ebx
        sub ebx, eax
        and ebx, 7
        or ebx, 4
        sub ecx, ebx
        mov ebx, eax
        and ebx, 0x8000
        or ecx, ebx
        mov dword ptr [esp + 0x18], ecx
        fld tbyte ptr [esp + 0x10]
        mov dword ptr [esp + 0x18], eax
        fxch st(1)
        _emit 0xd9 __asm _emit 0xf8   ; fprem
        fstp tbyte ptr [esp + 0x28]
        fstp st(0)
        jmp L4cea70
L4ceace:
        test edx, 2
        jne L4ceade
        fld tbyte ptr [esp + 0x10]
        fstp tbyte ptr [esp + 0x1c]
L4ceade:
        fnstcw word ptr [esp + 0x34]
        mov eax, dword ptr [esp + 0x34]
        or eax, 0x33f
        mov dword ptr [esp + 0x38], eax
        fldcw word ptr [esp + 0x38]
        mov eax, dword ptr [esp + 0x18]
        and eax, 0x7fff
        mov ebx, dword ptr [esp + 0x30]
        and ebx, 0x7fff
        sub ebx, eax
        and ebx, 0x3f
        or ebx, 0x20
        add ebx, 1
        mov ecx, ebx
        mov eax, dword ptr [esp + 0x18]
        mov ebx, dword ptr [esp + 0x30]
        and ebx, 0x7fff
        and eax, 0x8000
        or ebx, eax
        mov dword ptr [esp + 0x18], ebx
        fld tbyte ptr [esp + 0x10]
        _emit 0xd9 __asm _emit 0xe1   ; fabs
        fld tbyte ptr [esp + 0x28]
        _emit 0xd9 __asm _emit 0xe1   ; fabs
L4ceb38:
        _emit 0xd8 __asm _emit 0xd1   ; fcom st(1)
        fnstsw ax
        and eax, 0x100
        jne L4ceb45
        _emit 0xd8 __asm _emit 0xe1   ; fsub st(1)
L4ceb45:
        fxch st(1)
        fmul qword ptr ds:[0x50251c]
        fxch st(1)
        sub ecx, 1
        jne L4ceb38
        mov ebx, dword ptr [esp + 0x30]
        fstp tbyte ptr [esp + 0x28]
        fstp st(0)
        fld tbyte ptr [esp + 0x1c]
        fld tbyte ptr ds:[0x502524]
        _emit 0xd9 __asm _emit 0xf8   ; fprem
        fstp st(0)
        fld tbyte ptr [esp + 0x28]
        fldcw word ptr [esp + 0x34]
        and ebx, 0x8000
        je L4ceb8a
        _emit 0xd9 __asm _emit 0xe0   ; fchs
        jmp L4ceb8a
L4ceb80:
        fld tbyte ptr [esp + 0x10]
        fld tbyte ptr [esp + 0x28]
        _emit 0xd9 __asm _emit 0xf8   ; fprem
L4ceb8a:
        test edx, 3
        je L4cebe8
        fnstsw word ptr [esp + 0x3c]
        test edx, 1
        je L4cebbd
        fnstcw word ptr [esp + 0x34]
        mov eax, dword ptr [esp + 0x34]
        or eax, 0x300
        mov dword ptr [esp + 0x38], eax
        fldcw word ptr [esp + 0x38]
        fmul qword ptr ds:[0x50250c]
        fldcw word ptr [esp + 0x34]
L4cebbd:
        mov eax, dword ptr [esp + 0x3c]
        fxch st(1)
        fstp st(0)
        fld tbyte ptr [esp + 0x1c]
        fxch st(1)
        and eax, 0x4300
        sub esp, 0x1c
        fnstenv [esp]
        and dword ptr [esp + 4], 0xbcff
        or dword ptr [esp + 4], eax
        fldenv [esp]
        add esp, 0x1c
L4cebe8:
        pop ecx
        pop ebx
        pop eax
        ret
    }
#endif
}
// ---- adj_fprem (0x4cebec)
static __declspec(naked) void c_adj_fprem() {
#ifdef VP_GCC
    __asm__ volatile(
        "push edx\n\t"
        "sub esp, 0x30\n\t"
        "fstp tbyte ptr [esp + 0x18]\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "%{load%} xor edx, edx\n\t"
        "mov eax, dword ptr [esp + 6]\n\t"
        "test eax, 0x7fff0000\n\t"
        "je L4cec0e%=\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0x30\n\t"
        "pop edx\n\t"
        "ret\n\t"
        "L4cec0e%=:\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fld tbyte ptr [esp + 0x18]\n\t"
        "mov eax, dword ptr [esp]\n\t"
        "or eax, dword ptr [esp + 4]\n\t"
        "je L4cec97%=\n\t"
        "fxch st(1)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "or edx, 2\n\t"
        "fnstcw word ptr [esp + 0x24]\n\t"
        "mov eax, dword ptr [esp + 0x24]\n\t"
        "or eax, 0x33f\n\t"
        "mov dword ptr [esp + 0x28], eax\n\t"
        "fldcw word ptr [esp + 0x28]\n\t"
        "mov eax, dword ptr [esp + 0x20]\n\t"
        "and eax, 0x7fff\n\t"
        "cmp eax, 0x7fbe\n\t"
        "ja L4cec69%=\n\t"
        "or edx, 1\n\t"
        "fmul qword ptr ds:[0x502504]\n\t"
        "fstp tbyte ptr [esp + 0x18]\n\t"
        "fmul qword ptr ds:[0x502504]\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "jmp L4cec89%=\n\t"
        "L4cec69%=:\n\t"
        "fnstcw word ptr [esp + 0x24]\n\t"
        "mov eax, dword ptr [esp + 0x24]\n\t"
        "or eax, 0x300\n\t"
        "mov dword ptr [esp + 0x28], eax\n\t"
        "fldcw word ptr [esp + 0x28]\n\t"
        "fstp st(0)\n\t"
        "fmul qword ptr ds:[0x502504]\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "L4cec89%=:\n\t"
        "fldcw word ptr [esp + 0x24]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0x30\n\t"
        "pop edx\n\t"
        "ret\n\t"
        "L4cec97%=:\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xf8\n\t"
        "add esp, 0x30\n\t"
        "pop edx\n\t"
        "ret"
        : : "i"(&g_fprem_common));
#else
    __asm {
        push edx
        sub esp, 0x30
        fstp tbyte ptr [esp + 0x18]
        fstp tbyte ptr [esp]
        xor edx, edx
        mov eax, dword ptr [esp + 6]
        test eax, 0x7fff0000
        je L4cec0e
        call dword ptr [g_fprem_common]
        add esp, 0x30
        pop edx
        ret
L4cec0e:
        fld tbyte ptr [esp]
        fld tbyte ptr [esp + 0x18]
        mov eax, dword ptr [esp]
        or eax, dword ptr [esp + 4]
        je L4cec97
        fxch st(1)
        fstp tbyte ptr [esp + 0xc]
        fld tbyte ptr [esp]
        fxch st(1)
        or edx, 2
        fnstcw word ptr [esp + 0x24]
        mov eax, dword ptr [esp + 0x24]
        or eax, 0x33f
        mov dword ptr [esp + 0x28], eax
        fldcw word ptr [esp + 0x28]
        mov eax, dword ptr [esp + 0x20]
        and eax, 0x7fff
        cmp eax, 0x7fbe
        ja L4cec69
        or edx, 1
        fmul qword ptr ds:[0x502504]
        fstp tbyte ptr [esp + 0x18]
        fmul qword ptr ds:[0x502504]
        fstp tbyte ptr [esp]
        jmp L4cec89
L4cec69:
        fnstcw word ptr [esp + 0x24]
        mov eax, dword ptr [esp + 0x24]
        or eax, 0x300
        mov dword ptr [esp + 0x28], eax
        fldcw word ptr [esp + 0x28]
        fstp st(0)
        fmul qword ptr ds:[0x502504]
        fstp tbyte ptr [esp]
L4cec89:
        fldcw word ptr [esp + 0x24]
        call dword ptr [g_fprem_common]
        add esp, 0x30
        pop edx
        ret
L4cec97:
        _emit 0xd9 __asm _emit 0xf8   ; fprem
        add esp, 0x30
        pop edx
        ret
    }
#endif
}
// ---- fprem1_common (0x4cec9e)
static __declspec(naked) void c_fprem1_common() {
#ifdef VP_GCC
    __asm__ volatile(
        "push eax\n\t"
        "push ebx\n\t"
        "push ecx\n\t"
        "mov eax, dword ptr [esp + 0x16]\n\t"
        "xor eax, 0x700\n\t"
        "test eax, 0x700\n\t"
        "jne L4cee38%=\n\t"
        "shr eax, 0xb\n\t"
        "and eax, 0xf\n\t"
        "cmp byte ptr [eax + 0x5024ec], 0\n\t"
        "je L4cee38%=\n\t"
        "mov eax, dword ptr [esp + 0x16]\n\t"
        "and eax, 0x7fff0000\n\t"
        "cmp eax, 0x7fff0000\n\t"
        "je L4cee38%=\n\t"
        "mov eax, dword ptr [esp + 0x2e]\n\t"
        "and eax, 0x7fff0000\n\t"
        "je L4cee38%=\n\t"
        "cmp eax, 0x7fff0000\n\t"
        "je L4cee38%=\n\t"
        "mov eax, dword ptr [esp + 0x2c]\n\t"
        "%{load%} add eax, eax\n\t"
        "jne L4cee38%=\n\t"
        "mov eax, dword ptr [esp + 0x14]\n\t"
        "%{load%} add eax, eax\n\t"
        "jne L4cee38%=\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "and eax, 0x7fff\n\t"
        "add eax, 0x3f\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "and ebx, 0x7fff\n\t"
        "%{load%} sub ebx, eax\n\t"
        "ja L4ced86%=\n\t"
        "L4ced28%=:\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "and eax, 0x7fff\n\t"
        "add eax, 0xa\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "and ebx, 0x7fff\n\t"
        "%{load%} sub ebx, eax\n\t"
        "js L4cee38%=\n\t"
        "fld tbyte ptr [esp + 0x28]\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "and ebx, 0x7fff\n\t"
        "%{load%} mov ecx, ebx\n\t"
        "%{load%} sub ebx, eax\n\t"
        "and ebx, 7\n\t"
        "or ebx, 4\n\t"
        "%{load%} sub ecx, ebx\n\t"
        "%{load%} mov ebx, eax\n\t"
        "and ebx, 0x8000\n\t"
        "%{load%} or ecx, ebx\n\t"
        "mov dword ptr [esp + 0x18], ecx\n\t"
        "fld tbyte ptr [esp + 0x10]\n\t"
        "mov dword ptr [esp + 0x18], eax\n\t"
        "fxch st(1)\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xf8\n\t"
        "fstp tbyte ptr [esp + 0x28]\n\t"
        "fstp st(0)\n\t"
        "jmp L4ced28%=\n\t"
        "L4ced86%=:\n\t"
        "test ebx, 2\n\t"
        "jne L4ced96%=\n\t"
        "fld tbyte ptr [esp + 0x10]\n\t"
        "fstp tbyte ptr [esp + 0x1c]\n\t"
        "L4ced96%=:\n\t"
        "fnstcw word ptr [esp + 0x34]\n\t"
        "mov eax, dword ptr [esp + 0x34]\n\t"
        "or eax, 0x33f\n\t"
        "mov dword ptr [esp + 0x38], eax\n\t"
        "fldcw word ptr [esp + 0x38]\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "and eax, 0x7fff\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "and ebx, 0x7fff\n\t"
        "%{load%} sub ebx, eax\n\t"
        "and ebx, 0x3f\n\t"
        "or ebx, 0x20\n\t"
        "add ebx, 1\n\t"
        "%{load%} mov ecx, ebx\n\t"
        "mov eax, dword ptr [esp + 0x18]\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "and ebx, 0x7fff\n\t"
        "and eax, 0x8000\n\t"
        "%{load%} or ebx, eax\n\t"
        "mov dword ptr [esp + 0x18], ebx\n\t"
        "fld tbyte ptr [esp + 0x10]\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xe1\n\t"
        "fld tbyte ptr [esp + 0x28]\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xe1\n\t"
        "L4cedf0%=:\n\t"
        ".byte 0xd8\n\t"
        ".byte 0xd1\n\t"
        "fnstsw ax\n\t"
        "and eax, 0x100\n\t"
        "jne L4cedfd%=\n\t"
        ".byte 0xd8\n\t"
        ".byte 0xe1\n\t"
        "L4cedfd%=:\n\t"
        "fxch st(1)\n\t"
        "fmul qword ptr ds:[0x50251c]\n\t"
        "fxch st(1)\n\t"
        "sub ecx, 1\n\t"
        "jne L4cedf0%=\n\t"
        "mov ebx, dword ptr [esp + 0x30]\n\t"
        "fstp tbyte ptr [esp + 0x28]\n\t"
        "fstp st(0)\n\t"
        "fld tbyte ptr [esp + 0x1c]\n\t"
        "fld tbyte ptr ds:[0x502524]\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xf5\n\t"
        "fstp st(0)\n\t"
        "fld tbyte ptr [esp + 0x28]\n\t"
        "fldcw word ptr [esp + 0x34]\n\t"
        "and ebx, 0x8000\n\t"
        "je L4cee42%=\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xe0\n\t"
        "jmp L4cee42%=\n\t"
        "L4cee38%=:\n\t"
        "fld tbyte ptr [esp + 0x10]\n\t"
        "fld tbyte ptr [esp + 0x28]\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xf5\n\t"
        "L4cee42%=:\n\t"
        "test edx, 3\n\t"
        "je L4ceea0%=\n\t"
        "fnstsw word ptr [esp + 0x3c]\n\t"
        "test edx, 1\n\t"
        "je L4cee75%=\n\t"
        "fnstcw word ptr [esp + 0x34]\n\t"
        "mov eax, dword ptr [esp + 0x34]\n\t"
        "or eax, 0x300\n\t"
        "mov dword ptr [esp + 0x38], eax\n\t"
        "fldcw word ptr [esp + 0x38]\n\t"
        "fmul qword ptr ds:[0x50250c]\n\t"
        "fldcw word ptr [esp + 0x34]\n\t"
        "L4cee75%=:\n\t"
        "mov eax, dword ptr [esp + 0x3c]\n\t"
        "fxch st(1)\n\t"
        "fstp st(0)\n\t"
        "fld tbyte ptr [esp + 0x1c]\n\t"
        "fxch st(1)\n\t"
        "and eax, 0x4300\n\t"
        "sub esp, 0x1c\n\t"
        "fnstenv [esp]\n\t"
        "and dword ptr [esp + 4], 0xbcff\n\t"
        "or dword ptr [esp + 4], eax\n\t"
        "fldenv [esp]\n\t"
        "add esp, 0x1c\n\t"
        "L4ceea0%=:\n\t"
        "pop ecx\n\t"
        "pop ebx\n\t"
        "pop eax\n\t"
        "ret"
        : :);
#else
    __asm {
        push eax
        push ebx
        push ecx
        mov eax, dword ptr [esp + 0x16]
        xor eax, 0x700
        test eax, 0x700
        jne L4cee38
        shr eax, 0xb
        and eax, 0xf
        cmp byte ptr [eax + 0x5024ec], 0
        je L4cee38
        mov eax, dword ptr [esp + 0x16]
        and eax, 0x7fff0000
        cmp eax, 0x7fff0000
        je L4cee38
        mov eax, dword ptr [esp + 0x2e]
        and eax, 0x7fff0000
        je L4cee38
        cmp eax, 0x7fff0000
        je L4cee38
        mov eax, dword ptr [esp + 0x2c]
        add eax, eax
        jne L4cee38
        mov eax, dword ptr [esp + 0x14]
        add eax, eax
        jne L4cee38
        mov eax, dword ptr [esp + 0x18]
        and eax, 0x7fff
        add eax, 0x3f
        mov ebx, dword ptr [esp + 0x30]
        and ebx, 0x7fff
        sub ebx, eax
        ja L4ced86
L4ced28:
        mov eax, dword ptr [esp + 0x18]
        and eax, 0x7fff
        add eax, 0xa
        mov ebx, dword ptr [esp + 0x30]
        and ebx, 0x7fff
        sub ebx, eax
        js L4cee38
        fld tbyte ptr [esp + 0x28]
        mov eax, dword ptr [esp + 0x18]
        mov ebx, dword ptr [esp + 0x30]
        and ebx, 0x7fff
        mov ecx, ebx
        sub ebx, eax
        and ebx, 7
        or ebx, 4
        sub ecx, ebx
        mov ebx, eax
        and ebx, 0x8000
        or ecx, ebx
        mov dword ptr [esp + 0x18], ecx
        fld tbyte ptr [esp + 0x10]
        mov dword ptr [esp + 0x18], eax
        fxch st(1)
        _emit 0xd9 __asm _emit 0xf8   ; fprem
        fstp tbyte ptr [esp + 0x28]
        fstp st(0)
        jmp L4ced28
L4ced86:
        test ebx, 2
        jne L4ced96
        fld tbyte ptr [esp + 0x10]
        fstp tbyte ptr [esp + 0x1c]
L4ced96:
        fnstcw word ptr [esp + 0x34]
        mov eax, dword ptr [esp + 0x34]
        or eax, 0x33f
        mov dword ptr [esp + 0x38], eax
        fldcw word ptr [esp + 0x38]
        mov eax, dword ptr [esp + 0x18]
        and eax, 0x7fff
        mov ebx, dword ptr [esp + 0x30]
        and ebx, 0x7fff
        sub ebx, eax
        and ebx, 0x3f
        or ebx, 0x20
        add ebx, 1
        mov ecx, ebx
        mov eax, dword ptr [esp + 0x18]
        mov ebx, dword ptr [esp + 0x30]
        and ebx, 0x7fff
        and eax, 0x8000
        or ebx, eax
        mov dword ptr [esp + 0x18], ebx
        fld tbyte ptr [esp + 0x10]
        _emit 0xd9 __asm _emit 0xe1   ; fabs
        fld tbyte ptr [esp + 0x28]
        _emit 0xd9 __asm _emit 0xe1   ; fabs
L4cedf0:
        _emit 0xd8 __asm _emit 0xd1   ; fcom st(1)
        fnstsw ax
        and eax, 0x100
        jne L4cedfd
        _emit 0xd8 __asm _emit 0xe1   ; fsub st(1)
L4cedfd:
        fxch st(1)
        fmul qword ptr ds:[0x50251c]
        fxch st(1)
        sub ecx, 1
        jne L4cedf0
        mov ebx, dword ptr [esp + 0x30]
        fstp tbyte ptr [esp + 0x28]
        fstp st(0)
        fld tbyte ptr [esp + 0x1c]
        fld tbyte ptr ds:[0x502524]
        _emit 0xd9 __asm _emit 0xf5   ; fprem1
        fstp st(0)
        fld tbyte ptr [esp + 0x28]
        fldcw word ptr [esp + 0x34]
        and ebx, 0x8000
        je L4cee42
        _emit 0xd9 __asm _emit 0xe0   ; fchs
        jmp L4cee42
L4cee38:
        fld tbyte ptr [esp + 0x10]
        fld tbyte ptr [esp + 0x28]
        _emit 0xd9 __asm _emit 0xf5   ; fprem1
L4cee42:
        test edx, 3
        je L4ceea0
        fnstsw word ptr [esp + 0x3c]
        test edx, 1
        je L4cee75
        fnstcw word ptr [esp + 0x34]
        mov eax, dword ptr [esp + 0x34]
        or eax, 0x300
        mov dword ptr [esp + 0x38], eax
        fldcw word ptr [esp + 0x38]
        fmul qword ptr ds:[0x50250c]
        fldcw word ptr [esp + 0x34]
L4cee75:
        mov eax, dword ptr [esp + 0x3c]
        fxch st(1)
        fstp st(0)
        fld tbyte ptr [esp + 0x1c]
        fxch st(1)
        and eax, 0x4300
        sub esp, 0x1c
        fnstenv [esp]
        and dword ptr [esp + 4], 0xbcff
        or dword ptr [esp + 4], eax
        fldenv [esp]
        add esp, 0x1c
L4ceea0:
        pop ecx
        pop ebx
        pop eax
        ret
    }
#endif
}
// ---- adj_fprem1 (0x4ceea4)
static __declspec(naked) void c_adj_fprem1() {
#ifdef VP_GCC
    __asm__ volatile(
        "push edx\n\t"
        "sub esp, 0x30\n\t"
        "fstp tbyte ptr [esp + 0x18]\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "mov edx, 0\n\t"
        "mov eax, dword ptr [esp + 6]\n\t"
        "test eax, 0x7fff0000\n\t"
        "je L4ceec9%=\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0x30\n\t"
        "pop edx\n\t"
        "ret\n\t"
        "L4ceec9%=:\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fld tbyte ptr [esp + 0x18]\n\t"
        "mov eax, dword ptr [esp]\n\t"
        "or eax, dword ptr [esp + 4]\n\t"
        "je L4cef52%=\n\t"
        "fxch st(1)\n\t"
        "fstp tbyte ptr [esp + 0xc]\n\t"
        "fld tbyte ptr [esp]\n\t"
        "fxch st(1)\n\t"
        "or edx, 2\n\t"
        "fnstcw word ptr [esp + 0x24]\n\t"
        "mov eax, dword ptr [esp + 0x24]\n\t"
        "or eax, 0x33f\n\t"
        "mov dword ptr [esp + 0x28], eax\n\t"
        "fldcw word ptr [esp + 0x28]\n\t"
        "mov eax, dword ptr [esp + 0x20]\n\t"
        "and eax, 0x7fff\n\t"
        "cmp eax, 0x7fbe\n\t"
        "ja L4cef24%=\n\t"
        "or edx, 1\n\t"
        "fmul qword ptr ds:[0x502504]\n\t"
        "fstp tbyte ptr [esp + 0x18]\n\t"
        "fmul qword ptr ds:[0x502504]\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "jmp L4cef44%=\n\t"
        "L4cef24%=:\n\t"
        "fnstcw word ptr [esp + 0x24]\n\t"
        "mov eax, dword ptr [esp + 0x24]\n\t"
        "or eax, 0x300\n\t"
        "mov dword ptr [esp + 0x28], eax\n\t"
        "fldcw word ptr [esp + 0x28]\n\t"
        "fstp st(0)\n\t"
        "fmul qword ptr ds:[0x502504]\n\t"
        "fstp tbyte ptr [esp]\n\t"
        "L4cef44%=:\n\t"
        "fldcw word ptr [esp + 0x24]\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 0x30\n\t"
        "pop edx\n\t"
        "ret\n\t"
        "L4cef52%=:\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xf8\n\t"
        "add esp, 0x30\n\t"
        "pop edx\n\t"
        "ret"
        : : "i"(&g_fprem1_common));
#else
    __asm {
        push edx
        sub esp, 0x30
        fstp tbyte ptr [esp + 0x18]
        fstp tbyte ptr [esp]
        mov edx, 0
        mov eax, dword ptr [esp + 6]
        test eax, 0x7fff0000
        je L4ceec9
        call dword ptr [g_fprem1_common]
        add esp, 0x30
        pop edx
        ret
L4ceec9:
        fld tbyte ptr [esp]
        fld tbyte ptr [esp + 0x18]
        mov eax, dword ptr [esp]
        or eax, dword ptr [esp + 4]
        je L4cef52
        fxch st(1)
        fstp tbyte ptr [esp + 0xc]
        fld tbyte ptr [esp]
        fxch st(1)
        or edx, 2
        fnstcw word ptr [esp + 0x24]
        mov eax, dword ptr [esp + 0x24]
        or eax, 0x33f
        mov dword ptr [esp + 0x28], eax
        fldcw word ptr [esp + 0x28]
        mov eax, dword ptr [esp + 0x20]
        and eax, 0x7fff
        cmp eax, 0x7fbe
        ja L4cef24
        or edx, 1
        fmul qword ptr ds:[0x502504]
        fstp tbyte ptr [esp + 0x18]
        fmul qword ptr ds:[0x502504]
        fstp tbyte ptr [esp]
        jmp L4cef44
L4cef24:
        fnstcw word ptr [esp + 0x24]
        mov eax, dword ptr [esp + 0x24]
        or eax, 0x300
        mov dword ptr [esp + 0x28], eax
        fldcw word ptr [esp + 0x28]
        fstp st(0)
        fmul qword ptr ds:[0x502504]
        fstp tbyte ptr [esp]
L4cef44:
        fldcw word ptr [esp + 0x24]
        call dword ptr [g_fprem1_common]
        add esp, 0x30
        pop edx
        ret
L4cef52:
        _emit 0xd9 __asm _emit 0xf8   ; fprem
        add esp, 0x30
        pop edx
        ret
    }
#endif
}
// ---- safe_fprem (0x4cef59)
static __declspec(naked) void c_safe_fprem() {
#ifdef VP_GCC
    __asm__ volatile(
        "call dword ptr [%c0]\n\t"
        "ret"
        : : "i"(&g_adj_fprem));
#else
    __asm {
        call dword ptr [g_adj_fprem]
        ret
    }
#endif
}
// ---- safe_fprem1 (0x4cef5f)
static __declspec(naked) void c_safe_fprem1() {
#ifdef VP_GCC
    __asm__ volatile(
        "call dword ptr [%c0]\n\t"
        "ret"
        : : "i"(&g_adj_fprem1));
#else
    __asm {
        call dword ptr [g_adj_fprem1]
        ret
    }
#endif
}
// ---- adj_fptan (0x4cef68)
static __declspec(naked) void c_adj_fptan() {
#ifdef VP_GCC
    __asm__ volatile(
        ".byte 0xd9\n\t"
        ".byte 0xf2\n\t"
        "ret"
        : :);
#else
    __asm {
        _emit 0xd9 __asm _emit 0xf2   ; fptan
        ret
    }
#endif
}
// ---- ms_p5_test_fdiv (0x4d0840)
static __declspec(naked) void c_ms_p5_test_fdiv() {
#ifdef VP_GCC
    __asm__ volatile(
        "sub esp, 0x18\n\t"
        "mov dword ptr [esp + 8], 0x80000000\n\t"
        "mov dword ptr [esp + 0xc], 0x4147ffff\n\t"
        "mov dword ptr [esp], 0xc0000000\n\t"
        "mov dword ptr [esp + 4], 0x4150017e\n\t"
        "fld qword ptr [esp]\n\t"
        "fdiv qword ptr [esp + 8]\n\t"
        "fmul qword ptr [esp + 8]\n\t"
        "fsubr qword ptr [esp]\n\t"
        "fstp qword ptr [esp + 0x10]\n\t"
        ".byte 0xd9\n\t"
        ".byte 0xe8\n\t"
        "fcomp qword ptr [esp + 0x10]\n\t"
        "fnstsw ax\n\t"
        "test ah, 1\n\t"
        "mov eax, 1\n\t"
        "jne L4d088b%=\n\t"
        "%{load%} xor eax, eax\n\t"
        "L4d088b%=:\n\t"
        "add esp, 0x18\n\t"
        "ret"
        : :);
#else
    __asm {
        sub esp, 0x18
        mov dword ptr [esp + 8], 0x80000000
        mov dword ptr [esp + 0xc], 0x4147ffff
        mov dword ptr [esp], 0xc0000000
        mov dword ptr [esp + 4], 0x4150017e
        fld qword ptr [esp]
        fdiv qword ptr [esp + 8]
        fmul qword ptr [esp + 8]
        fsubr qword ptr [esp]
        fstp qword ptr [esp + 0x10]
        _emit 0xd9 __asm _emit 0xe8   ; fld1
        fcomp qword ptr [esp + 0x10]
        fnstsw ax
        test ah, 1
        mov eax, 1
        jne L4d088b
        xor eax, eax
L4d088b:
        add esp, 0x18
        ret
    }
#endif
}
// ---- chkstk (0x4cf3d0)
static __declspec(naked) void c_chkstk() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ecx\n\t"
        "cmp eax, 0x1000\n\t"
        "lea ecx, [esp + 8]\n\t"
        "jb L4cf3f0%=\n\t"
        "L4cf3dc%=:\n\t"
        "sub ecx, 0x1000\n\t"
        "sub eax, 0x1000\n\t"
        "test dword ptr [ecx], eax\n\t"
        "cmp eax, 0x1000\n\t"
        "jae L4cf3dc%=\n\t"
        "L4cf3f0%=:\n\t"
        "%{load%} sub ecx, eax\n\t"
        "%{load%} mov eax, esp\n\t"
        "test dword ptr [ecx], eax\n\t"
        "%{load%} mov esp, ecx\n\t"
        "mov ecx, dword ptr [eax]\n\t"
        "mov eax, dword ptr [eax + 4]\n\t"
        "push eax\n\t"
        "ret"
        : :);
#else
    __asm {
        push ecx
        cmp eax, 0x1000
        lea ecx, [esp + 8]
        jb L4cf3f0
L4cf3dc:
        sub ecx, 0x1000
        sub eax, 0x1000
        test dword ptr [ecx], eax
        cmp eax, 0x1000
        jae L4cf3dc
L4cf3f0:
        sub ecx, eax
        mov eax, esp
        test dword ptr [ecx], eax
        mov esp, ecx
        mov ecx, dword ptr [eax]
        mov eax, dword ptr [eax + 4]
        push eax
        ret
    }
#endif
}

// ---- __adj_fdiv_r (0x4ce2f7): eax & 0x3f picks the entry, which runs in the frame every entry expects (sub esp, 0x2c)
static void (*const k_fdivr_tab[64])() = {
    fdivr_h00, fdivr_h01, fdivr_h02, fdivr_h03, fdivr_h04, fdivr_h05, fdivr_h06, fdivr_h07,
    fdivr_h08, fdivr_h09, fdivr_h10, fdivr_h11, fdivr_h12, fdivr_h13, fdivr_h14, fdivr_h15,
    fdivr_h16, fdivr_h17, fdivr_h18, fdivr_h19, fdivr_h20, fdivr_h21, fdivr_h22, fdivr_h23,
    fdivr_h24, fdivr_h25, fdivr_h26, fdivr_h27, fdivr_h28, fdivr_h29, fdivr_h30, fdivr_h31,
    fdivr_h32, fdivr_h33, fdivr_h34, fdivr_h35, fdivr_h36, fdivr_h37, fdivr_h38, fdivr_h39,
    fdivr_h40, fdivr_h41, fdivr_h42, fdivr_h43, fdivr_h44, fdivr_h45, fdivr_h46, fdivr_h47,
    fdivr_h48, fdivr_h49, fdivr_h50, fdivr_h51, fdivr_h52, fdivr_h53, fdivr_h54, fdivr_h55,
    fdivr_h56, fdivr_h57, fdivr_h58, fdivr_h59, fdivr_h60, fdivr_h61, fdivr_h62, fdivr_h63
};
static __declspec(naked) void c_adj_fdiv_r() {
#ifdef VP_GCC
    __asm__ volatile(
        "sub esp, 0x2c\n\t"
        "and eax, 0x3f\n\t"
        "jmp dword ptr [%c0 + eax * 4]"
        : : "i"(&k_fdivr_tab));
#else
    __asm {
        sub esp, 0x2c
        and eax, 0x3f
        jmp dword ptr k_fdivr_tab[eax * 4]
    }
#endif
}

// ---- __ms_p5_mp_test_fdiv (0x4d0890): the test on this processor; if it passes, on each processor of the system's
// affinity mask in turn (pinning the thread), and the thread's affinity left as the system's mask
typedef BOOL(WINAPI* GetProcessAffinityMask_t)(HANDLE, DWORD_PTR*, DWORD_PTR*);
typedef HANDLE(WINAPI* GetCurrent_t)();
typedef DWORD_PTR(WINAPI* SetThreadAffinityMask_t)(HANDLE, DWORD_PTR);
static int32_t __cdecl c_ms_p5_mp_test_fdiv() {
    typedef int32_t(__cdecl * Test_t)();
    const Test_t test = (Test_t)(uintptr_t)g_ms_p5_test_fdiv;
    if (test()) return 1;
    const HMODULE k = kLoadLibraryA((LPCSTR)(uintptr_t)0x005026ec);                       // "KERNEL32"
    if (!k) return 0;
    const GetProcessAffinityMask_t gpam =
        (GetProcessAffinityMask_t)kGetProcAddress(k, (LPCSTR)(uintptr_t)0x005026d4);   // GetProcessAffinityMask
    if (!gpam) return 0;
    const GetCurrent_t gcp = (GetCurrent_t)kGetProcAddress(k, (LPCSTR)(uintptr_t)0x005026c0);   // GetCurrentProcess
    if (!gcp) return 0;
    const SetThreadAffinityMask_t stam =
        (SetThreadAffinityMask_t)kGetProcAddress(k, (LPCSTR)(uintptr_t)0x005026a8);    // SetThreadAffinityMask
    if (!stam) return 0;
    const GetCurrent_t gct = (GetCurrent_t)kGetProcAddress(k, (LPCSTR)(uintptr_t)0x00502694);   // GetCurrentThread
    if (!gct) return 0;
    DWORD_PTR m[2];                                 // [esp+0x10] the system's mask, [esp+0x14] the process's
    if (!gpam(gcp(), &m[1], &m[0])) return 0;
    for (uint32_t i = 0; i < 32; i++) {
        const DWORD_PTR bit = (DWORD_PTR)1 << i;
        if (!(m[0] & bit)) continue;
        stam(gct(), bit);
        if (test()) {
            stam(gct(), m[0]);
            return 1;
        }
    }
    stam(gct(), m[0]);
    return 0;
}
static void fp_mp_test(Footprint& f) { f.replay_only = "pins the thread to each processor in turn (start-up)"; }
PORT_FN(0x004d0890, "ms_p5_mp_test_fdiv", c_ms_p5_mp_test_fdiv, fp_mp_test)

// ---- registration: their shadow entry is the rewrite itself (see the top) ---------------------------------------------------------------------
static void fp_x87(Footprint& f) { f.replay_only = "x87-stack convention: checked offline (test/world_crt_io.cpp)"; }
PORT_FN(0x004ce1e0, "fdiv_main_routine", c_fdiv_main_routine, fp_x87) CRT_NO_SHADOW(c_fdiv_main_routine)
PORT_FN(0x004ce2f7, "adj_fdiv_r", c_adj_fdiv_r, fp_x87) CRT_NO_SHADOW(c_adj_fdiv_r)
PORT_FN(0x004ce796, "fdivp_sti_st", c_fdivp_sti_st, fp_x87) CRT_NO_SHADOW(c_fdivp_sti_st)
PORT_FN(0x004ce7a9, "fdivrp_sti_st", c_fdivrp_sti_st, fp_x87) CRT_NO_SHADOW(c_fdivrp_sti_st)
PORT_FN(0x004ce7bc, "adj_fdiv_m32", c_adj_fdiv_m32, fp_x87) CRT_NO_SHADOW(c_adj_fdiv_m32)
PORT_FN(0x004ce808, "adj_fdiv_m64", c_adj_fdiv_m64, fp_x87) CRT_NO_SHADOW(c_adj_fdiv_m64)
PORT_FN(0x004ce854, "adj_fdiv_m16i", c_adj_fdiv_m16i, fp_x87) CRT_NO_SHADOW(c_adj_fdiv_m16i)
PORT_FN(0x004ce888, "adj_fdiv_m32i", c_adj_fdiv_m32i, fp_x87) CRT_NO_SHADOW(c_adj_fdiv_m32i)
PORT_FN(0x004ce8bc, "adj_fdivr_m32", c_adj_fdivr_m32, fp_x87) CRT_NO_SHADOW(c_adj_fdivr_m32)
PORT_FN(0x004ce908, "adj_fdivr_m64", c_adj_fdivr_m64, fp_x87) CRT_NO_SHADOW(c_adj_fdivr_m64)
PORT_FN(0x004ce954, "adj_fdivr_m16i", c_adj_fdivr_m16i, fp_x87) CRT_NO_SHADOW(c_adj_fdivr_m16i)
PORT_FN(0x004ce988, "adj_fdivr_m32i", c_adj_fdivr_m32i, fp_x87) CRT_NO_SHADOW(c_adj_fdivr_m32i)
PORT_FN(0x004ce9bc, "safe_fdiv", c_safe_fdiv, fp_x87) CRT_NO_SHADOW(c_safe_fdiv)
PORT_FN(0x004ce9d1, "safe_fdivr", c_safe_fdivr, fp_x87) CRT_NO_SHADOW(c_safe_fdivr)
PORT_FN(0x004ce9e6, "fprem_common", c_fprem_common, fp_x87) CRT_NO_SHADOW(c_fprem_common)
PORT_FN(0x004cebec, "adj_fprem", c_adj_fprem, fp_x87) CRT_NO_SHADOW(c_adj_fprem)
PORT_FN(0x004cec9e, "fprem1_common", c_fprem1_common, fp_x87) CRT_NO_SHADOW(c_fprem1_common)
PORT_FN(0x004ceea4, "adj_fprem1", c_adj_fprem1, fp_x87) CRT_NO_SHADOW(c_adj_fprem1)
PORT_FN(0x004cef59, "safe_fprem", c_safe_fprem, fp_x87) CRT_NO_SHADOW(c_safe_fprem)
PORT_FN(0x004cef5f, "safe_fprem1", c_safe_fprem1, fp_x87) CRT_NO_SHADOW(c_safe_fprem1)
PORT_FN(0x004cef68, "adj_fptan", c_adj_fptan, fp_x87) CRT_NO_SHADOW(c_adj_fptan)
PORT_FN(0x004d0840, "ms_p5_test_fdiv", c_ms_p5_test_fdiv, fp_x87) CRT_NO_SHADOW(c_ms_p5_test_fdiv)
PORT_FN(0x004cf3d0, "chkstk", c_chkstk, fp_x87) CRT_NO_SHADOW(c_chkstk)

}   // namespace crt_fdiv
