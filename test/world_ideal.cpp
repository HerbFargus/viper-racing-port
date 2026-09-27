// world_ideal.cpp -- the racing-line rewrites (hook/phys_ideal.cpp: ai:ideal.obj, idealres.obj, track.obj)
// against the originals, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_ideal.cpp
//        /Fo%TEMP%\wi\ /Fe%TEMP%\wi\world_ideal.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_ideal.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. Two kinds of world:
//
//  * a track: a synthetic 'NILI' resource (a noisy closed loop of 1..64 nodes: Catmull-Rom tangents, lengths,
//    distances, checkpoints, speeds, widths; sometimes a bad header or version) is served by a scripted
//    ResourceTryDiscardable, and loaded through the ORIGINAL loaders -- CenterLine::CenterLine (1-4 of them,
//    reversed or not, track.ild or the dlatlong.ili fallback), ConstIdealLine::load, MutableIdealLine::load or
//    a MutableIdealLine built node by node. Cars drive round the loop (with lateral wander, reversing,
//    teleports near and far -- the teleport that leaves the bead NULL and the stock AI crash that follows are
//    reproduced, both passes faulting), and each tick runs CenterLine::update per car, update_car_info and
//    get_rabbit_position on the AI's line, and at random every other entry point: the conversions, the
//    queries, the line editor's calls, Draw, the destructors.
//  * maths: a ring of 8 nodes filled with random values (NaNs, infinities, denormals, zeros mixed in) for the
//    pure and near-pure functions -- QuickEval/QuickTan/__Curvature/__Curv, calculate_lat, the cookie and
//    metre conversions, fixup_res on random resources, the bead functions on random rings.
//
// Everything outside ideal.obj is a stub that logs its call and answers from a per-call script (MemAlloc from
// a bump heap in the world, the resource, TerrainGetHeight, mrProjectPoint, gxCircle/gxRect, the profiler
// hooks, the file calls; LogPanic raises, as the game's never returns); PhysicsGetTime, the Pool, memmove and
// ASSERT_MSG run as the originals. For each call the original runs, the world (one arena of every object
// plus the globals) and the log are kept, the world restored, the rewrite runs, and the world, the return
// value, the log and any fault are compared; every byte the original changed must lie inside the rewrite's
// footprint unless it's replay_only. The world continues from the original's result.
//
// The fixes (port.h: VP_FIX). The plain build defines VP_FAITHFUL: the rewrites as the original, checked bit for
// bit as above. Built with /DFIX_TESTS it compiles the fixed rewrites (as the game runs them) and checks them:
//  * ordinary worlds (the track worlds without NaNs, infinities or lines under 3 nodes): every call must still
//    equal the original, except where the original crashed (the fixed one mustn't) or left a line's bead NULL
//    (the fixed one must have put it at the brute-force nearest point of the line, for reset_bead_position and
//    CenterLine::reset);
//  * the lost bead, against the originals: reset_bead_position on points near, far and on the nodes (where the
//    original finds a bead the fixed one finds the same; where it leaves it NULL the fixed one is at the nearest
//    point), NaN positions (the bead stays where it was, or goes to the head), and every reader of a NULL bead
//    (the original faults, the fixed one doesn't);
//  * two-node lines (QuickTan: the original NaN, the fixed one the chord), and then -- with every rewrite hooked
//    in the image, as the game runs them -- cars driven round two-node lines, lines with a zero-length node,
//    the editor's zero-length lines and NaN positions: nothing faults, hangs (the watchdog) or goes NaN, and the
//    bead gets round the loop.
#ifndef FIX_TESTS
#define VP_FAITHFUL                                 // the original's behaviour, bit for bit
#endif
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tuple>
#include <type_traits>
#include "../hook/port.h"

// the rewrites, compiled in: each PORT_FN just names the original's address and the footprint function (and
// registers the pair, for the fix tests that hook every rewrite in the image as the game does)
struct PortReg { uint32_t at; void* fn; };
static PortReg g_regs[128];
static int g_nregs;
struct PortRegAdd { PortRegAdd(uint32_t a, void* f) { g_regs[g_nregs++] = {a, f}; } };
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                        \
    static const char* const VP_CAT(name_, NEW) = NAME;                                                   \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;                                                         \
    static PortRegAdd VP_CAT(reg_, NEW)(V10, (void*)&NEW);

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
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 4, what); }

#include "../hook/phys_ideal.cpp"

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
static float special_s(uint32_t s) {
    switch ((s >> 24) % 9) {
    case 0: return fbits(0x7fc00000u | (s & 0x3fffff));                    // quiet NaN
    case 1: return fbits(0xffc00000u | (s & 0x3fffff));                    // negative quiet NaN
    case 2: return fbits(0x7f800001u + (s & 0x3ffffe));                    // signalling NaN
    case 3: return fbits(0x7f800000u | (s & 0x80000000u));                 // +-inf
    case 4: return fbits(s & 0x807fffffu);                                 // denormal
    case 5: return (s & 1) ? 0.0f : -0.0f;
    case 6: return fbits((s & 0x80000000u) | 0x7f7fffffu);                  // +-FLT_MAX
    case 7: return fbits((s & 0x80000000u) | 0x00800000u | (s & 0x7fff)); // tiny normal
    default: return fbits(s);                                              // any bits
    }
}
static float special() { return special_s(rnd()); }
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

// ---- the world: one tracked arena holding every object ------------------------------------------------------
enum {
    OFF_SRC = 0x0000,                                              // the served resource (12 + 64 x 0x44)
    OFF_CL = 0x1200, CL_STRIDE = 0x80, NCL = 4,                    // CenterLines
    OFF_CIL = 0x1400,                                              // the AI's ConstIdealLine
    OFF_MIL = 0x1440,                                              // a MutableIdealLine (0x58)
    OFF_RLINE = 0x14a0,                                            // an IdealLine over the ring (0x38)
    OFF_OUT = 0x1500, NOUT = 32,                                   // 8-byte slots
    OFF_OPTS = 0x1600,                                             // GameOptions (+0x25 reverse), +0x30 a flag byte
    OFF_RING = 0x1700, NRING = 8,                                  // a res header + 8 nodes (0x22c)
    OFF_FCL = 0x1940,                                              // two fake CenterLines (maths), 0x80 apart
    OFF_FTIMES = 0x1a40,                                           // their times (2 x 8 floats)
    OFF_SCRATCH = 0x1a80,                                          // fixup_res's resources (12 + 16 x 0x44)
    OFF_HEAP = 0x2000, HEAP_SIZE = 0x22000,
    ARENA_SIZE = OFF_HEAP + HEAP_SIZE,
    MAXSEG = 64,
};
static uint8_t* g_arena;
static uint8_t* g_heap_top;

struct GRange { uint32_t at, n; const char* what; };
static const GRange g_globals[] = {
    {0x004ec2a0, 4, "Draw's head flag"},
    {0x0052161c, 4, "physics_tick"},
};
enum { GLOBALS_BYTES = 8 };
static const GRange g_auto_saved[] = {{0x004ec2a0, 480, "fruit (ideal.obj statics)"}, {0x0052161c, 4, "physics_tick"}};
static int32_t* const g_tick = (int32_t*)0x0052161c;

static CenterLine* cl(int i) { return (CenterLine*)(g_arena + OFF_CL + i * CL_STRIDE); }
static ConstIdealLine* cil() { return (ConstIdealLine*)(g_arena + OFF_CIL); }
static MutableIdealLine* mil() { return (MutableIdealLine*)(g_arena + OFF_MIL); }
static IdealLine* rline() { return (IdealLine*)(g_arena + OFF_RLINE); }
static Point2D* slot(int i) { return (Point2D*)(g_arena + OFF_OUT + (i & (NOUT - 1)) * 8); }
static ILinePos* pslot(int i) { return (ILinePos*)slot(i); }
static IdealLineRes* ringres() { return (IdealLineRes*)(g_arena + OFF_RING); }
static ILSeg* ring(int i) { return res_seg(ringres(), i & (NRING - 1)); }
static CenterLine* fcl(int i) { return (CenterLine*)(g_arena + OFF_FCL + i * 0x80); }

// ---- the stubs' script and log -------------------------------------------------------------------------------
struct LogEntry { uint32_t kind, a[5]; };
enum { LOGN = 8192 };
static LogEntry g_log[2][LOGN];
static int g_nlog[2], g_pass;
static uint32_t g_script[64];
static int g_si;
static uint32_t script() { return g_script[g_si++ & 63]; }
static void logn(uint32_t kind, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0) {
    if (g_nlog[g_pass] < LOGN) g_log[g_pass][g_nlog[g_pass]] = {kind, {a0, a1, a2, a3, a4}};
    g_nlog[g_pass]++;
}
enum LogKind {
    L_MEMALLOC = 1, L_MEMFREE, L_DELETE, L_RESTRY, L_RESFORGET, L_REPORT, L_PANIC, L_TERRAIN, L_PROJECT,
    L_CIRCLE, L_RECT, L_WOPTS, L_FCREATE, L_RWRITE, L_FWRITE, L_FCLOSE, L_PR_BEGIN, L_PR_END, L_PROF_START,
    L_PROF_STOP,
};
static const char* const g_log_names[] = {
    "?", "MemAlloc", "MemFree", "operator delete", "ResourceTryDiscardable", "ResourceForget", "LogReport",
    "LogPanic", "TerrainGetHeight", "mrProjectPoint", "gxCircle", "gxRect", "WorldGameOptions", "FileCreate",
    "ResourceWrite", "FileWrite", "FileClose", "pr_overhead_begin", "pr_overhead_end", "prof_start", "prof_stop",
};

// the resource the stub serves, per world
static uint32_t g_res_version = 3;
static int32_t g_res_size;
static int g_ild_missing_pct, g_ili_missing_pct;

static void* __cdecl stub_MemAlloc(int n) {
    uint8_t* p = g_heap_top;
    g_heap_top += ((uint32_t)n + 15) & ~15u;
    if (g_heap_top > g_arena + OFF_HEAP + HEAP_SIZE) { printf("the fake heap is full\n"); exit(3); }
    logn(L_MEMALLOC, (uint32_t)n, (uint32_t)(uintptr_t)p);
    return p;
}
static void __cdecl stub_MemFree(void* p) { logn(L_MEMFREE, (uint32_t)(uintptr_t)p); }
static void __cdecl stub_delete(void* p) { logn(L_DELETE, (uint32_t)(uintptr_t)p); }
static const void* __cdecl stub_ResTry(const char* name, uint32_t tag, uint32_t* ver, int32_t* size) {
    logn(L_RESTRY, (uint32_t)(uintptr_t)name, tag);
    uint32_t s = script();
    int miss = (uintptr_t)name == 0x004ec41c ? g_ild_missing_pct : g_ili_missing_pct;
    if ((int)(s % 100) < miss) return 0;
    *ver = g_res_version;
    *size = g_res_size;
    return g_arena + OFF_SRC;
}
static uint8_t __cdecl stub_ResForget(void* p) { logn(L_RESFORGET, (uint32_t)(uintptr_t)p); return 1; }
static void __cdecl stub_report(const char* fmt, ...) {
    uint32_t a[3] = {0, 0, 0};
    int n = 0;
    for (const char* f = fmt; *f && n < 3; f++)             // only the arguments the format has
        if (*f == '%') a[n++] = 1;
    va_list ap;
    va_start(ap, fmt);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    logn(L_REPORT, (uint32_t)(uintptr_t)fmt, a[0], a[1], a[2]);
}
static void __cdecl stub_panic(const char* fmt, ...) {
    logn(L_PANIC, (uint32_t)(uintptr_t)fmt);
    RaiseException(0xe0000bad, 0, 0, 0);                   // the game's LogPanic doesn't return
}
static double __cdecl stub_terrain(uint32_t x, uint32_t z) {
    logn(L_TERRAIN, x, z);
    uint32_t s = script();
    float h = (s & 31) == 0 ? special_s(s) : fbits(x) * 0.01f + fbits(z) * 0.02f;
    return h;
}
static uint8_t __cdecl stub_project(const P3* in, P3* out) {
    logn(L_PROJECT, ubits(in->x), ubits(in->y), ubits(in->z));
    uint32_t s = script();
    if ((s & 7) == 0) return 0;
    out->x = in->x * 0.37f + 320.0f;
    out->y = in->z * -0.29f + 240.0f + in->y;
    out->z = 1.0f;
    if ((s & 0x3f00) == 0) out->x = special_s(s);
    return 1;
}
static void __cdecl stub_circle(int x, int y, int r, uint32_t c) { logn(L_CIRCLE, x, y, r, c); }
static void __cdecl stub_rect(int x, int y, int x2, int y2, uint32_t c) { logn(L_RECT, x, y, x2, y2, c); }
static const uint8_t* __cdecl stub_wopts() { logn(L_WOPTS); return g_arena + OFF_OPTS; }
static int __cdecl stub_fcreate(const char* n) { logn(L_FCREATE, (uint32_t)(uintptr_t)n); return (script() & 3) ? 0x1234 : 0; }
static void __cdecl stub_rwrite(int h, uint32_t tag, uint32_t v) { logn(L_RWRITE, h, tag, v); }
static uint8_t __cdecl stub_fwrite(int h, const void* d, int n) {
    // the header's first 8 bytes are the original's uninitialised stack: only its count is compared
    uint32_t hash = 0x811c9dc5u;
    if (n == 12) hash = ((const uint32_t*)d)[2];
    else for (int i = 0; i < n; i++) hash = (hash ^ ((const uint8_t*)d)[i]) * 0x01000193u;
    logn(L_FWRITE, h, n, hash);
    return 1;
}
static void __cdecl stub_fclose(int* h) { logn(L_FCLOSE, *h); }
static void __cdecl stub_pr_begin() { logn(L_PR_BEGIN); }
static void __cdecl stub_pr_end() { logn(L_PR_END); }
static int __cdecl stub_prof_start(const char* n) { logn(L_PROF_START, (uint32_t)(uintptr_t)n); return 0x55; }
static void __cdecl stub_prof_stop(int h) { logn(L_PROF_STOP, h); }

static void install_stubs() {
    patch_jmp(0x004140e0, (void*)&stub_MemAlloc);
    patch_jmp(0x00414300, (void*)&stub_MemFree);
    patch_jmp(0x00414390, (void*)&stub_delete);
    patch_jmp(0x00419f40, (void*)&stub_ResTry);
    patch_jmp(0x0041a450, (void*)&stub_ResForget);
    patch_jmp(0x00411150, (void*)&stub_report);
    patch_jmp(0x004112b0, (void*)&stub_panic);
    patch_jmp(0x00465ae0, (void*)&stub_terrain);
    patch_jmp(0x0044f200, (void*)&stub_project);
    patch_jmp(0x00452e80, (void*)&stub_circle);
    patch_jmp(0x004506e0, (void*)&stub_rect);
    patch_jmp(0x004627a0, (void*)&stub_wopts);
    patch_jmp(0x004115f0, (void*)&stub_fcreate);
    patch_jmp(0x0041a5c0, (void*)&stub_rwrite);
    patch_jmp(0x00411a30, (void*)&stub_fwrite);
    patch_jmp(0x00411850, (void*)&stub_fclose);
    *(void**)0x004e642c = (void*)&stub_pr_begin;
    *(void**)0x004e6430 = (void*)&stub_pr_end;
    *(void**)0x004e6434 = (void*)&stub_prof_start;
    *(void**)0x004e6438 = (void*)&stub_prof_stop;
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

struct Stat { const char* name; long calls, changed, fails, fp_fails, faults, replay_only, fixed; };
static Stat g_stats[96];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    g_stats[g_nstats] = {name, 0, 0, 0, 0, 0, 0, 0};
    return g_stats[g_nstats++];
}

struct Snapshot { uint8_t arena[ARENA_SIZE]; uint8_t globals[GLOBALS_BYTES]; uint8_t* heap_top; };
static Snapshot g_start, g_after;
static Footprint g_fp;
static int g_world;
static const char* g_phase = "";
static bool g_pc53;                                 // this call at the main thread's 53-bit precision
static bool g_last_fault;
static uint64_t g_last_ret;                         // the original's return value
static volatile LONG g_busy;                        // the watchdog: the current check's number
static const char* volatile g_busy_name;

static void snap_globals(uint8_t* p) {
    for (const GRange& g : g_globals) { memcpy(p, (void*)(uintptr_t)g.at, g.n); p += g.n; }
}
static void save(Snapshot& s) {
    memcpy(s.arena, g_arena, ARENA_SIZE);
    snap_globals(s.globals);
    s.heap_top = g_heap_top;
}
static void load(const Snapshot& s) {
    memcpy(g_arena, s.arena, ARENA_SIZE);
    const uint8_t* p = s.globals;
    for (const GRange& g : g_globals) { memcpy((void*)(uintptr_t)g.at, p, g.n); p += g.n; }
    g_heap_top = s.heap_top;
}
static bool in_ranges(uint32_t addr, const GRange* r, int n) {
    for (int i = 0; i < n; i++)
        if (addr >= r[i].at && addr < r[i].at + r[i].n) return true;
    return false;
}
static bool in_footprint(const void* p) {
    for (int k = 0; k < g_fp.n; k++)
        if ((const uint8_t*)p >= (const uint8_t*)g_fp.r[k].p && (const uint8_t*)p < (const uint8_t*)g_fp.r[k].p + g_fp.r[k].n) return true;
    return false;
}
static const char* region_of(uint32_t off, uint32_t* rel) {
    struct R { uint32_t at; const char* name; } static const rs[] = {
        {OFF_SRC, "resource"}, {OFF_CL, "CenterLines"}, {OFF_CIL, "ConstIdealLine"}, {OFF_MIL, "MutableIdealLine"},
        {OFF_RLINE, "ring IdealLine"}, {OFF_OUT, "out slots"}, {OFF_OPTS, "options"}, {OFF_RING, "ring"},
        {OFF_FCL, "fake CenterLines"}, {OFF_FTIMES, "fake times"}, {OFF_SCRATCH, "scratch res"}, {OFF_HEAP, "heap"},
    };
    int k = 0;
    for (int i = 0; i < (int)(sizeof rs / sizeof *rs); i++)
        if (off >= rs[i].at) k = i;
    *rel = off - rs[k].at;
    return rs[k].name;
}
#ifdef FIX_TESTS
// ---- the fix checks' helpers -------------------------------------------------------------------------------------
static int g_fix_fail;
static long g_fix_nearest, g_fix_found;
#define FIXCHECK(cond, ...)                                                                             \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            if (g_fix_fail++ < 40) { printf("FIX FAIL (world %d): ", g_world); printf(__VA_ARGS__); printf("\n"); } \
        }                                                                                               \
    } while (0)
static bool ffin(float x) { return (ubits(x) & 0x7f800000u) != 0x7f800000u; }
// the world's lines, by offset in the arena
static const uint32_t g_line_offs[] = {OFF_CL, OFF_CL + CL_STRIDE, OFF_CL + 2 * CL_STRIDE, OFF_CL + 3 * CL_STRIDE, OFF_CIL,
                                       OFF_MIL, OFF_RLINE, OFF_FCL, OFF_FCL + 0x80};
static bool on_loop(const IdealLine* l, const ILSeg* s) {
    const ILSeg* h = l->head;
    const ILSeg* q = h;
    for (int n = 0; q && n < 10000; n++) {
        if (q == s) return true;
        q = q->next;
        if (q == h) break;
    }
    return false;
}
// brute force, at 53 bits: the squared distance from pt to the nearest of 4001 points along each chord of the line,
// and to the bead's point on its chord
static unsigned pc53_begin() { unsigned cw, x; _controlfp_s(&cw, 0, 0); _controlfp_s(&x, _PC_53, _MCW_PC); return cw; }
static void pc_end(unsigned cw) { unsigned x; _controlfp_s(&x, cw & _MCW_PC, _MCW_PC); }
static double brute_nearest(const IdealLine* l, const Point2D* pt) {
    unsigned cw = pc53_begin();
    const ILSeg* h = l->head;
    const ILSeg* s = h;
    double best = 1e300;
    for (int n = 0; s && n < 10000; n++) {
        const ILSeg* nx = s->next;
        for (int k = 0; k <= 4000; k++) {
            double t = k / 4000.0;
            double x = s->p.x + t * ((double)nx->p.x - s->p.x) - pt->x, z = s->p.z + t * ((double)nx->p.z - s->p.z) - pt->z;
            double d = x * x + z * z;
            if (d < best) best = d;
        }
        s = nx;
        if (s == h) break;
    }
    pc_end(cw);
    return sqrt(best);
}
static double bead_dist(const IdealLine* l, const Point2D* pt) {
    unsigned cw = pc53_begin();
    const ILSeg* s = l->bead_seg;
    const ILSeg* nx = s->next;
    double t = l->bead_t;
    double x = s->p.x + t * ((double)nx->p.x - s->p.x) - pt->x, z = s->p.z + t * ((double)nx->p.z - s->p.z) - pt->z;
    double d = sqrt(x * x + z * z);
    pc_end(cw);
    return d;
}
// the bead of a line the fixed code put back: on the line, t in 0..1, at the nearest point of the line to pt
static void check_found_bead(const IdealLine* l, const Point2D* pt, const char* what) {
    FIXCHECK(l->bead_seg && on_loop(l, l->bead_seg), "%s: the bead isn't on the line", what);
    if (!l->bead_seg || !on_loop(l, l->bead_seg)) return;
    FIXCHECK(l->bead_t >= 0.0f && l->bead_t <= 1.0f, "%s: bead t %g", what, l->bead_t);
    if (ffin(pt->x) && ffin(pt->z)) {
        double b = brute_nearest(l, pt), d = bead_dist(l, pt);
        FIXCHECK(d <= b + 1e-4 * (1.0 + b) + 1e-3, "%s: the bead is %.6f m from the car, the nearest point of the line %.6f m", what, d, b);
        g_fix_nearest++;
    }
}
// did the original leave some line's bead NULL (a line with a head) that the fixed one filled?
static bool fix_lost_bead_found() {
    bool found = false;
    for (uint32_t off : g_line_offs) {
        const IdealLine* o = (const IdealLine*)(g_after.arena + off);
        const IdealLine* n = (const IdealLine*)(g_arena + off);
        if (o->head && !o->bead_seg && n->bead_seg) found = true;
    }
    return found;
}
// after a call the original crashed on or left a bead NULL on: every line whose bead the fixed one put back is on
// the line (and, for the resets, at the point nearest the car)
static void fix_validate(const char* name, Stat&) {
    bool nearest = !strcmp(name, "IdealLine::reset_bead_position") || !strcmp(name, "CenterLine::reset");
    for (uint32_t off : g_line_offs) {
        const IdealLine* s = (const IdealLine*)(g_start.arena + off);
        const IdealLine* o = (const IdealLine*)(g_after.arena + off);
        const IdealLine* n = (const IdealLine*)(g_arena + off);
        if (!n->head || n->bead_seg == o->bead_seg) continue;
        if (o->bead_seg && s->bead_seg) continue;                 // (not a bead the original lost)
        g_fix_found++;
        char what[96];
        sprintf(what, "%s (line +0x%x)", name, off);
        if (nearest) check_found_bead(n, &n->car_pos, what);
        else FIXCHECK(n->bead_seg && on_loop(n, n->bead_seg) && ffin(n->bead_t), "%s: the bead isn't on the line", what);
    }
}
#endif
static void fpu_reset() {
    __asm fninit
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, g_pc53 ? _PC_53 : _PC_24, _MCW_PC);
}

template <typename Run, typename Fp> static void check(const char* name, Run run, Fp footprint) {
    Stat& st = stat(name);
    st.calls++;
    g_busy_name = name;
    InterlockedIncrement(&g_busy);
    for (int i = 0; i < 64; i++) g_script[i] = rnd();
    save(g_start);
    g_fp.n = 0; g_fp.replay_only = 0; g_fp.pure = false;
    int ff = 0;
    __try { footprint(g_fp); } __except (EXCEPTION_EXECUTE_HANDLER) { ff = 1; }
    if (ff) { g_fp.n = 0; g_fp.replay_only = "(the footprint faulted)"; }
    if (g_fp.replay_only) st.replay_only++;
    g_pass = 0; g_nlog[0] = 0; g_si = 0;
    uint64_t ro = 0, rn = 0;
    int fo = 0, fn = 0;
    fpu_reset();
    __try { ro = run(true); } __except (EXCEPTION_EXECUTE_HANDLER) { fo = 1; }
    fpu_reset();
    save(g_after);
    if (memcmp(g_after.arena, g_start.arena, ARENA_SIZE) || memcmp(g_after.globals, g_start.globals, GLOBALS_BYTES)) st.changed++;
    load(g_start);
    g_pass = 1; g_nlog[1] = 0; g_si = 0;
    fpu_reset();
    __try { rn = run(false); } __except (EXCEPTION_EXECUTE_HANDLER) { fn = 1; }
    fpu_reset();
    g_last_fault = fo != 0;
    g_last_ret = ro;
#ifdef FIX_TESTS
    // the fixed rewrite: never a fault, and every byte it changed inside its footprint
    if (fn && !fo) {
        if (st.fails++ < 4) printf("FIXED REWRITE FAULTED %s (world %d, %s): the original ran\n", name, g_world, g_phase);
        load(g_after);
        return;
    }
    if (!fn && !g_fp.replay_only)
        for (uint32_t i = 0; i < ARENA_SIZE; i++) {
            if (g_arena[i] == g_start.arena[i] || in_footprint(g_arena + i)) continue;
            uint32_t rel;
            const char* r = region_of(i, &rel);
            if (st.fp_fails++ < 3) printf("FOOTPRINT (fixed) %s (world %d, %s): %s+0x%x changed outside it\n", name, g_world, g_phase, r, rel);
            break;
        }
    if (fo && !fn) {                                              // the original crashed; the fixed one ran
        st.fixed++;
        fix_validate(name, st);
        load(g_after);
        return;
    }
#endif
    if (fo || fn) {
        st.faults++;
        if (fo != fn && st.fails++ < 4) printf("MISMATCH %s (world %d, %s): original %s, rewrite %s\n", name, g_world, g_phase,
                                                fo ? "faulted" : "ran", fn ? "faulted" : "ran");
        load(g_after);
        return;
    }
    uint8_t now_globals[GLOBALS_BYTES];
    snap_globals(now_globals);
    int nlog0 = g_nlog[0] < LOGN ? g_nlog[0] : LOGN;
    bool bad = ro != rn || memcmp(g_after.arena, g_arena, ARENA_SIZE) || memcmp(g_after.globals, now_globals, GLOBALS_BYTES) ||
               g_after.heap_top != g_heap_top || g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], sizeof(LogEntry) * nlog0);
#ifdef FIX_TESTS
    if (bad && fix_lost_bead_found()) {                           // the original left a bead NULL; the fixed one found it
        st.fixed++;
        fix_validate(name, st);
        bad = false;
    }
#endif
    if (bad && st.fails++ < 4) {
        printf("MISMATCH %s (world %d, %s, PC %d)\n", name, g_world, g_phase, g_pc53 ? 53 : 24);
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
        if (nl > LOGN) nl = LOGN;
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
    // the footprint must cover every byte the original changed (unless the call is left to the replay)
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
                if (pa[i] != ps[i] && !in_footprint((void*)(uintptr_t)(g.at + i)) &&
                    !in_ranges(g.at + i, g_auto_saved, sizeof g_auto_saved / sizeof *g_auto_saved)) {
                    if (st.fp_fails++ < 3) printf("FOOTPRINT %s (world %d, %s): global %s changed outside it\n", name, g_world, g_phase, g.what);
                    break;
                }
            pa += g.n; ps += g.n;
        }
    }
    load(g_after);                                                    // go on from the original's result
}

// CHECK(rewrite, args...): the arguments are evaluated once; the original is the same signature at the
// rewrite's registered address
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

// ---- coverage ----------------------------------------------------------------------------------------------------
enum {
    C_LOADED, C_REVERSED, C_FALLBACK, C_BADRES, C_CP_CHANGED, C_STAMPED, C_OFFLINE, C_BEAD_NULL, C_FAULTS,
    C_GAVE_UP, C_NEXT_SEG, C_BACK_SEG, C_WRONG_WAY, C_RABBIT_WRAP, C_MIL_BUILT, C_DRAWN, C_PANICS, C_ADV_NULL, C_N
};
static const char* const g_cov_names[] = {
    "lines loaded", "reversed", "dlatlong fallback", "bad resources", "checkpoint changes", "node times stamped",
    "bead off the line", "bead left NULL", "faults (both passes)", "get_nearest_bead gave up", "bead to next node",
    "bead back a node", "wrong way", "rabbit past a node", "editor lines built", "lines drawn", "LogPanics (pool out)",
    "advance_bead on a NULL bead (faults)",
};
static long g_cov[C_N];

// ---- a synthetic line resource ---------------------------------------------------------------------------------
struct Path { int n; double x[MAXSEG], z[MAXSEG], vx[MAXSEG], vz[MAXSEG], len[MAXSEG]; };
static Path g_path;

static void build_resource(int n) {
    uint8_t* src = g_arena + OFF_SRC;
    memset(src, 0, 0xc + MAXSEG * 0x44);
    IdealLineRes* r = (IdealLineRes*)src;
    r->magic = -2;
    r->seg_size = 0x44;
    r->count = n;
    g_res_version = 3;
    g_res_size = 0xc + n * 0x44 + (chance(20) ? ri(1, 3) : 0);            // an odd size: the byte copy
    if (chance(4)) { r->magic = chance(50) ? ri(1, 9) : ri(-9, -3); g_cov[C_BADRES]++; }
    else if (chance(3)) { r->seg_size = chance(50) ? 0x40 : 0x48; g_cov[C_BADRES]++; }
    else if (chance(3)) { g_res_version = ri(0, 5) == 3 ? 2 : 4; g_cov[C_BADRES]++; }
    double cx = rf(-3000, 3000), cz = rf(-3000, 3000), rx = rf(60, 1500), rz = rf(60, 1500);
    double dir = chance(50) ? 1.0 : -1.0, rot = rf(0, 6.283f), noise = rf(0, 0.3f);
    Path& p = g_path;
    p.n = n;
    for (int i = 0; i < n; i++) {
        double th = rot + dir * 6.2831853 * (i + rf(-0.3f, 0.3f) * noise) / n;
        double w = 1.0 + rf(-1, 1) * noise * 0.3;
        p.x[i] = cx + rx * w * cos(th);
        p.z[i] = cz + rz * w * sin(th);
    }
    for (int i = 0; i < n; i++) {
        int a = (i + n - 1) % n, b = (i + 1) % n;
        double k = rf(0.7f, 1.3f);
        p.vx[i] = (p.x[b] - p.x[a]) * 0.5 * k;
        p.vz[i] = (p.z[b] - p.z[a]) * 0.5 * k;
        double dx = p.x[b] - p.x[i], dz = p.z[b] - p.z[i];
        p.len[i] = sqrt(dx * dx + dz * dz) * rf(1.0f, 1.08f) + 0.5;
    }
    int ncp = ri(0, 4);
    float dist = 0;
    for (int i = 0; i < n; i++) {
        ILSeg* s = res_seg(r, i);
        s->next = (ILSeg*)(uintptr_t)rnd();
        s->p.x = sp((float)p.x[i]);
        s->p.z = sp((float)p.z[i]);
        s->v.x = sp((float)p.vx[i]);
        s->v.z = sp((float)p.vz[i]);
        s->width = sp(rf(3, 20));
        s->speed = sp(rf(8, 90));
        s->curvature = rf(-0.1f, 0.1f);
        s->length = (float)p.len[i];
        s->dist = chance(97) ? dist : sp(dist);
        dist += s->length;
        s->index = (int16_t)(chance(90) ? i : ri(-5, 100));
        s->checkpoint = (uint8_t)(ncp ? 1 + i * ncp / n : 0);
        if (chance(3)) s->checkpoint = (uint8_t)ri(0, 7);
        s->_2b = (uint8_t)rnd();
        s->_2c = rnd(); s->_30 = rnd();
        s->_34 = rf(-50, 50);
        s->_38 = rnd();
        s->_3c = chance(25) ? 0xfeedbeefu : rnd();
        s->_40 = rnd();
    }
}

// a point on the source loop: node i, t along the Hermite curve, `lat` metres to the side
static void path_point(double u, double lat, float* ox, float* oz) {
    const Path& p = g_path;
    int n = p.n;
    double fl = floor(u);
    int i = ((int)fl % n + n) % n, j = (i + 1) % n;
    double t = u - fl;
    double h00 = (2 * t - 3) * t * t + 1, h01 = (-2 * t + 3) * t * t, h10 = ((t - 2) * t + 1) * t, h11 = (t - 1) * t * t;
    double x = p.x[i] * h00 + p.vx[i] * h10 + p.x[j] * h01 + p.vx[j] * h11;
    double z = p.z[i] * h00 + p.vz[i] * h10 + p.z[j] * h01 + p.vz[j] * h11;
    double d00 = 6 * t * t - 6 * t, d01 = -6 * t * t + 6 * t, d10 = 3 * t * t - 4 * t + 1, d11 = 3 * t * t - 2 * t;
    double tx = p.x[i] * d00 + p.vx[i] * d10 + p.x[j] * d01 + p.vx[j] * d11;
    double tz = p.z[i] * d00 + p.vz[i] * d10 + p.z[j] * d01 + p.vz[j] * d11;
    double l = sqrt(tx * tx + tz * tz);
    if (l > 1e-9) { x += -tz / l * lat; z += tx / l * lat; }
    *ox = (float)x;
    *oz = (float)z;
}

// ---- the cars ------------------------------------------------------------------------------------------------------
struct CarState { double u, lat, speed; int dir; Point2D pos, prev; };
static CarState g_car[NCL];

static void move_car(int i, double dt) {
    CarState& c = g_car[i];
    int n = g_path.n;
    c.prev = c.pos;
    double seglen = g_path.len[((int)floor(c.u) % n + n) % n];
    c.u += c.dir * c.speed * dt / seglen;
    c.lat += rf(-0.4f, 0.4f);
    if (c.lat > 25) c.lat = 25;
    if (c.lat < -25) c.lat = -25;
    if (chance(2)) c.dir = -c.dir;
    if (chance(3)) c.speed = chance(80) ? rf(10, 90) : rf(0, 10);
    path_point(c.u, c.lat, &c.pos.x, &c.pos.z);
    int k = rnd() % 1000;
    if (k < 8) {                                                     // a teleport near the line
        c.u = rf(0, (float)n);
        c.lat = rf(-60, 60);
        path_point(c.u, c.lat, &c.pos.x, &c.pos.z);
    } else if (k < 12) {                                             // or far off it
        c.pos.x = rf(-12000, 12000);
        c.pos.z = rf(-12000, 12000);
    } else if (k < 14) {
        c.pos.x = sp(c.pos.x);
        c.pos.z = sp(c.pos.z);
    }
}

// the lines the checks run on (the ones whose load succeeded)
// (the editor's line only while its nodes all came from the resource: a node it adds has no length, and the
// bead functions divide by it -- t goes infinite and their loops never end, in the game as here)
static int g_ncl;
static bool g_mil_bead_ok;
static IdealLine* any_line(bool bead) {
    switch (rnd() % 4) {
    case 0: return cil();
    case 1: if (!bead || g_mil_bead_ok) return mil(); return cil();
    default: return cl(ri(0, g_ncl - 1));
    }
}
static ILSeg* seg_of(IdealLine* l, int k) {
    ILSeg* s = l->head;
    if (!s) return 0;
    for (int i = 0; i < k; i++) s = s->next;
    return s;
}
static int loop_len(IdealLine* l) {
    ILSeg* h = l->head;
    if (!h) return 0;
    int n = 0;
    ILSeg* s = h;
    do { s = s->next; n++; } while (s != h && n < 10000);
    return n;
}
static bool bead_ok(IdealLine* l) { return l->head && l->bead_seg; }

static const char* const k_name_ild = (const char*)0x004ec41c;
static const char* const k_name_ili = (const char*)0x004ec428;
static uint8_t* rev_flag() { return g_arena + OFF_OPTS + 0x30; }

static void run_track_world() {
    g_heap_top = g_arena + OFF_HEAP;
    memset(g_arena, 0, ARENA_SIZE);
    int n = chance(10) ? ri(1, 3) : ri(4, MAXSEG);
    g_special_pct = chance(65) ? 0 : chance(70) ? 1 : 4;          // NaNs etc. in the nodes and arguments
#ifdef FIX_TESTS
    n = ri(3, MAXSEG);                                             // ordinary inputs: a line of 3 or more nodes,
    g_special_pct = 0;                                             // no NaNs or infinities
#endif
    build_resource(n);
    g_ild_missing_pct = chance(30) ? (chance(50) ? 100 : 30) : 0;
    g_ili_missing_pct = chance(10) ? 100 : 0;
    g_arena[OFF_OPTS + 0x25] = (uint8_t)chance(40);
    *rev_flag() = (uint8_t)chance(40);
    *g_tick = ri(0, 5000);
    // the lines
    g_phase = "load";
    g_pc53 = chance(50);
    g_ncl = ri(1, NCL);
    for (int i = 0; i < g_ncl; i++) {
        CHECK(CenterLine_ctor, cl(i), 0, chance(50) ? (const uint8_t*)0 : (const uint8_t*)rev_flag());
        if (cl(i)->res) g_cov[C_LOADED]++;
    }
    if (g_ild_missing_pct) g_cov[C_FALLBACK]++;
    if (g_arena[OFF_OPTS + 0x25] || *rev_flag()) g_cov[C_REVERSED]++;
    CHECK(ConstIdealLine_ctor, cil(), 0);
    CHECK(ConstIdealLine_load, cil(), 0, chance(50) ? k_name_ild : k_name_ili, (uint8_t)chance(40));
    CHECK(MIL_ctor, mil(), 0);
    g_mil_bead_ok = false;
    if (chance(60)) {
        CHECK(MIL_load, mil(), 0, k_name_ili, (uint8_t)chance(40));
        g_mil_bead_ok = mil()->head != 0;
    } else {
        int m = ri(1, n);
        for (int k = 0; k < m; k++) {
            Point2D* p = slot(20);
            p->x = sp((float)g_path.x[k]); p->z = sp((float)g_path.z[k]);
            if (chance(20) && k > 1) CHECK(MIL_add_control_near, mil(), 0, (const Point2D*)p, sp(rf(3, 15)));
            else CHECK(MIL_add_next_control, mil(), 0, (const Point2D*)p, sp(rf(3, 15)), ri(0, 4));
            if (chance(10)) CHECK(MIL_autostop, mil(), 0);
        }
        g_cov[C_MIL_BUILT]++;
    }
    g_pc53 = false;
    for (int i = 0; i < NCL; i++) {
        g_car[i].u = rf(0, (float)n);
        g_car[i].lat = rf(-8, 8);
        g_car[i].speed = rf(5, 90);
        g_car[i].dir = chance(85) ? 1 : -1;
        path_point(g_car[i].u, g_car[i].lat, &g_car[i].pos.x, &g_car[i].pos.z);
    }
    if (getenv("VP_TRACE") && cl(0)->res) {
        IdealLineRes* r = cl(0)->res;
        for (int k = 0; k < r->count; k++) {
            ILSeg* q = res_seg(r, k);
            printf("node %d: p %.1f %.1f v %.1f %.1f len %.1f dist %.1f cp %d next %d\n", k, q->p.x, q->p.z, q->v.x, q->v.z,
                   q->length, q->dist, q->checkpoint, (int)(((uint8_t*)q->next - (uint8_t*)r - 12) / 0x44));
        }
        for (int k = 0; k < g_path.n; k++) printf("path %d: %.1f %.1f\n", k, g_path.x[k], g_path.z[k]);
        printf("car0 %.1f %.1f u %.2f\n", g_car[0].pos.x, g_car[0].pos.z, g_car[0].u);
    }
    // on the grid: each centre line found afresh at its car (RaceDeity::Reset -> CenterLine::reset)
    for (int i = 0; i < g_ncl; i++)
        if (cl(i)->res && chance(90)) { cl(i)->car_pos = g_car[i].pos; CHECK(CenterLine_reset, cl(i), 0); }
    // the race
    g_phase = "race";
    int steps = ri(40, 220);
    int reloads = 0;
    for (int step = 0; step < steps; step++) {
        *g_tick += ri(1, 3);
        double dt = chance(85) ? 0.016 * ri(1, 4) : rf(0.1f, 1.2f);   // (sometimes a big step: several nodes at once)
        for (int i = 0; i < NCL; i++) move_car(i, dt);
        for (int i = 0; i < g_ncl; i++) {
            CenterLine* c = cl(i);
            Point2D* pos = slot(i);
            *pos = g_car[i].pos;
            if (c->head && !c->bead_seg) g_cov[C_BEAD_NULL]++;
            CHECK(CenterLine_update, c, 0, (const Point2D*)pos, i);
            if (g_last_fault) g_cov[C_FAULTS]++;
            // a car lost by its line is found again (the deity teleports it, or the stranded check): the
            // line reset at the car -- and if that leaves the bead NULL, the harness puts it at the head
            if (c->res && (g_last_fault || !c->bead_seg || (!c->bead_on_line && chance(25)))) {
                c->car_pos = g_car[i].pos;
                CHECK(CenterLine_reset, c, 0);
                if (!c->bead_seg && chance(70)) { c->bead_seg = c->head; c->bead_t = 0; }
            }
            if (g_last_fault) continue;
            if (c->head && c->bead_seg && !c->bead_on_line) g_cov[C_OFFLINE]++;
            if ((uint8_t)g_last_ret) g_cov[C_CP_CHANGED]++;
            if (getenv("VP_TRACE") && i == 0 && c->head && c->bead_seg)
                printf("w%d s%d car %.1f %.1f bead %d t %.3f pos %.1f %.1f on %d lat %.2f\n", g_world, step, pos->x, pos->z,
                       (int)(((uint8_t*)c->bead_seg - (uint8_t*)c->res - 12) / 0x44), c->bead_t, c->pos.x, c->pos.z, c->bead_on_line, c->lat);
            if (chance(3) && c->res) CHECK(CenterLine_reset, c, 0);
        }
        // the AI's line: update_car_info then the rabbit, as AICar does
        {
            IdealLine* l = chance(80) || !g_mil_bead_ok ? (IdealLine*)cil() : (IdealLine*)mil();
            Point2D* pos = slot(8);
            Point2D* dir = slot(9);
            *pos = g_car[0].pos;
            dir->x = sp(g_car[0].pos.x - g_car[0].prev.x);
            dir->z = sp(g_car[0].pos.z - g_car[0].prev.z);
            ILSeg* before = l->bead_seg;
            CHECK(IdealLine_update_car_info, l, 0, (const Point2D*)pos, (const Point2D*)dir);
            if (g_last_fault) g_cov[C_FAULTS]++;
            if (l->head && (g_last_fault || !l->bead_seg || (!l->bead_on_line && chance(25)))) {
                CHECK(IdealLine_reset_bead_position, l, 0);
                if (!l->bead_seg && chance(70)) { l->bead_seg = l->head; l->bead_t = 0; }
            }
            if (before && l->bead_seg && before != l->bead_seg) {
                if (before->next == l->bead_seg) g_cov[C_NEXT_SEG]++;
                else if (l->bead_seg->next == before) g_cov[C_BACK_SEG]++;
            }
            if (l->head && l->bead_seg) {
                ILSeg* b = l->bead_seg;
                CHECK(IdealLine_get_rabbit_position, l, 0, slot(10), slot(11), chance(95) ? rf(0, 80) : rf(-5, 0),
                      chance(50) ? pslot(12) : (ILinePos*)0);
                if (pslot(12)->seg && pslot(12)->seg != b) g_cov[C_RABBIT_WRAP]++;
            }
        }
        // one or two more entry points
        for (int extra = ri(0, 2); extra > 0; extra--) {
            int op = rnd() % 40;
            IdealLine* l = any_line(op <= 12);
            CenterLine* c = cl(ri(0, g_ncl - 1));
            Point2D* pt = slot(13);
            int ci = ri(0, NCL - 1);
            pt->x = sp(g_car[ci].pos.x + rf(-20, 20));
            pt->z = sp(g_car[ci].pos.z + rf(-20, 20));
            if (chance(5)) { pt->x = rf(-12000, 12000); pt->z = rf(-12000, 12000); }
            switch (op) {
            case 0: CHECK(IdealLine_nearest_node_to, l, 0, (const Point2D*)pt); break;
            case 1: CHECK(IdealLine_get_nearest_pair, l, 0, (const Point2D*)pt); break;
            case 2: {
                ILinePos* p = pslot(14);
                p->seg = chance(50) ? 0 : l->bead_seg;
                p->t = chance(50) ? 0.0f : l->bead_t;
                float mx = chance(80) ? 10000.0f : rf(0, 200);
                CHECK(IdealLine_get_nearest_bead, l, 0, (const Point2D*)pt, p, mx);
                if (!g_last_fault && l->head && !p->seg) g_cov[C_GAVE_UP]++;
                break;
            }
            case 3:
                if (bead_ok(l)) CHECK(IdealLine_advance_bead, l, 0);
                else if (l->head) { CHECK(IdealLine_advance_bead, l, 0); if (g_last_fault) g_cov[C_ADV_NULL]++; }   // the stock AI crash
                break;
            case 4: CHECK(IdealLine_reset_bead_position, l, 0); break;
            case 5: CHECK(IdealLine_reset_to_head, l, 0); break;
            case 6: CHECK(IdealLine_segloop_count, l, 0); CHECK(IdealLine_get_actual_bead_position, l, 0, pslot(15)); break;
            case 7: CHECK(CenterLine_get_car_dlong_meters, c, 0); break;
#ifdef FIX_TESTS
            case 8: if (c->res) CHECK(CenterLine_get_car_dlong_cookie, c, 0); break;       // (a NULL bead too)
#else
            case 8: if (c->res && c->bead_seg) CHECK(CenterLine_get_car_dlong_cookie, c, 0); break;
#endif
            case 9: if (c->res && c->res->count > 0) {
                uint32_t k = (uint32_t)ri(0, c->res->count - 1) << 16 | (rnd() & 0xffff);
                CHECK(CenterLine_convert_cookie_to_ilpos, c, 0, pslot(16), k);
                CHECK(CenterLine_convert_cookie_to_meters, c, 0, k);
            } break;
            case 10: if (c->res && c->res->count > 0) {
                ILinePos* p = pslot(17);
                p->seg = res_seg(c->res, ri(0, c->res->count - 1));
                p->t = chance(90) ? rf(0, 1) : sp(rf(-0.5f, 1.5f));
                CHECK(CenterLine_convert_ilpos_to_cookie, c, 0, (const ILinePos*)p);
                CHECK(CenterLine_convert_ilpos_to_meters, c, 0, (const ILinePos*)p);
            } break;
            case 11: {
                float m = sp(rf(-20, c->total + 20));
                if (c->res) CHECK(CenterLine_convert_meters_to_ilpos, c, 0, pslot(18), m);
                if (c->res && c->res->count > 0) CHECK(CenterLine_convert_meters_to_cookie, c, 0, m);
                else if (!c->res) CHECK(CenterLine_convert_meters_to_ilpos, c, 0, pslot(18), m);
                break;
            }
            case 12: CHECK(CenterLine_get_ilpos_at_point, c, 0, pslot(19), (const Point2D*)pt); break;
            case 13: {
                CenterLine* o = cl(ri(0, g_ncl - 1));
#ifdef FIX_TESTS
                if (c->res && o->res && o->times && c->times && c->res->count > 0) CHECK(CenterLine_time_between, c, 0, (const CenterLine*)o);
#else
                if (c->res && c->bead_seg && o->bead_seg && o->times && c->times && c->res->count > 0) CHECK(CenterLine_time_between, c, 0, (const CenterLine*)o);
#endif
                break;
            }
            case 14: {
                P3* v = (P3*)(g_arena + OFF_OUT + 21 * 8);
                v->x = sp(rf(-40, 40)); v->y = sp(rf(-3, 3)); v->z = sp(rf(-40, 40));
                if (chance(20)) { v->x = rf(-1.5f, 1.5f); v->z = rf(-1.5f, 1.5f); }
                CHECK(CenterLine_wrong_way, c, 0, (const P3*)v);
                if ((uint8_t)g_last_ret) g_cov[C_WRONG_WAY]++;
                break;
            }
            case 15: CHECK(CenterLine_get_next_checkpoint, c, 0); break;
            case 16: if (bead_ok(c)) CHECK(CenterLine_update_2d_data, c, 0); CHECK(CenterLine_calculate_lat, c, 0); break;
            case 17: g_pc53 = chance(70); CHECK(IdealLine_Draw, l, 0, (const Point2D*)pt, (uint8_t)chance(30)); g_pc53 = false; g_cov[C_DRAWN]++; break;
            case 18: if (l->head) { g_pc53 = chance(70); CHECK(ILSeg_DrawLine, seg_of(l, ri(0, loop_len(l) - 1)), 0); g_pc53 = false; } break;
            case 19: g_pc53 = chance(70); CHECK(project, (const Point2D*)pt, slot(22)); g_pc53 = false; break;
            case 20: if (l->head) {
                ILSeg* s = seg_of(l, ri(0, loop_len(l) - 1));
                float t = chance(90) ? rf(0, 1) : sp(rf(-1, 2));
                CHECK(ILSeg_QuickEval, (const ILSeg*)s, 0, slot(23), t);
                CHECK(ILSeg_QuickTan, (const ILSeg*)s, 0, slot(24), t);
            } break;
            case 21: if (l->head) {
                ILSeg* s = seg_of(l, ri(0, loop_len(l) - 1));
                CHECK(ILSeg_Curvature, (const ILSeg*)s, 0);
                CHECK(ILSeg_Curv, (const ILSeg*)s, 0);
            } break;
            // the line editor's calls
            case 22: CHECK(MIL_modify_speed_everywhere, mil(), 0, sp(rf(0.8f, 1.2f))); break;
            case 23: CHECK(MIL_modify_vscale_everywhere, mil(), 0, sp(rf(0.8f, 1.2f))); break;
            case 24: CHECK(MIL_modify_speed_at, mil(), 0, (const Point2D*)pt, sp(rf(5, 90))); break;
            case 25: {
                Point2D* v = slot(25);
                v->x = sp(rf(-50, 50)); v->z = sp(rf(-50, 50));
                CHECK(MIL_modify_v_at, mil(), 0, (const Point2D*)pt, (const Point2D*)v, sp(rf(5, 90)));
                break;
            }
            case 26: CHECK(MIL_modify_wid_at, mil(), 0, (const Point2D*)pt, sp(rf(-2, 2))); break;
            case 27: CHECK(MIL_get_speed_at, mil(), 0, (const Point2D*)pt); CHECK(MIL_get_wid_at, mil(), 0, (const Point2D*)pt); break;
            case 28: CHECK(MIL_snap_to, mil(), 0, (const Point2D*)pt); break;
            case 29: CHECK(MIL_autostop, mil(), 0); break;
            case 30: if (loop_len(mil()) < 200) { CHECK(MIL_add_next_control, mil(), 0, (const Point2D*)pt, sp(rf(3, 15)), ri(0, 4)); g_mil_bead_ok = false; } break;
            case 31: if (loop_len(mil()) < 200) { CHECK(MIL_add_control_near, mil(), 0, (const Point2D*)pt, sp(rf(3, 15))); g_mil_bead_ok = false; } break;
            case 32: if (loop_len(mil()) > 3) { CHECK(MIL_delete_control_near, mil(), 0, (const Point2D*)pt); g_mil_bead_ok = false; } break;
            case 33: if (loop_len(mil()) > 3) { CHECK(MIL_delete_last_node, mil(), 0); g_mil_bead_ok = false; } break;
            case 34: if (loop_len(mil()) > 3) {
                ILSeg* s = seg_of(mil(), ri(0, loop_len(mil()) - 1));
                if (mil()->bead_seg == s) break;                  // a freed bead would be read by the next update
                CHECK(MIL_delete_segment, mil(), 0, s);
                g_mil_bead_ok = false;
            } break;
            case 35: if (chance(10)) { g_pc53 = true; CHECK(MIL_save, mil(), 0, k_name_ili); g_pc53 = false; } break;
            case 36: if (chance(20) && reloads < 3) {
                reloads++;
                g_pc53 = chance(50);
                if (chance(50)) {
                    CHECK(MIL_clear, mil(), 0);
                    CHECK(MIL_load, mil(), 0, k_name_ili, (uint8_t)chance(50));
                    g_mil_bead_ok = mil()->head != 0;
                    mil()->bead_seg = mil()->head;                // (the old bead is a freed node)
                }
                else CHECK(ConstIdealLine_load, cil(), 0, k_name_ild, (uint8_t)chance(50));
                if (g_last_fault) g_cov[C_PANICS]++;
                g_pc53 = false;
            } break;
            case 37: if (chance(20) && reloads < 3) {
                reloads++;
                g_pc53 = chance(50);
                CHECK(ILineTry, chance(50) ? k_name_ild : k_name_ili, (uint8_t)chance(50));
                CHECK(IdealLine_load_res, l, 0, k_name_ili, (uint8_t)chance(50));
                g_pc53 = false;
            } break;
            case 38: {                                           // the stranded-car teleport: far away, then reset
                int k = ri(0, g_ncl - 1);
                CenterLine* ck = cl(k);
                g_car[k].pos.x = rf(-12000, 12000);
                g_car[k].pos.z = rf(-12000, 12000);
                *slot(k) = g_car[k].pos;
                ck->car_pos = g_car[k].pos;
                if (ck->res) CHECK(CenterLine_reset, ck, 0);
                break;
            }
            case 39: CHECK(AIGetTrackInfo, (int)rnd()); break;
            }
        }
        for (int i = 0; i < g_ncl; i++) {
            CenterLine* c = cl(i);
            if (c->started) g_cov[C_STAMPED]++;
        }
    }
    // the end: the editor's line filled from the resource until its pool runs out (LogPanic), the destructors
    g_phase = "teardown";
    g_pc53 = chance(50);
    if (cl(0)->res && cl(0)->res->count > 0 && chance(30)) {
        for (int k = 0; k < 600 / cl(0)->res->count + 2; k++) {
            CHECK(MIL_copy_res_to_loop, mil(), 0, cl(0)->res);
            if (g_last_fault) { g_cov[C_PANICS]++; break; }
        }
    }
    for (int i = 0; i < g_ncl; i++) CHECK(CenterLine_dtor, cl(i), 0);
    CHECK(ConstIdealLine_dtor, cil(), 0);
    CHECK(MIL_dtor, mil(), 0);
    CHECK(IdealLine_dtor, (IdealLine*)cil(), 0);
    g_pc53 = false;
}

// ---- maths: a ring of random nodes ------------------------------------------------------------------------------
static float mix(float x, int pct) { return (int)(rnd() % 100) < pct ? special() : x; }
static void fill_ring(int pct) {
    IdealLineRes* r = ringres();
    r->magic = -2; r->seg_size = 0x44; r->count = NRING;
    double cx = rf(-500, 500), cz = rf(-500, 500), rad = rf(20, 400);
    float dist = 0;
    for (int i = 0; i < NRING; i++) {
        ILSeg* s = ring(i);
        s->next = ring(i + 1);
        double th = 6.2831853 * i / NRING;
        s->p.x = mix((float)(cx + rad * cos(th)) + rf(-5, 5), pct);
        s->p.z = mix((float)(cz + rad * sin(th)) + rf(-5, 5), pct);
        s->v.x = mix((float)(-sin(th) * rad * 0.8) + rf(-20, 20), pct);
        s->v.z = mix((float)(cos(th) * rad * 0.8) + rf(-20, 20), pct);
        s->width = mix(rf(2, 20), pct);
        s->speed = mix(rf(0, 90), pct);
        s->length = rf(0.5f, 300);                     // the bead loops divide by it: kept sane (no hang)
        s->dist = chance(90) ? dist : mix(dist, 50);
        dist += s->length;
        s->index = (int16_t)(chance(85) ? i : ri(-3, 12));
        s->checkpoint = (uint8_t)ri(0, 5);
        s->_34 = mix(rf(-1, 1), pct);
    }
}
static void fill_fake_cl(CenterLine* c, int k, int pct) {
    memset(c, 0, sizeof *c);
    c->vtable = (void**)0x004db6f0;
    c->res = ringres();
    c->head = ring(0);
    c->bead_seg = chance(95) ? ring(ri(0, NRING - 1)) : 0;
    c->bead_t = chance(90) ? rf(0, 1) : mix(rf(-1, 2), 60);
    c->bead_on_line = (uint8_t)chance(80);
    float* f = &c->car_pos.x;
    f[0] = mix(rf(-600, 600), pct); f[1] = mix(rf(-600, 600), pct);
    c->pos.x = mix(rf(-600, 600), pct); c->pos.z = mix(rf(-600, 600), pct);
    c->normal.x = mix(rf(-1, 1), pct); c->normal.z = mix(rf(-1, 1), pct);
    c->times = (float*)(g_arena + OFF_FTIMES + k * 32);
    for (int i = 0; i < NRING; i++) c->times[i] = mix(rf(0, 1000), pct);
    c->total = mix(rf(100, 3000), pct);
}

static void run_math_world() {
    g_heap_top = g_arena + OFF_HEAP;
    memset(g_arena, 0, OFF_HEAP);
    g_phase = "maths";
    int pct = chance(50) ? 3 : 15;
    for (int it = 0; it < 400; it++) {
        fill_ring(pct);
        fill_fake_cl(fcl(0), 0, pct);
        fill_fake_cl(fcl(1), 1, pct);
        CenterLine* c = fcl(0);
        IdealLine* l = rline();
        memset(l, 0, sizeof *l);
        l->vtable = (void**)0x004db6d0;
        l->head = ring(0);
        l->bead_seg = ring(ri(0, NRING - 1));
        l->bead_t = rf(0, 1);
        l->car_pos.x = mix(ring(ri(0, 7))->p.x + rf(-40, 40), pct);
        l->car_pos.z = mix(ring(ri(0, 7))->p.z + rf(-40, 40), pct);
        ILSeg* s = ring(ri(0, NRING - 1));
        float t = chance(80) ? rf(0, 1) : mix(rf(-2, 3), 70);
        g_pc53 = chance(15);
        switch (rnd() % 16) {
        case 0: CHECK(ILSeg_QuickEval, (const ILSeg*)s, 0, slot(0), t); CHECK(ILSeg_QuickTan, (const ILSeg*)s, 0, slot(1), t); break;
        case 1: CHECK(ILSeg_Curvature, (const ILSeg*)s, 0); CHECK(ILSeg_Curv, (const ILSeg*)s, 0); break;
        case 2: CHECK(CenterLine_calculate_lat, c, 0); break;
        case 3: {
            ILinePos* p = pslot(2);
            p->seg = s; p->t = t;
            CHECK(CenterLine_convert_ilpos_to_meters, c, 0, (const ILinePos*)p);
            CHECK(CenterLine_convert_ilpos_to_cookie, c, 0, (const ILinePos*)p);
            break;
        }
        case 4: {
            uint32_t k = (uint32_t)ri(0, NRING - 1) << 16 | (rnd() & 0xffff);
            CHECK(CenterLine_convert_cookie_to_ilpos, c, 0, pslot(3), k);
            CHECK(CenterLine_convert_cookie_to_meters, c, 0, k);
            break;
        }
        case 5: {
            float m = mix(rf(-100, 3000), pct);
            CHECK(CenterLine_convert_meters_to_ilpos, c, 0, pslot(4), m);
            CHECK(CenterLine_convert_meters_to_cookie, c, 0, m);
            break;
        }
        case 6: if (c->bead_seg && fcl(1)->bead_seg) CHECK(CenterLine_time_between, c, 0, (const CenterLine*)fcl(1)); break;
        case 7: {
            P3* v = (P3*)slot(5);
            v->x = mix(rf(-30, 30), pct); v->y = mix(rf(-3, 3), pct); v->z = mix(rf(-30, 30), pct);
            CHECK(CenterLine_wrong_way, c, 0, (const P3*)v);
            CHECK(CenterLine_get_next_checkpoint, c, 0);
            if (c->bead_seg) CHECK(CenterLine_get_car_dlong_meters, c, 0);
            break;
        }
        case 8: if (c->bead_seg && !(fabs(c->bead_t) > 4)) CHECK(CenterLine_update_2d_data, c, 0); break;
        case 9: if (!(fabs(l->bead_t) > 4)) CHECK(IdealLine_advance_bead, l, 0); break;
        case 10: CHECK(IdealLine_get_rabbit_position, l, 0, slot(6), slot(7), rf(0, 200), chance(50) ? pslot(8) : (ILinePos*)0); break;
        case 11: {
            Point2D* pt = slot(9);
            pt->x = mix(ring(ri(0, 7))->p.x + rf(-60, 60), pct);
            pt->z = mix(ring(ri(0, 7))->p.z + rf(-60, 60), pct);
            ILinePos* p = pslot(10);
            p->seg = chance(50) ? 0 : ring(ri(0, 7));
            p->t = chance(50) ? 0.0f : rf(0, 1);
            CHECK(IdealLine_get_nearest_bead, l, 0, (const Point2D*)pt, p, chance(70) ? 10000.0f : rf(0, 500));
            CHECK(IdealLine_nearest_node_to, l, 0, (const Point2D*)pt);
            CHECK(IdealLine_get_nearest_pair, l, 0, (const Point2D*)pt);
            break;
        }
        case 12: {                                               // fixup_res on a random resource
            IdealLineRes* r = (IdealLineRes*)(g_arena + OFF_SCRATCH);
            int cnt = ri(1, 16);
            r->magic = -2; r->seg_size = 0x44; r->count = cnt;
            uint32_t* w = (uint32_t*)((uint8_t*)r + 0xc);
            for (int i = 0; i < cnt * 17; i++) w[i] = chance(70) ? ubits(mix(rf(-100, 100), pct)) : rnd();
            for (int i = 0; i < cnt; i++) {
                ILSeg* q = res_seg(r, i);
                if (chance(40)) q->checkpoint = (uint8_t)ri(0, 5);
                if (chance(30)) q->_3c = 0xfeedbeefu;
            }
            if (cnt > 1 && chance(50)) {                         // checkpoints as a track has them
                int ncp = ri(1, 4);
                for (int i = 0; i < cnt; i++) res_seg(r, i)->checkpoint = (uint8_t)(1 + i * ncp / cnt);
            }
            CHECK(fixup_res, r, (uint8_t)chance(60));
            break;
        }
        case 13: CHECK(AIGetTrackInfo, (int)rnd()); CHECK(IdealLine_segloop_count, l, 0); break;
        case 14: {
            IdealLine* x = (IdealLine*)(g_arena + OFF_SCRATCH);
            for (int i = 0; i < 0x40; i++) g_arena[OFF_SCRATCH + i] = (uint8_t)rnd();
            switch (rnd() % 5) {
            case 0: CHECK(IdealLine_ctor, x, 0); break;
            case 1: CHECK(IdealLine_init, x, 0); break;
            case 2: CHECK(ConstIdealLine_ctor, (ConstIdealLine*)x, 0); break;
            case 3: CHECK(IdealLine_reset_to_head, x, 0); break;
            default: CHECK(IdealLine_get_actual_bead_position, x, 0, pslot(11)); break;
            }
            break;
        }
        case 15: {
            Point2D* pt = slot(12);
            pt->x = mix(rf(-600, 600), pct); pt->z = mix(rf(-600, 600), pct);
            CHECK(project, (const Point2D*)pt, slot(13));
            CHECK(ILSeg_DrawLine, s, 0);
            l->rabbit_valid = (uint8_t)chance(50);
            CHECK(IdealLine_Draw, l, 0, (const Point2D*)pt, (uint8_t)chance(50));
            break;
        }
        }
        g_pc53 = false;
    }
}

// the watchdog: a check that runs for 20 s has hung (a loop the inputs never leave)
static DWORD WINAPI watchdog(void*) {
    LONG last = -1;
    int still = 0;
    for (;;) {
        Sleep(1000);
        LONG now = g_busy;
        if (now == last) {
            if (++still >= 20) {
                printf("HUNG in %s (world %d, %s)\n", g_busy_name, g_world, g_phase);
                ExitProcess(4);
            }
        } else {
            still = 0;
        }
        last = now;
    }
}

#ifdef FIX_TESTS
// ==== the fix tests ================================================================================================
template <typename F> static bool ran(F f) {
    __try {
        f();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
static void busy(const char* what) {
    g_busy_name = what;
    InterlockedIncrement(&g_busy);
}
static float qnan() { return fbits(0x7fc00000u); }

// a fresh world: the arena cleared, the stubs' resource served, the FPU as the physics thread has it
static void fresh_world() {
    g_heap_top = g_arena + OFF_HEAP;
    memset(g_arena, 0, ARENA_SIZE);
    g_ild_missing_pct = 0;
    g_ili_missing_pct = 0;
    g_special_pct = 0;
    g_pc53 = false;
    fpu_reset();
}
// the served resource loaded as centre lines 0 and 1 and the AI's ConstIdealLine
static void load_lines() {
    CenterLine_ctor(cl(0), 0, 0);
    CenterLine_ctor(cl(1), 0, 0);
    ConstIdealLine_ctor(cil(), 0);
    ConstIdealLine_load(cil(), 0, k_name_ili, 0);
}
// a line resource of our own: nodes, tangents and lengths as given
static void build_custom(int n, const double* x, const double* z, const double* vx, const double* vz, const float* len) {
    uint8_t* src = g_arena + OFF_SRC;
    memset(src, 0, 0xc + MAXSEG * 0x44);
    IdealLineRes* r = (IdealLineRes*)src;
    r->magic = -2;
    r->seg_size = 0x44;
    r->count = n;
    g_res_version = 3;
    g_res_size = 0xc + n * 0x44;
    float dist = 0;
    g_path.n = n;
    for (int i = 0; i < n; i++) {
        ILSeg* s = res_seg(r, i);
        s->p.x = (float)x[i];
        s->p.z = (float)z[i];
        s->v.x = (float)vx[i];
        s->v.z = (float)vz[i];
        s->width = 12.0f;
        s->speed = 30.0f;
        s->length = len[i];
        s->dist = dist;
        dist += len[i];
        s->index = (int16_t)i;
        s->checkpoint = (uint8_t)(1 + i * 3 / n);
        g_path.x[i] = x[i];
        g_path.z[i] = z[i];
        g_path.vx[i] = vx[i];
        g_path.vz[i] = vz[i];
        g_path.len[i] = len[i];
    }
}
// a closed loop of n nodes round an ellipse, Catmull-Rom tangents, chord lengths; node `dup` (if >= 0) is moved onto
// the next one (a zero-length node), with its tangent zeroed too if zero_tan
static void build_ellipse(int n, int dup, bool zero_tan) {
    double x[MAXSEG], z[MAXSEG], vx[MAXSEG], vz[MAXSEG];
    float len[MAXSEG];
    for (int i = 0; i < n; i++) {
        double th = 6.2831853 * i / n;
        x[i] = 400 + 220 * cos(th);
        z[i] = -300 + 130 * sin(th);
    }
    if (dup >= 0) { x[dup] = x[(dup + 1) % n]; z[dup] = z[(dup + 1) % n]; }
    for (int i = 0; i < n; i++) {
        int a = (i + n - 1) % n, b = (i + 1) % n;
        vx[i] = (x[b] - x[a]) * 0.5;
        vz[i] = (z[b] - z[a]) * 0.5;
        double dx = x[b] - x[i], dz = z[b] - z[i];
        len[i] = (float)sqrt(dx * dx + dz * dz);
    }
    if (dup >= 0 && zero_tan) { vx[dup] = 0; vz[dup] = 0; }
    build_custom(n, x, z, vx, vz, len);
}
// a point s metres along the loop of chords from node 0, lat metres to the side
static Point2D drive_point(double s, double lat) {
    const Path& p = g_path;
    double total = 0;
    for (int i = 0; i < p.n; i++) {
        int j = (i + 1) % p.n;
        total += sqrt((p.x[j] - p.x[i]) * (p.x[j] - p.x[i]) + (p.z[j] - p.z[i]) * (p.z[j] - p.z[i]));
    }
    s = fmod(s, total);
    if (s < 0) s += total;
    Point2D r = {(float)p.x[0], (float)p.z[0]};
    for (int i = 0; i < p.n; i++) {
        int j = (i + 1) % p.n;
        double dx = p.x[j] - p.x[i], dz = p.z[j] - p.z[i], c = sqrt(dx * dx + dz * dz);
        if (c > 0 && s <= c) {
            double t = s / c;
            r.x = (float)(p.x[i] + t * dx - dz / c * lat);
            r.z = (float)(p.z[i] + t * dz + dx / c * lat);
            return r;
        }
        s -= c;
    }
    return r;
}
static int node_of(const ConstIdealLine* l, const ILSeg* s) { return (int)(((const uint8_t*)s - (const uint8_t*)l->res - 12) / 0x44); }
static void set_bead(IdealLine* l, ILSeg* s, float t) { l->bead_seg = s; l->bead_t = t; }

// ---- against the originals (nothing hooked yet) --------------------------------------------------------------------

// reset_bead_position: where the original finds the bead the fixed one finds the same; where it leaves it NULL the
// fixed one puts it at the nearest point of the line
static void test_reset_vs_original() {
    auto orig = (decltype(&IdealLine_reset_bead_position))(uintptr_t)addr_IdealLine_reset_bead_position;
    long same = 0, lost = 0, kinds[5] = {0, 0, 0, 0, 0};
    for (int w = 0; w < 80; w++) {
        busy("reset_bead_position vs the original");
        fresh_world();
        build_resource(ri(3, MAXSEG));
        load_lines();
        IdealLine* lines[2] = {cl(0), cil()};
        for (int k = 0; k < 250; k++) {
            IdealLine* l = lines[k & 1];
            if (!l->head) continue;
            Point2D pt;
            int kind = ri(0, 4);
            switch (kind) {
            case 0: path_point(rf(0, (float)g_path.n), rf(-80, 80), &pt.x, &pt.z); break;       // near the line
            case 1: pt.x = rf(-12000, 12000); pt.z = rf(-12000, 12000); break;                // far
            case 2: pt = seg_of(l, ri(0, loop_len(l) - 1))->p; break;                         // on a node (a teleport)
            case 3: pt.x = rf(-1e7f, 1e7f); pt.z = rf(-1e7f, 1e7f); break;                    // very far
            default: pt = seg_of(l, ri(0, loop_len(l) - 1))->p; pt.x += rf(-0.01f, 0.01f); pt.z += rf(-0.01f, 0.01f); break;
            }
            l->car_pos = pt;
            set_bead(l, chance(50) ? 0 : seg_of(l, ri(0, loop_len(l) - 1)), rf(0, 1));
            save(g_start);
            fpu_reset();
            bool fo = !ran([&] { orig(l, 0); });
            ILSeg* os = l->bead_seg;
            uint32_t ot = ubits(l->bead_t);
            load(g_start);
            fpu_reset();
            bool fn = !ran([&] { IdealLine_reset_bead_position(l, 0); });
            fpu_reset();
            FIXCHECK(!fo && !fn, "reset_bead_position faulted (original %d, fixed %d)", fo, fn);
            if (os) {
                same++;
                FIXCHECK(l->bead_seg == os && ubits(l->bead_t) == ot, "reset_bead_position: the original found the bead, the fixed one differs");
            } else {
                lost++;
                kinds[kind]++;
                check_found_bead(l, &pt, "reset_bead_position (the original left it NULL)");
            }
        }
    }
    printf("reset_bead_position: %ld beads as the original's, %ld the original lost (near %ld, far %ld, on a node %ld, very far %ld, "
           "by a node %ld) put at the nearest point\n", same, lost, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4]);
    FIXCHECK(lost > 100, "only %ld points made the original lose the bead", lost);
}

// a NaN position: the bead stays where it was (on this line), or goes to the head
static void test_nan_position() {
    auto orig = (decltype(&IdealLine_reset_bead_position))(uintptr_t)addr_IdealLine_reset_bead_position;
    auto orig_adv = (decltype(&IdealLine_advance_bead))(uintptr_t)addr_IdealLine_advance_bead;
    long lost = 0, n = 0;
    for (int w = 0; w < 40; w++) {
        busy("NaN positions");
        fresh_world();
        build_resource(ri(3, MAXSEG));
        load_lines();
        IdealLine* l = chance(50) ? (IdealLine*)cl(0) : (IdealLine*)cil();
        if (!l->head) continue;
        for (int k = 0; k < 40; k++) {
            n++;
            l->car_pos.x = chance(50) ? qnan() : rf(-100, 100);
            l->car_pos.z = chance(50) || !ffin(l->car_pos.x) ? fbits(chance(50) ? 0x7fc00000u : 0xff800000u) : qnan();
            int mode = ri(0, 3);
            ILSeg* prev = mode == 0 ? (ILSeg*)0 : mode == 3 ? cl(1)->head : seg_of(l, ri(0, loop_len(l) - 1));   // (3: another line's node)
            float pt = mode == 2 ? qnan() : rf(0, 1);
            set_bead(l, prev, pt);
            save(g_start);
            fpu_reset();
            ran([&] { orig(l, 0); });
            ILSeg* os = l->bead_seg;
            uint32_t ot = ubits(l->bead_t);
            load(g_start);
            fpu_reset();
            bool fn = !ran([&] { IdealLine_reset_bead_position(l, 0); });
            fpu_reset();
            FIXCHECK(!fn, "reset_bead_position faulted on a NaN position");
            if (os) FIXCHECK(l->bead_seg == os && ubits(l->bead_t) == ot, "NaN position: the original found a bead, the fixed one differs");
            else {
                lost++;
                if (mode == 1) FIXCHECK(l->bead_seg == prev && l->bead_t == pt, "NaN position: the bead didn't stay where it was");
                else if (mode == 2) FIXCHECK(l->bead_seg == prev && ubits(l->bead_t) == 0, "NaN position: the bead didn't stay (t 0)");
                else FIXCHECK(l->bead_seg == l->head && ubits(l->bead_t) == 0, "NaN position: the bead isn't at the head");
            }
            // advance_bead on a NULL bead with a NaN position: the original faults, the fixed one goes to the head
            set_bead(l, 0, 0.5f);
            save(g_start);
            bool fo = !ran([&] { orig_adv(l, 0); });
            load(g_start);
            fn = !ran([&] { IdealLine_advance_bead(l, 0); });
            fpu_reset();
            FIXCHECK(fo, "the original advance_bead didn't fault on a NULL bead");
            FIXCHECK(!fn && l->bead_seg == l->head && ubits(l->bead_t) == 0, "advance_bead, NULL bead, NaN position: not at the head");
        }
    }
    printf("NaN positions: %ld resets (%ld beads the original left NULL): kept where they were, or at the head\n", n, lost);
}

// every reader of a NULL bead: the original faults (or, the cookie, makes one from a garbage index), the fixed one
// puts the bead at the nearest point of the line and runs
static void test_null_readers() {
    long faults[8] = {0}, runs = 0;
    for (int w = 0; w < 60; w++) {
        busy("NULL bead readers");
        fresh_world();
        build_resource(ri(3, MAXSEG));
        load_lines();
        if (!cl(0)->head || !cl(1)->head || !cil()->head) continue;
        for (int k = 0; k < 20; k++) {
            runs++;
            Point2D pt;
            path_point(rf(0, (float)g_path.n), rf(-40, 40), &pt.x, &pt.z);
            if (chance(20)) { pt.x = rf(-12000, 12000); pt.z = rf(-12000, 12000); }
            CenterLine* c = cl(0);
            CenterLine* o = cl(1);
            int which = ri(0, 6);
            *g_tick += 3;
            c->car_pos = pt;
            o->car_pos = pt;
            o->car_pos.x += rf(-30, 30);
            set_bead(c, 0, 0);
            if (which == 5) set_bead(o, 0, 0);
            if (which == 6) { set_bead(c, c->head, 0.25f); set_bead(o, 0, 0); }
            IdealLine* checked = which == 6 ? (IdealLine*)o : (IdealLine*)c;
            Point2D* cp = &checked->car_pos;
            Point2D at = *cp;
            c->reset_pending = (uint8_t)chance(30);
            IdealLine* a = cil();
            a->car_pos = pt;
            set_bead(a, 0, 0);
            Point2D* pos = slot(0);
            *pos = pt;
            uint64_t ro = 0, rn = 0;
            auto both = [&](auto of, auto nf) {
                save(g_start);
                fpu_reset();
                bool fo = !ran([&] { ro = of(); });
                load(g_start);
                fpu_reset();
                bool fn = !ran([&] { rn = nf(); });
                fpu_reset();
                if (fo) faults[which]++;
                FIXCHECK(!fn, "reader %d faulted on a NULL bead", which);
                return fo;
            };
            switch (which) {
            case 0: {
                bool fo = both([&] { ((decltype(&IdealLine_advance_bead))(uintptr_t)addr_IdealLine_advance_bead)(a, 0); return 0ull; },
                               [&] { IdealLine_advance_bead(a, 0); return 0ull; });
                FIXCHECK(fo, "the original advance_bead didn't fault");
                FIXCHECK(a->bead_seg && on_loop(a, a->bead_seg) && ffin(a->bead_t), "advance_bead: no bead after");
                break;
            }
            case 1: {
                bool fo = both([&] { return (uint64_t)((decltype(&CenterLine_update))(uintptr_t)addr_CenterLine_update)(c, 0, pos, 0); },
                               [&] { return (uint64_t)CenterLine_update(c, 0, pos, 0); });
                FIXCHECK(fo, "the original CenterLine::update didn't fault");
                FIXCHECK(c->bead_seg && on_loop(c, c->bead_seg) && ffin(c->pos.x) && ffin(c->tan.x) && ffin(c->lat),
                         "CenterLine::update: no bead, or NaN, after");
                break;
            }
            case 2: {
                bool fo = both([&] { double d = ((decltype(&CenterLine_get_car_dlong_meters))(uintptr_t)addr_CenterLine_get_car_dlong_meters)(c, 0); uint64_t u; memcpy(&u, &d, 8); return u; },
                               [&] { double d = CenterLine_get_car_dlong_meters(c, 0); uint64_t u; memcpy(&u, &d, 8); return u; });
                FIXCHECK(fo, "the original get_car_dlong_meters didn't fault");
                check_found_bead(c, &at, "get_car_dlong_meters");
                double d;
                memcpy(&d, &rn, 8);
                FIXCHECK(d == d && d >= 0 && d <= c->total + 1, "get_car_dlong_meters: %g", d);
                break;
            }
            case 3: {
                both([&] { return (uint64_t)((decltype(&CenterLine_get_car_dlong_cookie))(uintptr_t)addr_CenterLine_get_car_dlong_cookie)(c, 0); },
                     [&] { return (uint64_t)CenterLine_get_car_dlong_cookie(c, 0); });
                check_found_bead(c, &at, "get_car_dlong_cookie");
                FIXCHECK((int32_t)(rn >> 16 & 0xffff) < c->res->count, "get_car_dlong_cookie: node %d of %d", (int)(rn >> 16), c->res->count);
                break;
            }
            case 4: {
                bool fo = both([&] { ((decltype(&CenterLine_update_2d_data))(uintptr_t)addr_CenterLine_update_2d_data)(c, 0); return 0ull; },
                               [&] { CenterLine_update_2d_data(c, 0); return 0ull; });
                FIXCHECK(fo, "the original update_2d_data didn't fault");
                check_found_bead(c, &at, "update_2d_data");
                FIXCHECK(ffin(c->pos.x) && ffin(c->pos.z) && ffin(c->tan.x) && ffin(c->normal.z), "update_2d_data: NaN");
                break;
            }
            default: {
                bool fo = both([&] { double d = ((decltype(&CenterLine_time_between))(uintptr_t)addr_CenterLine_time_between)(c, 0, o); uint64_t u; memcpy(&u, &d, 8); return u; },
                               [&] { double d = CenterLine_time_between(c, 0, o); uint64_t u; memcpy(&u, &d, 8); return u; });
                FIXCHECK(fo, "the original time_between didn't fault");
                check_found_bead(checked, &at, "time_between");
                if (which == 5) check_found_bead(o, &o->car_pos, "time_between (the other car)");
                break;
            }
            }
        }
    }
    printf("NULL-bead readers: %ld calls; the original faulted in advance_bead %ld, CenterLine::update %ld, get_car_dlong_meters %ld, "
           "update_2d_data %ld, time_between %ld (the cookie: garbage, no fault); the fixed ones never\n",
           runs, faults[0], faults[1], faults[2], faults[4], faults[5] + faults[6]);
}

// QuickTan on a two-node line (zero tangents): the original NaN, the fixed one the chord's direction
static void test_two_node_tan() {
    auto orig = (decltype(&ILSeg_QuickTan))(uintptr_t)addr_ILSeg_QuickTan;
    long nan_orig = 0, same = 0;
    for (int w = 0; w < 400; w++) {
        busy("QuickTan");
        fresh_world();
        double x[2] = {rf(-2000, 2000), rf(-2000, 2000)}, z[2] = {rf(-2000, 2000), rf(-2000, 2000)};
        double vx[2] = {0, 0}, vz[2] = {0, 0};
        int k = ri(0, 2);
        if (k == 1) { vx[0] = rf(-50, 50); vz[0] = rf(-50, 50); vx[1] = -vx[0]; vz[1] = -vz[0]; }   // zero at t = 0.5
        if (k == 2) { vx[0] = rf(-50, 50); vz[0] = rf(-50, 50); vx[1] = rf(-50, 50); vz[1] = rf(-50, 50); }   // ordinary
        float len[2];
        len[0] = len[1] = (float)sqrt((x[1] - x[0]) * (x[1] - x[0]) + (z[1] - z[0]) * (z[1] - z[0]));
        build_custom(2, x, z, vx, vz, len);
        load_lines();
        IdealLine* l = cil();
        for (int i = 0; i < 2; i++) {
            ILSeg* s = seg_of(l, i);
            float t = k == 1 ? 0.5f : rf(0, 1);
            Point2D a, b;
            orig(s, 0, &a, t);
            ILSeg_QuickTan(s, 0, &b, t);
            fpu_reset();
            if (k == 2 || (ffin(a.x) && ffin(a.z) && !(a.x == 0 && a.z == 0))) {
                // an ordinary tangent: the same bits
                if (ffin(a.x) && ffin(a.z)) { same++; FIXCHECK(!memcmp(&a, &b, 8), "QuickTan: an ordinary tangent differs"); }
                continue;
            }
            nan_orig++;
            double cx = s->next->p.x - (double)s->p.x, cz = s->next->p.z - (double)s->p.z, cl2 = sqrt(cx * cx + cz * cz);
            FIXCHECK(ffin(b.x) && ffin(b.z) && fabs(b.x - cx / cl2) < 1e-5 && fabs(b.z - cz / cl2) < 1e-5,
                     "QuickTan: (%g, %g), the chord's direction is (%g, %g)", b.x, b.z, cx / cl2, cz / cl2);
        }
    }
    printf("QuickTan: %ld zero tangents (the original NaN) give the chord's direction; %ld ordinary ones the same bits\n", nan_orig, same);
    FIXCHECK(nan_orig > 100, "QuickTan: only %ld degenerate tangents", nan_orig);
}

// the original's loops on a zero-length node never end: shown in a thread given two seconds
static HANDLE g_hang_thread;
static volatile LONG g_hang_done;
static DWORD WINAPI hang_worker(void* p) {
    (*(void (*)())p)();
    InterlockedExchange(&g_hang_done, 1);
    return 0;
}
static void (*g_hang_fn)();
static bool original_hangs(void (*fn)()) {
    g_hang_done = 0;
    HANDLE h = CreateThread(0, 0, hang_worker, (void*)fn, 0, 0);
    DWORD r = WaitForSingleObject(h, 2000);
    if (r == WAIT_TIMEOUT) {
        TerminateThread(h, 0);
        CloseHandle(h);
        return true;
    }
    CloseHandle(h);
    return false;
}
static void hang_adv() {
    unsigned x; _controlfp_s(&x, _PC_24, _MCW_PC);
    ((decltype(&IdealLine_advance_bead))(uintptr_t)addr_IdealLine_advance_bead)(cil(), 0);
}
static void hang_rabbit() {
    unsigned x; _controlfp_s(&x, _PC_24, _MCW_PC);
    ((decltype(&IdealLine_get_rabbit_position))(uintptr_t)addr_IdealLine_get_rabbit_position)(cil(), 0, slot(1), slot(2), 25.0f, pslot(3));
}
static void hang_nearest() {
    unsigned x; _controlfp_s(&x, _PC_24, _MCW_PC);
    ((decltype(&IdealLine_get_nearest_bead))(uintptr_t)addr_IdealLine_get_nearest_bead)(cil(), 0, slot(4), pslot(5), qnan());
}
static void setup_zero_node(int* dup_out) {
    fresh_world();
    int dup = 4;
    build_ellipse(10, dup, false);
    load_lines();
    *dup_out = dup;
    IdealLine* l = cil();
    ILSeg* z = seg_of(l, dup);                                    // the zero-length node
    set_bead(l, z, 0.0f);
    Point2D a1 = seg_of(l, dup + 1)->p, a2 = seg_of(l, dup + 2)->p;   // the car past it, within 150 m
    Point2D ahead = {(a1.x + a2.x) * 0.5f, (a1.z + a2.z) * 0.5f};
    l->car_pos = ahead;
    *slot(4) = ahead;
    pslot(5)->seg = z;
    pslot(5)->t = 0.0f;
}
static void test_zero_node_hangs_vs_original() {
    int dup;
    setup_zero_node(&dup);
    save(g_start);
    bool h1 = original_hangs(hang_adv);
    load(g_start);
    bool h2 = original_hangs(hang_rabbit);
    load(g_start);
    bool h3 = original_hangs(hang_nearest);
    load(g_start);
    printf("a zero-length node: the original advance_bead %s, get_rabbit_position %s, get_nearest_bead (max_dist NaN) %s\n",
           h1 ? "hangs" : "returns", h2 ? "hangs" : "returns", h3 ? "hangs" : "returns");
    FIXCHECK(h1 && h2 && h3, "the originals didn't all hang on a zero-length node");
    // the fixed ones, the same inputs
    fpu_reset();
    busy("zero-length node (fixed)");
    IdealLine* l = cil();
    bool ok = ran([&] { IdealLine_advance_bead(l, 0); });
    FIXCHECK(ok && l->bead_seg && node_of(cil(), l->bead_seg) != dup && ffin(l->bead_t), "advance_bead: stuck at the zero-length node");
    load(g_start);
    fpu_reset();
    ok = ran([&] { IdealLine_get_rabbit_position(l, 0, slot(1), slot(2), 25.0f, pslot(3)); });
    FIXCHECK(ok && pslot(3)->seg == seg_of(l, dup + 1) && pslot(3)->t == 0.0f && ffin(slot(1)->x) && ffin(slot(2)->x),
             "get_rabbit_position: not at the next node");
    load(g_start);
    fpu_reset();
    ok = ran([&] { IdealLine_get_nearest_bead(l, 0, slot(4), pslot(5), qnan()); });
    FIXCHECK(ok && pslot(5)->seg && ffin(pslot(5)->t), "get_nearest_bead: gave up");
    fpu_reset();
}

// ---- with every rewrite hooked in the image, as the game runs them ------------------------------------------------
static void hook_all() {
    for (int i = 0; i < g_nregs; i++) patch_jmp(g_regs[i].at, g_regs[i].fn);
}

// drive a car round the line for `laps`: CenterLine 0's update, the AI line's update_car_info and rabbit, the
// deity's queries; NaN positions every nan_every ticks, far teleports (with the AI's reset) every tele_every.
// Nothing may fault or go NaN, no bead NULL; with no NaNs or teleports the beads must get round every lap and
// stamp every node.
static int g_seq[400], g_nseq;
static void drive(const char* what, IdealLine* ai, double laps, int nan_every, int tele_every, bool zero_lengths) {
    g_nseq = 0;
    CenterLine* c = cl(0);
    const Path& p = g_path;
    double total = 0;
    for (int i = 0; i < p.n; i++) {
        int j = (i + 1) % p.n;
        total += sqrt((p.x[j] - p.x[i]) * (p.x[j] - p.x[i]) + (p.z[j] - p.z[i]) * (p.z[j] - p.z[i]));
    }
    double s = 0, lat = 0;
    int ticks = (int)(laps * total / 1.5), wraps = 0, last_node = -1;
    bool seen[MAXSEG] = {false};
    for (int k = 0; k < ticks; k++) {
        busy(what);
        *g_tick += 1;
        s += 1.5;                                                 // 1.5 m a tick
        lat = lat * 0.95 + rf(-0.3f, 0.3f);
        Point2D* pos = slot(0);
        Point2D* dir = slot(1);
        *pos = drive_point(s, lat);
        Point2D ahead = drive_point(s + 1, lat);
        dir->x = ahead.x - pos->x;
        dir->z = ahead.z - pos->z;
        bool nan = nan_every && k % nan_every == nan_every - 1;
        bool tele = tele_every && k % tele_every == tele_every - 1;
        if (nan) pos->x = qnan();
        if (tele) { pos->x = rf(-12000, 12000); pos->z = rf(-12000, 12000); }
        double m = 0;
        uint32_t cookie = 0;
        bool ok = ran([&] {
            CenterLine_update(c, 0, pos, 0);
            IdealLine_update_car_info(ai, 0, pos, dir);
            if (tele) {                                           // AICar::teleport_to_track
                Point2D zero = {0, 0};
                IdealLine_update_car_info(ai, 0, pos, &zero);
                IdealLine_reset_bead_position(ai, 0);
            }
            IdealLine_get_rabbit_position(ai, 0, slot(2), slot(3), 25.0f, pslot(4));
            m = CenterLine_get_car_dlong_meters(c, 0);
            cookie = CenterLine_get_car_dlong_cookie(c, 0);
            CenterLine_time_between(c, 0, cl(1));
        });
        fpu_reset();
        FIXCHECK(ok, "%s: faulted at tick %d", what, k);
        if (!ok) return;
        FIXCHECK(c->bead_seg && ai->bead_seg && on_loop(c, c->bead_seg) && on_loop(ai, ai->bead_seg), "%s: a bead is NULL (tick %d)", what, k);
        FIXCHECK(ffin(c->bead_t) && ffin(ai->bead_t) && ffin(slot(2)->x) && ffin(slot(2)->z) && ffin(slot(3)->x) && ffin(slot(3)->z) && m == m,
                 "%s: NaN at tick %d", what, k);
        if (!nan) FIXCHECK(ffin(c->pos.x) && ffin(c->tan.x) && ffin(c->tan.z) && ffin(c->lat), "%s: the centre line's point NaN at tick %d", what, k);
        if (!zero_lengths || !ai->bead_seg) continue;
        int nd = node_of(cil(), ai->bead_seg);
        if (nd >= 0 && nd < MAXSEG) seen[nd] = true;
        if (last_node >= p.n - 2 && nd <= 1 && nd < last_node) wraps++;
        if (nd != last_node && g_nseq < 400) g_seq[g_nseq++] = nd;
        last_node = nd;
    }
    if (zero_lengths) {
        int unseen = 0;
        for (int i = 0; i < p.n; i++)
            if (!seen[i] && p.len[i] > 0) unseen++;
        FIXCHECK(unseen == 0, "%s: the AI's bead never reached %d nodes", what, unseen);
        FIXCHECK(wraps >= (int)laps - 1, "%s: the AI's bead went round %d times in %g laps", what, wraps, laps);
        if (wraps < (int)laps - 1) {
            printf("  %d nodes, lengths:", p.n);
            for (int i = 0; i < p.n; i++) printf(" %.1f", p.len[i]);
            printf("\n  the AI's bead:");
            for (int i = 0; i < g_nseq; i++) printf(" %d", g_seq[i]);
            printf("\n");
        }
        int stale = 0;
        const float now = (float)(*g_tick * 0.016);
        for (int i = 0; i < c->res->count; i++)
            if (c->times[i] > now + 100.0f) stale++;              // (reset stamps now + 1000; a pass, now)
        FIXCHECK(c->started && stale == 0, "%s: the centre line never stamped %d nodes", what, stale);
        if (stale) {
            printf("  %d nodes, stale:", p.n);
            for (int i = 0; i < c->res->count; i++)
                if (c->times[i] > now + 100.0f) printf(" %d (length %.1f)", i, p.len[i]);
            printf("; its bead %d %.3f, time_seg %d\n", node_of(c, c->bead_seg), c->bead_t, node_of(c, c->time_seg));
        }
    }
}

static void test_drives() {
    // two-node lines (zero tangents), a car driven round and round
    for (int w = 0; w < 20; w++) {
        fresh_world();
        double x[2] = {rf(-500, 500), rf(-500, 500)}, z[2] = {rf(-500, 500), rf(-500, 500)}, v[2] = {0, 0};
        float len[2];
        len[0] = len[1] = (float)sqrt((x[1] - x[0]) * (x[1] - x[0]) + (z[1] - z[0]) * (z[1] - z[0]));
        build_custom(2, x, z, v, v, len);
        load_lines();
        // (no progress to check: both segments are the same chord, one each way, so the bead slides to and fro on
        // the first as the car goes out and back -- the original's NaN is what's fixed)
        drive("a two-node line", cil(), 4, 0, 0, false);
        drive("a two-node line, NaN positions and teleports", cil(), 2, 9, 60, false);
    }
    // a zero-length node (the tangent there ordinary, or zero too)
    for (int w = 0; w < 20; w++) {
        fresh_world();
        build_ellipse(ri(12, 40), -1, false);                     // (fine enough that the bead can follow round every corner)
        int n = g_path.n;
        build_ellipse(n, ri(1, n - 1), chance(50));             // (not the head: its bead must rest there to start timing)
        load_lines();
        drive("a zero-length node", cil(), 3, 0, 0, true);
        drive("a zero-length node, NaN positions and teleports", cil(), 2, 7, 45, false);
    }
    // ordinary lines, NaN positions and far teleports
    for (int w = 0; w < 20; w++) {
        fresh_world();
        build_resource(ri(3, MAXSEG));
        load_lines();
        if (!cl(0)->res || !cil()->head) continue;
        drive("NaN positions and teleports", cil(), 2, ri(3, 11), ri(20, 80), false);
    }
    // the line editor's line: nodes added one by one (no lengths: every one zero)
    for (int w = 0; w < 20; w++) {
        fresh_world();
        build_ellipse(ri(3, 30), -1, false);
        load_lines();
        MIL_ctor(mil(), 0);
        for (int k = 0; k < g_path.n; k++) {
            Point2D q = {(float)g_path.x[k], (float)g_path.z[k]};
            MIL_add_next_control(mil(), 0, &q, 10.0f, 1);
        }
        drive("the editor's line (zero lengths)", mil(), 2, 13, 50, false);
    }
    printf("drives: two-node lines, zero-length nodes, NaN positions, teleports and the editor's line -- done\n");
}

static void run_fix_tests() {
    test_reset_vs_original();
    test_nan_position();
    test_null_readers();
    test_two_node_tan();
    test_zero_node_hangs_vs_original();
    hook_all();
    printf("(every rewrite hooked in the image now)\n");
    test_drives();
}
#endif

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 300;
    if (argc > 2) g_state = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_ideal.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    install_stubs();
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!g_arena) return 2;
    uint32_t gb = 0;
    for (const GRange& g : g_globals) gb += g.n;
    if (gb != GLOBALS_BYTES) { printf("GLOBALS_BYTES is %u, should be %u\n", (unsigned)GLOBALS_BYTES, gb); return 2; }
    CreateThread(0, 0, watchdog, 0, 0, 0);
    for (g_world = 0; g_world < worlds; g_world++) {
#ifdef FIX_TESTS
        run_track_world();                                         // (the maths worlds are all NaNs and infinities)
#else
        if (g_world % 4 == 3) run_math_world();
        else run_track_world();
#endif
    }
    int failed = 0;
    long calls = 0;
    printf("%-42s %8s %8s %8s %8s %8s %8s %8s\n", "function", "calls", "changed", "replay", "faults", "differ", "fp-miss", "fixed");
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("%-42s %8ld %8ld %8ld %8ld %8ld %8ld %8ld\n", st.name, st.calls, st.changed, st.replay_only, st.faults, st.fails, st.fp_fails,
               st.fixed);
        if (st.fails || st.fp_fails) failed++;
        calls += st.calls;
    }
    printf("%d worlds, %ld calls; %d of %d functions differ or escape their footprint\n", worlds, calls, failed, g_nstats);
    printf("covered:");
    for (int i = 0; i < C_N; i++) printf("%s %s %ld", i ? "," : "", g_cov_names[i], g_cov[i]);
    printf("\n");
#ifdef FIX_TESTS
    printf("ordinary worlds: %ld lost beads found (%ld checked against the brute-force nearest point)\n", g_fix_found, g_fix_nearest);
    run_fix_tests();
    printf("fix tests: %d failures\n", g_fix_fail);
    if (g_fix_fail) failed++;
#endif
    return failed ? 1 : 0;
}
