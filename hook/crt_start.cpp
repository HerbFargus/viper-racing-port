// crt_start.cpp -- M3 stage S1 (standalone), group A: the C runtime's start-up and exit code, its exception plumbing, and
// the last pieces of original code the game can reach that have no inventory entry of their own, rewritten. With these,
// nothing of the 1998 code is left on the path from the process entry to ExitProcess.
//
// The objects: wincrt0 (_WinMainCRTStartup, _amsg_exit), crt0dat (_cinit, exit, _exit, doexit, _initterm), crt0msg
// (_FF_MSGBANNER, _NMSG_WRITE), crtmbox (__crtMessageBoxA), crt0fp (_fptrap), stdargv (_setargv, parse_cmdline), stdenvp
// (_setenvp), aw_env (__crtGetEnvironmentStringsA), ioinit (_ioinit), heapinit (_heap_init), onexit (_onexit, atexit,
// __onexitinit), closeall (_fcloseall), purevirt (_purecall), handler (_callnewh), winxfltr (_XcptFilter, xcptlookup),
// fpinit (_fpmath, _fpclear, _cfltcvt_init), fpexcept (_raise_exc, _handle_exc, _set_errno), 87except (_87except),
// matherr (_matherr), exsup (_global_unwind2, _unwind_handler, _local_unwind2, _abnormal_termination, _NLG_Notify,
// _NLG_Notify1), exsup3 (_except_handler3, _seh_longjmp_unwind@4); and, with no inventory entry: _fFMOD (87fmod's
// kernel, 0x4cf374), 87disp's result routines (0x4d2e93..0x4d2f69), __adj_fpatan (0x4cef65), _prof.obj's __rdtsc
// (0x4188ad); and ds.obj's directsound_create_mixers (an empty function MixerBegin's rewrite still calls).
//
// Written from the v1.0 disassembly, faithful: every call in the original's order, by its v1.0 address (so whichever of
// the original or the rewrite is installed runs); Windows through the game's import slots, each read where the original
// reads it; the CRT's state at its v1.0 addresses (crt_types.h). What the CRT wrote in C is C here; what has a frame or
// register convention a C function can't reproduce -- _WinMainCRTStartup's SEH frame, the exsup / exsup3 routines,
// __crtGetEnvironmentStringsA (which reads an uninitialised local on a second call), _fpmath, the x87 kernels -- is
// naked assembly, instruction for instruction.
//
// The DLL route runs these too. The DLL is loaded (DllMain: port_install) before Windows calls race.exe's entry point,
// so _WinMainCRTStartup's hook is in place when the process starts, and everything after it -- heap, ioinit, argv,
// environ, the $E initialisers, WinMain, the exit path -- is the code below in both routes. In shadow mode the C ones
// are replay_only (they allocate, start the runtime or end the process: the original runs) and the naked ones run new.
//
// Code reached only through tables, or too short for a 5-byte jump (crt_types.h, "islands"): _fFMOD is an ordinary
// PORT_FN (its entry is hookable). 87disp's result routines are packed tight -- a 1-byte `ret` entry, 2-byte entries
// that fall into the next one -- and __adj_fpatan (3 bytes, __adj_fptan's hook right behind it) and __rdtsc (3 bytes,
// isCpuidSupported's hook right behind it) are too short: each live entry gets either a 5-byte jmp to its rewrite, or a
// 2-byte short jmp to a 5-byte jmp placed in dead bytes nearby (the 87disp entries nothing references, int3 padding).
// crt_islands_install() writes them, after checking every byte it replaces is the original's, int3 (the standalone's
// fill) or already its own; _WinMainCRTStartup's rewrite calls it first thing, so both routes get them (after
// port_check_stock, which fingerprints trandisp2 / adj_fptan / onexit / KernelEnd with their original bytes).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "vp_os.h"
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "crt_types.h"

void logf(const char* fmt, ...);

// The SEH frames below are the 1998 runtime's: their handler is _except_handler3 at its v1.0 address, inside race.exe
// (no SafeSEH table there), not a function of this DLL -- so the compiler's "handler not registered as safe" is moot.
#pragma warning(disable : 4733)

// A naked rewrite: never through PORT_FN's C shadow wrapper (in shadow mode its entry is the rewrite). A harness that
// registers the rewrites itself defines this as nothing.
#ifndef CRT_START_NO_SHADOW
#ifdef VP_FUZZ
#define CRT_START_NO_SHADOW(NEW)
#else
#define CRT_START_NO_SHADOW(NEW) \
    namespace { const bool VP_CAT(noshadow_, NEW) = (VP_CAT(port_, NEW).shadow = VP_CAT(port_, NEW).repl, true); }
#endif
#endif

namespace crt_start {   // (file-local helpers; one namespace per file so a harness can include several)

// ---- the game's imports, functions and state ----------------------------------------------------------------------------
#define IAT(slot, fn) (*(decltype(&fn) volatile*)(uintptr_t)(slot))
#define kGetModuleFileNameA       IAT(0x005d74e4, GetModuleFileNameA)
#define kHeapCreate               IAT(0x005d7494, HeapCreate)
#define kExitProcess              IAT(0x005d7478, ExitProcess)
#define kGetStartupInfoA          IAT(0x005d75f8, GetStartupInfoA)
#define kGetFileType              IAT(0x005d75e8, GetFileType)
#define kSetHandleCount           IAT(0x005d75e4, SetHandleCount)
#define kWriteFile                IAT(0x005d7538, WriteFile)
#define kLoadLibraryA             IAT(0x005d758c, LoadLibraryA)
#define kUnhandledExceptionFilter IAT(0x005d75bc, UnhandledExceptionFilter)
#define kRaiseException           IAT(0x005d756c, RaiseException)
enum : uint32_t { SLOT_GetStdHandle = 0x005d751c, SLOT_GetProcAddress = 0x005d75a0 };

template <typename R, typename... A> static __forceinline R gcall(uint32_t fn, A... a) {
    typedef R(__cdecl * Fn)(A...);
    return ((Fn)(uintptr_t)fn)(a...);
}
enum : uint32_t {
    F_malloc = 0x004d1de0, F_free = 0x004d58b0, F_realloc = 0x004d1d50, F_msize = 0x004d1dc0, F_strncpy = 0x004cf3a0,
    F_fclose = 0x004d0630, F_amsg_exit = 0x004cf730, F_FF_MSGBANNER = 0x004d4570, F_NMSG_WRITE = 0x004d45b0,
    F_crtMessageBoxA = 0x004d6850, F_onexit = 0x004cef70, F_initterm = 0x004d1d30, F_doexit = 0x004d1cb0,
    F_parse_cmdline = 0x004d3c40, F_xcptlookup = 0x004d3a30, F_handle_exc = 0x004d34f0, F_raise_exc = 0x004d3180,
    F_ctrlfp = 0x004d2fb0, F_matherr = 0x004d6840, F_set_errno = 0x004d37b0, F_statfp = 0x004d2f70,
    F_clrfp = 0x004d2f90, F_set_statfp = 0x004d2ff0, F_decomp = 0x004d3090,
};
#define G32(va) CRT_G(uint32_t, va)
#define G8(va) CRT_G(uint8_t, va)

// the same addresses as memory operands for the assembly rewrites' calls
const uint32_t a_heap_init = 0x004d4470, a_ioinit = 0x004d4290, a_initmbctable = 0x004d4280,
               a_crtGetEnvironmentStringsA = 0x004d3e20, a_exit = 0x004d1c70, a__exit = 0x004d1c90,
               a_setargv = 0x004d3ba0, a_setenvp = 0x004d3ac0, a_cinit = 0x004d1c40, a_ismbblead = 0x004d3a60,
               a_WinMain = 0x00412260, a_XcptFilter = 0x004d38d0, a_cfltcvt_init = 0x004ce150,
               a_ms_p5_mp_test_fdiv = 0x004d0890, a_setdefaultprecision = 0x004d0820, a_global_unwind2 = 0x004d37dc,
               a_local_unwind2 = 0x004d381e, a_NLG_Notify = 0x004d38b2, a_adj_fprem = 0x004cebec,
               a_malloc = F_malloc, a_free = F_free;

static void fp_runtime(Footprint& f) { f.replay_only = "the C runtime's start-up / exit (allocates, ends the process)"; }

// =====================================================================================================================
// fpinit.obj
// =====================================================================================================================
// _fpmath (0x4ce120, reached through _FPinit from _cinit): the float conversions, the FDIV test, 53-bit precision
static __declspec(naked) void crt_fpmath() {
#ifdef VP_GCC
    __asm__ volatile(
        "call dword ptr [%c0]\n\t"
        "call dword ptr [%c1]\n\t"
        "mov dword ptr ds:[0x5024b8], eax\n\t"
        "call dword ptr [%c2]\n\t"
        "fnclex\n\t"
        "ret"
        : : "i"(&a_cfltcvt_init), "i"(&a_ms_p5_mp_test_fdiv), "i"(&a_setdefaultprecision));
#else
    __asm {
        call dword ptr [a_cfltcvt_init]
        call dword ptr [a_ms_p5_mp_test_fdiv]
        mov dword ptr ds:[0x5024b8], eax
        call dword ptr [a_setdefaultprecision]
        fnclex
        ret
    }
#endif
}
static void fp_asm(Footprint& f) { f.replay_only = "register / FPU convention"; }
PORT_FN(0x004ce120, "fpmath", crt_fpmath, fp_asm)
CRT_START_NO_SHADOW(crt_fpmath)

// _fpclear (0x4ce140): nothing (the single-threaded library's FPU reset hook)
static void __cdecl crt_fpclear() {}
static void fp_none(Footprint& f) { f.pure = true; }
PORT_FN(0x004ce140, "fpclear", crt_fpclear, fp_none)

// _cfltcvt_init (0x4ce150): printf / scanf's float routines into _cfltcvt_tab (until then each is _fptrap)
static void __cdecl crt_cfltcvt_init() {
    G32(CRT_CROPZEROS_VA) = 0x004d0a20;
    G32(CRT_FASSIGN_VA) = 0x004d0aa0;
    G32(CRT_FORCDECPT_VA) = 0x004d09b0;
    G32(CRT_POSITIVE_VA) = 0x004d0a80;
    G32(CRT_CFLTCVT_VA) = 0x004d0e40;
    G32(CRT_CFLTCVT2_VA) = 0x004d0e40;
}
static void fp_cfltcvt_init(Footprint& f) { f.add((void*)(uintptr_t)CRT_CFLTCVT_VA, 0x18, "_cfltcvt_tab"); }
PORT_FN(0x004ce150, "cfltcvt_init", crt_cfltcvt_init, fp_cfltcvt_init)

// =====================================================================================================================
// onexit.obj: the atexit table [__onexitbegin, __onexitend), grown 4 entries at a time
// =====================================================================================================================
// _onexit (0x4cef70)
static uint32_t __cdecl crt_onexit(uint32_t fn) {
    const uint32_t have = gcall<uint32_t>(F_msize, G32(CRT_ONEXITBEGIN_VA));
    if (have < G32(CRT_ONEXITEND_VA) - G32(CRT_ONEXITBEGIN_VA) + 4) {
        const uint32_t sz = gcall<uint32_t>(F_msize, G32(CRT_ONEXITBEGIN_VA));
        const uint32_t p = gcall<uint32_t>(F_realloc, G32(CRT_ONEXITBEGIN_VA), sz + 0x10);
        if (!p) return 0;
        const uint32_t used = (G32(CRT_ONEXITEND_VA) - G32(CRT_ONEXITBEGIN_VA)) & ~3u;
        G32(CRT_ONEXITBEGIN_VA) = p;
        G32(CRT_ONEXITEND_VA) = used + p;
    }
    *(uint32_t*)(uintptr_t)G32(CRT_ONEXITEND_VA) = fn;
    G32(CRT_ONEXITEND_VA) += 4;
    return fn;
}
static void fp_onexit(Footprint& f, uint32_t) { f.replay_only = "allocates"; }
PORT_FN(0x004cef70, "onexit", crt_onexit, fp_onexit)

// atexit (0x4ceff0): 0, or -1 when _onexit failed
static int32_t __cdecl crt_atexit(uint32_t fn) { return gcall<uint32_t>(F_onexit, fn) ? 0 : -1; }
static void fp_atexit(Footprint& f, uint32_t) { f.replay_only = "allocates"; }
PORT_FN(0x004ceff0, "atexit", crt_atexit, fp_atexit)

// __onexitinit (0x4cf010, the first initialiser): a 32-entry table
static void __cdecl crt_onexitinit() {
    G32(CRT_ONEXITBEGIN_VA) = gcall<uint32_t>(F_malloc, 0x80u);
    if (!G32(CRT_ONEXITBEGIN_VA)) gcall<void>(F_amsg_exit, 0x18);
    *(uint32_t*)(uintptr_t)G32(CRT_ONEXITBEGIN_VA) = 0;
    G32(CRT_ONEXITEND_VA) = G32(CRT_ONEXITBEGIN_VA);
}
PORT_FN(0x004cf010, "onexitinit", crt_onexitinit, fp_runtime)

// =====================================================================================================================
// purevirt.obj, crt0fp.obj, handler.obj, matherr.obj
// =====================================================================================================================
// _purecall (0x4cf350): R6025
static void __cdecl crt_purecall() { gcall<void>(F_amsg_exit, 0x19); }
PORT_FN(0x004cf350, "purecall", crt_purecall, fp_runtime)

// _fptrap (0x4d5c10): R6002, "floating point not loaded"
static void __cdecl crt_fptrap() { gcall<void>(F_amsg_exit, 2); }
PORT_FN(0x004d5c10, "fptrap", crt_fptrap, fp_runtime)

// _callnewh (0x4d64e0): the new handler (_pnhHeap), if any: 1 when it says it freed something
static int32_t __cdecl crt_callnewh(uint32_t size) {
    const uint32_t h = G32(CRT_PNHHEAP_VA);
    if (h && gcall<int32_t>(h, size)) return 1;
    return 0;
}
static void fp_callnewh(Footprint& f, uint32_t) { f.replay_only = "calls the new handler"; }
PORT_FN(0x004d64e0, "callnewh", crt_callnewh, fp_callnewh)

// _matherr (0x4d6840): the library's default, 0
static int32_t __cdecl crt_matherr(void*) { return 0; }
static void fp_matherr(Footprint& f, void*) { f.pure = true; }
PORT_FN(0x004d6840, "matherr", crt_matherr, fp_matherr)

// =====================================================================================================================
// wincrt0.obj, crt0dat.obj, crt0msg.obj, crtmbox.obj
// =====================================================================================================================
// _amsg_exit (0x4cf730): the banner (console error mode), the message, then _aexit_rtn(255)
static void __cdecl crt_amsg_exit(int32_t code) {
    if (G32(CRT_ERROR_MODE_VA) == 1) gcall<void>(F_FF_MSGBANNER);
    gcall<void>(F_NMSG_WRITE, code);
    gcall<void>(G32(CRT_AEXIT_RTN_VA), 0xff);
}
static void fp_amsg_exit(Footprint& f, int32_t) { fp_runtime(f); }
PORT_FN(0x004cf730, "amsg_exit", crt_amsg_exit, fp_amsg_exit)

// _FF_MSGBANNER (0x4d4570): "runtime error " before a message, on a console
static void __cdecl crt_FF_MSGBANNER() {
    const uint32_t m = G32(CRT_ERROR_MODE_VA);
    if (m == 1 || (m == 0 && G32(CRT_APP_TYPE_VA) == 1)) {
        gcall<void>(F_NMSG_WRITE, 0xfc);
        if (G32(CRT_ADBGMSG_VA)) gcall<void>(G32(CRT_ADBGMSG_VA));
        gcall<void>(F_NMSG_WRITE, 0xff);
    }
}
PORT_FN(0x004d4570, "FF_MSGBANNER", crt_FF_MSGBANNER, fp_runtime)

static __forceinline size_t cstrlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static __forceinline void cstrcat(char* d, const char* s) {
    d += cstrlen(d);
    size_t n = cstrlen(s) + 1;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

// _NMSG_WRITE (0x4d45b0): the message for `code` (rterrs, 0x502f88: {number, text} x 17) -- to stderr on a console,
// else a message box "Runtime Error! Program: <the exe, its last 60 characters>" (none for the banner, 252)
static void __cdecl crt_NMSG_WRITE(int32_t code) {
    uint32_t i = 0;
    for (uint32_t a = CRT_RTERRS_VA;;) {
        if ((int32_t)G32(a) == code) break;
        a += 8;
        i++;
        if (a >= CRT_RTERRS_END) break;
    }
    if ((int32_t)G32(CRT_RTERRS_VA + i * 8) != code) return;
    const char* msg;
    if (G32(CRT_ERROR_MODE_VA) == 1 || (G32(CRT_ERROR_MODE_VA) == 0 && G32(CRT_APP_TYPE_VA) == 1)) {
        HANDLE h = (HANDLE)(uintptr_t)(*(uint32_t*)(G32(CRT_PIOINFO_VA) + 0x10));
        if (h == INVALID_HANDLE_VALUE)
            h = (*(decltype(&GetStdHandle) volatile*)(uintptr_t)SLOT_GetStdHandle)(STD_ERROR_HANDLE);
        msg = (const char*)(uintptr_t)G32(CRT_RTERRS_VA + 4 + i * 8);
        DWORD written;
        kWriteFile(h, msg, (DWORD)cstrlen(msg), &written, 0);
        return;
    }
    if (code == 0xfc) return;
    char prog[0x104];
    char out[0xa0];
    if (!kGetModuleFileNameA(0, prog, 0x104)) memcpy(prog, (const void*)(uintptr_t)CRT_STR_PROGUNKNOWN_VA, 23);
    char* pch = prog;
    const size_t n = cstrlen(prog) + 1;
    if (n > 0x3c) {
        pch = prog + n - 0x3c;
        gcall<char*>(F_strncpy, pch, (const char*)(uintptr_t)CRT_STR_DOTS_VA, 3u);
    }
    memcpy(out, (const void*)(uintptr_t)CRT_STR_RUNTIMEERR_VA, 26);
    cstrcat(out, pch);
    cstrcat(out, (const char*)(uintptr_t)CRT_STR_NLNL_VA);
    cstrcat(out, (const char*)(uintptr_t)G32(CRT_RTERRS_VA + 4 + i * 8));
    gcall<int32_t>(F_crtMessageBoxA, (const char*)out, (const char*)(uintptr_t)CRT_STR_VCRT_VA, 0x12010u);
}
static void fp_NMSG_WRITE(Footprint& f, int32_t) { fp_runtime(f); }
PORT_FN(0x004d45b0, "NMSG_WRITE", crt_NMSG_WRITE, fp_NMSG_WRITE)

// __crtMessageBoxA (0x4d6850): user32's MessageBoxA (and GetActiveWindow / GetLastActivePopup for the owner), found
// once through LoadLibrary
static int32_t __cdecl crt_crtMessageBoxA(const char* text, const char* caption, uint32_t type) {
    typedef int(WINAPI * MB_t)(HWND, LPCSTR, LPCSTR, UINT);
    typedef HWND(WINAPI * GAW_t)();
    typedef HWND(WINAPI * GLAP_t)(HWND);
    HWND owner = 0;
    if (!G32(CRT_PFN_MESSAGEBOX_VA)) {
        HMODULE m = kLoadLibraryA((const char*)(uintptr_t)CRT_STR_USER32_VA);
        if (!m) return 0;
        const auto gpa = *(decltype(&GetProcAddress) volatile*)(uintptr_t)SLOT_GetProcAddress;
        G32(CRT_PFN_MESSAGEBOX_VA) = (uint32_t)(uintptr_t)gpa(m, (const char*)(uintptr_t)CRT_STR_MESSAGEBOXA_VA);
        if (!G32(CRT_PFN_MESSAGEBOX_VA)) return 0;
        G32(CRT_PFN_GETACTIVEWINDOW_VA) = (uint32_t)(uintptr_t)gpa(m, (const char*)(uintptr_t)CRT_STR_GETACTIVEWINDOW_VA);
        G32(CRT_PFN_GETLASTACTIVEPOPUP_VA) = (uint32_t)(uintptr_t)gpa(m, (const char*)(uintptr_t)CRT_STR_GETLASTACTIVEPOPUP_VA);
    }
    if (const uint32_t gaw = G32(CRT_PFN_GETACTIVEWINDOW_VA)) owner = ((GAW_t)(uintptr_t)gaw)();
    if (owner && G32(CRT_PFN_GETLASTACTIVEPOPUP_VA)) owner = ((GLAP_t)(uintptr_t)G32(CRT_PFN_GETLASTACTIVEPOPUP_VA))(owner);
    return ((MB_t)(uintptr_t)G32(CRT_PFN_MESSAGEBOX_VA))(owner, text, caption, type);
}
static void fp_crtMessageBoxA(Footprint& f, const char*, const char*, uint32_t) { f.replay_only = "a message box"; }
PORT_FN(0x004d6850, "crtMessageBoxA", crt_crtMessageBoxA, fp_crtMessageBoxA)

// _initterm (0x4d1d30): call each non-null pointer in [begin, end)
static void __cdecl crt_initterm(uint32_t* begin, uint32_t* end) {
    for (; begin < end; begin++)
        if (*begin) gcall<void>(*begin);
}
static void fp_initterm(Footprint& f, uint32_t*, uint32_t*) { fp_runtime(f); }
PORT_FN(0x004d1d30, "initterm", crt_initterm, fp_initterm)

// _cinit (0x4d1c40): _FPinit (_fpmath), then the C initialisers (__xi: __onexitinit, __initstdio) and the C++ ones
// (__xc: the game's 2,508 $E functions)
static void __cdecl crt_cinit() {
    if (const uint32_t fpinit = G32(CRT_FPINIT_VA)) gcall<void>(fpinit);
    gcall<void>(F_initterm, (uint32_t)CRT_XI_A, (uint32_t)CRT_XI_Z);
    gcall<void>(F_initterm, (uint32_t)CRT_XC_A, (uint32_t)CRT_XC_Z);
}
PORT_FN(0x004d1c40, "cinit", crt_cinit, fp_runtime)

// doexit (0x4d1cb0): the atexit functions last-first (unless quick), the pre-terminators (__endstdio), the
// terminators, then ExitProcess (unless the caller wants to return)
static void __cdecl crt_doexit(int32_t code, int32_t quick, int32_t retcaller) {
    G32(CRT_C_TERMINATION_DONE_VA) = 1;
    G8(CRT_EXITFLAG_VA) = (uint8_t)retcaller;
    if (!quick) {
        if (G32(CRT_ONEXITBEGIN_VA)) {
            for (uint32_t p = G32(CRT_ONEXITEND_VA) - 4; p >= G32(CRT_ONEXITBEGIN_VA); p -= 4) {
                const uint32_t fn = *(volatile uint32_t*)(uintptr_t)p;
                if (fn) gcall<void>(fn);
            }
        }
        gcall<void>(F_initterm, (uint32_t)CRT_XP_A, (uint32_t)CRT_XP_Z);
    }
    gcall<void>(F_initterm, (uint32_t)CRT_XT_A, (uint32_t)CRT_XT_Z);
    if (!retcaller) kExitProcess((UINT)code);
}
static void fp_doexit(Footprint& f, int32_t, int32_t, int32_t) { fp_runtime(f); }
PORT_FN(0x004d1cb0, "doexit", crt_doexit, fp_doexit)

// exit (0x4d1c70), _exit (0x4d1c90)
static void __cdecl crt_exit(int32_t code) { gcall<void>(F_doexit, code, 0, 0); }
static void fp_exit(Footprint& f, int32_t) { fp_runtime(f); }
PORT_FN(0x004d1c70, "exit", crt_exit, fp_exit)
static void __cdecl crt__exit(int32_t code) { gcall<void>(F_doexit, code, 1, 0); }
PORT_FN(0x004d1c90, "_exit", crt__exit, fp_exit)

// =====================================================================================================================
// heapinit.obj, ioinit.obj
// =====================================================================================================================
// _heap_init (0x4d4470): _crtheap = HeapCreate(HEAP_NO_SERIALIZE, 4096, 0) (the handle is left in eax)
static uint32_t __cdecl crt_heap_init() {
    const uint32_t h = (uint32_t)(uintptr_t)kHeapCreate(HEAP_NO_SERIALIZE, 0x1000, 0);
    G32(CRT_CRTHEAP_VA) = h;
    return h;
}
PORT_FN(0x004d4470, "heap_init", crt_heap_init, fp_runtime)

// a fresh block of 32 ioinfo entries: closed, no handle, pipe character LF (the original re-reads the block pointer
// for the loop's end)
static __forceinline void ioinfo_block_init(uint8_t* p, const volatile uint32_t* blk) {
    for (;;) {
        p[4] = 0;
        p += 8;
        *(uint32_t*)(p - 8) = 0xffffffffu;
        p[-3] = 10;
        if (!(*blk + 0x100 > (uint32_t)(uintptr_t)p)) break;
    }
}
// _ioinit (0x4d4290): the handle table -- inherited handles (STARTUPINFO's lpReserved2: a count, the osfile bytes, the
// handles; at most 2048), then 0..2 from the standard handles
static void __cdecl crt_ioinit() {
    uint8_t* p = gcall<uint8_t*>(F_malloc, 0x100u);
    if (!p) gcall<void>(F_amsg_exit, 0x1b);
    G32(CRT_PIOINFO_VA) = (uint32_t)(uintptr_t)p;
    G32(CRT_NHANDLE_VA) = 0x20;
    if (p + 0x100 > p) ioinfo_block_init(p, (const volatile uint32_t*)(uintptr_t)CRT_PIOINFO_VA);
    STARTUPINFOA si;
    kGetStartupInfoA(&si);
    if (si.cbReserved2 && si.lpReserved2) {
        int32_t n = *(int32_t*)si.lpReserved2;
        uint8_t* posfile = si.lpReserved2 + 4;
        uint32_t* posfhnd = (uint32_t*)(posfile + n);
        if (n >= 0x800) n = 0x800;
        if (n > (int32_t)G32(CRT_NHANDLE_VA)) {
            volatile uint32_t* blk = (volatile uint32_t*)(uintptr_t)(CRT_PIOINFO_VA + 4);
            for (;;) {
                uint8_t* q = gcall<uint8_t*>(F_malloc, 0x100u);
                if (!q) {
                    n = (int32_t)G32(CRT_NHANDLE_VA);
                    break;
                }
                *blk = (uint32_t)(uintptr_t)q;
                G32(CRT_NHANDLE_VA) += 0x20;
                if (q + 0x100 > q) ioinfo_block_init(q, blk);
                blk++;
                if (!(n > (int32_t)G32(CRT_NHANDLE_VA))) break;
            }
        }
        for (int32_t fh = 0; fh < n; fh++, posfile++, posfhnd++) {
            if (*posfhnd != 0xffffffffu && (*posfile & CRT_FOPEN) && kGetFileType((HANDLE)(uintptr_t)*posfhnd)) {
                CrtIoinfo* io = crt_ioinfo(fh);
                io->osfhnd = (intptr_t)*posfhnd;
                io->osfile = *posfile;
            }
        }
    }
    const auto get_std = *(decltype(&GetStdHandle) volatile*)(uintptr_t)SLOT_GetStdHandle;   // read once
    for (int32_t fh = 0; fh < 3; fh++) {
        CrtIoinfo* io = (CrtIoinfo*)(uintptr_t)(G32(CRT_PIOINFO_VA) + fh * 8);
        if (io->osfhnd == -1) {
            io->osfile = CRT_FOPEN | CRT_FTEXT;
            const DWORD which = fh == 0 ? STD_INPUT_HANDLE : fh == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE;
            HANDLE h = get_std(which);
            DWORD type;
            if (h != INVALID_HANDLE_VALUE && (type = kGetFileType(h)) != 0) {
                type &= 0xff;
                io->osfhnd = (intptr_t)h;
                if (type == FILE_TYPE_CHAR) io->osfile |= CRT_FDEV;
                else if (type == FILE_TYPE_PIPE) io->osfile |= CRT_FPIPE;
            } else {
                io->osfile |= CRT_FDEV;
            }
        } else {
            io->osfile |= CRT_FTEXT;
        }
    }
    kSetHandleCount(G32(CRT_NHANDLE_VA));
}
PORT_FN(0x004d4290, "ioinit", crt_ioinit, fp_runtime)

// =====================================================================================================================
// stdargv.obj, stdenvp.obj, aw_env.obj
// =====================================================================================================================
static __forceinline bool mb_lead(uint8_t c) { return (G8(CRT_MBCTYPE_VA + 1 + c) & 4) != 0; }

// parse_cmdline (0x4d3c40): the program name (quoted or not), then the arguments -- 2N backslashes + quote -> N
// backslashes and a quote toggle, 2N+1 + quote -> N and a literal quote, "" inside quotes a literal quote. With argv /
// args null it only counts. Kept: a quoted program name's lead byte isn't skipped while only counting (as compiled).
static void __cdecl crt_parse_cmdline(const uint8_t* p, uint32_t* argv, uint8_t* args, int32_t* numargs,
                                      int32_t* numchars) {
    *numchars = 0;
    *numargs = 1;
    if (argv) *argv++ = (uint32_t)(uintptr_t)args;
    if (*p == '"') {
        p++;
        while (*p != '"' && *p) {
            if (mb_lead(*p)) {
                ++*numchars;
                if (args) *args++ = *p++;
            }
            ++*numchars;
            if (args) *args++ = *p;
            p++;
        }
        ++*numchars;
        if (args) *args++ = 0;
        if (*p == '"') p++;
    } else {
        uint8_t c;
        do {
            ++*numchars;
            if (args) *args++ = *p;
            c = *p++;
            if (mb_lead(c)) {
                ++*numchars;
                if (args) *args++ = *p;
                p++;
            }
        } while (c != ' ' && c && c != '\t');
        if (!c) p--;
        else if (args) args[-1] = 0;
    }
    int32_t inquote = 0;
    for (;;) {
        if (!*p) break;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argv) *argv++ = (uint32_t)(uintptr_t)args;
        ++*numargs;
        for (;;) {
            int32_t copychar = 1;
            uint32_t numslash = 0;
            while (*p == '\\') {
                p++;
                numslash++;
            }
            if (*p == '"') {
                if (!(numslash & 1)) {
                    if (inquote && p[1] == '"') p++;
                    else copychar = 0;
                    inquote = !inquote;
                }
                numslash >>= 1;
            }
            while (numslash--) {
                if (args) *args++ = '\\';
                ++*numchars;
            }
            const uint8_t c = *p;
            if (!c || (!inquote && (c == ' ' || c == '\t'))) break;
            if (copychar) {
                if (args) {
                    if (mb_lead(c)) {
                        *args++ = c;
                        p++;
                        ++*numchars;
                    }
                    *args++ = *p;
                    p++;
                    ++*numchars;
                } else {
                    if (mb_lead(c)) {
                        p++;
                        ++*numchars;
                    }
                    ++*numchars;
                    p++;
                }
            } else {
                p++;
            }
        }
        if (args) *args++ = 0;
        ++*numchars;
    }
    if (argv) *argv++ = 0;
    ++*numargs;
}
static void fp_parse_cmdline(Footprint& f, const uint8_t*, uint32_t*, uint8_t*, int32_t*, int32_t*) { fp_runtime(f); }
PORT_FN(0x004d3c40, "parse_cmdline", crt_parse_cmdline, fp_parse_cmdline)

// _setargv (0x4d3ba0): _pgmptr = the module's path (0x5d5610), __argv / __argc from _acmdln (or the path, when the
// command line is empty), in one block: the pointers, then the strings
static int32_t __cdecl crt_setargv() {
    char* const pgm = (char*)(uintptr_t)CRT_PGMNAME_VA;
    kGetModuleFileNameA(0, pgm, 0x104);
    G32(CRT_PGMPTR_VA) = CRT_PGMNAME_VA;
    const uint8_t* start = *(const uint8_t*)(uintptr_t)G32(CRT_ACMDLN_VA) ? (const uint8_t*)(uintptr_t)G32(CRT_ACMDLN_VA)
                                                                          : (const uint8_t*)pgm;
    int32_t numargs, numchars;
    gcall<void>(F_parse_cmdline, start, (uint32_t*)0, (uint8_t*)0, &numargs, &numchars);
    uint32_t* p = gcall<uint32_t*>(F_malloc, (uint32_t)(numargs * 4 + numchars));
    if (!p) gcall<void>(F_amsg_exit, 8);
    gcall<void>(F_parse_cmdline, start, p, (uint8_t*)(p + numargs), &numargs, &numchars);
    G32(CRT_ARGV_VA) = (uint32_t)(uintptr_t)p;
    G32(CRT_ARGC_VA) = (uint32_t)(numargs - 1);
    return numargs - 1;
}
PORT_FN(0x004d3ba0, "setargv", crt_setargv, fp_runtime)

// _setenvp (0x4d3ac0): _environ from _aenvptr, one malloc per string ("=X:" drive entries left out), then the block
// freed (_aenvptr isn't cleared)
static void __cdecl crt_setenvp() {
    const char* e = (const char*)(uintptr_t)G32(CRT_AENVPTR_VA);
    int32_t n = 0;
    while (*e) {
        if (*e != '=') n++;
        e += cstrlen(e) + 1;
    }
    uint32_t* env = gcall<uint32_t*>(F_malloc, (uint32_t)(n * 4 + 4));
    G32(CRT_ENVIRON_VA) = (uint32_t)(uintptr_t)env;
    if (!env) gcall<void>(F_amsg_exit, 9);
    for (const char* s = (const char*)(uintptr_t)G32(CRT_AENVPTR_VA); *s;) {
        const size_t len = cstrlen(s) + 1;
        if (*s != '=') {
            *env = gcall<uint32_t>(F_malloc, (uint32_t)len);
            if (!*env) gcall<void>(F_amsg_exit, 9);
            const size_t len2 = cstrlen(s) + 1;
            char* d = (char*)(uintptr_t)*env;
            env++;
            for (size_t i = 0; i < len2; i++) d[i] = s[i];
        }
        s += len;
    }
    gcall<void>(F_free, G32(CRT_AENVPTR_VA));
    *env = 0;
}
PORT_FN(0x004d3ac0, "setenvp", crt_setenvp, fp_runtime)

// __crtGetEnvironmentStringsA (0x4d3e20): the environment as one ANSI block in a malloc'd copy -- from the wide
// strings when Windows has them (f_use 1), else the ANSI ones (2). Naked: on a call after the first (f_use already
// set) the original uses an uninitialised local as the strings' pointer, which only the same frame reproduces.
static __declspec(naked) void crt_crtGetEnvironmentStringsA() {
#ifdef VP_GCC
    __asm__ volatile(
        "sub esp, 4\n\t"
        "cmp dword ptr ds:[0x502af8], 0\n\t"
        "push ebx\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "push ebp\n\t"
        "jne again%=\n\t"
        "mov esi, dword ptr ds:[0x5d75d0]\n\t"
        "call esi\n\t"
        "%{load%} mov edi, eax\n\t"
        "test edi, edi\n\t"
        "je try_ansi%=\n\t"
        "mov dword ptr ds:[0x502af8], 1\n\t"
        "mov ebx, dword ptr [esp + 0x10]\n\t"
        "jmp have%=\n\t"
        "try_ansi%=:\n\t"
        "call dword ptr ds:[0x5d75c8]\n\t"
        "%{load%} mov ebx, eax\n\t"
        "test ebx, ebx\n\t"
        "je none_a%=\n\t"
        "mov dword ptr ds:[0x502af8], 2\n\t"
        "jmp have%=\n\t"
        "none_a%=:\n\t"
        "%{load%} xor eax, eax\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "add esp, 4\n\t"
        "ret\n\t"
        "again%=:\n\t"
        "mov edi, dword ptr [esp + 0x10]\n\t"
        "mov ebx, dword ptr [esp + 0x10]\n\t"
        "mov esi, dword ptr ds:[0x5d75d0]\n\t"
        "have%=:\n\t"
        "cmp dword ptr ds:[0x502af8], 1\n\t"
        "jne not_wide%=\n\t"
        "test edi, edi\n\t"
        "jne wide_ok%=\n\t"
        "call esi\n\t"
        "%{load%} mov edi, eax\n\t"
        "test edi, edi\n\t"
        "jne wide_ok%=\n\t"
        "%{load%} xor eax, eax\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "add esp, 4\n\t"
        "ret\n\t"
        "wide_ok%=:\n\t"
        "cmp word ptr [edi], 0\n\t"
        "%{load%} mov esi, edi\n\t"
        "je wend%=\n\t"
        "wscan%=:\n\t"
        "add esi, 2\n\t"
        "cmp word ptr [esi], 0\n\t"
        "jne wscan%=\n\t"
        "add esi, 2\n\t"
        "cmp word ptr [esi], 0\n\t"
        "jne wscan%=\n\t"
        "wend%=:\n\t"
        "%{load%} sub esi, edi\n\t"
        "push 0\n\t"
        "sar esi, 1\n\t"
        "push 0\n\t"
        "inc esi\n\t"
        "push 0\n\t"
        "push 0\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "push 0\n\t"
        "push 0\n\t"
        "call dword ptr ds:[0x5d75d4]\n\t"
        "%{load%} mov ebp, eax\n\t"
        "test ebp, ebp\n\t"
        "je wfail%=\n\t"
        "push ebp\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 4\n\t"
        "%{load%} mov ebx, eax\n\t"
        "test ebx, ebx\n\t"
        "je wfail%=\n\t"
        "push 0\n\t"
        "push 0\n\t"
        "push ebp\n\t"
        "push ebx\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "push 0\n\t"
        "push 0\n\t"
        "call dword ptr ds:[0x5d75d4]\n\t"
        "test eax, eax\n\t"
        "jne wdone%=\n\t"
        "push ebx\n\t"
        "call dword ptr [%c1]\n\t"
        "add esp, 4\n\t"
        "%{load%} xor ebx, ebx\n\t"
        "wdone%=:\n\t"
        "push edi\n\t"
        "call dword ptr ds:[0x5d75cc]\n\t"
        "%{load%} mov eax, ebx\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "add esp, 4\n\t"
        "ret\n\t"
        "wfail%=:\n\t"
        "push edi\n\t"
        "call dword ptr ds:[0x5d75cc]\n\t"
        "%{load%} xor eax, eax\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "add esp, 4\n\t"
        "ret\n\t"
        "not_wide%=:\n\t"
        "cmp dword ptr ds:[0x502af8], 2\n\t"
        "jne neither%=\n\t"
        "test ebx, ebx\n\t"
        "jne ansi_ok%=\n\t"
        "call dword ptr ds:[0x5d75c8]\n\t"
        "%{load%} mov ebx, eax\n\t"
        "test ebx, ebx\n\t"
        "jne ansi_ok%=\n\t"
        "%{load%} xor eax, eax\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "add esp, 4\n\t"
        "ret\n\t"
        "ansi_ok%=:\n\t"
        "%{load%} mov ebp, ebx\n\t"
        "cmp byte ptr [ebx], 0\n\t"
        "je aend%=\n\t"
        "ascan%=:\n\t"
        "inc ebp\n\t"
        "cmp byte ptr [ebp], 0\n\t"
        "jne ascan%=\n\t"
        "inc ebp\n\t"
        "cmp byte ptr [ebp], 0\n\t"
        "jne ascan%=\n\t"
        "aend%=:\n\t"
        "%{load%} sub ebp, ebx\n\t"
        "inc ebp\n\t"
        "push ebp\n\t"
        "call dword ptr [%c0]\n\t"
        "mov dword ptr [esp + 0x14], eax\n\t"
        "add esp, 4\n\t"
        "test eax, eax\n\t"
        "jne acopy%=\n\t"
        "push ebx\n\t"
        "call dword ptr ds:[0x5d75c0]\n\t"
        "%{load%} xor eax, eax\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "add esp, 4\n\t"
        "ret\n\t"
        "acopy%=:\n\t"
        "mov edi, dword ptr [esp + 0x10]\n\t"
        "%{load%} mov esi, ebx\n\t"
        "%{load%} mov ecx, ebp\n\t"
        "shr ecx, 2\n\t"
        "rep movsd\n\t"
        "%{load%} mov ecx, ebp\n\t"
        "push ebx\n\t"
        "and ecx, 3\n\t"
        "rep movsb\n\t"
        "call dword ptr ds:[0x5d75c0]\n\t"
        "mov eax, dword ptr [esp + 0x10]\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "add esp, 4\n\t"
        "ret\n\t"
        "neither%=:\n\t"
        "%{load%} xor eax, eax\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "add esp, 4\n\t"
        "ret"
        : : "i"(&a_malloc), "i"(&a_free));
#else
    __asm {
        sub esp, 4
        cmp dword ptr ds:[0x502af8], 0
        push ebx
        push esi
        push edi
        push ebp
        jne again
        mov esi, dword ptr ds:[0x5d75d0]
        call esi
        mov edi, eax
        test edi, edi
        je try_ansi
        mov dword ptr ds:[0x502af8], 1
        mov ebx, dword ptr [esp + 0x10]
        jmp have
    try_ansi:
        call dword ptr ds:[0x5d75c8]
        mov ebx, eax
        test ebx, ebx
        je none_a
        mov dword ptr ds:[0x502af8], 2
        jmp have
    none_a:
        xor eax, eax
        pop ebp
        pop edi
        pop esi
        pop ebx
        add esp, 4
        ret
    again:
        mov edi, dword ptr [esp + 0x10]
        mov ebx, dword ptr [esp + 0x10]
        mov esi, dword ptr ds:[0x5d75d0]
    have:
        cmp dword ptr ds:[0x502af8], 1
        jne not_wide
        test edi, edi
        jne wide_ok
        call esi
        mov edi, eax
        test edi, edi
        jne wide_ok
        xor eax, eax
        pop ebp
        pop edi
        pop esi
        pop ebx
        add esp, 4
        ret
    wide_ok:
        cmp word ptr [edi], 0
        mov esi, edi
        je wend
    wscan:
        add esi, 2
        cmp word ptr [esi], 0
        jne wscan
        add esi, 2
        cmp word ptr [esi], 0
        jne wscan
    wend:
        sub esi, edi
        push 0
        sar esi, 1
        push 0
        inc esi
        push 0
        push 0
        push esi
        push edi
        push 0
        push 0
        call dword ptr ds:[0x5d75d4]
        mov ebp, eax
        test ebp, ebp
        je wfail
        push ebp
        call dword ptr [a_malloc]
        add esp, 4
        mov ebx, eax
        test ebx, ebx
        je wfail
        push 0
        push 0
        push ebp
        push ebx
        push esi
        push edi
        push 0
        push 0
        call dword ptr ds:[0x5d75d4]
        test eax, eax
        jne wdone
        push ebx
        call dword ptr [a_free]
        add esp, 4
        xor ebx, ebx
    wdone:
        push edi
        call dword ptr ds:[0x5d75cc]
        mov eax, ebx
        pop ebp
        pop edi
        pop esi
        pop ebx
        add esp, 4
        ret
    wfail:
        push edi
        call dword ptr ds:[0x5d75cc]
        xor eax, eax
        pop ebp
        pop edi
        pop esi
        pop ebx
        add esp, 4
        ret
    not_wide:
        cmp dword ptr ds:[0x502af8], 2
        jne neither
        test ebx, ebx
        jne ansi_ok
        call dword ptr ds:[0x5d75c8]
        mov ebx, eax
        test ebx, ebx
        jne ansi_ok
        xor eax, eax
        pop ebp
        pop edi
        pop esi
        pop ebx
        add esp, 4
        ret
    ansi_ok:
        mov ebp, ebx
        cmp byte ptr [ebx], 0
        je aend
    ascan:
        inc ebp
        cmp byte ptr [ebp], 0
        jne ascan
        inc ebp
        cmp byte ptr [ebp], 0
        jne ascan
    aend:
        sub ebp, ebx
        inc ebp
        push ebp
        call dword ptr [a_malloc]
        mov dword ptr [esp + 0x14], eax
        add esp, 4
        test eax, eax
        jne acopy
        push ebx
        call dword ptr ds:[0x5d75c0]
        xor eax, eax
        pop ebp
        pop edi
        pop esi
        pop ebx
        add esp, 4
        ret
    acopy:
        mov edi, dword ptr [esp + 0x10]
        mov esi, ebx
        mov ecx, ebp
        shr ecx, 2
        rep movsd
        mov ecx, ebp
        push ebx
        and ecx, 3
        rep movsb
        call dword ptr ds:[0x5d75c0]
        mov eax, dword ptr [esp + 0x10]
        pop ebp
        pop edi
        pop esi
        pop ebx
        add esp, 4
        ret
    neither:
        xor eax, eax
        pop ebp
        pop edi
        pop esi
        pop ebx
        add esp, 4
        ret
    }
#endif
}
PORT_FN(0x004d3e20, "crtGetEnvironmentStringsA", crt_crtGetEnvironmentStringsA, fp_asm)
CRT_START_NO_SHADOW(crt_crtGetEnvironmentStringsA)

// =====================================================================================================================
// closeall.obj
// =====================================================================================================================
// _fcloseall (0x4d71d0): every stream past the three standard ones -- closed if open; the ones past _iob (20 and up)
// freed and their __piob slot cleared, open or not. Returns how many closed.
static int32_t __cdecl crt_fcloseall() {
    int32_t n = 0;
    for (int32_t i = 3; (int32_t)G32(CRT_NSTREAM_VA) > i; i++) {
        CrtFile* f = (CrtFile*)(uintptr_t)((uint32_t*)(uintptr_t)G32(CRT_PIOB_VA))[i];
        if (!f) continue;
        if (f->flag & (CRT_IOREAD | CRT_IOWRT | CRT_IORW))
            if (gcall<int32_t>(F_fclose, f) != -1) n++;
        if (i >= 20) {
            gcall<void>(F_free, ((uint32_t*)(uintptr_t)G32(CRT_PIOB_VA))[i]);
            ((uint32_t*)(uintptr_t)G32(CRT_PIOB_VA))[i] = 0;
        }
    }
    return n;
}
PORT_FN(0x004d71d0, "fcloseall", crt_fcloseall, fp_runtime)

// =====================================================================================================================
// winxfltr.obj: the start-up frame's filter -- signal()'s actions for the exceptions it knows (_XcptActTab)
// =====================================================================================================================
// xcptlookup (0x4d3a30): the table entry for an exception code, or 0
static uint32_t __cdecl crt_xcptlookup(uint32_t xcpt) {
    uint32_t e = CRT_XCPTACTTAB_VA;
    for (;;) {
        if (G32(e) == xcpt) break;
        e += 12;
        if (!(CRT_XCPTACTTAB_VA + G32(CRT_XCPTACTTABCOUNT_VA) * 12 > e)) break;
    }
    return G32(e) == xcpt ? e : 0;
}
static void fp_xcptlookup(Footprint&, uint32_t) {}                          // writes nothing
PORT_FN(0x004d3a30, "xcptlookup", crt_xcptlookup, fp_xcptlookup)

// _XcptFilter (0x4d38d0): no action (or SIG_DFL) -> UnhandledExceptionFilter; SIG_IGN (1) -> continue execution;
// SIG_DIE (5) -> reset, execute the handler; else call the signal handler (SIGFPE with _fpecode for the float codes,
// every SIGFPE entry reset first) and continue execution
static int32_t __cdecl crt_XcptFilter(uint32_t xcpt, EXCEPTION_POINTERS* ptrs) {
    const uint32_t e = gcall<uint32_t>(F_xcptlookup, xcpt);
    uint32_t h;
    if (!e || !(h = G32(e + 8))) return kUnhandledExceptionFilter(ptrs);
    if (h == 5) {
        G32(e + 8) = 0;
        return 1;
    }
    if (h == 1) return -1;
    const uint32_t old = G32(CRT_PXCPTINFOPTRS_VA);
    G32(CRT_PXCPTINFOPTRS_VA) = (uint32_t)(uintptr_t)ptrs;
    if (G32(e + 4) == 8) {
        if ((int32_t)G32(CRT_FIRST_FPE_INDX_VA) < (int32_t)(G32(CRT_NUM_FPE_VA) + G32(CRT_FIRST_FPE_INDX_VA))) {
            uint32_t a = G32(CRT_FIRST_FPE_INDX_VA) * 12 + CRT_XCPTACTTAB_VA + 8;
            for (uint32_t k = G32(CRT_NUM_FPE_VA); k; k--, a += 12) G32(a) = 0;
        }
        const uint32_t oldfpe = G32(CRT_FPECODE_VA);
        switch (G32(e)) {
        case 0xc000008e: G32(CRT_FPECODE_VA) = 0x83; break;
        case 0xc0000090: G32(CRT_FPECODE_VA) = 0x81; break;
        case 0xc0000091: G32(CRT_FPECODE_VA) = 0x84; break;
        case 0xc0000093: G32(CRT_FPECODE_VA) = 0x85; break;
        case 0xc000008d: G32(CRT_FPECODE_VA) = 0x82; break;
        case 0xc000008f: G32(CRT_FPECODE_VA) = 0x86; break;
        case 0xc0000092: G32(CRT_FPECODE_VA) = 0x8a; break;
        }
        gcall<void>(h, 8, G32(CRT_FPECODE_VA));
        G32(CRT_FPECODE_VA) = oldfpe;
    } else {
        G32(e + 8) = 0;
        gcall<void>(h, G32(e + 4));
    }
    G32(CRT_PXCPTINFOPTRS_VA) = old;
    return -1;
}
static void fp_XcptFilter(Footprint& f, uint32_t, EXCEPTION_POINTERS*) { f.replay_only = "exception handling"; }
PORT_FN(0x004d38d0, "XcptFilter", crt_XcptFilter, fp_XcptFilter)

// =====================================================================================================================
// _WinMainCRTStartup (0x4cf5a0): the process entry. An SEH frame (_except_handler3, a one-level scope table whose
// filter is _XcptFilter and whose handler is _exit(code)), the Windows version, the heap, the handle table, the
// multibyte table, the command line and environment (exit(-1) without either), argv / environ, _cinit, then WinMain
// with the command line past the program name, and exit(WinMain's result).
// The scope table is this file's: the original's (0x4e01a0) points at its own filter and handler code inside the
// function, which the standalone fills with int3.
// =====================================================================================================================
static __declspec(naked) void crt_wmcs_filter() {
#ifdef VP_GCC
    __asm__ volatile(
        "mov eax, dword ptr [ebp - 0x14]\n\t"
        "mov eax, dword ptr [eax]\n\t"
        "mov eax, dword ptr [eax]\n\t"
        "mov dword ptr [ebp - 0x20], eax\n\t"
        "mov eax, dword ptr [ebp - 0x14]\n\t"
        "push eax\n\t"
        "mov eax, dword ptr [ebp - 0x20]\n\t"
        "push eax\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 8\n\t"
        "ret"
        : : "i"(&a_XcptFilter));
#else
    __asm {
        mov eax, dword ptr [ebp - 0x14]
        mov eax, dword ptr [eax]
        mov eax, dword ptr [eax]
        mov dword ptr [ebp - 0x20], eax
        mov eax, dword ptr [ebp - 0x14]
        push eax
        mov eax, dword ptr [ebp - 0x20]
        push eax
        call dword ptr [a_XcptFilter]
        add esp, 8
        ret
    }
#endif
}
static __declspec(naked) void crt_wmcs_handler() {
#ifdef VP_GCC
    __asm__ volatile(
        "mov esp, dword ptr [ebp - 0x18]\n\t"
        "mov eax, dword ptr [ebp - 0x20]\n\t"
        "push eax\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 4\n\t"
        "mov dword ptr [ebp - 4], 0xffffffff\n\t"
        "mov eax, dword ptr [ebp - 0x10]\n\t"
        "pop edi\n\t"
        "mov dword ptr fs:[0], eax\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "%{load%} mov esp, ebp\n\t"
        "pop ebp\n\t"
        "ret"
        : : "i"(&a__exit));
#else
    __asm {
        mov esp, dword ptr [ebp - 0x18]
        mov eax, dword ptr [ebp - 0x20]
        push eax
        call dword ptr [a__exit]
        add esp, 4
        mov dword ptr [ebp - 4], 0xffffffff
        mov eax, dword ptr [ebp - 0x10]
        pop edi
        mov dword ptr fs:[0], eax
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret
    }
#endif
}
static const uint32_t k_wmcs_scope[3] = {0xffffffffu, (uint32_t)(uintptr_t)&crt_wmcs_filter,
                                         (uint32_t)(uintptr_t)&crt_wmcs_handler};
static void __cdecl wmcs_islands() {
#ifndef VP_CRT_HARNESS
    crt_islands_install();
#endif
}
static __declspec(naked) void crt_WinMainCRTStartup() {
#ifdef VP_GCC
    __asm__ volatile(
        "call %P0\n\t"
        "mov eax, dword ptr fs:[0]\n\t"
        "push ebp\n\t"
        "%{load%} mov ebp, esp\n\t"
        "push 0xffffffff\n\t"
        "push %1\n\t"
        "push 0x004d4490\n\t"
        "push eax\n\t"
        "mov dword ptr fs:[0], esp\n\t"
        "sub esp, 0x60\n\t"
        "push ebx\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "mov dword ptr [ebp - 0x18], esp\n\t"
        "call dword ptr ds:[0x5d7558]\n\t"
        "mov dword ptr ds:[0x50273c], eax\n\t"
        "%{load%} xor eax, eax\n\t"
        "mov al, byte ptr ds:[0x50273d]\n\t"
        "mov dword ptr ds:[0x502748], eax\n\t"
        "mov eax, dword ptr ds:[0x50273c]\n\t"
        "shr dword ptr ds:[0x50273c], 0x10\n\t"
        "and eax, 0xff\n\t"
        "mov dword ptr ds:[0x502744], eax\n\t"
        "shl eax, 8\n\t"
        "add eax, dword ptr ds:[0x502748]\n\t"
        "mov dword ptr ds:[0x502740], eax\n\t"
        "call dword ptr [%c2]\n\t"
        "mov dword ptr [ebp - 4], 0\n\t"
        "call dword ptr [%c3]\n\t"
        "call dword ptr [%c4]\n\t"
        "call dword ptr ds:[0x5d75ac]\n\t"
        "mov dword ptr ds:[0x5d6f28], eax\n\t"
        "call dword ptr [%c5]\n\t"
        "mov dword ptr ds:[0x502680], eax\n\t"
        "test eax, eax\n\t"
        "je no_env%=\n\t"
        "cmp dword ptr ds:[0x5d6f28], 0\n\t"
        "jne have_both%=\n\t"
        "no_env%=:\n\t"
        "push 0xffffffff\n\t"
        "call dword ptr [%c6]\n\t"
        "add esp, 4\n\t"
        "have_both%=:\n\t"
        "call dword ptr [%c7]\n\t"
        "call dword ptr [%c8]\n\t"
        "call dword ptr [%c9]\n\t"
        "mov esi, dword ptr ds:[0x5d6f28]\n\t"
        "mov al, byte ptr [esi]\n\t"
        "cmp al, 0x22\n\t"
        "je quoted%=\n\t"
        "cmp al, 0x20\n\t"
        "jbe skip_ws%=\n\t"
        "unq%=:\n\t"
        "inc esi\n\t"
        "cmp byte ptr [esi], 0x20\n\t"
        "ja unq%=\n\t"
        "jmp skip_ws%=\n\t"
        "quoted%=:\n\t"
        "inc esi\n\t"
        "cmp byte ptr [esi], 0x22\n\t"
        "je q_end%=\n\t"
        "mov bl, byte ptr [ebp - 0x28]\n\t"
        "q_loop%=:\n\t"
        "mov bl, byte ptr [esi]\n\t"
        "test bl, bl\n\t"
        "je q_close%=\n\t"
        "%{load%} xor eax, eax\n\t"
        "%{load%} mov al, bl\n\t"
        "push eax\n\t"
        "call dword ptr [%c10]\n\t"
        "add esp, 4\n\t"
        "test eax, eax\n\t"
        "je q_next%=\n\t"
        "inc esi\n\t"
        "q_next%=:\n\t"
        "inc esi\n\t"
        "cmp byte ptr [esi], 0x22\n\t"
        "jne q_loop%=\n\t"
        "q_close%=:\n\t"
        "cmp byte ptr [esi], 0x22\n\t"
        "jne skip_ws%=\n\t"
        "q_end%=:\n\t"
        "inc esi\n\t"
        "skip_ws%=:\n\t"
        "cmp byte ptr [esi], 0\n\t"
        "je args_done%=\n\t"
        "ws_loop%=:\n\t"
        "cmp byte ptr [esi], 0x20\n\t"
        "ja args_done%=\n\t"
        "inc esi\n\t"
        "cmp byte ptr [esi], 0\n\t"
        "jne ws_loop%=\n\t"
        "args_done%=:\n\t"
        "mov dword ptr [ebp - 0x44], 0\n\t"
        "lea eax, [ebp - 0x70]\n\t"
        "push eax\n\t"
        "call dword ptr ds:[0x5d75f8]\n\t"
        "test byte ptr [ebp - 0x44], 1\n\t"
        "mov eax, 0xa\n\t"
        "je show_default%=\n\t"
        "mov eax, dword ptr [ebp - 0x40]\n\t"
        "and eax, 0xffff\n\t"
        "show_default%=:\n\t"
        "push eax\n\t"
        "push esi\n\t"
        "push 0\n\t"
        "push 0\n\t"
        "call dword ptr ds:[0x5d75b4]\n\t"
        "push eax\n\t"
        "call dword ptr [%c11]\n\t"
        "push eax\n\t"
        "call dword ptr [%c6]\n\t"
        "add esp, 4\n\t"
        "mov dword ptr [ebp - 4], 0xffffffff\n\t"
        "mov eax, dword ptr [ebp - 0x10]\n\t"
        "pop edi\n\t"
        "mov dword ptr fs:[0], eax\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "%{load%} mov esp, ebp\n\t"
        "pop ebp\n\t"
        "ret"
        : : "i"(&wmcs_islands), "i"(&k_wmcs_scope), "i"(&a_heap_init), "i"(&a_ioinit), "i"(&a_initmbctable), "i"(&a_crtGetEnvironmentStringsA), "i"(&a_exit), "i"(&a_setargv), "i"(&a_setenvp), "i"(&a_cinit), "i"(&a_ismbblead), "i"(&a_WinMain));
#else
    __asm {
        call wmcs_islands                               // (not the original's: the islands, crt_types.h)
        mov eax, dword ptr fs:[0]
        push ebp
        mov ebp, esp
        push 0xffffffff
        push offset k_wmcs_scope
        push 0x004d4490                                 // _except_handler3
        push eax
        mov dword ptr fs:[0], esp
        sub esp, 0x60
        push ebx
        push esi
        push edi
        mov dword ptr [ebp - 0x18], esp
        call dword ptr ds:[0x5d7558]                    // GetVersion
        mov dword ptr ds:[0x50273c], eax
        xor eax, eax
        mov al, byte ptr ds:[0x50273d]
        mov dword ptr ds:[0x502748], eax
        mov eax, dword ptr ds:[0x50273c]
        shr dword ptr ds:[0x50273c], 0x10
        and eax, 0xff
        mov dword ptr ds:[0x502744], eax
        shl eax, 8
        add eax, dword ptr ds:[0x502748]
        mov dword ptr ds:[0x502740], eax
        call dword ptr [a_heap_init]
        mov dword ptr [ebp - 4], 0
        call dword ptr [a_ioinit]
        call dword ptr [a_initmbctable]
        call dword ptr ds:[0x5d75ac]                    // GetCommandLineA
        mov dword ptr ds:[0x5d6f28], eax
        call dword ptr [a_crtGetEnvironmentStringsA]
        mov dword ptr ds:[0x502680], eax
        test eax, eax
        je no_env
        cmp dword ptr ds:[0x5d6f28], 0
        jne have_both
    no_env:
        push 0xffffffff
        call dword ptr [a_exit]
        add esp, 4
    have_both:
        call dword ptr [a_setargv]
        call dword ptr [a_setenvp]
        call dword ptr [a_cinit]
        mov esi, dword ptr ds:[0x5d6f28]
        mov al, byte ptr [esi]
        cmp al, 0x22
        je quoted
        cmp al, 0x20
        jbe skip_ws
    unq:
        inc esi
        cmp byte ptr [esi], 0x20
        ja unq
        jmp skip_ws
    quoted:
        inc esi
        cmp byte ptr [esi], 0x22
        je q_end
        mov bl, byte ptr [ebp - 0x28]
    q_loop:
        mov bl, byte ptr [esi]
        test bl, bl
        je q_close
        xor eax, eax
        mov al, bl
        push eax
        call dword ptr [a_ismbblead]
        add esp, 4
        test eax, eax
        je q_next
        inc esi
    q_next:
        inc esi
        cmp byte ptr [esi], 0x22
        jne q_loop
    q_close:
        cmp byte ptr [esi], 0x22
        jne skip_ws
    q_end:
        inc esi
    skip_ws:
        cmp byte ptr [esi], 0
        je args_done
    ws_loop:
        cmp byte ptr [esi], 0x20
        ja args_done
        inc esi
        cmp byte ptr [esi], 0
        jne ws_loop
    args_done:
        mov dword ptr [ebp - 0x44], 0
        lea eax, [ebp - 0x70]
        push eax
        call dword ptr ds:[0x5d75f8]                    // GetStartupInfoA
        test byte ptr [ebp - 0x44], 1
        mov eax, 0xa
        je show_default
        mov eax, dword ptr [ebp - 0x40]
        and eax, 0xffff
    show_default:
        push eax
        push esi
        push 0
        push 0
        call dword ptr ds:[0x5d75b4]                    // GetModuleHandleA
        push eax
        call dword ptr [a_WinMain]
        push eax
        call dword ptr [a_exit]
        add esp, 4
        mov dword ptr [ebp - 4], 0xffffffff
        mov eax, dword ptr [ebp - 0x10]
        pop edi
        mov dword ptr fs:[0], eax
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret
    }
#endif
}
PORT_FN(0x004cf5a0, "WinMainCRTStartup", crt_WinMainCRTStartup, fp_asm)
CRT_START_NO_SHADOW(crt_WinMainCRTStartup)

// =====================================================================================================================
// exsup.obj, exsup3.obj: the SEH runtime the 1998 compiler's __try frames use. A frame: [ebp-0x18] saved esp, [-0x14]
// the exception pointers, [-0x10] the registration (next, handler = _except_handler3, scope table, try level at
// [-4]), the scope table's entries {enclosing level, filter (0: a __finally), handler}. Naked, instruction for
// instruction: the filters and handlers run in the frame's ebp; _NLG_Notify records each transfer for debuggers.
// =====================================================================================================================
// _global_unwind2 (0x4d37dc): RtlUnwind to the frame (its continuation is crt_gu_return)
static __declspec(naked) void crt_gu_return() {
#ifdef VP_GCC
    __asm__ volatile(
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "%{load%} mov esp, ebp\n\t"
        "pop ebp\n\t"
        "ret"
        : :);
#else
    __asm {
        pop ebp
        pop edi
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret
    }
#endif
}
static __declspec(naked) void crt_global_unwind2() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebp\n\t"
        "%{load%} mov ebp, esp\n\t"
        "push ebx\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "push ebp\n\t"
        "push 0\n\t"
        "push 0\n\t"
        "push %0\n\t"
        "push dword ptr [ebp + 8]\n\t"
        "call dword ptr ds:[0x5d757c]\n\t"
        "jmp %P0"
        : : "i"(&crt_gu_return));
#else
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push ebp
        push 0
        push 0
        push offset crt_gu_return
        push dword ptr [ebp + 8]
        call dword ptr ds:[0x5d757c]                    // RtlUnwind (the original calls its thunk, 0x4da340)
        jmp crt_gu_return
    }
#endif
}
PORT_FN(0x004d37dc, "global_unwind2", crt_global_unwind2, fp_asm)
CRT_START_NO_SHADOW(crt_global_unwind2)

// _unwind_handler (0x4d37fc): _local_unwind2's own frame -- a nested exception during an unwind: collided unwind
static __declspec(naked) void crt_unwind_handler() {
#ifdef VP_GCC
    __asm__ volatile(
        "mov ecx, dword ptr [esp + 4]\n\t"
        "test dword ptr [ecx + 4], 6\n\t"
        "mov eax, 1\n\t"
        "je done%=\n\t"
        "mov eax, dword ptr [esp + 8]\n\t"
        "mov edx, dword ptr [esp + 0x10]\n\t"
        "mov dword ptr [edx], eax\n\t"
        "mov eax, 3\n\t"
        "done%=:\n\t"
        "ret"
        : :);
#else
    __asm {
        mov ecx, dword ptr [esp + 4]
        test dword ptr [ecx + 4], 6
        mov eax, 1
        je done
        mov eax, dword ptr [esp + 8]
        mov edx, dword ptr [esp + 0x10]
        mov dword ptr [edx], eax
        mov eax, 3
    done:
        ret
    }
#endif
}
PORT_FN(0x004d37fc, "unwind_handler", crt_unwind_handler, fp_asm)
CRT_START_NO_SHADOW(crt_unwind_handler)

// _local_unwind2 (0x4d381e): run the __finally blocks from the frame's try level out to `stop`, under a frame of its
// own (handler _unwind_handler, by its v1.0 address: _abnormal_termination looks for it)
static __declspec(naked) void crt_local_unwind2() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebx\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "mov eax, dword ptr [esp + 0x10]\n\t"
        "push eax\n\t"
        "push 0xfffffffe\n\t"
        "push 0x004d37fc\n\t"
        "push dword ptr fs:[0]\n\t"
        "mov dword ptr fs:[0], esp\n\t"
        "next%=:\n\t"
        "mov eax, dword ptr [esp + 0x20]\n\t"
        "mov ebx, dword ptr [eax + 8]\n\t"
        "mov esi, dword ptr [eax + 0xc]\n\t"
        "cmp esi, 0xffffffff\n\t"
        "je done%=\n\t"
        "cmp esi, dword ptr [esp + 0x24]\n\t"
        "je done%=\n\t"
        "lea esi, [esi + esi*2]\n\t"
        "mov ecx, dword ptr [ebx + esi*4]\n\t"
        "mov dword ptr [esp + 8], ecx\n\t"
        "mov dword ptr [eax + 0xc], ecx\n\t"
        "cmp dword ptr [ebx + esi*4 + 4], 0\n\t"
        "jne next%=\n\t"
        "push 0x101\n\t"
        "mov eax, dword ptr [ebx + esi*4 + 8]\n\t"
        "call dword ptr [%c0]\n\t"
        "call dword ptr [ebx + esi*4 + 8]\n\t"
        "jmp next%=\n\t"
        "done%=:\n\t"
        "pop dword ptr fs:[0]\n\t"
        "add esp, 0xc\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "ret"
        : : "i"(&a_NLG_Notify));
#else
    __asm {
        push ebx
        push esi
        push edi
        mov eax, dword ptr [esp + 0x10]
        push eax
        push 0xfffffffe
        push 0x004d37fc
        push dword ptr fs:[0]
        mov dword ptr fs:[0], esp
    next:
        mov eax, dword ptr [esp + 0x20]
        mov ebx, dword ptr [eax + 8]
        mov esi, dword ptr [eax + 0xc]
        cmp esi, 0xffffffff
        je done
        cmp esi, dword ptr [esp + 0x24]
        je done
        lea esi, [esi + esi*2]
        mov ecx, dword ptr [ebx + esi*4]
        mov dword ptr [esp + 8], ecx
        mov dword ptr [eax + 0xc], ecx
        cmp dword ptr [ebx + esi*4 + 4], 0
        jne next
        push 0x101
        mov eax, dword ptr [ebx + esi*4 + 8]
        call dword ptr [a_NLG_Notify]
        call dword ptr [ebx + esi*4 + 8]
        jmp next
    done:
        pop dword ptr fs:[0]
        add esp, 0xc
        pop edi
        pop esi
        pop ebx
        ret
    }
#endif
}
PORT_FN(0x004d381e, "local_unwind2", crt_local_unwind2, fp_asm)
CRT_START_NO_SHADOW(crt_local_unwind2)

// _abnormal_termination (0x4d3886): in a __finally run by _local_unwind2 for this frame's current level?
static __declspec(naked) void crt_abnormal_termination() {
#ifdef VP_GCC
    __asm__ volatile(
        "%{load%} xor eax, eax\n\t"
        "mov ecx, dword ptr fs:[0]\n\t"
        "cmp dword ptr [ecx + 4], 0x004d37fc\n\t"
        "jne done%=\n\t"
        "mov edx, dword ptr [ecx + 0xc]\n\t"
        "mov edx, dword ptr [edx + 0xc]\n\t"
        "cmp dword ptr [ecx + 8], edx\n\t"
        "jne done%=\n\t"
        "mov eax, 1\n\t"
        "done%=:\n\t"
        "ret"
        : :);
#else
    __asm {
        xor eax, eax
        mov ecx, dword ptr fs:[0]
        cmp dword ptr [ecx + 4], 0x004d37fc
        jne done
        mov edx, dword ptr [ecx + 0xc]
        mov edx, dword ptr [edx + 0xc]
        cmp dword ptr [ecx + 8], edx
        jne done
        mov eax, 1
    done:
        ret
    }
#endif
}
PORT_FN(0x004d3886, "abnormal_termination", crt_abnormal_termination, fp_asm)
CRT_START_NO_SHADOW(crt_abnormal_termination)

// _NLG_Notify (0x4d38b2): the destination (eax), the code (the argument) and ebp into __NLG_Destination (0x502a58);
// _NLG_Notify1 (0x4d38a9) takes the code in ecx. (The original's __NLG_Dispatch label, 0x4d38c5, is only a debugger's
// breakpoint address: nothing calls it.)
static __declspec(naked) void crt_NLG_Notify1() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebx\n\t"
        "push ecx\n\t"
        "mov ebx, 0x00502a58\n\t"
        "mov dword ptr [ebx + 8], ecx\n\t"
        "mov dword ptr [ebx + 4], eax\n\t"
        "mov dword ptr [ebx + 0xc], ebp\n\t"
        "pop ecx\n\t"
        "pop ebx\n\t"
        "ret 4"
        : :);
#else
    __asm {
        push ebx
        push ecx
        mov ebx, 0x00502a58
        mov dword ptr [ebx + 8], ecx
        mov dword ptr [ebx + 4], eax
        mov dword ptr [ebx + 0xc], ebp
        pop ecx
        pop ebx
        ret 4
    }
#endif
}
PORT_FN(0x004d38a9, "NLG_Notify1", crt_NLG_Notify1, fp_asm)
CRT_START_NO_SHADOW(crt_NLG_Notify1)
static __declspec(naked) void crt_NLG_Notify() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebx\n\t"
        "push ecx\n\t"
        "mov ebx, 0x00502a58\n\t"
        "mov ecx, dword ptr [ebp + 8]\n\t"
        "mov dword ptr [ebx + 8], ecx\n\t"
        "mov dword ptr [ebx + 4], eax\n\t"
        "mov dword ptr [ebx + 0xc], ebp\n\t"
        "pop ecx\n\t"
        "pop ebx\n\t"
        "ret 4"
        : :);
#else
    __asm {
        push ebx
        push ecx
        mov ebx, 0x00502a58
        mov ecx, dword ptr [ebp + 8]
        mov dword ptr [ebx + 8], ecx
        mov dword ptr [ebx + 4], eax
        mov dword ptr [ebx + 0xc], ebp
        pop ecx
        pop ebx
        ret 4
    }
#endif
}
PORT_FN(0x004d38b2, "NLG_Notify", crt_NLG_Notify, fp_asm)
CRT_START_NO_SHADOW(crt_NLG_Notify)

// _except_handler3 (0x4d4490): walk the frame's scope table from the current level: a filter that says execute ->
// global unwind, local unwind to that level, its handler (never returns); continue execution (-1) -> 0; on an unwind,
// the frame's __finally blocks
static __declspec(naked) void crt_except_handler3() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebp\n\t"
        "%{load%} mov ebp, esp\n\t"
        "sub esp, 8\n\t"
        "push ebx\n\t"
        "push esi\n\t"
        "push edi\n\t"
        "push ebp\n\t"
        "cld\n\t"
        "mov ebx, dword ptr [ebp + 0xc]\n\t"
        "mov eax, dword ptr [ebp + 8]\n\t"
        "test dword ptr [eax + 4], 6\n\t"
        "jne unwinding%=\n\t"
        "mov dword ptr [ebp - 8], eax\n\t"
        "mov eax, dword ptr [ebp + 0x10]\n\t"
        "mov dword ptr [ebp - 4], eax\n\t"
        "lea eax, [ebp - 8]\n\t"
        "mov dword ptr [ebx - 4], eax\n\t"
        "mov esi, dword ptr [ebx + 0xc]\n\t"
        "mov edi, dword ptr [ebx + 8]\n\t"
        "level%=:\n\t"
        "cmp esi, 0xffffffff\n\t"
        "je search%=\n\t"
        "lea ecx, [esi + esi*2]\n\t"
        "cmp dword ptr [edi + ecx*4 + 4], 0\n\t"
        "je outer%=\n\t"
        "push esi\n\t"
        "push ebp\n\t"
        "lea ebp, [ebx + 0x10]\n\t"
        "call dword ptr [edi + ecx*4 + 4]\n\t"
        "pop ebp\n\t"
        "pop esi\n\t"
        "mov ebx, dword ptr [ebp + 0xc]\n\t"
        "%{load%} or eax, eax\n\t"
        "je outer%=\n\t"
        "js dismiss%=\n\t"
        "mov edi, dword ptr [ebx + 8]\n\t"
        "push ebx\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 4\n\t"
        "lea ebp, [ebx + 0x10]\n\t"
        "push esi\n\t"
        "push ebx\n\t"
        "call dword ptr [%c1]\n\t"
        "add esp, 8\n\t"
        "lea ecx, [esi + esi*2]\n\t"
        "push 1\n\t"
        "mov eax, dword ptr [edi + ecx*4 + 8]\n\t"
        "call dword ptr [%c2]\n\t"
        "mov eax, dword ptr [edi + ecx*4]\n\t"
        "mov dword ptr [ebx + 0xc], eax\n\t"
        "call dword ptr [edi + ecx*4 + 8]\n\t"
        "outer%=:\n\t"
        "mov edi, dword ptr [ebx + 8]\n\t"
        "lea ecx, [esi + esi*2]\n\t"
        "mov esi, dword ptr [edi + ecx*4]\n\t"
        "jmp level%=\n\t"
        "dismiss%=:\n\t"
        "mov eax, 0\n\t"
        "jmp leave_%=\n\t"
        "search%=:\n\t"
        "mov eax, 1\n\t"
        "jmp leave_%=\n\t"
        "unwinding%=:\n\t"
        "push ebp\n\t"
        "lea ebp, [ebx + 0x10]\n\t"
        "push 0xffffffff\n\t"
        "push ebx\n\t"
        "call dword ptr [%c1]\n\t"
        "add esp, 8\n\t"
        "pop ebp\n\t"
        "mov eax, 1\n\t"
        "leave_%=:\n\t"
        "pop ebp\n\t"
        "pop edi\n\t"
        "pop esi\n\t"
        "pop ebx\n\t"
        "%{load%} mov esp, ebp\n\t"
        "pop ebp\n\t"
        "ret"
        : : "i"(&a_global_unwind2), "i"(&a_local_unwind2), "i"(&a_NLG_Notify));
#else
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 8
        push ebx
        push esi
        push edi
        push ebp
        cld
        mov ebx, dword ptr [ebp + 0xc]
        mov eax, dword ptr [ebp + 8]
        test dword ptr [eax + 4], 6
        jne unwinding
        mov dword ptr [ebp - 8], eax
        mov eax, dword ptr [ebp + 0x10]
        mov dword ptr [ebp - 4], eax
        lea eax, [ebp - 8]
        mov dword ptr [ebx - 4], eax
        mov esi, dword ptr [ebx + 0xc]
        mov edi, dword ptr [ebx + 8]
    level:
        cmp esi, 0xffffffff
        je search
        lea ecx, [esi + esi*2]
        cmp dword ptr [edi + ecx*4 + 4], 0
        je outer
        push esi
        push ebp
        lea ebp, [ebx + 0x10]
        call dword ptr [edi + ecx*4 + 4]
        pop ebp
        pop esi
        mov ebx, dword ptr [ebp + 0xc]
        or eax, eax
        je outer
        js dismiss
        mov edi, dword ptr [ebx + 8]
        push ebx
        call dword ptr [a_global_unwind2]
        add esp, 4
        lea ebp, [ebx + 0x10]
        push esi
        push ebx
        call dword ptr [a_local_unwind2]
        add esp, 8
        lea ecx, [esi + esi*2]
        push 1
        mov eax, dword ptr [edi + ecx*4 + 8]
        call dword ptr [a_NLG_Notify]
        mov eax, dword ptr [edi + ecx*4]
        mov dword ptr [ebx + 0xc], eax
        call dword ptr [edi + ecx*4 + 8]
    outer:
        mov edi, dword ptr [ebx + 8]
        lea ecx, [esi + esi*2]
        mov esi, dword ptr [edi + ecx*4]
        jmp level
    dismiss:
        mov eax, 0
        jmp leave_
    search:
        mov eax, 1
        jmp leave_
    unwinding:
        push ebp
        lea ebp, [ebx + 0x10]
        push 0xffffffff
        push ebx
        call dword ptr [a_local_unwind2]
        add esp, 8
        pop ebp
        mov eax, 1
    leave_:
        pop ebp
        pop edi
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret
    }
#endif
}
PORT_FN(0x004d4490, "except_handler3", crt_except_handler3, fp_asm)
CRT_START_NO_SHADOW(crt_except_handler3)

// _seh_longjmp_unwind@4 (0x4d454d): longjmp's unwind of a jmp_buf's frame (ebp at +0, registration +0x18, level +0x1c)
static __declspec(naked) void crt_seh_longjmp_unwind() {
#ifdef VP_GCC
    __asm__ volatile(
        "push ebp\n\t"
        "mov ecx, dword ptr [esp + 8]\n\t"
        "mov ebp, dword ptr [ecx]\n\t"
        "mov eax, dword ptr [ecx + 0x1c]\n\t"
        "push eax\n\t"
        "mov eax, dword ptr [ecx + 0x18]\n\t"
        "push eax\n\t"
        "call dword ptr [%c0]\n\t"
        "add esp, 8\n\t"
        "pop ebp\n\t"
        "ret 4"
        : : "i"(&a_local_unwind2));
#else
    __asm {
        push ebp
        mov ecx, dword ptr [esp + 8]
        mov ebp, dword ptr [ecx]
        mov eax, dword ptr [ecx + 0x1c]
        push eax
        mov eax, dword ptr [ecx + 0x18]
        push eax
        call dword ptr [a_local_unwind2]
        add esp, 8
        pop ebp
        ret 4
    }
#endif
}
PORT_FN(0x004d454d, "seh_longjmp_unwind", crt_seh_longjmp_unwind, fp_asm)
CRT_START_NO_SHADOW(crt_seh_longjmp_unwind)

// =====================================================================================================================
// fpexcept.obj, 87except.obj: a math function's error -> the IEEE handler's record, matherr, errno
// =====================================================================================================================
// the x87 pieces handle_exc does on memory doubles
static __declspec(naked) uint32_t __cdecl fcomp_zero_ah(const void* d) {       // fldz; fcomp qword [d]; ah
#ifdef VP_GCC
    __asm__ volatile(
        "mov ecx, dword ptr [esp + 4]\n\t"
        "fldz\n\t"
        "fcomp qword ptr [ecx]\n\t"
        "fnstsw ax\n\t"
        "movzx eax, ah\n\t"
        "ret"
        : :);
#else
    __asm {
        mov ecx, dword ptr [esp + 4]
        fldz
        fcomp qword ptr [ecx]
        fnstsw ax
        movzx eax, ah
        ret
    }
#endif
}
static __declspec(naked) void __cdecl store_neg(void* to, const void* from) {  // fld qword [from]; fchs; fstp [to]
#ifdef VP_GCC
    __asm__ volatile(
        "mov ecx, dword ptr [esp + 8]\n\t"
        "mov edx, dword ptr [esp + 4]\n\t"
        "fld qword ptr [ecx]\n\t"
        "fchs\n\t"
        "fstp qword ptr [edx]\n\t"
        "ret"
        : :);
#else
    __asm {
        mov ecx, dword ptr [esp + 8]
        mov edx, dword ptr [esp + 4]
        fld qword ptr [ecx]
        fchs
        fstp qword ptr [edx]
        ret
    }
#endif
}
static __declspec(naked) void __cdecl decomp_to(void* to, uint32_t lo, uint32_t hi, int32_t* expo) {
#ifdef VP_GCC
    __asm__ volatile(
        "push dword ptr [esp + 0x10]\n\t"
        "push dword ptr [esp + 0x10]\n\t"
        "push dword ptr [esp + 0x10]\n\t"
        "mov eax, 0x004d3090\n\t"
        "call eax\n\t"
        "add esp, 0xc\n\t"
        "mov ecx, dword ptr [esp + 4]\n\t"
        "fstp qword ptr [ecx]\n\t"
        "ret"
        : :);
#else
    __asm {                                                                    // _decomp's ST0, stored as a double
        push dword ptr [esp + 0x10]
        push dword ptr [esp + 0x10]
        push dword ptr [esp + 0x10]
        mov eax, 0x004d3090
        call eax
        add esp, 0xc
        mov ecx, dword ptr [esp + 4]
        fstp qword ptr [ecx]
        ret
    }
#endif
}

// _handle_exc (0x4d34f0): the default IEEE results for the masked cases -- invalid / zero-divide only acknowledged;
// overflow: +-inf or +-DBL_MAX by the rounding mode; underflow: the result denormalised by hand (rounded by truncation
// with a sticky bit for inexact); inexact acknowledged. 1 when everything raised was masked (handled).
static int32_t __cdecl crt_handle_exc(uint32_t flags, uint32_t* res, uint32_t cw) {
    uint32_t left = flags & 0x1f;
    if ((flags & 8) && (cw & 1)) {
        gcall<void>(F_set_statfp, 1);
        left &= ~8u;
    } else if ((flags & 4) && (cw & 4)) {
        gcall<void>(F_set_statfp, 4);
        left &= ~4u;
    } else if ((flags & 1) && (cw & 8)) {
        gcall<void>(F_set_statfp, 8);
        const uint32_t rc = cw & 0xc00;
        // C0 after fldz; fcomp *res: 0 < x, or unordered
        const bool pos = (fcomp_zero_ah(res) & 1) != 0;
        uint32_t big = 0;                                                      // 0: inf, 1: DBL_MAX
        if (rc == 0) big = pos ? 0 : 0x10;
        else if (rc == 0x400) big = pos ? 1 : 0x10;
        else if (rc == 0x800) big = pos ? 0 : 0x11;
        else big = pos ? 1 : 0x11;
        const uint32_t src = (big & 1) ? CRT_D_MAX_VA : CRT_D_INF_VA;
        if (big & 0x10) store_neg(res, (const void*)(uintptr_t)src);
        else {
            res[1] = G32(src + 4);
            res[0] = G32(src);
        }
        left &= ~1u;
    } else if ((flags & 2) && (cw & 0x10)) {
        uint32_t sticky = (flags & 0x10) ? 1 : 0;
        if (!((res[1] & 0x7fffffff) | res[0])) {
            sticky = 1;
        } else {
            uint32_t mant[2];
            int32_t e;
            decomp_to(mant, res[0], res[1], &e);
            e -= 0x600;
            if (e < -0x432) {
                mant[0] = mant[1] = 0;
                sticky = 1;
            } else {
                const bool neg = (fcomp_zero_ah(mant) & 0x41) == 0;            // 0 > mant
                ((uint16_t*)mant)[3] &= 0xf;
                ((uint8_t*)mant)[6] |= 0x10;
                if (e < -0x3fd) {
                    int32_t n = -0x3fd - e;
                    do {
                        if ((mant[0] & 1) && !sticky) sticky = 1;
                        mant[0] >>= 1;
                        if (mant[1] & 1) mant[0] |= 0x80000000u;
                        mant[1] >>= 1;
                    } while (--n);
                }
                if (neg) store_neg(mant, mant);
            }
            res[1] = mant[1];
            res[0] = mant[0];
        }
        if (sticky) gcall<void>(F_set_statfp, 0x10);
        left &= ~2u;
    }
    if ((flags & 0x10) && (cw & 0x20)) {
        gcall<void>(F_set_statfp, 0x20);
        left &= ~0x10u;
    }
    return left == 0;
}
static void fp_handle_exc(Footprint& f, uint32_t, uint32_t* res, uint32_t) { f.add(res, 8, "the result"); }
PORT_FN(0x004d34f0, "handle_exc", crt_handle_exc, fp_handle_exc)

// _raise_exc (0x4d3180): fill the _FPIEEE_RECORD (cause, enable from the control word, status from the FPU, rounding
// and precision, the opcode, operand 1 and the result as doubles), clear the FPU's exceptions and RaiseException with
// it; then take back what a handler changed: the enables, rounding and precision into *pcw (the precision cases clear
// the ROUNDING bits -- the 1997 library's bug, kept), and the result.
static void __cdecl crt_raise_exc(uint32_t* rec, uint32_t* pcw, uint32_t flags, uint32_t opcode, const uint32_t* arg1,
                                  uint32_t* result) {
    rec[1] = 0;
    rec[2] = 0;
    rec[3] = 0;
    // (the original's code is an uninitialised local when no cause bit is set; _87except always passes one: dead)
    uint32_t code = 0;
    if (flags & 0x10) { code = 0xc000008f; rec[1] |= 1; }
    if (flags & 2) { code = 0xc0000093; rec[1] |= 2; }
    if (flags & 1) { code = 0xc0000091; rec[1] |= 4; }
    if (flags & 4) { code = 0xc000008e; rec[1] |= 8; }
    if (flags & 8) { code = 0xc0000090; rec[1] |= 0x10; }
    rec[2] = (rec[2] & ~0x10u) | ((~*pcw & 1) << 4);
    rec[2] = (rec[2] & ~0x08u) | ((*pcw & 4) ? 0 : 8);
    rec[2] = (rec[2] & ~0x04u) | ((*pcw & 8) ? 0 : 4);
    rec[2] = (rec[2] & ~0x02u) | ((*pcw & 0x10) ? 0 : 2);
    rec[2] = (rec[2] & ~0x01u) | ((*pcw & 0x20) ? 0 : 1);
    const uint32_t sw = gcall<uint32_t>(F_statfp);
    if (sw & 1) rec[3] |= 0x10;
    if (sw & 4) rec[3] |= 8;
    if (sw & 8) rec[3] |= 4;
    if (sw & 0x10) rec[3] |= 2;
    if (sw & 0x20) rec[3] |= 1;
    switch (*pcw & 0xc00) {
    case 0: rec[0] &= ~3u; break;
    case 0x400: rec[0] = (rec[0] & ~2u) | 1; break;
    case 0x800: rec[0] = (rec[0] & ~1u) | 2; break;
    case 0xc00: rec[0] |= 3; break;
    }
    switch (*pcw & 0x300) {
    case 0: rec[0] = (rec[0] & ~0x14u) | 8; break;
    case 0x200: rec[0] = (rec[0] & ~0x18u) | 4; break;
    case 0x300: rec[0] &= ~0x1cu; break;
    }
    rec[0] = ((opcode << 5) & 0x1ffe0) | (rec[0] & ~0x1ffe0u);
    rec[8] |= 1;
    rec[8] = (rec[8] & ~0x1cu) | 2;
    rec[5] = arg1[1];
    rec[4] = arg1[0];
    rec[0x14] |= 1;
    rec[0x14] = (rec[0x14] & ~0x1cu) | 2;
    rec[0x11] = result[1];
    rec[0x10] = result[0];
    gcall<void>(F_clrfp);
    ULONG_PTR arg = (ULONG_PTR)(uintptr_t)rec;
    kRaiseException(code, 0, 1, &arg);
    const uint32_t en = rec[2];
    if (en & 0x10) *pcw &= ~1u;
    if (en & 8) *pcw &= ~4u;
    if (en & 4) *pcw &= ~8u;
    if (en & 2) *pcw &= ~0x10u;
    if (en & 1) *pcw &= ~0x20u;
    switch (rec[0] & 3) {
    case 0: *pcw &= ~0xc00u; break;
    case 1: *pcw = (*pcw & ~0x800u) | 0x400; break;
    case 2: *pcw = (*pcw & ~0x400u) | 0x800; break;
    case 3: *pcw |= 0xc00; break;
    }
    switch ((rec[0] & 0x1c) >> 2) {
    case 0: *pcw = (*pcw & ~0xc00u) | 0x300; break;
    case 1: *pcw = (*pcw & ~0xc00u) | 0x200; break;
    case 2: *pcw &= ~0xc00u; break;
    }
    result[1] = rec[0x11];
    result[0] = rec[0x10];
}
static void fp_raise_exc(Footprint& f, uint32_t*, uint32_t*, uint32_t, uint32_t, const uint32_t*, uint32_t*) {
    f.replay_only = "raises an exception";
}
PORT_FN(0x004d3180, "raise_exc", crt_raise_exc, fp_raise_exc)

// _set_errno (0x4d37b0): _DOMAIN -> EDOM; _OVERFLOW / _UNDERFLOW -> ERANGE
static void __cdecl crt_set_errno(int32_t type) {
    if (type == 1) crt_errno = 0x21;
    else if (type >= 2 && type <= 3) crt_errno = 0x22;
}
static void fp_set_errno(Footprint& f, int32_t) { f.add((void*)(uintptr_t)CRT_ERRNO_VA, 4, "errno"); }
PORT_FN(0x004d37b0, "set_errno", crt_set_errno, fp_set_errno)

// _87except (0x4d6510): the dispatchers' error exit -- the IEEE flags for the error type (domain / sing: invalid,
// overflow, underflow, tloss: inexact; plos becomes a domain error with none), the masked default (or the IEEE
// handler), the control word back, then _matherr (unless _matherr_flag) and errno
static void __cdecl crt_87except(int32_t opcode, uint32_t* exc, const uint16_t* pcw) {
    uint32_t cw = *pcw;
    uint32_t rec[0x58 / 4];                                   // (uninitialised, as the original's)
    uint32_t flags;
    switch (exc[0]) {
    case 1: case 5: flags = 8; break;
    case 2: flags = 4; break;
    case 3: flags = 0x11; break;
    case 4: flags = 0x12; break;
    case 7: exc[0] = 1; flags = 0; break;
    case 8: flags = 0x10; break;
    default: flags = 0; break;
    }
    if (flags && !gcall<int32_t>(F_handle_exc, flags, exc + 6, cw)) {
        if (opcode == 0x10 || opcode == 0x16 || opcode == 0x1d) {
            rec[0xe] |= 1;
            rec[0xe] = (rec[0xe] & ~0x1cu) | 2;
            rec[0xb] = exc[5];
            rec[0xa] = exc[4];
        } else {
            rec[0xe] &= ~1u;
        }
        gcall<void>(F_raise_exc, rec, &cw, flags, opcode, exc + 2, exc + 6);
    }
    gcall<void>(F_ctrlfp, cw, 0xffffu);
    int32_t r = 0;
    if (exc[0] != 8 && !G32(CRT_MATHERR_FLAG_VA)) r = gcall<int32_t>(F_matherr, exc);
    if (!r) gcall<void>(F_set_errno, exc[0]);
}
static void fp_87except(Footprint& f, int32_t, uint32_t*, const uint16_t*) { f.replay_only = "may raise an exception"; }
PORT_FN(0x004d6510, "87except", crt_87except, fp_87except)

// =====================================================================================================================
// 87fmod.obj: _fFMOD (0x4cf374), fmod's kernel (the table at 0x502630's normal case): fprem until complete (through
// __adj_fprem when __adjust_fdiv is set), the divisor dropped. Hooked at its own address (no inventory entry); its
// loop runs back into its first bytes, so it has no shadow trampoline.
// =====================================================================================================================
static __declspec(naked) void crt_fFMOD() {
#ifdef VP_GCC
    __asm__ volatile(
        "fxch st(1)\n\t"
        "again%=:\n\t"
        "cmp dword ptr ds:[0x5024b8], 1\n\t"
        "je adj%=\n\t"
        "fprem\n\t"
        "jmp wait_%=\n\t"
        "adj%=:\n\t"
        "call dword ptr [%c0]\n\t"
        "wait_%=:\n\t"
        "wait\n\t"
        "fnstsw ax\n\t"
        "wait\n\t"
        "sahf\n\t"
        "jp again%=\n\t"
        "fstp st(1)\n\t"
        "ret"
        : : "i"(&a_adj_fprem));
#else
    __asm {
        fxch st(1)
    again:
        cmp dword ptr ds:[0x5024b8], 1
        je adj
        fprem
        jmp wait_
    adj:
        call dword ptr [a_adj_fprem]
    wait_:
        wait
        fnstsw ax
        wait
        sahf
        jp again
        fstp st(1)
        ret
    }
#endif
}
PORT_FN(0x004cf374, "fFMOD", crt_fFMOD, fp_asm)
CRT_START_NO_SHADOW(crt_fFMOD)

// =====================================================================================================================
// 87disp.obj's result routines (0x4d2e93..0x4d2f69): the special-case results the operation tables (0x502630,
// 0x502780..) and the 87triga kernels jump to, in the dispatcher's frame (ebp: the error class at -0x90, a scratch
// extended at -0x9e) with cl the sign. Reached through the islands (crt_islands_install). Each carries its own copy of
// what the original falls into. The ones nothing references (__rttospopde 0x4d2e93, __rttospop 0x4d2e98,
// __rtnospop 0x4d2e9a, __rtnospopde 0x4d2e9d, __rtonepop 0x4d2eab, __rtonenpop 0x4d2ead, and __rttosnpopde 0x4d2f59
// except as rtindfnpop's tail) aren't ported: dead.
// =====================================================================================================================
static __declspec(naked) void crt_rttosnpop() {         // 0x4d2e9c: the operand is the result
#ifdef VP_GCC
    __asm__ volatile(
        "ret"
        : :);
#else
    __asm { ret }
#endif
}
static __declspec(naked) void crt_rtzeropop() {         // 0x4d2ea4: drop two, 0
#ifdef VP_GCC
    __asm__ volatile(
        "fstp st(0)\n\t"
        "fstp st(0)\n\t"
        "fldz\n\t"
        "ret"
        : :);
#else
    __asm {
        fstp st(0)
        fstp st(0)
        fldz
        ret
    }
#endif
}
static __declspec(naked) void crt_rtzeronpop() {        // 0x4d2ea6: drop one, 0
#ifdef VP_GCC
    __asm__ volatile(
        "fstp st(0)\n\t"
        "fldz\n\t"
        "ret"
        : :);
#else
    __asm {
        fstp st(0)
        fldz
        ret
    }
#endif
}
static __declspec(naked) void crt_tosnan1() {           // 0x4d2eb2: a NaN operand -- quiet it (class 1) or pass it (7)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [ebp - 0x9e]\n\t"
        "fld tbyte ptr [ebp - 0x9e]\n\t"
        "test byte ptr [ebp - 0x97], 0x40\n\t"
        "je signalling%=\n\t"
        "mov byte ptr [ebp - 0x90], 7\n\t"
        "ret\n\t"
        "signalling%=:\n\t"
        "mov byte ptr [ebp - 0x90], 1\n\t"
        "fadd qword ptr ds:[0x502864]\n\t"
        "ret"
        : :);
#else
    __asm {
        fstp tbyte ptr [ebp - 0x9e]
        fld tbyte ptr [ebp - 0x9e]
        test byte ptr [ebp - 0x97], 0x40
        je signalling
        mov byte ptr [ebp - 0x90], 7
        ret
    signalling:
        mov byte ptr [ebp - 0x90], 1
        fadd qword ptr ds:[0x502864]
        ret
    }
#endif
}
#define CRT_TOSNAN2_BODY                                                                                           \
    __asm { fstp tbyte ptr [ebp - 0x9e] }                                                                          \
    __asm { fld tbyte ptr [ebp - 0x9e] }                                                                           \
    __asm { test byte ptr [ebp - 0x97], 0x40 }                                                                     \
    __asm { je signalling }                                                                                        \
    __asm { mov byte ptr [ebp - 0x90], 7 }                                                                         \
    __asm { jmp add_ }                                                                                             \
    __asm { signalling: }                                                                                          \
    __asm { mov byte ptr [ebp - 0x90], 1 }                                                                         \
    __asm { add_: }                                                                                                \
    __asm { _emit 0xde __asm _emit 0xc1 }               /* faddp st(1), st */                                     \
    __asm { ret }
#ifdef VP_GCC
#define CRT_TOSNAN2_BODY_GNU                                                                                       \
    "fstp tbyte ptr [ebp - 0x9e]\n\t"                                                                              \
    "fld tbyte ptr [ebp - 0x9e]\n\t"                                                                               \
    "test byte ptr [ebp - 0x97], 0x40\n\t"                                                                         \
    "je signalling%=\n\t"                                                                                          \
    "mov byte ptr [ebp - 0x90], 7\n\t"                                                                             \
    "jmp add_%=\n\t"                                                                                               \
    "signalling%=:\n\t"                                                                                            \
    "mov byte ptr [ebp - 0x90], 1\n\t"                                                                             \
    "add_%=:\n\t"                                                                                                  \
    ".byte 0xde, 0xc1\n\t"                              /* faddp st(1), st */                                     \
    "ret"
#endif
static __declspec(naked) void crt_nosnan2() {           // 0x4d2edd: swap, then tosnan2
#ifdef VP_GCC
    __asm__ volatile(
        "fxch st(1)\n\t"
        CRT_TOSNAN2_BODY_GNU
        : :);
#else
    __asm { fxch st(1) }
    CRT_TOSNAN2_BODY
#endif
}
static __declspec(naked) void crt_tosnan2() {           // 0x4d2edf: ST0 a NaN -- quiet or pass, added to ST1
#ifdef VP_GCC
    __asm__ volatile(
        CRT_TOSNAN2_BODY_GNU
        : :);
#else
    CRT_TOSNAN2_BODY
#endif
}
static __declspec(naked) void crt_nan2() {              // 0x4d2f07: both NaNs
#ifdef VP_GCC
    __asm__ volatile(
        "fstp tbyte ptr [ebp - 0x9e]\n\t"
        "fld tbyte ptr [ebp - 0x9e]\n\t"
        "test byte ptr [ebp - 0x97], 0x40\n\t"
        "je signalling%=\n\t"
        "fxch st(1)\n\t"
        "fstp tbyte ptr [ebp - 0x9e]\n\t"
        "fld tbyte ptr [ebp - 0x9e]\n\t"
        "test byte ptr [ebp - 0x97], 0x40\n\t"
        "je signalling%=\n\t"
        "mov byte ptr [ebp - 0x90], 7\n\t"
        "jmp add_%=\n\t"
        "signalling%=:\n\t"
        "mov byte ptr [ebp - 0x90], 1\n\t"
        "add_%=:\n\t"
        ".byte 0xde\n\t"
        ".byte 0xc1\n\t"
        "ret"
        : :);
#else
    __asm {
        fstp tbyte ptr [ebp - 0x9e]
        fld tbyte ptr [ebp - 0x9e]
        test byte ptr [ebp - 0x97], 0x40
        je signalling
        fxch st(1)
        fstp tbyte ptr [ebp - 0x9e]
        fld tbyte ptr [ebp - 0x9e]
        test byte ptr [ebp - 0x97], 0x40
        je signalling
        mov byte ptr [ebp - 0x90], 7
        jmp add_
    signalling:
        mov byte ptr [ebp - 0x90], 1
    add_:
        _emit 0xde
        _emit 0xc1
        ret
    }
#endif
}
#define CRT_RTINDFNPOP_BODY                                                                                        \
    __asm { fstp st(0) }                                                                                           \
    __asm { fld tbyte ptr ds:[0x502850] }                                                                          \
    __asm { cmp byte ptr [ebp - 0x90], 0 }                                                                         \
    __asm { jg keep }                                                                                              \
    __asm { mov byte ptr [ebp - 0x90], 1 }                                                                         \
    __asm { keep: }                                                                                                \
    __asm { or cl, cl }                                                                                            \
    __asm { ret }
#ifdef VP_GCC
#define CRT_RTINDFNPOP_BODY_GNU                                                                                    \
    "fstp st(0)\n\t"                                                                                               \
    "fld tbyte ptr ds:[0x502850]\n\t"                                                                              \
    "cmp byte ptr [ebp - 0x90], 0\n\t"                                                                             \
    "jg keep%=\n\t"                                                                                                \
    "mov byte ptr [ebp - 0x90], 1\n\t"                                                                             \
    "keep%=:\n\t"                                                                                                  \
    "%{load%} or cl, cl\n\t"                                                                                       \
    "ret"
#endif
static __declspec(naked) void crt_rtindfpop() {         // 0x4d2f46: drop two, the indefinite (a domain error)
#ifdef VP_GCC
    __asm__ volatile(
        "fstp st(0)\n\t"
        CRT_RTINDFNPOP_BODY_GNU
        : :);
#else
    __asm { fstp st(0) }
    CRT_RTINDFNPOP_BODY
#endif
}
static __declspec(naked) void crt_rtindfnpop() {        // 0x4d2f48: drop one, the indefinite
#ifdef VP_GCC
    __asm__ volatile(
        CRT_RTINDFNPOP_BODY_GNU
        : :);
#else
    CRT_RTINDFNPOP_BODY
#endif
}
static __declspec(naked) void crt_rtchsifneg() {        // 0x4d2f63: negate when cl
#ifdef VP_GCC
    __asm__ volatile(
        "%{load%} or cl, cl\n\t"
        "je done%=\n\t"
        "fchs\n\t"
        "done%=:\n\t"
        "ret"
        : :);
#else
    __asm {
        or cl, cl
        je done
        fchs
    done:
        ret
    }
#endif
}

// __adj_fpatan (0x4cef65): fpatan (the FDIV-bug library's entry: the instruction itself)
static __declspec(naked) void crt_adj_fpatan() {
#ifdef VP_GCC
    __asm__ volatile(
        "fpatan\n\t"
        "ret"
        : :);
#else
    __asm {
        fpatan
        ret
    }
#endif
}
// __rdtsc (0x4188ad, _prof.obj): the time-stamp counter, edx:eax
static __declspec(naked) void crt_rdtsc() {
#ifdef VP_GCC
    __asm__ volatile(
        "rdtsc\n\t"
        "ret"
        : :);
#else
    __asm {
        rdtsc
        ret
    }
#endif
}

// directsound_create_mixers (0x476750, ds.obj): empty in the release build; MixerBegin's rewrite calls it
static void __cdecl crt_directsound_create_mixers() {}
PORT_FN(0x00476750, "directsound_create_mixers", crt_directsound_create_mixers, fp_none)

// =====================================================================================================================
// the islands
// =====================================================================================================================
enum IslandKind : uint8_t { JMP5, SHORT2 };
struct Island {
    uint32_t at;
    IslandKind kind;
    uint8_t group;            // 0: 87disp, 1: __adj_fpatan, 2: __rdtsc
    uint8_t orig[5];          // the v1.0 bytes it replaces
    void* to;                 // JMP5: the rewrite
    uint32_t short_to;        // SHORT2: the slot
    const char* what;
};
#define B5(a, b, c, d, e) {a, b, c, d, e}
static const Island k_islands[] = {
    {0x004d2e9c, JMP5, 0, B5(0xc3, 0xe8, 0xb7, 0x00, 0x00), (void*)&crt_rttosnpop, 0, "__rttosnpop"},
    {0x004d2ea4, SHORT2, 0, B5(0xdd, 0xd8, 0, 0, 0), 0, 0x004d2eab, "__rtzeropop"},
    {0x004d2eab, JMP5, 0, B5(0xdd, 0xd8, 0xdd, 0xd8, 0xd9), (void*)&crt_rtzeropop, 0, "__rtzeropop's slot"},
    {0x004d2ea6, JMP5, 0, B5(0xdd, 0xd8, 0xd9, 0xee, 0xc3), (void*)&crt_rtzeronpop, 0, "__rtzeronpop"},
    {0x004d2eb2, JMP5, 0, B5(0xdb, 0xbd, 0x62, 0xff, 0xff), (void*)&crt_tosnan1, 0, "__tosnan1"},
    {0x004d2edd, SHORT2, 0, B5(0xd9, 0xc9, 0, 0, 0), 0, 0x004d2ee4, "__nosnan2"},
    {0x004d2ee4, JMP5, 0, B5(0xff, 0xdb, 0xad, 0x62, 0xff), (void*)&crt_nosnan2, 0, "__nosnan2's slot"},
    {0x004d2edf, JMP5, 0, B5(0xdb, 0xbd, 0x62, 0xff, 0xff), (void*)&crt_tosnan2, 0, "__tosnan2"},
    {0x004d2f07, JMP5, 0, B5(0xdb, 0xbd, 0x62, 0xff, 0xff), (void*)&crt_nan2, 0, "__nan2"},
    {0x004d2f46, SHORT2, 0, B5(0xdd, 0xd8, 0, 0, 0), 0, 0x004d2f4d, "__rtindfpop"},
    {0x004d2f4d, JMP5, 0, B5(0x28, 0x50, 0x00, 0x80, 0xbd), (void*)&crt_rtindfpop, 0, "__rtindfpop's slot"},
    {0x004d2f48, JMP5, 0, B5(0xdd, 0xd8, 0xdb, 0x2d, 0x50), (void*)&crt_rtindfnpop, 0, "__rtindfnpop"},
    {0x004d2f63, JMP5, 0, B5(0x0a, 0xc9, 0x74, 0x02, 0xd9), (void*)&crt_rtchsifneg, 0, "__rtchsifneg"},
    {0x004cef65, SHORT2, 1, B5(0xd9, 0xf3, 0, 0, 0), 0, 0x004cef6d, "__adj_fpatan"},
    {0x004cef6d, SHORT2, 1, B5(0xcc, 0xcc, 0, 0, 0), 0, 0x004cefe9, "__adj_fpatan's hop (adj_fptan's padding)"},
    {0x004cefe9, JMP5, 1, B5(0xcc, 0xcc, 0xcc, 0xcc, 0xcc), (void*)&crt_adj_fpatan, 0, "__adj_fpatan's slot (onexit's padding)"},
    {0x004188ad, SHORT2, 2, B5(0x0f, 0x31, 0, 0, 0), 0, 0x00418912, "__rdtsc"},
    {0x00418912, JMP5, 2, B5(0xcc, 0xcc, 0xcc, 0xcc, 0xcc), (void*)&crt_rdtsc, 0, "__rdtsc's slot (KernelEnd's padding)"},
};
#undef B5
enum { N_ISLANDS = sizeof k_islands / sizeof k_islands[0] };

static void island_bytes(const Island& is, uint8_t* b, int* n) {
    if (is.kind == JMP5) {
        b[0] = 0xe9;
        const int32_t rel = (int32_t)((uintptr_t)is.to - (is.at + 5));
        memcpy(b + 1, &rel, 4);
        *n = 5;
    } else {
        b[0] = 0xeb;
        b[1] = (uint8_t)(int8_t)(int32_t)(is.short_to - (is.at + 2));
        *n = 2;
    }
}

}   // namespace crt_start

// ---- the islands' interface (crt_types.h) ---------------------------------------------------------------------------------
int crt_islands(const CrtIsland** out) {
    static CrtIsland list[crt_start::N_ISLANDS];
    for (int i = 0; i < crt_start::N_ISLANDS; i++) {
        const crt_start::Island& is = crt_start::k_islands[i];
        list[i].at = is.at;
        list[i].len = is.kind == crt_start::JMP5 ? 5 : 2;
        list[i].target = is.kind == crt_start::JMP5 ? (uint32_t)(uintptr_t)is.to : is.short_to;
        list[i].what = is.what;
    }
    *out = list;
    return crt_start::N_ISLANDS;
}

bool crt_islands_install() {
    using namespace crt_start;
    bool all = true;
    for (int g = 0; g < 3; g++) {
        // every byte first: the original's, int3 (the standalone's fill) or already this patch -- or nothing of the group
        bool ok = true, done = true;
        for (const Island& is : k_islands) {
            if (is.group != g) continue;
            uint8_t want[5];
            int n;
            island_bytes(is, want, &n);
            const uint8_t* p = (const uint8_t*)(uintptr_t)is.at;
            bool orig = true, cc = true, mine = true;
            for (int k = 0; k < n; k++) {
                orig &= p[k] == is.orig[k];
                cc &= p[k] == 0xcc;
                mine &= p[k] == want[k];
            }
            ok &= orig || cc || mine;
            done &= mine;
        }
        static const char* const k_group[] = {"87disp's result routines", "__adj_fpatan", "__rdtsc"};
        if (!ok) {
            logf("crt: NOT redirecting %s: the bytes aren't v1.0's", k_group[g]);
            all = false;
            continue;
        }
        if (done) continue;
        for (const Island& is : k_islands) {
            if (is.group != g) continue;
            uint8_t want[5];
            int n;
            island_bytes(is, want, &n);
            uint8_t* p = (uint8_t*)(uintptr_t)is.at;
            DWORD old;
            vpos_VirtualProtect(p, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old);
            memcpy(p, want, (size_t)n);
            vpos_VirtualProtect(p, (SIZE_T)n, old, &old);
            vpos_flush_code(p, (SIZE_T)n);
        }
        logf("crt: %s redirected to their rewrites", k_group[g]);
    }
    return all;
}
