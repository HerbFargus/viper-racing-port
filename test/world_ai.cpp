// world_ai.cpp -- the "ai" library rewrites (hook/phys_ai.cpp: ai.obj, notes.obj, prox.obj, gcviewer.obj) against the
// originals, on random worlds outside the game (docs/PORTING.md, step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_ai.cpp
//        /Fo%TEMP%\wa\ /Fe%TEMP%\wa\world_ai.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_ai.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. A world is one tracked arena holding every object
// the functions touch: 16 AICarInfos and fake cars (a logging vtable whose Teleport slot moves the car), a Proxer
// built by the ORIGINAL Proxer::Begin (its tables on the fake heap) with random positions, velocities and deltas,
// a driver lounge (a fake IDriverLounge: counts and 0x210-byte driver records), skills, car-list entries, CarMgr
// infos, options, set-up and car data, a World, notes blobs (valid headers, crcs made with the image's CRC16, or
// broken ones), an ideal line, and a GhostCarViewer built by the original constructor with random driver ghosts and
// libraries. Every function outside the four object files is a stub that logs its call and answers from a per-call
// script (MemAlloc on the fake heap, the resource and file system, the UI, sprintf through the host's vsprintf,
// munge_carname_to_4char terminated); what's pure code in the image runs as the original (CRC16, qsort, memmove,
// strncpy, stricmp, tolower, AITrackIsReversed, the UIDialogItem constructor, ASSERT_MSG) and so does every
// function of the four files other than the one under test. For each call the original runs, the world (arena,
// heap top, globals) and the log are kept, the world restored, the rewrite runs, and the world, return value, log
// and any fault are compared; every byte the original changed must lie inside the rewrite's footprint (or an AI
// static, for the physics-thread functions) unless it's replay_only. The world continues from the original's result.
//
// The fixes (port.h: VP_FIX): the plain build defines VP_FAITHFUL and checks the rewrites against the originals as
// above. Built with /DFIX_TESTS it compiles the fixed rewrites and runs the same worlds (every one must still equal
// the original), then runs GetUniqueDSTCMunge / GetUniqueDSTCFname with the game's own munge_carname_to_4char on
// mod car names over a stack full of junk: the original's name runs on into the junk, the fixed one stops at the
// four characters (and the stock names and short ones come out as the original's).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>
#include <type_traits>
#ifndef FIX_TESTS
#define VP_FAITHFUL                                 // the original's behaviour, bit for bit (the fixes: /DFIX_TESTS)
#endif
#include "../hook/port.h"

#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                        \
    static const char* const VP_CAT(name_, NEW) = NAME;                                                   \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

// ---- what the rewrites link against, standing in for the DLL ------------------------------------------
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
static int g_fp_unknown;
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) {
    printf("footprint: object() isn't expected here (%s)\n", what);
    g_fp_unknown++;
    add(obj, 4, what);
}

#include "../hook/phys_ai.cpp"

// ---- random values ------------------------------------------------------------------------------------------
static uint32_t g_state = 0x6d2b79f5u;
static uint32_t rnd() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}
static float fbits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t ubits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float rf(float lo, float hi) { return lo + (hi - lo) * uni(); }
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float special() {
    switch (rnd() % 8) {
    case 0: return fbits(0x7fc00000u | (rnd() & 0x3fffff));
    case 1: return fbits(0xffc00000u | (rnd() & 0x3fffff));
    case 2: return fbits(0x7f800001u + (rnd() & 0x3ffffe));                    // signalling NaN
    case 3: return fbits(0x7f800000u | (rnd() & 0x80000000u));
    case 4: return fbits(rnd() & 0x807fffffu);
    case 5: return (rnd() & 1) ? 0.0f : -0.0f;
    case 6: return fbits((rnd() & 0x80000000u) | 0x7f7fffffu);
    default: return fbits(rnd());
    }
}
static int g_special_pct = 2;
static float sp(float x) { return (int)(rnd() % 100) < g_special_pct ? special() : x; }

// ---- the original, loaded at 0x400000 (as test/fuzz.cpp) ---------------------------------------------------
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
    free(file);
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
    return (int)code;
}
static void patch_jmp(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// ---- the world: one tracked arena holding every object, and the globals ------------------------------------
enum {
    ARENA_SIZE = 0x40000,
    OFF_INFO = 0x0000, INFO_STRIDE = 0x80,                         // 16 AICarInfo
    OFF_CARS = 0x0800, CAR_STRIDE = 0x300,                         // 16 fake cars
    OFF_PROXOBJ = 0x3800,                                          // a Proxer object for the ctor/dtor tests
    OFF_DELTAS = 0x3820, NDELTAS = 32,                             // loose ProxerDeltas
    OFF_LIST = 0x3aa0,                                             // 32 delta pointers
    OFF_SKILL = 0x3c00,                                            // 0x300
    OFF_DRIVERS = 0x3f00, DRIVER_STRIDE = 0x220,                   // 16 driver records
    OFF_LOUNGE = 0x6100,
    OFF_OPTS = 0x6120,
    OFF_ENTRIES = 0x6200,                                          // 16 x 0xc8 car-list entries
    OFF_CARMGR = 0x6e80,                                           // 16 x 0x40
    OFF_TRACKINFO = 0x7280,
    OFF_CARLIST = 0x72c0,                                          // 0xc90
    OFF_BUF = 0x8000,                                              // out buffers, 0x800
    OFF_SETUP = 0x8800,
    OFF_CARDATA = 0x8900,
    OFF_WORLDOBJ = 0x8b00,                                         // 0xd00
    OFF_BLOBS = 0x9800, BLOB_STRIDE = 0x400,                       // 4 notes blobs
    OFF_LINE = 0xa800,                                             // an ideal line (0x100)
    OFF_GCV = 0xa900,                                              // GhostCarViewer 0x2558
    OFF_GINTS = 0xcf00,                                            // its three ints
    OFF_GHOSTS = 0xcf20, GHOST_STRIDE = 0xf0, NGHOSTS = 48,        // GhostInfos / SubEntries
    OFF_GFILE = 0xfc40, GFILE_STRIDE = 0x100,                      // 4 ghost files
    OFF_STRS = 0x10100,                                            // strings
    OFF_HEAP = 0x10800, HEAP_END = 0x3f000,
};
static uint8_t* g_arena;
static uint8_t* g_heap_top;

struct GRange { uint32_t at, n; const char* what; };
static const GRange g_globals[] = {
    {0x004eb648, 1, "ai_learn_mode"},     {0x004eb66c, 0x10, "ai count/page/toggle/active"},
    {0x004eb694, 8, "DU, DV"},            {0x004eb8b4, 4, "Lounge"},
    {0x004ec480, 12, "Proxer statics"},   {0x004ec838, 4, "GhostCarViewer::global"},
    {0x00509910, 0xd8, "ai.obj statics"}, {0x00520ad0, 0x48, "notes.obj statics"},
    {0x00521074, 12, "phystask's point"},
};
enum { GLOBALS_BYTES = 1 + 0x10 + 8 + 4 + 12 + 4 + 0xd8 + 0x48 + 12 };
// the AI statics a physics-thread check saves automatically (the first eight ranges)
static bool auto_saved(uint32_t a) {
    for (int i = 0; i < 8; i++)
        if (a >= g_globals[i].at && a < g_globals[i].at + g_globals[i].n) return true;
    return false;
}

static AICarInfo* info(int i) { return (AICarInfo*)(g_arena + OFF_INFO + i * INFO_STRIDE); }
static uint8_t* car(int i) { return g_arena + OFF_CARS + i * CAR_STRIDE; }
static ProxerDelta* delta(int i) { return (ProxerDelta*)(g_arena + OFF_DELTAS + i * 20); }
static uint8_t* driver_rec(int i) { return g_arena + OFF_DRIVERS + (i & 15) * DRIVER_STRIDE; }
static GhostCarViewer* gcv() { return (GhostCarViewer*)(g_arena + OFF_GCV); }
static int32_t* gints() { return (int32_t*)(g_arena + OFF_GINTS); }
static uint8_t* ghost(int i) { return g_arena + OFF_GHOSTS + i * GHOST_STRIDE; }
static uint8_t* gfile(int i) { return g_arena + OFF_GFILE + i * GFILE_STRIDE; }
static uint8_t* blob(int i) { return g_arena + OFF_BLOBS + i * BLOB_STRIDE; }
static char* str(int i) { return (char*)g_arena + OFF_STRS + i * 0x20; }

static AICarInfo** const ai_cars = (AICarInfo**)0x00509920;
static int g_track;                                              // AIGetTrackNumber / WorldGetTrackNumber
static int g_driver_of[16];                                      // AIGetDriverForCar
static int g_lounge_count[8];                                    // the lounge's drivers per strength
static uint16_t g_driver_crc[16];                                // CRC16 of each driver record (the image's)
static const char* const g_track_names[8] = {"gate", "bemidji", "nfield", "kenyon", "lh", "rt", "orient", "sl"};

// ---- the stubs' script and log ---------------------------------------------------------------------------------
struct LogEntry { uint32_t kind, a[5]; };
enum { NLOG = 2048 };
static LogEntry g_log[2][NLOG];
static int g_nlog[2], g_pass;
static uint32_t g_script[64];
static int g_si;
static uint32_t script() { return g_script[g_si++ & 63]; }
static void logn(uint32_t kind, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0) {
    if (g_nlog[g_pass] < NLOG) g_log[g_pass][g_nlog[g_pass]] = {kind, {a0, a1, a2, a3, a4}};
    g_nlog[g_pass]++;
}
// a pointer as logged: the arena's by offset, the image's as is, anything else (the caller's stack) as one tag
static uint32_t pv(const void* p) {
    uintptr_t a = (uintptr_t)p;
    if (!a) return 0;
    if (a >= (uintptr_t)g_arena && a < (uintptr_t)g_arena + ARENA_SIZE) return 0xa0000000u | (uint32_t)(a - (uintptr_t)g_arena);
    if (a >= 0x400000 && a < 0x600000) return (uint32_t)a;
    return 0x5eac0000u;
}
static uint32_t hash_bytes(const void* p, size_t n, uint32_t h = 2166136261u) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
static uint32_t hs(const char* s) {
    if (!s) return 0;
    uint32_t h = 2166136261u;
    __try { for (int i = 0; i < 512 && s[i]; i++) h = (h ^ (uint8_t)s[i]) * 16777619u; }
    __except (EXCEPTION_EXECUTE_HANDLER) { h = 0xbadbad; }
    return h;
}
enum LogKind {
    L_MEMALLOC = 1, L_FREE, L_DELETE, L_PANIC, L_REPORT, L_SPRINTF, L_TRACKNUM, L_DRIVERFOR, L_CARENTRY, L_SKILL,
    L_TRACKINFO, L_CARMGR, L_TRACKNAME, L_WOPTS, L_WTRACKNAME, L_WTRACKNUM, L_GETTRACKNUM, L_MUSTLOAD, L_UNLOAD,
    L_MKDIR, L_FILEREMOVE, L_SETUPRES, L_DEFSETUP, L_CARFILE, L_RESTRY, L_RESFORGET, L_RESWRITE, L_ILINETRY, L_MUNGE,
    L_FILECREATE, L_FILEWRITE, L_FILECLOSE, L_WORLDCTOR, L_LOADRACE, L_DORACE, L_UNLOADRACE, L_SETMAP, L_RESETMAP,
    L_CIL_CTOR, L_CIL_DTOR, L_CIL_LOAD, L_RESETBEAD, L_PROF_OB, L_PROF_OE, L_PROF_START, L_PROF_STOP, L_ADDNOTIF,
    L_REMNOTIF, L_ADDITEMS, L_HIDE, L_SHOW, L_DRAT, L_TIMESTR, L_FILECOPY, L_SETWRITABLE, L_SYSTEM, L_FINDFIRST,
    L_FINDNEXT, L_FINDCLOSE, L_GHOSTLOAD, L_LOUNGE_COUNT, L_LOUNGE_GET, L_TELEPORT, L_BADVIRTUAL,
};
static const char* const g_log_names[] = {
    "?", "MemAlloc", "MemFree", "operator delete", "LogPanic", "LogReport", "sprintf", "AIGetTrackNumber",
    "AIGetDriverForCar", "WorldGetCarEntry", "AIGetSkill", "AIGetTrackInfo", "CarMgrGetInfo", "GetTrackName",
    "WorldGameOptions", "WorldGetTrackname", "WorldGetTrackNumber", "GetTrackNumber", "ResourceSetMustLoad",
    "ResourceSetUnload", "FileCreateDirectory", "FileRemove", "CarFileLoadSetupRes", "CarFileMakeDefaultSetup",
    "CarFileLoad", "ResourceTryDiscardable", "ResourceForget", "ResourceWrite", "ILineTry", "munge_carname_to_4char",
    "FileCreate", "FileWrite", "FileClose", "World::World", "LoadRace", "DoRace", "UnloadRace", "AISetDriverMap",
    "AIResetDriverMap", "ConstIdealLine::ConstIdealLine", "ConstIdealLine::~ConstIdealLine", "ConstIdealLine::load",
    "IdealLine::reset_bead_position", "i_pr_overhead_begin", "i_pr_overhead_end", "i_prof_start", "i_prof_stop",
    "UICustomControl::AddNotification", "UICustomControl::RemoveNotification", "UICustomControl::AddItems",
    "UIHideGroup", "UIShowGroup", "UIDoDratBox", "PhysicsTimeString", "FileCopy", "FileSetWritable", "Win32System",
    "FileFindFirst", "FileFindNext", "FileFindClose", "GhostLoad", "IDriverLounge::Count", "IDriverLounge::Get",
    "Car::Teleport", "(unexpected virtual)",
};

static bool g_alloc_may_fail;
static uint8_t* bump(int n) {
    uint8_t* p = g_heap_top;
    g_heap_top += ((uint32_t)n + 15) & ~15u;
    if (g_heap_top > g_arena + HEAP_END) { printf("the fake heap is full\n"); exit(3); }
    return p;
}
static void* __cdecl stub_MemAlloc(int n) {
    uint32_t s = script();
    if (g_alloc_may_fail && (s & 63) == 0) { logn(L_MEMALLOC, (uint32_t)n, 0); return 0; }
    if (n < 0 || n > 0x10000) { logn(L_MEMALLOC, (uint32_t)n, 0xffffffff); return 0; }
    uint8_t* p = bump(n);
    logn(L_MEMALLOC, (uint32_t)n, pv(p));
    return p;
}
static void __cdecl stub_MemFree(void* p) { logn(L_FREE, pv(p)); }
static void __cdecl stub_delete(void* p) { logn(L_DELETE, pv(p)); }
static void __cdecl stub_panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint32_t a = 0;
    if ((uintptr_t)fmt == 0x4ec65c || (uintptr_t)fmt == 0x4ec62c) a = hs(va_arg(ap, const char*));
    va_end(ap);
    logn(L_PANIC, pv(fmt), a);
}
static void __cdecl stub_report(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint32_t a = 0, b = 0, c = 0;
    if ((uintptr_t)fmt == 0x4ec59c) { a = va_arg(ap, uint32_t); b = va_arg(ap, uint32_t); c = va_arg(ap, uint32_t); }
    va_end(ap);
    logn(L_REPORT, pv(fmt), a, b, c);
}
static int __cdecl stub_sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    logn(L_SPRINTF, pv(fmt), hs(buf), pv(buf) >> 28 == 0xa ? pv(buf) : 0, (uint32_t)n);
    return n;
}
static int __cdecl stub_tracknum() { logn(L_TRACKNUM); return g_track; }
static int __cdecl stub_driver_for(int i) { logn(L_DRIVERFOR, (uint32_t)i); return g_driver_of[i & 15]; }
static const uint8_t* __cdecl stub_car_entry(int i) { logn(L_CARENTRY, (uint32_t)i); return g_arena + OFF_ENTRIES + (i & 15) * 0xc8; }
static const uint8_t* __cdecl stub_skill(int i) { logn(L_SKILL, (uint32_t)i); return g_arena + OFF_SKILL; }
static const uint8_t* __cdecl stub_trackinfo(int i) { logn(L_TRACKINFO, (uint32_t)i); return g_arena + OFF_TRACKINFO; }
static const uint8_t* __cdecl stub_carmgr(int i) { logn(L_CARMGR, (uint32_t)i); return g_arena + OFF_CARMGR + (i & 15) * 0x40; }
static const char* __cdecl stub_trackname(int i) { logn(L_TRACKNAME, (uint32_t)i); return g_track_names[i & 7]; }
static const uint8_t* __cdecl stub_wopts() { logn(L_WOPTS); return g_arena + OFF_OPTS; }
static const char* __cdecl stub_wtrackname() { logn(L_WTRACKNAME); return g_track_names[g_track & 7]; }
static int __cdecl stub_wtracknum() { logn(L_WTRACKNUM); return g_track; }
static int __cdecl stub_gettracknum(const char* s) { uint32_t r = script(); logn(L_GETTRACKNUM, hs(s), r % 8); return (int)(r % 8); }
static void __cdecl stub_mustload(const char* s) { logn(L_MUSTLOAD, hs(s)); }
static void __cdecl stub_unload(const char* s) { logn(L_UNLOAD, hs(s)); }
static uint8_t __cdecl stub_mkdir(const char* s) { logn(L_MKDIR, hs(s)); return 1; }
static uint8_t __cdecl stub_fileremove(const char* s) { logn(L_FILEREMOVE, hs(s)); return (uint8_t)(script() & 1); }
static void fill_script(uint8_t* p, int n, uint32_t seed) {         // deterministic bytes from a script word
    uint32_t x = seed | 1;
    for (int i = 0; i < n; i += 4) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        float f = (float)(int)(x % 2000) * 0.37f - 300.0f;
        if ((x >> 20) % 50 == 0) f = fbits(x);
        memcpy(p + i, &f, 4 <= n - i ? 4 : n - i);
    }
}
static uint8_t __cdecl stub_setupres(uint8_t* sd, const char* name) {
    uint32_t s = script();
    logn(L_SETUPRES, pv(sd), hs(name), s % 3 != 0);
    if (s % 3 == 0) return 0;
    fill_script(sd, 0x8c, s);
    return 1;
}
static void __cdecl stub_defsetup(uint8_t* p) { uint32_t s = script(); logn(L_DEFSETUP, pv(p)); fill_script(p, 0x8c, s); }
static uint8_t __cdecl stub_carfile(uint8_t* cf, const char* name, uint8_t* flag) {
    uint32_t s = script();
    logn(L_CARFILE, hs(name), pv(flag), s % 4 != 0);
    if (s % 4 == 0) return 0;
    fill_script(cf, 0x228, s);
    int32_t gears = (s >> 8) % 4 == 0 ? (int32_t)((s >> 12) % 9) - 1 : 5;
    memcpy(cf + 0x108, &gears, 4);
    return 1;
}
// notes blobs: the world prepares four; a lookup answers with one of them (or none)
static int g_blob_version[4];
static const void* __cdecl stub_restry(const char* name, uint32_t type, uint32_t* ver, int* x) {
    uint32_t s = script();
    int which = (int)(s % 6);
    logn(L_RESTRY, hs(name), type, pv(x), (uint32_t)which);
    if (which >= 4) return 0;
    *ver = (uint32_t)g_blob_version[which];
    return blob(which);
}
static uint8_t __cdecl stub_resforget(const void* p) { logn(L_RESFORGET, pv(p)); return 1; }
static void __cdecl stub_reswrite(int fd, uint32_t type, uint32_t ver) { logn(L_RESWRITE, (uint32_t)fd, type, ver); }
#ifdef FIX_TESTS
static bool g_ilinetry_always;                         // (the fix test: every line found -- no line is another crash)
#endif
static void* __cdecl stub_ilinetry(const char* name, uint32_t rev) {
    uint32_t s = script();
    logn(L_ILINETRY, hs(name), rev & 0xff, s % 3 != 0);
#ifdef FIX_TESTS
    if (g_ilinetry_always) return g_arena + OFF_LINE;
#endif
    return s % 3 ? g_arena + OFF_LINE : 0;
}
static void __cdecl stub_munge(char* out, const char* name) {
    logn(L_MUNGE, hs(name));
    int i = 0;
    for (; i < 4 && name[i]; i++) out[i] = name[i];
    out[i] = 0;
}
static int __cdecl stub_filecreate(const char* name) { uint32_t s = script(); logn(L_FILECREATE, hs(name)); return s % 4 ? 0x100 + (int)(s % 7) : 0; }
#ifdef FIX_TESTS
static uint8_t g_cap_world[0xcd4], g_cap_hdr[0x40];     // (the fix test: the World LoadRace got, the notes header)
static uint32_t g_cap_world_seed;
static int g_cap_n;
#endif
static uint8_t __cdecl stub_filewrite(int fd, const void* data, int n) {
#ifdef FIX_TESTS
    if (n == 0x40) { memcpy(g_cap_hdr, data, 0x40); g_cap_n++; }
#endif
    uint32_t h;
    if (n == 0x40 && pv(data) == 0x5eac0000u) {                    // the notes header: its defined bytes
        const uint8_t* b = (const uint8_t*)data;
        h = hash_bytes(b, 0x14);
        h = hash_bytes(b + 0x14, strnlen((const char*)b + 0x14, 0x20), h);
        h = hash_bytes(b + 0x34, 0xc, h);
    } else {
        h = pv(data) >> 28 == 0xa && n > 0 && n < 0x1000 ? hash_bytes(data, (size_t)n) : pv(data);
    }
    logn(L_FILEWRITE, (uint32_t)fd, (uint32_t)n, h);
    return 1;
}
static void __cdecl stub_fileclose(int* fd) { logn(L_FILECLOSE, (uint32_t)*fd); *fd = 0; }
static void* __fastcall stub_world_ctor(uint8_t* w, int) {
    uint32_t s = script();
    logn(L_WORLDCTOR, pv(w));
    fill_script(w, 0xcd4, s);
#ifdef FIX_TESTS
    g_cap_world_seed = s;
#endif
    return w;
}
static void __cdecl stub_loadrace(const uint8_t* w) {
    logn(L_LOADRACE, hash_bytes(w, 0xcd4));
#ifdef FIX_TESTS
    memcpy(g_cap_world, w, 0xcd4);
#endif
}
static bool g_dorace_aborts;
static void __cdecl stub_dorace(const uint8_t* w, void* r, void* cb) {
    uint32_t s = script();
    logn(L_DORACE, *(uint8_t*)0x004eb648, pv(r), pv(cb), hash_bytes(w, 0xcd4));
    if (s % 5) {                                                   // the car drops off its notes
        uint8_t* p = bump(0x40);
        fill_script(p, 0x40, s);
        *(void**)0x00520b10 = p;
        *(int32_t*)0x00520b14 = (int32_t)(s % 0x40);
    }
    if (g_dorace_aborts && (s >> 8) % 4 == 0) *(uint8_t*)0x00520ad0 = 1;
}
static void __cdecl stub_unloadrace(const uint8_t* w) { logn(L_UNLOADRACE, hash_bytes(w, 0xcd4)); }
static void __cdecl stub_setmap(int* p, int n) {
    uint32_t s = script();
    logn(L_SETMAP, (uint32_t)*p, (uint32_t)n);
    if (s % 5 == 0) *p = (int)(s >> 8) % 16;                        // (the map may be written through)
}
static void __cdecl stub_resetmap() { logn(L_RESETMAP); }
static void* g_cil_vtable[8];
static void* __fastcall stub_cil_ctor(uint8_t* self, int) { logn(L_CIL_CTOR, pv(self)); *(void***)self = g_cil_vtable; return self; }
static void __fastcall stub_cil_dtor(uint8_t* self, int) { logn(L_CIL_DTOR, pv(self)); }
static uint8_t __fastcall stub_cil_load(uint8_t* self, int, const char* name, uint32_t rev) {
    uint32_t s = script();
    logn(L_CIL_LOAD, pv(self), hs(name), rev, s % 3 == 0);
    return s % 3 == 0;
}
static void __fastcall stub_resetbead(uint8_t* self, int) { logn(L_RESETBEAD, pv(self)); }
static void __cdecl stub_prof_ob() { logn(L_PROF_OB); }
static void __cdecl stub_prof_oe() { logn(L_PROF_OE); }
static int __cdecl stub_prof_start(const char* s) { uint32_t r = script(); logn(L_PROF_START, hs(s), r); return (int)r; }
static void __cdecl stub_prof_stop(int id) { logn(L_PROF_STOP, (uint32_t)id); }
static void __fastcall stub_addnotif(void* self, int, int a, const void* p, unsigned n) { logn(L_ADDNOTIF, pv(self), (uint32_t)a, pv(p), n); }
static void __fastcall stub_remnotif(void* self, int, const void* p) { logn(L_REMNOTIF, pv(self), pv(p)); }
static void __fastcall stub_additems(void* self, int, const uint8_t* items) { logn(L_ADDITEMS, pv(self), hash_bytes(items, 20 * 56)); }
static void __cdecl stub_hide(unsigned g) { logn(L_HIDE, g); }
static void __cdecl stub_show(unsigned g) { logn(L_SHOW, g); }
static void __cdecl stub_drat(const char* a, const char* b, void* c) { logn(L_DRAT, pv(a), pv(b), pv(c)); }
static char g_timestr[32];
static const char* __cdecl stub_timestr(uint32_t bits, uint32_t flag) {
    logn(L_TIMESTR, bits, flag & 0xff);
    sprintf(g_timestr, "t%08x", bits);
    return g_timestr;
}
static uint8_t __cdecl stub_filecopy(const char* a, const char* b) { uint32_t s = script(); logn(L_FILECOPY, hs(a), hs(b)); return s % 4 != 0; }
static void __cdecl stub_setwritable(const char* a, uint32_t b) { logn(L_SETWRITABLE, hs(a), b & 0xff); }
static uint8_t __cdecl stub_system(int* a, int* code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const char* s1 = va_arg(ap, const char*);
    const char* s2 = va_arg(ap, const char*);
    va_end(ap);
    uint32_t s = script();
    logn(L_SYSTEM, pv(fmt), hs(s1), hs(s2), (uint32_t)*code);
    *a = 7;
    *code = (int)(s % 3) - 1;
    return (s >> 4) % 4 != 0;
}
static int g_find_left;
static void find_name(char* out, uint32_t s) {
    if ((s >> 16) % 4 == 0) { sprintf(out, "zz%02x.tmp", s & 0xff); return; }
    static const char hex[] = "0123456789abcdefABCDEF";
    sprintf(out, "%c%c%c%c%s.gcf", hex[s % 22], hex[(s >> 5) % 22], hex[(s >> 10) % 22], 'a' + (s >> 20) % 26, "vip");
}
static void* __cdecl stub_findfirst(const char* pat, char* out, int n) {
    uint32_t s = script();
    logn(L_FINDFIRST, hs(pat), (uint32_t)n);
    if (s % 4 == 0) return (void*)-1;
    g_find_left = (int)((s >> 4) % 5);
    find_name(out, script());
    return (void*)0x77;
}
static uint8_t __cdecl stub_findnext(void* h, char* out, int n) {
    logn(L_FINDNEXT, (uint32_t)(uintptr_t)h, (uint32_t)n, (uint32_t)g_find_left);
    if (g_find_left <= 0) return 0;
    g_find_left--;
    find_name(out, script());
    return 1;
}
static void __cdecl stub_findclose(void* h) { logn(L_FINDCLOSE, (uint32_t)(uintptr_t)h); }
static void* __cdecl stub_ghostload(const char* path, uint32_t flag) {
    uint32_t s = script();
    logn(L_GHOSTLOAD, hs(path), flag & 0xff, s % 4 != 0);
    if (s % 4 == 0) return 0;
    uint8_t* g = bump(0x100);
    fill_script(g, 0x100, s);
    strcpy((char*)g + 0x10, g_track_names[s % 8]);
    g[0x1d] = (uint8_t)((s >> 3) & 1);
    strcpy((char*)g + 0x24, "ghostname");
    return g;
}
static int __fastcall stub_lounge_count(void* self, int, int strength) {
    logn(L_LOUNGE_COUNT, pv(self), (uint32_t)strength);
    return g_lounge_count[strength & 7];
}
static const uint8_t* __fastcall stub_lounge_get(void* self, int, int strength, int driver) {
    logn(L_LOUNGE_GET, pv(self), (uint32_t)strength, (uint32_t)driver);
    return driver_rec(strength * 3 + driver);
}
static void __fastcall stub_teleport(uint8_t* c, int, const float* pos, const Point2D* dir) {
    logn(L_TELEPORT, pv(c), ubits(pos[0]), ubits(pos[1]), ubits(pos[2]), ubits(dir->x) ^ ubits(dir->z));
    memcpy(c + 0x5c, pos, 12);
    memset(c + 0x234, 0, 12);
}
static int __fastcall stub_bad_virtual(void* self, int) { logn(L_BADVIRTUAL, pv(self)); return 0; }
static void* g_car_vtable[32];
static void* g_lounge_vtable[8];

static void install_stubs() {
    struct { uint32_t at; void* to; } s[] = {
        {0x004140e0, (void*)&stub_MemAlloc}, {0x00414300, (void*)&stub_MemFree}, {0x00414390, (void*)&stub_delete},
        {0x004112b0, (void*)&stub_panic}, {0x00411150, (void*)&stub_report}, {0x004cf0a0, (void*)&stub_sprintf},
        {0x00420df0, (void*)&stub_tracknum}, {0x00420ce0, (void*)&stub_driver_for}, {0x00462720, (void*)&stub_car_entry},
        {0x00420d90, (void*)&stub_skill}, {0x00424e40, (void*)&stub_trackinfo}, {0x00464490, (void*)&stub_carmgr},
        {0x00406810, (void*)&stub_trackname}, {0x004627a0, (void*)&stub_wopts}, {0x00462780, (void*)&stub_wtrackname},
        {0x00462760, (void*)&stub_wtracknum}, {0x004068b0, (void*)&stub_gettracknum}, {0x00419a30, (void*)&stub_mustload},
        {0x00419bb0, (void*)&stub_unload}, {0x00411d00, (void*)&stub_mkdir}, {0x00411ba0, (void*)&stub_fileremove},
        {0x00465280, (void*)&stub_setupres}, {0x00465780, (void*)&stub_defsetup}, {0x004647d0, (void*)&stub_carfile},
        {0x00419f40, (void*)&stub_restry}, {0x0041a450, (void*)&stub_resforget}, {0x0041a5c0, (void*)&stub_reswrite},
        {0x004211d0, (void*)&stub_ilinetry}, {0x0040dc70, (void*)&stub_munge}, {0x004115f0, (void*)&stub_filecreate},
        {0x00411a30, (void*)&stub_filewrite}, {0x00411850, (void*)&stub_fileclose}, {0x00462a90, (void*)&stub_world_ctor},
        {0x0040a840, (void*)&stub_loadrace}, {0x00401e10, (void*)&stub_dorace}, {0x0040a870, (void*)&stub_unloadrace},
        {0x00420c50, (void*)&stub_setmap}, {0x00420c30, (void*)&stub_resetmap}, {0x00422370, (void*)&stub_cil_ctor},
        {0x00422390, (void*)&stub_cil_dtor}, {0x00421520, (void*)&stub_resetbead}, {0x0047e840, (void*)&stub_addnotif},
        {0x0047e860, (void*)&stub_remnotif}, {0x0047e870, (void*)&stub_additems}, {0x004791b0, (void*)&stub_hide},
        {0x004791e0, (void*)&stub_show}, {0x00479280, (void*)&stub_drat}, {0x0042bf30, (void*)&stub_timestr},
        {0x00411e40, (void*)&stub_filecopy}, {0x00411b80, (void*)&stub_setwritable}, {0x00412d90, (void*)&stub_system},
        {0x00411c20, (void*)&stub_findfirst}, {0x00411c80, (void*)&stub_findnext}, {0x00411cd0, (void*)&stub_findclose},
        {0x0040ebe0, (void*)&stub_ghostload},
    };
    for (auto& x : s) patch_jmp(x.at, x.to);
    *(void**)0x004e642c = (void*)&stub_prof_ob;
    *(void**)0x004e6430 = (void*)&stub_prof_oe;
    *(void**)0x004e6434 = (void*)&stub_prof_start;
    *(void**)0x004e6438 = (void*)&stub_prof_stop;
    for (int i = 0; i < 8; i++) g_cil_vtable[i] = (void*)&stub_bad_virtual;
    g_cil_vtable[1] = (void*)&stub_cil_load;
    for (int i = 0; i < 32; i++) g_car_vtable[i] = (void*)&stub_bad_virtual;
    g_car_vtable[0x40 / 4] = (void*)&stub_teleport;
    for (int i = 0; i < 8; i++) g_lounge_vtable[i] = (void*)&stub_bad_virtual;
    g_lounge_vtable[0] = (void*)&stub_lounge_count;
    g_lounge_vtable[1] = (void*)&stub_lounge_get;
}

// ---- running one call both ways ----------------------------------------------------------------------------
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

struct Stat { const char* name; long calls, changed, fails, fp_fails, faults, replay_only; long first_fail; long ret[4]; };
static Stat g_stats[128];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    g_stats[g_nstats] = {name, 0, 0, 0, 0, 0, 0, -1, {0, 0, 0, 0}};
    return g_stats[g_nstats++];
}

struct Snapshot { uint8_t arena[ARENA_SIZE]; uint8_t globals[GLOBALS_BYTES]; uint8_t* heap_top; int find_left; };
static Snapshot g_start, g_after;
static Footprint g_fp;
static int g_world;
static const char* g_phase = "";
static bool g_main_thread;                                         // the function runs on the main thread
static uint64_t g_ret_mask = ~0ull;
static int g_pc = _PC_24;

static void save(Snapshot& s) {
    memcpy(s.arena, g_arena, ARENA_SIZE);
    uint8_t* p = s.globals;
    for (const GRange& g : g_globals) { memcpy(p, (void*)(uintptr_t)g.at, g.n); p += g.n; }
    s.heap_top = g_heap_top;
    s.find_left = g_find_left;
}
static void load(const Snapshot& s) {
    memcpy(g_arena, s.arena, ARENA_SIZE);
    const uint8_t* p = s.globals;
    for (const GRange& g : g_globals) { memcpy((void*)(uintptr_t)g.at, p, g.n); p += g.n; }
    g_heap_top = s.heap_top;
    g_find_left = s.find_left;
}
static void snap_globals(uint8_t* p) {
    for (const GRange& g : g_globals) { memcpy(p, (void*)(uintptr_t)g.at, g.n); p += g.n; }
}
static bool in_footprint(const void* p) {
    for (int k = 0; k < g_fp.n; k++)
        if ((const uint8_t*)p >= (const uint8_t*)g_fp.r[k].p && (const uint8_t*)p < (const uint8_t*)g_fp.r[k].p + g_fp.r[k].n) return true;
    return false;
}
static const char* region_of(uint32_t off, uint32_t* rel) {
    static const struct { uint32_t at; const char* name; } r[] = {
        {OFF_INFO, "AICarInfo"}, {OFF_CARS, "car"}, {OFF_PROXOBJ, "Proxer object"}, {OFF_DELTAS, "deltas"},
        {OFF_LIST, "delta list"}, {OFF_SKILL, "skill"}, {OFF_DRIVERS, "drivers"}, {OFF_LOUNGE, "lounge"},
        {OFF_OPTS, "options"}, {OFF_ENTRIES, "car entries"}, {OFF_CARMGR, "carmgr"}, {OFF_TRACKINFO, "trackinfo"},
        {OFF_CARLIST, "car list"}, {OFF_BUF, "buffers"}, {OFF_SETUP, "set-up"}, {OFF_CARDATA, "car data"},
        {OFF_WORLDOBJ, "World"}, {OFF_BLOBS, "notes blobs"}, {OFF_LINE, "line"}, {OFF_GCV, "GhostCarViewer"},
        {OFF_GINTS, "viewer ints"}, {OFF_GHOSTS, "ghosts"}, {OFF_GFILE, "ghost files"}, {OFF_STRS, "strings"},
        {OFF_HEAP, "heap"}, {ARENA_SIZE, 0},
    };
    int i = 0;
    while (r[i + 1].name && off >= r[i + 1].at) i++;
    *rel = off - r[i].at;
    return r[i].name;
}

template <typename Run, typename Fp> static void check(const char* name, Run run, Fp footprint) {
    Stat& st = stat(name);
    st.calls++;
    for (int i = 0; i < 64; i++) g_script[i] = rnd();
    save(g_start);
    g_fp.n = 0; g_fp.replay_only = 0; g_fp.pure = false;
    footprint(g_fp);
    if (g_fp.replay_only) st.replay_only++;
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, (unsigned)g_pc, _MCW_PC);
    g_pass = 0; g_nlog[0] = 0; g_si = 0;
    uint64_t ro = 0, rn = 0;
    int fo = 0, fn = 0;
    __try { ro = run(true); } __except (EXCEPTION_EXECUTE_HANDLER) { fo = 1; }
    save(g_after);
    if (memcmp(g_after.arena, g_start.arena, ARENA_SIZE) || memcmp(g_after.globals, g_start.globals, GLOBALS_BYTES)) st.changed++;
    load(g_start);
    g_pass = 1; g_nlog[1] = 0; g_si = 0;
    _clearfp();
    _controlfp_s(&cw, (unsigned)g_pc, _MCW_PC);
    __try { rn = run(false); } __except (EXCEPTION_EXECUTE_HANDLER) { fn = 1; }
    _controlfp_s(&cw, _PC_53, _MCW_PC);
    ro &= g_ret_mask;
    rn &= g_ret_mask;
    if (fo || fn) {
        st.faults++;
        if (fo != fn) {
            if (st.first_fail < 0) st.first_fail = st.calls;
            if (st.fails++ < 4) printf("MISMATCH %s (world %d, %s): original %s, rewrite %s\n", name, g_world, g_phase,
                                       fo ? "faulted" : "ran", fn ? "faulted" : "ran");
        }
        load(g_after);
        return;
    }
    st.ret[(uint32_t)ro == 0xffffffffu ? 0 : ro == 0 ? 1 : ro == 1 ? 2 : 3]++;   // the return values seen: -1, 0, 1, other
    uint8_t now_globals[GLOBALS_BYTES];
    snap_globals(now_globals);
    int nlog0 = g_nlog[0] < NLOG ? g_nlog[0] : NLOG;
    bool bad = ro != rn || memcmp(g_after.arena, g_arena, ARENA_SIZE) || memcmp(g_after.globals, now_globals, GLOBALS_BYTES) ||
               g_after.heap_top != g_heap_top || g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], sizeof(LogEntry) * nlog0);
    if (bad) {
        if (st.first_fail < 0) st.first_fail = st.calls;
        if (st.fails++ < 4) {
            printf("MISMATCH %s (world %d, %s)\n", name, g_world, g_phase);
            if (ro != rn) printf("  return: original %016llx, rewrite %016llx\n", ro, rn);
            int shown = 0;
            for (uint32_t i = 0; i < ARENA_SIZE && shown < 12; i += 4)
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
                if (memcmp(pa, pb, g.n)) printf("  global %s (%08x) differs\n", g.what, g.at);
                pa += g.n; pb += g.n;
            }
            if (g_after.heap_top != g_heap_top) printf("  the heap top differs\n");
            int nl = g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1];
            if (nl > NLOG) nl = NLOG;
            for (int i = 0, k = 0; i < nl && k < 10; i++) {
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
        for (uint32_t i = 0; i < ARENA_SIZE; i++) {
            if (g_after.arena[i] == g_start.arena[i] || in_footprint(g_arena + i)) continue;
            uint32_t rel;
            const char* r = region_of(i, &rel);
            if (st.fp_fails++ < 3) printf("FOOTPRINT %s (world %d, %s): %s+0x%x changed outside it\n", name, g_world, g_phase, r, rel);
            break;
        }
        const uint8_t* pa = g_after.globals;
        const uint8_t* ps = g_start.globals;
        for (const GRange& g : g_globals) {
            for (uint32_t i = 0; i < g.n; i++)
                if (pa[i] != ps[i] && !in_footprint((void*)(uintptr_t)(g.at + i)) && (g_main_thread || !auto_saved(g.at + i))) {
                    if (st.fp_fails++ < 3) printf("FOOTPRINT %s (world %d, %s): global %s changed outside it\n", name, g_world, g_phase, g.what);
                    break;
                }
            pa += g.n; ps += g.n;
        }
        if (g_after.heap_top != g_start.heap_top && st.fp_fails++ < 3)
            printf("FOOTPRINT %s (world %d, %s): allocates but isn't replay_only\n", name, g_world, g_phase);
    }
    load(g_after);
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

// ---- building a world ----------------------------------------------------------------------------------------
static int g_cov[16];
static void rand_bytes(void* p, size_t n) { uint8_t* b = (uint8_t*)p; for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd(); }
static void rand_floats(void* p, size_t n, float lo, float hi) {
    float* f = (float*)p;
    for (size_t i = 0; i < n / 4; i++) f[i] = sp(rf(lo, hi));
}
static void rand_name(char* s, int maxlen) {
    int n = ri(1, maxlen);
    for (int i = 0; i < n; i++) s[i] = (char)('a' + rnd() % 26);
    s[n] = 0;
}
static Proxer* proxer() { return *(Proxer**)0x004ec484; }

static void build_world() {
    rand_bytes(g_arena, ARENA_SIZE);
    g_heap_top = g_arena + OFF_HEAP;
    g_alloc_may_fail = false;
    g_track = ri(0, 15);
    for (int i = 0; i < 16; i++) g_driver_of[i] = ri(0, 4);
    for (int i = 0; i < 8; i++) g_lounge_count[i] = ri(0, 3);
    // cars and infos
    for (int i = 0; i < 16; i++) {
        uint8_t* c = car(i);
        *(void***)c = g_car_vtable;
        rand_floats(c + 0x38, 48, -500, 500);
        rand_floats(c + 0x234, 12, -80, 80);
        AICarInfo* ci = info(i);
        rand_floats(ci, 16, -500, 500);
        *(void***)ci->line = g_cil_vtable;
        ci->car = c;
        ci->index = i;
        ci->aicar = chance(50) ? c : 0;
        ai_cars[i] = chance(80) ? ci : 0;
    }
    // the lounge, the drivers and their crcs
    *(void***)(g_arena + OFF_LOUNGE) = g_lounge_vtable;
    *(void**)0x004eb8b4 = g_arena + OFF_LOUNGE;
    for (int i = 0; i < 16; i++) {
        uint8_t* d = driver_rec(i);
        rand_bytes(d, DRIVER_STRIDE);
        rand_name((char*)d + 0x70, 7);
        rand_floats(d + 0x2c, 8, 0.5f, 150.0f);
        if (chance(30)) *(float*)(d + 0x2c) = (float)(ri(99, 101));
        if (chance(30)) *(float*)(d + 0x30) = ri(0, 1) ? 1.0f : fbits(0x3f800001);
        rand_name(str(16 + i), 7);
        *(char**)(d + 0x210) = str(16 + i);
        g_driver_crc[i] = ((uint16_t(__cdecl*)(const void*, int))0x004a3d70)(d, 0x210);
    }
    rand_floats(g_arena + OFF_SKILL, 0x300, 0.1f, 3.0f);
    rand_bytes(g_arena + OFF_OPTS, 0x40);
    g_arena[OFF_OPTS + 0x25] = (uint8_t)(chance(50) ? 0 : 1);
    *(int32_t*)(g_arena + OFF_OPTS + 0x1c) = chance(50) ? 0 : ri(1, 3);
    for (int i = 0; i < 16; i++) {
        rand_name((char*)g_arena + OFF_ENTRIES + i * 0xc8 + 0x11, 8);
        rand_name((char*)g_arena + OFF_CARMGR + i * 0x40 + 0x12, 8);
    }
    rand_bytes(g_arena + OFF_TRACKINFO, 0x20);
    *(int32_t*)(g_arena + OFF_CARLIST + 0xc80) = ri(1, 16);
    // statics
    *(uint8_t*)0x004eb648 = (uint8_t)ri(0, 1);
    *(int32_t*)0x004eb66c = ri(0, 16);
    *(int32_t*)0x004eb670 = ri(-1, 4);
    *(uint8_t*)0x004eb674 = (uint8_t)ri(0, 1);
    *(float*)0x004eb694 = sp(rf(-1, 1));
    *(float*)0x004eb698 = sp(rf(-1, 1));
    *(AICarInfo**)0x00509984 = info(ri(0, 15));
    *(int32_t*)0x00509990 = ri(0, 7);
    rand_floats((void*)0x005099b8, 12, -500, 500);
    rand_bytes((void*)0x00521074, 12);
    *(uint8_t*)0x00520ad0 = 0;
    // the Proxer, built by the original
    *(Proxer**)0x004ec484 = 0;
    ((void(__cdecl*)(int))0x004234b0)(ri(2, 16));
    Proxer* p = proxer();
    if (p) {
        for (int i = 0; i < p->n; i++) rand_floats(p->records + p->record_size * i, 16, -500, 500);
        int m = p->n * (p->n - 1) / 2;
        for (int k = 0; k < m; k++) {
            rand_floats(&p->deltas[k].dl, 16, -50, 50);
            if (chance(20)) { p->deltas[k].dl.x *= 0.05f; p->deltas[k].dl.z *= 0.05f; }
        }
    }
    // notes blobs: valid headers for some (driver, strength, track, car), broken for others
    for (int b = 0; b < 4; b++) {
        uint8_t* bl = blob(b);
        rand_bytes(bl, BLOB_STRIDE);
        NotesHeader* h = (NotesHeader*)bl;
        g_blob_version[b] = chance(85) ? 7 : 6;
        h->version = chance(95) ? 7 : 8;
        h->size = chance(95) ? 0x40 : 0x44;
        h->track = ri(0, 3);
        h->driver = ri(0, 3);
        h->strength = ri(0, 3);
        strcpy(h->car, chance(80) ? "carA" : "CARA");
        h->driver_crc = chance(80) ? g_driver_crc[(h->strength * 3 + h->driver) & 15] : (uint16_t)rnd();
        h->nodes = chance(90) ? ri(1, 12) : 0;
        h->datasize = chance(85) ? h->nodes * 8 : ri(0, 100);
    }
    // the ideal line: nodes at +8, eight crc'd bytes at +0xdc
    rand_bytes(g_arena + OFF_LINE, 0x100);
    *(int32_t*)(g_arena + OFF_LINE + 8) = ri(1, 12);
    uint16_t line_crc = ((uint16_t(__cdecl*)(const void*, int))0x004a3d70)(g_arena + OFF_LINE + 0xdc, 8);
    for (int b = 0; b < 4; b++)
        if (chance(70)) {
            NotesHeader* h = (NotesHeader*)blob(b);
            h->ili_crc = line_crc;
            h->nodes = *(int32_t*)(g_arena + OFF_LINE + 8);
            h->datasize = chance(90) ? h->nodes * 8 : h->nodes * 8 + 1;
        }
    strcpy(str(0), "carA");
    strcpy(str(1), "vipr");
    rand_name(str(2), 10);
    // the ghost viewer, built by the original constructor
    int32_t* gi = gints();
    gi[0] = ri(0, 15); gi[1] = ri(0, 7); gi[2] = ri(0, 15);
    ((GhostCarViewer*(__fastcall*)(GhostCarViewer*, int, int32_t*, int32_t*, int32_t*))0x00425f30)(gcv(), 0, &gi[0], &gi[1], &gi[2]);
    GhostCarViewer* v = gcv();
    v->x = ri(0, 600); v->y = ri(0, 400);
    v->g_library = 0x100 + rnd() % 16; v->g_scroll = 0x200 + rnd() % 16; v->g_main = 0x300 + rnd() % 16; v->g_load = 0x400 + rnd() % 16;
    int ng = 0;
    for (int k = 0; k < NGHOSTS; k++) {
        uint8_t* g = ghost(k);
        rand_bytes(g, GHOST_STRIDE);
        rand_name((char*)g, 10);                                   // a SubEntry's file name
        rand_name((char*)g + 0x24, 12);                            // the GhostInfo's name (+0x20 + 4)
        *(float*)(g + 0xe8) = sp(rf(30, 120));
        rand_name((char*)g + 4, 12);                               // (as a bare GhostInfo: +4)
    }
    for (int k = 0; k < 64; k++) v->ghosts[rnd() % 0x800] = (GhostInfo*)ghost(ng++ % NGHOSTS);
    v->ghosts[(((gi[1] << 4) + gi[0]) << 4) + gi[2]] = chance(50) ? (GhostInfo*)ghost(3) : 0;
    for (int t = 0; t < 16; t++) {
        Submits* s = &v->subs[t];
        s->count = chance(20) ? 0 : ri(1, chance(10) ? 16 : 6);
        for (int j = 0; j < 16; j++) s->items[j] = j < s->count ? (SubEntry*)ghost(ng++ % NGHOSTS) : 0;
        // sorted by time, mostly
        for (int j = 1; j < s->count; j++)
            if (chance(90)) *(float*)((uint8_t*)s->items[j] + 0xe8) = *(float*)((uint8_t*)s->items[j - 1] + 0xe8) + rf(0, 5);
        s->current = s->count ? ri(chance(5) ? -1 : 0, s->count - (chance(5) ? 0 : 1)) : 0;
    }
    v->loaded = (uint8_t)chance(20);
    for (int b = 0; b < 4; b++) {
        uint8_t* g = gfile(b);
        rand_bytes(g, GFILE_STRIDE);
        strcpy((char*)g + 0x10, g_track_names[rnd() % 8]);
        g[0x1d] = (uint8_t)ri(0, 1);
        *(float*)(g + 0xe8) = sp(rf(30, 120));
    }
    *(GhostCarViewer**)0x004ec838 = v;
}

// ---- the tests ------------------------------------------------------------------------------------------------------
static void test_ai() {
    g_phase = "ai";
    g_main_thread = false;
    for (int r = 0; r < 4; r++) {
        int i = ri(0, 15);
        CHECK(AICarInfo_UpdatePos, info(i), 0);
    }
    CHECK0(AIResetCars);
    CHECK0(AIBGUpdate);
    for (int r = 0; r < 3; r++) {
        int i = ri(0, 15);
        CHECK(AICarInfo_init, info(i), 0, i);
    }
    {
        int i = ri(0, 15);
        CHECK(AICarInfo_ctor_car, info(i), 0, (void*)car(i), i);
        i = ri(0, 15);
        CHECK(AICarInfo_ctor_aicar, info(i), 0, (void*)car(i), i);
    }
    for (int r = 0; r < 6; r++) {
        char* buf = (char*)g_arena + OFF_BUF;
        CHECK(IterateILIname, buf, (uint8_t*)buf + 0x100, ri(-1, 6), ri(0, 5), ri(-2, 17), ri(0, 7), (const char*)str(ri(0, 2)));
    }
    CHECK(AIGetLine, ri(0, 15));
    CHECK(IsAI, ri(0, 15));
    CHECK0(AIGetStrength);
    CHECK0(AICarCount);
    CHECK0(AILearnMode);
    CHECK0(AIGetMaxGhostSubmitTicks);
    g_alloc_may_fail = true;
    for (int r = 0; r < 2; r++) {
        int i = ri(0, 15);
        CHECK(AIRegisterAICar, (void*)car(i), i);
        i = ri(0, 15);
        CHECK(AIRegisterPlayCar, (void*)car(i), i);
        i = ri(0, 15);
        CHECK(AIRegisterNetCar, (void*)car(i), i);
        i = ri(0, 15);
        CHECK(AIUnregisterCar, (void*)car(i), i);
    }
    g_alloc_may_fail = false;
    g_main_thread = true;
    CHECK(AISetStrength, ri(-2, 9));
    // the dashboard keys
    static const uint16_t keys[] = {0x3f, 0x68, 0x69, 0x6c, 0x73, 0x30, 0x42, 0x51, 0x56, 0x57, 0x62, 0x71, 0x76, 0x77,
                                    0x48, 0x67, 0x47, 0x31, 0x20, 0x1b, 0x148, 0xff67};
    for (int r = 0; r < 12; r++) {
        if (chance(30)) *(int32_t*)0x004eb670 = ri(-1, 4);
        uint16_t k = chance(85) ? keys[rnd() % (sizeof keys / 2)] : (uint16_t)rnd();
        CHECK(AIDashboardKey, k);
    }
    // the set-up: float work, main thread (either precision)
    for (int pc = 0; pc < 2; pc++) {
        g_pc = pc ? _PC_53 : _PC_24;
        rand_floats(g_arena + OFF_SETUP, 0x100, -3, 3);
        rand_floats(g_arena + OFF_CARDATA, 0x200, 0.1f, 5);
        *(int32_t*)(g_arena + OFF_CARDATA + 0x194) = ri(0, 15);
        g_arena[OFF_WORLDOBJ + 0xcd1] = (uint8_t)ri(0, 1);
        strcpy((char*)g_arena + OFF_WORLDOBJ + 8, g_track_names[rnd() % 8]);
        CHECK(AICarSetup, g_arena + OFF_SETUP, (const char*)str(ri(0, 2)), ri(0, 3), (const uint8_t*)g_arena + OFF_WORLDOBJ);
        CHECK(setup_gear_ratios, g_arena + OFF_SETUP, (const char*)str(ri(0, 2)), sp(rf(0.2f, 2.0f)));
        CHECK(AITweakCarData, g_arena + OFF_CARDATA);
    }
    g_pc = _PC_24;
    g_main_thread = false;
}

static void test_begin_end() {
    g_phase = "begin/end";
    g_main_thread = true;
    g_alloc_may_fail = true;
    *(int32_t*)(g_arena + OFF_CARLIST + 0xc80) = ri(0, 16);
    if (chance(50)) CHECK0(AIEnd);
    CHECK(AIBegin, (const uint8_t*)g_arena + OFF_CARLIST, (const char*)0);
    CHECK0(Proxer_End);
    CHECK(Proxer_Begin, ri(0, 16));
    Proxer* obj = (Proxer*)(g_arena + OFF_PROXOBJ);
    CHECK(Proxer_ctor, obj, 0, ri(-1, 16));
    if (chance(50)) CHECK(Proxer_dtor, obj, 0);
    CHECK0(AIEnd);
    g_alloc_may_fail = false;
    g_main_thread = false;
}

static void test_prox() {
    g_phase = "prox";
    g_main_thread = false;
    Proxer* p = proxer();
    if (!p) return;
    int n = p->n;
    for (int r = 0; r < 2; r++) {
        int i = ri(0, n - 1);
        Point2D a = {sp(rf(-500, 500)), sp(rf(-500, 500))}, b = {sp(rf(-80, 80)), sp(rf(-80, 80))};
        Point2D* pa = (Point2D*)(g_arena + OFF_BUF);
        pa[0] = a; pa[1] = b;
        CHECK(Proxer_update_object, p, 0, i, (const Point2D*)&pa[0], (const Point2D*)&pa[1]);
    }
    CHECK(Proxer_update, p, 0);
    if (chance(20)) CHECK(Proxer_init_tabs, p, 0);
    // the comparators, on the Proxer's own deltas and loose ones (some null, some near, some closing)
    for (int r = 0; r < 16; r++) {
        p->focus = ri(0, n - 1);
        int m = n * (n - 1) / 2;
        const ProxerDelta* a = chance(5) ? 0 : chance(50) && m ? &p->deltas[ri(0, m - 1)] : delta(ri(0, NDELTAS - 1));
        const ProxerDelta* b = chance(5) ? 0 : chance(50) && m ? &p->deltas[ri(0, m - 1)] : delta(ri(0, NDELTAS - 1));
        CHECK(Proxer_compare_edges_by_interest, p, 0, a, b);
        CHECK(Proxer_compare_edges_by_distance, p, 0, a, b);
    }
    // static_sort_f and sort_edges on a list of pointers
    ProxerDelta** list = (ProxerDelta**)(g_arena + OFF_LIST);
    int m = n * (n - 1) / 2;
    for (int k = 0; k < 32; k++) list[k] = chance(5) ? 0 : (m && chance(50) ? &p->deltas[ri(0, m - 1)] : delta(ri(0, NDELTAS - 1)));
    *(uint32_t*)0x004ec488 = chance(50) ? 0x004230b0 : 0x00423220;
    p->focus = ri(0, n - 1);
    CHECK(Proxer_static_sort_f, (const void*)&list[ri(0, 31)], (const void*)&list[ri(0, 31)]);
    *(uint32_t*)0x004ec488 = 0;
    CHECK(Proxer_sort_edges, p, 0, list, chance(50) ? 0x004230b0u : 0x00423220u, ri(0, n - 1));
    for (int r = 0; r < 8; r++) {
        ProxerDelta* d = chance(50) && m ? &p->deltas[ri(0, m - 1)] : delta(ri(0, NDELTAS - 1));
        float* side = chance(20) ? 0 : (float*)(g_arena + OFF_BUF + 0x10);
        CHECK(ProxerDelta_naive_contact_time, (const ProxerDelta*)d, 0, sp(rf(0, 20)), side);
        Point2D* out = (Point2D*)(g_arena + OFF_BUF + 0x20);
        CHECK(ProxerDelta_GetMyDL, (const ProxerDelta*)d, 0, chance(50) ? (int)d->a : ri(-1, 16), out);
        CHECK(ProxerDelta_GetMyDV, (const ProxerDelta*)d, 0, chance(50) ? (int)d->a : ri(-1, 16), out);
    }
    CHECK(ProxerDelta_ctor, delta(ri(0, NDELTAS - 1)), 0);
    g_cov[0]++;
}

static void test_notes() {
    g_phase = "notes";
    g_main_thread = false;
    CHECK(aicar_record_notes, (void*)(g_arena + OFF_BUF), ri(0, 100));
    uint8_t* b = g_arena + OFF_BUF;
    for (int k = 0; k < 64; k++) b[0x100 + k] = (uint8_t)(chance(50) ? rnd() : (chance(50) ? 0 : '1'));
    CHECK(convert_to_string, (char*)b, (const uint8_t*)b + 0x100, ri(-1, 40));
    CHECK(convert_to_bool, b, (const char*)b + 0x100, ri(-1, 40));
    for (int r = 0; r < 8; r++) {
        CHECK(hex2int, (int*)(b + 0x200), (char)rnd());
        char* s = (char*)b + 0x220;
        static const char hx[] = "0123456789abcdefABCDEFgG/:@`";
        for (int k = 0; k < 8; k++) s[k] = chance(85) ? hx[rnd() % 28] : (char)rnd();
        CHECK(InverseDSTC, (int*)(b + 0x200), (int*)(b + 0x204), (int*)(b + 0x208), (char*)b + 0x210, (const char*)s);
    }
    for (int r = 0; r < 3; r++) {
        CHECK(GetUniqueDSTCMunge, (char*)b + 0x300, ri(0, 20), ri(0, 20), ri(0, 20), (const char*)str(ri(0, 2)));
        CHECK(GetUniqueDSTCFname, (char*)b + 0x300, (const char*)0x004ec610, (const char*)0x004ec608, ri(0, 20), ri(0, 20),
              ri(0, 20), (const char*)str(ri(0, 2)));
        CHECK(get_notes_fname, (char*)b + 0x300, ri(0, 4), ri(0, 4), ri(0, 15), (const char*)str(ri(0, 2)));
        CHECK(get_notes_resname, ri(-1, 2), (char*)b + 0x300, ri(0, 4), ri(0, 4), ri(0, 15), (const char*)str(ri(0, 2)));
        CHECK(calc_driver_crc, ri(0, 3), ri(0, 3));
    }
    g_ret_mask = 0x0000ffffffffffffull;                             // ili_info's top half word is garbage
    for (int r = 0; r < 3; r++) CHECK(get_ili_info, ri(0, 15), ri(0, 3), ri(0, 3), (const char*)str(ri(0, 2)));
    g_ret_mask = ~0ull;
    for (int r = 0; r < 6; r++) {
        // mostly the blobs' own keys, so the checks get past the header
        const NotesHeader* h = (const NotesHeader*)blob(ri(0, 3));
        int d = chance(70) ? h->driver : ri(0, 3), s = chance(70) ? h->strength : ri(0, 3), t = chance(70) ? h->track : ri(0, 3);
        CHECK(GetDriverNotes, d, s, t, (const char*)str(chance(80) ? 0 : ri(0, 2)), (uint8_t)ri(0, 1));
    }
    for (int r = 0; r < 2; r++) CHECK(GetDriverData, ri(0, 15));
    CHECK(ReleaseDriverData, (const void*)(g_arena + OFF_BUF + 0x40));
    g_main_thread = true;
    for (int r = 0; r < 2; r++) {
        *(void**)0x00520b10 = 0;
        CHECK(generate_notes_for, ri(0, 3), ri(0, 3), ri(-3, 15), (const char*)str(ri(0, 2)));
    }
    // the learning run: a few tracks and strengths, the lounge's few drivers
    uint8_t* tracks = b + 0x400, *strengths = b + 0x410;
    for (int k = 0; k < 16; k++) tracks[k] = (uint8_t)chance(15);
    for (int k = 0; k < 8; k++) strengths[k] = (uint8_t)chance(30);
    g_dorace_aborts = chance(30);
    CHECK(check_learning, (const uint8_t*)tracks, (const uint8_t*)strengths);
    g_dorace_aborts = false;
    g_main_thread = false;
}

static void test_gcv() {
    g_phase = "gcviewer";
    g_main_thread = true;
    GhostCarViewer* v = gcv();
    int32_t* gi = gints();
    for (int r = 0; r < 3; r++) {
        if (chance(50)) { gi[0] = ri(0, 15); gi[1] = ri(0, 7); gi[2] = ri(0, 15); }
        switch (rnd() % 16) {
        case 0: CHECK(GCV_Create, v, 0); break;
        case 1: CHECK(GCV_Destroy, v, 0); break;
        case 2: CHECK(GCV_next_submit, v, 0); break;
        case 3: CHECK(GCV_prev_submit, v, 0); break;
        case 4: CHECK(GCV_Added, v, 0); break;
        case 5: CHECK(GCV_set_driver_ghost, v, 0); break;
        case 6: CHECK(GCV_update_driver, v, 0); break;
        case 7: CHECK(GCV_update_text, v, 0, (const GhostInfo*)ghost(ri(0, NGHOSTS - 1)), (UIData*)(g_arena + OFF_BUF)); break;
        case 8: CHECK(GCV_update_submit, v, 0); break;
        case 9: CHECK(GCV_update_widgets, v, 0); break;
        case 10: CHECK(GCV_Callback, v, 0, ri(0, 3), (const void*)0); break;
        case 11: CHECK(GCV_add_driver_ghost, v, 0, (const GhostInfo*)ghost(ri(0, NGHOSTS - 1)), ri(0, 15), ri(0, 7), ri(0, 15)); break;
        case 12: CHECK(GCV_delete_ghost, v, 0); break;
        case 13: CHECK(GCV_add_submitted_ghost, v, 0, (const uint8_t*)gfile(ri(0, 3)), (const char*)str(ri(0, 2))); break;
        case 14: if (chance(30)) v->loaded = 0; CHECK(GCV_load, v, 0); break;
        default:
            switch (rnd() % 5) {
            case 0: CHECK(GCV_SetDriverGhost, ri(0, 3)); break;
            case 1: CHECK(GCV_NextSubmit, ri(0, 3)); break;
            case 2: CHECK(GCV_PrevSubmit, ri(0, 3)); break;
            case 3: CHECK(GCV_LoadSet, ri(0, 3)); break;
            default: CHECK(GCV_Delete, ri(0, 3)); break;
            }
        }
    }
    if (chance(10)) {
        CHECK(GCV_dtor, v, 0);
        g_alloc_may_fail = true;
        CHECK(CreateIGhostCarViewer, &gi[0], &gi[1], &gi[2]);
        g_alloc_may_fail = false;
        CHECK(GCV_ctor, v, 0, &gi[0], &gi[1], &gi[2]);
    }
    g_main_thread = false;
}

#ifdef FIX_TESTS
// ==== the fix test: a mod car's four-character code ================================================================
static int g_fix_fail;
#define FIXCHECK(cond, ...)                                                                                     \
    do {                                                                                                        \
        if (!(cond)) {                                                                                          \
            if (g_fix_fail++ < 30) { printf("FIX FAIL: "); printf(__VA_ARGS__); printf("\n"); }                 \
        }                                                                                                       \
    } while (0)
static uint8_t g_munge_code[5];                        // munge_carname_to_4char's first bytes (the stub's jump replaces them)
static __declspec(noinline) void junk_stack() {
    volatile char junk[1024];
    for (int i = 0; i < 1023; i++) junk[i] = 'Z';
    junk[1023] = 0;
}
typedef void(__cdecl* Munge5_t)(char*, int, int, int, const char*);
static __declspec(noinline) void munge_on_junk(Munge5_t f, char* out, int d, int s, int t, const char* car) {
    junk_stack();
    f(out, d, s, t, car);
}
static void run_fix_tests() {
    memcpy((void*)0x0040dc70, g_munge_code, 5);          // the game's munge_carname_to_4char
    static char a[4096], b[4096];
    static const char* const cars[] = {"jeep", "jeepster", "willys", "indyjeep", "Burninator", "vw", "abc", "viper", "vipergt", "VIPER"};
    long garbage = 0, runs = 0;
    for (int it = 0; it < 2000; it++) {
        const char* car = cars[it % 10];
        const int d = ri(0, 15), st = ri(0, 7), tr = ri(0, 15);
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        munge_on_junk((Munge5_t)0x00424b30, a, d, st, tr, car);
        munge_on_junk(&GetUniqueDSTCMunge, b, d, st, tr, car);
        char want[64];
        char code[5] = {0};
        if (!_stricmp(car, "viper")) strcpy(code, "vipr");
        else if (!_stricmp(car, "vipergt")) strcpy(code, "vgt");
        else strncpy(code, car, 4);
        sprintf(want, "%x%x%x%s", d, st, tr, code);
        runs++;
        FIXCHECK(!strcmp(b, want), "GetUniqueDSTCMunge(%s): \"%.40s\", not \"%s\"", car, b, want);
        if (strlen(car) < 4 || !_stricmp(car, "viper") || !_stricmp(car, "vipergt"))
            FIXCHECK(!strcmp(a, b), "GetUniqueDSTCMunge(%s): the original's \"%.40s\", the fixed \"%s\"", car, a, b);
        else if (strcmp(a, b)) garbage++;
    }
    printf("GetUniqueDSTCMunge: %ld names; the original ran a mod car's code on into the stack %ld times, the fixed one "
           "never (stock and short names as the original's)\n", runs, garbage);
    FIXCHECK(garbage > 500, "the original never read the junk");
    // GetUniqueDSTCFname through the fixed munge (hooked, as in the game): dir + code + ext
    patch_jmp(0x00424b30, (void*)&GetUniqueDSTCMunge);
    for (int it = 0; it < 500; it++) {
        const char* car = cars[it % 5];
        const int d = ri(0, 15), st = ri(0, 7), tr = ri(0, 15);
        junk_stack();
        GetUniqueDSTCFname(a, "notes\\", ".dnt", d, st, tr, car);
        char want[128];
        char code[5] = {0};
        strncpy(code, car, 4);
        sprintf(want, "notes\\%x%x%x%s.dnt", d, st, tr, code);
        FIXCHECK(!strcmp(a, want), "GetUniqueDSTCFname(%s): \"%.60s\", not \"%s\"", car, a, want);
    }
    // generate_notes_for with over-long driver and car names: each cut to its field in the World (driver 13 bytes at
    // +0x2c, car 35 at +0x39) and the notes header (car 32 at +0x14); nothing else in the World changed by them
    static char dname[512], cname[512];
    long cut = 0;
    g_ilinetry_always = true;
    for (int it = 0; it < 3000; it++) {
        build_world();
        const int dl = it % 3 ? ri(13, 500) : ri(0, 12), cl = it % 4 ? ri(32, 500) : ri(1, 31);
        for (int i = 0; i < dl; i++) dname[i] = (char)('A' + rnd() % 26);
        dname[dl] = 0;
        for (int i = 0; i < cl; i++) cname[i] = (char)('a' + rnd() % 26);
        cname[cl] = 0;
        for (int i = 0; i < 16; i++) *(char**)(driver_rec(i) + 0x210) = dname;
        for (int i = 0; i < 64; i++) g_script[i] = rnd();
        g_si = 0;
        g_pass = 0;
        g_nlog[0] = 0;
        g_cap_n = 0;
        memset(g_cap_world, 0, sizeof g_cap_world);
        unsigned cw;
        _controlfp_s(&cw, _PC_53, _MCW_PC);
        bool fault = false;
        __try { generate_notes_for(ri(0, 3), ri(0, 3), ri(0, 15), cname); } __except (EXCEPTION_EXECUTE_HANDLER) { fault = true; }
        FIXCHECK(!fault, "generate_notes_for faulted (driver %d, car %d characters)", dl, cl);
        const int dw = dl < 12 ? dl : 12, cw2 = cl < 34 ? cl : 34;
        FIXCHECK(!memcmp(g_cap_world + 0x2c, dname, dw) && g_cap_world[0x2c + dw] == 0, "the World's driver name (%d characters)", dl);
        FIXCHECK(!memcmp(g_cap_world + 0x39, cname, cw2) && g_cap_world[0x39 + cw2] == 0, "the World's car name (%d characters)", cl);
        // the rest of the World as the constructor made it, but for the fields generate_notes_for sets
        uint8_t want[0xcd4];
        fill_script(want, 0xcd4, g_cap_world_seed);
        FIXCHECK(!memcmp(g_cap_world + 0x5c, want + 0x5c, 0xca8 - 0x5c), "the car's setup data in the World changed (car %d characters)", cl);
        if (g_cap_n) {
            const int hw = cl < 31 ? cl : 31;
            FIXCHECK(!memcmp(g_cap_hdr + 0x14, cname, hw) && g_cap_hdr[0x14 + hw] == 0, "the header's car name (%d characters)", cl);
        }
        if (dl >= 13 || cl >= 32) cut++;
    }
    printf("generate_notes_for: %ld runs with a driver's name of 13+ or a car's of 32+ characters, cut to their fields, none "
           "faulted or wrote past them\n", cut);
    g_ilinetry_always = false;
    // GetUniqueDSTCFname through the fixed munge (hooked, as in the game): dir + code + ext
}
#endif

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 2000;
    if (argc > 2) g_state = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_ai.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
#ifdef FIX_TESTS
    memcpy(g_munge_code, (void*)0x0040dc70, 5);
#endif
    install_stubs();
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!g_arena) return 2;
    uint32_t gb = 0;
    for (const GRange& g : g_globals) gb += g.n;
    if (gb != GLOBALS_BYTES) { printf("GLOBALS_BYTES is %u, should be %u\n", (unsigned)GLOBALS_BYTES, gb); return 2; }
    for (g_world = 0; g_world < worlds; g_world++) {
        g_pass = 0;
        build_world();
        g_special_pct = g_world % 3 == 0 ? 10 : 2;
        test_ai();
        test_prox();
        test_notes();
        test_gcv();
        test_begin_end();
    }
    int failed = 0;
    long calls = 0;
    printf("%-40s %8s %8s %8s %8s %8s %8s %6s\n", "function", "calls", "changed", "replay", "faults", "differ", "fp-miss", "first");
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("%-40s %8ld %8ld %8ld %8ld %8ld %8ld %6ld\n", st.name, st.calls, st.changed, st.replay_only, st.faults, st.fails,
               st.fp_fails, st.first_fail);
        if (strstr(st.name, "compare") || strstr(st.name, "static_sort") || !strcmp(st.name, "AIDashboardKey") ||
            !strcmp(st.name, "IterateILIname") || !strcmp(st.name, "InverseDSTC") || !strcmp(st.name, "hex2int") ||
            !strcmp(st.name, "GetDriverNotes") || !strcmp(st.name, "get_notes_resname"))
            printf("    returned -1: %ld, 0: %ld, 1: %ld, other: %ld\n", st.ret[0], st.ret[1], st.ret[2], st.ret[3]);
        if (st.fails || st.fp_fails) failed++;
        calls += st.calls;
    }
    printf("%d worlds, %ld calls; %d of %d functions differ or escape their footprint; %d unknown footprint classes\n",
           worlds, calls, failed, g_nstats, g_fp_unknown);
#ifdef FIX_TESTS
    run_fix_tests();
    printf("fix tests: %d failures\n", g_fix_fail);
    if (g_fix_fail) failed++;
#endif
    return failed ? 1 : 0;
}
