// world_krn_util.cpp -- the kernel utilities (hook/krn_util.cpp: key, mouse, scan, joy, useful, base64, table, node,
// random, pool, mstream, slerp, bag, clpboard, html) against the originals, outside the game (docs/PORTING.md, step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_krn_util.cpp
//        /Fo%TEMP%\k3w\ /Fe%TEMP%\k3w\world_krn_util.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_krn_util.exe [episodes] [seed] [seeds-for-the-long-random-run] [draws-per-seed]
//          (environment: VP_K3_VERBOSE=1 prints each episode, =2 each check; VP_K3_DUMP=1 prints the HTML lines;
//          VP_K3_DPDUMP=<episode> traces a pools episode's debug pool. A check running over a minute -- an original
//          looping forever on a cyclic list -- is reported by a watchdog and the harness ends itself.)
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. Everything the functions call outside this group is a
// stub that logs its call and answers from a per-call script: the Single / Multi locks, Win32Idle (which, as in the
// game, can deliver key and mouse messages: it calls KeyQueueChar / MouseQueueEvent by address), Win32GetWindow,
// LogReport, LogPanic (which doesn't return: the stub raises an exception that ends the call, in both passes),
// MemAlloc (a bump heap in the arena, now and then failing), MemFree, operator delete, PTimeNow, the file functions,
// the resources (ResourceGet hands out a string table built in the arena), and Windows through the game's import
// slots (MapVirtualKeyA, the clipboard and Global* functions: the user's real clipboard is never touched), and
// DirectInput (fake COM objects whose methods log their arguments and answer from the script). The game's own C
// runtime runs from the image (stricmp, strncpy, vsprintf, _CIfmod, __ftol).
//
// Each check runs the original, keeps the arena, the image statics and the log, restores them, runs the rewrite and
// compares the return value, the arena, the statics, the log and any fault / panic; every byte the original changed
// must lie inside the rewrite's footprint unless it's replay_only. Checks run at 24- or 53-bit precision, a quarter
// of them with overflow and divide-by-zero exceptions unmasked (the physics thread's mode). Each is made twice from
// one state: "isolated" (the group's other functions run as the originals from the image) and "chain" (every rewrite
// of the group hooked into the image for the rewrite's pass). The episodes: random states and operation sequences --
// queues filled past full, corrupted heads, pools exhausted and freed, streams overflowed and sought anywhere, string
// tables sorted and searched, base64 round trips and corruptions, quaternions with aliasing and special values, bags
// overfilled, the clipboard and HTML paths, DirectInput's joystick set-up with every failure. Then a long run of the
// random generator: many seeds, each seeded both ways and drawn from many times at both precisions, the value and the
// whole state compared after every draw.
//
// The game's CRT runs here without its start-up: _cfltcvt_init is called (its %f converters), KERNEL32's imports are
// resolved, and its fatal-error paths (_amsg_exit, _NMSG_WRITE, _FF_MSGBANNER, _fptrap, __crtMessageBoxA) are stubs
// that end the child quietly; SetErrorMode keeps Windows' own error boxes away. Nothing may open a dialog.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
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
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
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
void Footprint::object(void* obj, const char* what) { add(obj, 0x20, what); }

#include "../hook/krn_util.cpp"

// ---- random values ------------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x6b3a9c15u;
static uint32_t rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t ubits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float rf(float lo, float hi) { return lo + (hi - lo) * uni(); }
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static uint32_t hash_bytes(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static uint32_t hash_str(const char* s, size_t max = 0x1000) { return s ? hash_bytes(s, strnlen(s, max)) : 0xdead; }
static float special_float() {
    switch (rnd() % 12) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return fbits(0x7f800000);
    case 3: return fbits(0xff800000);
    case 4: return fbits(0x7fc00000 | (rnd() & 0x3fffff));
    case 5: return fbits(0x7f800001 | (rnd() & 0x3ffffe));         // signalling NaN
    case 6: return fbits(rnd() & 0x807fffff);                      // denormal
    case 7: return fbits(0x7f7fffff);
    case 8: return rf(-1e30f, 1e30f);
    case 9: return rf(-1e-30f, 1e-30f);
    default: return fbits(rnd());
    }
}

// ---- the original, loaded at 0x400000 (as test/fuzz.cpp) --------------------------------------------------------
static bool load_race_exe(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("can't open %s\n", path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* file = (uint8_t*)malloc(n);
    fread(file, 1, n, f);
    fclose(f);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(file + ((IMAGE_DOS_HEADER*)file)->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x362de68c) { printf("%s isn't the v1.0 race.exe\n", path); return false; }
    uint8_t* base = (uint8_t*)VirtualAlloc((void*)0x400000, nt->OptionalHeader.SizeOfImage, MEM_COMMIT,
                                           PAGE_EXECUTE_READWRITE);
    if (base != (uint8_t*)0x400000) { printf("0x400000 isn't reserved for race.exe in this process\n"); return false; }
    memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        memcpy(base + sec[i].VirtualAddress, file + sec[i].PointerToRawData,
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize || !sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData
                                                                                         : sec[i].Misc.VirtualSize);
    // KERNEL32's imports resolved (the game's CRT); the ones the functions call are replaced by stubs below
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); dir.Size && d->Name; d++) {
        const char* dll = (const char*)(base + d->Name);
        if (_stricmp(dll, "KERNEL32.dll")) continue;
        HMODULE m = GetModuleHandleA("kernel32.dll");
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
            FARPROC f = GetProcAddress(m, fn);
            if (f) iat->u1.Function = (DWORD)(uintptr_t)f;
        }
    }
    free(file);
    return true;
}
static int relaunch() {
    SetEnvironmentVariableA("VP_K3_CHILD", "1");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) return 2;
    if (!VirtualAllocEx(pi.hProcess, (void*)0x400000, 0x300000, MEM_RESERVE, PAGE_EXECUTE_READWRITE))
        printf("couldn't reserve 0x400000 in the test process (%lu)\n", GetLastError());
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}
static void patch_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// the chain: every rewrite of the group hooked into the image (for a rewrite's pass)
struct ChainSave { uint32_t at; uint8_t b[5]; };
static ChainSave g_chain_save[256];
static int g_nchain;
static void chain_patch() {
    g_nchain = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) {
        ChainSave& s = g_chain_save[g_nchain++];
        s.at = r->at;
        memcpy(s.b, (void*)r->at, 5);
        patch_jmp(r->at, r->fn);
    }
}
static void chain_unpatch() {
    for (int i = g_nchain - 1; i >= 0; i--) memcpy((void*)g_chain_save[i].at, g_chain_save[i].b, 5);
    g_nchain = 0;
}

// ---- the arena -------------------------------------------------------------------------------------------------------
enum {
    ARENA_SIZE = 0x80000,
    OFF_CTRL = 0x0000, OFF_OUT = 0x0400, OFF_POOLS = 0x0800, OFF_OBJS = 0x1000, OFF_QUAT = 0x2000,
    OFF_STR = 0x2400, OFF_B64IN = 0x3000, OFF_B64OUT = 0x4800, OFF_TABLE = 0x6000, TABLE_MAX = 0x14000,
    OFF_SDATA = 0x1a000, SDATA_MAX = 0x4000, OFF_CLIP = 0x1e000, OFF_GALLOC = 0x1f000, OFF_COM = 0x20000,
    OFF_INST = 0x20400, OFF_HEAP = 0x21000, HEAP_SIZE = 0x5f000,
};
static uint8_t* g_arena;
static uint8_t* at(uint32_t off) { return g_arena + off; }

struct Ctrl {
    uint32_t heap_top, mark, alloc_fail, next_handle;
    uint32_t file_ok, st_ok, st_version, clip_open, clip_has, clip_lock, clip_empty, clip_alloc;
    uint32_t hr_fail;            // pct of COM calls that fail
    uint32_t idle_msgs;          // Win32Idle delivers key / mouse messages
    uint32_t mvk;                // MapVirtualKeyA's salt
    uint32_t galloc_n;
};
static Ctrl* ctrl() { return (Ctrl*)at(OFF_CTRL); }

// the image statics the functions read or write, snapshotted with the arena
struct GRange { uint32_t at, n; const char* what; };
static const GRange g_globals[] = {
    {0x00508080, 0x1890, "statics 0x508080..0x509910 (scan, key, mouse, joy, base64 table, node, random)"},
    {0x004eb400, 0x100, "base64 alphabet / flag, node statics, random flag"},
    {0x00503dd0, 8, "html file"},
};
enum { GLOBALS_BYTES = 0x1890 + 0x100 + 8 };
static const char* global_name(uint32_t a) {
    struct { uint32_t at, n; const char* nm; } t[] = {
        {0x00508088, 0x100, "scan[]"}, {0x00508188, 4, "DirectInput"}, {0x0050818c, 4, "keyboard device"},
        {0x005082e8, 4, "key single"}, {0x005082ec, 2, "key meta"}, {0x005082f0, 0x40, "key buf[]"},
        {0x00508330, 0x200, "key off[]"}, {0x00508530, 4, "key head"}, {0x00508534, 4, "key tail"},
        {0x00508548, 8, "mouse x, y"}, {0x00508550, 0x100, "mouse buf[]"}, {0x00508650, 4, "mouse state"},
        {0x00508654, 4, "mouse single"}, {0x00508658, 4, "mouse head"}, {0x00508660, 4, "mouse tail"},
        {0x00508f08, 0x200, "joystick name"}, {0x00509108, 4, "joystick"}, {0x0050910c, 4, "joy DirectInput"},
        {0x00509114, 8, "joy effects"}, {0x0050911c, 4, "joystick2"}, {0x00509660, 0x100, "base64 table"},
        {0x00509760, 4, "node multi"}, {0x00509768, 4, "ran c"}, {0x00509770, 0x184, "ran u[]"},
        {0x005098f4, 4, "ran cd"}, {0x005098f8, 4, "ran cm"}, {0x005098fc, 4, "ran multi"}, {0x00509904, 4, "ran i97"},
        {0x00509908, 4, "ran j97"}, {0x004eb444, 1, "base64 to build"}, {0x004eb498, 4, "node pool"},
        {0x004eb49c, 4, "node max"}, {0x004eb4a0, 4, "nodes reserved"}, {0x004eb4f8, 1, "ran seeded"},
        {0x00503dd4, 4, "html file"},
    };
    for (auto& x : t)
        if (a >= x.at && a < x.at + x.n) return x.nm;
    return "?";
}

// ---- the stubs' script and log ---------------------------------------------------------------------------------------
struct LogEntry { uint32_t kind, a[5]; };
enum { LOGN = 1 << 14 };
static LogEntry g_log[2][LOGN];
static int g_nlog[2], g_pass;
static bool g_quiet;                    // the long random run: no logging
static uint32_t g_script[256];
static int g_si;
static uint32_t script() { return g_script[g_si++ & 255]; }
static void logn(uint32_t kind, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0) {
    if (g_quiet) return;
    if (g_nlog[g_pass] < LOGN) g_log[g_pass][g_nlog[g_pass]] = {kind, {a0, a1, a2, a3, a4}};
    g_nlog[g_pass]++;
}
// a pointer as the log sees it: the arena and the image as they are; the stack as a marker
static uint32_t P(const void* p) {
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    if ((uint8_t*)p >= (uint8_t*)tib->StackLimit && (uint8_t*)p < (uint8_t*)tib->StackBase) return 0x57ac0000u;
    return (uint32_t)(uintptr_t)p;
}
static uint32_t mark(uint32_t salt) {
    Ctrl* c = ctrl();
    c->mark = c->mark * 0x9e3779b1u + salt + (uint32_t)g_nlog[g_pass];
    return c->mark;
}

#define LOG_KINDS(X)                                                                                             \
    X(S_BEGIN) X(S_ENTER) X(S_LEAVE) X(S_END) X(M_BEGIN) X(M_ENTER) X(M_LEAVE) X(M_END) X(IDLE) X(GETWIN)        \
    X(REPORT) X(PANIC) X(MEMALLOC) X(MEMFREE) X(DELETE) X(PTIME) X(FWRITE) X(FREAD) X(FCREATE) X(FCLOSE)          \
    X(FPRINTF) X(RGET) X(RFORGET) X(RBEGIN) X(REND) X(MVK) X(CB_OPEN) X(CB_GET) X(G_LOCK) X(G_UNLOCK) X(CB_CLOSE) \
    X(CB_EMPTY) X(G_ALLOC) X(CB_SET) X(COM_QI) X(COM_RELEASE) X(COM_CREATEDEV) X(COM_SETPROP) X(COM_ACQUIRE)      \
    X(COM_UNACQUIRE) X(COM_SETFMT) X(COM_COOP) X(COM_EFFECT) X(COM_BAD)
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

// ---- stubs -----------------------------------------------------------------------------------------------------------
static int __cdecl st_s_begin(const char* n) { logn(L_S_BEGIN, P(n)); return (int)(++ctrl()->next_handle); }
static void __cdecl st_s_enter(int h, const char* f, int l) { logn(L_S_ENTER, h, P(f), l); }
static void __cdecl st_s_leave(int h, const char* f, int l) { logn(L_S_LEAVE, h, P(f), l); }
static void __cdecl st_s_end(int h, const char* f, int l) { logn(L_S_END, h, P(f), l); }
static int __cdecl st_m_begin(const char* n) { logn(L_M_BEGIN, P(n)); return (int)(0x100 + ++ctrl()->next_handle); }
static void __cdecl st_m_enter(int h, const char* f, int l) { logn(L_M_ENTER, h, P(f), l); }
static void __cdecl st_m_leave(int h, const char* f, int l) { logn(L_M_LEAVE, h, P(f), l); }
static void __cdecl st_m_end(int h, const char* f, int l) { logn(L_M_END, h, P(f), l); }
// Win32Idle: the window's messages -- now and then a character or a mouse event, delivered through the game's own
// functions by address (the originals, or the rewrites in a chain)
static void __cdecl st_idle() {
    logn(L_IDLE);
    if (!ctrl()->idle_msgs) return;
    const uint32_t s = script();
    for (uint32_t i = 0; i < (s & 3); i++) ((void(__cdecl*)(uint32_t, uint32_t))0x00413dd0)(script() & 0xffff, script() & 0xff);
    for (uint32_t i = 0; i < ((s >> 2) & 3); i++) {
        const uint32_t t = script();
        ((void(__cdecl*)(int32_t, int32_t, int32_t, int32_t))0x00414530)(t % 3 ? 6 : (int32_t)(t % 8), (int32_t)script() % 700,
                                                                       (int32_t)script() % 500, (int32_t)(script() & 7));
    }
}
static void* __cdecl st_getwin() { logn(L_GETWIN); return (void*)(uintptr_t)0x00beef00; }

// messages: the formats' arguments decoded (a %s logged as its pointer: always the arena or the image here)
static int fmt_args(const char* fmt) {
    switch ((uint32_t)(uintptr_t)fmt) {
    case 0x004eac74: case 0x004eacf4: case 0x004ead2c: case 0x00503e4c: case 0x004eb448: case 0x004eb560:
    case 0x004eb5f0: case 0x004eb614: case 0x00503d98: case 0x00503db4: return 1;
    case 0x004eb464: return 3;
    default: return 0;
    }
}
static void log_msg(uint32_t kind, const char* fmt, va_list ap) {
    uint32_t a[3] = {0, 0, 0};
    const int n = fmt_args(fmt);
    for (int i = 0; i < n; i++) a[i] = P(va_arg(ap, void*));
    if ((uint32_t)(uintptr_t)fmt == 0x004eac74) a[1] = hash_str((const char*)(uintptr_t)a[0], 0x104);
    logn(kind, P(fmt), a[0], a[1], a[2]);
}
enum { PANIC_CODE = 0xE0005050 };
static void __cdecl st_report(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_msg(L_REPORT, fmt, ap);
    va_end(ap);
}
static void __cdecl st_panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_msg(L_PANIC, fmt, ap);
    va_end(ap);
    RaiseException(PANIC_CODE, 0, 0, 0);
}
static void* __cdecl st_memalloc(int n) {
    Ctrl* c = ctrl();
    const uint32_t s = script();
    uint8_t* r = 0;
    if ((s % 100) >= c->alloc_fail && n >= 0 && (uint32_t)n <= HEAP_SIZE && c->heap_top + (uint32_t)n <= HEAP_SIZE) {
        r = at(OFF_HEAP + c->heap_top);
        c->heap_top = (c->heap_top + (uint32_t)(n ? n : 8) + 7) & ~7u;     // MemAlloc(0): a block of its own
        if (!n) memset(r, 0, 8);
        uint32_t m = mark(n);
        for (int i = 0; i < n; i++) { r[i] = (uint8_t)(m >> 24); m = m * 1103515245u + 12345u; }
    }
    logn(L_MEMALLOC, (uint32_t)n, P(r));
    return r;
}
static void __cdecl st_memfree(void* p) { logn(L_MEMFREE, P(p)); }
static void __cdecl st_delete(void* p) { logn(L_DELETE, P(p)); }
static int __cdecl st_ptime() {
    const uint32_t s = script();
    const int v = s % 5 == 0 ? (int)s : (int)(s % 4000000);
    logn(L_PTIME, (uint32_t)v);
    return v;
}
static uint8_t __cdecl st_fwrite(int f, const void* d, int n) {
    logn(L_FWRITE, (uint32_t)f, P(d), (uint32_t)n, n > 0 && n <= 0x10000 ? hash_bytes(d, (size_t)n) : 0);
    return 1;
}
static uint8_t __cdecl st_fread(int f, void* d, int n) {
    logn(L_FREAD, (uint32_t)f, P(d), (uint32_t)n);
    if (n > 0 && n <= 0x10000) {
        uint32_t m = mark(n);
        for (int i = 0; i < n; i++) { ((uint8_t*)d)[i] = (uint8_t)(m >> 24); m = m * 1103515245u + 12345u; }
    }
    return 1;
}
static int __cdecl st_fcreate(const char* n) { logn(L_FCREATE, P(n), hash_str(n)); return ctrl()->file_ok ? 7 : 0; }
static void __cdecl st_fclose(int* h) { logn(L_FCLOSE, P(h), (uint32_t)*h); *h = 0; }
static void __cdecl st_fprintf(int f, char* scratch, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint32_t a = 0, b = 0, c = 0;
    if ((uint32_t)(uintptr_t)fmt == 0x00503e74) {
        const char* line = va_arg(ap, const char*);
        a = hash_str(line, 0x400);
        if (getenv("VP_K3_DUMP")) printf("    [pass %d] line \"%.80s\"\n", g_pass, line);
    } else if ((uint32_t)(uintptr_t)fmt == 0x00503e7c) {
        a = P(va_arg(ap, const char*));
        b = hash_str(va_arg(ap, const char*), 0x400);
        c = P(va_arg(ap, const char*));
    }
    va_end(ap);
    logn(L_FPRINTF, (uint32_t)f, P(scratch), P(fmt), a, b ^ c * 31);
}
static void* __cdecl st_rget(const char* name, uint32_t type, uint32_t* ver, int* a, uint8_t* b, uint8_t* c) {
    logn(L_RGET, P(name), type, P(ver), P(a), P(b) ^ P(c));
    *ver = ctrl()->st_version;
    return ctrl()->st_ok ? at(OFF_TABLE) : 0;
}
static uint8_t __cdecl st_rforget(void* p) { logn(L_RFORGET, P(p)); return 1; }
static uint8_t __cdecl st_rbegin(const char* a, const char* b) { logn(L_RBEGIN, P(a), P(b)); return 1; }
static void __cdecl st_rend() { logn(L_REND); }
// Windows, in the game's import slots (__stdcall)
enum { CLIP_HANDLE = 0x00c11b00, GALLOC_HANDLE = 0x00a110c0 };
static uint32_t __stdcall st_mvk(uint32_t code, uint32_t type) {
    logn(L_MVK, code, type);
    return (code * 0x9e3779b1u + ctrl()->mvk) ^ type;
}
static int __stdcall st_cb_open(void* w) { logn(L_CB_OPEN, P(w)); return (int)ctrl()->clip_open; }
static void* __stdcall st_cb_get(uint32_t f) { logn(L_CB_GET, f); return ctrl()->clip_has ? (void*)CLIP_HANDLE : 0; }
static void* __stdcall st_g_lock(void* h) {
    logn(L_G_LOCK, P(h));
    if (h == (void*)CLIP_HANDLE) return ctrl()->clip_lock ? at(OFF_CLIP) : 0;
    if (h == (void*)GALLOC_HANDLE) return at(OFF_GALLOC);
    return 0;
}
static int __stdcall st_g_unlock(void* h) { logn(L_G_UNLOCK, P(h)); return 0; }
static int __stdcall st_cb_close() { logn(L_CB_CLOSE); return 1; }
static int __stdcall st_cb_empty() { logn(L_CB_EMPTY); return (int)ctrl()->clip_empty; }
static void* __stdcall st_g_alloc(uint32_t fl, uint32_t n) {
    logn(L_G_ALLOC, fl, n);
    ctrl()->galloc_n = n;
    return ctrl()->clip_alloc && n <= 0x1000 ? (void*)GALLOC_HANDLE : 0;
}
static void* __stdcall st_cb_set(uint32_t f, void* h) {
    logn(L_CB_SET, f, P(h), h == (void*)GALLOC_HANDLE ? hash_bytes(at(OFF_GALLOC), ctrl()->galloc_n) : 0);
    return h;
}
// DirectInput: fake COM objects in the arena, one vtable for all
struct FakeCom { void** vtbl; uint32_t id, refs, _p; };
static uint32_t com_hr() {
    const uint32_t s = script();
    if ((s % 100) >= ctrl()->hr_fail) return 0;
    static const uint32_t codes[] = {0x80004001u, 0x80004005u, 0x80070057u, 0x8007001eu, 0x80040154u, 0x12345678u, 1u};
    return codes[(s >> 8) % 7];
}
static FakeCom* com_obj(int i) { return (FakeCom*)at(OFF_COM + 16 * i); }
static long __stdcall st_com_qi(FakeCom* self, const void* iid, void** out) {
    const uint32_t hr = com_hr();
    logn(L_COM_QI, self->id, P(iid), P(out), hr);
    if (!hr) *out = com_obj(3);
    return (long)hr;
}
static unsigned long __stdcall st_com_release(FakeCom* self) { logn(L_COM_RELEASE, self->id, --self->refs); return self->refs; }
static long __stdcall st_com_createdev(FakeCom* self, const void* guid, void** out, void* outer) {
    const uint32_t hr = com_hr();
    logn(L_COM_CREATEDEV, self->id, P(guid), hash_bytes(guid, 16), P(outer), hr);
    *out = hr ? 0 : com_obj(2);
    return (long)hr;
}
static long __stdcall st_com_setprop(FakeCom* self, const void* guid, const uint32_t* hdr) {
    const uint32_t hr = com_hr();
    logn(L_COM_SETPROP, self->id, P(guid), hash_bytes(hdr, 0x14), hdr[4], hr);
    return (long)hr;
}
static long __stdcall st_com_acquire(FakeCom* self) { logn(L_COM_ACQUIRE, self->id); return (long)com_hr(); }
static long __stdcall st_com_unacquire(FakeCom* self) { logn(L_COM_UNACQUIRE, self->id); return 0; }
static long __stdcall st_com_setfmt(FakeCom* self, const void* f) { const uint32_t hr = com_hr(); logn(L_COM_SETFMT, self->id, P(f), hr); return (long)hr; }
static long __stdcall st_com_coop(FakeCom* self, void* w, uint32_t fl) { const uint32_t hr = com_hr(); logn(L_COM_COOP, self->id, P(w), fl, hr); return (long)hr; }
static long __stdcall st_com_effect(FakeCom* self, const void* guid, const uint32_t* eff, void** out, void* outer) {
    const uint32_t hr = com_hr();
    uint32_t h = hash_bytes(eff, 0x20) ^ eff[11] * 7;       // dwSize .. cAxes, cbTypeSpecificParams
    const uint32_t na = eff[7] < 4 ? eff[7] : 4;
    h = h * 31 + hash_bytes((void*)(uintptr_t)eff[8], 4 * na);
    h = h * 31 + hash_bytes((void*)(uintptr_t)eff[9], 4 * na);
    h = h * 31 + *(uint32_t*)(uintptr_t)eff[12] + eff[10];
    logn(L_COM_EFFECT, self->id, P(guid), h, P(out) ^ P(outer), hr);
    if (!hr) *out = com_obj(5 + (out == (void**)0x00509118));
    return (long)hr;
}
static void __cdecl st_com_bad() { logn(L_COM_BAD); RaiseException(0xE0000BAD, 0, 0, 0); }
static void* g_com_vtbl[20];

// the CRT's fatal paths: log and leave quietly
static void __cdecl st_crt_fatal() { printf("the game's CRT reached a fatal-error path\n"); fflush(stdout); ExitProcess(3); }
static int __cdecl st_crt_msgbox(const char*, const char*, unsigned) { printf("the game's CRT tried a message box\n"); ExitProcess(3); }

static void install_stubs() {
    struct { uint32_t at; void* to; } p[] = {
        {0x00414f30, (void*)&st_s_begin}, {0x00415000, (void*)&st_s_enter}, {0x00415070, (void*)&st_s_leave},
        {0x00415090, (void*)&st_s_end}, {0x004150a0, (void*)&st_m_begin}, {0x00415180, (void*)&st_m_enter},
        {0x004151d0, (void*)&st_m_leave}, {0x00415220, (void*)&st_m_end}, {0x00412bf0, (void*)&st_idle},
        {0x00412be0, (void*)&st_getwin}, {0x00411150, (void*)&st_report}, {0x004112b0, (void*)&st_panic},
        {0x004140e0, (void*)&st_memalloc}, {0x00414300, (void*)&st_memfree}, {0x00414390, (void*)&st_delete},
        {0x00413b40, (void*)&st_ptime}, {0x00411a30, (void*)&st_fwrite}, {0x004118b0, (void*)&st_fread},
        {0x004115f0, (void*)&st_fcreate}, {0x00411850, (void*)&st_fclose}, {0x00411b40, (void*)&st_fprintf},
        {0x00419fa0, (void*)&st_rget}, {0x0041a450, (void*)&st_rforget}, {0x00419640, (void*)&st_rbegin},
        {0x0041a5a0, (void*)&st_rend},
    };
    for (auto& x : p) patch_jmp(x.at, x.to);
    struct { uint32_t slot; void* to; } iat[] = {
        {0x005d769c, (void*)&st_mvk}, {0x005d7680, (void*)&st_cb_open}, {0x005d76d0, (void*)&st_cb_get},
        {0x005d7560, (void*)&st_g_lock}, {0x005d759c, (void*)&st_g_unlock}, {0x005d7684, (void*)&st_cb_close},
        {0x005d768c, (void*)&st_cb_empty}, {0x005d755c, (void*)&st_g_alloc}, {0x005d7688, (void*)&st_cb_set},
    };
    for (auto& x : iat) *(void**)(uintptr_t)x.slot = x.to;
    patch_jmp(0x004cf730, (void*)&st_crt_fatal);
    patch_jmp(0x004d45b0, (void*)&st_crt_fatal);
    patch_jmp(0x004d4570, (void*)&st_crt_fatal);
    patch_jmp(0x004d5c10, (void*)&st_crt_fatal);
    patch_jmp(0x004d6850, (void*)&st_crt_msgbox);
    ((void(__cdecl*)())0x004ce150)();            // _cfltcvt_init: the game's printf %f
    for (int i = 0; i < 20; i++) g_com_vtbl[i] = (void*)&st_com_bad;
    g_com_vtbl[0] = (void*)&st_com_qi;
    g_com_vtbl[2] = (void*)&st_com_release;
    g_com_vtbl[3] = (void*)&st_com_createdev;
    g_com_vtbl[6] = (void*)&st_com_setprop;
    g_com_vtbl[7] = (void*)&st_com_acquire;
    g_com_vtbl[8] = (void*)&st_com_unacquire;
    g_com_vtbl[11] = (void*)&st_com_setfmt;
    g_com_vtbl[13] = (void*)&st_com_coop;
    g_com_vtbl[18] = (void*)&st_com_effect;
}

// ---- running one call both ways ----------------------------------------------------------------------------------------
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

struct Stat { std::string name; long calls, changed, fails, fp_fails, faults, panics, replay_only; };
static std::vector<Stat> g_stats;
static Stat& stat(const std::string& name) {
    for (Stat& s : g_stats)
        if (s.name == name) return s;
    g_stats.push_back({name, 0, 0, 0, 0, 0, 0, 0});
    return g_stats.back();
}

struct Snapshot { uint8_t arena[ARENA_SIZE]; uint8_t globals[GLOBALS_BYTES]; };
static Snapshot g_start, g_after, g_saved;
static Footprint g_fp;
static int g_eno;
static const char* g_phase = "";
static bool g_chain;
static long g_mismatch_total;
static bool g_abort;                                     // the original faulted: the episode's state is gone
static bool g_verbose;

static void snap_globals(uint8_t* p) {
    for (const GRange& g : g_globals) { memcpy(p, (void*)(uintptr_t)g.at, g.n); p += g.n; }
}
static void save(Snapshot& s) {
    memcpy(s.arena, g_arena, ARENA_SIZE);
    snap_globals(s.globals);
}
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
static const char* region_of(uint32_t off, uint32_t* rel) {
    struct { uint32_t at; const char* name; } r[] = {
        {OFF_CTRL, "ctrl"}, {OFF_OUT, "outputs"}, {OFF_POOLS, "pools"}, {OFF_OBJS, "objects"}, {OFF_QUAT, "quats"},
        {OFF_STR, "strings"}, {OFF_B64IN, "base64 in"}, {OFF_B64OUT, "base64 out"}, {OFF_TABLE, "string table"},
        {OFF_SDATA, "stream data"}, {OFF_CLIP, "clipboard"}, {OFF_GALLOC, "global alloc"}, {OFF_COM, "com"},
        {OFF_INST, "device instance"}, {OFF_HEAP, "heap"},
    };
    int k = 0;
    for (int i = 0; i < (int)(sizeof r / sizeof *r); i++)
        if (off >= r[i].at) k = i;
    *rel = off - r[k].at;
    return r[k].name;
}

static __declspec(noinline) void stack_fill(uint32_t pat) {
    volatile uint32_t buf[0x4000];
    for (int i = 0; i < 0x4000; i++) buf[i] = pat;
}
static uint64_t g_last_ret;
static int g_last_result;                                // 0 ran, 1 panicked, 2 faulted (the original's pass)
static uint32_t g_fault_code, g_fault_at;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_at = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    // an unmasked x87 exception is still pending: the handler's first `wait` would raise it again
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
#else
    __asm fninit
#endif
    return EXCEPTION_EXECUTE_HANDLER;
}
template <typename Run> static int guarded(Run& run, uint64_t* ret) {
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] { *ret = run(); }, fault_filter)) {
        return g_fault_code == PANIC_CODE ? 1 : 2;
    }
    return 0;
#else
    __try {
        *ret = run();
        return 0;
    } __except (fault_filter(GetExceptionInformation())) {
        return g_fault_code == PANIC_CODE ? 1 : 2;
    }
#endif
}
static void fpu_mode(unsigned pc, bool unmask) {
    unsigned cw;
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
#else
    __asm fninit
#endif
    _clearfp();
    _controlfp_s(&cw, pc, _MCW_PC);
    if (unmask) _controlfp_s(&cw, _EM_INEXACT | _EM_UNDERFLOW | _EM_INVALID | _EM_DENORMAL, _MCW_EM);
}
static void fpu_reset() {
    unsigned cw;
#if defined(__GNUC__) && !defined(__clang__)
    __asm__ volatile("fninit" ::: VP_X87_CLOBBERS);
#else
    __asm fninit
#endif
    _clearfp();
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    _controlfp_s(&cw, _MCW_EM, _MCW_EM);
}
#if defined(__GNUC__) && !defined(__clang__)
// (GCC: declared here, before check() -- GCC binds a template's non-dependent names where the template is defined)
static volatile long g_check_serial;
static volatile const char* g_check_name = "";
#define COVS(X) X(key_full) X(key_dropped_disabled) X(key_meta) X(key_corrupt) X(mouse_full) X(mouse_coalesced)     \
    X(pool_exhausted) X(pool_panic) X(pool_zero_count) X(dpool_overalloc) X(node_grab_fail) X(node_release_fail)      \
    X(stream_overflow) X(stream_getstring_over_max) X(stream_getstring_uninit) X(table_sorted) X(table_sort_changed)  \
    X(table_find_present_missed) X(table_found) X(b64_roundtrip_ok) X(b64_bad) X(b64_over_max) X(slerp_negated_a)     \
    X(slerp_small_sin) X(slerp_div0) X(bag_full) X(clip_get) X(clip_put) X(joy_stop) X(joy_fail) X(random_out_of_range)
enum {
#define X(n) C_##n,
    COVS(X)
#undef X
    NCOV
};
static const char* const k_cov_names[] = {
#define X(n) #n,
    COVS(X)
#undef X
};
static long g_cov[NCOV];
#endif
template <typename Run, typename Fp> static void check(const char* fname, Run run, Fp footprint) {
    std::string name = fname;
    if (g_chain) name += " [chain]";
    if (g_verbose) printf("  %s\n", name.c_str());
    Stat& st = stat(name);
    st.calls++;
    g_check_serial++;
    g_check_name = fname;
    for (int i = 0; i < 256; i++) g_script[i] = rnd();
    const uint32_t pat = chance(50) ? rnd() : chance(50) ? 0x3f800000u : 0;
    const unsigned pc = chance(50) ? _PC_53 : _PC_24;
    const bool unmask = chance(25);
    save(g_start);
    g_fp.n = 0; g_fp.replay_only = 0; g_fp.pure = false;
    footprint(g_fp);
    // Random(int,int): in the game the inner Random is the recorder's hook, which feeds a shadow check's rewrite
    // pass the original's value, so the state isn't its footprint; here the inner call draws: the state is
    if (!strcmp(fname, "Random(int,int)")) fp_ran_state(g_fp);
    if (g_fp.replay_only) st.replay_only++;
    uint64_t ro = 0, rn = 0;
    // the original
    fpu_mode(pc, unmask);
    g_pass = 0; g_nlog[0] = 0; g_si = 0;
    auto r0 = [&]() { return run(true); };
    auto r1 = [&]() { return run(false); };
    stack_fill(pat);
    int fo = guarded(r0, &ro);
    const uint32_t fcode0 = g_fault_code;
    fpu_reset();
    save(g_after);
    if (memcmp(g_after.arena + sizeof(Ctrl), g_start.arena + sizeof(Ctrl), ARENA_SIZE - sizeof(Ctrl)) ||
        memcmp(g_after.globals, g_start.globals, GLOBALS_BYTES)) st.changed++;
    g_last_ret = ro;
    g_last_result = fo;
    // the rewrite
    load(g_start);
    g_pass = 1; g_nlog[1] = 0; g_si = 0;
    fpu_mode(pc, unmask);
    if (g_chain) chain_patch();
    stack_fill(pat);
    int fn = guarded(r1, &rn);
    const uint32_t fcode1 = g_fault_code;
    if (g_chain) chain_unpatch();
    fpu_reset();
    if (fo == 1) st.panics++;
    if (fo == 2 && fcode0 == 0xC000008E && !strncmp(fname, "Slerp", 5)) g_cov[C_slerp_div0]++;
    if (fo == 2 || fn == 2) {
        st.faults++;
        g_abort = true;
        if (st.faults <= 2) printf("  (fault in %s, episode %d, %s: code %08x at %08x)\n", name.c_str(), g_eno, g_phase, g_fault_code, g_fault_at);
        if (fo != fn || fcode0 != fcode1) {
            g_mismatch_total++;
            if (st.fails++ < 4) printf("MISMATCH %s (episode %d, %s): original %s (%08x), rewrite %s (%08x)\n", name.c_str(), g_eno, g_phase,
                                       fo == 2 ? "faulted" : fo ? "panicked" : "ran", fcode0, fn == 2 ? "faulted" : fn ? "panicked" : "ran", fcode1);
        }
        load(g_after);
        return;
    }
    uint8_t now_globals[GLOBALS_BYTES];
    snap_globals(now_globals);
    int nlog0 = g_nlog[0] < LOGN ? g_nlog[0] : LOGN;
    bool bad = fo != fn || (fo == 0 && ro != rn) || memcmp(g_after.arena, g_arena, ARENA_SIZE) ||
               memcmp(g_after.globals, now_globals, GLOBALS_BYTES) || g_nlog[0] != g_nlog[1] ||
               memcmp(g_log[0], g_log[1], sizeof(LogEntry) * nlog0);
    if (bad) {
        g_mismatch_total++;
        if (st.fails++ < 4) {
            printf("MISMATCH %s (episode %d, %s, pc %s%s)%s\n", name.c_str(), g_eno, g_phase, pc == _PC_24 ? "24" : "53",
                   unmask ? ", unmasked" : "", fo != fn ? " (panicked in one pass only)" : "");
            if (ro != rn) printf("  return: original %016llx, rewrite %016llx\n", ro, rn);
            int shown = 0;
            for (uint32_t i = 0; i < ARENA_SIZE && shown < 10; i += 4)
                if (memcmp(g_after.arena + i, g_arena + i, 4)) {
                    uint32_t a, b, s, rel;
                    memcpy(&a, g_after.arena + i, 4); memcpy(&b, g_arena + i, 4); memcpy(&s, g_start.arena + i, 4);
                    const char* r = region_of(i, &rel);
                    printf("  %s+0x%x: start %08x, original %08x (%.9g), rewrite %08x (%.9g)\n", r, rel, s, a, fbits(a), b, fbits(b));
                    shown++;
                }
            const uint8_t* pa = g_after.globals;
            const uint8_t* pb = now_globals;
            for (const GRange& g : g_globals) {
                int k = 0;
                for (uint32_t i = 0; i < g.n && k < 6; i++)
                    if (pa[i] != pb[i]) { printf("  global %s differs at %08x: %02x vs %02x\n", global_name(g.at + i), g.at + i, pa[i], pb[i]); k++; }
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
    if (!g_fp.replay_only) {
        bool fpbad = false;
        for (uint32_t i = sizeof(Ctrl); i < ARENA_SIZE && !fpbad; i++) {
            if (g_after.arena[i] == g_start.arena[i] || in_footprint(g_arena + i)) continue;
            uint32_t rel;
            const char* r = region_of(i, &rel);
            if (st.fp_fails++ < 3) printf("FOOTPRINT %s (episode %d, %s): %s+0x%x changed outside it\n", name.c_str(), g_eno, g_phase, r, rel);
            fpbad = true;
        }
        const uint8_t* pa = g_after.globals;
        const uint8_t* ps = g_start.globals;
        for (const GRange& g : g_globals) {
            for (uint32_t i = 0; i < g.n && !fpbad; i++)
                if (pa[i] != ps[i] && !in_footprint((void*)(uintptr_t)(g.at + i))) {
                    if (st.fp_fails++ < 3) printf("FOOTPRINT %s (episode %d, %s): global %s (%08x) changed outside it\n", name.c_str(), g_eno, g_phase, global_name(g.at + i), g.at + i);
                    fpbad = true;
                }
            pa += g.n; ps += g.n;
        }
    }
    load(g_after);                                        // go on from the original's result
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
#define CB(FN, ...) do { save(g_saved); g_chain = false; CHECK(FN, __VA_ARGS__); load(g_saved); g_chain = true; CHECK(FN, __VA_ARGS__); g_chain = false; } while (0)
#define CB0(FN) do { save(g_saved); g_chain = false; CHECK0(FN); load(g_saved); g_chain = true; CHECK0(FN); g_chain = false; } while (0)

// ---- coverage and evidence --------------------------------------------------------------------------------------------
#if defined(__GNUC__) && !defined(__clang__)
// (GCC: declared above check())
#else
#define COVS(X) X(key_full) X(key_dropped_disabled) X(key_meta) X(key_corrupt) X(mouse_full) X(mouse_coalesced)     \
    X(pool_exhausted) X(pool_panic) X(pool_zero_count) X(dpool_overalloc) X(node_grab_fail) X(node_release_fail)      \
    X(stream_overflow) X(stream_getstring_over_max) X(stream_getstring_uninit) X(table_sorted) X(table_sort_changed)  \
    X(table_find_present_missed) X(table_found) X(b64_roundtrip_ok) X(b64_bad) X(b64_over_max) X(slerp_negated_a)     \
    X(slerp_small_sin) X(slerp_div0) X(bag_full) X(clip_get) X(clip_put) X(joy_stop) X(joy_fail) X(random_out_of_range)
enum {
#define X(n) C_##n,
    COVS(X)
#undef X
    NCOV
};
static const char* const k_cov_names[] = {
#define X(n) #n,
    COVS(X)
#undef X
};
static long g_cov[NCOV];
#endif

// =====================================================================================================================
// the episodes
// =====================================================================================================================
static void fill_rand(void* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) ((uint8_t*)p)[i] = (uint8_t)rnd(); }
static int32_t interesting_int() {
    switch (rnd() % 10) {
    case 0: return 0;
    case 1: return -1;
    case 2: return 0x7fffffff;
    case 3: return (int32_t)0x80000000;
    case 4: return ri(-40, 40);
    case 5: return (int32_t)rnd();
    default: return ri(0, 100);
    }
}
static void new_episode() {
    memset(g_arena, 0, ARENA_SIZE);
    Ctrl* c = ctrl();
    c->alloc_fail = chance(70) ? 0 : ri(1, 30);
    c->file_ok = chance(85);
    c->st_ok = chance(95);
    c->st_version = chance(95) ? 0 : rnd();
    c->clip_open = chance(85);
    c->clip_has = chance(85);
    c->clip_lock = chance(90);
    c->clip_empty = chance(90);
    c->clip_alloc = chance(90);
    c->hr_fail = chance(50) ? 0 : ri(5, 60);
    c->idle_msgs = chance(50);
    c->mvk = rnd();
    for (int i = 0; i < 8; i++) {
        FakeCom* o = com_obj(i);
        o->vtbl = g_com_vtbl;
        o->id = (uint32_t)i;
        o->refs = 10;
    }
    // statics: some junk, then what each area needs
    fill_rand((void*)0x00508088, 0x100);
    G32(A_KEY_SINGLE) = ri(1, 9);
    G32(A_MOUSE_SINGLE) = ri(1, 9);
    G32(A_NODE_MULTI) = ri(0x100, 0x109);
    G32(A_RAN_MULTI) = ri(0x100, 0x109);
}

// ---- keyboard ---------------------------------------------------------------------------------------------------
static void key_episode() {
    g_phase = "keys";
    G16(A_KEY_META) = (uint16_t)(chance(50) ? 0 : rnd());
    G32(A_KEY_HEAD) = chance(90) ? ri(0, 31) : ri(-3, 40);
    G32(A_KEY_TAIL) = chance(90) ? ri(0, 31) : ri(-3, 40);
    if (G32(A_KEY_HEAD) > 31 || G32(A_KEY_HEAD) < 0) g_cov[C_key_corrupt]++;
    fill_rand((void*)(uintptr_t)A_KEY_BUF, 0x40);
    for (int i = 0; i < 0x200; i++) G8(A_KEY_OFF + i) = (uint8_t)(chance(90) ? 0 : chance(70) ? 1 : rnd());
    const int n = ri(30, 160);
    for (int k = 0; k < n && !g_abort; k++) {
        const uint32_t c = chance(60) ? (uint32_t)ri(0x08, 0x7f) : rnd();
        const uint32_t key = chance(70) ? (uint32_t)ri(0, 0x7f) : rnd();
        switch (rnd() % 24) {
        case 0: case 1: case 2: {                             // a burst: past full
            const int m = ri(1, 40);
            for (int i = 0; i < m && !g_abort; i++) CB(KeyQueueChar_rw, chance(80) ? (uint32_t)ri(0x20, 0x7e) : rnd(), (uint32_t)ri(0, 0xff));
            if (m > 31) g_cov[C_key_full]++;
            break;
        }
        case 3: CB(KeyQueueChar_rw, c, key); if (G8(A_KEY_OFF + (uint8_t)key)) g_cov[C_key_dropped_disabled]++; break;
        case 4: CB(KeyQueueMetaChar_rw, chance(70) ? (uint32_t)ri(0, 0x80) : c, key); g_cov[C_key_meta]++; break;
        case 5: CB(KeyDown_rw, chance(75) ? (uint32_t)ri(0x10, 0x12) : c); break;
        case 6: CB(KeyUp_rw, chance(75) ? (uint32_t)ri(0x10, 0x12) : c); break;
        case 7: CB0(KeyClearBits_rw); break;
        case 8: case 9: case 10: CB0(KeyGet_rw); break;
        case 11: CB0(KeyHit_rw); break;
        case 12: CB(KeyEnable_rw, (uint8_t)rnd()); break;
        case 13: CB(KeyDisable_rw, (uint8_t)rnd()); break;
        case 14: if (chance(20)) CB0(KeyDisableAll_rw); else CB0(KeyEnableAll_rw); break;
        case 15: if (chance(30)) CB0(KeyClear_rw); break;
        case 16: if (chance(15)) { CB0(KeyEnd_rw); CB0(KeyBegin_rw); } break;
        case 17: CB(queue_rw, c, key); break;
        case 18: CB(add_to_buffer_rw, c); break;
        case 19: CB0(buffer_get_rw); CB0(is_buffer_empty_rw); CB0(is_buffer_full_rw); break;
        case 20: CB(next_element_rw, interesting_int()); break;
        case 21: CB(is_meta_char_rw, chance(70) ? (uint32_t)ri(0, 0x100) : rnd()); break;
        case 22: CB(KeyConvertScanKey_rw, (uint8_t)rnd()); break;
        default: {                                            // drain
            const int m = ri(1, 40);
            for (int i = 0; i < m && !g_abort; i++) CB0(KeyGet_rw);
            break;
        }
        }
    }
}

// ---- mouse ------------------------------------------------------------------------------------------------------
static void mouse_episode() {
    g_phase = "mouse";
    G32(A_MOUSE_HEAD) = chance(92) ? ri(0, 15) : ri(-2, 20);
    G32(A_MOUSE_TAIL) = chance(92) ? ri(0, 15) : ri(-2, 20);
    fill_rand((void*)(uintptr_t)A_MOUSE_BUF, 0x100);
    for (int i = 0; i < 16; i++)
        if (chance(40)) G32(A_MOUSE_BUF + 16 * i) = 6;
    MouseEvent* ev = (MouseEvent*)at(OFF_OUT);
    int32_t* outs = (int32_t*)at(OFF_OUT + 0x40);
    const int n = ri(30, 160);
    for (int k = 0; k < n && !g_abort; k++) {
        const int32_t type = chance(50) ? 6 : chance(80) ? ri(0, 7) : (int32_t)rnd();
        switch (rnd() % 14) {
        case 0: case 1: {
            const int m = ri(1, 30);
            for (int i = 0; i < m && !g_abort; i++) CB(MouseQueueEvent_rw, chance(70) ? 6 : ri(0, 7), ri(-10, 700), ri(-10, 500), ri(0, 7));
            if (m > 15) g_cov[C_mouse_full]++;
            break;
        }
        case 2: case 3: {
            const bool co = type == 6 && !mouse_is_buffer_empty_rw() && mouse_buffer_top_rw()->type == 6;
            CB(MouseQueueEvent_rw, type, interesting_int(), interesting_int(), interesting_int());
            if (co) g_cov[C_mouse_coalesced]++;
            break;
        }
        case 4: case 5: CB(MouseGetEvent_rw, chance(90) ? ev : (MouseEvent*)at(OFF_OUT + ri(0, 0x30))); break;
        case 6: CB0(MousePeekEvent_rw); break;
        case 7: CB(MousePeek_rw, &outs[0], &outs[1], chance(10) ? &outs[0] : &outs[2]); break;
        case 8: if (chance(30)) CB0(MouseClear_rw); break;
        case 9: if (chance(15)) { CB0(MouseEnd_rw); CB0(MouseBegin_rw); } break;
        case 10: CB(mouse_buffer_get_rw, ev); break;
        case 11: CB(mouse_buffer_add_rw, ev); ev->type = type; ev->x = (int32_t)rnd(); break;
        case 12: CB0(mouse_is_buffer_empty_rw); CB0(mouse_is_buffer_full_rw); CB0(mouse_buffer_top_rw); break;
        default: {
            const int m = ri(1, 20);
            for (int i = 0; i < m && !g_abort; i++) CB(MouseGetEvent_rw, ev);
            break;
        }
        }
    }
}

// ---- scan, DirectInput, joystick --------------------------------------------------------------------------------
static const uint32_t k_di_codes[] = {
    0x8000000a, 0x80004001, 0x80004002, 0x80004005, 0x80040110, 0x80040154, 0x80040200, 0x80040201, 0x80040202,
    0x80040203, 0x80040204, 0x80040205, 0x80040206, 0x80040207, 0x80040208, 0x80040209, 0x80070002, 0x80070005,
    0x8007000c, 0x8007000e, 0x80070015, 0x8007001e, 0x80070057, 0x80070077, 0x800700aa, 0x8007047e, 0x80070481,
    0x800704df, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0xffffffff, 0x7fffffff, 0x80000000, 0x80004000,
    0x80070001, 0x80070003, 0x800704e0, 0x80040155,
};
static void input_episode() {
    g_phase = "scan / joystick";
    for (uint32_t c : k_di_codes) CB(di_error_rw, (int32_t)c);
    for (int i = 0; i < 40; i++) CB(di_error_rw, (int32_t)(chance(50) ? rnd() : 0x80040000u + (rnd() & 0x7ffff)));
    for (int i = 0; i < 40; i++) {
        CB(ScanDown_rw, (uint8_t)rnd());
        CB(ScanHit_rw, (uint8_t)rnd());
    }
    GP(void, A_DI) = chance(80) ? (void*)com_obj(1) : 0;
    GP(void, A_DIDEV) = chance(80) ? (void*)com_obj(4) : 0;
    CB0(ScanEnd_rw);
    // the joystick
    uint8_t* inst = at(OFF_INST);
    fill_rand(inst, 0x244);
    const char* names[] = {"Microsoft SideWinder Force Feedback Wheel", "Microsoft SideWinder Force Feedback Pro",
                           "Logitech WingMan", "", "microsoft sidewinder force feedback wheel"};
    strcpy((char*)inst + 0x12c, names[rnd() % 5]);
    GP(void, A_JOY_DI) = com_obj(1);
    GP(void, A_JOY_DEV) = chance(50) ? com_obj(2) : 0;
    CB0(JoyIsPresent_rw);
    const int n = ri(1, 4);
    for (int i = 0; i < n; i++) {
        CB(enum_joystick_rw, (const uint8_t*)inst, (void*)at(OFF_OUT));
        if (g_last_result == 0) g_cov[g_last_ret == 0 ? C_joy_stop : C_joy_fail]++;
    }
    CB0(JoyIsPresent_rw);
    GP(void, A_JOY_DEV) = com_obj(2);
    for (int i = 0; i < 6; i++) CB(set_prop_dword_rw, (const void*)(uintptr_t)ri(1, 12), rnd(), chance(40) ? 0xffffffffu : rnd());
}

// ---- random numbers ---------------------------------------------------------------------------------------------
static void random_state_junk() {
    for (int i = 0; i < 97; i++) GF(A_RAN_U + 4 * i) = chance(90) ? uni() : special_float();
    GF(A_RAN_C) = chance(80) ? uni() : special_float();
    GF(A_RAN_CD) = chance(80) ? uni() : special_float();
    GF(A_RAN_CM) = chance(80) ? uni() : special_float();
    G32(A_RAN_I97) = chance(95) ? ri(0, 96) : ri(-1, 96);
    G32(A_RAN_J97) = chance(95) ? ri(0, 96) : ri(-2, 96);
}
static int32_t seed_value() {
    switch (rnd() % 6) {
    case 0: return ri(0, 31328);
    case 1: return ri(0, 30081);
    case 2: return ri(-40000, 40000);
    case 3: return (int32_t)rnd();
    case 4: return ri(0, 5);
    default: return ri(0, 30000);
    }
}
static void random_episode() {
    g_phase = "random";
    if (chance(70)) CB(rmarin_rw, seed_value(), seed_value());
    else random_state_junk();
    const int n = ri(20, 120);
    for (int k = 0; k < n && !g_abort; k++) {
        switch (rnd() % 10) {
        case 0: case 1: case 2: CB0(ranmar_rw); break;
        case 3: case 4: case 5: {
            const int32_t r = chance(60) ? ri(1, 1000) : interesting_int();
            CB(Random1_rw, r);
            if (g_last_result == 0 && r > 0 && ((int32_t)g_last_ret >= r || (int32_t)g_last_ret < 0)) g_cov[C_random_out_of_range]++;
            break;
        }
        case 6: case 7: CB(Random2_rw, interesting_int(), interesting_int()); break;
        case 8: if (chance(30)) CB(rmarin_rw, seed_value(), seed_value()); break;
        default:
            if (chance(30)) CB0(Randomize_rw);
            else if (chance(20)) { CB0(RandomEnd_rw); CB0(RandomBegin_rw); }
            break;
        }
    }
}

// the long run: many seeds, many draws, value and state compared after every draw (no logging; the Multi stubs run)
static long g_long_draws, g_long_seeds, g_long_bad, g_long_random_oor;
static void long_random_run(int seeds, int draws) {
    uint8_t s_orig[RAN_BYTES], s_new[RAN_BYTES];
    g_quiet = true;
    for (int sd = 0; sd < seeds; sd++) {
        const int32_t ij = sd < 64 ? sd * 489 : seed_value(), kl = sd < 64 ? 30081 - sd * 470 : seed_value();
        const unsigned pc = sd & 1 ? _PC_24 : _PC_53;
        fpu_mode(pc, false);
        uint8_t s_pre[RAN_BYTES];
        memcpy(s_pre, (void*)(uintptr_t)A_RAN_FIRST, RAN_BYTES);
        G8(A_RAN_TEST) = 0;
        ((void(__cdecl*)(int32_t, int32_t))0x0041b720)(ij, kl);
        memcpy(s_orig, (void*)(uintptr_t)A_RAN_FIRST, RAN_BYTES);
        const uint8_t t0 = G8(A_RAN_TEST);
        memcpy((void*)(uintptr_t)A_RAN_FIRST, s_pre, RAN_BYTES);
        G8(A_RAN_TEST) = 0;
        rmarin_rw(ij, kl);
        memcpy(s_new, (void*)(uintptr_t)A_RAN_FIRST, RAN_BYTES);
        g_long_seeds++;
        if (memcmp(s_orig, s_new, RAN_BYTES) || t0 != G8(A_RAN_TEST)) {
            if (g_long_bad++ < 4) {
                printf("MISMATCH rmarin(%d, %d) at pc %s (seeded flag %d vs %d)\n", ij, kl, pc == _PC_24 ? "24" : "53", t0, G8(A_RAN_TEST));
                for (int i = 0, k = 0; i < RAN_BYTES && k < 6; i += 4)
                    if (memcmp(s_orig + i, s_new + i, 4)) {
                        uint32_t a, b;
                        memcpy(&a, s_orig + i, 4); memcpy(&b, s_new + i, 4);
                        printf("  %s (+%x): original %08x (%.9g), rewrite %08x (%.9g)\n", global_name(A_RAN_FIRST + i), i, a, fbits(a), b, fbits(b));
                        k++;
                    }
            }
            continue;
        }
        for (int d = 0; d < draws; d++) {
            const int kind = d % 3;
            const int32_t n = kind == 2 ? (d & 8 ? ri(1, 100) : interesting_int()) : 0;
            uint32_t r0, r1;
            memcpy((void*)(uintptr_t)A_RAN_FIRST, s_orig, RAN_BYTES);
            if (kind == 2) r0 = (uint32_t)((int32_t(__cdecl*)(int32_t))0x0041b6e0)(n);
            else r0 = ubits(((float(__cdecl*)())0x0041b910)());
            memcpy(s_orig, (void*)(uintptr_t)A_RAN_FIRST, RAN_BYTES);
            memcpy((void*)(uintptr_t)A_RAN_FIRST, s_new, RAN_BYTES);
            if (kind == 2) {                                   // the rewrite of Random over the rewrite of ranmar
                uint8_t save5[5];
                memcpy(save5, (void*)0x0041b910, 5);
                patch_jmp(0x0041b910, (void*)&ranmar_rw);
                r1 = (uint32_t)Random1_rw(n);
                memcpy((void*)0x0041b910, save5, 5);
            } else {
                r1 = ubits(ranmar_rw());
            }
            memcpy(s_new, (void*)(uintptr_t)A_RAN_FIRST, RAN_BYTES);
            g_long_draws++;
            if (kind == 2 && n > 0 && ((int32_t)r0 >= n || (int32_t)r0 < 0)) g_long_random_oor++;
            if (r0 != r1 || memcmp(s_orig, s_new, RAN_BYTES)) {
                if (g_long_bad++ < 4) printf("MISMATCH draw %d (kind %d, n %d) after rmarin(%d, %d) at pc %s: %08x vs %08x\n", d, kind, n, ij, kl,
                                             pc == _PC_24 ? "24" : "53", r0, r1);
                break;
            }
        }
        fpu_reset();
    }
    g_quiet = false;
}

// ---- pools and nodes --------------------------------------------------------------------------------------------
static const char* pool_name() { return chance(50) ? (const char*)at(OFF_STR) : (const char*)(uintptr_t)0x004eb4b8; }
static void pool_episode() {
    g_phase = "pools";
    strcpy((char*)at(OFF_STR), "testpool");
    PoolBase* pools = (PoolBase*)at(OFF_POOLS);             // 4 pools
    DebugPoolBase* dp = (DebugPoolBase*)at(OFF_POOLS + 0x100);
    std::vector<void*> live[4];
    std::vector<void*> dlive;
    for (int i = 0; i < 4; i++) {
        const int32_t count = chance(8) ? 0 : chance(90) ? ri(1, 40) : ri(41, 300);
        const uint32_t size = chance(80) ? (uint32_t)(4 * ri(1, 6)) : (uint32_t)ri(4, 13);
        if (count == 0) g_cov[C_pool_zero_count]++;
        CB(PoolBase_ctor_rw, &pools[i], 0, pool_name(), count, size);
    }
    CB(DebugPoolBase_ctor_rw, dp, 0, pool_name(), ri(0, 12), (uint32_t)ri(1, 40));
    const int n = ri(40, 200);
    const char* dd = getenv("VP_K3_DPDUMP");
    const bool dump = dd && atoi(dd) == g_eno;
    if (dump) printf("dp count/max %d size %u; inner pool mem %p count %d\n", dp->max, dp->size, dp->pool.mem, dp->pool.count);
    for (int k = 0; k < n && !g_abort; k++) {
        const int i = ri(0, 3);
        PoolBase* p = &pools[i];
        if (dump) {
            printf("  op %d: dp used %d crashes %d inner head %p used %d; blocks:", k, dp->used, dp->crashes, dp->pool.head, dp->pool.used);
            int hops = 0;
            for (DebugBlock* b = dp->blocks; b && hops < 12; b = b->next, hops++) printf(" %p(mem %p)", b, b->mem);
            printf("\n");
        }
        switch (rnd() % 16) {
        case 0: case 1: case 2: case 3: {
            const bool empty = !p->head;
            CB(PoolBase_alloc_rw, p, 0);
            if (empty) g_cov[C_pool_exhausted]++;
            if (g_last_result == 1) g_cov[C_pool_panic]++;
            if (g_last_result == 0 && g_last_ret) live[i].push_back((void*)(uintptr_t)(uint32_t)g_last_ret);
            break;
        }
        case 4: case 5:
            if (!live[i].empty()) {
                const size_t j = rnd() % live[i].size();
                CB(PoolBase_free_rw, p, 0, live[i][j]);
                live[i].erase(live[i].begin() + j);
            }
            break;
        case 6: if (chance(20)) { CB(PoolBase_freeall_rw, p, 0); live[i].clear(); } break;
        case 7:                                               // (as the game calls it: head at the block's start)
            if (chance(10)) { p->head = p->mem; CB(PoolBase_init_list_rw, p, 0); live[i].clear(); }
            break;
        case 8: CB(PoolBase_OverallocCrashes_rw, p, 0, (uint8_t)(chance(50) ? 0 : chance(80) ? 1 : rnd())); break;
        case 9: {
            if (dp->crashes && dp->used >= dp->max) g_cov[C_dpool_overalloc]++;
            CB(DebugPoolBase_alloc_rw, dp, 0);
            if (g_last_result == 0 && g_last_ret) dlive.push_back((void*)(uintptr_t)(uint32_t)g_last_ret);
            break;
        }
        case 10:
            if (!dlive.empty() && chance(85)) {
                const size_t j = rnd() % dlive.size();
                CB(DebugPoolBase_free_rw, dp, 0, dlive[j]);
                dlive.erase(dlive.begin() + j);
            } else CB(DebugPoolBase_free_rw, dp, 0, (void*)at(OFF_OUT));
            break;
        case 11: CB(DebugPoolBase_is_member_rw, dp, 0, !dlive.empty() && chance(60) ? dlive[rnd() % dlive.size()] : (void*)at(OFF_OUT)); break;
        case 12: CB(DebugPoolBase_OverallocCrashes_rw, dp, 0, (uint8_t)(chance(50) ? 0 : 1)); break;
        case 13: if (chance(15)) { CB(DebugPoolBase_freeall_rw, dp, 0); dlive.clear(); } break;
        case 14: CB(ASSERT_MSG_pool_rw, (int)rnd(), (const char*)at(OFF_STR)); break;
        default: break;
        }
    }
    if (g_abort) return;
    // the ends
    for (int i = 0; i < 4; i++) {
        switch (rnd() % 5) {
        case 0: CB(PoolBase_dtor_rw, &pools[i], 0); break;
        case 1: CB(PoolBase_sdd_rw, &pools[i], 0, (uint32_t)(rnd() & 3)); break;
        case 2: CB(PoolNode_sdd_rw, &pools[i], 0, (uint32_t)(rnd() & 3)); break;
        case 3: CB(PoolBlock_sdd_rw, &pools[i], 0, (uint32_t)(rnd() & 3)); break;
        default: break;
        }
    }
    if (chance(50)) CB(DebugPoolBase_dtor_rw, dp, 0);
    else CB(DebugPoolBase_sdd_rw, dp, 0, (uint32_t)(rnd() & 3));
}
static void node_episode() {
    g_phase = "nodes";
    ctrl()->alloc_fail = chance(85) ? 0 : 20;
    CB(NodeBegin_rw, chance(10) ? 10000 : ri(0, 60));
    std::vector<void*> live;
    const int n = ri(20, 120);
    for (int k = 0; k < n && !g_abort; k++) {
        switch (rnd() % 8) {
        case 0: CB(NodeGrab_rw, chance(80) ? ri(0, 20) : interesting_int()); if (g_nlog[0] > 2) g_cov[C_node_grab_fail] += g_log[0][1].kind == L_REPORT; break;
        case 1: CB(NodeRelease_rw, chance(80) ? ri(0, 20) : interesting_int()); if (g_nlog[0] > 2) g_cov[C_node_release_fail] += g_log[0][1].kind == L_REPORT; break;
        case 2: case 3: case 4:
            if (GP(PoolBase, A_NODE_POOL)) {
                CB0(NodeAlloc_rw);
                if (g_last_result == 0 && g_last_ret) live.push_back((void*)(uintptr_t)(uint32_t)g_last_ret);
            }
            break;
        case 5: case 6:
            if (!live.empty() && GP(PoolBase, A_NODE_POOL)) {
                const size_t j = rnd() % live.size();
                CB(NodeFree_rw, live[j]);
                live.erase(live.begin() + j);
            }
            break;
        default:
            if (chance(10)) { CB0(NodeEnd_rw); CB(NodeBegin_rw, ri(0, 30)); live.clear(); }
            break;
        }
    }
    if (g_abort) return;
    CB0(NodeEnd_rw);
    if (chance(30)) { CB0(UsefulBegin_rw); CB0(UsefulEnd_rw); }
}

// ---- memory streams -----------------------------------------------------------------------------------------------
static void stream_episode() {
    g_phase = "streams";
    // a stream made by the functions (heap), and one made here over the arena (for any size and garbage)
    MemStream* own = (MemStream*)at(OFF_OBJS);
    own->size = chance(80) ? ri(0, SDATA_MAX) : ri(-8, 16);
    own->data = at(OFF_SDATA);
    fill_rand(own->data, SDATA_MAX);
    MemStreamPtr* ptrs = (MemStreamPtr*)at(OFF_OBJS + 0x20);
    ptrs[0].s = own; ptrs[0].pos = 0; ptrs[0].overflow = 0;
    ptrs[1].s = own; ptrs[1].pos = chance(50) ? 0 : ri(-8, own->size + 8); ptrs[1].overflow = (uint8_t)chance(20);
    char* strs = (char*)at(OFF_STR);
    for (int i = 0; i < 8; i++) {
        const int len = chance(80) ? ri(0, 40) : ri(41, 300);
        for (int j = 0; j < len; j++) strs[i * 0x100 + j] = (char)ri(0x20, 0x7e);
        strs[i * 0x100 + (len < 0xff ? len : 0xff)] = 0;
    }
    uint8_t* outb = at(OFF_OUT);
    if (chance(50)) {
        CB(MemStreamCreate_rw, chance(90) ? ri(0, 0x800) : interesting_int());
        MemStream* hs = g_last_result == 0 ? (MemStream*)(uintptr_t)(uint32_t)g_last_ret : 0;
        if (hs && hs->data) {
            CB(MemStreamCreatePtr_rw, hs);
            if (g_last_result == 0 && g_last_ret) ptrs[1] = *(MemStreamPtr*)(uintptr_t)(uint32_t)g_last_ret;
        }
    }
    const int n = ri(40, 200);
    for (int k = 0; k < n && !g_abort; k++) {
        MemStreamPtr* p = &ptrs[rnd() & 1];
        const bool was = p->overflow != 0;
        switch (rnd() % 20) {
        case 0: CB(MemStreamSeekStart_rw, p); break;
        case 1: CB(MemStreamSeekEnd_rw, p); break;
        case 2: CB(MemStreamSeekPos_rw, p, chance(80) ? ri(-8, p->s->size + 8) : chance(50) ? interesting_int() : 0x7ffffffe - ri(0, 4)); break;
        case 3: CB(MemStreamGetPos_rw, p); CB(MemStreamValidPos_rw, p); break;
        case 4: CB(MemStreamGotOverflow_rw, p); break;
        case 5: case 6: CB(MemStreamPutInt_rw, p, (int32_t)rnd()); break;
        case 7: CB(MemStreamPutReal_rw, p, chance(70) ? ubits(rf(-1e3f, 1e3f)) : ubits(special_float())); break;
        case 8: CB(MemStreamPutString_rw, p, strs + 0x100 * ri(0, 7)); break;
        case 9: CB(MemStreamPutData_rw, p, (const void*)(strs + ri(0, 0x100)), chance(90) ? ri(0, 0x300) : ri(-4, -1)); break;
        case 10: case 11: CB(MemStreamGetInt_rw, p, (int32_t*)(outb + 4 * ri(0, 3))); break;
        case 12: CB(MemStreamGetReal_rw, p, (uint32_t*)(outb + 4 * ri(0, 3))); break;
        case 13: {
            const int32_t pos = p->pos;
            const bool ok = pos >= 0 && (int64_t)pos + 4 <= p->s->size;   // (a wrapped check faults in both)
            const int32_t len = ok ? *(int32_t*)(p->s->data + pos) : -1;
            const int32_t max = ri(1, 0x100);
            if (ok && len > max && len <= 0x300 && (int64_t)pos + 4 + len <= p->s->size) g_cov[C_stream_getstring_over_max]++;
            if (!ok) g_cov[C_stream_getstring_uninit]++;
            // only lengths the output area holds (a bigger one overruns the arena in both, as the game would)
            if (!ok || (len >= -0x100 && len <= 0x300)) CB(MemStreamGetString_rw, p, (char*)(outb + 0x10), max);
            break;
        }
        case 14: CB(MemStreamGetData_rw, p, (void*)(outb + 0x10), chance(90) ? ri(0, 0x300) : ri(-4, -1)); break;
        case 15: if (chance(30)) CB(MemStreamWrite_rw, p->s, ri(1, 9)); break;
        case 16: if (chance(30)) CB(MemStreamRead_rw, p->s, ri(1, 9)); break;
        default: {                                            // fill it up: overflow
            const int m = ri(1, 60);
            for (int i = 0; i < m && !g_abort; i++) CB(MemStreamPutInt_rw, p, (int32_t)rnd());
            break;
        }
        }
        if (!was && p->overflow) g_cov[C_stream_overflow]++;
    }
    if (g_abort) return;
    if (chance(50)) {
        MemStreamPtr* hp = (MemStreamPtr*)at(OFF_OBJS + 0x60);
        *hp = ptrs[1];
        CB(MemStreamDestroyPtr_rw, chance(90) ? hp : (MemStreamPtr*)0);
        CB(MemStreamDestroy_rw, chance(90) ? own : (MemStream*)0);
    }
}

// ---- string tables ------------------------------------------------------------------------------------------------
static void table_episode() {
    g_phase = "string tables";
    StringTable* t = (StringTable*)at(OFF_TABLE);
    const int32_t cols = ri(1, 16);
    int32_t rowsize = 0;
    for (int c = 0; c < cols; c++) {
        t->coloff[c] = rowsize;
        rowsize += chance(80) ? ri(2, 24) : ri(25, 60);
    }
    rowsize = rowsize > 0x400 ? 0x400 : rowsize;
    for (int c = 0; c < cols; c++) if (t->coloff[c] >= rowsize) t->coloff[c] = rowsize - 1;
    const int32_t fit = (int32_t)((TABLE_MAX - 0x4c) / (uint32_t)rowsize);
    const int32_t rows = chance(5) ? ri(-3, 0) : ri(1, fit < 60 ? fit : 60);
    t->rows = rows;
    t->cols = cols;
    t->rowsize = rowsize;
    for (int c = cols; c < 16; c++) t->coloff[c] = (int32_t)rnd() % 64;
    fill_rand(t->data, (uint32_t)(rows > 0 ? rows : 0) * (uint32_t)rowsize);
    static const char* words[] = {"alpha", "Beta", "beta", "GAMMA", "delta", "a", "", "zz", "Zz", "viper", "Viper",
                                  "track", "car", "0", "10", "9"};
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            const int32_t w = (c + 1 < cols ? t->coloff[c + 1] : rowsize) - t->coloff[c];
            char* e = t->data + r * rowsize + t->coloff[c];
            if (w <= 0) continue;
            const char* s = words[rnd() % 16];
            const int l = (int)strlen(s) < w - 1 ? (int)strlen(s) : w - 1;
            memcpy(e, s, (size_t)l);
            e[l] = 0;
        }
    char* key = (char*)at(OFF_STR);
    int32_t* row_out = (int32_t*)at(OFF_OUT);
    const int n = ri(10, 60);
    for (int k = 0; k < n && !g_abort; k++) {
        const int32_t col = ri(0, cols - 1);
        switch (rnd() % 8) {
        case 0: {
            std::vector<uint8_t> before(t->data, t->data + (rows > 0 ? rows : 0) * rowsize);
            CB(StringTableSort_rw, t, col);
            g_cov[C_table_sorted]++;
            if (memcmp(before.data(), t->data, before.size())) g_cov[C_table_sort_changed]++;
            break;
        }
        case 1: case 2: case 3: {
            bool present = false;
            if (rows > 0 && chance(70)) {
                strcpy(key, StringTableGetEntry_rw(t, ri(0, rows - 1), col));
                present = true;
            } else strcpy(key, words[rnd() % 16]);
            CB(StringTableFind_rw, t, col, (const char*)key, row_out);
            if (present && g_last_result == 0) g_cov[(uint8_t)g_last_ret ? C_table_found : C_table_find_present_missed]++;
            break;
        }
        case 4: CB(StringTableGetEntry_rw, (const StringTable*)t, rows > 0 ? ri(0, rows - 1) : 0, col); break;
        case 5: CB(StringTableNumRows_rw, (const StringTable*)t); CB(StringTableNumColumns_rw, (const StringTable*)t); break;
        case 6: CB(StringTableGet_rw, (const char*)key); break;
        default: CB(StringTableForget_rw, t); break;
        }
    }
}

// ---- base64 ---------------------------------------------------------------------------------------------------------
static void base64_episode() {
    g_phase = "base64";
    if (chance(30)) G8(A_B64_INIT) = 1;                      // the table built again (and from scratch)
    if (chance(10)) fill_rand((void*)(uintptr_t)A_B64_TABLE, 0x100);
    uint8_t* raw = at(OFF_B64IN);
    char* txt = (char*)at(OFF_B64OUT);
    uint8_t* back = at(OFF_B64IN + 0x800);
    const int n = ri(10, 40);
    for (int k = 0; k < n && !g_abort; k++) {
        const int32_t len = chance(90) ? ri(0, 0x300) : interesting_int() % 0x300;
        fill_rand(raw, 0x300);
        const int32_t max = chance(70) ? 0x1000 : ri(-2, 0x420);
        const int32_t groups = len > 0 && max > 0 ? ((len + 2) / 3 < (max + 3) / 4 ? (len + 2) / 3 : (max + 3) / 4) : 0;
        if (4 * groups >= max && max > 0) g_cov[C_b64_over_max]++;
        CB(MemToBase64_rw, txt, max, (const uint8_t*)raw, len);
        switch (rnd() % 5) {
        case 0: txt[ri(0, (int)strlen(txt))] = (char)(chance(50) ? '=' : rnd()); g_cov[C_b64_bad]++; break;
        case 1: if (strlen(txt) >= 4) txt[strlen(txt) - ri(1, 3)] = 0; break;
        case 2: { const size_t l = strlen(txt); if (l < 0x1000 - 4) strcat(txt, "===="); } break;
        default: break;
        }
        const int32_t bmax = chance(70) ? ri(0, 0x300) : interesting_int() % 0x300;
        CB(Base64ToMem_rw, back, bmax, (const char*)txt);
        if (g_last_result == 0 && (uint8_t)g_last_ret && len > 0 && bmax >= len && !memcmp(raw, back, (size_t)len)) g_cov[C_b64_roundtrip_ok]++;
    }
    for (int i = 0; i < 20; i++) {
        char quad[5];
        for (int j = 0; j < 4; j++) quad[j] = chance(85) ? ((const char*)(uintptr_t)A_B64_ALPHABET)[ri(0, 63)] : (char)rnd();
        quad[4] = 0;
        memcpy(at(OFF_STR), quad, 5);
        CB(base64_to_triple_rw, (const char*)at(OFF_STR));
        CB(from_64_rw, rnd());
        CB(to_64_rw, (uint8_t)rnd());
        CB(triple_to_base64_rw, (char*)at(OFF_STR + 0x10), rnd());
    }
}

// ---- Slerp ----------------------------------------------------------------------------------------------------------
static void rand_quat(Q4* q) {
    switch (rnd() % 9) {
    case 7:
        if (chance(30)) { q->x = q->y = q->z = q->w = 0.0f; return; }   // zero: a zero-length result
        // fall through
    case 8: {                                                 // an axis: exact dot products of +-1 and 0
        q->x = q->y = q->z = q->w = 0.0f;
        (&q->x)[rnd() & 3] = chance(50) ? 1.0f : -1.0f;
        return;
    }
    case 0: q->x = special_float(); q->y = rf(-1, 1); q->z = rf(-1, 1); q->w = special_float(); return;
    case 1: q->x = rf(-1e20f, 1e20f); q->y = rf(-1e20f, 1e20f); q->z = rf(-1e20f, 1e20f); q->w = rf(-1e20f, 1e20f); return;
    default: {
        float x = rf(-1, 1), y = rf(-1, 1), z = rf(-1, 1), w = rf(-1, 1);
        const float l = sqrtf(x * x + y * y + z * z + w * w);
        if (l > 0) { x /= l; y /= l; z /= l; w /= l; }
        q->x = x; q->y = y; q->z = z; q->w = w;
    }
    }
}
static void slerp_episode() {
    g_phase = "slerp";
    Q4* q = (Q4*)at(OFF_QUAT);
    const int n = ri(20, 80);
    for (int k = 0; k < n && !g_abort; k++) {
        rand_quat(&q[0]);
        rand_quat(&q[1]);
        if (chance(15)) q[1] = q[0];                                          // sin(omega) = 0
        if (chance(10)) { q[1] = q[0]; q[1].x = fbits(ubits(q[1].x) + ri(-3, 3)); }   // nearly equal
        if (chance(15)) { q[1].x = -q[0].x; q[1].y = -q[0].y; q[1].z = -q[0].z; q[1].w = -q[0].w; }
        const float t = chance(80) ? uni() : chance(50) ? (float)ri(-2, 2) : special_float();
        Q4* out = &q[2];
        Q4* a = &q[0];
        const Q4* b = &q[1];
        switch (rnd() % 6) {
        case 0: out = a; break;
        case 1: out = (Q4*)b; break;
        case 2: b = a; break;
        default: break;
        }
        const double dot = (double)b->x * a->x + (double)b->y * a->y + (double)b->w * a->w + (double)a->z * b->z;
        if (dot < 0) g_cov[C_slerp_negated_a]++;
        {
            const float c = (float)dot;
            if (c * c == 1.0f) g_cov[C_slerp_small_sin]++;
        }
        CB(Slerp_rw, out, a, b, t);
    }
}

// ---- bags -------------------------------------------------------------------------------------------------------------
static void bag_episode() {
    g_phase = "bags";
    BagBase* bag = (BagBase*)at(OFF_OBJS + 0x100);
    CB(BagBase_ctor_rw, bag, 0, (const char*)at(OFF_STR), chance(90) ? ri(0, 24) : ri(-2, 0));
    if (g_last_result != 0 || !bag->items) return;
    if (chance(40)) bag->crashes = 0;
    std::vector<void*> in;
    const int n = ri(20, 100);
    for (int k = 0; k < n && !g_abort; k++) {
        switch (rnd() % 6) {
        case 0: case 1: case 2: {
            void* p = (void*)(uintptr_t)(0x1000 * ri(1, 40));
            const bool full = bag->count >= bag->cap;
            CB(BagBase_add_rw, bag, 0, p);
            if (full) g_cov[C_bag_full]++;
            if (g_last_result == 0 && (uint8_t)g_last_ret) in.push_back(p);
            break;
        }
        case 3: case 4:
            if (!in.empty() && chance(85)) {
                const size_t j = rnd() % in.size();
                CB(BagBase_remove_rw, bag, 0, in[j]);
                in.erase(in.begin() + j);
            } else CB(BagBase_remove_rw, bag, 0, (void*)(uintptr_t)0x999);
            break;
        default: if (chance(20)) { CB(BagBase_remove_all_rw, bag, 0); in.clear(); } break;
        }
    }
    if (g_abort) return;
    CB(BagBase_dtor_rw, bag, 0);
}

// ---- clipboard and HTML -----------------------------------------------------------------------------------------------
static void clip_html_episode() {
    g_phase = "clipboard / html";
    char* clip = (char*)at(OFF_CLIP);
    const int cl = ri(0, 0x300);
    for (int i = 0; i < cl; i++) clip[i] = (char)ri(1, 255);
    clip[cl] = 0;
    char* buf = (char*)at(OFF_OUT);
    char* str = (char*)at(OFF_STR);
    for (int i = 0; i < 8; i++) {
        const int32_t n = chance(95) ? ri(1, 0x300) : 0;
        CB(ClipboardGetText_rw, buf + 0x10, n);
        g_cov[C_clip_get]++;
        const int l = ri(0, 0x200);
        for (int j = 0; j < l; j++) str[j] = (char)ri(1, 255);
        str[l] = 0;
        CB(ClipboardPutText_rw, (const char*)str);
        g_cov[C_clip_put]++;
    }
    // HTML
    char* fmt = (char*)at(OFF_STR + 0x400);
    char* s1 = (char*)at(OFF_STR + 0x600);
    HTMLStyle* style = (HTMLStyle*)at(OFF_OBJS + 0x200);
    strcpy(s1, "some text");
    style->open = (const char*)at(OFF_STR + 0x680);
    style->close = (const char*)at(OFF_STR + 0x6c0);
    strcpy((char*)style->open, chance(50) ? "<B>" : "<FONT size=2>");     // a tag (as a format: no %)
    strcpy((char*)style->close, "</B>");
    CB(HTMLBegin_rw, (const char*)at(OFF_STR + 0x700), (const char*)s1);
    const int n = ri(3, 12);
    for (int k = 0; k < n && !g_abort; k++) {
        static const char* fmts[] = {"plain line", "%d and %s", "%x %c %d", "%f / %d", "%s%s", "<TR><TD>%d</TD><TD>%s</TD></TR>",
                                     "%08x %5.2f %-10s|", "%%"};
        strcpy(fmt, fmts[rnd() % 8]);
        if (chance(10)) { memset(fmt, 'x', 0x220); fmt[0x220] = 0; }        // a line over 0x1ff characters
        const double d = chance(80) ? (double)rf(-1000, 1000) : (double)special_float();
        uint32_t lo, hi;
        memcpy(&lo, &d, 4);
        memcpy(&hi, (const uint8_t*)&d + 4, 4);
        uint32_t a[16];
        for (int i = 0; i < 16; i++) a[i] = (uint32_t)ri(0, 999);
        if (strstr(fmt, "%s")) { a[1] = (uint32_t)(uintptr_t)s1; a[0] = (uint32_t)(uintptr_t)s1; }
        if (!strncmp(fmt, "%x %c", 5)) a[1] = (uint32_t)ri(0x20, 0x7e);
        if (!strncmp(fmt, "%f", 2)) { a[0] = lo; a[1] = hi; }
        if (!strncmp(fmt, "%08x", 4)) { a[1] = lo; a[2] = hi; a[3] = (uint32_t)(uintptr_t)s1; }
        if (!strncmp(fmt, "<TR>", 4)) a[1] = (uint32_t)(uintptr_t)s1;
        switch (rnd() % 4) {
        case 0: CB(HTMLWriteLn_rw, (const char*)fmt, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]); break;
        case 1: CB(HTMLWrite_rw, (const HTMLStyle*)style, (const char*)fmt, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]); break;
        case 2: CB(HTMLEnter_rw, (const HTMLStyle*)style); break;
        default: CB(HTMLLeave_rw, (const HTMLStyle*)style); break;
        }
    }
    if (g_abort) return;
    CB0(HTMLEnd_rw);
}

static void run_episode() {
    new_episode();
    g_abort = false;
    switch (g_eno % 11) {
    case 0: key_episode(); break;
    case 1: mouse_episode(); break;
    case 2: input_episode(); break;
    case 3: random_episode(); break;
    case 4: pool_episode(); break;
    case 5: node_episode(); break;
    case 6: stream_episode(); break;
    case 7: table_episode(); break;
    case 8: base64_episode(); break;
    case 9: slerp_episode(); bag_episode(); break;
    default: clip_html_episode(); break;
    }
}

// the watchdog: a check that runs for more than a minute (an original looping forever, e.g. over a cyclic list)
// is reported and the harness ends itself
#if defined(__GNUC__) && !defined(__clang__)
// (GCC: declared above check())
#else
static volatile long g_check_serial;
static volatile const char* g_check_name = "";
#endif
static DWORD WINAPI watchdog(void*) {
    long last = -1, same = 0;
    for (;;) {
        Sleep(1000);
        const long now = g_check_serial;
        same = now == last ? same + 1 : 0;
        last = now;
        if (same >= 60) {
            printf("HUNG in %s (episode %d, %s, pass %d, %s): a check ran for a minute\n",(const char*)g_check_name, g_eno, g_phase,
                   g_pass, g_chain ? "chain" : "isolated");
            fflush(stdout);
            ExitProcess(6);
        }
    }
}

static LONG WINAPI unhandled(EXCEPTION_POINTERS* e) {
    printf("UNHANDLED exception %08x at %08x (episode %d, %s)\n", (unsigned)e->ExceptionRecord->ExceptionCode,
           (unsigned)(uintptr_t)e->ExceptionRecord->ExceptionAddress, g_eno, g_phase);
    fflush(stdout);
    ExitProcess(4);
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
#if defined(__GNUC__) && !defined(__clang__) && !defined(_UCRT)
    // (mingw on msvcrt.dll: no _set_abort_behavior there -- and its abort() never calls Windows Error Reporting)
#else
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    if (!GetEnvironmentVariableA("VP_K3_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    SetUnhandledExceptionFilter(unhandled);
    CreateThread(0, 0, watchdog, 0, 0, 0);
    const int episodes = argc > 1 ? atoi(argv[1]) : 1100;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    const int seeds = argc > 3 ? atoi(argv[3]) : 2000;
    const int draws = argc > 4 ? atoi(argv[4]) : 1500;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_krn_util.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    install_stubs();
    // the arena, with a no-access page after it: a runaway copy (a negative length, as in the game) faults there in
    // both passes instead of running on into the harness's own memory
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_SIZE + 0x1000, MEM_COMMIT, PAGE_READWRITE);
    if (!g_arena) return 2;
    DWORD old_prot;
    VirtualProtect(g_arena + ARENA_SIZE, 0x1000, PAGE_NOACCESS, &old_prot);
    uint32_t gb = 0;
    for (const GRange& g : g_globals) gb += g.n;
    if (gb != GLOBALS_BYTES) { printf("GLOBALS_BYTES is %u, should be %u\n", (unsigned)GLOBALS_BYTES, gb); return 2; }
    int nreg = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) nreg++;
    printf("%d rewrites registered\n", nreg);
    const bool verbose = GetEnvironmentVariableA("VP_K3_VERBOSE", 0, 0) != 0;
    g_verbose = GetEnvironmentVariableA("VP_K3_VERBOSE", 0, 0) > 0 && getenv("VP_K3_VERBOSE")[0] == '2';
    for (g_eno = 0; g_eno < episodes; g_eno++) {
        if (verbose) printf("episode %d\n", g_eno);
        run_episode();
    }
    int failed = 0;
    long calls = 0;
    printf("%-58s %7s %7s %7s %7s %7s %7s %7s\n", "function", "calls", "changed", "replay", "panics", "faults", "differ", "fp-miss");
    for (Stat& st : g_stats) {
        printf("%-58s %7ld %7ld %7ld %7ld %7ld %7ld %7ld\n", st.name.c_str(), st.calls, st.changed, st.replay_only, st.panics,
               st.faults, st.fails, st.fp_fails);
        if (st.fails || st.fp_fails) failed++;
        calls += st.calls;
    }
    int unchecked = 0;
    for (ChainReg* r = ChainReg::head(); r; r = r->next) {
        bool seen = false;
        for (Stat& st : g_stats) if (st.name == r->name) seen = true;
        if (!seen) { printf("  never checked: %s\n", r->name); unchecked++; }
    }
    printf("%d episodes, %ld calls; %d of %d checks differ or escape their footprint; %d rewrites never checked\n", episodes, calls,
           failed, (int)g_stats.size(), unchecked);
    printf("covered:");
    for (int i = 0; i < NCOV; i++) printf("%s %s %ld", i ? "," : "", k_cov_names[i], g_cov[i]);
    printf("\n");
    // the long run of the random generator
    long_random_run(seeds, draws);
    printf("random generator: %ld seeds, %ld draws compared value and state after each: %ld differ; Random(n > 0) outside [0, n): %ld\n",
           g_long_seeds, g_long_draws, g_long_bad, g_long_random_oor);
    return failed || g_long_bad ? 1 : 0;
}
