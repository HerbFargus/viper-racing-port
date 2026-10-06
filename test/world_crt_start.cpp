// world_crt_start.cpp -- M3 stage S1, group C: the C runtime's start-up, exit and exception plumbing (hook/crt_start.cpp)
// against the originals, outside the game: wincrt0 / crt0dat / crt0msg / crtmbox / crt0fp / stdargv / stdenvp / aw_env /
// ioinit / heapinit / onexit / closeall / purevirt / handler / winxfltr / fpinit / fpexcept / 87except / matherr / exsup /
// exsup3, _fFMOD, and the islands (87disp's result routines, __adj_fpatan, __rdtsc).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_crt_start.cpp
//        /Fo%TEMP%\crts\ /Fe%TEMP%\crts\world_crt_start.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//        /SAFESEH:NO /NXCOMPAT:NO /STACK:0x800000
//   (/SAFESEH:NO and /NXCOMPAT:NO: the wmcs section runs _WinMainCRTStartup, which registers an SEH frame whose handler
//   is _except_handler3 inside the VirtualAlloc'd image, or the rewrite's frame inside this exe. Nothing here raises an
//   exception, so the frame is never dispatched to; the flags only keep a stray fault from going unhandled.)
//   run:   world_crt_start.exe [scale] [seed] [only]
//          (scale 1 = the default counts; only: one of startup argv env exit msg fpinit xcpt fpexc x87 islands wmcs)
//          world_crt_start.exe addrs   -- the rewrites' addresses in this exe (for the static check)
//   static: python test\world_crt_start_static.py %TEMP%\crts\world_crt_start.exe
//          the exception machinery (exsup, exsup3, the start-up frame's filter and handler) and every other naked
//          rewrite, instruction by instruction against out\race_v10.exe (capstone): call / jump targets, labels and
//          data addresses normalised to v1.0's. The C ones of the exception machinery (_XcptFilter, xcptlookup,
//          _raise_exc, _handle_exc, _87except) were compared with the disassembly by reading, and are also run below
//          (called directly, nothing raised).
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does (a child process with the range reserved before its heap
// exists) and fills its import table itself: the C runtime's Windows calls are deterministic fakes that log every call
// (strings and buffers hashed, pointers into the caller's frame logged as a marker): the heap (HeapCreate / HeapAlloc /
// HeapReAlloc / HeapFree / HeapSize on a fixed arena, first-fit by address, with a scripted failure), GetVersion,
// GetCommandLineA, GetEnvironmentStrings(W) / FreeEnvironmentStrings(A/W), WideCharToMultiByte (the real one, with a
// scripted failure), GetStartupInfoA (a scripted STARTUPINFO: show-window flags, inherited handle tables in
// lpReserved2), GetFileType / GetStdHandle / SetHandleCount and the file calls on fake handles, GetModuleFileNameA /
// GetModuleHandleA, GetACP / GetOEMCP (scripted code pages: lead bytes), LoadLibraryA / GetProcAddress (fake KERNEL32
// affinity calls and user32 message-box calls, each scriptable to fail), UnhandledExceptionFilter (a scripted answer),
// RaiseException (logs the _FPIEEE_RECORD and changes it as a handler would), ExitProcess (ends the pass: the call
// thunk is resumed with the exit code). RtlUnwind is the real one. __ms_p5_test_fdiv (0x4d0840) answers from a script,
// so _fpmath sees __adjust_fdiv both ways.
//
// Every check runs the original (the image), then from the same state the rewrite called directly ("isolated": what it
// calls by address is the original) and the original's entry with every rewrite of this file hooked into the image and
// the islands installed ("chain": the rewrites calling each other), and compares: the CRT's globals at their v1.0
// addresses (the whole .data for the start-up, exit and WinMainCRTStartup sections, the CRT's ranges elsewhere), the
// heap, an arena of inputs at a fixed address, the fakes' state, the log of every import call and every callback (the
// init / exit / signal / filter / handler / finally / WinMain stubs), the return registers, the callee-saved registers,
// the stack pointer, the flags where a routine returns them, the whole x87 state (fnsave), a fault, an exit and its code.
// Each pass's stack below the call is filled with the same value first (uninitialised locals read the same).
//
// Sections:
//   startup  the start-up step by step from the image as loaded (each step checked, then the original's result carried
//            on): _heap_init, _ioinit (inherited handle tables: counts 0..3000, the 2048 cap, -1 / unknown-type handles,
//            FOPEN or not, lpReserved2 null; the standard handles none / char / pipe / disk / unknown), __initmbctable
//            (the original, with scripted code pages), __crtGetEnvironmentStringsA (wide, ANSI, neither; a second call),
//            _setargv, _setenvp, _cinit (the __xc table pointing at logging stubs, some registering atexit functions;
//            _FPinit set, cleared or a stub; _fpmath with __adjust_fdiv forced either way), atexit / _onexit growing the
//            table, _fcloseall with open streams, and exit / _exit / doexit with each flag.
//   argv     parse_cmdline (counting and filling) and _setargv on random command lines: quoted / unquoted / empty
//            program names, 2N and 2N+1 backslashes before quotes, "" inside quotes, tabs, lead bytes (random _mbctype
//            lead-byte sets) including a lead byte before the NUL; malloc failing (R6008).
//   env      __crtGetEnvironmentStringsA in each f_use state, conversions failing, malloc failing, the second-call path
//            with its uninitialised local (0, or a block); _setenvp on random blocks ("=X:" entries, empty), malloc
//            failing (R6009).
//   exit     __onexitinit, _onexit / atexit sequences (to 200 entries: realloc growth, realloc failing),
//            doexit / exit / _exit with every flag, exit functions that register more, the __xp / __xt tables real
//            (__endstdio with open streams) or stubs; _fcloseall with 0..40 streams (the ones past _iob malloc'd).
//   msg      _amsg_exit / _FF_MSGBANNER / _NMSG_WRITE / __crtMessageBoxA for every rterrs code and others, each error
//            mode and app type, console and GUI paths, long / empty / failing module names, user32 missing, any of its
//            three functions missing, the cached pointers; _purecall, _fptrap, _callnewh, _initterm, _matherr,
//            _set_errno, _cfltcvt_init, _fpclear, directsound_create_mixers.
//   fpinit   _fpmath: __ms_p5_test_fdiv's answers per processor, KERNEL32 / its functions missing, affinity masks.
//   xcpt     xcptlookup and _XcptFilter called directly (nothing raised: the EXCEPTION_POINTERS are built in the arena)
//            for every code in _XcptActTab and others, with each action (SIG_DFL, SIG_IGN, SIG_DIE, a handler), the
//            SIGFPE reset loop, table sizes, UnhandledExceptionFilter answering each way.
//   fpexc    _handle_exc (every flag set, every control word, special results, denormalising underflows), _raise_exc
//            (records, control words, a handler changing the record: RaiseException is a fake that returns, so nothing
//            is raised), _87except (every error type, the operand-2 opcodes, _matherr_flag), called directly.
//            Exception DISPATCH (SEH frames, unwinds, _except_handler3, _local_unwind2 ...) is not run: see "static".
//   x87      _fFMOD (random operands, exponent gaps, __adjust_fdiv 0 / 1: __adj_fprem), 87disp's result routines
//            (random x87 stacks, NaNs quiet and signalling, the error-class byte, cl), __adj_fpatan, __rdtsc; then
//            asin / acos / atan / atan2 / fmod and their _CI forms on special values through the dispatchers, original
//            vs the islands installed (and the error path's rewrites hooked). The FPU's exceptions stay masked.
//   islands  crt_islands_install: the bytes it writes, idempotence, a group with a foreign byte left alone, int3 fill.
//   wmcs     _WinMainCRTStartup end to end with WinMain (0x412260) stubbed: the stub compares the arguments, the SEH frame
//            (registration, try level, saved esp; the scope table by its contents), the STARTUPINFO local, the CRT's
//            globals and the heap; then WinMain returns, exits, _exits or calls atexit, and the exit path runs to
//            ExitProcess (nothing raises). Also: no environment, no command line, HeapCreate failing.
// Built VP_FAITHFUL (group C has no fixes).
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <float.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#define VP_FAITHFUL
#define VP_CRT_HARNESS              // _WinMainCRTStartup's rewrite doesn't install the islands itself
#include "../hook/port.h"
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                 // vp_guard: the GCC build's __try / __except
#include <malloc.h>                 // _resetstkoflw
#include <math.h>                   // ldexp (MSVC's headers bring it in)
#endif

struct ChainReg {
    uint32_t at; void* fn; const char* name; ChainReg* next;
    static ChainReg*& head() { static ChainReg* h; return h; }
    ChainReg(uint32_t a, void* f, const char* n) : at(a), fn(f), name(n), next(head()) { head() = this; }
};
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;                                                        \
    static ChainReg VP_CAT(chain_, NEW)(V10, (void*)&NEW, NAME);
#define CRT_START_NO_SHADOW(NEW)

static bool g_quiet_logf = true;
void logf(const char* fmt, ...) {
    if (g_quiet_logf) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 0x40, what); }

#pragma warning(disable : 4733)
#include "../hook/crt_start.cpp"

// ---- random values, hashes ------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
static uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static uint64_t rnd64() { return (uint64_t)rnd() << 32 | rnd(); }
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static uint32_t fnv(const void* p, size_t n, uint32_t h = 2166136261u) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static uint32_t hs(const char* s) { return s ? fnv(s, strlen(s)) : 0xdead; }
static uint32_t P(const void* p) { return (uint32_t)(uintptr_t)p; }
#if defined(__GNUC__) && !defined(__clang__)
template <class F> static uint32_t P(F* f) { return (uint32_t)(uintptr_t)f; }   // (a function pointer: MSVC converts)
#endif
#define G32(va) CRT_G(uint32_t, va)
#define G8(va) CRT_G(uint8_t, va)

// ---- the original, loaded at 0x400000 --------------------------------------------------------------------------------
enum { DATA_AT = 0x004e1000, DATA_SIZE = 0xf5f2c };
static IMAGE_NT_HEADERS* g_nt;
static uint8_t* g_file;
static bool load_race_exe(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("can't open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = (uint8_t*)malloc(n);
    fread(g_file, 1, n, f);
    fclose(f);
    g_nt = (IMAGE_NT_HEADERS*)(g_file + ((IMAGE_DOS_HEADER*)g_file)->e_lfanew);
    if (g_nt->FileHeader.TimeDateStamp != 0x362de68c) { printf("%s isn't the v1.0 race.exe\n", path); return false; }
    uint8_t* base = (uint8_t*)VirtualAlloc((void*)0x400000, g_nt->OptionalHeader.SizeOfImage, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (base != (uint8_t*)0x400000) { printf("0x400000 isn't reserved for race.exe in this process\n"); return false; }
    memcpy(base, g_file, g_nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(g_nt);
    for (int i = 0; i < g_nt->FileHeader.NumberOfSections; i++)
        memcpy(base + sec[i].VirtualAddress, g_file + sec[i].PointerToRawData,
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData
                                                                                         : sec[i].Misc.VirtualSize);
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_CRTS_CHILD", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) return 2;
    if (!VirtualAllocEx(pi.hProcess, (void*)0x400000, 0x300000, MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        printf("couldn't reserve 0x400000 in the test process (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// ---- the chain: every rewrite hooked into the image, and the islands --------------------------------------------------
static void patch_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    const int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}
struct ChainSave { uint32_t at; uint8_t b[5]; };
static ChainSave g_chain_save[128];
static int g_nchain;
static uint8_t* g_count_thunks;
static std::map<uint32_t, uint32_t*> g_chain_count;
static bool g_chain_hooks = true;                    // patch the PORT_FN entries (else only the islands)
static uint8_t g_island_save[32][5];
static int g_nisland;
static const CrtIsland* g_islands;
static void islands_save() {
    for (int i = 0; i < g_nisland; i++) memcpy(g_island_save[i], (void*)(uintptr_t)g_islands[i].at, g_islands[i].len);
}
static void islands_restore() {
    for (int i = g_nisland - 1; i >= 0; i--) memcpy((void*)(uintptr_t)g_islands[i].at, g_island_save[i], g_islands[i].len);
}
static void chain_patch() {
    g_nchain = 0;
    if (!g_count_thunks) {
        g_count_thunks = (uint8_t*)VirtualAlloc(0, 0x10000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        int k = 0;
        for (ChainReg* r = ChainReg::head(); r; r = r->next, k++) {
            uint8_t* t = g_count_thunks + 16 * k;
            uint32_t* c = (uint32_t*)(g_count_thunks + 0x8000 + 4 * k);
            g_chain_count[r->at] = c;
            t[0] = 0xff;
            t[1] = 0x05;
            const uint32_t ca = P(c);
            memcpy(t + 2, &ca, 4);
            t[6] = 0xe9;
            const int32_t rel = (int32_t)((uintptr_t)r->fn - ((uintptr_t)t + 11));
            memcpy(t + 7, &rel, 4);
        }
    }
    if (g_chain_hooks) {
        int k = 0;
        for (ChainReg* r = ChainReg::head(); r; r = r->next, k++) {
            ChainSave& s = g_chain_save[g_nchain++];
            s.at = r->at;
            memcpy(s.b, (void*)(uintptr_t)r->at, 5);
            patch_jmp(r->at, g_count_thunks + 16 * k);
        }
    }
    islands_save();
    crt_islands_install();
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}
static void chain_unpatch() {
    islands_restore();
    for (int i = g_nchain - 1; i >= 0; i--) memcpy((void*)(uintptr_t)g_chain_save[i].at, g_chain_save[i].b, 5);
    g_nchain = 0;
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
}
static void* rewrite_of(uint32_t at) {
    for (ChainReg* r = ChainReg::head(); r; r = r->next)
        if (r->at == at) return r->fn;
    for (int i = 0; i < g_nisland; i++)                 // an island's entry: its rewrite (through a short hop's slot)
        if (g_islands[i].at == at) {
            uint32_t t = g_islands[i].target;
            for (int hop = 0; hop < 4 && g_islands[i].len == 2; hop++) {
                int j = 0;
                while (j < g_nisland && g_islands[j].at != t) j++;
                if (j == g_nisland) break;
                t = g_islands[j].target;
                if (g_islands[j].len == 5) return (void*)(uintptr_t)t;
            }
            return (void*)(uintptr_t)t;
        }
    printf("no rewrite registered at %08x\n", at);
    ExitProcess(3);
    return 0;
}

// ---- the log of import calls and callbacks -------------------------------------------------------------------------------
enum : uint32_t {
    K_HeapCreate = 1, K_HeapAlloc, K_HeapFree, K_HeapSize, K_HeapReAlloc,
    K_GetVersion = 10, K_GetCommandLineA, K_GetEnvW, K_GetEnvA, K_FreeEnvW, K_FreeEnvA, K_WCTMB, K_GetStartupInfoA,
    K_GetFileType, K_GetStdHandle, K_SetHandleCount, K_GetModuleFileNameA, K_GetModuleHandleA, K_ExitProcess,
    K_LoadLibraryA, K_GetProcAddress, K_UEF, K_RaiseException, K_WriteFile, K_ReadFile, K_CloseHandle, K_FlushFileBuffers,
    K_SetFilePointer, K_CreateFileA, K_SetStdHandle, K_GetACP, K_GetOEMCP,
    K_Aff = 50, K_CurProc, K_SetThrAff, K_CurThr, K_MsgBox, K_ActiveWnd, K_LastPopup, K_TestFdiv,
    E_Init = 70, E_Exit, E_Signal, E_DbgMsg, E_AexitRtn, E_NewH, E_WinMain, E_WinMainFrame, E_WinMainState, E_Filter,
    E_Handler, E_Finally, E_Enter, E_Resume, E_Resumed, E_Returned, E_Abort, E_Fpinit, E_Mark, E_AtexitRet, E_Matherr,
};
static const char* kind_name(uint32_t k) {
    static const struct { uint32_t k; const char* n; } t[] = {
        {K_HeapCreate, "HeapCreate"}, {K_HeapAlloc, "HeapAlloc"}, {K_HeapFree, "HeapFree"}, {K_HeapSize, "HeapSize"},
        {K_HeapReAlloc, "HeapReAlloc"}, {K_GetVersion, "GetVersion"}, {K_GetCommandLineA, "GetCommandLineA"},
        {K_GetEnvW, "GetEnvironmentStringsW"}, {K_GetEnvA, "GetEnvironmentStrings"}, {K_FreeEnvW, "FreeEnvironmentStringsW"},
        {K_FreeEnvA, "FreeEnvironmentStringsA"}, {K_WCTMB, "WideCharToMultiByte"}, {K_GetStartupInfoA, "GetStartupInfoA"},
        {K_GetFileType, "GetFileType"}, {K_GetStdHandle, "GetStdHandle"}, {K_SetHandleCount, "SetHandleCount"},
        {K_GetModuleFileNameA, "GetModuleFileNameA"}, {K_GetModuleHandleA, "GetModuleHandleA"},
        {K_ExitProcess, "ExitProcess"}, {K_LoadLibraryA, "LoadLibraryA"}, {K_GetProcAddress, "GetProcAddress"},
        {K_UEF, "UnhandledExceptionFilter"}, {K_RaiseException, "RaiseException"}, {K_WriteFile, "WriteFile"},
        {K_ReadFile, "ReadFile"}, {K_CloseHandle, "CloseHandle"}, {K_FlushFileBuffers, "FlushFileBuffers"},
        {K_SetFilePointer, "SetFilePointer"}, {K_CreateFileA, "CreateFileA"}, {K_SetStdHandle, "SetStdHandle"},
        {K_GetACP, "GetACP"}, {K_GetOEMCP, "GetOEMCP"}, {K_Aff, "GetProcessAffinityMask"},
        {K_CurProc, "GetCurrentProcess"}, {K_SetThrAff, "SetThreadAffinityMask"}, {K_CurThr, "GetCurrentThread"},
        {K_MsgBox, "MessageBoxA"}, {K_ActiveWnd, "GetActiveWindow"}, {K_LastPopup, "GetLastActivePopup"},
        {K_TestFdiv, "__ms_p5_test_fdiv"}, {E_Init, "init stub"}, {E_Exit, "exit stub"}, {E_Signal, "signal handler"},
        {E_DbgMsg, "_adbgmsg stub"}, {E_AexitRtn, "_aexit_rtn stub"}, {E_NewH, "new handler"}, {E_WinMain, "WinMain"},
        {E_WinMainFrame, "WinMain: the SEH frame"}, {E_WinMainState, "WinMain: the CRT's state"}, {E_Filter, "filter"},
        {E_Handler, "__except handler"}, {E_Finally, "__finally"}, {E_Enter, "enter __try"}, {E_Resume, "resume"},
        {E_Resumed, "resumed after the fault"}, {E_Returned, "returned from the inner frame"}, {E_Abort, "ABORT"},
        {E_Fpinit, "_FPinit stub"}, {E_Mark, "mark"}, {E_AtexitRet, "atexit result"}, {E_Matherr, "matherr"}};
    for (auto& x : t)
        if (x.k == k) return x.n;
    return "?";
}
static std::vector<uint32_t> g_log;
static long g_nev, g_ev_limit = 20000;
static void bail_abort();
static void lg(uint32_t kind, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0, uint32_t e = 0, uint32_t f = 0) {
    const uint32_t v[7] = {kind, a, b, c, d, e, f};
    g_log.insert(g_log.end(), v, v + 7);
    if (++g_nev > g_ev_limit) bail_abort();
}
// a pointer that may be into a caller's frame (the originals' and the C rewrites' frames differ): logged as a marker
static uint32_t PS(const void* p) {
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    if (p >= tib->StackLimit && p < tib->StackBase) return 0x57ac0000u;
    return P(p);
}

// ---- the fakes' state: plain data, saved and restored with the CRT's ----------------------------------------------------
enum { ENVW_N = 3000, ENVA_N = 4000, RES2_N = 4 + 3200 * 5, CMD_N = 1100, MOD_N = 700 };
struct Fake {
    uint32_t heap_top;
    int32_t alloc_n, fail_alloc_at, realloc_n, fail_realloc_at, heapcreate_fail;
    uint32_t version;
    int32_t cmdline_null;
    char cmdline[CMD_N];
    int32_t mod_ret0;
    char modname[MOD_N];
    int32_t envw_avail, enva_avail, wctmb_n, wctmb_fail_at;
    wchar_t envw[ENVW_N];
    char enva[ENVA_N];
    uint32_t si_flags, si_show, si_cb2, si_res2_null;
    uint8_t si_res2[RES2_N];
    uint32_t std_handle[3];
    uint32_t acp, oemcp;
    uint32_t loadlib_fail, gpa_fail;
    uint32_t active_window, popup, msgbox_ret;
    int32_t uef_ret;
    uint32_t raise_rng;
    int32_t raise_mutate;
    uint32_t fdiv_sched, fdiv_n;
    uint32_t aff_proc, aff_sys, aff_thread, aff_fail;
    uint32_t next_vh;
    int32_t winmain_action;
    uint32_t winmain_ret, winmain_code, winmain_flags;
    int32_t exit_depth, init_calls, exit_calls, sig_calls, newh_ret, newh_calls, aexit_ret;
    uint32_t sig_fix;
};
static Fake g_fk;
static uint8_t g_valid[64];                         // what a fixed-up faulting load reads

// ---- the deterministic heap ------------------------------------------------------------------------------------------------
enum { HEAP_ARENA = 32u << 20 };
static uint8_t* g_heap;
static uint32_t g_heap_hi;
static const HANDLE FAKE_HEAP = (HANDLE)(uintptr_t)0x5eed0000;
struct HBlk { uint32_t size, cap, live, pad; };
static void* heap_alloc_(uint32_t n, bool zero) {
    const uint32_t cap = (n + 15) & ~15u;
    if (n > HEAP_ARENA / 4) return 0;
    for (uint32_t o = 0; o < g_fk.heap_top;) {
        HBlk* b = (HBlk*)(g_heap + o);
        if (!b->live && b->cap >= cap) {
            if (b->cap >= cap + 32) {
                HBlk* r = (HBlk*)(g_heap + o + 16 + cap);
                r->cap = b->cap - cap - 16;
                r->size = 0;
                r->live = 0;
                b->cap = cap;
            }
            b->size = n;
            b->live = 1;
            memset(b + 1, zero ? 0 : 0xcd, b->cap);
            return b + 1;
        }
        o += 16 + b->cap;
    }
    if (g_fk.heap_top + 16 + cap > HEAP_ARENA) return 0;
    HBlk* b = (HBlk*)(g_heap + g_fk.heap_top);
    b->size = n;
    b->cap = cap;
    b->live = 1;
    b->pad = 0;
    g_fk.heap_top += 16 + cap;
    if (g_fk.heap_top > g_heap_hi) g_heap_hi = g_fk.heap_top;
    memset(b + 1, zero ? 0 : 0xcd, cap);
    return b + 1;
}
static HBlk* heap_blk(const void* p) {
    if ((const uint8_t*)p < g_heap + 16 || (const uint8_t*)p >= g_heap + g_fk.heap_top) return 0;
    return (HBlk*)p - 1;
}
static HANDLE WINAPI st_HeapCreate(DWORD o, SIZE_T a, SIZE_T b) {
    lg(K_HeapCreate, o, (uint32_t)a, (uint32_t)b);
    return g_fk.heapcreate_fail ? 0 : FAKE_HEAP;
}
static LPVOID WINAPI st_HeapAlloc(HANDLE h, DWORD fl, SIZE_T n) {
    void* p = 0;
    if (g_fk.alloc_n++ != g_fk.fail_alloc_at && h == FAKE_HEAP) p = heap_alloc_((uint32_t)n, (fl & HEAP_ZERO_MEMORY) != 0);
    lg(K_HeapAlloc, P(h), fl, (uint32_t)n, P(p));
    return p;
}
static BOOL WINAPI st_HeapFree(HANDLE h, DWORD fl, LPVOID p) {
    HBlk* b = heap_blk(p);
    BOOL ok = h == FAKE_HEAP && b && b->live;
    if (ok) {
        b->live = 0;
        memset(p, 0xdd, b->cap);
    }
    lg(K_HeapFree, P(h), fl, P(p), ok);
    return ok;
}
static SIZE_T WINAPI st_HeapSize(HANDLE h, DWORD fl, LPCVOID p) {
    HBlk* b = heap_blk(p);
    SIZE_T r = h == FAKE_HEAP && b && b->live ? b->size : (SIZE_T)-1;
    lg(K_HeapSize, P(h), fl, P(p), (uint32_t)r);
    return r;
}
static LPVOID WINAPI st_HeapReAlloc(HANDLE h, DWORD fl, LPVOID p, SIZE_T n) {
    HBlk* b = heap_blk(p);
    void* q = 0;
    if (g_fk.realloc_n++ != g_fk.fail_realloc_at && h == FAKE_HEAP && b && b->live) {
        if ((((uint32_t)n + 15) & ~15u) <= b->cap) {
            b->size = (uint32_t)n;
            q = p;
        } else if ((q = heap_alloc_((uint32_t)n, false)) != 0) {
            memcpy(q, p, b->size);
            b->live = 0;
            memset(p, 0xdd, b->cap);
        }
    }
    lg(K_HeapReAlloc, P(h), fl, P(p), (uint32_t)n, P(q));
    return q;
}

// ---- the other fakes -------------------------------------------------------------------------------------------------------
static void bail_exit(uint32_t code);
static DWORD WINAPI st_GetVersion() { lg(K_GetVersion); return g_fk.version; }
static LPSTR WINAPI st_GetCommandLineA() { lg(K_GetCommandLineA); return g_fk.cmdline_null ? 0 : g_fk.cmdline; }
static LPWCH WINAPI st_GetEnvironmentStringsW() { lg(K_GetEnvW); return g_fk.envw_avail ? g_fk.envw : 0; }
static LPCH WINAPI st_GetEnvironmentStrings() { lg(K_GetEnvA); return g_fk.enva_avail ? g_fk.enva : 0; }
static BOOL WINAPI st_FreeEnvironmentStringsW(LPWCH p) { lg(K_FreeEnvW, P(p)); return TRUE; }
static BOOL WINAPI st_FreeEnvironmentStringsA(LPCH p) { lg(K_FreeEnvA, P(p)); return TRUE; }
static int WINAPI st_WideCharToMultiByte(UINT cp, DWORD f, LPCWSTR s, int n, LPSTR d, int dn, LPCSTR dc, LPBOOL used) {
    int r = 0;
    if (g_fk.wctmb_n++ != g_fk.wctmb_fail_at) r = WideCharToMultiByte(cp, f, s, n, d, dn, dc, used);
    lg(K_WCTMB, cp, f, (s ? fnv(s, n > 0 ? (size_t)n * 2 : 0) : 0) ^ (uint32_t)n, P(d) ^ (uint32_t)dn, P(dc) ^ P(used),
       (uint32_t)r ^ (d && r > 0 ? fnv(d, (size_t)r) : 0));
    return r;
}
static void WINAPI st_GetStartupInfoA(LPSTARTUPINFOA si) {
    memset(si, 0, sizeof *si);
    si->cb = sizeof *si;
    si->dwFlags = g_fk.si_flags;
    si->wShowWindow = (WORD)g_fk.si_show;
    si->cbReserved2 = (WORD)g_fk.si_cb2;
    si->lpReserved2 = g_fk.si_res2_null ? 0 : g_fk.si_res2;
    lg(K_GetStartupInfoA, PS(si));
}
// a handle's file type: unknown (an error), disk, char, pipe, or with high bits the CRT masks off
static DWORD ftype(uint32_t h) {
    static const DWORD t[8] = {0, FILE_TYPE_DISK, FILE_TYPE_CHAR, FILE_TYPE_PIPE, 0x8000 | FILE_TYPE_CHAR,
                               0x8000 | FILE_TYPE_PIPE, FILE_TYPE_DISK, 0x100 | FILE_TYPE_DISK};
    if (h == 0 || h == 0xffffffffu) return 0;
    return t[(h * 2654435761u) >> 29];
}
static DWORD WINAPI st_GetFileType(HANDLE h) { const DWORD r = ftype(P(h)); lg(K_GetFileType, P(h), r); return r; }
static HANDLE WINAPI st_GetStdHandle(DWORD n) {
    const uint32_t r = n == STD_INPUT_HANDLE ? g_fk.std_handle[0] : n == STD_OUTPUT_HANDLE ? g_fk.std_handle[1]
                     : n == STD_ERROR_HANDLE ? g_fk.std_handle[2] : 0xffffffffu;
    lg(K_GetStdHandle, n, r);
    return (HANDLE)(uintptr_t)r;
}
static UINT WINAPI st_SetHandleCount(UINT n) { lg(K_SetHandleCount, n); return n; }
static DWORD WINAPI st_GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n) {
    DWORD r = 0;
    if (!g_fk.mod_ret0) {
        const DWORD len = (DWORD)strlen(g_fk.modname);
        if (len + 1 <= n) { memcpy(buf, g_fk.modname, len + 1); r = len; }
        else if (n) { memcpy(buf, g_fk.modname, n - 1); buf[n - 1] = 0; r = n; }
    }
    lg(K_GetModuleFileNameA, P(m), PS(buf), n, r);
    return r;
}
static HMODULE WINAPI st_GetModuleHandleA(LPCSTR n) { lg(K_GetModuleHandleA, hs(n)); return n ? 0 : (HMODULE)0x400000; }
static void WINAPI st_ExitProcess(UINT code) {
    lg(K_ExitProcess, code);
    bail_exit(code);
}
static UINT WINAPI st_GetACP() { lg(K_GetACP, g_fk.acp); return g_fk.acp; }
static UINT WINAPI st_GetOEMCP() { lg(K_GetOEMCP, g_fk.oemcp); return g_fk.oemcp; }
// LoadLibraryA / GetProcAddress: fake KERNEL32 (the FDIV test's affinity calls) and user32 (the message box)
static BOOL WINAPI fk_GetProcessAffinityMask(HANDLE h, PDWORD_PTR pm, PDWORD_PTR sm) {
    lg(K_Aff, P(h), PS(pm), PS(sm), g_fk.aff_fail);
    if (g_fk.aff_fail) return FALSE;
    *pm = g_fk.aff_proc;
    *sm = g_fk.aff_sys;
    return TRUE;
}
static HANDLE WINAPI fk_GetCurrentProcess() { lg(K_CurProc); return (HANDLE)(intptr_t)-1; }
static DWORD_PTR WINAPI fk_SetThreadAffinityMask(HANDLE h, DWORD_PTR m) {
    const uint32_t prev = g_fk.aff_thread;
    g_fk.aff_thread = (uint32_t)m;
    lg(K_SetThrAff, P(h), (uint32_t)m, prev);
    return prev;
}
static HANDLE WINAPI fk_GetCurrentThread() { lg(K_CurThr); return (HANDLE)(intptr_t)-2; }
static int WINAPI fk_MessageBoxA(HWND w, LPCSTR t, LPCSTR c, UINT ty) {
    lg(K_MsgBox, P(w), hs(t), hs(c), ty, t ? (uint32_t)strlen(t) : 0);
    return (int)g_fk.msgbox_ret;
}
static HWND WINAPI fk_GetActiveWindow() { lg(K_ActiveWnd, g_fk.active_window); return (HWND)(uintptr_t)g_fk.active_window; }
static HWND WINAPI fk_GetLastActivePopup(HWND w) { lg(K_LastPopup, P(w), g_fk.popup); return (HWND)(uintptr_t)g_fk.popup; }
static HMODULE WINAPI st_LoadLibraryA(LPCSTR n) {
    uint32_t r = 0;
    if (n && !_stricmp(n, "KERNEL32") && !(g_fk.loadlib_fail & 1)) r = 0x7e000000;
    if (n && !_stricmp(n, "user32.dll") && !(g_fk.loadlib_fail & 2)) r = 0x7e100000;
    lg(K_LoadLibraryA, hs(n), r);
    return (HMODULE)(uintptr_t)r;
}
static FARPROC WINAPI st_GetProcAddress(HMODULE m, LPCSTR n) {
    static const struct { const char* n; void* f; } t[] = {
        {"GetProcessAffinityMask", (void*)&fk_GetProcessAffinityMask}, {"GetCurrentProcess", (void*)&fk_GetCurrentProcess},
        {"SetThreadAffinityMask", (void*)&fk_SetThreadAffinityMask}, {"GetCurrentThread", (void*)&fk_GetCurrentThread},
        {"MessageBoxA", (void*)&fk_MessageBoxA}, {"GetActiveWindow", (void*)&fk_GetActiveWindow},
        {"GetLastActivePopup", (void*)&fk_GetLastActivePopup}};
    void* r = 0;
    for (int i = 0; i < 7; i++)
        if (n && !strcmp(n, t[i].n) && !(g_fk.gpa_fail >> i & 1) && (P(m) == 0x7e000000 || P(m) == 0x7e100000)) r = t[i].f;
    lg(K_GetProcAddress, P(m), hs(n), P(r));
    return (FARPROC)r;
}
static LONG WINAPI st_UnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    lg(K_UEF, ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
       ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionFlags : 0, (uint32_t)g_fk.uef_ret);
    return g_fk.uef_ret;
}
// RaiseException (only _raise_exc uses the import): the _FPIEEE_RECORD logged by the bits its writers define, then
// changed as an IEEE handler would (the enables, the rounding / precision, the result)
static uint32_t raise_rnd() { g_fk.raise_rng = g_fk.raise_rng * 1103515245u + 12345u; return g_fk.raise_rng >> 8; }
static void WINAPI st_RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR* args) {
    uint32_t h = 0, full = 0;
    if (n >= 1 && args) {
        uint32_t* rec = (uint32_t*)args[0];
        const uint32_t op = (rec[0] >> 5) & 0xfff;
        const bool op2 = op == 0x10 || op == 0x16 || op == 0x1d;
        uint32_t d[16] = {rec[0] & 0x1ffff, rec[1], rec[2], rec[3], rec[4], rec[5], rec[8] & 0x1f,
                          rec[0xe] & (op2 ? 0x1fu : 1u), op2 ? rec[0xa] : 0, op2 ? rec[0xb] : 0, rec[0x10], rec[0x11],
                          rec[0x14] & 0x1f};
        h = fnv(d, sizeof d);
        full = fnv(rec, 0x58);
        if (g_fk.raise_mutate) {
            if (raise_rnd() & 1) rec[2] = (rec[2] & ~0x1fu) | (raise_rnd() & 0x1f);
            if (raise_rnd() & 1) rec[0] = (rec[0] & ~0x1fu) | (raise_rnd() & 0x1f);
            if (raise_rnd() & 1) { rec[0x10] = raise_rnd() << 8 | raise_rnd(); rec[0x11] = raise_rnd() << 8 | raise_rnd(); }
        }
    }
    lg(K_RaiseException, code, flags, n, h);
    (void)full;
}
// files on fake handles
static BOOL WINAPI st_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD put, LPOVERLAPPED o) {
    const bool fail = ((P(h) >> 2) & 7) == 7;
    if (put) *put = fail ? 0 : n;
    lg(K_WriteFile, P(h), n, fnv(buf, n), PS(put), P(o), fail);
    if (fail) SetLastError(ERROR_DISK_FULL);
    return !fail;
}
static BOOL WINAPI st_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED o) {
    if (got) *got = 0;
    lg(K_ReadFile, P(h), n, PS(buf), P(o));
    return TRUE;
}
static BOOL WINAPI st_CloseHandle(HANDLE h) { lg(K_CloseHandle, P(h)); return TRUE; }
static BOOL WINAPI st_FlushFileBuffers(HANDLE h) { lg(K_FlushFileBuffers, P(h)); return TRUE; }
static DWORD WINAPI st_SetFilePointer(HANDLE h, LONG d, PLONG hi, DWORD m) {
    lg(K_SetFilePointer, P(h), (uint32_t)d, PS(hi), m);
    if (hi) *hi = 0;
    return 0;
}
static HANDLE WINAPI st_CreateFileA(LPCSTR n, DWORD acc, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cr, DWORD fl, HANDLE t) {
    const uint32_t r = 0x3000 + 4 * g_fk.next_vh++;
    lg(K_CreateFileA, hs(n), acc, sh, PS(sa), cr, fl ^ P(t) ^ r);
    return (HANDLE)(uintptr_t)r;
}
static BOOL WINAPI st_SetStdHandle(DWORD n, HANDLE h) { lg(K_SetStdHandle, n, P(h)); return TRUE; }

struct Stub { const char* name; void* fn; };
static const Stub k_stubs[] = {
    {"HeapCreate", (void*)&st_HeapCreate}, {"HeapAlloc", (void*)&st_HeapAlloc}, {"HeapFree", (void*)&st_HeapFree},
    {"HeapSize", (void*)&st_HeapSize}, {"HeapReAlloc", (void*)&st_HeapReAlloc}, {"GetVersion", (void*)&st_GetVersion},
    {"GetCommandLineA", (void*)&st_GetCommandLineA}, {"GetEnvironmentStringsW", (void*)&st_GetEnvironmentStringsW},
    {"GetEnvironmentStrings", (void*)&st_GetEnvironmentStrings},
    {"FreeEnvironmentStringsW", (void*)&st_FreeEnvironmentStringsW},
    {"FreeEnvironmentStringsA", (void*)&st_FreeEnvironmentStringsA},
    {"WideCharToMultiByte", (void*)&st_WideCharToMultiByte}, {"GetStartupInfoA", (void*)&st_GetStartupInfoA},
    {"GetFileType", (void*)&st_GetFileType}, {"GetStdHandle", (void*)&st_GetStdHandle},
    {"SetHandleCount", (void*)&st_SetHandleCount}, {"GetModuleFileNameA", (void*)&st_GetModuleFileNameA},
    {"GetModuleHandleA", (void*)&st_GetModuleHandleA}, {"ExitProcess", (void*)&st_ExitProcess},
    {"LoadLibraryA", (void*)&st_LoadLibraryA}, {"GetProcAddress", (void*)&st_GetProcAddress},
    {"UnhandledExceptionFilter", (void*)&st_UnhandledExceptionFilter}, {"RaiseException", (void*)&st_RaiseException},
    {"WriteFile", (void*)&st_WriteFile}, {"ReadFile", (void*)&st_ReadFile}, {"CloseHandle", (void*)&st_CloseHandle},
    {"FlushFileBuffers", (void*)&st_FlushFileBuffers}, {"SetFilePointer", (void*)&st_SetFilePointer},
    {"CreateFileA", (void*)&st_CreateFileA}, {"SetStdHandle", (void*)&st_SetStdHandle}, {"GetACP", (void*)&st_GetACP},
    {"GetOEMCP", (void*)&st_GetOEMCP},
};
static void __cdecl trap_import(uint32_t slot) {
    printf("an unexpected import was called (IAT slot %08x): stopping\n", slot);
    ExitProcess(5);
}
static int g_nstubbed, g_nreal, g_ntrapped;
static void install_iat() {
    uint8_t* base = (uint8_t*)0x400000;
    IMAGE_DATA_DIRECTORY dir = g_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    uint8_t* thunks = (uint8_t*)VirtualAlloc(0, 0x10000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    int nt = 0;
    HMODULE k32m = GetModuleHandleA("kernel32.dll");
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.Size && d->Name; d++) {
        const char* dll = (const char*)(base + d->Name);
        const bool k32 = _stricmp(dll, "KERNEL32.dll") == 0;
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            const char* fn = names->u1.Ordinal & IMAGE_ORDINAL_FLAG32 ? "" : (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            void* to = 0;
            if (k32)
                for (auto& s : k_stubs)
                    if (!strcmp(s.name, fn)) to = s.fn;
            if (to) g_nstubbed++;
            else if (k32 && *fn && (to = (void*)GetProcAddress(k32m, fn)) != 0) g_nreal++;
            else {
                uint8_t* t = thunks + 16 * nt++;
                const uint32_t slot = P(&iat->u1.Function), fnp = P(&trap_import);
                t[0] = 0x68; memcpy(t + 1, &slot, 4);
                t[5] = 0xb8; memcpy(t + 6, &fnp, 4);
                t[10] = 0xff; t[11] = 0xd0;
                to = t;
                g_ntrapped++;
            }
            iat->u1.Function = (DWORD)(uintptr_t)to;
        }
    }
}
// __ms_p5_test_fdiv (0x4d0840): the scripted answer, one bit per call
static int __cdecl st_test_fdiv() {
    const int r = (int)(g_fk.fdiv_sched >> (g_fk.fdiv_n & 31) & 1);
    g_fk.fdiv_n++;
    lg(K_TestFdiv, (uint32_t)r);
    return r;
}

// ---- generated stubs at fixed addresses: init / exit / signal / ... functions with an id ---------------------------------
static uint8_t* g_code;                                // RWX, at 0x2e000000
static uint32_t g_code_used;
static uint32_t code_put(const uint8_t* b, uint32_t n) {
    const uint32_t at = P(g_code) + g_code_used;
    memcpy(g_code + g_code_used, b, n);
    g_code_used = (g_code_used + n + 15) & ~15u;
    return at;
}
static void put_rel(uint8_t* b, uint32_t at_of_next, uint32_t to) { const int32_t r = (int32_t)(to - at_of_next); memcpy(b, &r, 4); }
// mov eax, id; jmp common  (the common routine: push eax; call c(id); add esp,4; ret)
static uint32_t make_id_thunk(uint32_t id, uint32_t common) {
    uint8_t b[10] = {0xb8, 0, 0, 0, 0, 0xe9};
    memcpy(b + 1, &id, 4);
    put_rel(b + 6, P(g_code) + g_code_used + 10, common);
    return code_put(b, 10);
}
static uint32_t make_common(void* cfn) {               // push eax; call cfn; add esp,4; ret  (cdecl, returns eax)
    uint8_t b[10] = {0x50, 0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x04, 0xc3};
    put_rel(b + 2, P(g_code) + g_code_used + 6, P(cfn));
    return code_put(b, 10);
}
// the C sides
static int32_t g_init_atexit_mode;
static void __cdecl init_c(uint32_t id);
static void __cdecl exit_c(uint32_t id);
static uint32_t g_init_common, g_exit_common, g_sig_common, g_misc_common;
enum { N_INIT = 64, N_EXIT = 256 };
static uint32_t g_init_fn[N_INIT], g_exit_fn[N_EXIT];
static void __cdecl init_c(uint32_t id) {
    g_fk.init_calls++;
    lg(E_Init, id, g_fk.init_calls, G32(CRT_ONEXITBEGIN_VA) ? G32(CRT_ONEXITEND_VA) - G32(CRT_ONEXITBEGIN_VA) : 0xffffffffu);
    if (id % 3 == 1) {                                 // the game's $E functions register destructors: atexit (0x4ceff0)
        const int32_t r = ((int32_t(__cdecl*)(uint32_t))0x004ceff0)(g_exit_fn[(id * 7) % N_EXIT]);
        lg(E_AtexitRet, id, (uint32_t)r);
    } else if (id % 5 == 2) {
        const uint32_t r = ((uint32_t(__cdecl*)(uint32_t))0x004cef70)(g_exit_fn[(id * 11 + 3) % N_EXIT]);
        lg(E_AtexitRet, id, r);
    }
}
static void __cdecl exit_c(uint32_t id) {
    g_fk.exit_calls++;
    lg(E_Exit, id, g_fk.exit_calls, G32(CRT_ONEXITEND_VA) - G32(CRT_ONEXITBEGIN_VA), G8(CRT_EXITFLAG_VA),
       G32(CRT_C_TERMINATION_DONE_VA));
    if (id % 9 == 4 && g_fk.exit_depth < 3) {          // an exit function that registers another
        g_fk.exit_depth++;
        const int32_t r = ((int32_t(__cdecl*)(uint32_t))0x004ceff0)(g_exit_fn[(id * 5 + 1) % N_EXIT]);
        lg(E_AtexitRet, id, (uint32_t)r);
    }
}
// misc stubs by id: 1 _adbgmsg, 2 _aexit_rtn, 3 new handler, 4 _FPinit, 5.. signal handlers
static uint32_t __cdecl misc_c(uint32_t id, uint32_t a1, uint32_t a2) {
    switch (id) {
    case 1: lg(E_DbgMsg); return 0;
    case 2: lg(E_AexitRtn, a1); return 0;
    case 3: g_fk.newh_calls++; lg(E_NewH, a1, g_fk.newh_calls); return (uint32_t)g_fk.newh_ret;
    case 4: lg(E_Fpinit); return 0;
    default: {
        g_fk.sig_calls++;
        if (a1 != 8) a2 = 0;                           // only SIGFPE's handler is passed a second argument (_fpecode)
        EXCEPTION_POINTERS* ep = (EXCEPTION_POINTERS*)(uintptr_t)G32(CRT_PXCPTINFOPTRS_VA);
        uint32_t code = 0;
#if defined(__GNUC__) && !defined(__clang__)
        struct ReadCode { EXCEPTION_POINTERS* ep; uint32_t code; } rc = {ep, 0};
        if (vp_guard([](void* a) {
                ReadCode* r = (ReadCode*)a;
                r->code = r->ep ? r->ep->ExceptionRecord->ExceptionCode : 0;
            }, &rc))
            rc.code = 0xbad;
        code = rc.code;
#else
        __try { code = ep ? ep->ExceptionRecord->ExceptionCode : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { code = 0xbad; }
#endif
        lg(E_Signal, id, a1, a2, G32(CRT_FPECODE_VA), code, fnv((void*)(uintptr_t)CRT_XCPTACTTAB_VA, 120));
        if (g_fk.sig_fix && ep && code == EXCEPTION_ACCESS_VIOLATION) ep->ContextRecord->Esi = P(g_valid);
        if (id == 6) G32(CRT_XCPTACTTAB_VA + 8) = (uint32_t)(5 + id);   // a handler that re-arms an entry
        return 0;
    }
    }
}
// push [esp+8]; push [esp+8]; push eax; call misc_c; add esp,12; ret   (two cdecl arguments passed on)
static uint32_t make_misc_common() {
    uint8_t b[] = {0xff, 0x74, 0x24, 0x08, 0xff, 0x74, 0x24, 0x08, 0x50, 0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x0c, 0xc3};
    put_rel(b + 10, P(g_code) + g_code_used + 14, P(&misc_c));
    return code_put(b, sizeof b);
}
static uint32_t g_misc_fn[16];

// ---- the arena: inputs at a fixed address ------------------------------------------------------------------------------------
enum { ARENA_SIZE = 0x10000 };
static uint8_t* g_arena;                              // at 0x20000000
static uint32_t g_aused;
static uint8_t* aalloc(uint32_t n, uint32_t align = 4) {
    g_aused = (g_aused + align - 1) & ~(align - 1);
    uint8_t* p = g_arena + g_aused;
    g_aused += n;
    if (g_aused > ARENA_SIZE) { printf("arena overflow\n"); ExitProcess(3); }
    return p;
}

// ---- the call thunk ------------------------------------------------------------------------------------------------------------
struct CallIn {
    uint32_t fn;
    uint32_t args[16];
    int32_t nargs;
    uint32_t eax, ecx, edx, ebx, esi, edi, ebp;
    int32_t set_ebp;
    uint32_t cw;
    int32_t nst;
    uint8_t st[8][10];
    uint32_t fill;
    uint32_t fs0;                 // nonzero: fs:[0] for the call (a fake registration chain ending in the real one)
    uint32_t preflags;            // the x87 status flags set before the call (masked): 1 ZE, 2 IE, 4 PE
};
struct CallOut {
    uint32_t eax, ecx, edx, ebx, esi, edi, ebp, efl;
    uint32_t esp_before, esp_after, fs0_after, exited, exit_code;
    uint8_t save[108];
};
static CallIn g_ci;
static CallOut g_co;
static uint32_t g_saved_esp, g_saved_fs0;
static __declspec(naked) void call_thunk() {
#if defined(__GNUC__) && !defined(__clang__)
    // (two statements, split at the call: one would need more than GCC's 30 operands)
    __asm__ volatile(
        "pushad\n\t"
        "mov dword ptr [%c0], esp\n\t"
        "mov eax, dword ptr fs:[0]\n\t"
        "mov dword ptr [%c1], eax\n\t"
        "mov edi, esp\n\t"
        "sub edi, 4\n\t"
        "mov ecx, 0x8000\n\t"
        "mov eax, dword ptr [%c2]\n\t"
        "std\n\t"
        "rep stosd\n\t"
        "cld\n\t"
        "fninit\n\t"
        "fldcw word ptr [%c3]\n\t"
        "test dword ptr [%c4], 1\n\t"
        "jz pf2%=\n\t"
        "fld1\n\t"
        "fldz\n\t"
        "fdivp st(1), st\n\t"
        "fstp st(0)\n"
        "pf2%=:\n\t"
        "test dword ptr [%c4], 2\n\t"
        "jz pf4%=\n\t"
        "fld1\n\t"
        "fchs\n\t"
        "fsqrt\n\t"
        "fstp st(0)\n"
        "pf4%=:\n\t"
        "test dword ptr [%c4], 4\n\t"
        "jz pf_done%=\n\t"
        "fld1\n\t"
        "fld1\n\t"
        "fadd st(0), st(0)\n\t"
        "fld1\n\t"
        "faddp st(1), st\n\t"
        "fld1\n\t"
        "fdivrp st(1), st\n\t"
        "fstp st(0)\n"
        "pf_done%=:\n\t"
        "mov ecx, dword ptr [%c5]\n"
        "ld%=:\n\t"
        "test ecx, ecx\n\t"
        "jz ld_done%=\n\t"
        "dec ecx\n\t"
        "lea eax, [ecx + ecx * 4]\n\t"
        "lea edx, [%c6]\n\t"
        "fld tbyte ptr [edx + eax * 2]\n\t"
        "jmp ld%=\n"
        "ld_done%=:\n\t"
        "mov ecx, dword ptr [%c7]\n"
        "pa%=:\n\t"
        "test ecx, ecx\n\t"
        "jz pa_done%=\n\t"
        "dec ecx\n\t"
        "lea eax, [%c8]\n\t"
        "push dword ptr [eax + ecx * 4]\n\t"
        "jmp pa%=\n"
        "pa_done%=:\n\t"
        "mov eax, dword ptr [%c9]\n\t"
        "test eax, eax\n\t"
        "jz no_fs%=\n\t"
        "mov dword ptr fs:[0], eax\n"
        "no_fs%=:\n\t"
        "mov dword ptr [%c10], esp\n\t"
        "mov eax, dword ptr [%c11]\n\t"
        "mov ecx, dword ptr [%c12]\n\t"
        "mov edx, dword ptr [%c13]\n\t"
        "mov ebx, dword ptr [%c14]\n\t"
        "mov esi, dword ptr [%c15]\n\t"
        "mov edi, dword ptr [%c16]\n\t"
        "cmp dword ptr [%c17], 0\n\t"
        "je keep_ebp%=\n\t"
        "mov ebp, dword ptr [%c18]\n"
        "keep_ebp%=:\n\t"
        "call dword ptr [%c19]"
        : : "i"(&g_saved_esp), "i"(&g_saved_fs0), "i"(&g_ci.fill), "i"(&g_ci.cw), "i"(&g_ci.preflags),
            "i"(&g_ci.nst), "i"(&g_ci.st), "i"(&g_ci.nargs), "i"(&g_ci.args), "i"(&g_ci.fs0), "i"(&g_co.esp_before),
            "i"(&g_ci.eax), "i"(&g_ci.ecx), "i"(&g_ci.edx), "i"(&g_ci.ebx), "i"(&g_ci.esi), "i"(&g_ci.edi),
            "i"(&g_ci.set_ebp), "i"(&g_ci.ebp), "i"(&g_ci.fn));
    __asm__ volatile(
        "mov dword ptr [%c0], eax\n\t"
        "mov dword ptr [%c1], ecx\n\t"
        "mov dword ptr [%c2], edx\n\t"
        "mov dword ptr [%c3], ebx\n\t"
        "mov dword ptr [%c4], esi\n\t"
        "mov dword ptr [%c5], edi\n\t"
        "mov dword ptr [%c6], ebp\n\t"
        "pushfd\n\t"
        "pop dword ptr [%c7]\n\t"
        "mov dword ptr [%c8], esp\n\t"
        "mov eax, dword ptr fs:[0]\n\t"
        "mov dword ptr [%c9], eax\n\t"
        "fnsave [%c10]\n\t"
        "mov dword ptr [%c11], 0\n\t"
        "cld\n\t"
        "mov esp, dword ptr [%c12]\n\t"
        "mov eax, dword ptr [%c13]\n\t"
        "mov dword ptr fs:[0], eax\n\t"
        "popad\n\t"
        "ret"
        : : "i"(&g_co.eax), "i"(&g_co.ecx), "i"(&g_co.edx), "i"(&g_co.ebx), "i"(&g_co.esi), "i"(&g_co.edi),
            "i"(&g_co.ebp), "i"(&g_co.efl), "i"(&g_co.esp_after), "i"(&g_co.fs0_after), "i"(&g_co.save),
            "i"(&g_co.exited), "i"(&g_saved_esp), "i"(&g_saved_fs0));
#else
    __asm {
        pushad
        mov g_saved_esp, esp
        mov eax, dword ptr fs:[0]
        mov g_saved_fs0, eax
        mov edi, esp                    // the stack below: the same value in every pass (touched top-down: guard pages)
        sub edi, 4
        mov ecx, 0x8000
        mov eax, g_ci.fill
        std
        rep stosd
        cld
        fninit
        fldcw word ptr g_ci.cw
        test g_ci.preflags, 1
        jz pf2
        fld1
        fldz
        fdivp st(1), st
        fstp st(0)
    pf2:
        test g_ci.preflags, 2
        jz pf4
        fld1
        fchs
        fsqrt
        fstp st(0)
    pf4:
        test g_ci.preflags, 4
        jz pf_done
        fld1
        fld1
        fadd st(0), st(0)
        fld1
        faddp st(1), st
        fld1
        fdivrp st(1), st
        fstp st(0)
    pf_done:
        mov ecx, g_ci.nst
    ld:
        test ecx, ecx
        jz ld_done
        dec ecx
        lea eax, [ecx + ecx * 4]
        lea edx, g_ci.st
        fld tbyte ptr [edx + eax * 2]
        jmp ld
    ld_done:
        mov ecx, g_ci.nargs
    pa:
        test ecx, ecx
        jz pa_done
        dec ecx
        lea eax, g_ci.args
        push dword ptr [eax + ecx * 4]
        jmp pa
    pa_done:
        mov eax, g_ci.fs0
        test eax, eax
        jz no_fs
        mov dword ptr fs:[0], eax
    no_fs:
        mov g_co.esp_before, esp
        mov eax, g_ci.eax
        mov ecx, g_ci.ecx
        mov edx, g_ci.edx
        mov ebx, g_ci.ebx
        mov esi, g_ci.esi
        mov edi, g_ci.edi
        cmp g_ci.set_ebp, 0
        je keep_ebp
        mov ebp, g_ci.ebp
    keep_ebp:
        call dword ptr g_ci.fn
        mov g_co.eax, eax
        mov g_co.ecx, ecx
        mov g_co.edx, edx
        mov g_co.ebx, ebx
        mov g_co.esi, esi
        mov g_co.edi, edi
        mov g_co.ebp, ebp
        pushfd
        pop g_co.efl
        mov g_co.esp_after, esp
        mov eax, dword ptr fs:[0]
        mov g_co.fs0_after, eax
        fnsave g_co.save
        mov g_co.exited, 0
        cld
        mov esp, g_saved_esp
        mov eax, g_saved_fs0
        mov dword ptr fs:[0], eax
        popad
        ret
    }
#endif
}
// ExitProcess (and the event limit): end the pass, back to the thunk's caller
static uint32_t g_bail_code;
static __declspec(naked) void bail_back() {
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile(
        "fnsave [%c0]\n\t"
        "mov dword ptr [%c1], 1\n\t"
        "mov eax, dword ptr [%c2]\n\t"
        "mov dword ptr [%c3], eax\n\t"
        "cld\n\t"
        "mov esp, dword ptr [%c4]\n\t"
        "mov eax, dword ptr [%c5]\n\t"
        "mov dword ptr fs:[0], eax\n\t"
        "popad\n\t"
        "ret"
        : : "i"(&g_co.save), "i"(&g_co.exited), "i"(&g_bail_code), "i"(&g_co.exit_code), "i"(&g_saved_esp),
            "i"(&g_saved_fs0));
#else
    __asm {
        fnsave g_co.save
        mov g_co.exited, 1
        mov eax, g_bail_code
        mov g_co.exit_code, eax
        cld
        mov esp, g_saved_esp
        mov eax, g_saved_fs0
        mov dword ptr fs:[0], eax
        popad
        ret
    }
#endif
}
static bool g_in_call;
static void bail_exit(uint32_t code) {
    if (!g_in_call) { printf("ExitProcess(%u) outside a call\n", code); ExitProcess(4); }
    g_bail_code = code;
    bail_back();
}
static void bail_abort() {
    if (!g_in_call) return;
    g_nev = -1000000;
    const uint32_t v[7] = {E_Abort};
    g_log.insert(g_log.end(), v, v + 7);
    g_bail_code = 0xab0127;
    bail_back();
}
static __declspec(noinline) uint32_t run_guarded() {
#if defined(__GNUC__) && !defined(__clang__)
    const uint32_t code = vp_guard([](void*) { call_thunk(); }, nullptr);
    if (code) {
        __writefsdword(0, g_saved_fs0);        // the SEH chain as __except's unwind leaves it (the thunk's may not be)
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return code;
    }
    return 0;
#else
    __try {
        call_thunk();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const uint32_t code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return code;
    }
    return 0;
#endif
}
// what a run records
enum { R_EAX = 1, R_EDX = 2, R_ECX = 4, R_REGS = 8, R_FLAGS = 16, R_X87 = 32, R_ESP = 64, R_CW = 128 };
static std::vector<uint32_t> g_out;
static std::vector<const char*> g_out_what;
static void out(uint32_t v, const char* what) { g_out.push_back(v); g_out_what.push_back(what); }
static void prep(uint32_t fill = 0) {
    memset(&g_ci, 0, sizeof g_ci);
    g_ci.cw = 0x27f;
    g_ci.fill = fill;
    g_ci.eax = 0xa1a1a1a1; g_ci.ecx = 0xc1c1c1c1; g_ci.edx = 0xd1d1d1d1;
    g_ci.ebx = 0xb1b1b1b1; g_ci.esi = 0x51515151; g_ci.edi = 0xd2d2d2d2;
}
static void args(std::initializer_list<uint32_t> a) {
    g_ci.nargs = 0;
    for (uint32_t v : a) g_ci.args[g_ci.nargs++] = v;
}
static uint32_t g_fault;
static void run(uint32_t fn, unsigned what) {
    g_ci.fn = fn;
    memset(&g_co, 0, sizeof g_co);
    g_in_call = true;
    const uint32_t f = run_guarded();
    g_in_call = false;
    g_fault = f;
    unsigned int cw;
    _clearfp();
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" : : : VP_X87_CLOBBERS);
#else
    __asm fninit
#endif
    _controlfp_s(&cw, _CW_DEFAULT, 0xffffffff);
    out(f, "fault");
    out(g_co.exited, "exited");
    out(g_co.exit_code, "exit code");
    if (f || g_co.exited) {
        if ((what & (R_X87 | R_CW)) && g_co.exited) out(*(uint16_t*)g_co.save, "x87 control word at the exit");
        return;
    }
    if (what & R_EAX) out(g_co.eax, "eax");
    if (what & R_EDX) out(g_co.edx, "edx");
    if (what & R_ECX) out(g_co.ecx, "ecx");
    if (what & R_REGS) {
        out(g_co.ebx, "ebx"); out(g_co.esi, "esi"); out(g_co.edi, "edi"); out(g_co.ebp, "ebp");
    }
    out(g_co.esp_after - g_co.esp_before, "esp change");
    out(g_co.fs0_after == (g_ci.fs0 ? g_ci.fs0 : g_saved_fs0), "fs:[0] restored");
    if (what & R_FLAGS) out(g_co.efl & 0xcd5, "eflags");
    if (what & R_X87) {
        memset(g_co.save + 12, 0, 16);                 // the last instruction's and operand's addresses
        for (int i = 0; i < 108; i += 4) {
            static const char* const nm[27] = {"x87 cw", "x87 sw", "x87 tw", "", "", "", "", "st0 lo", "st0 mid", "st0 hi/st1", "st1", "st1 hi", "st2", "st2", "st2/st3", "st3", "st3", "st4", "st4", "st4/st5", "st5", "st5", "st6", "st6", "st6/st7", "st7", "st7"};
            uint32_t v;
            memcpy(&v, g_co.save + i, 4);
            out(v, nm[i / 4]);
        }
    } else if (what & R_CW) {
        out(*(uint16_t*)g_co.save, "x87 control word");
        out(*(uint16_t*)(g_co.save + 4) & 0x3fff, "x87 status word");
    }
}

// ---- the world: the CRT's state, the heap, the arena, the fakes, the log --------------------------------------------------
struct Range { uint32_t at, n; };
static const Range k_full[] = {{DATA_AT, DATA_SIZE}};
static const Range k_crt[] = {{0x004e1000, 0x2760}, {0x005024a0, 0x1760}, {0x005d5500, 0x1a2c}};
static bool g_full = true;
struct World {
    std::vector<uint8_t> data, heap, arena;
    Fake fk;
    std::vector<uint32_t> log;
    bool full;
};
static void save_world(World& w) {
    w.full = g_full;
    w.data.clear();
    if (g_full) w.data.assign((uint8_t*)DATA_AT, (uint8_t*)DATA_AT + DATA_SIZE);
    else
        for (const Range& r : k_crt) w.data.insert(w.data.end(), (uint8_t*)(uintptr_t)r.at, (uint8_t*)(uintptr_t)r.at + r.n);
    w.heap.assign(g_heap, g_heap + g_fk.heap_top);
    w.arena.assign(g_arena, g_arena + ARENA_SIZE);
    w.fk = g_fk;
    w.log = g_log;
}
static void load_world(const World& w) {
    const uint8_t* p = w.data.data();
    if (w.full) memcpy((void*)DATA_AT, p, DATA_SIZE);
    else
        for (const Range& r : k_crt) { memcpy((void*)(uintptr_t)r.at, p, r.n); p += r.n; }
    g_fk = w.fk;
    memcpy(g_heap, w.heap.data(), w.heap.size());
    if (g_heap_hi > g_fk.heap_top) memset(g_heap + g_fk.heap_top, 0, g_heap_hi - g_fk.heap_top);
    memcpy(g_arena, w.arena.data(), ARENA_SIZE);
    g_log = w.log;
}
static uint32_t data_va(const World& w, uint32_t i) {
    if (w.full) return DATA_AT + i;
    for (const Range& r : k_crt) {
        if (i < r.n) return r.at + i;
        i -= r.n;
    }
    return 0;
}
static const char* data_name(uint32_t va) {
    static char b[96];
    static const struct { uint32_t a, n; const char* nm; } k[] = {
        {0x502730, 4, "errno"}, {0x502734, 4, "_doserrno"}, {0x503430, 0x280, "_iob"}, {0x5d6e14, 4, "_nhandle"},
        {0x5d6e20, 0x100, "__pioinfo"}, {0x5d5dfc, 4, "_nstream"}, {0x5d5e00, 4, "__piob"}, {0x502b00, 0x101, "_mbctype"},
        {0x50273c, 4, "_osver"}, {0x502740, 4, "_winver"}, {0x502744, 4, "_winmajor"}, {0x502748, 4, "_winminor"},
        {0x5d6f28, 4, "_acmdln"}, {0x502680, 4, "_aenvptr"}, {0x502af8, 4, "aw_env f_use"}, {0x50274c, 4, "__argc"},
        {0x502750, 4, "__argv"}, {0x502758, 4, "_environ"}, {0x502768, 4, "_pgmptr"}, {0x5d5610, 0x104, "_pgmname"},
        {0x5d6f24, 4, "__onexitbegin"}, {0x5d6f20, 4, "__onexitend"}, {0x502774, 4, "_C_Termination_Done"},
        {0x502770, 1, "_exitflag"}, {0x5024bc, 4, "_FPinit"}, {0x5024b8, 4, "_adjust_fdiv"}, {0x50268c, 4, "__error_mode"},
        {0x502690, 4, "__app_type"}, {0x502688, 4, "_aexit_rtn"}, {0x5036f0, 12, "crtMessageBoxA's pointers"},
        {0x5d5760, 4, "_pnhHeap"}, {0x502a68, 120, "_XcptActTab"}, {0x502aec, 4, "_fpecode"}, {0x502af0, 4, "_pxcptinfoptrs"},
        {0x502a58, 16, "__NLG_Destination"}, {0x5026f8, 0x18, "_cfltcvt_tab"}, {0x5d6e10, 4, "_crtheap"},
        {0x5036ec, 4, "_matherr_flag"}, {0x4e1000, 0x2734, "__xc_a table"}, {0x4e3738, 0x20, "__xi / __xp / __xt tables"}};
    for (auto& x : k)
        if (va >= x.a && va < x.a + x.n) { snprintf(b, sizeof b, "%s+0x%x (%08x)", x.nm, va - x.a, va); return b; }
    snprintf(b, sizeof b, ".data %08x", va);
    return b;
}
static std::string world_diff(const World& a, const World& b) {
    char t[512];
    for (size_t i = 0; i < a.data.size() && i < b.data.size(); i++)
        if (a.data[i] != b.data[i]) {
            snprintf(t, sizeof t, "%s: %02x vs %02x", data_name(data_va(a, (uint32_t)i)), a.data[i], b.data[i]);
            return t;
        }
    if (a.heap.size() != b.heap.size()) {
        snprintf(t, sizeof t, "heap top %x vs %x", (unsigned)a.heap.size(), (unsigned)b.heap.size());
        return t;
    }
    for (size_t i = 0; i < a.heap.size(); i++)
        if (a.heap[i] != b.heap[i]) {
            snprintf(t, sizeof t, "heap +0x%x (%08x): %02x vs %02x", (unsigned)i, P(g_heap) + (unsigned)i, a.heap[i], b.heap[i]);
            return t;
        }
    for (size_t i = 0; i < ARENA_SIZE; i++)
        if (a.arena[i] != b.arena[i]) {
            snprintf(t, sizeof t, "arena +0x%x: %02x vs %02x", (unsigned)i, a.arena[i], b.arena[i]);
            return t;
        }
    if (memcmp(&a.fk, &b.fk, sizeof a.fk)) {
        size_t i = 0;
        while (((const uint8_t*)&a.fk)[i] == ((const uint8_t*)&b.fk)[i]) i++;
        snprintf(t, sizeof t, "the fakes' state +0x%x", (unsigned)i);
        return t;
    }
    const size_t n = std::min(a.log.size(), b.log.size());
    for (size_t i = 0; i < n; i++)
        if (a.log[i] != b.log[i]) {
            const size_t e = i / 7 * 7;
            snprintf(t, sizeof t, "call/event %u: %s (%08x %08x %08x %08x %08x %08x) vs %s (%08x %08x %08x %08x %08x %08x)",
                     (unsigned)(i / 7), kind_name(a.log[e]), a.log[e + 1], a.log[e + 2], a.log[e + 3], a.log[e + 4], a.log[e + 5],
                     a.log[e + 6], kind_name(b.log[e]), b.log[e + 1], b.log[e + 2], b.log[e + 3], b.log[e + 4], b.log[e + 5], b.log[e + 6]);
            return t;
        }
    if (a.log.size() != b.log.size()) {
        const std::vector<uint32_t>& l = a.log.size() > b.log.size() ? a.log : b.log;
        snprintf(t, sizeof t, "calls/events %u vs %u (the first extra: %s %08x %08x)", (unsigned)(a.log.size() / 7),
                 (unsigned)(b.log.size() / 7), kind_name(l[n]), l[n + 1], l[n + 2]);
        return t;
    }
    return "";
}

// ---- checks: original, isolated, chain -------------------------------------------------------------------------------------
enum { M_ORIG, M_ISO, M_CHAIN };
static int g_mode;
static const char* const k_mode[3] = {"original", "isolated", "chain"};
static uint32_t at_of(uint32_t v10) { return g_mode == M_ISO ? P(rewrite_of(v10)) : v10; }
struct Pass { World w; std::vector<uint32_t> out; std::vector<const char*> what; };
static Pass g_pass[3];
static World g_start;
static std::string g_desc;
struct Fam { long checks = 0, fails[3] = {}; };
static std::map<std::string, Fam> g_fam;
static long g_failures, g_checks;
static const char* g_section = "";
template <typename F> static bool check3(const char* fam, F&& body, unsigned modes = 7) {
    save_world(g_start);
    for (int m = 0; m < 3; m++) {
        if (!(modes >> m & 1)) continue;
        load_world(g_start);
        g_mode = m;
        g_out.clear();
        g_out_what.clear();
        g_nev = 0;
        if (m == M_CHAIN) chain_patch();
        body();
        if (m == M_CHAIN) chain_unpatch();
        g_pass[m].out = g_out;
        g_pass[m].what = g_out_what;
        save_world(g_pass[m].w);
    }
    Fam& fs = g_fam[std::string(g_section) + " / " + fam];
    fs.checks++;
    g_checks++;
    bool ok = true;
    for (int m = 1; m < 3; m++) {
        if (!(modes >> m & 1)) continue;
        const Pass& a = g_pass[0];
        const Pass& b = g_pass[m];
        std::string d;
        const size_t n = std::min(a.out.size(), b.out.size());
        for (size_t i = 0; i < n && d.empty(); i++)
            if (a.out[i] != b.out[i]) {
                char t[256];
                snprintf(t, sizeof t, "result %u (%s): %08x vs %08x", (unsigned)i, a.what[i], a.out[i], b.out[i]);
                d = t;
            }
        if (d.empty() && a.out.size() != b.out.size()) {
            char t[128];
            snprintf(t, sizeof t, "%u vs %u results", (unsigned)a.out.size(), (unsigned)b.out.size());
            d = t;
        }
        if (d.empty()) d = world_diff(a.w, b.w);
        if (!d.empty()) {
            ok = false;
            fs.fails[m]++;
            g_failures++;
            if (g_failures <= 60)
                printf("  MISMATCH [%s / %s] %s: %s\n      inputs: %s\n", g_section, fam, k_mode[m], d.c_str(), g_desc.c_str());
        }
    }
    load_world(g_pass[0].w);                         // carry on from the original's state
    return ok;
}

// ---- states ---------------------------------------------------------------------------------------------------------------------
static World g_pristine, g_base;
static Fake g_fk_default;
static void fake_defaults() {
    Fake& f = g_fk;
    const uint32_t top = f.heap_top;
    memset(&f, 0, sizeof f);
    f.heap_top = top;
    f.fail_alloc_at = -1;
    f.fail_realloc_at = -1;
    f.wctmb_fail_at = -1;
    f.version = 0xc0000a04;
    strcpy(f.cmdline, "\"C:\\Games\\Viper Racing\\race.exe\" -window");
    strcpy(f.modname, "C:\\Games\\Viper Racing\\race.exe");
    const wchar_t* ew = L"PATH=C:\\WINDOWS\0TEMP=C:\\TEMP\0=C:=C:\\Games\0VIPER=1\0";
    memcpy(f.envw, ew, 52 * 2);
    f.envw_avail = 1;
    memcpy(f.enva, "PATH=C:\\WINDOWS\0TEMP=C:\\TEMP\0=C:=C:\\Games\0VIPER=1\0", 52);
    f.enva_avail = 1;
    f.si_res2_null = 1;
    f.std_handle[0] = f.std_handle[1] = f.std_handle[2] = 0xffffffffu;
    f.acp = 1252;
    f.oemcp = 437;
    f.uef_ret = EXCEPTION_EXECUTE_HANDLER;
    f.raise_rng = 1;
    f.aff_proc = 1;
    f.aff_sys = 1;
    f.aff_thread = 1;
    f.msgbox_ret = IDOK;
}
static void set_xc_stubs(int n, bool real_xi) {
    // the __xc table: n entries of logging stubs (some null), the rest null; __xi real or stubs
    for (uint32_t a = CRT_XC_A; a < CRT_XC_Z; a += 4) G32(a) = 0;
    for (int i = 0; i < n && CRT_XC_A + 4u * i < CRT_XC_Z; i++) G32(CRT_XC_A + 4 * i) = chance(15) ? 0 : g_init_fn[ri(0, N_INIT - 1)];
    if (!real_xi) {
        G32(CRT_XI_A) = chance(50) ? 0 : g_init_fn[ri(0, N_INIT - 1)];
        G32(CRT_XI_A + 4) = 0x004cf010;               // __onexitinit stays (atexit needs it)
        G32(CRT_XI_A + 8) = chance(50) ? 0x004d4fd0 : g_init_fn[ri(0, N_INIT - 1)];
    }
}

// ---- small helpers --------------------------------------------------------------------------------------------------------
static void descf(const char* fmt, ...) {
    char b[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_desc = b;
}
static uint32_t pick(std::initializer_list<uint32_t> v) { return *(v.begin() + rnd() % v.size()); }
// image code run outside a check (set-up, the carried start-up): the original, its results not compared
static uint32_t orig_call(uint32_t fn, std::initializer_list<uint32_t> a, const char* what, uint32_t fill = 0) {
    const int m = g_mode;
    g_mode = M_ORIG;
    prep(fill);
    args(a);
    const size_t n = g_out.size();
    run(fn, R_EAX);
    g_out.resize(n);
    g_out_what.resize(n);
    g_mode = m;
    if (g_fault || g_co.exited)
        printf("  (set-up call %s: fault %08x, exited %u, code %u)\n", what, g_fault, g_co.exited, g_co.exit_code);
    return g_co.eax;
}
static uint32_t hmalloc(uint32_t n) { return orig_call(0x004d1de0, {n}, "malloc"); }
static void arena_clear() {
    memset(g_arena, 0, ARENA_SIZE);
    g_aused = 0;
}
static uint64_t dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static double bitsd(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
// a double from every class (world_crt_fmt.cpp's classes)
static uint64_t rand_double() {
    switch (rnd() % 14) {
    case 0: return rnd64();
    case 1: case 2: {
        static const uint64_t sp[] = {0, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,
                                      0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff0000000000001ull,
                                      0x7ff4000000000000ull, 0xfff0000000000001ull, 0x0000000000000001ull,
                                      0x000fffffffffffffull, 0x0010000000000000ull, 0x7fefffffffffffffull,
                                      0x3ff0000000000000ull, 0xbff0000000000000ull, 0x3fe0000000000000ull,
                                      0x7fffffffffffffffull, 0xffffffffffffffffull, 0x8000000000000001ull,
                                      0x3ff0000000000001ull, 0x3fefffffffffffffull, 0xbfefffffffffffffull,
                                      0x400921fb54442d18ull, 0xc00921fb54442d18ull, 0x3ff921fb54442d18ull};
        return sp[rnd() % (sizeof sp / sizeof sp[0])];
    }
    case 3: return rnd64() & 0x800fffffffffffffull;                                   // denormals
    case 4: return dbits((double)(int32_t)rnd() / (double)(1 << ri(0, 30)));
    case 5: return dbits((double)ri(-1000, 1000));
    case 6: return ((uint64_t)ri(0, 2047) << 52) | (rnd64() & 0x800fffffffffffffull);   // any exponent
    case 7: return dbits(ldexp(1.0, ri(-1074, 1023)) * (chance(50) ? 1 : -1));
    case 8: return dbits((double)ri(-99999, 99999) / 1000.0);
    case 9: return dbits((double)ri(-2000, 2000) / 1000.0);                         // around [-1, 1]
    default: return ((uint64_t)ri(0x3c0, 0x440) << 52) | (rnd64() & 0x800fffffffffffffull);
    }
}
// an 80-bit value: from a double mostly, else any bits (NaNs quiet and signalling, unnormals)
static void rand_ext(uint8_t out[10]) {
    if (chance(75)) {
        const uint64_t u = rand_double();
        double d = bitsd(u);
        uint8_t* o = out;
#if defined(__GNUC__) && !defined(__clang__)
        __asm__ volatile("fld %0\n\t"
                         "mov eax, %1\n\t"
                         "fstp tbyte ptr [eax]"
                         : : "m"(d), "m"(o) : VP_X87_CLOBBERS, "eax", "memory");
#else
        __asm {
            fld qword ptr d
            mov eax, o
            fstp tbyte ptr [eax]
        }
#endif
        if ((u & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (u & 0xfffffffffffffull) && chance(50)) {
            out[7] &= 0xbf;                                                                // signalling again
            out[0] |= 1;
        }
    } else {
        uint64_t m = rnd64();
        if (chance(70)) m |= 0x8000000000000000ull;
        memcpy(out, &m, 8);
        const uint16_t e = (uint16_t)(chance(20) ? (0x7fff | (rnd() & 0x8000)) : chance(40) ? rnd()
                                      : (0x3fff + ri(-80, 80)) | (rnd() & 0x8000));
        memcpy(out + 8, &e, 2);
    }
}
// a control word with every exception masked: 24 / 53 / 64-bit precision, any rounding
static uint32_t rand_cw_masked() {
    static const uint32_t pcs[] = {0x000, 0x200, 0x300};
    return 0x7fu | pcs[rnd() % 3] | (rnd() & 3) << 10;
}
static std::string hexs(const void* p, int n) {
    std::string s;
    char b[4];
    for (int i = n - 1; i >= 0; i--) { snprintf(b, sizeof b, "%02x", ((const uint8_t*)p)[i]); s += b; }
    return s;
}

// ---- the WinMain stub (0x412260, __stdcall): the arguments, the start-up frame, the CRT's state; then an action --------
static uint32_t g_wm_ebp, g_wm_esp;
static uint32_t g_orig_scope, g_orig_filter, g_orig_handler;   // the original's scope table (0x4e01a0) and its entries
static uint32_t wm_norm(uint32_t a) {
    if (a == g_orig_scope || a == P(crt_start::k_wmcs_scope)) return 0x5c09e;
    if (a == g_orig_filter || a == P(&crt_start::crt_wmcs_filter)) return 0xf117e2;
    if (a == g_orig_handler || a == P(&crt_start::crt_wmcs_handler)) return 0x4a2d1e;
    return a;
}
static int __stdcall wm_c(uint32_t hinst, uint32_t prev, const char* cmd, int show) {
    const uint32_t ebp = g_wm_ebp;
    const uint32_t* f = (const uint32_t*)(uintptr_t)ebp;
    lg(E_WinMain, hinst, prev, P(cmd) - G32(CRT_ACMDLN_VA), hs(cmd), (uint32_t)show);
    const uint32_t fs0 = __readfsdword(0);
    const uint32_t scope = f[-2];
    const uint32_t* st = (const uint32_t*)(uintptr_t)scope;
    lg(E_WinMainFrame, f[-1], (fs0 == ebp - 0x10) | (f[-4] == g_saved_fs0) << 1, f[-3], wm_norm(scope), ebp - f[-6],
       ebp - g_wm_esp);
    lg(E_WinMainFrame, st[0], wm_norm(st[1]), wm_norm(st[2]), fnv((const void*)(uintptr_t)(ebp - 0x70), 0x44));
    lg(E_WinMainState, fnv((void*)DATA_AT, DATA_SIZE), g_fk.heap_top, fnv(g_heap, g_fk.heap_top));
    switch (g_fk.winmain_action) {
    case 1: ((void(__cdecl*)(uint32_t))0x004d1c70)(g_fk.winmain_code); break;          // exit
    case 2: ((void(__cdecl*)(uint32_t))0x004d1c90)(g_fk.winmain_code); break;          // _exit
    case 3: {                                                                         // atexit, then return
        const int32_t r = ((int32_t(__cdecl*)(uint32_t))0x004ceff0)(g_exit_fn[g_fk.winmain_flags % N_EXIT]);
        lg(E_AtexitRet, 0x3a17, (uint32_t)r);
        break;
    }
    }
    lg(E_Returned, g_fk.winmain_ret);
    return (int)g_fk.winmain_ret;
}
static __declspec(naked) void wm_entry() {
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("mov dword ptr [%c0], ebp\n\t"
                     "mov dword ptr [%c1], esp\n\t"
                     "jmp %P2"
                     : : "i"(&g_wm_ebp), "i"(&g_wm_esp), "i"(&wm_c));
#else
    __asm {
        mov g_wm_ebp, ebp
        mov g_wm_esp, esp
        jmp wm_c
    }
#endif
}

// ---- the world's random parts ------------------------------------------------------------------------------------------
static uint32_t rand_handle() {
    switch (rnd() % 8) {
    case 0: return 0xffffffffu;
    case 1: return 0;
    default: return 0x100u + 4u * (rnd() % 4000);
    }
}
static void rand_si(Fake& f) {
    f.si_flags = chance(50) ? STARTF_USESHOWWINDOW : chance(50) ? 0 : rnd();
    f.si_show = rnd() & (chance(70) ? 0xfu : 0xffffu);
    f.si_res2_null = chance(20);
    f.si_cb2 = chance(90) ? (uint32_t)ri(1, 0xffff) : 0;
    int32_t n;
    switch (rnd() % 10) {
    case 0: n = 0; break;
    case 1: n = ri(1, 8); break;
    case 2: case 3: n = ri(1, 64); break;
    case 4: n = ri(1, 400); break;
    case 5: n = (int32_t)pick({2047, 2048, 2049, 2016, 2017, 32, 33}); break;
    case 6: n = ri(2049, 3200); break;
    case 7: n = (int32_t)(0x80000000u | rnd()); break;
    default: n = ri(0, 3200); break;
    }
    memset(f.si_res2, 0, sizeof f.si_res2);
    memcpy(f.si_res2, &n, 4);
    const int nn = n < 0 ? 0 : n;
    for (int i = 0; i < nn; i++) f.si_res2[4 + i] = (uint8_t)(chance(75) ? rnd() | 1 : rnd() & ~1u);
    for (int i = 0; i < nn; i++) {
        const uint32_t h = rand_handle();
        memcpy(f.si_res2 + 4 + nn + 4 * i, &h, 4);
    }
    for (int i = 0; i < 3; i++) f.std_handle[i] = chance(30) ? 0xffffffffu : rand_handle();
}
static char lead_byte() { return (char)(0x81 + rnd() % 0x7e); }
static std::string rand_frag(bool leads, int n) {
    std::string s;
    while ((int)s.size() < n) {
        switch (rnd() % 15) {
        case 0: case 1: case 2: case 3: s += (char)('a' + rnd() % 26); break;
        case 4: s.append(ri(1, 5), '\\'); break;
        case 5: s += '"'; break;
        case 6: s += "\"\""; break;
        case 7: s += ' '; break;
        case 8: s += '\t'; break;
        case 9:
            if (leads) {
                s += lead_byte();
                s += chance(70) ? (char)(0x40 + rnd() % 0xbf) : (char)pick({' ', '"', '\\', '\t'});
            } else {
                s += 'x';
            }
            break;
        case 10: s += (char)(0x80 + rnd() % 0x80); break;
        case 11: s += (char)pick({'.', '-', '/', ':', '=', '\'', ',', '?', '*'}); break;
        case 12: s.append(ri(0, 4), '\\'); s += '"'; break;
        case 13: s.append(ri(1, 4), '\\'); s += "\"\""; break;
        default: s += (char)('0' + rnd() % 10); break;
        }
    }
    return s;
}
static std::string rand_cmdline(bool leads, int maxlen) {
    std::string s;
    switch (rnd() % 7) {
    case 0: s = "\"C:\\Program Files\\Viper Racing\\race.exe\""; break;
    case 1: s = "race.exe"; break;
    case 2: s = "\"\""; break;
    case 3: break;
    case 4: s = "\"C:\\Games\\" + rand_frag(leads, ri(0, 12)); if (chance(50)) s += "\""; break;
    case 5: s = "\"" + rand_frag(leads, ri(0, 20)) + "\""; break;
    default: s = rand_frag(leads, ri(1, 20)); break;
    }
    const int n = ri(0, 14);
    for (int i = 0; i < n && (int)s.size() < maxlen - 40; i++) {
        s += chance(80) ? " " : "\t";
        if (chance(20)) s += chance(50) ? " " : "\t";
        s += rand_frag(leads, ri(0, 30));
    }
    if (chance(15)) s += chance(50) ? " " : "\t ";
    if (leads && chance(15)) s += lead_byte();                     // a lead byte before the NUL
    if ((int)s.size() > maxlen) s.resize(maxlen);
    return s;
}
static void rand_mbctype() {
    for (int c = 0; c < 256; c++) G8(CRT_MBCTYPE_VA + 1 + c) &= (uint8_t)~4u;
    switch (rnd() % 4) {
    case 0: break;
    case 1:
        for (int c = 0x81; c <= 0x9f; c++) G8(CRT_MBCTYPE_VA + 1 + c) |= 4;
        for (int c = 0xe0; c <= 0xfc; c++) G8(CRT_MBCTYPE_VA + 1 + c) |= 4;
        break;
    case 2: for (int c = 0x81; c <= 0xfe; c++) G8(CRT_MBCTYPE_VA + 1 + c) |= 4; break;
    default:
        for (int c = 0x80; c < 256; c++)
            if (chance(30)) G8(CRT_MBCTYPE_VA + 1 + c) |= 4;
        if (chance(15)) G8(CRT_MBCTYPE_VA + 1 + pick({'"', '\\', ' ', '\t', 'a'})) |= 4;   // odd lead bytes (never NUL)
        break;
    }
}
// an environment: ANSI and wide blocks of the same strings ("=X:" drive entries, empty values, characters outside ASCII)
static void rand_env(Fake& f) {
    std::vector<std::wstring> v;
    const int n = chance(5) ? 0 : ri(1, 40);
    for (int i = 0; i < n; i++) {
        std::wstring s;
        switch (rnd() % 6) {
        case 0: s = L"="; s += (wchar_t)('A' + rnd() % 26); s += L":=C:\\x"; break;
        case 1: s = L"=ExitCode=00000000"; break;
        default: {
            const int k = ri(1, 12);
            for (int j = 0; j < k; j++) s += (wchar_t)('A' + rnd() % 26);
            s += L"=";
            const int m = ri(0, 60);
            for (int j = 0; j < m; j++) {
                const int c = rnd() % 20;
                s += c < 14 ? (wchar_t)(0x20 + rnd() % 0x5f) : c < 16 ? (wchar_t)(0xa0 + rnd() % 0x60)
                     : c < 18 ? (wchar_t)(0x100 + rnd() % 0x2000) : (wchar_t)(0x4e00 + rnd() % 0x500);
            }
            break;
        }
        }
        v.push_back(s);
    }
    memset(f.envw, 0, sizeof f.envw);
    memset(f.enva, 0, sizeof f.enva);
    size_t w = 0, a = 0;
    for (const std::wstring& s : v) {
        if (w + s.size() + 2 >= ENVW_N || a + s.size() + 2 >= ENVA_N) break;
        memcpy(f.envw + w, s.data(), s.size() * 2);
        w += s.size() + 1;
        for (wchar_t c : s) f.enva[a++] = c < 0x100 ? (char)c : (char)(0x81 + c % 0x7e);
        a++;
    }
}
static uint32_t enva_len(const Fake& f) {                     // the ANSI block's bytes, with the final NUL
    if (!f.enva[0]) return 1;
    uint32_t len = 0;
    while (f.enva[len] || f.enva[len + 1]) len++;
    return len + 2;
}
static void startup_fakes() {
    fake_defaults();
    Fake& f = g_fk;
    rand_si(f);
    f.version = pick({0xc0000a04, 0x0a280105, 0x23f00206, 0x80000004, 0}) ^ (chance(20) ? rnd() : 0);
    f.acp = pick({1252, 1252, 1252, 932, 936, 949, 950, 437, 1250});
    strcpy(f.cmdline, rand_cmdline(f.acp != 1252 && f.acp != 1250 && f.acp != 437, 700).c_str());
    if (chance(70)) rand_env(f);
    f.envw_avail = chance(85);
    f.enva_avail = chance(85);
    f.fdiv_sched = chance(50) ? 0 : rnd();
    f.aff_proc = chance(70) ? 1 : rnd() | 1;
    f.aff_sys = f.aff_proc | (chance(50) ? rnd() : 0);
    f.aff_fail = chance(5);
    f.loadlib_fail = chance(5) ? 1 : 0;
    f.gpa_fail = chance(5) ? 1u << ri(0, 3) : 0;
    f.heapcreate_fail = chance(3);
    f.fail_alloc_at = chance(5) ? ri(0, 80) : -1;
    f.wctmb_fail_at = chance(5) ? ri(0, 1) : -1;
    f.next_vh = rnd() & 0xff;
}
static void set_fpinit() { G32(CRT_FPINIT_VA) = pick({0x004ce120, 0x004ce120, 0x004ce120, 0, g_misc_fn[4]}); }
// streams for _fcloseall / __endstdio: _iob's (3..19) and malloc'd ones (20..), open for reading / writing (with
// buffered bytes) or not, on lowio handles 3..31
static void make_streams(int k) {
    const int ns = (int)G32(CRT_NSTREAM_VA);
    uint32_t* piob = (uint32_t*)(uintptr_t)G32(CRT_PIOB_VA);
    if (!piob) return;
    for (int i = 3; i < 3 + k && i < ns; i++) {
        if (i >= 20 && chance(15)) continue;
        CrtFile* f = i < 20 ? &CRT_IOB[i] : (CrtFile*)(uintptr_t)hmalloc(0x20);
        if (!f) continue;
        piob[i] = P(f);
        memset(f, 0, sizeof *f);
        const int fh = ri(3, 31);
        CrtIoinfo* io = crt_ioinfo(fh);
        if (chance(85)) {
            io->osfhnd = (intptr_t)(0x2000 + 4 * fh);
            io->osfile = (uint8_t)(CRT_FOPEN | (chance(50) ? CRT_FTEXT : 0) | (chance(10) ? CRT_FDEV : 0) |
                                   (chance(10) ? CRT_FAPPEND : 0));
        }
        f->file = fh;
        switch (rnd() % 6) {
        case 0: f->flag = 0; break;
        case 1: f->flag = CRT_IOREAD; break;
        case 2: case 3: {
            const uint32_t bs = pick({16, 64, 512});
            const uint32_t b = hmalloc(bs);
            if (!b) break;
            for (uint32_t j = 0; j < bs; j++) ((uint8_t*)(uintptr_t)b)[j] = (uint8_t)pick({'a', '\n', '\r', 0x1a, rnd()});
            const uint32_t used = (uint32_t)ri(0, (int)bs);
            f->base = (char*)(uintptr_t)b;
            f->ptr = f->base + used;
            f->cnt = (int32_t)(bs - used);
            f->bufsiz = (int32_t)bs;
            f->flag = CRT_IOWRT | CRT_IOMYBUF | (chance(20) ? CRT_IORW : 0);
            break;
        }
        case 4: {
            const uint32_t bs = 64, b = hmalloc(bs);
            if (!b) break;
            f->base = (char*)(uintptr_t)b;
            f->cnt = ri(0, 64);
            f->ptr = f->base + (bs - f->cnt);
            f->bufsiz = (int32_t)bs;
            f->flag = CRT_IOREAD | CRT_IOMYBUF | (chance(50) ? CRT_IORW : 0);
            break;
        }
        default:
            f->base = (char*)&f->charbuf;
            f->ptr = f->base;
            f->bufsiz = 2;
            f->flag = CRT_IOWRT | CRT_IONBF;
            break;
        }
    }
}
// before an exit: an exit function may register another only while the table has room (a realloc that moves the table
// under doexit's loop leaves it reading freed memory -- a fault in the original too, not a case worth running)
static void exit_room() {
    const uint32_t b = G32(CRT_ONEXITBEGIN_VA);
    if (!b) return;
    const uint32_t have = orig_call(0x004d1dc0, {b}, "_msize");
    if (have < G32(CRT_ONEXITEND_VA) - b + 16) g_fk.exit_depth = 3;
}
static void rand_exit_tables() {
    G32(CRT_XP_A + 4) = chance(70) ? 0x004d50a0 : chance(50) ? 0 : g_exit_fn[ri(0, N_EXIT - 1)];   // __endstdio
    G32(CRT_XP_A) = chance(80) ? 0 : g_exit_fn[ri(0, N_EXIT - 1)];
    G32(CRT_XT_A) = chance(70) ? 0 : g_exit_fn[ri(0, N_EXIT - 1)];
}
static bool g_exited0() { return g_pass[0].out.size() > 1 && (g_pass[0].out[0] || g_pass[0].out[1]); }

// ---- startup -------------------------------------------------------------------------------------------------------------
static void sec_startup(int n) {
    g_section = "startup";
    g_full = true;
    for (int it = 0; it < n; it++) {
        load_world(g_pristine);
        arena_clear();
        startup_fakes();
        set_xc_stubs(ri(0, 60), true);
        set_fpinit();
        const uint32_t fill = chance(50) ? 0 : rnd();
        descf("startup #%d: cmdline [%s] acp %u version %08x si res2 %s n %d fdiv %08x fpinit %08x", it, g_fk.cmdline,
              g_fk.acp, g_fk.version, g_fk.si_res2_null ? "null" : "set", *(int32_t*)g_fk.si_res2, g_fk.fdiv_sched,
              G32(CRT_FPINIT_VA));
        check3("heap_init", [&] { prep(fill); run(at_of(0x004d4470), R_EAX | R_REGS); });
        if (G32(CRT_CRTHEAP_VA) != P(FAKE_HEAP)) continue;
        check3("ioinit", [&] { prep(fill); run(at_of(0x004d4290), R_REGS); });
        if (g_exited0()) continue;                                            // malloc failed: R6027
        orig_call(0x004d4280, {}, "__initmbctable");
        G32(CRT_ACMDLN_VA) = P(g_fk.cmdline);
        if (chance(15)) G32(CRT_AW_ENV_FUSE_VA) = pick({1, 2});               // as on a second call
        check3("crtGetEnvironmentStringsA", [&] {
            prep(0);
            run(at_of(0x004d3e20), R_EAX | R_ECX | R_EDX | R_REGS);
            if (!g_fault && !g_co.exited) G32(CRT_AENVPTR_VA) = g_co.eax;
        });
        if (!G32(CRT_AENVPTR_VA)) continue;
        check3("setargv", [&] { prep(fill); run(at_of(0x004d3ba0), R_EAX | R_REGS); });
        if (g_exited0()) continue;
        check3("setenvp", [&] { prep(fill); run(at_of(0x004d3ac0), R_REGS); });
        if (g_exited0()) continue;
        check3("cinit", [&] { prep(fill); run(at_of(0x004d1c40), R_REGS | R_X87); });
        if (g_exited0() || !G32(CRT_ONEXITBEGIN_VA)) continue;
        g_fk.fail_alloc_at = -1;
        {                                                     // atexit / _onexit: a sequence growing the table
            const int k = chance(30) ? ri(20, 120) : ri(0, 12);
            std::vector<uint32_t> fns, which;
            for (int i = 0; i < k; i++) {
                fns.push_back(g_exit_fn[ri(0, N_EXIT - 1)]);
                which.push_back(chance(50) ? 0x004ceff0 : 0x004cef70);
            }
            if (chance(15)) g_fk.fail_realloc_at = g_fk.realloc_n + ri(0, 5);
            check3("atexit / _onexit", [&] {
                for (int i = 0; i < k; i++) {
                    prep(fill);
                    args({fns[i]});
                    run(at_of(which[i]), R_EAX | R_REGS);
                    if (g_fault || g_co.exited) break;
                }
            });
            g_fk.fail_realloc_at = -1;
        }
        if (chance(40)) {
            make_streams(ri(0, 40));
            check3("fcloseall", [&] { prep(fill); run(at_of(0x004d71d0), R_EAX | R_REGS); });
        }
        World here;                                           // the exits, each from here
        save_world(here);
        for (int v = 0; v < 3; v++) {
            load_world(here);
            rand_exit_tables();
            exit_room();
            if (chance(30)) make_streams(ri(0, 20));
            const uint32_t code = pick({0, 1, 0xff, 0xffffffff, rnd()});
            const uint32_t quick = chance(50), ret = chance(30) ? pick({1, 0x100, rnd()}) : 0;
            if (v == 0) check3("doexit", [&] { prep(fill); args({code, quick, ret}); run(at_of(0x004d1cb0), R_REGS | R_CW); });
            if (v == 1) check3("exit", [&] { prep(fill); args({code}); run(at_of(0x004d1c70), R_REGS | R_CW); });
            if (v == 2) check3("_exit", [&] { prep(fill); args({code}); run(at_of(0x004d1c90), R_REGS | R_CW); });
        }
    }
}

// ---- argv ------------------------------------------------------------------------------------------------------------------
static void sec_argv(int n) {
    g_section = "argv";
    g_full = false;
    for (int it = 0; it < n; it++) {
        load_world(g_base);
        arena_clear();
        const bool leads = chance(60);
        if (leads) rand_mbctype();
        const std::string s = rand_cmdline(leads, 900);
        uint8_t* cl = aalloc((uint32_t)s.size() + 24);
        memcpy(cl, s.data(), s.size());
        int32_t* na = (int32_t*)aalloc(4);
        int32_t* nc = (int32_t*)aalloc(4);
        *na = 0x5a5a5a5a;
        *nc = 0x5a5a5a5a;
        uint32_t* av = (uint32_t*)aalloc(4 * 1200);
        uint8_t* ab = aalloc(0x2400);
        const int way = (int)(rnd() % 4);                                 // count, fill, argv only, args only
        const uint32_t a_av = way == 1 || way == 2 ? P(av) : 0, a_ab = way == 1 || way == 3 ? P(ab) : 0;
        descf("[%s] (%u bytes) way %d leads %d", s.c_str(), (unsigned)s.size(), way, leads);
        check3("parse_cmdline", [&] { prep(); args({P(cl), a_av, a_ab, P(na), P(nc)}); run(at_of(0x004d3c40), R_REGS); });
        if (chance(35)) {
            load_world(g_base);
            if (leads) rand_mbctype();
            G32(CRT_ACMDLN_VA) = chance(12) ? P(cl + s.size() + 2) : P(cl);  // empty: the module's path is parsed
            const std::string m = chance(50) ? std::string("C:\\Games\\Viper Racing\\race.exe") : rand_cmdline(leads, 300);
            strncpy(g_fk.modname, m.c_str(), MOD_N - 1);
            g_fk.mod_ret0 = chance(5);
            if (chance(10)) g_fk.fail_alloc_at = g_fk.alloc_n;
            descf("_setargv: [%s] module [%s] fail_alloc %d", (const char*)(uintptr_t)G32(CRT_ACMDLN_VA), g_fk.modname,
                  g_fk.fail_alloc_at);
            check3("setargv", [&] { prep(); run(at_of(0x004d3ba0), R_EAX | R_REGS); });
        }
    }
}

// ---- env -------------------------------------------------------------------------------------------------------------------
static void sec_env(int n) {
    g_section = "env";
    g_full = false;
    for (int it = 0; it < n; it++) {
        load_world(g_base);
        arena_clear();
        rand_env(g_fk);
        g_fk.envw_avail = chance(80);
        g_fk.enva_avail = chance(80);
        const uint32_t fuse = pick({0, 0, 0, 1, 2, 3});
        G32(CRT_AW_ENV_FUSE_VA) = fuse;
        if (chance(15)) g_fk.wctmb_fail_at = g_fk.wctmb_n + ri(0, 1);
        if (chance(10)) g_fk.fail_alloc_at = g_fk.alloc_n;
        // the uninitialised local a second call reads: 0, or one of the blocks (copies in the arena)
        uint8_t* wb = aalloc(ENVW_N * 2 + 8);
        memcpy(wb, g_fk.envw, ENVW_N * 2);
        uint8_t* ab = aalloc(ENVA_N + 8);
        memcpy(ab, g_fk.enva, ENVA_N);
        const uint32_t fill = fuse ? pick({0, 0, P(wb), P(ab)}) : 0;
        descf("f_use %u w %d a %d wctmb_fail %d alloc_fail %d fill %08x", fuse, g_fk.envw_avail, g_fk.enva_avail,
              g_fk.wctmb_fail_at, g_fk.fail_alloc_at, fill);
        check3("crtGetEnvironmentStringsA", [&] { prep(fill); run(at_of(0x004d3e20), R_EAX | R_ECX | R_EDX | R_REGS); });
        // _setenvp on an ANSI block
        load_world(g_base);
        rand_env(g_fk);
        const uint32_t len = enva_len(g_fk);
        const uint32_t b = hmalloc(len);
        if (!b) continue;
        memcpy((void*)(uintptr_t)b, g_fk.enva, len);
        G32(CRT_AENVPTR_VA) = b;
        if (chance(15)) g_fk.fail_alloc_at = g_fk.alloc_n + ri(0, 20);
        descf("_setenvp: %u bytes, alloc_fail %d", len, g_fk.fail_alloc_at);
        check3("setenvp", [&] { prep(); run(at_of(0x004d3ac0), R_REGS); });
    }
}

// ---- exit ------------------------------------------------------------------------------------------------------------------
static void sec_exit(int n) {
    g_section = "exit";
    g_full = true;
    for (int it = 0; it < n; it++) {
        load_world(g_base);
        arena_clear();
        const uint32_t fill = chance(50) ? 0 : rnd();
        descf("exit #%d", it);
        if (chance(30)) {
            if (chance(15)) g_fk.fail_alloc_at = g_fk.alloc_n;
            check3("onexitinit", [&] { prep(fill); run(at_of(0x004cf010), R_REGS); });
            g_fk.fail_alloc_at = -1;
            if (g_exited0()) continue;
        }
        const int k = chance(40) ? ri(20, 200) : ri(0, 20);
        std::vector<uint32_t> fns, which;
        for (int i = 0; i < k; i++) {
            fns.push_back(g_exit_fn[ri(0, N_EXIT - 1)]);
            which.push_back(chance(50) ? 0x004ceff0 : 0x004cef70);
        }
        if (chance(20)) g_fk.fail_realloc_at = g_fk.realloc_n + ri(0, 10);
        descf("exit #%d: %d registrations, realloc fails at %d", it, k, g_fk.fail_realloc_at);
        check3("atexit / _onexit", [&] {
            for (int i = 0; i < k; i++) {
                prep(fill);
                args({fns[i]});
                run(at_of(which[i]), R_EAX | R_REGS);
                if (g_fault || g_co.exited) break;
            }
        });
        g_fk.fail_realloc_at = -1;
        if (chance(50)) {
            make_streams(ri(0, 40));
            check3("fcloseall", [&] { prep(fill); run(at_of(0x004d71d0), R_EAX | R_REGS); });
        }
        rand_exit_tables();
        exit_room();
        if (chance(40)) make_streams(ri(0, 30));
        const uint32_t code = pick({0, 1, 0xff, 0xffffffff, rnd()});
        const uint32_t quick = chance(50), ret = chance(40) ? pick({1, 0x100, rnd()}) : 0;
        descf("exit #%d: %d functions, code %d quick %d retcaller %d, __xp %08x %08x __xt %08x", it, k, code, quick, ret,
              G32(CRT_XP_A), G32(CRT_XP_A + 4), G32(CRT_XT_A));
        switch (rnd() % 3) {
        case 0: check3("doexit", [&] { prep(fill); args({code, quick, ret}); run(at_of(0x004d1cb0), R_REGS | R_CW); }); break;
        case 1: check3("exit", [&] { prep(fill); args({code}); run(at_of(0x004d1c70), R_REGS | R_CW); }); break;
        default: check3("_exit", [&] { prep(fill); args({code}); run(at_of(0x004d1c90), R_REGS | R_CW); }); break;
        }
    }
}

// ---- msg: the fatal-message path and the small routines ----------------------------------------------------------------------
static void sec_msg(int n) {
    g_section = "msg";
    g_full = false;
    for (int it = 0; it < n; it++) {
        load_world(g_base);
        arena_clear();
        G32(CRT_ERROR_MODE_VA) = pick({0, 0, 1, 1, 2, 3, rnd()});
        G32(CRT_APP_TYPE_VA) = pick({0, 1, 1, 2, 2, 3});
        G32(CRT_ADBGMSG_VA) = chance(20) ? g_misc_fn[1] : 0;
        G32(CRT_AEXIT_RTN_VA) = chance(50) ? 0x004d1c90 : g_misc_fn[2];
        CrtIoinfo* io2 = (CrtIoinfo*)(uintptr_t)(G32(CRT_PIOINFO_VA) + 16);
        io2->osfhnd = chance(40) ? -1 : (intptr_t)(0x1000 + 4 * ri(0, 64));
        g_fk.std_handle[2] = chance(30) ? 0xffffffffu : 0x1000 + 4 * ri(0, 64);
        {
            const uint32_t len = pick({0, 1, 30, 58, 59, 60, 61, 62, 100, 0x103, 0x104, 0x105, 0x200, (uint32_t)ri(0, 0x120)});
            std::string m = "C:\\";
            while (m.size() < len) m += (char)('a' + rnd() % 26);
            m.resize(len);
            strcpy(g_fk.modname, m.c_str());
        }
        g_fk.mod_ret0 = chance(10);
        g_fk.loadlib_fail = chance(10) ? 2 : 0;
        g_fk.gpa_fail = chance(20) ? (rnd() & 7) << 4 : 0;
        g_fk.active_window = chance(50) ? 0 : 0x10000 + (rnd() & 0xffff);
        g_fk.popup = chance(50) ? 0 : 0x20000 + (rnd() & 0xffff);
        g_fk.msgbox_ret = pick({IDOK, IDCANCEL, 0, rnd()});
        if (chance(30)) {                                      // the pointers cached by an earlier call
            G32(CRT_PFN_MESSAGEBOX_VA) = P(&fk_MessageBoxA);
            G32(CRT_PFN_GETACTIVEWINDOW_VA) = chance(80) ? P(&fk_GetActiveWindow) : 0;
            G32(CRT_PFN_GETLASTACTIVEPOPUP_VA) = chance(80) ? P(&fk_GetLastActivePopup) : 0;
        }
        G32(CRT_PNHHEAP_VA) = chance(30) ? 0 : g_misc_fn[3];
        g_fk.newh_ret = (int32_t)pick({0, 1, 2, 0xffffffff});
        const uint32_t code = chance(75) ? G32(CRT_RTERRS_VA + 8 * ri(0, 16)) : pick({0, 1, 3, 7, 11, 100, 253, 254, 256, rnd()});
        const uint32_t fill = chance(50) ? 0 : rnd();
        char* text = (char*)aalloc(300);
        const int tl = ri(0, 250);
        for (int i = 0; i < tl; i++) text[i] = (char)(0x20 + rnd() % 0x5f);
        const uint32_t typ = rnd();
        descf("msg #%d: error mode %u app type %u code %u module %u chars ret0 %d loadlib_fail %u gpa_fail %02x aexit %08x "
              "stderr %08x", it, G32(CRT_ERROR_MODE_VA), G32(CRT_APP_TYPE_VA), code, (unsigned)strlen(g_fk.modname),
              g_fk.mod_ret0, g_fk.loadlib_fail, g_fk.gpa_fail, G32(CRT_AEXIT_RTN_VA), (uint32_t)io2->osfhnd);
        switch (rnd() % 16) {
        case 0: case 1: check3("_amsg_exit", [&] { prep(fill); args({code}); run(at_of(0x004cf730), R_REGS); }); break;
        case 2: check3("_FF_MSGBANNER", [&] { prep(fill); run(at_of(0x004d4570), R_REGS); }); break;
        case 3: case 4: check3("_NMSG_WRITE", [&] { prep(fill); args({code}); run(at_of(0x004d45b0), R_REGS); }); break;
        case 5:
            check3("__crtMessageBoxA", [&] {
                for (int k = 0; k < 2; k++) {                  // twice: the second with the pointers cached
                    prep(fill);
                    args({P(text), k ? CRT_STR_VCRT_VA : P(text + 100), typ});
                    run(at_of(0x004d6850), R_EAX | R_REGS);
                }
            });
            break;
        case 6: check3("_purecall", [&] { prep(fill); run(at_of(0x004cf350), R_REGS); }); break;
        case 7: check3("_fptrap", [&] { prep(fill); run(at_of(0x004d5c10), R_REGS); }); break;
        case 8: {
            const uint32_t sz = rnd();
            check3("_callnewh", [&] { prep(fill); args({sz}); run(at_of(0x004d64e0), R_EAX | R_REGS); });
            break;
        }
        case 9: {
            uint32_t* t = (uint32_t*)aalloc(4 * 40);
            const int m = ri(0, 40);
            for (int i = 0; i < m; i++) t[i] = chance(20) ? 0 : g_init_fn[ri(0, N_INIT - 1)];
            const uint32_t b = P(t) + 4 * ri(0, 3), e = P(t) + 4 * m;
            check3("_initterm", [&] { prep(fill); args({b, e}); run(at_of(0x004d1d30), R_REGS); });
            break;
        }
        case 10: {
            const uint32_t a = P(aalloc(32));
            check3("_matherr", [&] { prep(fill); args({a}); run(at_of(0x004d6840), R_EAX | R_REGS); });
            break;
        }
        case 11: {
            const uint32_t t = pick({0, 1, 2, 3, 4, 5, 6, 7, 8, 0xffffffff, rnd()});
            crt_errno = (int32_t)rnd();
            check3("_set_errno", [&] { prep(fill); args({t}); run(at_of(0x004d37b0), R_REGS); });
            break;
        }
        case 12:
            for (int i = 0; i < 6; i++) G32(CRT_CFLTCVT_VA + 4 * i) = 0x004d5c10;
            check3("_cfltcvt_init", [&] { prep(fill); run(at_of(0x004ce150), R_REGS); });
            break;
        case 13: check3("_fpclear", [&] { prep(fill); g_ci.preflags = typ & 7; run(at_of(0x004ce140), R_REGS | R_X87); }); break;
        case 14: check3("directsound_create_mixers", [&] { prep(fill); g_ci.preflags = typ & 7; run(at_of(0x00476750), R_REGS | R_X87); }); break;
        default:
            check3("_FF_MSGBANNER + _NMSG_WRITE", [&] {
                prep(fill);
                run(at_of(0x004d4570), R_REGS);
                prep(fill);
                args({code});
                run(at_of(0x004d45b0), R_REGS);
            });
            break;
        }
    }
}

// ---- fpinit: _fpmath -----------------------------------------------------------------------------------------------------------
static void sec_fpinit(int n) {
    g_section = "fpinit";
    g_full = false;
    for (int it = 0; it < n; it++) {
        load_world(g_base);
        arena_clear();
        g_fk.fdiv_sched = pick({0, 1, 0xffffffff, rnd(), rnd() & 0xfffffffe});
        g_fk.fdiv_n = rnd() & 31;
        g_fk.aff_proc = pick({1, 3, 0xff, 0x80000000, 0, rnd()});
        g_fk.aff_sys = g_fk.aff_proc | (chance(50) ? rnd() : 0);
        g_fk.aff_thread = rnd();
        g_fk.aff_fail = chance(10);
        g_fk.loadlib_fail = chance(10) ? 1 : 0;
        g_fk.gpa_fail = chance(20) ? rnd() & 15 : 0;
        G32(CRT_ADJUST_FDIV_VA) = rnd() & 1;
        for (int i = 0; i < 6; i++) G32(CRT_CFLTCVT_VA + 4 * i) = 0x004d5c10;
        const uint32_t cw = chance(50) ? 0x27f : rand_cw_masked();
        descf("fdiv sched %08x n %u aff %08x/%08x fail %u loadlib_fail %u gpa_fail %x cw %04x", g_fk.fdiv_sched, g_fk.fdiv_n,
              g_fk.aff_proc, g_fk.aff_sys, g_fk.aff_fail, g_fk.loadlib_fail, g_fk.gpa_fail, cw);
        const uint32_t pf = rnd() & 7;
        check3("fpmath", [&] { prep(); g_ci.cw = cw; g_ci.preflags = pf; run(at_of(0x004ce120), R_EAX | R_REGS | R_X87); });
    }
}

// ---- xcpt: xcptlookup, _XcptFilter (called directly) -----------------------------------------------------------------------------
static void sec_xcpt(int n) {
    g_section = "xcpt";
    g_full = false;
    uint32_t codes[10];
    for (int i = 0; i < 10; i++) codes[i] = G32(CRT_XCPTACTTAB_VA + 12 * i);
    for (int it = 0; it < n; it++) {
        load_world(g_base);
        arena_clear();
        for (int i = 0; i < 10; i++) {
            const uint32_t e = CRT_XCPTACTTAB_VA + 12 * i;
            if (chance(5)) G32(e) = rnd();
            if (chance(5)) G32(e + 4) = pick({8, 2, 4, 11, 22, rnd() & 31});
            G32(e + 8) = pick({0, 0, 1, 5, g_misc_fn[5], g_misc_fn[6], g_misc_fn[7 + ri(0, 7)]});
        }
        if (chance(15)) G32(CRT_XCPTACTTABCOUNT_VA) = ri(0, 10);
        if (chance(15)) {
            G32(CRT_FIRST_FPE_INDX_VA) = ri(0, 9);
            G32(CRT_NUM_FPE_VA) = chance(20) ? 0 : ri(1, 10 - (int)G32(CRT_FIRST_FPE_INDX_VA));
        }
        G32(CRT_FPECODE_VA) = rnd() & 0xff;
        G32(CRT_PXCPTINFOPTRS_VA) = chance(50) ? 0 : 0x1234560;
        g_fk.uef_ret = (int32_t)pick({0, 1, 0xffffffff, 2});
        const uint32_t code = chance(80) ? codes[ri(0, 9)] : pick({0, 0xc0000094, 0x80000003, 0xe06d7363, rnd()});
        EXCEPTION_RECORD* er = (EXCEPTION_RECORD*)aalloc(sizeof(EXCEPTION_RECORD));
        CONTEXT* cx = (CONTEXT*)aalloc(sizeof(CONTEXT), 16);
        EXCEPTION_POINTERS* ep = (EXCEPTION_POINTERS*)aalloc(sizeof(EXCEPTION_POINTERS));
        er->ExceptionCode = code;
        er->ExceptionFlags = rnd() & 1;
        er->ExceptionAddress = (void*)(uintptr_t)(0x400000 + (rnd() & 0xfffff));
        ep->ExceptionRecord = er;
        ep->ContextRecord = cx;
        descf("code %08x count %u fpe %u/%u uef %d actions %08x %08x %08x %08x", code, G32(CRT_XCPTACTTABCOUNT_VA),
              G32(CRT_FIRST_FPE_INDX_VA), G32(CRT_NUM_FPE_VA), g_fk.uef_ret, G32(CRT_XCPTACTTAB_VA + 8),
              G32(CRT_XCPTACTTAB_VA + 44), G32(CRT_XCPTACTTAB_VA + 56), G32(CRT_XCPTACTTAB_VA + 116));
        if (chance(30)) check3("xcptlookup", [&] { prep(); args({code}); run(at_of(0x004d3a30), R_EAX | R_REGS); });
        else check3("XcptFilter", [&] { prep(); args({code, P(ep)}); run(at_of(0x004d38d0), R_EAX | R_REGS); });
    }
}

// ---- fpexc: _handle_exc, _raise_exc, _87except (called directly; the fake RaiseException returns) -------------------------------
static void sec_fpexc(int n) {
    g_section = "fpexc";
    g_full = false;
    for (int it = 0; it < n; it++) {
        load_world(g_base);
        arena_clear();
        const uint32_t cwin = rand_cw_masked(), pf = chance(50) ? rnd() & 7 : 0;
        auto prep_fp = [&] { prep(); g_ci.cw = cwin; g_ci.preflags = pf; };
        switch (rnd() % 3) {
        case 0: {
            uint32_t* res = (uint32_t*)aalloc(8);
            const uint64_t d = chance(40) ? (rnd64() & 0x800fffffffffffffull) | ((uint64_t)ri(0, 0x80) << 52) : rand_double();
            memcpy(res, &d, 8);
            const uint32_t flags = chance(80) ? rnd() & 0x1f : rnd();
            const uint32_t cw = chance(70) ? rnd() & 0xffff : rnd();
            descf("_handle_exc flags %02x res %016llx cw %04x", flags, (unsigned long long)d, cw);
            check3("handle_exc", [&] { prep_fp(); args({flags, P(res), cw}); run(at_of(0x004d34f0), R_EAX | R_REGS | R_X87); });
            break;
        }
        case 1: {
            uint32_t* rec = (uint32_t*)aalloc(0x58);
            for (int i = 0; i < 0x58 / 4; i++) rec[i] = chance(50) ? 0 : rnd();
            uint32_t* pcw = (uint32_t*)aalloc(4);
            *pcw = chance(70) ? rnd() & 0xffff : rnd();
            uint32_t* a1 = (uint32_t*)aalloc(8);
            uint32_t* r = (uint32_t*)aalloc(8);
            const uint64_t x = rand_double(), y = rand_double();
            memcpy(a1, &x, 8);
            memcpy(r, &y, 8);
            // at least one cause bit (with none the original raises an uninitialised code: a known difference)
            const uint32_t flags = (1u << ri(0, 4)) | (chance(50) ? rnd() & 0x1f : 0) | (chance(10) ? rnd() & ~0x1fu : 0);
            const uint32_t op = chance(50) ? pick({0x10, 0x16, 0x1d, 1, 2, 3, 0xfff}) : rnd();
            g_fk.raise_mutate = chance(60);
            g_fk.raise_rng = rnd();
            descf("_raise_exc flags %x cw %08x opcode %x arg %016llx result %016llx mutate %d", flags, *pcw, op,
                  (unsigned long long)x, (unsigned long long)y, g_fk.raise_mutate);
            check3("raise_exc", [&] { prep_fp(); args({P(rec), P(pcw), flags, op, P(a1), P(r)}); run(at_of(0x004d3180), R_REGS | R_X87); });
            break;
        }
        default: {
            uint32_t* exc = (uint32_t*)aalloc(32);
            exc[0] = pick({1, 2, 3, 4, 5, 6, 7, 8, 0, 9, rnd()});
            exc[1] = rnd();
            const uint64_t a = rand_double(), b = rand_double(), r = chance(30) ? (rnd64() & 0x800fffffffffffffull) : rand_double();
            memcpy(exc + 2, &a, 8);
            memcpy(exc + 4, &b, 8);
            memcpy(exc + 6, &r, 8);
            uint16_t* pcw = (uint16_t*)aalloc(4);
            *pcw = (uint16_t)(chance(50) ? (rand_cw_masked() & ~(rnd() & 0x3d)) : (rnd() | 2));   // DM stays masked
            const uint32_t op = chance(60) ? pick({0x10, 0x16, 0x1d, 1, 4, 7, 0x0e}) : rnd() & 0xfff;
            G32(CRT_MATHERR_FLAG_VA) = chance(70) ? 0 : 1;
            g_fk.raise_mutate = chance(50);
            g_fk.raise_rng = rnd();
            crt_errno = (int32_t)rnd();
            const uint32_t fill = rnd();
            descf("_87except op %x type %u cw %04x retval %016llx matherr_flag %u fill %08x", op, exc[0], *pcw,
                  (unsigned long long)r, G32(CRT_MATHERR_FLAG_VA), fill);
            check3("87except", [&] { prep(fill); g_ci.cw = cwin; args({op, P(exc), P(pcw)}); run(at_of(0x004d6510), R_REGS | R_X87); });
            break;
        }
        }
    }
}

// ---- x87: _fFMOD, 87disp's result routines, __adj_fpatan, __rdtsc, the dispatchers --------------------------------------------------
static void sec_x87(int n) {
    g_section = "x87";
    g_full = false;
    static const struct { uint32_t at; const char* name; int ops; } disp[] = {
        {0x004d2e9c, "__rttosnpop", 1}, {0x004d2ea4, "__rtzeropop", 2}, {0x004d2ea6, "__rtzeronpop", 1},
        {0x004d2eb2, "__tosnan1", 1},   {0x004d2edd, "__nosnan2", 2},   {0x004d2edf, "__tosnan2", 2},
        {0x004d2f07, "__nan2", 2},      {0x004d2f46, "__rtindfpop", 2}, {0x004d2f48, "__rtindfnpop", 1},
        {0x004d2f63, "__rtchsifneg", 1}};
    static const struct { uint32_t at; const char* name; int dargs, ops; } tr[] = {
        {0x004cf05a, "acos", 1, 0},   {0x004cf050, "asin", 1, 0},    {0x004cf061, "atan", 1, 0},
        {0x004cf068, "atan2", 2, 0},  {0x004cf360, "fmod", 2, 0},    {0x004cf07c, "_CIacos", 0, 1},
        {0x004cf072, "_CIasin", 0, 1}, {0x004cf083, "_CIatan", 0, 1}, {0x004cf08a, "_CIatan2", 0, 2},
        {0x004cf36a, "_CIfmod", 0, 2}};
    for (int it = 0; it < n; it++) {
        load_world(g_base);
        arena_clear();
        const uint32_t cw = rand_cw_masked(), pf = chance(40) ? rnd() & 7 : 0;
        uint8_t st[3][10];
        for (int i = 0; i < 3; i++) rand_ext(st[i]);
        auto load_st = [&](int k) {
            g_ci.cw = cw;
            g_ci.preflags = pf;
            g_ci.nst = k;
            for (int i = 0; i < k; i++) memcpy(g_ci.st[i], st[i], 10);
        };
        const int what = (int)(rnd() % 20);
        if (what < 5) {                                             // _fFMOD
            if (chance(40)) {                                       // exponent gaps
                const uint16_t e0 = (uint16_t)(0x3fff + ri(-200, 9000) | (rnd() & 0x8000)), e1 = (uint16_t)(0x3fff + ri(-200, 200));
                memcpy(st[0] + 8, &e0, 2);
                memcpy(st[1] + 8, &e1, 2);
                st[0][7] |= 0x80;
                st[1][7] |= 0x80;
            }
            G32(CRT_ADJUST_FDIV_VA) = chance(40) ? 1 : 0;
            descf("_fFMOD st0 %s st1 %s cw %04x adjust_fdiv %u", hexs(st[0], 10).c_str(), hexs(st[1], 10).c_str(), cw,
                  G32(CRT_ADJUST_FDIV_VA));
            check3("fFMOD", [&] { prep(); load_st(2); run(at_of(0x004cf374), R_EAX | R_FLAGS | R_REGS | R_X87); });
        } else if (what < 11) {                                     // 87disp's result routines, in a dispatcher-like frame
            const int k = (int)(rnd() % 10);
            uint8_t* fr = aalloc(0x200);
            for (int i = 0; i < 0x200; i++) fr[i] = (uint8_t)rnd();
            const uint32_t ebp = P(fr) + 0x180;
            const uint32_t ecx = rnd();
            const int nst = chance(85) ? disp[k].ops : ri(0, 3);
            descf("%s st %s %s class %02x cl %02x nst %d cw %04x", disp[k].name, hexs(st[0], 10).c_str(),
                  hexs(st[1], 10).c_str(), fr[0x180 - 0x90], ecx & 0xff, nst, cw);
            check3(disp[k].name, [&] {
                prep();
                load_st(nst);
                g_ci.set_ebp = 1;
                g_ci.ebp = ebp;
                g_ci.ecx = ecx;
                run(at_of(disp[k].at), R_EAX | R_ECX | R_EDX | R_FLAGS | R_REGS | R_X87);
            });
        } else if (what < 13) {                                     // __adj_fpatan
            descf("__adj_fpatan st0 %s st1 %s cw %04x", hexs(st[0], 10).c_str(), hexs(st[1], 10).c_str(), cw);
            check3("__adj_fpatan", [&] { prep(); load_st(2); run(at_of(0x004cef65), R_EAX | R_ECX | R_EDX | R_FLAGS | R_REGS | R_X87); });
        } else if (what < 14) {                                     // __rdtsc: the counter, inside the call's window
            descf("__rdtsc");
            check3("__rdtsc", [&] {
                prep();
                const uint64_t t0 = __rdtsc();
                load_st(0);
                run(at_of(0x004188ad), R_ECX | R_REGS | R_X87);
                const uint64_t t1 = __rdtsc(), v = (uint64_t)g_co.edx << 32 | g_co.eax;
                out(v >= t0 && v <= t1, "edx:eax inside the call's window");
            });
        } else {                                                    // the dispatchers: original vs islands + rewrites
            const uint64_t x = rand_double(), y = rand_double();
            G32(CRT_MATHERR_FLAG_VA) = chance(80) ? 0 : 1;
            crt_errno = 0;
            const int k = (int)(rnd() % 10);
            descf("%s x %016llx y %016llx st0 %s st1 %s cw %04x", tr[k].name, (unsigned long long)x, (unsigned long long)y,
                  hexs(st[0], 10).c_str(), hexs(st[1], 10).c_str(), cw);
            check3(tr[k].name, [&] {
                prep();
                load_st(tr[k].ops);
                if (tr[k].dargs == 1) args({(uint32_t)x, (uint32_t)(x >> 32)});
                if (tr[k].dargs == 2) args({(uint32_t)x, (uint32_t)(x >> 32), (uint32_t)y, (uint32_t)(y >> 32)});
                run(tr[k].at, R_REGS | R_X87);
            }, 1 | 4);
        }
    }
}

// ---- islands: crt_islands_install's own behaviour ------------------------------------------------------------------------------
static long g_isl_checks, g_isl_fails;
static void expect(bool ok, const char* what) {
    g_isl_checks++;
    g_checks++;
    if (!ok) {
        g_isl_fails++;
        g_failures++;
        printf("  MISMATCH [islands] %s\n", what);
    }
}
static void island_want(int i, uint8_t* b) {
    const CrtIsland& is = g_islands[i];
    if (is.len == 5) {
        b[0] = 0xe9;
        const int32_t rel = (int32_t)(is.target - (is.at + 5));
        memcpy(b + 1, &rel, 4);
    } else {
        b[0] = 0xeb;
        b[1] = (uint8_t)(int8_t)(int32_t)(is.target - (is.at + 2));
    }
}
static int island_group(int i) {
    const uint32_t a = g_islands[i].at;
    return a >= 0x4d2e00 && a < 0x4d3000 ? 0 : a >= 0x4cef00 && a < 0x4cf000 ? 1 : 2;
}
static bool island_is(int i, bool installed) {
    uint8_t want[5];
    island_want(i, want);
    const uint8_t* p = (const uint8_t*)(uintptr_t)g_islands[i].at;
    if (installed) return !memcmp(p, want, g_islands[i].len);
    return !memcmp(p, g_island_save[i], g_islands[i].len);
}
static void sec_islands() {
    g_section = "islands";
    islands_save();
    char b[200];
    bool r = crt_islands_install();                      // from v1.0's bytes: every island written, true
    expect(r, "install from v1.0's bytes returns true");
    for (int i = 0; i < g_nisland; i++) {
        snprintf(b, sizeof b, "island %08x (%s) written", g_islands[i].at, g_islands[i].what);
        expect(island_is(i, true), b);
    }
    for (int i = 0; i < g_nisland; i++)                  // a short island lands on another island (its slot)
        if (g_islands[i].len == 2) {
            bool found = false;
            for (int j = 0; j < g_nisland; j++) found |= g_islands[j].at == g_islands[i].target;
            snprintf(b, sizeof b, "short island %08x lands on an island", g_islands[i].at);
            expect(found, b);
        }
    r = crt_islands_install();                           // idempotent
    expect(r, "a second install returns true");
    for (int i = 0; i < g_nisland; i++) expect(island_is(i, true), "unchanged by a second install");
    islands_restore();
    for (int i = 0; i < g_nisland; i++) expect(island_is(i, false), "restored");
    for (int g = 0; g < 3; g++) {                        // a foreign byte in one group: that group alone left, false
        int victim = -1;
        for (int i = 0; i < g_nisland && victim < 0; i++)
            if (island_group(i) == g) victim = i;
        uint8_t* p = (uint8_t*)(uintptr_t)g_islands[victim].at;
        const uint8_t keep = p[0];
        p[0] = 0x90;
        r = crt_islands_install();
        expect(!r, "a foreign byte: install returns false");
        for (int i = 0; i < g_nisland; i++) {
            if (island_group(i) == g) {
                snprintf(b, sizeof b, "group %d with a foreign byte: %08x left alone", g, g_islands[i].at);
                const bool left = i == victim ? p[0] == 0x90 && !memcmp(p + 1, g_island_save[i] + 1, g_islands[i].len - 1)
                                              : island_is(i, false);
                expect(left, b);
            } else {
                snprintf(b, sizeof b, "group %d foreign: the other groups' %08x written", g, g_islands[i].at);
                expect(island_is(i, true), b);
            }
        }
        p[0] = keep;
        islands_restore();
    }
    for (int g = 0; g < 3; g++) {                        // the standalone's int3 fill: written
        for (int i = 0; i < g_nisland; i++)
            if (island_group(i) == g) memset((void*)(uintptr_t)g_islands[i].at, 0xcc, g_islands[i].len);
        r = crt_islands_install();
        expect(r, "an int3-filled group: install returns true");
        for (int i = 0; i < g_nisland; i++) expect(island_is(i, true), "int3 fill: every island written");
        islands_restore();
    }
    for (int i = 0; i < g_nisland; i += 2) {             // half installed already, the rest v1.0's
        uint8_t w[5];
        island_want(i, w);
        memcpy((void*)(uintptr_t)g_islands[i].at, w, g_islands[i].len);
    }
    r = crt_islands_install();
    expect(r, "partly installed: install returns true");
    for (int i = 0; i < g_nisland; i++) expect(island_is(i, true), "partly installed: every island written");
    islands_restore();
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
    Fam& fs = g_fam["islands / crt_islands_install"];
    fs.checks = g_isl_checks;
    fs.fails[1] = g_isl_fails;
}

static long g_dispatched;
// ---- wmcs: _WinMainCRTStartup end to end -------------------------------------------------------------------------------------
static void sec_wmcs(int n) {
    g_section = "wmcs";
    g_full = true;
    for (int it = 0; it < n; it++) {
        load_world(g_pristine);
        arena_clear();
        startup_fakes();
        g_fk.heapcreate_fail = chance(4);
        g_fk.cmdline_null = chance(4);
        if (chance(5)) g_fk.envw_avail = g_fk.enva_avail = 0;
        set_xc_stubs(ri(0, 60), true);
        set_fpinit();
        rand_exit_tables();
        g_fk.winmain_action = (int32_t)(rnd() % 4);
        g_fk.winmain_ret = pick({0, 1, 0xffffffff, rnd()});
        g_fk.winmain_code = pick({0, 3, rnd()});
        g_fk.winmain_flags = rnd();
        g_fk.exit_depth = 3;                           // no exit function registers another (see exit_room)
        // exit(-1) before _cinit (no environment or no command line) reaches __endstdio before __initstdio has run, and
        // v1.0's _flsall reads __piob[0] through a null __piob: an access violation that goes to the start-up frame's
        // filter. That is exception dispatch (not run here), so where it can happen __xp holds no __endstdio.
        if ((g_fk.cmdline_null || (!g_fk.envw_avail && !g_fk.enva_avail) || g_fk.wctmb_fail_at >= 0 || g_fk.fail_alloc_at >= 0) &&
            G32(CRT_XP_A + 4) == 0x004d50a0)
            G32(CRT_XP_A + 4) = chance(50) ? 0 : g_exit_fn[ri(0, N_EXIT - 1)];
        const uint32_t fill = chance(50) ? 0 : rnd();
        descf("wmcs #%d: cmdline [%s]%s acp %u version %08x res2 %s n %d std %08x %08x %08x env w%d a%d heapfail %d "
              "alloc fails at %d action %d ret %08x fpinit %08x", it, g_fk.cmdline, g_fk.cmdline_null ? " (null)" : "",
              g_fk.acp, g_fk.version, g_fk.si_res2_null ? "null" : "set", *(int32_t*)g_fk.si_res2, g_fk.std_handle[0],
              g_fk.std_handle[1], g_fk.std_handle[2], g_fk.envw_avail, g_fk.enva_avail, g_fk.heapcreate_fail,
              g_fk.fail_alloc_at, g_fk.winmain_action, g_fk.winmain_ret, G32(CRT_FPINIT_VA));
        check3("WinMainCRTStartup", [&] { prep(fill); run(at_of(0x004cf5a0), R_REGS | R_CW); });
        // nothing here is meant to raise: an exception dispatched to the start-up frame's filter would show as
        // UnhandledExceptionFilter in the original's log
        for (size_t i = 0; i < g_pass[0].w.log.size(); i += 7)
            if (g_pass[0].w.log[i] == K_UEF) {
                if (++g_dispatched <= 5) {
                    printf("  NOTE [wmcs] an exception reached the start-up frame: %s\n", g_desc.c_str());
                    if (getenv("VP_CRTS_TRACE"))
                        for (size_t j = i > 7 * 40 ? i - 7 * 40 : 0; j <= i; j += 7) {
                            const uint32_t* e = &g_pass[0].w.log[j];
                            printf("      %-28s %08x %08x %08x %08x %08x %08x\n", kind_name(e[0]), e[1], e[2], e[3], e[4], e[5], e[6]);
                        }
                }
                break;
            }
    }
}

// ---- addresses for the static check ---------------------------------------------------------------------------------------------
static void print_addrs() {
    for (ChainReg* r = ChainReg::head(); r; r = r->next) printf("fn %08x %08x %s\n", r->at, P(r->fn), r->name);
    for (int i = 0; i < g_nisland; i++)
        if (!strstr(g_islands[i].what, "slot") && !strstr(g_islands[i].what, "hop"))
            printf("island %08x %08x %s\n", g_islands[i].at, P(rewrite_of(g_islands[i].at)), g_islands[i].what);
    printf("alias %08x %08x scope table\n", 0x004e01a0, P(crt_start::k_wmcs_scope));
    printf("alias %08x %08x wmcs filter\n", 0x004cf6e5, P(&crt_start::crt_wmcs_filter));
    printf("alias %08x %08x wmcs handler\n", 0x004cf700, P(&crt_start::crt_wmcs_handler));
    printf("alias %08x %08x global_unwind2's continuation\n", 0x004d37f4, P(&crt_start::crt_gu_return));
    printf("helper %08x wmcs_islands\n", P(&crt_start::wmcs_islands));
}

// ---- main -------------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    g_nisland = crt_islands(&g_islands);
    if (argc > 1 && !strcmp(argv[1], "addrs")) {
        print_addrs();
        return 0;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    if (!GetEnvironmentVariableA("VP_CRTS_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    const double scale = argc > 1 ? atof(argv[1]) : 1.0;
    if (argc > 2) g_rng = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    const char* only = argc > 3 ? argv[3] : 0;
    const uint32_t seed = g_rng;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* sl = strstr(exe, "\\test\\world_crt_start.cpp");
    if (!sl) { printf("can't find the repository from %s\n", __FILE__); return 2; }
    strcpy(sl, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    install_iat();
    g_heap = (uint8_t*)VirtualAlloc((void*)0x30000000, HEAP_ARENA, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_arena = (uint8_t*)VirtualAlloc((void*)0x20000000, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_code = (uint8_t*)VirtualAlloc((void*)0x2e000000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!g_heap || !g_arena || !g_code) { printf("can't place the heap / arena / stubs\n"); return 2; }
    g_orig_scope = 0x004e01a0;                                // _WinMainCRTStartup's scope table, for the WinMain stub
    g_orig_filter = G32(0x004e01a4);
    g_orig_handler = G32(0x004e01a8);
    patch_jmp(0x004d0840, (void*)&st_test_fdiv);              // __ms_p5_test_fdiv: scripted
    patch_jmp(0x00412260, (void*)&wm_entry);                  // WinMain: the stub
    g_init_common = make_common((void*)&init_c);
    g_exit_common = make_common((void*)&exit_c);
    g_misc_common = make_misc_common();
    for (int i = 0; i < N_INIT; i++) g_init_fn[i] = make_id_thunk(i, g_init_common);
    for (int i = 0; i < N_EXIT; i++) g_exit_fn[i] = make_id_thunk(0x100 + i, g_exit_common);
    for (int i = 1; i < 16; i++) g_misc_fn[i] = make_id_thunk(i, g_misc_common);
    FlushInstructionCache(GetCurrentProcess(), 0, 0);
    unsigned int cw;
    _controlfp_s(&cw, _CW_DEFAULT, 0xffffffff);
    // the image as loaded; then the start-up as the originals run it (the state most sections start from)
    fake_defaults();
    g_log.clear();
    save_world(g_pristine);
    set_xc_stubs(40, true);
    orig_call(0x004d4470, {}, "_heap_init");
    orig_call(0x004d4290, {}, "_ioinit");
    orig_call(0x004d4280, {}, "__initmbctable");
    G32(CRT_ACMDLN_VA) = P(g_fk.cmdline);
    G32(CRT_AENVPTR_VA) = orig_call(0x004d3e20, {}, "__crtGetEnvironmentStringsA");
    orig_call(0x004d3ba0, {}, "_setargv");
    orig_call(0x004d3ac0, {}, "_setenvp");
    orig_call(0x004d1c40, {}, "_cinit");
    g_log.clear();
    g_nev = 0;
    save_world(g_base);
    int nreg = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) nreg++;
    printf("world_crt_start: imports %d stubbed, %d real (KERNEL32), %d trapped; %d rewrites registered, %d islands; "
           "seed %08x scale %g; base heap %u bytes, argc %u\n", g_nstubbed, g_nreal, g_ntrapped, nreg, g_nisland, seed, scale,
           g_fk.heap_top, G32(CRT_ARGC_VA));
    auto want = [&](const char* s) { return !only || !strcmp(only, s); };
    auto N = [&](int k) { return std::max(1, (int)(k * scale)); };
    struct Sec { const char* name; void (*fn)(int); int n; };
    const Sec secs[] = {{"startup", sec_startup, 400}, {"argv", sec_argv, 20000},   {"env", sec_env, 4000},
                        {"exit", sec_exit, 1500},      {"msg", sec_msg, 6000},      {"fpinit", sec_fpinit, 2000},
                        {"xcpt", sec_xcpt, 6000},      {"fpexc", sec_fpexc, 30000}, {"x87", sec_x87, 40000},
                        {"wmcs", sec_wmcs, 1500}};
    for (const Sec& s : secs)
        if (want(s.name)) {
            const DWORD t0 = GetTickCount();
            const long f0 = g_failures, c0 = g_checks;
            s.fn(N(s.n));
            printf("%-8s %7ld checks, %ld mismatches (%.1f s)\n", s.name, g_checks - c0, g_failures - f0,
                   (GetTickCount() - t0) / 1000.0);
        }
    if (want("islands")) {
        const long f0 = g_failures, c0 = g_checks;
        sec_islands();
        printf("%-8s %7ld checks, %ld mismatches\n", "islands", g_checks - c0, g_failures - f0);
    }
    printf("\nfamilies: checks, mismatches isolated / chain\n");
    for (auto& f : g_fam) printf("  %-44s %7ld  %ld / %ld\n", f.first.c_str(), f.second.checks, f.second.fails[1], f.second.fails[2]);
    if (!g_chain_count.empty()) {
        printf("calls into each rewrite in the chain passes:\n");
        int k = 0;
        for (ChainReg* r = ChainReg::head(); r; r = r->next)
            printf("  %-26s %9u%s", r->name, *g_chain_count[r->at], ++k % 3 ? "" : "\n");
        printf("\n");
    }
    printf("%ld checks, %ld mismatches; %ld checks where an exception reached the start-up frame\n", g_checks, g_failures,
           g_dispatched);
    return g_failures || g_dispatched ? 1 : 0;
}
