// standalone.cpp -- the port DLL's side of viperport.exe (standalone.h): the int3 fill, its audit, the handler that
// names any original code reached, and the start of the game through its rewritten C runtime startup.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <intrin.h>
#include "SDL.h"
#include "viperport.h"
#include "standalone.h"
#include "crt_types.h"                                  // crt_islands (crt_start.cpp): code too short to hook

// ---- the flag ----------------------------------------------------------------------------------------------------
// Read when the DLL's statics are built (before DllMain's install asks), then taken out of the environment so the
// game's own C runtime (its environ) and anything it starts never see it.
static bool read_flag() {
    char v[16];
    const DWORD n = GetEnvironmentVariableA(VP_STANDALONE_ENV, v, sizeof v);
    if (!n || n >= sizeof v) return false;
    SetEnvironmentVariableA(VP_STANDALONE_ENV, 0);
    return atoi(v) == VP_STANDALONE_VERSION;
}
static const bool g_standalone = read_flag();
bool vp_standalone() { return g_standalone; }

// ---- the tables (tools/gen_standalone.py, tools/gen_port_tables.py) ------------------------------------------------
namespace {
struct SaFunc { uint32_t va, size, first, spans; const char* name; };
struct SaSpan { uint32_t start, end; };
struct SaKeep { uint32_t start, bytes; const char* why; };
struct SaThunk { uint32_t va, slot; };
struct SaNamed { uint32_t va; const char* where; };
struct Prologue { uint32_t v10; uint8_t len; int8_t rel; uint8_t bytes[16]; };

const SaFunc k_funcs[] = {
#define VP_SA_FUNCS
#include "standalone.inc"
#undef VP_SA_FUNCS
};
const SaSpan k_spans[] = {
#define VP_SA_SPANS
#include "standalone.inc"
#undef VP_SA_SPANS
};
const SaKeep k_keep[] = {
#define VP_SA_KEEP
#include "standalone.inc"
#undef VP_SA_KEEP
};
const SaThunk k_thunks[] = {
#define VP_SA_THUNKS
#include "standalone.inc"
#undef VP_SA_THUNKS
};
const SaNamed k_named[] = {
#define VP_SA_NAMED
#include "standalone.inc"
#undef VP_SA_NAMED
};
const Prologue k_hooked[] = {                           // every address a PORT_FN or detour hooks
#include "prologues.inc"
};
const int N_FUNCS = sizeof k_funcs / sizeof k_funcs[0];

const uint32_t TEXT_LO = 0x401000, TEXT_HI = 0x401000 + 0xd948d;   // v1.0's .text
const uint32_t IAT_LO = 0x5d7410, IAT_HI = 0x5d7410 + 0x320;
const uint32_t ENTRY = 0x4cf5a0;                        // _WinMainCRTStartup: AddressOfEntryPoint

uint32_t g_dll_lo, g_dll_hi;                            // this DLL's image

// the inventory function holding va (or -1)
int func_of(uint32_t va) {
    int lo = 0, hi = N_FUNCS - 1, at = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (k_funcs[mid].va <= va) { at = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return at >= 0 && va < k_funcs[at].va + k_funcs[at].size ? at : -1;
}

// is there a hook jmp (E9 into this DLL) at va?
bool hooked_at(uint32_t va) {
    const uint8_t* p = (const uint8_t*)(uintptr_t)va;
    if (p[0] != 0xE9) return false;
    int32_t rel;
    memcpy(&rel, p + 1, 4);
    const uint32_t to = va + 5 + (uint32_t)rel;
    return to >= g_dll_lo && to < g_dll_hi;
}

// an address, for a person: race.exe's function and offset, the DLL's offset (hook\build\dinput.map names it), or
// another module's
void describe(uint32_t a, char* out, size_t n) {
    if (a >= TEXT_LO && a < TEXT_HI) {
        const int f = func_of(a);
        if (f >= 0 && a == k_funcs[f].va) _snprintf(out, n, "%08x %s", a, k_funcs[f].name);
        else if (f >= 0) _snprintf(out, n, "%08x %s+0x%x", a, k_funcs[f].name, a - k_funcs[f].va);
        else _snprintf(out, n, "%08x (race.exe, outside every function)", a);
    } else if (a >= g_dll_lo && a < g_dll_hi) {
        _snprintf(out, n, "dinput.dll+%05x", a - g_dll_lo);
    } else {
        HMODULE m = 0;
        char path[MAX_PATH] = "";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)a, &m) && GetModuleFileNameA(m, path, MAX_PATH)) {
            const char* s = strrchr(path, '\\');
            _snprintf(out, n, "%s+%x", s ? s + 1 : path, a - (uint32_t)(uintptr_t)m);
        } else {
            _snprintf(out, n, "%08x", a);
        }
    }
    out[n - 1] = 0;
}

bool is_code_addr(uint32_t v) { return (v >= TEXT_LO && v < TEXT_HI) || (v >= g_dll_lo + 0x1000 && v < g_dll_hi); }

// ---- the report --------------------------------------------------------------------------------------------------
void(__cdecl* g_out)(const char*);
int g_failures;
void say(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    logf("standalone: %s", line);
    if (g_out) g_out(line);
}
void fail(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    line[sizeof line - 1] = 0;
    g_failures++;
    say("FAIL: %s", line);
}

// ---- the handler -------------------------------------------------------------------------------------------------
// An int3 in race.exe's .text is original code reached: name it, the caller (the return address on top of the stack:
// a filled function is entered by a call, so that's where it was called from), and the code addresses further up the
// stack, then end the game -- ExitProcess, so the DLL's exit log is written. Other threads that reach original code
// meanwhile wait for the end. In the --check self-test it is caught and returned from instead.
volatile LONG g_reached;
volatile LONG g_selftest;                               // 1: the self-test's call is in flight
char g_selftest_line[512];

LONG CALLBACK on_int3(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t at = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    if (at < TEXT_LO || at >= TEXT_HI) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    const uint32_t* sp = (const uint32_t*)(uintptr_t)c->Esp;
    char where[160], from[160] = "?", stack[16 * 48] = "";
    describe(at, where, sizeof where);
    if (!IsBadReadPtr(sp, 4)) describe(sp[0], from, sizeof from);
    int n = 0;
    for (int i = 1; i < 512 && n < 10 && !IsBadReadPtr(sp + i, 4); i++) {
        if (!is_code_addr(sp[i])) continue;
        char one[160];
        describe(sp[i], one, sizeof one);
        if (strlen(stack) + strlen(one) + 4 >= sizeof stack) break;
        lstrcatA(stack, n ? " <- " : "");
        lstrcatA(stack, one);
        n++;
    }
    if (g_selftest == 1) {
        _snprintf(g_selftest_line, sizeof g_selftest_line, "original code reached at %s, called from %s", where, from);
        g_selftest_line[sizeof g_selftest_line - 1] = 0;
        g_selftest = 2;
        c->Eip = sp[0];                                 // return to the caller, as the function would have
        c->Esp += 4;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (InterlockedIncrement(&g_reached) != 1) {
        logf("standalone: original code reached at %s too (thread %lu), called from %s", where, GetCurrentThreadId(), from);
        Sleep(INFINITE);
    }
    logf("standalone: ORIGINAL CODE REACHED at %s, called from %s (thread %lu)", where, from, GetCurrentThreadId());
    logf("standalone:   the stack's code addresses: %s", *stack ? stack : "(none)");
    logf("standalone:   (an int3 the standalone put in place of v1.0's code: a function the port doesn't replace yet, or "
         "one the install left original). The game ends here.");
    ExitProcess(0xC0DE0003);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---- SEH through the mapping ---------------------------------------------------------------------------------------
// The game's own frames name handlers inside race.exe's mapping (_WinMainCRTStartup's frame: _except_handler3, 0x4d4490,
// now a jmp to its rewrite). Windows dispatches to a handler only if it trusts its address (SafeSEH / DEP rules for
// memory that isn't a loaded image), so --check raises an exception through a frame whose handler is in the mapping:
// a jmp placed in a dead, filled function, on to seh_probe_handler. If Windows refused the handler the exception
// would be unhandled and the process would end here -- the report's last line says so.
const DWORD SEH_PROBE_CODE = 0xE0565001;
volatile LONG g_seh_probe;
EXCEPTION_DISPOSITION __cdecl seh_probe_handler(EXCEPTION_RECORD* r, void*, CONTEXT*, void*) {
    if (r->ExceptionCode != SEH_PROBE_CODE) return ExceptionContinueSearch;
    g_seh_probe = 1;
    return ExceptionContinueExecution;
}
struct SehReg { SehReg* next; void* handler; };
#pragma warning(push)
#pragma warning(disable : 4733)   // fs:[0] set by hand: the handler is the test, deliberately not a SafeSEH one
__declspec(noinline) void seh_probe_raise(uint32_t handler) {
    SehReg reg;
    reg.handler = (void*)(uintptr_t)handler;
    reg.next = (SehReg*)(uintptr_t)__readfsdword(0);
    __writefsdword(0, (DWORD)(uintptr_t)&reg);
    RaiseException(SEH_PROBE_CODE, 0, 0, 0);
    __writefsdword(0, (DWORD)(uintptr_t)reg.next);
}
#pragma warning(pop)

// ---- the fill ----------------------------------------------------------------------------------------------------
struct Range { uint32_t a, b; };

bool in_ranges(const std::vector<Range>& r, uint32_t x) {
    for (const Range& k : r)
        if (x >= k.a && x < k.b) return true;
    return false;
}

int run(const VpStandaloneArgs* args) {
    g_out = args->out;
    const bool check = args->mode == VP_SA_CHECK;
    say("%s, race.exe %s (%s)", check ? "--check" : "starting", args->exe_path,
        args->stock ? "the stock v1.0 file" : "v1.0, but not the stock file: vrmod's patches?");
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery((void*)&run, &mbi, sizeof mbi);
    g_dll_lo = (uint32_t)(uintptr_t)mbi.AllocationBase;
    const IMAGE_NT_HEADERS* dnt = (const IMAGE_NT_HEADERS*)(g_dll_lo + ((const IMAGE_DOS_HEADER*)(uintptr_t)g_dll_lo)->e_lfanew);
    g_dll_hi = g_dll_lo + dnt->OptionalHeader.SizeOfImage;
    if (!g_standalone) fail("the DLL wasn't loaded with %s set: it installed as on the dinput.dll route", VP_STANDALONE_ENV);
    if (!build_is_v10()) {
        fail("the DLL didn't install on v1.0 race.exe (viperport.log says why); nothing filled");
        return g_failures;
    }
    // the game's own GetModuleHandleA(NULL), through its import slot, is race.exe's image (the loader's shim); the
    // process's (the PEB's: SDL's, DirectInput's) is viperport.exe, a module Windows loaded
    if (((HMODULE(WINAPI*)(LPCSTR)) * (void* volatile*)(uintptr_t)0x005d75b4)(0) != (HMODULE)0x400000)
        fail("the game's GetModuleHandleA(NULL) isn't race.exe's image at 0x400000");

    // 1. what the install hooked: every inventory entry and every PORT_FN/detour address, live
    std::vector<uint8_t> hooked(N_FUNCS);
    std::vector<Range> keep;                            // never filled: hook jmps, run-time reads
    int nhooked = 0, inner = 0;
    for (int i = 0; i < N_FUNCS; i++)
        if (hooked_at(k_funcs[i].va)) { hooked[i] = 1; nhooked++; keep.push_back({k_funcs[i].va, k_funcs[i].va + 5}); }
    int not_in_force = 0;
    for (const Prologue& p : k_hooked) {
        const int f = func_of(p.v10);
        if (hooked_at(p.v10)) {
            if (f < 0 || k_funcs[f].va != p.v10) { keep.push_back({p.v10, p.v10 + 5}); inner++; }
            continue;
        }
        char d[160];
        describe(p.v10, d, sizeof d);
        if (++not_in_force <= 40) fail("%s has a rewrite, but the install left it original (viperport.log says why: a vrmod-patched function?)", d);
    }
    if (not_in_force > 40) fail("... and %d more rewrites not in force", not_in_force - 40);
    if (!hooked_at(ENTRY)) fail("the entry point 004cf5a0 (_WinMainCRTStartup) isn't hooked: the game can't start on the port's C runtime");
    for (const SaKeep& k : k_keep) keep.push_back({k.start, k.start + k.bytes});
    say("hooked: %d of %d inventory functions at their entries, %d more entries inside functions", nhooked, N_FUNCS, inner);

    // 2. the audit of the install: every byte of .text it changed is a hook's jmp or a kept operand (M1's), or lies in
    //    code the fill overwrites (call sites the dinput.dll route redirects inside original functions)
    if (args->file && args->file_size >= 0x400 + (TEXT_HI - TEXT_LO)) {
        const uint8_t* before = args->file + 0x400;   // .text's raw data starts at file offset 0x400 (v1.0)
        int runs = 0, in_hooks = 0, in_keep = 0, in_code = 0, other = 0;
        for (uint32_t a = TEXT_LO; a < TEXT_HI;) {
            if (*(const uint8_t*)(uintptr_t)a == before[a - TEXT_LO]) { a++; continue; }
            uint32_t b = a;
            while (b < TEXT_HI && *(const uint8_t*)(uintptr_t)b != before[b - TEXT_LO]) b++;
            runs++;
            bool all_hook = true, all_keep = true;
            for (uint32_t x = a; x < b; x++) {
                bool h = false;
                for (const Range& r : keep)
                    if (x >= r.a && x < r.b && r.b - r.a == 5 && hooked_at(r.a)) { h = true; break; }
                all_hook = all_hook && h;
                all_keep = all_keep && in_ranges(keep, x);
            }
            const int f = func_of(a);
            if (all_hook) in_hooks++;
            else if (all_keep) in_keep++;
            else if (f >= 0) in_code++;
            else {
                other++;
                char d[160];
                describe(a, d, sizeof d);
                fail("the install changed %u bytes at %s, outside every function", b - a, d);
            }
            a = b;
        }
        say("the install changed .text in %d places: %d hook jmps, %d kept operands (M1), %d inside code the fill "
            "overwrites (call sites redirected on the dinput.dll route), %d elsewhere", runs, in_hooks, in_keep, in_code, other);
    }

    // 3. the fill
    std::vector<uint8_t> kept_before;
    for (const SaKeep& k : k_keep)
        kept_before.insert(kept_before.end(), (const uint8_t*)(uintptr_t)k.start, (const uint8_t*)(uintptr_t)k.start + k.bytes);
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)TEXT_LO, TEXT_HI - TEXT_LO, PAGE_EXECUTE_READWRITE, &old)) {
        fail("can't make race.exe's .text writable (%lu)", GetLastError());
        return g_failures;
    }
    // keep, sorted, for a quick test per byte
    std::vector<uint8_t> keep_map(TEXT_HI - TEXT_LO);
    for (const Range& r : keep)
        for (uint32_t x = r.a; x < r.b; x++)
            if (x >= TEXT_LO && x < TEXT_HI) keep_map[x - TEXT_LO] = 1;
    std::vector<uint8_t> is_thunk(N_FUNCS);
    for (const SaThunk& t : k_thunks) {
        const int f = func_of(t.va);
        if (f >= 0 && !hooked[f]) is_thunk[f] = 1;   // (Win32GetTime is a bare jmp [timeGetTime] too, and rewritten)
    }
    uint32_t filled = 0, nfilled = 0, nthunks = 0;
    for (int i = 0; i < N_FUNCS; i++) {
        if (is_thunk[i]) { nthunks++; continue; }
        const SaFunc& f = k_funcs[i];
        for (uint32_t s = f.first; s < f.first + f.spans; s++)
            for (uint32_t x = k_spans[s].start; x < k_spans[s].end; x++)
                if (!keep_map[x - TEXT_LO]) { *(uint8_t*)(uintptr_t)x = 0xCC; filled++; }
        if (!hooked[i]) nfilled++;
    }
    VirtualProtect((void*)(uintptr_t)TEXT_LO, TEXT_HI - TEXT_LO, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)TEXT_LO, TEXT_HI - TEXT_LO);
    say("filled %u bytes of v1.0 code with int3: %d hooked functions after their jmp, %d unhooked ones whole; "
        "%u import thunks left (jmp [IAT])", filled, nhooked, nfilled, nthunks);

    // 4. verify: every function hooked, whole int3, or a thunk into the IAT; kept bytes intact
    int bad = 0;
    for (int i = 0; i < N_FUNCS; i++) {
        const SaFunc& f = k_funcs[i];
        const uint8_t* p = (const uint8_t*)(uintptr_t)f.va;
        char d[160];
        if (is_thunk[i]) {
            uint32_t slot;
            memcpy(&slot, p + 2, 4);
            if (!(p[0] == 0xFF && p[1] == 0x25 && slot >= IAT_LO && slot < IAT_HI && *(const uint32_t*)(uintptr_t)slot)) {
                describe(f.va, d, sizeof d);
                if (++bad <= 20) fail("%s: an import thunk that isn't jmp [a resolved IAT slot]", d);
            }
            continue;
        }
        if (hooked[i]) continue;
        if (p[0] != 0xCC && !keep_map[f.va - TEXT_LO]) {
            describe(f.va, d, sizeof d);
            if (++bad <= 20) fail("%s is neither hooked nor int3 at its entry", d);
        }
        if (check) {
            for (uint32_t s = f.first; s < f.first + f.spans; s++)
                for (uint32_t x = k_spans[s].start; x < k_spans[s].end; x++)
                    if (*(const uint8_t*)(uintptr_t)x != 0xCC && !keep_map[x - TEXT_LO]) {
                        describe(x, d, sizeof d);
                        if (++bad <= 20) fail("%s: code left after the fill", d);
                        s = f.first + f.spans;
                        break;
                    }
        }
    }
    {
        size_t at = 0;
        for (const SaKeep& k : k_keep) {
            if (memcmp((const void*)(uintptr_t)k.start, &kept_before[at], k.bytes)) {
                char d[160];
                describe(k.start, d, sizeof d);
                fail("%s: a byte the rewrites read (%s) changed in the fill", d, k.why);
            }
            at += k.bytes;
        }
    }
    say("every inventory function is hooked (%d), int3 (%d) or an import thunk (%u)%s", nhooked, nfilled, nthunks,
        bad ? " -- NOT all: see above" : "");

    // 4b. the islands (crt_start.cpp): 87disp's result routines, __adj_fpatan and __rdtsc are too short or too packed for a
    //     hook's 5 bytes; _WinMainCRTStartup's rewrite redirects them first thing, over the fill. Done here too (it's
    //     idempotent), so they are in place and checked before anything runs.
    {
        const bool ok = crt_islands_install();
        const CrtIsland* is = 0;
        const int n = crt_islands(&is);
        int good = 0;
        for (int i = 0; i < n; i++) {
            const uint8_t* p = (const uint8_t*)(uintptr_t)is[i].at;
            bool right;
            if (is[i].len == 5) right = hooked_at(is[i].at);
            else right = p[0] == 0xEB && is[i].at + 2 + (uint32_t)(int32_t)(int8_t)p[1] == is[i].target;
            if (right) good++;
            else fail("%08x %s isn't redirected to its rewrite", is[i].at, is[i].what);
        }
        if (!ok) fail("crt_islands_install refused a group (viperport.log says which)");
        say("islands: %d of %d short entries redirected (87disp's result routines, __adj_fpatan, __rdtsc)", good, n);
        // every unhooked entry the port's code calls or names (standalone.inc) is hooked now, or an island
        for (const SaNamed& nm : k_named) {
            bool island = false;
            for (int i = 0; i < n; i++)
                if (is[i].at == nm.va) island = true;
            if (hooked_at(nm.va) || island) continue;
            char d[160];
            describe(nm.va, d, sizeof d);
            fail("%s is called by the port (%s) but isn't hooked: it would be int3", d, nm.where);
        }
    }

    // 5. the handler
    AddVectoredExceptionHandler(1, on_int3);
    if (check) {
        // the self-test: call a function the fill made int3 (DSoundMixer's dump_devcaps, dead with SDL audio) and see it
        // caught and named
        typedef void(__cdecl * Dump_t)(void*);
        const uint32_t probe = 0x00475cc0;
        if (*(const uint8_t*)(uintptr_t)probe == 0xCC) {
            g_selftest = 1;
            ((Dump_t)(uintptr_t)probe)(0);
            if (g_selftest == 2) say("the handler: \"%s\"", g_selftest_line);
            else fail("the handler didn't catch an int3 at %08x", probe);
            g_selftest = 0;
        } else {
            fail("the self-test's function %08x wasn't filled", probe);
        }
        // SDL as sdl_create_window (platform.cpp) starts it: the app registered with the game's hInstance (0x400000),
        // then video, events, joysticks and haptics -- the joystick and haptic drivers initialise DirectInput 8 with
        // GetModuleHandle(NULL), which must be a module Windows knows. No window is opened.
        {
            SDL_SetMainReady();
            SDL_RegisterApp((char*)"viperport check", CS_HREDRAW | CS_VREDRAW, (void*)(uintptr_t)0x400000);
            const Uint32 sub = SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC;
            if (SDL_InitSubSystem(sub) != 0) {
                fail("SDL doesn't start as the game starts it: %s (GetModuleHandle(NULL) = %p)", SDL_GetError(),
                     (void*)GetModuleHandleA(0));
            } else {
                say("SDL starts as the game starts it (video, events, joysticks, haptics: %d joysticks)", SDL_NumJoysticks());
                SDL_QuitSubSystem(sub);
            }
            SDL_UnregisterApp();
        }
        // SEH: a handler in the mapping (see seh_probe_raise)
        {
            uint8_t* slot = (uint8_t*)(uintptr_t)probe;
            DWORD o2;
            VirtualProtect(slot, 5, PAGE_EXECUTE_READWRITE, &o2);
            slot[0] = 0xE9;
            const int32_t rel = (int32_t)((uintptr_t)&seh_probe_handler - ((uintptr_t)slot + 5));
            memcpy(slot + 1, &rel, 4);
            FlushInstructionCache(GetCurrentProcess(), slot, 5);
            DWORD dep = 0;
            BOOL perm = FALSE;
            GetProcessDEPPolicy(GetCurrentProcess(), &dep, &perm);
            say("SEH: raising an exception through a frame whose handler is %08x, in race.exe's mapping (DEP %s)...",
                probe, dep & PROCESS_DEP_ENABLE ? "on" : "off");
            seh_probe_raise(probe);
            memset(slot, 0xCC, 5);
            VirtualProtect(slot, 5, o2, &o2);
            FlushInstructionCache(GetCurrentProcess(), slot, 5);
            if (g_seh_probe) say("SEH: dispatched to the handler in the mapping and continued");
            else fail("SEH: the handler in the mapping wasn't called");
        }
        say(g_failures ? "%d checks FAILED" : "all checks passed: no original v1.0 code can run unnoticed", g_failures);
        return g_failures;
    }

    // 6. the game: race.exe's entry point, now the rewritten _WinMainCRTStartup
    if (g_failures) {
        say("NOT starting the game: %d checks failed (the first few are above)", g_failures);
        return g_failures;
    }
    say("starting the game at 004cf5a0 (_WinMainCRTStartup, rewritten) with the command line \"%s\"",
        ((LPSTR(WINAPI*)()) * (void* volatile*)(uintptr_t)0x005d75ac)());   // the game's GetCommandLineA slot
    g_out = 0;
    ((void(__cdecl*)())(uintptr_t)ENTRY)();
    logf("standalone: _WinMainCRTStartup returned (it ends the process itself in v1.0)");
    ExitProcess(0);
}
}  // namespace

extern "C" __declspec(dllexport) int __cdecl viperport_standalone(const VpStandaloneArgs* args) {
    if (!args || args->size < sizeof(VpStandaloneArgs) || args->version != VP_STANDALONE_VERSION) {
        logf("standalone: viperport.exe and dinput.dll are from different builds (interface version)");
        if (args && args->size >= 32 && args->out) args->out("viperport.exe and dinput.dll are from different builds");
        return 1;
    }
    return run(args);
}
