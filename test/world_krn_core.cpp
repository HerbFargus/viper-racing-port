// world_krn_core.cpp -- the kernel's core (hook/krn_core.cpp: kernel, mem, task, sync, bg, except, prof, _prof, time,
// vxd, abend) against the originals, outside the game (docs/PORTING.md, step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_krn_core.cpp
//        /Fo%TEMP%\k1\ /Fe%TEMP%\k1\world_krn_core.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_krn_core.exe [worlds] [seed]
//   the fixes (docs/PORTING.md, "Fixes"): the same with /DFIX_TESTS -- the rewrites as the game has them, on the same
//   worlds (ExceptBegin and unsafe_check, the fixed functions, are left out of them and of the chain, and so are
//   _SingleEnter / _SingleLeave, which call unsafe_check), then ExceptBegin's crash log in <race.exe's folder>\log\
//   (fix_except_begin; CreateDirectoryA is a stub that makes nothing) and unsafe_check's owner check that no longer
//   panics (fix_unsafe_check).
//
// Loads out\race_v10.exe at 0x400000 in a child process (as test/fuzz.cpp) and resolves its KERNEL32 / USER32 / WINMM
// imports; then every import slot the kernel uses holds a logging stub instead -- the heap (a bump heap in the arena),
// threads (CreateThread logs and hands back a scripted id, never runs anything), critical sections (emulated on the
// struct, no real lock), QueryPerformanceCounter / GetLocalTime (a fake clock), GetSystemInfo (a scripted processor
// count), files and mappings (race_v10.exe's own bytes as the view, so the real appended link map is searched),
// SetUnhandledExceptionFilter, ClipCursor, ExitProcess / ExitThread (log, then end the pass), DeviceIoControl (a fake
// vexed.vxd). The rewrite must make the same Windows calls through the same slots, in the same order, with the same
// arguments. The game functions outside the group (LogReport / LogError / LogPanic -- which as in the game may not
// return --, Win32GetTime, Win32GetErrorString, Win32GetAppInstance, File*, Log/Win32/Key/Scan/Mouse/Joy Begin/End,
// atexit) are stubs too; the game's CRT (sprintf, vsprintf, sscanf, strstr, stricmp, strnicmp, strncpy, qsort,
// memmove, _controlfp, _clearfp) runs from the image. The TSC: every rdtsc instruction of the original's _prof.obj is
// turned into ud2 and a vectored handler returns the fake clock; the rewrite gets the same clock through KRN_RDTSC.
//
// Each world randomises the kernel's statics (task table, Single/Multi tables, the critical-section pool, the profiler,
// heap counters, BG hooks, exit handlers, the map pointer, the VxD handle) within ranges whose every pointer lands in
// the arena or the image, then checks each function: the original runs, the state is restored, the rewrite runs, and
// the return value, the statics, the arena, the FPU control word afterwards, how the pass ended (returned, panicked,
// exited, faulted, hung) and the stubs' call logs are compared; every byte the original changed must lie in the
// rewrite's footprint unless it's replay_only. Each check runs "isolated" (the group's callees are the originals) and
// "chain" (every rewrite of the group hooked into the image for the rewrite's pass, rdtsc's through a short jump into
// the padding at 0x418912). The timer loop runs to completion with a fake clock and a callback that sets the task's
// kill flag after a scripted number of ticks. Then a "live" pass: real threads, real locks, the real clock and heap --
// KernelBegin, a BG hook for half a second, MemAlloc/MemFree, KernelEnd -- once with the originals and once with the
// whole group rewritten, compared by what can be compared across two runs (tables, counters, the hook's FPU control
// word and thread priority, how often it ran). Nothing opens a dialog: SetErrorMode, the CRT's fatal paths are stubs.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <tuple>
#include <type_traits>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif
#ifndef FIX_TESTS
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#endif
#include "../hook/port.h"

struct ChainReg {
    uint32_t at; void* fn; const char* name; ChainReg* next;
    static ChainReg*& head() { static ChainReg* h; return h; }
    ChainReg(uint32_t a, void* f, const char* n) : at(a), fn(f), name(n), next(head()) { head() = this; }
};
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                        \
    static const char* const VP_CAT(name_, NEW) = NAME;                                                   \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;                                                        \
    static ChainReg VP_CAT(chain_, NEW)(V10, (void*)&NEW, NAME);

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
uint32_t A(uint32_t v10) { return v10; }
bool have(uint32_t) { return true; }
bool build_is_v10() { return true; }
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 0x40, what); }

static uint64_t __cdecl fake_rdtsc();
#define KRN_RDTSC() fake_rdtsc()
#ifndef KRN_CORE_FILE
#define KRN_CORE_FILE "../hook/krn_core.cpp"
#endif
#include KRN_CORE_FILE

// ---- random values ------------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x6c8e9cf5u;
static uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static uint32_t hash_bytes(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static uint32_t hash_str(const char* s) { return s ? hash_bytes(s, strlen(s)) : 0xdead; }

// ---- the original, loaded at 0x400000 ----------------------------------------------------------------------------------
static uint8_t* g_file;                 // race_v10.exe's bytes (the "view" MapViewOfFile returns)
static uint32_t g_file_size, g_sections_end;
struct Slot { const char* dll; const char* name; uint32_t at; void* real; };
static std::vector<Slot> g_slots;
static bool load_race_exe(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("can't open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = (uint8_t*)malloc(n);
    fread(g_file, 1, n, f);
    fclose(f);
    g_file_size = (uint32_t)n;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_file + ((IMAGE_DOS_HEADER*)g_file)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x362de68c) { printf("%s isn't the v1.0 race.exe\n", path); return false; }
    uint8_t* base = (uint8_t*)VirtualAlloc((void*)0x400000, nt->OptionalHeader.SizeOfImage, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (base != (uint8_t*)0x400000) { printf("0x400000 isn't reserved for race.exe in this process\n"); return false; }
    memcpy(base, g_file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        memcpy(base + sec[i].VirtualAddress, g_file + sec[i].PointerToRawData,
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize);
        uint32_t e = sec[i].PointerToRawData + sec[i].SizeOfRawData;
        if (e > g_sections_end) g_sections_end = e;
    }
    // every import slot: KERNEL32 / USER32 / WINMM resolved (the game's CRT reaches KERNEL32), the rest left alone
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.Size && d->Name; d++) {
        const char* dll = (const char*)(base + d->Name);
        HMODULE m = 0;
        if (!_stricmp(dll, "KERNEL32.dll") || !_stricmp(dll, "USER32.dll") || !_stricmp(dll, "WINMM.dll")) m = LoadLibraryA(dll);
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            FARPROC p = m ? GetProcAddress(m, fn) : 0;
            if (p) iat->u1.Function = (DWORD)(uintptr_t)p;
            g_slots.push_back({dll, fn, (uint32_t)(uintptr_t)&iat->u1.Function, (void*)p});
        }
    }
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_WORLD_CHILD", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) return 2;
    if (!VirtualAllocEx(pi.hProcess, (void*)0x400000, 0x300000, MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        printf("couldn't reserve 0x400000 in the test process (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    if (code > 4) printf("the test process ended with %08lx\n", code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}
static void patch_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// the chain: every rewrite of the group hooked into the image (for a rewrite's pass); rdtsc (3 bytes, then
// isCpuidSupported) through a short jump into the int3 padding at 0x418912
struct ChainSave { uint32_t at; uint8_t b[5]; };
static ChainSave g_chain_save[256];
static int g_nchain;
static void chain_save(uint32_t at) {
    ChainSave& s = g_chain_save[g_nchain++];
    s.at = at;
    memcpy(s.b, (void*)(uintptr_t)at, 5);
}
static void chain_patch() {
    g_nchain = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) {
#ifdef FIX_TESTS
        if (r->at == 0x004154a0) continue;             // (the fixed ExceptBegin: checked on its own, fix_except_begin)
        if (r->at == 0x00415020) continue;             // (the fixed unsafe_check: checked on its own, fix_unsafe_check)
#endif
        if (r->at == 0x004188ad) {
            chain_save(0x004188ad);
            chain_save(0x00418912);
            patch_jmp(0x00418912, r->fn);
            ((uint8_t*)0x004188ad)[0] = 0xeb;
            ((uint8_t*)0x004188ad)[1] = (uint8_t)(0x418912 - 0x4188af);
            continue;
        }
        chain_save(r->at);
        patch_jmp(r->at, r->fn);
    }
}
static void chain_unpatch() {
    for (int i = g_nchain - 1; i >= 0; i--) memcpy((void*)(uintptr_t)g_chain_save[i].at, g_chain_save[i].b, 5);
    g_nchain = 0;
}

// ---- the arena (64 KB aligned: the fake stack my_handler walks ends on a 64 KB boundary) -------------------------------
enum {
    ARENA_SIZE = 0x40000,
    OFF_CTRL = 0x0000, OFF_STRS = 0x1000, OFF_FAKEPE = 0x2000, OFF_STACK = 0x8000, OFF_STACK_TOP = 0x10000,
    OFF_MISC = 0x10000, OFF_MAP = 0x14000, MAP_CAP = 0xc000, OFF_HEAP = 0x20000, HEAP_SIZE = 0x20000,
};
enum { M_LI_A = 0x000, M_LI_B = 0x010, M_ORDER = 0x020 /* int[40] */, M_TOD = 0x100, M_EP = 0x120, M_REC = 0x140,
       M_CTX = 0x200 /* CONTEXT, 0x2cc */, M_LAME = 0x500, M_HANDLE = 0x510, M_STRBUF = 0x600 /* 0x200 */,
       M_GETSTR = 0x800 /* 0x400 */, M_NAMEMAP = 0xc00 /* a name_int_map, 0x104 */, M_CSPTR = 0xd10, M_ERR = 0xd20 };
static uint8_t* g_arena;
struct Ctrl {
    uint32_t heap_top;
    uint64_t qpc, tsc;             // the fake clocks
    uint32_t qpf_lo, qpf_hi, qpf_fail_pct;
    uint32_t ncpu;
    uint32_t tid;                  // GetCurrentThreadId
    uint32_t new_tid, new_handle;  // CreateThread
    uint32_t sleeps, sleep_flag_at;
    uint32_t cb_calls, cb_limit, cb_task, cb_step;   // the timer callback
    uint32_t panic_returns;        // LogPanic returns (else it ends the pass, as in the game)
    uint32_t prev_filter;
    uint32_t file_size;
    uint32_t w32time;
    uint32_t app_instance;
    uint32_t log_begin_ok;
    uint32_t calls;                // stub calls this pass (a hang detector)
    uint32_t events;
};
static Ctrl* ctrl() { return (Ctrl*)(g_arena + OFF_CTRL); }
static uint8_t* misc(uint32_t off) { return g_arena + OFF_MISC + off; }
static bool in_arena(const void* p, uint32_t n = 1) { return (const uint8_t*)p >= g_arena && (const uint8_t*)p + n <= g_arena + ARENA_SIZE; }
static bool in_image_data(const void* p, uint32_t n = 1) {
    return (uintptr_t)p >= 0x4e1000 && (uintptr_t)p + n <= 0x5d6f2c;           // .data (and its .bss)
}
static bool writable(const void* p, uint32_t n) { return in_arena(p, n) || in_image_data(p, n); }

// the kernel's statics (and a little around them), snapshotted with the arena
struct GRange { uint32_t at, n; const char* what; };
static const GRange g_globals[] = {
    {0x004e6420, 0x0ac0, "prof/mem/task/sync/except data"}, {0x004ea7b4, 0x03b0, "vxd/kernel/_prof/bg data"},
    {0x00508190, 0x0d40, "prof/mem/task/sync/except/bg bss"},
};
enum { GLOBALS_BYTES = 0x0ac0 + 0x03b0 + 0x0d40 };
static const char* global_name(uint32_t a) {
    struct { uint32_t at, n; const char* nm; } t[] = {
        {0x4e6420, 1, "prof filter"}, {0x4e6424, 0x18, "prof hooks"}, {0x4e643c, 4, "ticks/ms"}, {0x4e6440, 4, "ms/2^32"},
        {0x4e6448, 8, "tsc0"}, {0x4e6450, 1, "timestr flip"}, {0x4e6588, 4, "heap"}, {0x4e658c, 4, "mem sync"},
        {0x4e6590, 1, "alloc mode"}, {0x4e6694, 4, "task sync"}, {0x4e6698, 4, "main task"}, {0x4e685c, 4, "nsingle"},
        {0x4e6860, 4, "nmulti"}, {0x4e6864, 4, "sync mutex"}, {0x4e6a5c, 4, "except log"}, {0x4e6a60, 8, "map"},
        {0x4e6a68, 0xc, "map handles"}, {0x4e6a74, 1, "abended"}, {0x4e6a78, 4, "exc count"}, {0x4ea7b4, 4, "vxd"},
        {0x4ea850, 1, "kernel up"}, {0x4ea91c, 0x18, "prof sys/ov"}, {0x4ea934, 0x200, "profiles"}, {0x4eab34, 0xc, "bg"},
        {0x508198, 4, "qpc/ms"}, {0x5081a0, 8, "qpc0"}, {0x5081a8, 1, "use rdtsc"}, {0x5081b0, 16, "timestr A"},
        {0x5081c0, 0x104, "prof names"}, {0x5082d8, 16, "timestr B"}, {0x50853c, 4, "mem cur"}, {0x508544, 4, "mem max"},
        {0x508668, 0xe0, "tasks"}, {0x508748, 8, "test timer"}, {0x508750, 0x80, "singles"}, {0x5087d0, 0x80, "multis"},
        {0x508850, 4, "ncs"}, {0x508858, 0x1c0, "cs pool"}, {0x508a18, 4, "prev filter"}, {0x508a20, 0x300, "map buffers"},
        {0x508d20, 0x10, "exit handlers"}, {0x508e50, 0x80, "bg hooks"},
    };
    for (auto& x : t)
        if (a >= x.at && a < x.at + x.n) return x.nm;
    return "?";
}

// ---- the stubs' log -------------------------------------------------------------------------------------------------------
#define LOG_KINDS(X)                                                                                                     \
    X(QPF) X(QPC) X(SYSINFO) X(HEAPCREATE) X(HEAPALLOC) X(HEAPFREE) X(HEAPVALIDATE) X(HEAPDESTROY) X(TID) X(TIMEKILL)      \
    X(CREATETHREAD) X(WAIT) X(CURTHREAD) X(SETPRIO) X(RESUME) X(SLEEP) X(SUSPEND) X(EXITTHREAD) X(EXITCODE) X(CSINIT)    \
    X(CSENTER) X(CSLEAVE) X(CSDELETE) X(CREATEEVENT) X(PULSE) X(CLOSE) X(SETFILTER) X(MODNAME) X(CREATEFILE) X(FILESIZE) \
    X(CREATEMAP) X(MAPVIEW) X(UNMAP) X(CLIPCURSOR) X(EXITPROCESS) X(IOCTL) X(LOCALTIME) X(RDTSC) X(REPORT) X(ERROR)      \
    X(PANIC) X(ERRSTR) X(W32TIME) X(APPINST) X(FWRITE) X(FCREATE) X(FCLOSE) X(FBEGIN) X(FEND) X(FVERIFY) X(LBEGIN)        \
    X(LEND) X(W32BEGIN) X(W32END) X(KBEGIN) X(KEND) X(SBEGIN) X(SEND) X(MBEGIN) X(MEND) X(JBEGIN) X(JEND) X(ATEXIT)       \
    X(HOOK) X(TIMERCB) X(THREADFN) X(EXITHANDLER) X(STRICMP) X(STRNICMP) X(STRNCPY) X(SPRINTF) X(VSPRINTF) X(SSCANF)         X(STRSTR) X(QSORT) X(MEMMOVE) X(CREATEDIR)
enum LogKind {
    L_NONE,
#define X(n) L_##n,
    LOG_KINDS(X)
#undef X
};
static const char* const g_log_names[] = {
    "?",
#define X(n) #n,
    LOG_KINDS(X)
#undef X
};
struct LogEntry { uint32_t kind, a[5], statics; };
enum { LOGN = 1 << 15, HANG_CALLS = 100000 };
static LogEntry g_log[2][LOGN];
static int g_nlog[2], g_pass;
static uint32_t g_script[256];
static int g_si;
static volatile bool g_live;                     // the live pass: real threads, the stubs only count
static volatile long g_live_calls[128];
static uint32_t script() { return g_script[g_si++ & 255]; }
enum : DWORD { PANIC_CODE = 0xE0005050, EXIT_CODE = 0xE0005051, HANG_CODE = 0xE0005052 };
// the statics a store could be moved across a call in: hashed into every log entry
static uint32_t statics_hash() {
    static const struct { uint32_t at, n; } r[] = {
        {0x4e6420, 0x34}, {0x4e6588, 0x0c}, {0x4e6694, 8}, {0x4e685c, 0x0c}, {0x4e6a5c, 0x20}, {0x4ea7b4, 4}, {0x4ea850, 4},
        {0x4ea91c, 0x18}, {0x4eab34, 0x0c}, {0x508198, 0x14}, {0x5082c0, 4}, {0x50853c, 0x0c}, {0x508668, 0xe8},
        {0x508750, 0x104}, {0x508a18, 4}, {0x508d20, 0x10}, {0x508e50, 0x80},
    };
    uint32_t h = 2166136261u;
    for (auto& x : r) h = (h ^ hash_bytes((void*)(uintptr_t)x.at, x.n)) * 16777619u;
    return h;
}
static void logn(uint32_t kind, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0) {
    if (g_live) { InterlockedIncrement(&g_live_calls[kind]); return; }
    if (g_nlog[g_pass] < LOGN) g_log[g_pass][g_nlog[g_pass]] = {kind, {a0, a1, a2, a3, a4}, statics_hash()};
    g_nlog[g_pass]++;
    if (++ctrl()->calls > HANG_CALLS) RaiseException(HANG_CODE, 0, 0, 0);
}
// a pointer as the log sees it: the stack (whose layout differs between the original and the rewrite) as a marker
static uint32_t P(const void* p) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    if ((uint8_t*)p >= (uint8_t*)tib->StackLimit && (uint8_t*)p < (uint8_t*)tib->StackBase) return 0x57ac0000u;
    return (uint32_t)(uintptr_t)p;
}
static uint32_t safe_hash_str(const char* s) {
    if (!s) return 0xdead;
    if (!in_arena(s) && !((uintptr_t)s >= 0x400000 && (uintptr_t)s < 0x5d7000)) {
        NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
        if (!((uint8_t*)s >= (uint8_t*)tib->StackLimit && (uint8_t*)s < (uint8_t*)tib->StackBase)) return 0xbad0000u ^ (uint32_t)(uintptr_t)s;
    }
    return hash_str(s);
}
// a printf-style call: the format, and a hash of its arguments by their conversions (%s by content)
static void log_fmt(uint32_t kind, const char* fmt, va_list ap) {
    uint32_t h = 0x811c9dc5u;
    int n = 0;
    const char* f = fmt;
    if (f && ((uintptr_t)f >= 0x400000 && (uintptr_t)f < 0x5d7000))
        for (; *f; f++) {
            if (*f != '%') continue;
            f++;
            while (*f && strchr("-+ #0123456789.lhL", *f)) f++;
            if (!*f) break;
            if (*f == '%') continue;
            n++;
            if (*f == 's') h = (h ^ safe_hash_str(va_arg(ap, const char*))) * 16777619u;
            else if (strchr("feEgG", *f)) {
                uint64_t d = va_arg(ap, uint64_t);
                h = (h ^ (uint32_t)d) * 16777619u;
                h = (h ^ (uint32_t)(d >> 32)) * 16777619u;
            } else h = (h ^ va_arg(ap, uint32_t)) * 16777619u;
        }
    logn(kind, P(fmt), h, n);
}

// ---- the Windows stubs ------------------------------------------------------------------------------------------------------
static BOOL WINAPI st_qpf(LARGE_INTEGER* f) {
    logn(L_QPF, P(f));
    if (script() % 100 < ctrl()->qpf_fail_pct) return FALSE;
    if (writable(f, 8) || P(f) == 0x57ac0000u) { f->LowPart = ctrl()->qpf_lo; f->HighPart = (LONG)ctrl()->qpf_hi; }
    return TRUE;
}
static BOOL WINAPI st_qpc(LARGE_INTEGER* c) {
    uint32_t s = script();
    uint64_t step = s % 5 == 0 ? 0 : (uint64_t)(s >> 3) % (ctrl()->qpf_lo / 3 + 7) + 1;
    ctrl()->qpc += step;
    logn(L_QPC, P(c), (uint32_t)ctrl()->qpc, (uint32_t)(ctrl()->qpc >> 32));
    if (writable(c, 8) || P(c) == 0x57ac0000u) c->QuadPart = (LONGLONG)ctrl()->qpc;
    return TRUE;
}
static void WINAPI st_sysinfo(SYSTEM_INFO* si) {
    logn(L_SYSINFO, P(si));
    memset(si, 0, sizeof *si);
    si->dwPageSize = 4096;
    si->dwNumberOfProcessors = ctrl()->ncpu;
    si->dwActiveProcessorMask = 1;
}
static HANDLE WINAPI st_heapcreate(DWORD o, SIZE_T i, SIZE_T m) {
    logn(L_HEAPCREATE, o, (uint32_t)i, (uint32_t)m);
    return script() % 8 == 0 ? 0 : (HANDLE)0x00b0b000;
}
static void* bump(uint32_t n) {
    Ctrl* c = ctrl();
    n = (n + 7) & ~7u;
    if (n > HEAP_SIZE || c->heap_top + n > HEAP_SIZE) return 0;
    void* p = g_arena + OFF_HEAP + c->heap_top;
    c->heap_top += n;
    return p;
}
static void* WINAPI st_heapalloc(HANDLE h, DWORD fl, SIZE_T n) {
    logn(L_HEAPALLOC, (uint32_t)(uintptr_t)h, fl, (uint32_t)n);
    if (script() % 16 == 0) return 0;
    return bump((uint32_t)n);
}
static BOOL WINAPI st_heapfree(HANDLE h, DWORD fl, void* p) { logn(L_HEAPFREE, (uint32_t)(uintptr_t)h, fl, P(p)); return TRUE; }
static BOOL WINAPI st_heapvalidate(HANDLE h, DWORD fl, const void* p) { logn(L_HEAPVALIDATE, (uint32_t)(uintptr_t)h, fl, P(p)); return script() & 1; }
static BOOL WINAPI st_heapdestroy(HANDLE h) { logn(L_HEAPDESTROY, (uint32_t)(uintptr_t)h); return TRUE; }
static DWORD WINAPI st_tid() { logn(L_TID); return ctrl()->tid; }
static MMRESULT WINAPI st_timekill(UINT id) { logn(L_TIMEKILL, id); return 0; }
static HANDLE WINAPI st_createthread(LPSECURITY_ATTRIBUTES sa, SIZE_T st, LPTHREAD_START_ROUTINE fn, void* param, DWORD fl, DWORD* tid) {
    logn(L_CREATETHREAD, P(sa), (uint32_t)st, (uint32_t)(uintptr_t)fn, P(param), fl ^ (P(tid) << 8));
    if (script() % 8 == 0) return 0;
    Ctrl* c = ctrl();
    if (tid && writable(tid, 4)) *tid = c->new_tid;
    c->new_tid += 0x10;
    return (HANDLE)(uintptr_t)(c->new_handle++);
}
static DWORD WINAPI st_wait(HANDLE h, DWORD ms) { logn(L_WAIT, (uint32_t)(uintptr_t)h, ms); return script() % 4 == 0 ? WAIT_TIMEOUT : 0; }
static HANDLE WINAPI st_curthread() { logn(L_CURTHREAD); return (HANDLE)(intptr_t)-2; }
static BOOL WINAPI st_setprio(HANDLE h, int p) { logn(L_SETPRIO, (uint32_t)(uintptr_t)h, (uint32_t)p); return script() % 4 != 0; }
static DWORD WINAPI st_resume(HANDLE h) {
    logn(L_RESUME, (uint32_t)(uintptr_t)h);
    uint32_t s = script() % 4;
    return s == 3 ? (DWORD)-1 : s;
}
static void WINAPI st_sleep(DWORD ms) {
    logn(L_SLEEP, ms);
    if (++ctrl()->sleeps == ctrl()->sleep_flag_at) G8(T_TEST) = 1;     // the test timer "runs"
}
static DWORD WINAPI st_suspend(HANDLE h) { logn(L_SUSPEND, (uint32_t)(uintptr_t)h); return script() % 3; }
static void WINAPI st_exitthread(DWORD code) { logn(L_EXITTHREAD, code); RaiseException(EXIT_CODE, 0, 0, 0); }
static BOOL WINAPI st_exitcode(HANDLE h, DWORD* code) {
    logn(L_EXITCODE, (uint32_t)(uintptr_t)h, P(code));
    uint32_t s = script() % 4;
    if (s == 0) return FALSE;
    *code = s == 1 ? STILL_ACTIVE : script();
    return TRUE;
}
// critical sections: emulated on the struct (deterministic contents), never a real lock
static void WINAPI st_csinit(CRITICAL_SECTION* cs) {
    logn(L_CSINIT, P(cs));
    if (!writable(cs, 24)) return;
    memset(cs, 0, 24);
    cs->LockCount = -1;
}
static void WINAPI st_csenter(CRITICAL_SECTION* cs) { logn(L_CSENTER, P(cs)); if (writable(cs, 24)) cs->RecursionCount++; }
static void WINAPI st_csleave(CRITICAL_SECTION* cs) { logn(L_CSLEAVE, P(cs)); if (writable(cs, 24)) cs->RecursionCount--; }
static void WINAPI st_csdelete(CRITICAL_SECTION* cs) { logn(L_CSDELETE, P(cs)); if (writable(cs, 24)) memset(cs, 0xdd, 24); }
static HANDLE WINAPI st_createevent(LPSECURITY_ATTRIBUTES sa, BOOL m, BOOL i, LPCSTR n) {
    logn(L_CREATEEVENT, P(sa), m, i, P(n));
    return (HANDLE)(uintptr_t)(0xe0e0 + ctrl()->events++);
}
static BOOL WINAPI st_pulse(HANDLE h) { logn(L_PULSE, (uint32_t)(uintptr_t)h); return TRUE; }
static BOOL WINAPI st_close(HANDLE h) { logn(L_CLOSE, (uint32_t)(uintptr_t)h); return TRUE; }
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI st_setfilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    logn(L_SETFILTER, (uint32_t)(uintptr_t)f);
    return (LPTOP_LEVEL_EXCEPTION_FILTER)(uintptr_t)ctrl()->prev_filter;
}
#ifdef FIX_TESTS
static const char* g_modname_override;           // the fix test's exe path ("": GetModuleFileNameA fails)
#endif
static DWORD WINAPI st_modname(HMODULE m, LPSTR buf, DWORD n) {
    logn(L_MODNAME, (uint32_t)(uintptr_t)m, P(buf), n);
#ifdef FIX_TESTS
    if (g_modname_override) {
        const DWORD len = (DWORD)strlen(g_modname_override);
        if (!len || !n) return 0;
        if (len >= n) { memcpy(buf, g_modname_override, n - 1); buf[n - 1] = 0; return n; }   // cut short
        memcpy(buf, g_modname_override, len + 1);
        return len;
    }
#endif
    if (script() % 8 == 0) return 0;
    static const char path[] = "C:\\Games\\Viper Racing\\race.exe";
    memcpy(buf, path, sizeof path);
    return (DWORD)strlen(path);
}
#ifdef FIX_TESTS
// the fixed ExceptBegin makes its log folder: logged, never made
static BOOL WINAPI st_createdir(LPCSTR n, LPSECURITY_ATTRIBUTES sa) { logn(L_CREATEDIR, hash_str(n), P(sa)); return TRUE; }
#endif
static HANDLE WINAPI st_createfile(LPCSTR n, DWORD acc, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE t) {
    logn(L_CREATEFILE, hash_str(n), acc, sh ^ (disp << 8) ^ (P(sa) << 16), fl, (uint32_t)(uintptr_t)t);
    uint32_t s = script() % 6;
    return s == 0 ? (HANDLE)0 : s == 1 ? INVALID_HANDLE_VALUE : (HANDLE)0x1f0;
}
static DWORD WINAPI st_filesize(HANDLE h, DWORD* hi) { logn(L_FILESIZE, (uint32_t)(uintptr_t)h, P(hi)); return ctrl()->file_size; }
static HANDLE WINAPI st_createmap(HANDLE h, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR n) {
    logn(L_CREATEMAP, (uint32_t)(uintptr_t)h, P(sa), prot, hi ^ (lo << 4), hash_str(n));
    return script() % 6 == 0 ? (HANDLE)0 : (HANDLE)0x2f0;
}
static void* WINAPI st_mapview(HANDLE m, DWORD acc, DWORD hi, DWORD lo, SIZE_T n) {
    logn(L_MAPVIEW, (uint32_t)(uintptr_t)m, acc, hi, lo, (uint32_t)n);
    return script() % 6 == 0 ? 0 : g_file;
}
static BOOL WINAPI st_unmap(const void* p) { logn(L_UNMAP, P(p)); return TRUE; }
static BOOL WINAPI st_clipcursor(const RECT* r) { logn(L_CLIPCURSOR, P(r)); return TRUE; }
static void WINAPI st_exitprocess(UINT code) { logn(L_EXITPROCESS, code); RaiseException(EXIT_CODE, 0, 0, 0); }
static BOOL WINAPI st_ioctl(HANDLE h, DWORD code, void* in, DWORD insz, void* out, DWORD outsz, DWORD* got, LPOVERLAPPED ov) {
    uint32_t inv = in && (writable(in, 4) || P(in) == 0x57ac0000u) ? *(uint32_t*)in : 0;
    logn(L_IOCTL, (uint32_t)(uintptr_t)h, code, inv ^ (P(in) << 1), insz ^ (outsz << 8), P(out) ^ (P(got) << 1) ^ P(ov));
    if (script() % 4 == 0) return FALSE;
    if (out && outsz >= 4 && (writable(out, 4) || P(out) == 0x57ac0000u)) {
        if (code == 4) *(uint32_t*)out = (uint32_t)(uintptr_t)bump(inv < 0x1000 ? inv : 0x1000);
        else *(uint32_t*)out = script();
    }
    if (got) *got = outsz;
    return TRUE;
}
static void WINAPI st_localtime(SYSTEMTIME* st) {
    logn(L_LOCALTIME, P(st));
    uint32_t a = script(), b = script(), c = script();
    st->wYear = (WORD)(1990 + a % 50); st->wMonth = (WORD)(b % 13); st->wDayOfWeek = (WORD)(c % 7);
    st->wDay = (WORD)(a >> 16); st->wHour = (WORD)(b >> 16); st->wMinute = (WORD)(c >> 16);
    st->wSecond = (WORD)(a >> 8); st->wMilliseconds = (WORD)(b >> 8);
}

// ---- the TSC: every rdtsc of the original's _prof.obj is a ud2 answered by the vectored handler -----------------------------
static const uint32_t k_rdtsc_sites[] = {0x418782, 0x41879a, 0x41883a, 0x41887d, 0x418891, 0x4188ad};
static uint64_t __cdecl fake_rdtsc() {
    if (g_live) return __rdtsc();
    uint32_t s = script();
    uint64_t step = s % 7 == 0 ? 0 : (uint64_t)(s >> 4) * (1 + (s & 7));
    ctrl()->tsc += step;
    logn(L_RDTSC, (uint32_t)ctrl()->tsc, (uint32_t)(ctrl()->tsc >> 32));
    return ctrl()->tsc;
}
static LONG CALLBACK rdtsc_veh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t at = e->ContextRecord->Eip;
    for (uint32_t s : k_rdtsc_sites)
        if (at == s) {
            uint64_t t = fake_rdtsc();
            e->ContextRecord->Eax = (DWORD)t;
            e->ContextRecord->Edx = (DWORD)(t >> 32);
            e->ContextRecord->Eip += 2;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void rdtsc_sites(bool fake) {
    for (uint32_t s : k_rdtsc_sites) ((uint8_t*)(uintptr_t)s)[1] = fake ? 0x0b : 0x31;   // 0F 0B ud2 / 0F 31 rdtsc
}

// ---- the game's functions outside the group -----------------------------------------------------------------------------------
static int g_panic_mode_ends = 1;
static void __cdecl st_report(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_fmt(L_REPORT, fmt, ap); va_end(ap); }
static void __cdecl st_error(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_fmt(L_ERROR, fmt, ap); va_end(ap); }
static void __cdecl st_panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_fmt(L_PANIC, fmt, ap);
    va_end(ap);
    if (g_live) { printf("  (live) LogPanic: %s\n", fmt); return; }
    if (!ctrl()->panic_returns) RaiseException(PANIC_CODE, 0, 0, 0);   // _LogPanic -> abend: it doesn't return
}
static const char* __cdecl st_errstr() { logn(L_ERRSTR); return (const char*)misc(M_ERR); }
static int __cdecl st_w32time() { logn(L_W32TIME); return (int)(ctrl()->w32time += script() % 50); }
static uint8_t* __cdecl st_appinst() { logn(L_APPINST); return (uint8_t*)(uintptr_t)ctrl()->app_instance; }
static uint8_t __cdecl st_fwrite(int f, const void* p, int n) {
    logn(L_FWRITE, (uint32_t)f, n > 0 && n < 0x10000 ? hash_bytes(p, (size_t)n) : 0xbad, (uint32_t)n);
    return 1;
}
static int __cdecl st_fcreate(const char* n) { logn(L_FCREATE, safe_hash_str(n)); return script() % 3 == 0 ? 0 : 7; }
static void __cdecl st_fclose(int* p) { logn(L_FCLOSE, P(p), writable(p, 4) ? (uint32_t)*p : 0xbad); }
#define VSTUB(NAME, KIND) static void __cdecl NAME() { logn(KIND); }
VSTUB(st_fbegin, L_FBEGIN) VSTUB(st_fend, L_FEND) VSTUB(st_fverify, L_FVERIFY) VSTUB(st_lend, L_LEND)
VSTUB(st_w32begin, L_W32BEGIN) VSTUB(st_w32end, L_W32END) VSTUB(st_kbegin, L_KBEGIN) VSTUB(st_kend, L_KEND)
VSTUB(st_sbegin, L_SBEGIN) VSTUB(st_send, L_SEND) VSTUB(st_mbegin, L_MBEGIN) VSTUB(st_mend, L_MEND)
VSTUB(st_jbegin, L_JBEGIN) VSTUB(st_jend, L_JEND)
#undef VSTUB
static uint8_t __cdecl st_lbegin() { logn(L_LBEGIN); return (uint8_t)ctrl()->log_begin_ok; }
static int __cdecl st_atexit(uint32_t f) { logn(L_ATEXIT, f); return 0; }
static void __cdecl st_crt_fatal(int code) {
    printf("the game's CRT hit a fatal error (%d), pass %d: stopping\n", code, g_pass);
    ExitProcess(4);
}
static int __cdecl st_crt_msgbox(const char* text, const char*, unsigned) {
    printf("the game's CRT tried a message box: %s\n", text ? text : "?");
    ExitProcess(4);
    return 0;
}
// the game's CRT: every call logged (arguments by content), then done by the host's CRT. A null string faults, as
// the game's would (the host's would call its invalid-parameter handler, which here raises too).
static void touch_null(const void* p) { if (!p) *(volatile char*)0 = 0; }
static int __cdecl st_stricmp(const char* a, const char* b) {
    logn(L_STRICMP, safe_hash_str(a), safe_hash_str(b), P(a), P(b));
    touch_null(a); touch_null(b);
    return _stricmp(a, b);
}
static int __cdecl st_strnicmp(const char* a, const char* b, uint32_t n) {
    logn(L_STRNICMP, safe_hash_str(a), safe_hash_str(b), P(a), P(b), n);
    touch_null(a); touch_null(b);
    return _strnicmp(a, b, n);
}
static char* __cdecl st_strncpy(char* d, const char* s, uint32_t n) {
    logn(L_STRNCPY, P(d), safe_hash_str(s), P(s), n);
    touch_null(d); touch_null(s);
    return strncpy(d, s, n);
}
static int __cdecl st_vsprintf(char* buf, const char* fmt, va_list ap) {
    va_list aq = ap;
    log_fmt(L_VSPRINTF, fmt, aq);
    logn(L_VSPRINTF, P(buf));
    touch_null(buf); touch_null(fmt);
    return vsprintf(buf, fmt, ap);
}
static int __cdecl st_sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_fmt(L_SPRINTF, fmt, ap);
    va_end(ap);
    logn(L_SPRINTF, P(buf));
    touch_null(buf); touch_null(fmt);
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}
static int __cdecl st_sscanf(const char* str, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint32_t h = 0;
    for (int i = 0; i < 3; i++) h = h * 31 + P(((void**)ap)[i]);
    logn(L_SSCANF, safe_hash_str(str), P(fmt), h);
    touch_null(str); touch_null(fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}
static char* __cdecl st_strstr(const char* a, const char* b) {
    logn(L_STRSTR, safe_hash_str(a), safe_hash_str(b), P(a), P(b));
    touch_null(a); touch_null(b);
    return (char*)strstr(a, b);
}
static void __cdecl st_qsort(void* base, uint32_t n, uint32_t size, int(__cdecl* cmp)(const void*, const void*)) {
    logn(L_QSORT, P(base), n, size, (uint32_t)(uintptr_t)cmp);
    qsort(base, n, size, cmp);
}
static void* __cdecl st_memmove(void* d, const void* s, uint32_t n) {
    logn(L_MEMMOVE, P(d), P(s), n);
    return memmove(d, s, n);
}
static void invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, 0);
}

// callbacks: BG hooks, the timer's function, a task's body, exit handlers
static void __cdecl st_hook0() { logn(L_HOOK, 0); }
static void __cdecl st_hook1() { logn(L_HOOK, 1); }
static void __cdecl st_hook2() { logn(L_HOOK, 2); }
static void __cdecl st_hook3() { logn(L_HOOK, 3); }
static void(__cdecl* const g_hooks[])() = {st_hook0, st_hook1, st_hook2, st_hook3, FN(Void_t, 0x004189e0)};
static void __cdecl st_timer_cb() {
    Ctrl* c = ctrl();
    logn(L_TIMERCB, c->cb_calls);
    c->qpc += (uint64_t)(script() % (c->cb_step + 1)) * (c->qpf_lo / 1000 + 1);   // the callback "takes" some ms
    c->tsc += (uint64_t)(script() % (c->cb_step + 1)) * 1000000u;
    if (++c->cb_calls >= c->cb_limit && c->cb_task) G8(T_TABLE + (c->cb_task - 1) * 0x1c + 0x19) = 1;
}
static void __cdecl st_thread_fn() { logn(L_THREADFN); }
static void __cdecl st_exit0() { logn(L_EXITHANDLER, 0); }
static void __cdecl st_exit1() { logn(L_EXITHANDLER, 1); }

struct SlotStub { const char* name; void* stub; uint32_t expect; };
static const SlotStub g_slot_stubs[] = {
    {"QueryPerformanceFrequency", (void*)st_qpf, 0x5d74c4}, {"QueryPerformanceCounter", (void*)st_qpc, 0x5d7488},
    {"GetSystemInfo", (void*)st_sysinfo, 0x5d745c}, {"HeapCreate", (void*)st_heapcreate, 0x5d7494},
    {"HeapAlloc", (void*)st_heapalloc, 0x5d7498}, {"HeapValidate", (void*)st_heapvalidate, 0x5d749c},
    {"HeapFree", (void*)st_heapfree, 0x5d74a0}, {"HeapDestroy", (void*)st_heapdestroy, 0x5d74a4},
    {"GetCurrentThreadId", (void*)st_tid, 0x5d7474}, {"timeKillEvent", (void*)st_timekill, 0x5d76dc},
    {"CreateThread", (void*)st_createthread, 0x5d74a8}, {"WaitForSingleObject", (void*)st_wait, 0x5d7480},
    {"GetCurrentThread", (void*)st_curthread, 0x5d74b0}, {"SetThreadPriority", (void*)st_setprio, 0x5d74ac},
    {"ResumeThread", (void*)st_resume, 0x5d74b4}, {"Sleep", (void*)st_sleep, 0x5d74b8},
    {"SuspendThread", (void*)st_suspend, 0x5d74bc}, {"ExitThread", (void*)st_exitthread, 0x5d74c0},
    {"GetExitCodeThread", (void*)st_exitcode, 0x5d7574}, {"InitializeCriticalSection", (void*)st_csinit, 0x5d7578},
    {"EnterCriticalSection", (void*)st_csenter, 0x5d74cc}, {"LeaveCriticalSection", (void*)st_csleave, 0x5d74d0},
    {"DeleteCriticalSection", (void*)st_csdelete, 0x5d74d4}, {"CreateEventA", (void*)st_createevent, 0x5d7564},
    {"PulseEvent", (void*)st_pulse, 0x5d7568}, {"CloseHandle", (void*)st_close, 0x5d7548},
    {"SetUnhandledExceptionFilter", (void*)st_setfilter, 0x5d7570}, {"GetModuleFileNameA", (void*)st_modname, 0x5d74e4},
    {"CreateFileA", (void*)st_createfile, 0x5d7554}, {"GetFileSize", (void*)st_filesize, 0x5d7544},
    {"CreateFileMappingA", (void*)st_createmap, 0x5d7468}, {"MapViewOfFile", (void*)st_mapview, 0x5d7464},
    {"UnmapViewOfFile", (void*)st_unmap, 0x5d746c}, {"ClipCursor", (void*)st_clipcursor, 0x5d76b4},
    {"ExitProcess", (void*)st_exitprocess, 0x5d7478}, {"DeviceIoControl", (void*)st_ioctl, 0x5d74ec},
    {"GetLocalTime", (void*)st_localtime, 0x5d75f0},
#ifdef FIX_TESTS
    {"CreateDirectoryA", (void*)st_createdir, 0x5d7490},
#endif
};
// the live pass keeps these as stubs (no process-wide side effects); everything else gets the real function
static bool live_keeps_stub(const char* n) {
    static const char* const keep[] = {"SetUnhandledExceptionFilter", "GetModuleFileNameA", "CreateFileA", "GetFileSize",
        "CreateFileMappingA", "MapViewOfFile", "UnmapViewOfFile", "ClipCursor", "ExitProcess", "DeviceIoControl", "CreateDirectoryA"};
    for (const char* k : keep)
        if (!strcmp(k, n)) return true;
    return false;
}
static void set_slots(bool live) {
    for (const SlotStub& s : g_slot_stubs) {
        const Slot* found = 0;
        for (const Slot& x : g_slots)
            if (!strcmp(x.name, s.name)) found = &x;
        if (!found) { printf("no import %s\n", s.name); ExitProcess(2); }
        if (found->at != s.expect) { printf("import %s is at %08x, the rewrite uses %08x\n", s.name, found->at, s.expect); ExitProcess(2); }
        *(void**)(uintptr_t)found->at = live && !live_keeps_stub(s.name) ? found->real : s.stub;
    }
}
static void install_stubs() {
    struct { uint32_t at; void* to; } p[] = {
        {0x00411150, (void*)&st_report}, {0x004111d0, (void*)&st_error}, {0x004112b0, (void*)&st_panic},
        {0x00416070, (void*)&st_errstr}, {0x00412d70, (void*)&st_w32time}, {0x00412bc0, (void*)&st_appinst},
        {0x00411a30, (void*)&st_fwrite}, {0x004115f0, (void*)&st_fcreate}, {0x00411850, (void*)&st_fclose},
        {0x00411540, (void*)&st_fbegin}, {0x004115d0, (void*)&st_fend}, {0x00411580, (void*)&st_fverify},
        {0x00410d10, (void*)&st_lbegin}, {0x00411080, (void*)&st_lend}, {0x00412650, (void*)&st_w32begin},
        {0x00412b50, (void*)&st_w32end}, {0x00413bf0, (void*)&st_kbegin}, {0x00413c30, (void*)&st_kend},
        {0x00412f00, (void*)&st_sbegin}, {0x00412ff0, (void*)&st_send}, {0x00414430, (void*)&st_mbegin},
        {0x00414450, (void*)&st_mend}, {0x00418bb0, (void*)&st_jbegin}, {0x00418f30, (void*)&st_jend},
        {0x004ceff0, (void*)&st_atexit}, {0x004da350, (void*)&st_stricmp}, {0x004da3e0, (void*)&st_strnicmp},
        {0x004cf3a0, (void*)&st_strncpy}, {0x004cf0a0, (void*)&st_sprintf}, {0x004cf7c0, (void*)&st_vsprintf},
        {0x004ce190, (void*)&st_sscanf}, {0x004cf9a0, (void*)&st_strstr}, {0x004cf160, (void*)&st_qsort},
        {0x004cf400, (void*)&st_memmove},
    };
    for (auto& x : p) patch_jmp(x.at, x.to);
    patch_jmp(0x004cf730, (void*)&st_crt_fatal);
    patch_jmp(0x004d45b0, (void*)&st_crt_fatal);
    patch_jmp(0x004d4570, (void*)&st_crt_fatal);
    patch_jmp(0x004d5c10, (void*)&st_crt_fatal);
    patch_jmp(0x004d6850, (void*)&st_crt_msgbox);
    ((void(__cdecl*)())0x004ce150)();                  // _cfltcvt_init: the CRT's %f converters
    rdtsc_sites(true);
    AddVectoredExceptionHandler(1, rdtsc_veh);
    set_slots(false);
}

// ---- running one call both ways ----------------------------------------------------------------------------------------------
template <typename F, typename... A> static uint64_t invoke(F f, A... a) {
    typedef decltype(f(a...)) R;
    if constexpr (std::is_void_v<R>) {
        f(a...);
        return 0;
    } else {
        R r = f(a...);
        uint64_t u = 0;
        memcpy(&u, &r, sizeof r);
        return u;
    }
}
struct Stat { std::string name; long calls, changed, fails, fp_fails, faults, panics, exits, hangs, replay_only; uint64_t seen[2]; };
static std::vector<Stat> g_stats;
static Stat& stat(const std::string& name) {
    for (Stat& s : g_stats)
        if (s.name == name) return s;
    g_stats.push_back({name, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0, 0}});
    return g_stats.back();
}
struct Snapshot { uint8_t arena[ARENA_SIZE]; uint8_t globals[GLOBALS_BYTES]; };
static Snapshot g_start, g_after, g_saved;
static Footprint g_fp;
static int g_wno;
static bool g_chain, g_verbose;
static long g_mismatch_total;
static void snap_globals(uint8_t* p) {
    for (const GRange& g : g_globals) { memcpy(p, (void*)(uintptr_t)g.at, g.n); p += g.n; }
}
static void save(Snapshot& s) { memcpy(s.arena, g_arena, ARENA_SIZE); snap_globals(s.globals); }
static void load(const Snapshot& s) {
    memcpy(g_arena, s.arena, ARENA_SIZE);
    const uint8_t* p = s.globals;
    for (const GRange& g : g_globals) { memcpy((void*)(uintptr_t)g.at, p, g.n); p += g.n; }
}
static bool in_footprint(const void* p) {
    for (int k = 0; k < g_fp.n; k++)
        if ((const uint8_t*)p >= (const uint8_t*)g_fp.r[k].p && (const uint8_t*)p < (const uint8_t*)g_fp.r[k].p + g_fp.r[k].n) return true;
    return false;
}
static __declspec(noinline) void stack_fill(uint32_t pat) {
    volatile uint32_t buf[0x4000];
    for (int i = 0; i < 0x4000; i++) buf[i] = pat;
}
static uint32_t g_fault_code, g_fault_at;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_at = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
enum { R_RAN, R_PANIC, R_EXIT, R_HANG, R_FAULT };
static const char* const k_result[] = {"returned", "panicked", "exited", "hung", "faulted"};
#if defined(__GNUC__) && !defined(__clang__)
template <typename Run> static int guarded(Run& run, uint64_t* ret, uint16_t* cw) {
    if (vp_try([&] {
            *ret = run();
            uint16_t w;
            __asm__ volatile("fnstcw %0" : "=m"(w));
            *cw = w;
        }, fault_filter)) {
        uint16_t w;
        __asm__ volatile("fnclex" ::: VP_X87_CLOBBERS);  // an x87 fault is still pending: /fp:precise ends this block with fwait
        __asm__ volatile("fnstcw %0" : "=m"(w));
        *cw = w;
        return g_fault_code == PANIC_CODE ? R_PANIC : g_fault_code == EXIT_CODE ? R_EXIT : g_fault_code == HANG_CODE ? R_HANG : R_FAULT;
    }
    return R_RAN;
}
static void fpu_start(uint16_t cw) {
    __asm__ volatile("fninit\n\tfldcw %0" :: "m"(cw) : VP_X87_CLOBBERS);
}
static void fpu_reset() {
    unsigned cw;
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
    _controlfp_s(&cw, _PC_53, _MCW_PC);
}
#else
template <typename Run> static int guarded(Run& run, uint64_t* ret, uint16_t* cw) {
    __try {
        *ret = run();
        uint16_t w;
        __asm fnstcw w
        *cw = w;
        return R_RAN;
    } __except (fault_filter(GetExceptionInformation())) {
        uint16_t w;
        __asm fnclex                                     // an x87 fault is still pending: /fp:precise ends this block with fwait
        __asm fnstcw w
        *cw = w;
        return g_fault_code == PANIC_CODE ? R_PANIC : g_fault_code == EXIT_CODE ? R_EXIT : g_fault_code == HANG_CODE ? R_HANG : R_FAULT;
    }
}
static void fpu_start(uint16_t cw) {
    __asm fninit
    __asm fldcw cw
}
static void fpu_reset() {
    unsigned cw;
    __asm fninit
    _controlfp_s(&cw, _PC_53, _MCW_PC);
}
#endif
template <typename Run, typename Fp> static void check(const char* fname, Run run, Fp footprint) {
    std::string name = fname;
    if (g_chain) name += " [chain]";
    if (g_verbose) printf("  %s\n", name.c_str());
    Stat& st = stat(name);
    st.calls++;
    for (int i = 0; i < 256; i++) g_script[i] = rnd();
    const uint32_t pat = chance(50) ? rnd() : 0;
    // a random starting control word: precision 24 / 53 / 64, the physics' masks or all masked
    static const uint16_t k_cw[] = {0x027f, 0x007f, 0x037f, 0x0073, 0x0063, 0x0373};
    const uint16_t cw0 = k_cw[rnd() % 6];
    ctrl()->calls = 0;
    save(g_start);
    g_fp.n = 0; g_fp.replay_only = 0; g_fp.pure = false;
    footprint(g_fp);
    if (g_fp.replay_only) st.replay_only++;
    uint64_t ro = 0, rn = 0;
    uint16_t cwo = 0, cwn = 0;
    auto r0 = [&]() { return run(true); };
    auto r1 = [&]() { return run(false); };
    // the original
    g_pass = 0; g_nlog[0] = 0; g_si = 0;
    stack_fill(pat);
    fpu_start(cw0);
    int fo = guarded(r0, &ro, &cwo);
    fpu_reset();
    save(g_after);
    if (memcmp(g_after.arena + sizeof(Ctrl), g_start.arena + sizeof(Ctrl), ARENA_SIZE - sizeof(Ctrl)) ||
        memcmp(g_after.globals, g_start.globals, GLOBALS_BYTES)) st.changed++;
    // the rewrite
    load(g_start);
    g_pass = 1; g_nlog[1] = 0; g_si = 0;
    if (g_chain) chain_patch();
    stack_fill(pat);
    fpu_start(cw0);
    int fn = guarded(r1, &rn, &cwn);
    fpu_reset();
    if (g_chain) chain_unpatch();
    for (int i = 0; i < (g_nlog[0] < LOGN ? g_nlog[0] : LOGN); i++) st.seen[g_log[0][i].kind >> 6] |= 1ull << (g_log[0][i].kind & 63);
    if (fo == R_PANIC) st.panics++;
    if (fo == R_EXIT) st.exits++;
    if (fo == R_HANG) st.hangs++;
    if (fo == R_FAULT) st.faults++;
    uint8_t now_globals[GLOBALS_BYTES];
    snap_globals(now_globals);
    int nlog0 = g_nlog[0] < LOGN ? g_nlog[0] : LOGN;
    // a fault ends the pass wherever it happens: compare how it ended and the log so far, not the state
    bool bad = fo != fn || (fo == R_RAN && ro != rn) || g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], sizeof(LogEntry) * nlog0) ||
               (fo != R_FAULT && (memcmp(g_after.arena, g_arena, ARENA_SIZE) || memcmp(g_after.globals, now_globals, GLOBALS_BYTES) || cwo != cwn));
    if (bad) {
        g_mismatch_total++;
        if (st.fails++ < 4) {
            printf("MISMATCH %s (world %d): original %s, rewrite %s\n", name.c_str(), g_wno, k_result[fo], k_result[fn]);
            if (fo == R_FAULT || fn == R_FAULT) printf("  fault %08x at %08x\n", g_fault_code, g_fault_at);
            if (ro != rn) printf("  return: original %016llx, rewrite %016llx\n", ro, rn);
            if (cwo != cwn) printf("  control word: original %04x, rewrite %04x\n", cwo, cwn);
            int shown = 0;
            for (uint32_t i = 0; i < ARENA_SIZE && shown < 8; i += 4)
                if (memcmp(g_after.arena + i, g_arena + i, 4)) {
                    uint32_t a, b, s;
                    memcpy(&a, g_after.arena + i, 4); memcpy(&b, g_arena + i, 4); memcpy(&s, g_start.arena + i, 4);
                    printf("  arena+0x%x: start %08x, original %08x, rewrite %08x\n", i, s, a, b);
                    shown++;
                }
            const uint8_t* pa = g_after.globals;
            const uint8_t* pb = now_globals;
            for (const GRange& g : g_globals) {
                int k = 0;
                for (uint32_t i = 0; i < g.n && k < 6; i++)
                    if (pa[i] != pb[i]) { printf("  global %08x (%s): original %02x, rewrite %02x\n", g.at + i, global_name(g.at + i), pa[i], pb[i]); k++; }
                pa += g.n; pb += g.n;
            }
            int nl = g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1];
            if (nl > LOGN) nl = LOGN;
            for (int i = 0, k = 0; i < nl && k < 6; i++) {
                LogEntry* a = i < g_nlog[0] ? &g_log[0][i] : 0;
                LogEntry* b = i < g_nlog[1] ? &g_log[1][i] : 0;
                if (a && b && !memcmp(a, b, sizeof *a)) continue;
                k++;
                if (a) printf("  call %d original: %s %08x %08x %08x %08x %08x\n", i, g_log_names[a->kind], a->a[0], a->a[1], a->a[2], a->a[3], a->a[4]);
                if (b) printf("  call %d rewrite:  %s %08x %08x %08x %08x %08x\n", i, g_log_names[b->kind], b->a[0], b->a[1], b->a[2], b->a[3], b->a[4]);
            }
            if (g_nlog[0] != g_nlog[1]) printf("  %d calls vs %d\n", g_nlog[0], g_nlog[1]);
        }
    }
    // the footprint must cover every byte the original changed (unless the call is left to the replay)
    if (!g_fp.replay_only && fo != R_FAULT) {
        bool fpbad = false;
        for (uint32_t i = sizeof(Ctrl); i < ARENA_SIZE && !fpbad; i++)
            if (g_after.arena[i] != g_start.arena[i] && !in_footprint(g_arena + i)) {
                if (st.fp_fails++ < 3) printf("FOOTPRINT %s (world %d): arena+0x%x changed outside it\n", name.c_str(), g_wno, i);
                fpbad = true;
            }
        const uint8_t* pa = g_after.globals;
        const uint8_t* ps = g_start.globals;
        for (const GRange& g : g_globals) {
            for (uint32_t i = 0; i < g.n && !fpbad; i++)
                if (pa[i] != ps[i] && !in_footprint((void*)(uintptr_t)(g.at + i))) {
                    if (st.fp_fails++ < 3) printf("FOOTPRINT %s (world %d): %08x (%s) changed outside it\n", name.c_str(), g_wno, g.at + i, global_name(g.at + i));
                    fpbad = true;
                }
            pa += g.n; ps += g.n;
        }
    }
    load(g_after);                                       // go on from the original's result
}

#define CHECK(FN, ...)                                                                                  \
    do {                                                                                                \
        auto args_ = std::make_tuple(__VA_ARGS__);                                                      \
        check(VP_CAT(name_, FN),                                                                        \
              [&](bool orig) {                                                                          \
                  auto f = orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN;                    \
                  return std::apply([&](auto... a) { return invoke(f, a...); }, args_);                 \
              },                                                                                        \
              [&](Footprint& fp) { std::apply([&](auto... a) { VP_CAT(fpof_, FN)(fp, a...); }, args_); });\
    } while (0)
#define CHECK0(FN)                                                                                      \
    check(VP_CAT(name_, FN), [&](bool orig) { return invoke(orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN); }, \
          [&](Footprint& fp) { VP_CAT(fpof_, FN)(fp); })
#define CHECK_BOTH(FN, ...) do { save(g_saved); g_chain = false; CHECK(FN, __VA_ARGS__); load(g_saved); g_chain = true; CHECK(FN, __VA_ARGS__); g_chain = false; } while (0)
#define CHECK0_BOTH(FN) do { save(g_saved); g_chain = false; CHECK0(FN); load(g_saved); g_chain = true; CHECK0(FN); g_chain = false; } while (0)

// ---- random worlds ---------------------------------------------------------------------------------------------------------------
static const char* const k_names[] = {"Physics", "physics", "PHYSICS", "Sound", "Mem", "task", "BG", "gx", "Replay", "net", "x"};
static char* arena_str(int i) { return (char*)(g_arena + OFF_STRS + (i % 16) * 32); }
static const char* rand_name() {
    int k = ri(0, 19);
    if (k < 11) return arena_str(k);
    if (k < 14) return S(0x4e6594 + 0);                 // "Mem" (the game's own string)
    if (k < 16) return S(0x4eab44);                     // "BG"
    if (k < 18) return S(0x4e66bc);                     // "task"
    return 0;
}
static uint32_t rand_cs_ptr() {
    int k = ri(0, 9);
    if (k < 6) return Y_CS + (uint32_t)ri(0, 15) * 0x1c;
    if (k < 8) return 0;
    return (uint32_t)(uintptr_t)(g_arena + OFF_MISC + M_CSPTR);
}
static uint32_t g_tids[] = {0x104, 0x208, 0x30c, 0x410};
static void random_world() {
    memset(g_arena, 0, ARENA_SIZE);
    Ctrl* c = ctrl();
    for (int i = 0; i < 11; i++) strcpy(arena_str(i), k_names[i]);
    strcpy((char*)misc(M_ERR), "The operation completed successfully.");
    static const uint32_t k_freqs[][2] = {{10000000, 0}, {3579545, 0}, {1193182, 0}, {2992530000u, 0}, {999, 0}, {0x1234, 1}, {2400000000u, 0}};
    int fq = ri(0, 6);
    c->qpf_lo = k_freqs[fq][0];
    c->qpf_hi = k_freqs[fq][1];
    c->qpf_fail_pct = chance(20) ? 30 : 0;
    c->qpc = (uint64_t)rnd() << (chance(30) ? 8 : 0);
    c->tsc = ((uint64_t)rnd() << 32 | rnd()) >> ri(0, 20);
    c->ncpu = chance(50) ? 1 : ri(0, 8);
    c->tid = chance(80) ? g_tids[rnd() % 4] : rnd();
    c->new_tid = 0x500 + (rnd() & 0xff) * 4;
    c->new_handle = 0x7000 + (rnd() & 0xff);
    c->sleep_flag_at = chance(70) ? ri(1, 35) : 0;
    c->cb_limit = chance(90) ? ri(1, 150) : ri(150, 2000);
    c->cb_step = ri(0, 40);
    c->panic_returns = chance(50);
    c->prev_filter = chance(50) ? 0 : rnd();
    c->file_size = chance(80) ? g_file_size : chance(50) ? g_sections_end : g_file_size + ri(-200, 200);
    c->w32time = rnd();
    c->app_instance = chance(80) ? 0x400000 : (uint32_t)(uintptr_t)(g_arena + OFF_FAKEPE);
    c->log_begin_ok = chance(85);
    // a fake PE whose section names vary (lockdown_exe)
    {
        uint8_t* pe = g_arena + OFF_FAKEPE;
        memcpy(pe, (void*)0x400000, 0x400);
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(pe + ((IMAGE_DOS_HEADER*)pe)->e_lfanew);
        nt->FileHeader.NumberOfSections = (WORD)ri(0, 5);
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        static const char* const k_secs[] = {".text", ".rdata", ".data", ".idata", ".rsrc", ".TEXT", ".DaTa", ".textbss"};
        for (int i = 0; i < 5; i++) {
            const char* nm = k_secs[rnd() % 8];
            memset(sec[i].Name, 0, 8);
            memcpy(sec[i].Name, nm, strlen(nm) > 8 ? 8 : strlen(nm));
            sec[i].Misc.VirtualSize = rnd() % 0x100000;
            sec[i].SizeOfRawData = rnd() % 0x100000;
        }
    }
    // a small fake link map (the search's text), sometimes cut without a final '\n'... always ending in one
    {
        char* m = (char*)(g_arena + OFF_MAP);
        int n = 0, lines = ri(0, 60);
        for (int i = 0; i < lines && n < MAP_CAP - 200; i++) {
            int kind = ri(0, 9);
            if (kind < 7) n += sprintf(m + n, " 0001:%08x       %s_%d          %08x f   x.obj\r\n", rnd() & 0xfffff, k_names[rnd() % 11], i, 0x401000 + (rnd() & 0x7ffff));
            else if (kind < 8) n += sprintf(m + n, " Address         Publics by Value              Rva+Base     Lib:Object\r\n");
            else if (kind < 9) n += sprintf(m + n, "\r\n");
            else n += sprintf(m + n, " FIXUPS: 1234 5678\r\n");
        }
        n += sprintf(m + n, " 0002:00000000 end\r\n");
    }
    // prof
    G8(P_FILTER) = chance(80) ? 0 : (uint8_t)"PpSxM"[rnd() % 5];
    static const uint32_t k_prof_ptrs[6][2] = {{0x41883e, 0x413550}, {0x41888f, 0x413550}, {0x418780, 0x413550},
                                               {0x418798, 0x413550}, {0x413480, 0x413560}, {0x4134d0, 0x413570}};
    bool rdtsc_path = chance(50);
    for (int i = 0; i < 6; i++) G32(P_SYS_BEGIN + i * 4) = k_prof_ptrs[i][rdtsc_path ? 0 : 1];
    G32(P_TICKS_MS) = chance(90) ? ri(1, 4000000) : 0;
    G32(P_HI_MS) = chance(90) ? rnd() % 5000000 : rnd();
    G64(P_TSC0) = c->tsc - (rnd() % 0x10000000);
    G8(P_FLIP) = (uint8_t)(rnd() & 3);
    G32(P_QPC_MS) = chance(85) ? ri(1, 3000000) : 0;
    G64(P_QPC0) = c->qpc - (rnd() % 0x1000000);
    G8(P_RDTSC) = rdtsc_path;
    name_int_map* nm = (name_int_map*)(uintptr_t)P_NAMES;
    memset(nm, 0, sizeof *nm);
    int nn = chance(90) ? ri(0, 16) : ri(-2, 20);
    for (int i = 0; i < 16; i++) {
        if (chance(80)) sprintf(nm->names[i], "%s%d", k_names[rnd() % 11], ri(0, 3));
        else memset(nm->names[i], 'A' + (int)(rnd() % 26), 16);
    }
    nm->count = nn;
    for (int i = 0; i < 128; i++) G32(P_PROFS + i * 4) = chance(50) ? rnd() : rnd() % 1000;
    G64(P_SYS) = chance(90) ? (uint64_t)rnd() * (1 + rnd() % 64) : 0;
    G64(P_OV_ACC) = (uint64_t)rnd() * (rnd() % 4);
    G64(P_OV_T) = c->tsc - (rnd() % 0x100000);
    memset((void*)(uintptr_t)P_STR_A, 'a', 16);
    memset((void*)(uintptr_t)P_STR_B, 'b', 16);
    // mem
    G32(M_HEAP) = chance(90) ? 0x00b0b000 : 0xffffffff;
    G32(M_SYNC) = (uint32_t)ri(0, 17);
    G8(M_MODE) = (uint8_t)(chance(50) ? 1 : 0);
    G32(M_CUR) = chance(50) ? rnd() % 100000 : rnd();
    G32(M_MAX) = chance(50) ? rnd() % 200000 : rnd();
    // tasks
    for (int i = 0; i < 8; i++) {
        VTask* t = (VTask*)(uintptr_t)(T_TABLE + i * 0x1c);
        bool used = i == 0 || chance(55);
        t->name = used ? (chance(90) ? rand_name() : arena_str(5)) : 0;
        t->thread_id = used ? (chance(85) ? g_tids[(i + (rnd() & 1)) % 4] : rnd()) : (chance(80) ? 0 : g_tids[rnd() % 4]);
        t->period = chance(60) ? ri(0, 40) : 0;
        t->handle = (HANDLE)(uintptr_t)(chance(80) ? 0x7100 + i : 0);
        t->parent = ri(0, 8);
        t->_14 = (int32_t)rnd();
        t->suspend_req = (uint8_t)(chance(20));
        t->kill = (uint8_t)(chance(15));
        t->flags = (uint8_t)(rnd() & 3);
        t->_1b = (uint8_t)rnd();
    }
    G32(T_SYNC) = (uint32_t)ri(0, 17);
    G32(T_MAIN) = (uint32_t)ri(0, 8);
    G32(T_TEST) = chance(10) ? 1 : 0;
    G32(T_TEST + 4) = rnd();
    // sync
    G32(Y_NSINGLE) = (uint32_t)(chance(85) ? ri(0, 16) : ri(-1, 18));
    G32(Y_NMULTI) = (uint32_t)(chance(85) ? ri(0, 16) : ri(-1, 18));
    G32(Y_MUTEX) = rand_cs_ptr();
    for (int i = 0; i < 16; i++) {
        single_entry(i + 1)->name = chance(95) ? arena_str(ri(0, 10)) : rand_name();
        single_entry(i + 1)->v = chance(60) ? (uint32_t)ri(1, 8) : chance(50) ? 0 : (uint32_t)ri(-1, 12);
        multi_entry(i + 1)->name = chance(95) ? arena_str(ri(0, 10)) : rand_name();
        multi_entry(i + 1)->v = chance(70) ? rand_cs_ptr() : 0;
    }
    if (chance(50) && single_entry(1)->name == 0) single_entry(1)->name = arena_str(0);
    G32(Y_NCS) = (uint32_t)(chance(85) ? ri(0, 16) : ri(-1, 18));
    for (int i = 0; i < 16; i++) {
        uint8_t* cs = (uint8_t*)(uintptr_t)(Y_CS + i * 0x1c);
        for (int k = 0; k < 24; k++) cs[k] = (uint8_t)rnd();
        cs[0x18] = (uint8_t)(chance(70) ? 1 : 0);
    }
    // except
    G32(X_LOG) = chance(70) ? 7 : 0;
    int mk = ri(0, 19);
    if (mk < 12) { G32(X_MAP) = (uint32_t)(uintptr_t)(g_arena + OFF_MAP); G32(X_MAP_SIZE) = (uint32_t)strlen((char*)(g_arena + OFF_MAP)); }
    else if (mk < 13) { G32(X_MAP) = (uint32_t)(uintptr_t)(g_file + g_sections_end); G32(X_MAP_SIZE) = g_file_size - g_sections_end; }
    else { G32(X_MAP) = 0; G32(X_MAP_SIZE) = rnd(); }
    G32(X_FILE) = rnd(); G32(X_MAPPING) = rnd(); G32(X_VIEW) = rnd();
    G8(X_ABENDED) = (uint8_t)chance(30);
    G32(X_COUNT) = (uint32_t)(chance(70) ? ri(0, 3) : ri(-1, 6));
    G32(X_PREV) = rnd();
    for (int i = 0; i < 4; i++) G32(X_HANDLERS + i * 4) = chance(40) ? 0 : chance(50) ? (uint32_t)(uintptr_t)st_exit0 : (uint32_t)(uintptr_t)st_exit1;
    memset((void*)(uintptr_t)X_BEST, 'q', 0x300);
    // vxd, kernel
    G32(V_HANDLE) = chance(60) ? 0xffffffffu : 0x55;
    G8(K_UP) = (uint8_t)chance(50);
    // bg
    G32(B_SYNC) = (uint32_t)ri(0, 17);
    G32(B_TASK) = (uint32_t)ri(1, 8);
    G32(B_N) = (uint32_t)(chance(85) ? ri(0, 15) : ri(-1, 16));
    for (int i = 0; i < 16; i++) { bg_hook(i)->fn = g_hooks[rnd() % 5]; bg_hook(i)->prio = ri(-1, 3); }
    // misc inputs
    for (int i = 0; i < 0x400; i++) misc(M_GETSTR)[i] = (uint8_t)(chance(4) ? '\n' : 'a' + rnd() % 26);
    misc(M_GETSTR)[0x3ff] = '\n';
}

// ---- the checks ----------------------------------------------------------------------------------------------------------------
static int rand_task_id() { return chance(90) ? ri(1, 8) : ri(-2, 10); }
static int rand_single() { return chance(90) ? ri(1, 16) : ri(-1, 18); }
static void check_prof() {
    CHECK_BOTH(map_name_rw, (name_int_map*)(uintptr_t)P_NAMES, 0, (const char*)(chance(50) ? rand_name() : ((name_int_map*)(uintptr_t)P_NAMES)->names[rnd() % 16]));
    const char* nm = rand_name();
    if (nm) CHECK_BOTH(prof_start_rw, nm);
    CHECK_BOTH(prof_stop_rw, chance(20) ? -1 : ri(-1, 16));
    {
        LARGE_INTEGER* a = (LARGE_INTEGER*)misc(M_LI_A);
        a->LowPart = rnd(); a->HighPart = (LONG)(chance(50) ? rnd() % 1000 : rnd());
        int d = chance(10) ? 0 : chance(50) ? ri(1, 100000) : (int)rnd();
        CHECK_BOTH(li_div_rw, a, d);
        LARGE_INTEGER* b = chance(20) ? a : (LARGE_INTEGER*)misc(M_LI_B);
        b->LowPart = rnd(); b->HighPart = (LONG)rnd();
        CHECK_BOTH(li_sub_rw, a, b);
        CHECK_BOTH(li_add_rw, a, b);
    }
    CHECK0_BOTH(noop_void_rw);
    CHECK_BOTH(noop_prof_start_rw, (const char*)arena_str(1));
    CHECK_BOTH(noop_prof_stop_rw, ri(-5, 5));
    CHECK0_BOTH(only_one_processor_rw);
    CHECK0_BOTH(ProfReset_rw);
    if (GI32(P_NNAMES) <= 16) CHECK0_BOTH(ProfResetAndReport_rw);
    if (GI32(P_NNAMES) <= 16) CHECK0_BOTH(prof_report_rw);
    CHECK_BOTH(timestr_rw, chance(10) ? 0 : chance(30) ? rnd() % 1000 : chance(50) ? rnd() % 4000000 : rnd());
    if (GI32(P_NNAMES) <= 40) CHECK_BOTH(sort_profs_rw, (int32_t*)misc(M_ORDER));
    {
        int32_t* o = (int32_t*)misc(M_ORDER);
        o[0] = ri(0, 15); o[1] = chance(90) ? ri(0, 15) : ri(-1, 17);
        if (o[0] >= 0 && o[0] < GI32(P_NNAMES) && o[1] >= 0 && o[1] < GI32(P_NNAMES)) CHECK_BOTH(compare_f_rw, (const void*)&o[0], (const void*)&o[1]);
        else if (chance(10)) CHECK_BOTH(compare_f_rw, (const void*)&o[0], (const void*)&o[1]);   // a null name: faults
    }
    CHECK0_BOTH(ProfEnd_rw);
    CHECK0_BOTH(PTimeNow_rw);
    CHECK_BOTH(pticks2msec_rw, (uint64_t)rnd() << ri(0, 32) | rnd());
    CHECK0_BOTH(pr_overhead_begin_rw);
    CHECK0_BOTH(pr_overhead_end_rw);
    CHECK_BOTH(pr_start_rw, ri(-1, 16));
    CHECK_BOTH(pr_end_rw, ri(-1, 16));
    CHECK0_BOTH(pr_rdtsc_lo_rw);
    CHECK0_BOTH(pr_sys_begin_rw);
    CHECK0_BOTH(pr_sys_end_rw);
    CHECK0_BOTH(rdtsc_rw);
    CHECK0_BOTH(isCpuidSupported_rw);
    CHECK0_BOTH(isRdtscSupported_rw);
    CHECK0_BOTH(prof_E7_rw);
    CHECK0_BOTH(prof_E5_rw);
    CHECK0_BOTH(prof_E2_rw);
    CHECK0_BOTH(ProfBegin_rw);
}
static void check_mem() {
    CHECK0_BOTH(mem_E2_rw);
    CHECK_BOTH(do_nothing_with_rw, (int)rnd());
    CHECK_BOTH(MemSetAllocMode_rw, (uint8_t)(chance(50) ? 0 : rnd()));
    CHECK0_BOTH(mem_use_vxd_allocator_rw);
    CHECK_BOTH(MemBegin_rw, chance(50) ? 0x1400000u : rnd());
    void* blocks[4] = {};
    for (int i = 0; i < 4; i++) {
        int n = chance(80) ? ri(0, 3000) : chance(50) ? ri(-4, -1) : ri(3000, 0x30000);
        uint32_t top = ctrl()->heap_top;
        CHECK_BOTH(MemAlloc_rw, n);
        // the block MemAlloc returned (if any): the arena bump heap's previous top + 4, when the VxD path wasn't used
        if (ctrl()->heap_top != top) blocks[i] = g_arena + OFF_HEAP + top + 4;
    }
    for (int i = 3; i >= 0; i--)
        if (blocks[i] && chance(70)) {
            if (chance(50)) CHECK_BOTH(MemFree_rw, blocks[i]);
            else CHECK_BOTH(operator_delete_rw, blocks[i]);
        }
    CHECK_BOTH(touch_memory_rw, (void*)misc(0), (int)rnd());
    CHECK_BOTH(operator_new_rw, rnd() % 1000);
    CHECK_BOTH(MemAttrib_rw, (void*)misc(0), ri(0, 3));
    CHECK0_BOTH(MemVerifyAll_rw);
    CHECK_BOTH(MemVerify_rw, (void*)(blocks[0] ? blocks[0] : misc(0)));
    CHECK_BOTH(MemDump_rw, ri(0, 3));
    CHECK0_BOTH(MemEnd_rw);
}
static void check_task() {
    CHECK0_BOTH(task_E2_rw);
    CHECK_BOTH(get_task_info_rw, ri(-3, 12));
    CHECK_BOTH(alloc_task_rw, rand_name(), ri(-1, 8));
    CHECK0_BOTH(task_exception_handler_rw);
    CHECK0_BOTH(timer_test_f_rw);
    CHECK_BOTH(free_task_rw, rand_task_id());
    CHECK_BOTH(TaskCreate_rw, rand_name(), (uint32_t)(uintptr_t)st_thread_fn);
    CHECK_BOTH(thread_vector_rw, (void*)st_thread_fn);
    CHECK0_BOTH(current_taskid_rw);
    CHECK_BOTH(TaskDestroy_rw, rand_task_id());
    CHECK_BOTH(TaskSetTimer_rw, rand_name(), (uint32_t)(uintptr_t)st_timer_cb, ri(0, 100));
    CHECK_BOTH(TaskKillTimer_rw, rand_task_id());
    CHECK_BOTH(TaskSetPriority_rw, ri(-15, 15));
    CHECK0_BOTH(current_taskname_rw);
    CHECK_BOTH(TaskSuspend_rw, rand_task_id());
    CHECK_BOTH(TaskKill_rw, rand_task_id());
    CHECK_BOTH(TaskResume_rw, rand_task_id());
    CHECK0_BOTH(TaskShouldISuspend_rw);
    CHECK0_BOTH(TaskShouldIDie_rw);
    CHECK0_BOTH(TaskGetID_rw);
    CHECK_BOTH(TaskSleep_rw, ri(0, 300));
    CHECK0_BOTH(TaskSuspendMe_rw);
    CHECK0_BOTH(TaskKillMe_rw);
    CHECK_BOTH(TaskIsSuspended_rw, rand_task_id());
    CHECK_BOTH(TaskIsDead_rw, rand_task_id());
    CHECK_BOTH(TaskGetName_rw, rand_task_id());
    // the timer loop: this thread is a timer task; the callback asks it to die after cb_limit ticks
    {
        int k = ri(1, 8);
        VTask* t = (VTask*)(uintptr_t)(T_TABLE + (k - 1) * 0x1c);
        if (chance(90)) { t->thread_id = ctrl()->tid; t->kill = 0; if (!t->name) t->name = arena_str(3); }
        for (int i = 0; i < 8; i++)                     // no other entry claims this thread
            if (i != k - 1 && ((VTask*)(uintptr_t)(T_TABLE + i * 0x1c))->thread_id == ctrl()->tid) ((VTask*)(uintptr_t)(T_TABLE + i * 0x1c))->thread_id = 0;
        ctrl()->cb_task = (uint32_t)k;
        ctrl()->cb_calls = 0;
        CHECK_BOTH(timer_vector_rw, (void*)st_timer_cb);
    }
    CHECK0_BOTH(TaskBegin_rw);
    CHECK0_BOTH(TaskEnd_rw);
}
static void check_sync() {
    CHECK0_BOTH(sync_E2_rw);
    CHECK0_BOTH(sync_E5_rw);
    CHECK0_BOTH(SyncBegin_rw);
    const char* n = rand_name();
    if (n) CHECK_BOTH(SingleBegin_rw, n);
#ifndef FIX_TESTS
    CHECK_BOTH(SingleEnter_rw, rand_single(), (const char*)0, ri(0, 100));
    CHECK_BOTH(unsafe_check_rw, rand_single(), (const char*)0, 0);
    CHECK_BOTH(SingleLeave_rw, rand_single(), (const char*)0, 0);
#endif                                                   // (the fix build: fix_unsafe_check)
    CHECK_BOTH(SingleEnd_rw, rand_single(), (const char*)0, 0);
    n = rand_name();
    if (n) CHECK_BOTH(MultiBegin_rw, n);
    CHECK_BOTH(MultiEnter_rw, rand_single(), (const char*)0, 0);
    CHECK_BOTH(MultiLeave_rw, rand_single(), (const char*)0, 0);
    CHECK_BOTH(MultiEnd_rw, rand_single(), (const char*)0, 0);
    CHECK0_BOTH(SyncMutexAlloc_rw);
    CHECK0_BOTH(alloc_cs_rw);
    uint32_t cs = rand_cs_ptr();
    if (cs) CHECK_BOTH(SyncMutexLock_rw, (void*)(uintptr_t)cs);
    if (cs) CHECK_BOTH(SyncMutexUnlock_rw, (void*)(uintptr_t)cs);
    {
        volatile uint32_t* p = (volatile uint32_t*)misc(M_CSPTR - 4);
        *p = rand_cs_ptr();
        if (*p) CHECK_BOTH(SyncMutexFree_rw, p);
    }
    CHECK0_BOTH(SyncStrobeAlloc_rw);
    CHECK_BOTH(SyncStrobeWait_rw, (HANDLE)(uintptr_t)(0xe0e0 + ri(0, 3)));
    CHECK_BOTH(SyncStrobeFlash_rw, (HANDLE)(uintptr_t)(0xe0e0 + ri(0, 3)));
    {
        volatile uint32_t* h = (volatile uint32_t*)misc(M_HANDLE);
        *h = 0xe0e0 + ri(0, 3);
        CHECK_BOTH(SyncStrobeFree_rw, h);
    }
    {
        volatile long* p = (volatile long*)misc(M_LAME);
        *p = chance(70) ? 0 : chance(50) ? 1 : (long)rnd();
        CHECK_BOTH(SyncLameMutexLock_rw, p);
        CHECK_BOTH(SyncLameMutexUnlock_rw, p);
    }
    CHECK_BOTH(cs_tag_rw, (void*)(uintptr_t)(Y_CS + (uint32_t)ri(0, 15) * 0x1c), 0);
    CHECK0_BOTH(SyncEnd_rw);
}
// a fake exception: record, context and a stack of return addresses (just after an E8 call in 0x400000..0x480000)
static std::vector<uint32_t> g_call_sites;
static EXCEPTION_POINTERS* fake_exception(uint32_t max_depth) {
    EXCEPTION_POINTERS* ep = (EXCEPTION_POINTERS*)misc(M_EP);
    EXCEPTION_RECORD* rec = (EXCEPTION_RECORD*)misc(M_REC);
    CONTEXT* ctx = (CONTEXT*)misc(M_CTX);
    static const uint32_t k_codes[] = {0xc0000005, 0xc000008e, 0xc0000091, 0xc0000094, 0x80000003, 0xc00000fd, 0xc0000090, 0x12345678};
    rec->ExceptionCode = k_codes[rnd() % 8];
    rec->ExceptionAddress = (void*)(uintptr_t)(chance(70) ? g_call_sites[rnd() % g_call_sites.size()] - ri(0, 40) : rnd());
    ep->ExceptionRecord = rec;
    ep->ContextRecord = chance(90) ? ctx : 0;
    uint32_t depth = chance(10) ? 0 : (uint32_t)ri(1, (int)max_depth);
    uint32_t* top = (uint32_t*)(g_arena + OFF_STACK_TOP);
    for (uint32_t i = 1; i <= 300; i++) {
        int k = ri(0, 9);
        top[-(int)i] = k < 4 ? g_call_sites[rnd() % g_call_sites.size()] : k < 6 ? 0x400000 + (rnd() & 0x7ffff) : rnd();
    }
    ctx->Esp = (DWORD)(uintptr_t)(top - depth);
    // now and then a frame exactly at the walk's window edge: its call's target 0x18f / 0x190 / 0x191 below the
    // previous frame (the exception address)
    if (depth > 0 && chance(40)) {
        uint32_t r = g_call_sites[rnd() % g_call_sites.size()];
        uint32_t target = r + *(int32_t*)(uintptr_t)(r - 4);
        uint32_t last = target + 0x18f + (uint32_t)ri(0, 2);
        if (last >= 0x400000 && last <= 0x480000) {
            rec->ExceptionAddress = (void*)(uintptr_t)last;
            top[-(int)depth] = r;
        }
    }
    return ep;
}
static void check_except() {
    CHECK0_BOTH(except_E2_rw);
    {
        char* buf = (char*)misc(M_STRBUF);
        static const uint32_t k_fmts[] = {0x4e6b7c, 0x4e6b88, 0x4e6ba4, 0x4e6bb4};
        uint32_t f = k_fmts[rnd() % 4];
        CHECK_BOTH(DumpExceptionString_rw, (int)(chance(80) ? 7 : 0), buf, S(f), rnd(), (uint32_t)(uintptr_t)arena_str(ri(0, 10)), 0u, 0u);
    }
    CHECK0_BOTH(get_mapfile_ptr_rw);
#ifndef FIX_TESTS
    CHECK0_BOTH(ExceptBegin_rw);                         // (the fix build checks the fixed one on its own: fix_except_begin)
#endif
    {
        static const uint32_t k_codes[] = {0x80000001, 0x80000002, 0x80000003, 0x80000004, 0x80000005, 0x80000000,
            0xc0000005, 0xc0000006, 0xc000001d, 0xc0000025, 0xc0000026, 0xc000008c, 0xc0000093, 0xc0000094,
            0xc0000096, 0xc0000097, 0xc00000fd, 0xc000013a, 0xc000013b, 0xc0000004, 0xc0000024, 0xc000001e};
        uint32_t code = chance(60) ? k_codes[rnd() % (sizeof k_codes / 4)] : chance(50) ? 0xc000008d + (rnd() % 0xb0) : rnd();
        CHECK_BOTH(except2name_rw, code);
        CHECK_BOTH(is_floating_exception_rw, code);
        // and every code the switch names, and its neighbours
        static const uint32_t k_named[] = {0x80000001, 0x80000002, 0x80000003, 0x80000004, 0xc0000005, 0xc0000006,
            0xc000001d, 0xc0000025, 0xc0000026, 0xc000008c, 0xc000008d, 0xc000008e, 0xc000008f, 0xc0000090, 0xc0000091,
            0xc0000092, 0xc0000093, 0xc0000094, 0xc0000095, 0xc0000096, 0xc00000fd, 0xc000013a};
        for (uint32_t c : k_named) {
            uint32_t v = c + (uint32_t)ri(-1, 1) * (chance(70) ? 0 : 1);
            CHECK(except2name_rw, v);
            CHECK(is_floating_exception_rw, v);
        }
    }
    CHECK_BOTH(find_func_in_mapfile_rw, (int)(chance(80) ? 0x401000 + (rnd() & 0xfffff) : rnd()));
    {
        char* src = (char*)misc(M_GETSTR) + ri(0, 0x3f0);
        CHECK_BOTH(get_string_rw, (char*)misc(M_STRBUF), src);
    }
    {
        // (what's installed may later run: ExceptTrigger, my_handler -- only real void() functions)
        const uint32_t k_fns[] = {0x414920, (uint32_t)(uintptr_t)st_exit0, (uint32_t)(uintptr_t)st_exit1};
        uint32_t fn = k_fns[rnd() % 3];
        CHECK_BOTH(ExceptInstallExitHandler_rw, fn);
        CHECK_BOTH(ExceptUninstallExitHandler_rw, chance(70) ? fn : k_fns[rnd() % 3]);
    }
    CHECK0_BOTH(ExceptTrigger_rw);
    CHECK_BOTH(ExceptDiv0Crashes_rw, (uint8_t)(chance(50) ? 0 : rnd()));
    CHECK_BOTH(ExceptSinglePrecision_rw, (uint8_t)(chance(50) ? 0 : rnd()));
    for (int i = 0; i < 4; i++)                          // (ExceptTrigger above cleared them)
        G32(X_HANDLERS + i * 4) = chance(40) ? 0 : chance(50) ? (uint32_t)(uintptr_t)st_exit0 : (uint32_t)(uintptr_t)st_exit1;
    // with the real 12,000-line map every frame found is a full search: a shallow stack then
    CHECK_BOTH(my_handler_rw, fake_exception(G32(X_MAP) == (uint32_t)(uintptr_t)(g_file + g_sections_end) ? 3 : 300));
    CHECK0_BOTH(abend_rw);
    CHECK0_BOTH(ExceptEnd_rw);
}
static void check_vxd_kernel_bg_time() {
    CHECK0_BOTH(vxdLoad_rw);
    CHECK0_BOTH(lockdown_exe_rw);
    CHECK_BOTH(lockdown_memory_rw, (void*)misc(0), rnd());
    {
        static const char* const k_secs[] = {".text", ".rdata", ".data", ".DATA", ".rsrc", ".textbss", ""};
        char* s = (char*)misc(M_STRBUF);
        memset(s, 0, 16);
        strcpy(s, k_secs[rnd() % 7]);
        CHECK_BOTH(should_lockdown_section_rw, (const uint8_t*)s);
    }
    CHECK0_BOTH(vxdIsLoaded_rw);
    CHECK_BOTH(vxdMemAlloc_rw, ri(-4, 5000));
    CHECK_BOTH(call_vxd_rw, ri(0, 6), (void*)misc(M_LI_A), 4, (void*)misc(M_LI_B), chance(50) ? 4 : 0);
    CHECK_BOTH(vxdMemFree_rw, (void*)misc(M_LI_A));
    CHECK0_BOTH(vxdRegisterPhysicsThread_rw);
    CHECK0_BOTH(vxdUnregisterPhysicsThread_rw);
    CHECK0_BOTH(vxdGetPhysicsThreadEIP_rw);
    CHECK0_BOTH(vxdUnload_rw);
    CHECK0_BOTH(kernel_E2_rw);
    CHECK0_BOTH(bg_E2_rw);
    CHECK0_BOTH(time_E2_rw);
    CHECK0_BOTH(BGBegin_rw);
    CHECK0_BOTH(bg_vector_rw);
    CHECK0_BOTH(empty_hook_rw);
    CHECK_BOTH(BGHook_rw, (uint32_t)(uintptr_t)g_hooks[rnd() % 5], ri(-1, 4));
    CHECK_BOTH(ASSERT_MSG_rw, ri(0, 1), S(0x4eab60));
    CHECK_BOTH(BGUnhook_rw, (uint32_t)(uintptr_t)g_hooks[rnd() % 5]);
    CHECK0_BOTH(bg_vector_rw);
    CHECK0_BOTH(BGEnd_rw);
    CHECK_BOTH(TimeGetTimeOfDay_rw, (TimeOfDay*)misc(M_TOD));
    CHECK0_BOTH(KernelBegin_rw);
    CHECK0_BOTH(KernelEnd_rw);
}
// everything else first, then a whole kernel's life from a fresh start
static void check_kernel_life() {
    // a fresh process: the statics as the image has them, the $E initialisers run
    for (const GRange& g : g_globals) {
        uint32_t rva = g.at - 0x400000, off = 0;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_file + ((IMAGE_DOS_HEADER*)g_file)->e_lfanew);
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (uint32_t i = 0; i < g.n; i++, rva++) {
            uint8_t v = 0;
            for (int s = 0; s < nt->FileHeader.NumberOfSections; s++)
                if (rva >= sec[s].VirtualAddress && rva < sec[s].VirtualAddress + sec[s].SizeOfRawData) v = g_file[sec[s].PointerToRawData + rva - sec[s].VirtualAddress];
            ((uint8_t*)(uintptr_t)g.at)[i] = v;
            (void)off;
        }
    }
    ctrl()->ncpu = chance(30) ? 1 : 4;
    ctrl()->log_begin_ok = 1;
    ctrl()->panic_returns = 1;
    CHECK0_BOTH(prof_E7_rw);
    CHECK0_BOTH(sync_E5_rw);
    CHECK0_BOTH(KernelBegin_rw);
    int n = ri(0, 6);
    for (int i = 0; i < n; i++) CHECK_BOTH(MemAlloc_rw, ri(0, 5000));
    CHECK_BOTH(BGHook_rw, (uint32_t)(uintptr_t)g_hooks[rnd() % 4], ri(0, 1));
    CHECK0_BOTH(bg_vector_rw);
    CHECK0_BOTH(PTimeNow_rw);
    CHECK_BOTH(prof_start_rw, (const char*)arena_str(ri(0, 10)));
    CHECK_BOTH(SingleBegin_rw, (const char*)arena_str(ri(0, 10)));
    CHECK_BOTH(TaskCreate_rw, (const char*)arena_str(ri(0, 10)), (uint32_t)(uintptr_t)st_thread_fn);
    CHECK0_BOTH(KernelEnd_rw);
}

static void run_world() {
    random_world();
    switch (g_wno % 6) {
    case 0: check_prof(); break;
    case 1: check_mem(); break;
    case 2: check_task(); break;
    case 3: check_sync(); break;
    case 4: check_except(); break;
    default: check_vxd_kernel_bg_time(); break;
    }
    if (g_wno % 5 == 0) { random_world(); check_kernel_life(); }
}

// ---- the live pass: real threads, locks, clock and heap ----------------------------------------------------------------------------
static volatile long g_hook_calls;
static volatile uint16_t g_hook_cw;
static volatile int g_hook_prio;
static volatile DWORD g_hook_tid;
static void __cdecl live_hook() {
    uint16_t w;
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fnstcw %0" : "=m"(w));
#else
    __asm fnstcw w
#endif
    g_hook_cw = w;
    g_hook_prio = GetThreadPriority(GetCurrentThread());
    g_hook_tid = GetCurrentThreadId();
    InterlockedIncrement(&g_hook_calls);
}
struct LiveResult { uint8_t began; long hooks; uint16_t cw; int prio; uint32_t mem_cur, mem_max, ntasks_named, nmulti, nsingle, ncs, bg_n; uint8_t up_after; bool ok; };
static LiveResult live_run(bool rewrite) {
    LiveResult r = {};
    random_world();
    for (const GRange& g : g_globals) {                // the image's statics
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_file + ((IMAGE_DOS_HEADER*)g_file)->e_lfanew);
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (uint32_t i = 0; i < g.n; i++) {
            uint32_t rva = g.at - 0x400000 + i;
            uint8_t v = 0;
            for (int s = 0; s < nt->FileHeader.NumberOfSections; s++)
                if (rva >= sec[s].VirtualAddress && rva < sec[s].VirtualAddress + sec[s].SizeOfRawData) v = g_file[sec[s].PointerToRawData + rva - sec[s].VirtualAddress];
            ((uint8_t*)(uintptr_t)g.at)[i] = v;
        }
    }
    ctrl()->log_begin_ok = 1;
    ctrl()->app_instance = 0x400000;
    for (int i = 0; i < 256; i++) g_script[i] = 1;     // the kept stubs succeed (GetModuleFileName, CreateFile...)
    rdtsc_sites(false);
    set_slots(true);
    g_live = true;
    g_hook_calls = 0;
    if (rewrite) chain_patch();
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] {
            FN(Void_t, 0x004133a0)();                        // prof $E7
            FN(Void_t, 0x00415270)();                        // sync $E5
            r.began = FN(Flag_t, 0x00418620)();              // KernelBegin
            if (r.began) {
                FN(void(__cdecl*)(void(__cdecl*)(), int), 0x004189f0)(live_hook, 0);   // BGHook, as the physics is
                Sleep(500);
                FN(void(__cdecl*)(void(__cdecl*)()), 0x00418ac0)(live_hook);           // BGUnhook
                r.hooks = g_hook_calls;
                r.cw = g_hook_cw;
                r.prio = g_hook_prio;
                void* b[5];
                for (int i = 0; i < 5; i++) b[i] = FN(void*(__cdecl*)(int), 0x004140e0)(100 + i * 1000);
                for (int i = 0; i < 5; i += 2) FN(PtrArg_t, 0x00414300)(b[i]);
                r.mem_cur = G32(M_CUR);
                r.mem_max = G32(M_MAX);
                for (int i = 0; i < 8; i++) r.ntasks_named += ((VTask*)(uintptr_t)(T_TABLE + i * 0x1c))->name != 0;
                r.nmulti = G32(Y_NMULTI);
                r.nsingle = G32(Y_NSINGLE);
                r.ncs = G32(Y_NCS);
                r.bg_n = G32(B_N);
                for (int i = 1; i < 5; i += 2) FN(PtrArg_t, 0x00414300)(b[i]);
                FN(Void_t, 0x00418710)();                    // KernelEnd
                r.up_after = G8(K_UP);
            }
            r.ok = true;
        }, fault_filter)) {
        printf("  live pass (%s) faulted: %08x at %08x\n", rewrite ? "rewrites" : "originals", g_fault_code, g_fault_at);
    }
#else
    __try {
        FN(Void_t, 0x004133a0)();                        // prof $E7
        FN(Void_t, 0x00415270)();                        // sync $E5
        r.began = FN(Flag_t, 0x00418620)();              // KernelBegin
        if (r.began) {
            FN(void(__cdecl*)(void(__cdecl*)(), int), 0x004189f0)(live_hook, 0);   // BGHook, as the physics is
            Sleep(500);
            FN(void(__cdecl*)(void(__cdecl*)()), 0x00418ac0)(live_hook);           // BGUnhook
            r.hooks = g_hook_calls;
            r.cw = g_hook_cw;
            r.prio = g_hook_prio;
            void* b[5];
            for (int i = 0; i < 5; i++) b[i] = FN(void*(__cdecl*)(int), 0x004140e0)(100 + i * 1000);
            for (int i = 0; i < 5; i += 2) FN(PtrArg_t, 0x00414300)(b[i]);
            r.mem_cur = G32(M_CUR);
            r.mem_max = G32(M_MAX);
            for (int i = 0; i < 8; i++) r.ntasks_named += ((VTask*)(uintptr_t)(T_TABLE + i * 0x1c))->name != 0;
            r.nmulti = G32(Y_NMULTI);
            r.nsingle = G32(Y_NSINGLE);
            r.ncs = G32(Y_NCS);
            r.bg_n = G32(B_N);
            for (int i = 1; i < 5; i += 2) FN(PtrArg_t, 0x00414300)(b[i]);
            FN(Void_t, 0x00418710)();                    // KernelEnd
            r.up_after = G8(K_UP);
        }
        r.ok = true;
    } __except (fault_filter(GetExceptionInformation())) {
        printf("  live pass (%s) faulted: %08x at %08x\n", rewrite ? "rewrites" : "originals", g_fault_code, g_fault_at);
    }
#endif
    if (rewrite) chain_unpatch();
    g_live = false;
    set_slots(false);
    rdtsc_sites(true);
    fpu_reset();
    return r;
}
#ifdef FIX_TESTS
// ExceptBegin's FIX: the crash log is <race.exe's folder>\log\except.log, the folder made; with no exe path, or one too
// long for the file table's 0x100-byte names, c:\except.log as before. Checked on the stubs' log: the folder
// CreateDirectoryA was asked for (a stub: nothing is made) and the name FileCreate got.
static char g_long_exe[300];
static int fix_except_begin() {
    struct Case { const char* exe; const char* dir; const char* file; };
    const char* long_exe = g_long_exe;                   // "C:\ddd...\race.exe": a 248-character folder
    memcpy(g_long_exe, "C:\\", 3);
    memset(g_long_exe + 3, 'd', 244);
    strcpy(g_long_exe + 247, "\\race.exe");
    const Case cases[] = {
        {"C:\\Games\\Viper Racing\\race.exe", "C:\\Games\\Viper Racing\\log", "C:\\Games\\Viper Racing\\log\\except.log"},
        {"D:\\race.exe", "D:\\log", "D:\\log\\except.log"},
        {"", 0, "c:\\except.log"},                                            // GetModuleFileNameA fails
        {long_exe, 0, "c:\\except.log"},                           // too long for log\except.log in 0x100
    };
    int bad = 0;
    for (const Case& k : cases) {
        random_world();
        for (int i = 0; i < 256; i++) g_script[i] = 1;   // (FileCreate succeeds: File 7)
        g_modname_override = k.exe;
        g_pass = 1; g_nlog[1] = 0; g_si = 0;
#if defined(__GNUC__) && !defined(__clang__)
        if (vp_try([&] { ExceptBegin_rw(); })) {
            printf("  FAIL ExceptBegin (exe \"%.40s\") faulted\n", k.exe);
            bad++;
        }
#else
        __try {
            ExceptBegin_rw();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            printf("  FAIL ExceptBegin (exe \"%.40s\") faulted\n", k.exe);
            bad++;
        }
#endif
        fpu_reset();
        uint32_t dirs = 0, dir_hash = 0, creates = 0, create_hash = 0;
        for (int i = 0; i < g_nlog[1] && i < LOGN; i++) {
            const LogEntry& e = g_log[1][i];
            if (e.kind == L_CREATEDIR) dirs++, dir_hash = e.a[0];
            if (e.kind == L_FCREATE && !creates++) create_hash = e.a[0];
        }
        const bool ok = creates == 1 && create_hash == hash_str(k.file) && G32(X_LOG) == 7 &&
                        (k.dir ? dirs == 1 && dir_hash == hash_str(k.dir) : dirs == 0);
        if (!ok) {
            bad++;
            printf("  FAIL ExceptBegin (exe \"%.40s\"): %u CreateDirectory (%s), %u FileCreate (%s), the log file %u\n", k.exe, dirs,
                   k.dir && dir_hash == hash_str(k.dir) ? "right" : "wrong", creates, create_hash == hash_str(k.file) ? "right" : "wrong",
                   G32(X_LOG));
        }
    }
    g_modname_override = 0;
    printf("fix build: ExceptBegin's crash log in <race.exe's folder>\\log\\: %d cases, %d wrong\n", (int)(sizeof cases / sizeof *cases), bad);
    return bad;
}

// unsafe_check's FIX: a thread entering a module another thread owns (or none) no longer panics; the check writes
// nothing. On random worlds and handles: where the original returns, the rewrite does the same thing (the same arena,
// globals and stub calls); where the original panics, the rewrite returns and the arena and globals are as they were.
// _SingleEnter / _SingleLeave (which call unsafe_check through its address) likewise, with the fixed unsafe_check in
// place (the chain), against the originals.
typedef void(__cdecl* Lock3_fix_t)(int, const char*, int);
static int fix_one(Lock3_fix_t f, int h) {
    uint64_t ret;
    uint16_t cw;
    auto run = [&]() -> uint64_t { f(h, 0, 0); return 0; };
    fpu_start(0x027f);
    const int r = guarded(run, &ret, &cw);
    fpu_reset();
    return r;
}
static int fix_unsafe_check() {
    static Snapshot s0, a;
    int bad = 0, panics = 0, same = 0;
    struct Fn { const char* name; uint32_t orig; Lock3_fix_t fixed; } fns[] = {
        {"unsafe_check", 0x00415020, &unsafe_check_rw},
        {"_SingleEnter", 0x00415000, &SingleEnter_rw},
        {"_SingleLeave", 0x00415070, &SingleLeave_rw},
    };
    for (int w = 0; w < 600; w++) {
        random_world();
        ctrl()->panic_returns = 0;                       // (LogPanic ends the call, as in the game)
        const Fn& fn = fns[w % 3];
        const int h = ri(1, 16);
        save(s0);
        g_pass = 0; g_nlog[0] = 0; g_si = 0;
        const int ro = fix_one((Lock3_fix_t)(uintptr_t)fn.orig, h);
        save(a);
        load(s0);
        uint8_t saved[5];
        memcpy(saved, (void*)0x00415020, 5);
        patch_jmp(0x00415020, (void*)&unsafe_check_rw);  // the fixed check where the callers reach it
        g_pass = 1; g_nlog[1] = 0; g_si = 0;
        const int rn = fix_one(fn.fixed, h);
        memcpy((void*)0x00415020, saved, 5);
        static Snapshot b;
        save(b);
        const bool logs_same = g_nlog[0] == g_nlog[1] && !memcmp(g_log[0], g_log[1], sizeof(LogEntry) * (g_nlog[0] < LOGN ? g_nlog[0] : LOGN));
        // the owner check's panic ("\"%s\" called module %s owned by \"%s\"", 0x4e68cc); another (current_taskid's
        // "Current thread is unlisted", in a world whose thread isn't listed) happens in both, the same way
        auto owner_panic = [](int pass) {
            for (int i = 0; i < g_nlog[pass] && i < LOGN; i++)
                if (g_log[pass][i].kind == L_PANIC && g_log[pass][i].a[0] == 0x004e68ccu) return true;
            return false;
        };
        bool ok;
        if (ro == R_PANIC && owner_panic(0)) {
            panics++;
            ok = rn == R_RAN && !owner_panic(1) && !memcmp(b.arena + sizeof(Ctrl), s0.arena + sizeof(Ctrl), ARENA_SIZE - sizeof(Ctrl)) &&
                 !memcmp(b.globals, s0.globals, GLOBALS_BYTES);
        } else {
            same++;
            ok = rn == ro && !memcmp(b.arena, a.arena, ARENA_SIZE) && !memcmp(b.globals, a.globals, GLOBALS_BYTES) && logs_same;
        }
        if (!ok) {
            if (bad++ < 5) printf("  FAIL %s(%d), world %d: the original %s, the fixed one %s%s\n", fn.name, h, w, k_result[ro],
                                  k_result[rn], rn == ro ? " but differently" : "");
        }
        load(a);
    }
    printf("fix build: unsafe_check's owner check: %d calls panicked in the original (none in the fixed: nothing written), "
           "%d ran the same; %d wrong\n", panics, same, bad);
    return bad;
}
#endif
static int live_check() {
    LiveResult o = live_run(false), n = live_run(true);
    printf("live: %-10s began %d, BG hook ran %ld times in 0.5 s (cw %04x, priority %d), mem cur %u max %u, tasks %u, multi %u,"
           " single %u, cs %u, bg hooks %u, up after %d\n", "originals", o.began, o.hooks, o.cw, o.prio, o.mem_cur, o.mem_max, o.ntasks_named,
           o.nmulti, o.nsingle, o.ncs, o.bg_n, o.up_after);
    printf("live: %-10s began %d, BG hook ran %ld times in 0.5 s (cw %04x, priority %d), mem cur %u max %u, tasks %u, multi %u,"
           " single %u, cs %u, bg hooks %u, up after %d\n", "rewrites", n.began, n.hooks, n.cw, n.prio, n.mem_cur, n.mem_max, n.ntasks_named,
           n.nmulti, n.nsingle, n.ncs, n.bg_n, n.up_after);
    bool same = o.ok && n.ok && o.began == n.began && o.cw == n.cw && o.prio == n.prio && o.mem_cur == n.mem_cur && o.mem_max == n.mem_max &&
                o.ntasks_named == n.ntasks_named && o.nmulti == n.nmulti && o.nsingle == n.nsingle && o.ncs == n.ncs && o.bg_n == n.bg_n &&
                o.up_after == n.up_after && n.hooks * 2 > o.hooks && n.hooks < o.hooks * 2 && o.hooks > 5;   // (Sleep's 15.6 ms granularity: 19..31 ticks)
    printf("live: %s\n", same ? "same" : "DIFFERENT");
    return same ? 0 : 1;
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
#if defined(__GNUC__) && !defined(__clang__) && !defined(_UCRT)
    // (mingw on msvcrt.dll: no _set_abort_behavior there -- and its abort() never calls Windows Error Reporting)
#else
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    _set_invalid_parameter_handler(invalid_parameter);
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
#if defined(__GNUC__) && !defined(__clang__)
    struct Unhandled {                                   // (GCC doesn't convert a lambda to a WINAPI function pointer)
        static LONG WINAPI filter(EXCEPTION_POINTERS* e) {
            printf("unhandled exception %08lx at %p (world %d, pass %d)\n", e->ExceptionRecord->ExceptionCode,
                   e->ExceptionRecord->ExceptionAddress, g_wno, g_pass);
            ExitProcess(3);
            return 0;
        }
    };
    SetUnhandledExceptionFilter(Unhandled::filter);
#else
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e) -> LONG {
        printf("unhandled exception %08lx at %p (world %d, pass %d)\n", e->ExceptionRecord->ExceptionCode,
               e->ExceptionRecord->ExceptionAddress, g_wno, g_pass);
        ExitProcess(3);
        return 0;
    });
#endif
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 1200;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
#ifdef RACE_EXE
    strcpy(exe, RACE_EXE);                               // (mutation tests build a copy elsewhere)
#else
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_krn_core.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
#endif
    if (!load_race_exe(exe)) return 2;
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_arena || ((uintptr_t)g_arena & 0xffff)) { printf("the arena isn't 64 KB aligned\n"); return 2; }
    install_stubs();
    // return addresses just after an E8 call, in 0x400000..0x480000 (my_handler's stack walk)
    for (uint32_t a = 0x401005; a < 0x480000; a++)
        if (*(uint8_t*)(uintptr_t)(a - 5) == 0xe8) {
            uint32_t t = a + *(int32_t*)(uintptr_t)(a - 4);
            // (not one inside a prologue the chain patches: the walk reads the code bytes, which differ then)
            bool patched = a - 5 < 0x418912 + 5 && a > 0x418912;
            for (ChainReg* r = ChainReg::head(); r && !patched; r = r->next) patched = a - 5 < r->at + 5 && a > r->at;
            if (t >= 0x401000 && t < 0x4db000 && !patched) g_call_sites.push_back(a);
        }
    uint32_t gb = 0;
    for (const GRange& g : g_globals) gb += g.n;
    if (gb != GLOBALS_BYTES) { printf("GLOBALS_BYTES is %u, should be %u\n", (unsigned)GLOBALS_BYTES, gb); return 2; }
    bool verbose = g_verbose = GetEnvironmentVariableA("VP_VERBOSE", 0, 0) != 0;
    for (g_wno = 0; g_wno < worlds; g_wno++) {
        if (verbose) printf("world %d\n", g_wno);
        run_world();
    }
    int failed = 0;
    long calls = 0;
    printf("%-40s %7s %7s %7s %7s %7s %7s %7s %7s %7s\n", "function", "calls", "changed", "replay", "panics", "exits", "hangs", "faults", "differ", "fp-miss");
    for (Stat& st : g_stats) {
        printf("%-40s %7ld %7ld %7ld %7ld %7ld %7ld %7ld %7ld %7ld\n", st.name.c_str(), st.calls, st.changed, st.replay_only, st.panics,
               st.exits, st.hangs, st.faults, st.fails, st.fp_fails);
        if (st.fails || st.fp_fails) failed++;
        calls += st.calls;
    }
    if (GetEnvironmentVariableA("VP_COVER", 0, 0))         // what each function's original reached
        for (Stat& st : g_stats) {
            if (st.name.find("[chain]") != std::string::npos) continue;
            printf("  %s:", st.name.c_str());
            for (int k = 1; k < (int)(sizeof g_log_names / sizeof *g_log_names); k++)
                if (st.seen[k >> 6] >> (k & 63) & 1) printf(" %s", g_log_names[k]);
            printf("\n");
        }
    int nfn = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) nfn++;
    printf("%d rewrites; %d worlds, %ld checks; %d of %d check kinds differ or escape their footprint\n", nfn, worlds, calls, failed,
           (int)g_stats.size());
    int live = live_check();
#ifdef FIX_TESTS
    failed += fix_except_begin();
    failed += fix_unsafe_check();
#endif
    return failed || live ? 1 : 0;
}
