// world_task.cpp -- the physics task and its API (hook/phys_task.cpp: phystask.obj, physics.obj, the DriverGet*
// readers) against the originals, on random worlds outside the game (docs/PORTING.md, step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_task.cpp
//        /Fo%TEMP%\wt\ /Fe%TEMP%\wt\world_task.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_task.exe [worlds] [seed]
//   the fixes (docs/PORTING.md, "Fixes"): the same with /DFIX_TESTS (or /DVP_TEST_FIXES) -- the rewrites as the game builds them (the
//   random streams' records fit create_phob's buffer, so they must still match); then create_phob on records bigger
//   than its buffer, or of a negative size; and TimerWatchdog::dump's timer.log in <race.exe's folder>\log\ (its random
//   check is left out of the worlds: the file name differs; GetModuleFileNameA and CreateDirectoryA are stubs).
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. A world is one tracked arena holding:
//   * a phob list of up to 12 physics objects of the real classes (PlayCar, Ball, Obstacle, CheckPoint,
//     PhobStatic, and an Obstacle whose IsDynamic answers from a byte of the object), each with a copy of its
//     class's vtable in which Reset, Update, UpdateCommon, GetMessage and the destructor are logging stubs
//     (IsDynamic / IsSolid stay the originals); random frames (sometimes NaNs, infinities, denormals); up to 4
//     sphere volumes each (a SphereVolume vtable copy whose Collide logs its call and marks both volumes);
//   * the track's static objects and their quadtree (random node ranges, up to 4 levels), with real
//     SphereVolume vtables in their storage; the cars[] table; the event queue; a fake deity whose vtable
//     logs; the shared packets; a fake MemStream (creation records); a bump heap.
// Everything the functions call outside this group is a stub that logs its call and answers from a per-call
// script: the Single* locks, the physics parts' Begin/End, MemAlloc / delete, the static list, the memory
// streams, the shared memory, the profiler's function pointers, the replay recorder, the network, the AI, the
// camera (update_camera), collide_phobs, Car::UpdateDraft, the phob constructors, PTimeNow (a scripted clock),
// TaskSleep (which plays the other thread: after a scripted number of sleeps it moves physics.obj's state on),
// BGHook, ExceptSinglePrecision, DriverUpdate, the telemetry, the options, the file calls, sprintf (the host's
// vsprintf), LogPanic / LogReport. This group's own functions run as the originals from the image when another
// calls them (PhysTaskUpdate -> GetTicks, renormalize_phobs, update_phobs, operator=; create_phob -> make_phob;
// physics_thread -> the watchdog), except that physics_thread's check patches the five PhysTask* entry points
// with logging stubs for its duration. For each call the original runs, the world (arena, globals) and the log
// are kept, the world restored, the rewrite runs, and the world, return value, log and any fault are compared;
// every byte the original changed must lie inside the rewrite's footprint (or a physics static, for the
// physics-thread functions) unless it's replay_only. The world continues from the original's result.
// Physics-thread checks run at 24-bit precision; main-thread ones at 53 or 24.
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
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif
#if defined(FIX_TESTS) && !defined(VP_TEST_FIXES)
#define VP_TEST_FIXES           // (the flag the other harnesses use)
#endif
#ifndef VP_TEST_FIXES
#define VP_FAITHFUL                 // the rewrites exactly as the originals, bugs included (docs/PORTING.md, "Fixes")
#endif
#include "../hook/port.h"

#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                      \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                        \
    static const char* const VP_CAT(name_, NEW) = NAME;                                                   \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

static int g_logf_quiet, g_logf_count;              // the fix test counts the rewrite's log lines instead
void logf(const char* fmt, ...) {
    g_logf_count++;
    if (g_logf_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
uint32_t A(uint32_t v10) { return v10; }
bool have(uint32_t) { return true; }
bool build_is_v10() { return true; }

// ---- classes (state_layout.inc) and the logging vtable copies -----------------------------------------------
struct ClassRow { uint32_t vt, size; const char* name; int first, count; };
static const ClassRow k_classes[] = {
#define VP_CLASSES
#include "../hook/state_layout.inc"
#undef VP_CLASSES
};
enum { NVT = 8 };
struct VtCopy { void* slot[40]; };
static VtCopy g_vt[NVT];                     // 0 PlayCar, 1 Ball, 2 Obstacle, 3 CheckPoint, 4 PhobStatic, 5 flaky Obstacle, 6 volume, 7 deity
static const uint32_t k_vt_real[NVT] = {0x004dc238, 0x004dc3b0, 0x004dc0c8, 0x004dc350, 0x004dc1b0, 0x004dc0c8, 0x004dbd18, 0};
static const uint32_t k_vt_size[NVT] = {3824, 1152, 1200, 132, 108, 1200, 64, 0x100};
static int g_fp_unknown;

void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) {
    uint32_t vt = *(uint32_t*)obj;
    for (int i = 0; i < NVT; i++)
        if (vt == (uint32_t)(uintptr_t)&g_vt[i]) { add(obj, k_vt_size[i], what); return; }
    for (const ClassRow& c : k_classes)
        if (c.vt == vt) { add(obj, c.size, what); return; }
    printf("footprint: %s at %p has an unknown class (vtable %08x)\n", what, obj, vt);
    g_fp_unknown++;
}

#include "../hook/phys_task.cpp"

// ---- random values ------------------------------------------------------------------------------------------
static uint32_t g_rng = 0x2545f491u;
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
static float special() {
    switch (rnd() % 8) {
    case 0: return fbits(0x7fc00000u | (rnd() & 0x3fffff));
    case 1: return fbits(0xffc00000u | (rnd() & 0x3fffff));
    case 2: return fbits(0x7f800001u + (rnd() & 0x3ffffe));                    // signalling NaN
    case 3: return fbits(0x7f800000u | (rnd() & 0x80000000u));
    case 4: return fbits(rnd() & 0x807fffffu);                                 // denormal
    case 5: return (rnd() & 1) ? 0.0f : -0.0f;
    case 6: return fbits(rnd() & 0x80000000u | 0x7f7fffffu);
    default: return fbits(rnd());
    }
}
static int g_special_pct = 3;
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
// a patch that can be taken out again (physics_thread's check stubs the PhysTask* entry points)
struct TempPatch { uint32_t at; uint8_t saved[5]; };
static void temp_patch(TempPatch& t, uint32_t at, void* to) {
    t.at = at;
    memcpy(t.saved, (void*)at, 5);
    patch_jmp(at, to);
}
static void temp_unpatch(const TempPatch& t) { memcpy((void*)t.at, t.saved, 5); }

// ---- the world ------------------------------------------------------------------------------------------------
enum {
    ARENA_SIZE = 0x48000,
    OFF_CTRL = 0x0000, OFF_DEITY = 0x0200, OFF_LOCALE = 0x0300, OFF_OPTS = 0x0340, OFF_DATA = 0x0400,
    OFF_EVT = 0x0600, OFF_EVT2 = 0x0c00, OFF_PHOBLIST = 0x1400, OFF_VOLS = 0x1c00, VOL_STRIDE = 0x40,
    OFF_STATIC = 0x2800, NSTATIC_MAX = 24, MAXNODES = 85,
    OFF_PKT_W = 0x4400, OFF_PKT_R = 0x5400, OFF_PKT_OUT = 0x6400, OFF_STREAM = 0x6800, STREAM_CAP = 0x1ff0,
    OFF_SPTR = 0x8800,
    OFF_OBJS = 0x9000, OBJ_STRIDE = 0x1100, NOBJS = 12,
    OFF_HEAP = 0x16000, HEAP_SIZE = 0x30000,
};
static uint8_t* g_arena;

// the harness's own state that the stubs change: in the arena, so a check restores it
struct Ctrl {
    uint32_t clock, clock_k;
    int32_t clock_step[8];
    int32_t sleeps, sleep_after, sleep_target;
    uint8_t sleep_on, multi, incognito, _p;
    int32_t carmgr_count;
    int32_t prof_next;
    uint32_t heap_top;
    int32_t file_handle;
    int32_t metric;
    int32_t nsptr;
    uint32_t mark;
};
static Ctrl* ctrl() { return (Ctrl*)(g_arena + OFF_CTRL); }
struct FakeStream { int32_t len, cap; uint8_t data[STREAM_CAP]; };
struct FakeSPtr { FakeStream* s; int32_t pos; int32_t _8, _c; };
static FakeStream* stream() { return (FakeStream*)(g_arena + OFF_STREAM); }
static uint8_t* obj_at(int i) { return g_arena + OFF_OBJS + i * OBJ_STRIDE; }
static CollisionVolume* vol_at(int obj, int k) { return (CollisionVolume*)(g_arena + OFF_VOLS + (obj * 4 + k) * VOL_STRIDE); }
static StaticObjectList* slist() { return (StaticObjectList*)(g_arena + OFF_STATIC); }

// the image globals the functions read or write, snapshotted with the arena
struct GRange { uint32_t at, n; const char* what; };
static const GRange g_globals[] = {
    {0x004ec9d0, 0x6c, "phystask statics (0x4ec9d0)"}, {0x004ecf74, 0x1c, "physics statics (0x4ecf74)"},
    {0x00520ba8, 0x4a8, "phystask statics (0x520ba8)"}, {0x00521050, 0x30, "blimp_frame.."},
    {0x00521080, 0x474, "phystask statics (0x521080)"}, {0x005215d8, 0x2ac, "physics statics (0x5215d8)"},
    {0x005218ac, 4, "deity"}, {0x005218e8, 1, "replay play mode"}, {0x005221c8, 0x104, "driver statics"},
    {0x00509354, 4, "locale"}, {0x004e642c, 0x10, "profiler pointers"},
};
enum { GLOBALS_BYTES = 0x6c + 0x1c + 0x4a8 + 0x30 + 0x474 + 0x2ac + 4 + 1 + 0x104 + 4 + 0x10 };
// every stub call logs a hash of them: the originals' writes must land on the same side of each call
static uint32_t globals_hash() {
    uint32_t h = 2166136261u;
    for (const GRange& g : g_globals)
        for (uint32_t i = 0; i < g.n; i += 4) {
            uint32_t w;
            memcpy(&w, (const void*)(uintptr_t)(g.at + i), 4);
            h = (h ^ w) * 16777619u;
        }
    return h;
}
// the physics statics a shadow check on the physics thread saves automatically
struct GlobalRow { uint32_t va, size; const char* name; };
static const GlobalRow g_auto_saved[] = {
#include "../hook/globals_phys.inc"
};
static bool in_auto_saved(uint32_t a) {
    for (const GlobalRow& g : g_auto_saved)
        if (a >= g.va && a < g.va + g.size) return true;
    return false;
}

// ---- the stubs' script and log -------------------------------------------------------------------------------
struct LogEntry { uint32_t kind, a[5], globals; };
enum { LOGN = 16384 };
static LogEntry g_log[2][LOGN];
static int g_nlog[2], g_pass;
static uint32_t g_script[64];
static int g_si;
static uint32_t script() { return g_script[g_si++ & 63]; }
static uint32_t globals_hash();
static void logn(uint32_t kind, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0) {
    if (g_nlog[g_pass] < LOGN) g_log[g_pass][g_nlog[g_pass]] = {kind, {a0, a1, a2, a3, a4}, globals_hash()};
    g_nlog[g_pass]++;
}
static uint32_t P(const void* p) { return (uint32_t)(uintptr_t)p; }
// a mark that depends on the order of calls: written into the objects the stubs touch
static uint32_t mark(uint32_t salt) {
    Ctrl* c = ctrl();
    c->mark = c->mark * 0x9e3779b1u + salt + (uint32_t)g_nlog[g_pass];
    return c->mark;
}

#define LOG_KINDS(X)                                                                                             \
    X(SINGLE_BEGIN) X(SINGLE_ENTER) X(SINGLE_LEAVE) X(SINGLE_END) X(REPLAY_BG_BEGIN) X(REPLAY_BG_END)            \
    X(REPLAY_DONE) X(REPLAY_SEAL) X(COLLIDE_BEGIN) X(COLLIDE_END) X(TIRE_BEGIN) X(TIRE_END) X(AERO_BEGIN)        \
    X(AERO_END) X(CAR_BEGIN) X(CAR_END) X(DEITY_BEGIN) X(DEITY_END) X(TV_CAMERAS) X(RECORD_RESET)                \
    X(AI_RESET) X(AI_BG) X(MULTI_RECV) X(MULTI_SEND) X(MULTI_ENABLED) X(WORLD_OPTS) X(MEMALLOC) X(DELETE)        \
    X(STATIC_GET) X(STATIC_FORGET) X(MS_CREATE) X(MS_CREATE_PTR) X(MS_SEEK_START) X(MS_DESTROY_PTR)              \
    X(MS_DESTROY) X(MS_GET_INT) X(MS_GET_POS) X(MS_SEEK_POS) X(MS_GET_DATA) X(MS_PUT_INT) X(MS_PUT_DATA)         \
    X(SHM_ALLOC) X(SHM_GET) X(SHM_FREE) X(SHM_GET_W) X(SHM_REL_W) X(SHM_GET_R) X(SHM_REL_R) X(PR_BEGIN)          \
    X(PR_END) X(PROF_START) X(PROF_STOP) X(REC_UPDATE) X(PLAY_UPDATE) X(REGISTER_PHOB) X(DRAFT)                 \
    X(COLLIDE_PHOBS) X(UPDATE_CAMERA) X(CARMGR_COUNT) X(SLEEP) X(PTIME) X(DRIVER_BEGIN) X(DRIVER_END)            \
    X(DRIVER_UPDATE) X(DASH_BEGIN) X(DASH_END) X(TELE_CREATE) X(TELE_UPDATE) X(OPTIONS_SET) X(BGHOOK)            \
    X(BGUNHOOK) X(EXCEPT_SP) X(PANIC) X(REPORT) X(INCOGNITO) X(SPRINTF) X(FILE_CREATE) X(FILE_PRINTF)            \
    X(FILE_CLOSE) X(CTOR) X(V_DTOR) X(V_RESET) X(V_UPDATE) X(V_COMMON) X(V_GETMSG) X(V_ISDYN) X(VOL_COLLIDE)      \
    X(D_STOP) X(D_INIT) X(D_UPDATE) X(D_RESET) X(D_POSTRACE) X(D_GETMSG) X(D_BAD) X(T_BEGIN) X(T_UPDATE)         \
    X(T_RESTART) X(T_POSTRACE) X(T_END)
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

// ---- stubs: begin / end / housekeeping ------------------------------------------------------------------------
#define VSTUB(NAME, KIND) static void __cdecl NAME() { logn(KIND); }
VSTUB(st_replay_bg_begin, L_REPLAY_BG_BEGIN) VSTUB(st_replay_bg_end, L_REPLAY_BG_END)
VSTUB(st_replay_done, L_REPLAY_DONE) VSTUB(st_replay_seal, L_REPLAY_SEAL) VSTUB(st_collide_begin, L_COLLIDE_BEGIN)
VSTUB(st_collide_end, L_COLLIDE_END) VSTUB(st_tire_begin, L_TIRE_BEGIN) VSTUB(st_tire_end, L_TIRE_END)
VSTUB(st_aero_begin, L_AERO_BEGIN) VSTUB(st_aero_end, L_AERO_END) VSTUB(st_car_begin, L_CAR_BEGIN)
VSTUB(st_car_end, L_CAR_END) VSTUB(st_deity_end, L_DEITY_END) VSTUB(st_tv_cameras, L_TV_CAMERAS)
VSTUB(st_record_reset, L_RECORD_RESET) VSTUB(st_ai_reset, L_AI_RESET) VSTUB(st_ai_bg, L_AI_BG)
VSTUB(st_multi_recv, L_MULTI_RECV) VSTUB(st_multi_send, L_MULTI_SEND) VSTUB(st_driver_end, L_DRIVER_END)
VSTUB(st_dash_begin, L_DASH_BEGIN) VSTUB(st_dash_end, L_DASH_END) VSTUB(st_pr_begin, L_PR_BEGIN)
VSTUB(st_pr_end, L_PR_END) VSTUB(st_collide_phobs, L_COLLIDE_PHOBS)
#undef VSTUB

static int __cdecl st_single_begin(const char* name) { logn(L_SINGLE_BEGIN, P(name)); return (int)(script() & 0xff) + 1; }
static void __cdecl st_single_enter(int h, const char* f, int l) { logn(L_SINGLE_ENTER, h, P(f), l); }
static void __cdecl st_single_leave(int h, const char* f, int l) { logn(L_SINGLE_LEAVE, h, P(f), l); }
static void __cdecl st_single_end(int h, const char* f, int l) { logn(L_SINGLE_END, h, P(f), l); }
static const void* __cdecl st_world_opts() { logn(L_WORLD_OPTS); return g_arena + OFF_OPTS; }
static void __cdecl st_deity_begin(const void* o) { logn(L_DEITY_BEGIN, P(o)); }
static uint8_t __cdecl st_multi_enabled() { logn(L_MULTI_ENABLED); return ctrl()->multi; }
static int __cdecl st_carmgr_count() { logn(L_CARMGR_COUNT); return ctrl()->carmgr_count; }
static uint8_t __cdecl st_incognito() { logn(L_INCOGNITO); return ctrl()->incognito; }
static void* __cdecl st_memalloc(int n) {
    Ctrl* c = ctrl();
    uint32_t s = script();
    if ((s & 63) == 0) { logn(L_MEMALLOC, (uint32_t)n, 0); return 0; }
    uint8_t* p = g_arena + OFF_HEAP + c->heap_top;
    c->heap_top += ((uint32_t)n + 15) & ~15u;
    if (c->heap_top > HEAP_SIZE) { printf("the fake heap is full\n"); exit(3); }
    memset(p, 0xcd, (size_t)n);
    logn(L_MEMALLOC, (uint32_t)n, P(p));
    return p;
}
static void __cdecl st_delete(void* p) { logn(L_DELETE, P(p)); }
static StaticObjectList* __cdecl st_static_get(const char* name) { logn(L_STATIC_GET, P(name)); return slist(); }
static void __cdecl st_static_forget(StaticObjectList* l) { logn(L_STATIC_FORGET, P(l)); }

// memory streams: one fake stream in the arena, and pointers over it
static void* __cdecl st_ms_create(int n) { logn(L_MS_CREATE, (uint32_t)n); return stream(); }
static void* __cdecl st_ms_create_ptr(void* s) {
    Ctrl* c = ctrl();
    FakeSPtr* p = (FakeSPtr*)(g_arena + OFF_SPTR) + (c->nsptr++ & 3);
    p->s = (FakeStream*)s;
    p->pos = 0x7777;
    logn(L_MS_CREATE_PTR, P(s), P(p));
    return p;
}
static void __cdecl st_ms_seek_start(FakeSPtr* p) { logn(L_MS_SEEK_START, P(p)); p->pos = 0; }
static void __cdecl st_ms_destroy_ptr(FakeSPtr* p) { logn(L_MS_DESTROY_PTR, P(p)); }
static void __cdecl st_ms_destroy(void* s) { logn(L_MS_DESTROY, P(s)); }
static void __cdecl st_ms_get_int(FakeSPtr* p, int32_t* out) {
    FakeStream* s = p->s;
    if (p->pos >= 0 && p->pos + 4 <= s->len) { memcpy(out, s->data + p->pos, 4); p->pos += 4; }
    else *out = -1;
    logn(L_MS_GET_INT, P(p), (uint32_t)*out);
}
static int __cdecl st_ms_get_pos(FakeSPtr* p) { logn(L_MS_GET_POS, P(p), (uint32_t)p->pos); return p->pos; }
static void __cdecl st_ms_seek_pos(FakeSPtr* p, int pos) { logn(L_MS_SEEK_POS, P(p), (uint32_t)pos); p->pos = pos; }
static void __cdecl st_ms_get_data(FakeSPtr* p, void* dst, int n) {
    FakeStream* s = p->s;
    int avail = s->len - p->pos;
    int k = n < avail ? n : avail;
    if (k < 0) k = 0;
    if (k > 0x1e4) k = 0x1e4;                                           // never past create_phob's buffer
    memcpy(dst, s->data + p->pos, (size_t)k);
    p->pos += k;
    uint32_t h = 0;
    for (int i = 0; i < k; i++) h = h * 31 + ((uint8_t*)dst)[i];
    logn(L_MS_GET_DATA, P(p), (uint32_t)n, h);
}
static void __cdecl st_ms_put_int(FakeSPtr* p, int v) {
    FakeStream* s = p->s;
    if (s->len + 4 <= s->cap) { memcpy(s->data + s->len, &v, 4); s->len += 4; }
    logn(L_MS_PUT_INT, P(p), (uint32_t)v);
}
static void __cdecl st_ms_put_data(FakeSPtr* p, const void* d, int n) {
    FakeStream* s = p->s;
    if (n >= 0 && s->len + n <= s->cap) { memcpy(s->data + s->len, d, (size_t)n); s->len += n; }
    logn(L_MS_PUT_DATA, P(p), P(d), (uint32_t)n);
}
// shared memory
static uint32_t __cdecl st_shm_alloc(const char* name, int n) { logn(L_SHM_ALLOC, P(name), (uint32_t)n); return (script() & 0xf) + 1; }
static uint32_t __cdecl st_shm_get(const char* name) { logn(L_SHM_GET, P(name)); return (script() & 0xf) + 0x20; }
static void __cdecl st_shm_free(uint32_t h) { logn(L_SHM_FREE, h); }
static void* __cdecl st_shm_get_w(uint32_t h) { logn(L_SHM_GET_W, h); return g_arena + OFF_PKT_W; }
static void __cdecl st_shm_rel_w(uint32_t h) { logn(L_SHM_REL_W, h); }
static void* __cdecl st_shm_get_r(uint32_t h) { logn(L_SHM_GET_R, h); return g_arena + OFF_PKT_R; }
static void __cdecl st_shm_rel_r(uint32_t h) { logn(L_SHM_REL_R, h); }
// the profiler (through the image's function pointers)
static int __cdecl st_prof_start(const char* name) { logn(L_PROF_START, P(name)); return ctrl()->prof_next++; }
static void __cdecl st_prof_stop(int k) { logn(L_PROF_STOP, (uint32_t)k); }
// the recorder, drafting, the camera
static void __cdecl st_rec_update(PhobRoot** l, int n) { logn(L_REC_UPDATE, P(l), (uint32_t)n); }
static void __cdecl st_play_update(PhobRoot** l, int n) { logn(L_PLAY_UPDATE, P(l), (uint32_t)n); }
static void __cdecl st_register_phob(PhobRoot* p) { logn(L_REGISTER_PHOB, P(p)); }
static void __fastcall st_draft(void* a, Edx, void* b) {
    logn(L_DRAFT, P(a), P(b));
    if (a) *(uint32_t*)((uint8_t*)a + 0x10) = mark(0xd4af7);
}
static int __cdecl st_update_camera(Frame* f) {
    uint32_t s = script();
    logn(L_UPDATE_CAMERA, P(f));
    for (int i = 0; i < 12; i++) ((uint32_t*)f)[i] = s * (i + 1);
    return (int)(script() % 9) - 1;
}
// time and the other thread
static int __cdecl st_ptime() {
    Ctrl* c = ctrl();
    c->clock += (uint32_t)c->clock_step[c->clock_k++ & 7];
    logn(L_PTIME, c->clock);
    return (int)c->clock;
}
static void __cdecl st_sleep(int ms) {
    Ctrl* c = ctrl();
    logn(L_SLEEP, (uint32_t)ms, (uint32_t)c->sleeps);
    c->sleeps++;
    if (c->sleep_on && c->sleeps >= c->sleep_after) { g_state = c->sleep_target; c->sleep_on = 0; }
}
static uint8_t __cdecl st_driver_begin() { logn(L_DRIVER_BEGIN); return 1; }
static void __cdecl st_driver_update(uint32_t dt) { logn(L_DRIVER_UPDATE, dt); }
static int __cdecl st_tele_create(const char* n) { logn(L_TELE_CREATE, P(n)); return ctrl()->metric; }
static void __cdecl st_tele_update(int m, uint32_t v) { logn(L_TELE_UPDATE, (uint32_t)m, v); }
static void __cdecl st_options_set(const char* s, const char* k, int v) { logn(L_OPTIONS_SET, P(s), P(k), (uint32_t)v); }
static void __cdecl st_bghook(uint32_t fn, int pri) { logn(L_BGHOOK, fn, (uint32_t)pri); }
static void __cdecl st_bgunhook(uint32_t fn) { logn(L_BGUNHOOK, fn); }
static void __cdecl st_except_sp(uint8_t on) { logn(L_EXCEPT_SP, on); }
// messages: only the formats known to take an argument have one read
static bool fmt_has_arg(const char* fmt) {
    return fmt == k_fmt_msgsize || fmt == k_fmt_phob_type || fmt == k_fmt_state;
}
static void __cdecl st_panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint32_t a = fmt_has_arg(fmt) ? va_arg(ap, uint32_t) : 0;
    va_end(ap);
    logn(L_PANIC, P(fmt), a);
}
static void __cdecl st_report(const char* fmt, ...) { logn(L_REPORT, P(fmt)); }
static int __cdecl st_sprintf(char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    uint32_t h = 0;
    for (int i = 0; buf[i]; i++) h = h * 31 + (uint8_t)buf[i];
    logn(L_SPRINTF, P(buf), P(fmt), h, (uint32_t)n);
    return n;
}
#ifdef VP_TEST_FIXES
static char g_fc_name[512], g_cd_name[512];         // the last names FileCreate and CreateDirectoryA got
static const char* g_fake_exe = "C:\\Games\\Viper Racing\\race.exe";   // GetModuleFileNameA(NULL) ("": it fails)
static uint32_t __stdcall st_modname(void* m, char* buf, uint32_t n) {
    const uint32_t len = (uint32_t)strlen(g_fake_exe);
    if (m || !len || !n) return 0;
    if (len >= n) { memcpy(buf, g_fake_exe, n - 1); buf[n - 1] = 0; return n; }
    memcpy(buf, g_fake_exe, len + 1);
    return len;
}
static int __stdcall st_createdir(const char* name, void*) {  // (nothing is made)
    snprintf(g_cd_name, sizeof g_cd_name, "%s", name);
    return 1;
}
#endif
static int __cdecl st_file_create(const char* name) {
#ifdef VP_TEST_FIXES
    snprintf(g_fc_name, sizeof g_fc_name, "%s", name);
#endif
    logn(L_FILE_CREATE, P(name));
    return ctrl()->file_handle;
}
static void __cdecl st_file_printf(int h, char* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint32_t a = va_arg(ap, uint32_t), b = va_arg(ap, uint32_t);   // (every call here passes two ints)
    va_end(ap);
    logn(L_FILE_PRINTF, (uint32_t)h, P(fmt), a, b);
}
static void __cdecl st_file_close(int* h) { logn(L_FILE_CLOSE, (uint32_t)*h); }
// the phob constructors make_phob calls: a vtable copy by class, the data's FourCC and the message offset
static const uint32_t k_ctor_addr[9] = {0x004410f0, 0x0042eac0, 0x0042dde0, 0x00440b50, 0x0043f370,
                                        0x0043cd90, 0x0043e710, 0x0043e190, 0x0043d400};
static const int k_ctor_vt[9] = {1, 0, 0, 3, 0, 2, 0, 4, 5};
template <int K> static PhobRoot* __fastcall st_ctor(void* self, Edx, PhobData* d, void* msg) {
    logn(L_CTOR, K, P(self), d->type, P(msg));
    PhobRoot* p = (PhobRoot*)self;
    memset(p, 0, sizeof(PhobRoot));
    p->vtable = g_vt[k_ctor_vt[K]].slot;
    ((uint8_t*)p)[0x6b] = (uint8_t)(script() & 1);
    return p;
}
// the objects' own virtuals
static void* __fastcall st_v_dtor(PhobRoot* self, Edx, unsigned flags) { logn(L_V_DTOR, P(self), flags); return self; }
static void __fastcall st_v_reset(PhobRoot* self, Edx) { logn(L_V_RESET, P(self)); *(uint32_t*)((uint8_t*)self + 0x10) = mark(1); }
static void __fastcall st_v_update(PhobRoot* self, Edx) { logn(L_V_UPDATE, P(self)); *(uint32_t*)((uint8_t*)self + 0x10) = mark(2); }
static void __fastcall st_v_common(PhobRoot* self, Edx) { logn(L_V_COMMON, P(self)); *(uint32_t*)((uint8_t*)self + 0x10) = mark(3); }
static void __fastcall st_v_getmsg(PhobRoot* self, Edx, uint8_t* buf) {
    logn(L_V_GETMSG, P(self), P(buf));
    int i = (int)(((uint8_t*)self - obj_at(0)) / OBJ_STRIDE) & 15;
    if (buf) *(uint32_t*)(buf + i * 4) = mark(4);
}
static uint8_t __fastcall st_v_isdyn(PhobRoot* self, Edx) { logn(L_V_ISDYN, P(self)); return ((uint8_t*)self)[0x6b]; }
static void __fastcall st_vol_collide(CollisionVolume* self, Edx, CollisionVolume* other) {
    logn(L_VOL_COLLIDE, P(self), P(other));
    uint32_t m = mark(5);
    ((uint32_t*)self)[15] = m;                                     // +0x3c
    if (self->owner) *(uint32_t*)((uint8_t*)self->owner + 0x10) = m;
    if (other) ((uint32_t*)other)[15] ^= m;
}
// the fake deity
static void __fastcall st_d_stop(void* self, Edx) { logn(L_D_STOP, P(self)); ((uint32_t*)self)[1] = mark(6); }
static void __fastcall st_d_init(void* self, Edx) { logn(L_D_INIT, P(self)); ((uint32_t*)self)[1] = mark(7); }
static void __fastcall st_d_update(void* self, Edx) { logn(L_D_UPDATE, P(self)); ((uint32_t*)self)[1] = mark(8); }
static void __fastcall st_d_reset(void* self, Edx) { logn(L_D_RESET, P(self)); ((uint32_t*)self)[1] = mark(9); }
static void __fastcall st_d_postrace(void* self, Edx) { logn(L_D_POSTRACE, P(self)); ((uint32_t*)self)[1] = mark(10); }
static void __fastcall st_d_getmsg(void* self, Edx, uint8_t* pkt) { logn(L_D_GETMSG, P(self), P(pkt)); *(uint32_t*)(pkt + 0x44) = mark(11); }
static int __fastcall st_d_bad(void* self, Edx) { logn(L_D_BAD, P(self)); return 0; }
// physics_thread's callees, patched in for its check
static void __cdecl st_t_begin(void* s) { logn(L_T_BEGIN, P(s)); }
static void __cdecl st_t_update() { logn(L_T_UPDATE); }
static void __cdecl st_t_restart() { logn(L_T_RESTART); }
static void __cdecl st_t_postrace() { logn(L_T_POSTRACE); }
static void __cdecl st_t_end() { logn(L_T_END); }

static void install_stubs() {
    struct { uint32_t at; void* to; } p[] = {
        {0x00414f30, (void*)&st_single_begin}, {0x00415000, (void*)&st_single_enter}, {0x00415070, (void*)&st_single_leave},
        {0x00415090, (void*)&st_single_end}, {0x0042d350, (void*)&st_replay_bg_begin}, {0x0042d430, (void*)&st_replay_bg_end},
        {0x0042d3a0, (void*)&st_replay_done}, {0x0042d470, (void*)&st_replay_seal}, {0x0043c600, (void*)&st_collide_begin},
        {0x0043caf0, (void*)&st_collide_end}, {0x0043bd20, (void*)&st_tire_begin}, {0x0043bd30, (void*)&st_tire_end},
        {0x0043b1e0, (void*)&st_aero_begin}, {0x0043b1f0, (void*)&st_aero_end}, {0x0043b090, (void*)&st_car_begin},
        {0x0043b0b0, (void*)&st_car_end}, {0x0042c4f0, (void*)&st_deity_begin}, {0x0042c5f0, (void*)&st_deity_end},
        {0x00428db0, (void*)&st_tv_cameras}, {0x0042a030, (void*)&st_record_reset}, {0x0041d1a0, (void*)&st_ai_reset},
        {0x0041d1c0, (void*)&st_ai_bg}, {0x004a2440, (void*)&st_multi_recv}, {0x004a2620, (void*)&st_multi_send},
        {0x004a23c0, (void*)&st_multi_enabled}, {0x004627a0, (void*)&st_world_opts}, {0x004140e0, (void*)&st_memalloc},
        {0x00414390, (void*)&st_delete}, {0x00435f00, (void*)&st_static_get}, {0x00436040, (void*)&st_static_forget},
        {0x004d8b80, (void*)&st_ms_create}, {0x004d8be0, (void*)&st_ms_create_ptr}, {0x004d8c60, (void*)&st_ms_seek_start},
        {0x004d8c00, (void*)&st_ms_destroy_ptr}, {0x004d8bb0, (void*)&st_ms_destroy}, {0x004d8dc0, (void*)&st_ms_get_int},
        {0x004d8c90, (void*)&st_ms_get_pos}, {0x004d8c80, (void*)&st_ms_seek_pos}, {0x004d8e50, (void*)&st_ms_get_data},
        {0x004d8ce0, (void*)&st_ms_put_int}, {0x004d8d80, (void*)&st_ms_put_data}, {0x004d8960, (void*)&st_shm_alloc},
        {0x004d89b0, (void*)&st_shm_get}, {0x004d8aa0, (void*)&st_shm_free}, {0x004d8a70, (void*)&st_shm_get_w},
        {0x004d8a90, (void*)&st_shm_rel_w}, {0x004d8a30, (void*)&st_shm_get_r}, {0x004d8a80, (void*)&st_shm_rel_r},
        {0x0042d570, (void*)&st_rec_update}, {0x0042cf00, (void*)&st_play_update}, {0x0042d410, (void*)&st_register_phob},
        {0x00438830, (void*)&st_draft}, {0x00427120, (void*)&st_collide_phobs}, {0x00427610, (void*)&st_update_camera},
        {0x00464480, (void*)&st_carmgr_count}, {0x00414de0, (void*)&st_sleep}, {0x00413b40, (void*)&st_ptime},
        {0x00441490, (void*)&st_driver_begin}, {0x00441550, (void*)&st_driver_end}, {0x004417c0, (void*)&st_driver_update},
        {0x00429850, (void*)&st_dash_begin}, {0x00429860, (void*)&st_dash_end}, {0x00471a00, (void*)&st_tele_create},
        {0x00471b00, (void*)&st_tele_update}, {0x00471430, (void*)&st_options_set}, {0x004189f0, (void*)&st_bghook},
        {0x00418ac0, (void*)&st_bgunhook}, {0x00415ca0, (void*)&st_except_sp}, {0x004112b0, (void*)&st_panic},
        {0x00411150, (void*)&st_report}, {0x0040e940, (void*)&st_incognito}, {0x004cf0a0, (void*)&st_sprintf},
        {0x004115f0, (void*)&st_file_create}, {0x00411b40, (void*)&st_file_printf}, {0x00411850, (void*)&st_file_close},
        {k_ctor_addr[0], (void*)&st_ctor<0>}, {k_ctor_addr[1], (void*)&st_ctor<1>}, {k_ctor_addr[2], (void*)&st_ctor<2>},
        {k_ctor_addr[3], (void*)&st_ctor<3>}, {k_ctor_addr[4], (void*)&st_ctor<4>}, {k_ctor_addr[5], (void*)&st_ctor<5>},
        {k_ctor_addr[6], (void*)&st_ctor<6>}, {k_ctor_addr[7], (void*)&st_ctor<7>}, {k_ctor_addr[8], (void*)&st_ctor<8>},
    };
    for (auto& x : p) patch_jmp(x.at, x.to);
#ifdef VP_TEST_FIXES
    // the import slots TimerWatchdog::dump's FIX calls (the loader resolves no imports)
    *(void**)0x005d74e4 = (void*)&st_modname;                   // GetModuleFileNameA
    *(void**)0x005d7490 = (void*)&st_createdir;                 // CreateDirectoryA
#endif
    // the profiler's function pointers
    *(void**)0x004e642c = (void*)&st_pr_begin;
    *(void**)0x004e6430 = (void*)&st_pr_end;
    *(void**)0x004e6434 = (void*)&st_prof_start;
    *(void**)0x004e6438 = (void*)&st_prof_stop;
    // vtable copies: the class's own slots, with the ones that would run the world replaced
    for (int i = 0; i < NVT; i++) {
        for (int k = 0; k < 40; k++) g_vt[i].slot[k] = k_vt_real[i] ? ((void**)(uintptr_t)k_vt_real[i])[k] : (void*)&st_d_bad;
        if (i < 6) {
            g_vt[i].slot[0] = (void*)&st_v_dtor;
            g_vt[i].slot[1] = (void*)&st_v_reset;
            g_vt[i].slot[2] = (void*)&st_v_update;
            g_vt[i].slot[3] = (void*)&st_v_common;
            g_vt[i].slot[5] = (void*)&st_v_getmsg;
        }
    }
    g_vt[5].slot[0x2c / 4] = (void*)&st_v_isdyn;
    g_vt[6].slot[2] = (void*)&st_vol_collide;
    g_vt[7].slot[2] = (void*)&st_d_stop;
    g_vt[7].slot[3] = (void*)&st_d_init;
    g_vt[7].slot[4] = (void*)&st_d_update;
    g_vt[7].slot[5] = (void*)&st_d_reset;
    g_vt[7].slot[6] = (void*)&st_d_postrace;
    g_vt[7].slot[10] = (void*)&st_d_getmsg;
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

struct Stat { const char* name; long calls, changed, fails, fp_fails, faults, replay_only; };
static Stat g_stats[128];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    g_stats[g_nstats] = {name, 0, 0, 0, 0, 0, 0};
    return g_stats[g_nstats++];
}

struct Snapshot { uint8_t arena[ARENA_SIZE]; uint8_t globals[GLOBALS_BYTES]; };
static Snapshot g_start, g_after;
static Footprint g_fp;
static int g_world;
static const char* g_phase = "";
static bool g_main;                      // the function under test runs on the main thread

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
        {OFF_CTRL, "ctrl"}, {OFF_DEITY, "deity"}, {OFF_LOCALE, "locale"}, {OFF_OPTS, "options"}, {OFF_DATA, "phob data"},
        {OFF_EVT, "event stream A"}, {OFF_EVT2, "event stream B"}, {OFF_PHOBLIST, "phob list"}, {OFF_VOLS, "volumes"},
        {OFF_STATIC, "static list"}, {OFF_PKT_W, "packet (writable)"}, {OFF_PKT_R, "packet (readable)"},
        {OFF_PKT_OUT, "packet copy"}, {OFF_STREAM, "stream"}, {OFF_SPTR, "stream ptrs"}, {OFF_OBJS, "objects"},
        {OFF_HEAP, "heap"},
    };
    int k = 0;
    for (int i = 0; i < (int)(sizeof r / sizeof *r); i++)
        if (off >= r[i].at) k = i;
    uint32_t base = r[k].at;
    if (r[k].at == OFF_OBJS && off < OFF_HEAP) base = OFF_OBJS + (off - OFF_OBJS) / OBJ_STRIDE * OBJ_STRIDE;
    *rel = off - base;
    return r[k].name;
}

static int g_first_fail_world = -1;
static uint64_t g_last_ret;                                          // the original pass's, for coverage
static bool g_last_arena_changed;
static int g_kinds[160];
template <typename Run, typename Fp> static void check(const char* name, Run run, Fp footprint) {
    Stat& st = stat(name);
    st.calls++;
    for (int i = 0; i < 64; i++) g_script[i] = rnd();
    unsigned pc = g_main && chance(50) ? _PC_53 : _PC_24;
    save(g_start);
    g_fp.n = 0; g_fp.replay_only = 0; g_fp.pure = false;
    footprint(g_fp);
    if (g_fp.replay_only) st.replay_only++;
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, pc, _MCW_PC);
    g_pass = 0; g_nlog[0] = 0; g_si = 0;
    uint64_t ro = 0, rn = 0;
    int fo = 0, fn = 0;
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] { ro = run(true); })) { fo = 1; }
#else
    __try { ro = run(true); } __except (EXCEPTION_EXECUTE_HANDLER) { fo = 1; }
#endif
    save(g_after);
    if (memcmp(g_after.arena, g_start.arena, ARENA_SIZE) || memcmp(g_after.globals, g_start.globals, GLOBALS_BYTES)) st.changed++;
    g_last_ret = ro;
    g_last_arena_changed = memcmp(g_after.arena + sizeof(Ctrl), g_start.arena + sizeof(Ctrl), ARENA_SIZE - sizeof(Ctrl)) != 0;
    memset(g_kinds, 0, sizeof g_kinds);
    for (int i = 0; i < g_nlog[0] && i < LOGN; i++) g_kinds[g_log[0][i].kind]++;
    load(g_start);
    g_pass = 1; g_nlog[1] = 0; g_si = 0;
    _clearfp();
    _controlfp_s(&cw, pc, _MCW_PC);
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] { rn = run(false); })) { fn = 1; }
#else
    __try { rn = run(false); } __except (EXCEPTION_EXECUTE_HANDLER) { fn = 1; }
#endif
    _controlfp_s(&cw, _PC_53, _MCW_PC);
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
               g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], sizeof(LogEntry) * nlog0);
    if (bad && st.fails++ < 4) {
        if (g_first_fail_world < 0) g_first_fail_world = g_world;
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
            for (uint32_t i = 0; i < g.n; i++)
                if (pa[i] != pb[i]) { printf("  global %s differs at %08x: %02x vs %02x\n", g.what, g.at + i, pa[i], pb[i]); break; }
            pa += g.n; pb += g.n;
        }
        int nl = g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1];
        if (nl > LOGN) nl = LOGN;
        for (int i = 0, k = 0; i < nl && k < 10; i++) {
            LogEntry* a = i < g_nlog[0] ? &g_log[0][i] : 0;
            LogEntry* b = i < g_nlog[1] ? &g_log[1][i] : 0;
            if (a && b && !memcmp(a, b, sizeof *a)) continue;
            k++;
            if (a) printf("  call %d original: %s %08x %08x %08x %08x %08x [globals %08x]\n", i, g_log_names[a->kind], a->a[0], a->a[1], a->a[2], a->a[3], a->a[4], a->globals);
            if (b) printf("  call %d rewrite:  %s %08x %08x %08x %08x %08x [globals %08x]\n", i, g_log_names[b->kind], b->a[0], b->a[1], b->a[2], b->a[3], b->a[4], b->globals);
        }
        if (g_nlog[0] != g_nlog[1]) printf("  %d calls vs %d\n", g_nlog[0], g_nlog[1]);
    }
    // the footprint must cover every byte the original changed (unless the call is left to the replay)
    if (!g_fp.replay_only) {
        for (uint32_t i = 0; i < ARENA_SIZE; i++) {
            if (i >= OFF_CTRL && i < OFF_CTRL + sizeof(Ctrl)) continue;             // the harness's own
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
                if (pa[i] != ps[i] && !in_footprint((void*)(uintptr_t)(g.at + i)) && (g_main || !in_auto_saved(g.at + i))) {
                    if (st.fp_fails++ < 3) printf("FOOTPRINT %s (world %d, %s): global %s (%08x) changed outside it\n", name, g_world, g_phase, g.what, g.at + i);
                    break;
                }
            pa += g.n; ps += g.n;
        }
    }
    load(g_after);                                                    // go on from the original's result
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
#define MAIN(stmt) do { g_main = true; stmt; g_main = false; } while (0)

// ---- building a world ----------------------------------------------------------------------------------------
static void rotation(float* m) {
    double yaw = rf(-3.1416f, 3.1416f), lim = chance(10) ? 3.1416 : 0.4;
    double pitch = rf((float)-lim, (float)lim), roll = rf((float)-lim, (float)lim);
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), spp = sin(pitch), cr = cos(roll), sr = sin(roll);
    double R[9] = {cy * cr + sy * spp * sr, cp * sr, -sy * cr + cy * spp * sr, -cy * sr + sy * spp * cr, cp * cr,
                   sy * sr + cy * spp * cr, sy * cp, -spp, cy * cp};
    double drift = chance(50) ? 0.02 : 0.0005;
    for (int i = 0; i < 9; i++) m[i] = sp((float)(R[i] * (1.0 + drift * (uni() - 0.5)) + drift * (uni() - 0.5)));
    if (chance(3)) for (int i = 0; i < 9; i++) m[i] = sp(rf(-2, 2));
    if (chance(2)) memset(m, 0, 36);
}
static int g_nobjs;
static int g_cls[NOBJS];
static void build_object(int i) {
    int cls = chance(30) ? 0 : ri(1, 5);
    g_cls[i] = cls;
    uint8_t* o = obj_at(i);
    memset(o, 0, OBJ_STRIDE);
    for (uint32_t k = 0x6c; k < k_vt_size[cls] && k < OBJ_STRIDE; k += 4) *(uint32_t*)(o + k) = rnd();
    PhobRoot* p = (PhobRoot*)o;
    p->vtable = g_vt[cls].slot;
    rotation(p->frame.rot.m);
    p->frame.pos.x = sp(chance(10) ? rf(-4.5e6f, 4.5e6f) : rf(-3000, 3000));
    p->frame.pos.y = sp(rf(-50, 300));
    p->frame.pos.z = sp(chance(10) ? rf(-4.5e6f, 4.5e6f) : rf(-3000, 3000));
    o[0x6b] = (uint8_t)chance(60);
    int nv = cls == 3 || cls == 4 ? ri(0, 1) : ri(0, 4);
    p->num_volumes = nv;
    for (int k = 0; k < 4; k++) {
        CollisionVolume* v = vol_at(i, k);
        memset(v, 0, VOL_STRIDE);
        v->vtable = g_vt[6].slot;
        v->owner = p;
        v->frame = &p->frame;
        v->type_tag = chance(3) ? 0x47525550u : 0x53504852u;
        p->volumes[k] = k < nv ? v : 0;
    }
}
static int g_nnodes;
static void build_node(QuadNode* nodes, int node, int depth, int m) {
    QuadNode* nd = &nodes[node];
    int a = ri(0, m), b = ri(0, m);
    if (a > b) { int t = a; a = b; b = t; }
    if (chance(25)) b = a;
    nd->first = (uint16_t)a;
    nd->end = (uint16_t)b;
    if (chance(4)) { nd->first = (uint16_t)b; nd->end = (uint16_t)a; }
    nd->_6 = (uint16_t)rnd();
    nd->child = 0;
    if (depth < 3 && g_nnodes + 4 <= MAXNODES && chance(depth == 0 ? 90 : 55)) {
        int c = g_nnodes;
        g_nnodes += 4;
        nd->child = (uint16_t)c;
        for (int k = 0; k < 4; k++) build_node(nodes, c + k, depth + 1, m);
    }
}
static void build_statics() {
    StaticObjectList* l = slist();
    memset(l, 0, 0x1c00);
    int n = chance(5) ? 0 : ri(1, NSTATIC_MAX);
    l->count = n;
    l->count2 = ri(0, 60);
    l->objects = (StaticObject*)(g_arena + OFF_STATIC + 0x20);
    l->index = (uint16_t*)((uint8_t*)l->objects + NSTATIC_MAX * 0xe0);
    l->nodes = (QuadNode*)((uint8_t*)l->index + 0x100);
    for (int i = 0; i < n; i++) {
        StaticObject* o = &l->objects[i];
        o->id = chance(5) ? -1 : ri(0, 30);
        o->type_tag = 0x53504852u;
        CollisionVolume* v = (CollisionVolume*)o->storage;
        v->vtable = (void**)0x004dbd18;                          // SphereVolume (never called)
        v->owner = chance(10) && g_nobjs ? (PhobRoot*)obj_at(ri(0, g_nobjs - 1)) : 0;
        v->frame = &o->frame;
        v->type_tag = 0x53504852u;
        o->volume = v;
    }
    int m = n ? ri(0, 100) : 0;
    for (int i = 0; i < m; i++) l->index[i] = (uint16_t)ri(0, n - 1);
    g_nnodes = 1;
    build_node(l->nodes, 0, 0, m);
}
static void fill_events(PhysicsEventStream* e, bool sane) {
    e->len = sane ? (uint32_t)ri(0, 0x400) : (uint32_t)ri(0x401, 0x800);
    if (chance(10)) e->len = (uint32_t)ri(0x3f0, 0x400);
    for (int i = 0; i < 0x400; i += 4) *(uint32_t*)(e->data + i) = rnd();
}
static void setup_world() {
    memset(g_arena, 0, ARENA_SIZE);
    Ctrl* c = ctrl();
    c->clock = rnd();
    for (int i = 0; i < 8; i++) c->clock_step[i] = chance(10) ? ri(-100, 200) : ri(0, 40);
    c->carmgr_count = chance(3) ? 0 : ri(1, 8);
    c->multi = (uint8_t)chance(15);
    c->incognito = (uint8_t)chance(20);
    c->file_handle = chance(10) ? 0 : ri(1, 99);
    c->metric = ri(-1, 9);
    // objects, the phob list, cars[]
    g_nobjs = ri(1, NOBJS);
    for (int i = 0; i < g_nobjs; i++) build_object(i);
    PhobRoot** list = (PhobRoot**)(g_arena + OFF_PHOBLIST);
    for (int i = 0; i < g_nobjs; i++) list[i] = (PhobRoot*)obj_at(i);
    if (chance(15))                                                   // a hole: Restart and Update fault on it, as the originals
        list[ri(0, g_nobjs - 1)] = 0;
    g_phobs = list;
    g_nphobs = g_nobjs;
    for (int i = 0; i < 16; i++) g_cars[i] = chance(35) ? obj_at(ri(0, g_nobjs - 1)) : 0;
    build_statics();
    g_static_list = slist();
    // the deity
    *(void***)(g_arena + OFF_DEITY) = g_vt[7].slot;
    g_deity = g_arena + OFF_DEITY;
    // the physics globals
    fill_events(g_events, true);
    g_renorm_index = ri(-2, g_nobjs + 2);
    g_tick = chance(10) ? ri(0, 300) : (int)rnd();
    g_paused = (uint8_t)chance(15);
    g_speed = chance(70) ? 1 : ri(0, 3);
    *(uint8_t*)0x005218e8 = (uint8_t)chance(15);                    // replay play mode
    g_timer->time = sp(rf(0, 600));
    g_timer->ptime = (int32_t)rnd();
    g_timer->half_toggle = (uint8_t)ri(0, 2);
    g_timer->resync = (uint8_t)chance(20);
    g_tick_overflow = (uint8_t)chance(50);
    g_task_single = ri(1, 99);
    g_phys_single = ri(1, 99);
    g_shm_w = ri(1, 9);
    g_shm_r = ri(1, 9);
    g_msg_tail = ri(0, 0x800);
    g_camera_type = ri(0, 9);
    g_state = ri(0, 9);
    g_please_stop = (uint8_t)chance(20);
    g_controls_time = sp(rf(0, 600));
    g_metric = ri(-1, 9);
    g_stream = stream();
    g_stream_ptr = g_arena + OFF_SPTR;
    ((FakeSPtr*)g_stream_ptr)->s = stream();
    stream()->cap = STREAM_CAP;
    g_arena[OFF_LOCALE] = chance(70) ? '.' : chance(50) ? ',' : (uint8_t)rnd();
    g_locale = (const char*)(g_arena + OFF_LOCALE);
    for (int i = 0; i < 0x100; i += 4) *(uint32_t*)(0x005221c8 + i) = ubits(sp(rf(-1.5f, 1.5f)));
    TimerWatchdog* w = g_watchdog;
    w->ticks = chance(50) ? ri(0, 105) : ri(100, 100000);
    w->measured = ri(0, 1000);
    w->last = chance(20) ? (chance(50) ? 0.0f : -0.0f) : chance(5) ? special() : (float)(int32_t)(c->clock - ri(0, 90));
    w->tick_start = (int32_t)(c->clock - ri(0, 50));
    w->slowest = ri(0, 40);
    w->outliers = ri(0, 30);
    w->outlier_sum = ri(0, 3000);
    for (int i = 0; i < 128; i++) w->hist[i] = ri(0, 500);
    // packets
    for (int i = 0; i < 0x1000; i += 4) *(uint32_t*)(g_arena + OFF_PKT_R + i) = rnd();
    fill_events((PhysicsEventStream*)(g_arena + OFF_PKT_R + 0x3d8), true);
}
static void perturb() {
    if (chance(20)) g_paused = (uint8_t)chance(20);
    if (chance(10)) *(uint8_t*)0x005218e8 = (uint8_t)chance(15);
    if (chance(10)) g_speed = chance(70) ? 1 : ri(0, 3);
    if (chance(10)) ctrl()->multi = (uint8_t)chance(20);
    if (chance(10)) g_timer->resync = (uint8_t)chance(30);
    if (chance(5)) g_timer->time = sp(rf(0, 600));
    if (chance(5)) g_timer->ptime = (int32_t)(ctrl()->clock - ri(-2000, 2000));
    if (chance(10)) for (int i = 0; i < 8; i++) ctrl()->clock_step[i] = chance(10) ? ri(-100, 200) : ri(0, 40);
    if (chance(5)) g_tick = chance(50) ? ri(0, 300) : (int)rnd();
    if (chance(25)) {                                                 // real time about in step with the physics
        g_timer->time = (float)g_tick * 0.016f;
        g_timer->ptime = (int32_t)(ctrl()->clock - ri(0, 90));
        g_timer->resync = 0;
    }
    if (chance(20)) {
        int i = ri(0, g_nobjs - 1);
        rotation(((PhobRoot*)obj_at(i))->frame.rot.m);
    }
    if (chance(5)) g_renorm_index = ri(-2, g_nobjs + 2);
    if (chance(10)) fill_events(g_events, chance(95));
}

// ---- the calls -------------------------------------------------------------------------------------------------
enum Cov {
    C_RENORM, C_COLLIDE_HIT, C_COLLIDE_CALLS, C_UPD_LIVE, C_UPD_PLAY, C_TASK_IDLE, C_TASK_TICKS, C_TICKS0, C_TICKS1,
    C_TICKS2, C_TICKS34, C_TICKS_OTHER, C_EVENT_ADDED, C_EVENT_DROPPED, C_TIME_OVER, C_WD_HIST, C_WD_OUTLIER, C_WD_PANIC,
    C_BEGIN_PHOBS, C_START_PANIC, C_MSG_PANIC, C_PHOB_PANIC, C_THREAD_STATES, C_STEER_SCALED, C_FIND_STATIC_HIT, NCOV
};
static int g_cov[NCOV];
static const char* const k_cov_names[NCOV] = {
    "frames renormalised", "collide_object calls that collided", "volume Collide calls", "update_phobs live",
    "update_phobs playback", "PhysTaskUpdate with no tick", "ticks run in PhysTaskUpdate", "GetTicks = 0", "GetTicks = 1",
    "GetTicks = 2", "GetTicks = 3..4", "GetTicks other", "events queued", "events dropped", "time strings >= 1 h",
    "watchdog histogram", "watchdog outliers", "watchdog panics", "phobs made by PhysTaskBegin", "PhysicsStart panics",
    "msg size panics", "unknown phob type panics", "physics_thread states seen (mask)", "steering scaled",
    "static objects found",
};

static void call_physics_thread() {
    TempPatch t[5];
    temp_patch(t[0], 0x00426850, (void*)&st_t_begin);
    temp_patch(t[1], 0x00426b90, (void*)&st_t_update);
    temp_patch(t[2], 0x00426b20, (void*)&st_t_restart);
    temp_patch(t[3], 0x00426a60, (void*)&st_t_postrace);
    temp_patch(t[4], 0x00426a80, (void*)&st_t_end);
    g_state = chance(90) ? ri(1, 8) : ri(-1, 10);
    int st = g_state;
    CHECK0(physics_thread_rw);
    if (st >= 0 && st < 16) g_cov[C_THREAD_STATES] |= 1 << st;
    for (int i = 4; i >= 0; i--) temp_unpatch(t[i]);
}

// a stream of creation records: (message offset, PhobData), ended by -1 (or not)
static const uint32_t k_types[] = {0x42414c4c, 0x41434152, 0x47434152, 0x43484b50, 0x4e434152, 0x4f425354,
                                   0x50434152, 0x53544154, 0x574f424c};
static uint32_t random_type() { return chance(97) ? k_types[rnd() % 9] : (chance(50) ? rnd() : 0x434152); }
static void build_stream(int n) {
    FakeStream* s = stream();
    s->len = 0;
    s->cap = STREAM_CAP;
    for (int i = 0; i < n; i++) {
        int size = ri(8, 0x60);
        if (s->len + 4 + size > s->cap - 8) break;
        int32_t msg = ri(0, 0x1000);
        memcpy(s->data + s->len, &msg, 4);
        s->len += 4;
        PhobData d = {random_type(), size};
        memcpy(s->data + s->len, &d, 8);
        for (int k = 8; k < size; k++) s->data[s->len + k] = (uint8_t)rnd();
        s->len += size;
    }
    if (chance(90)) { int32_t end = -1; memcpy(s->data + s->len, &end, 4); s->len += 4; }
}

static void random_call() {
    int i = ri(0, g_nobjs - 1);
    PhobRoot* ph = (PhobRoot*)obj_at(i);
    switch (rnd() % 48) {
    case 0: case 1: case 2: CHECK0(renormalize_phobs_rw); g_cov[C_RENORM] += g_last_arena_changed; break;
    case 3: case 4:
        CHECK(collide_object_rw, ph);
        g_cov[C_COLLIDE_HIT] += g_kinds[L_VOL_COLLIDE] > 0;
        g_cov[C_COLLIDE_CALLS] += g_kinds[L_VOL_COLLIDE];
        break;
    case 5: {
        g_collide_phob = ph;
        g_collide_x = ri(-4000000, 4000000); g_collide_z = ri(-4000000, 4000000);
        int v = chance(80) ? 40000000 : ri(1, 1000);
        if (chance(50)) CHECK(collide_object_node_rw, chance(80) ? 0 : ri(0, g_nnodes - 1), -v, -v, v, v);
        else {
            int x0 = ri(-4000000, 0) - 1, z0 = ri(-4000000, 0) - 1, x1 = ri(0, 4000000) + 1, z1 = ri(0, 4000000) + 1;
            if (chance(50)) g_collide_x = (x0 + x1) / 2 + ri(-1, 0);      // on the midlines (a negative odd sum
            if (chance(50)) g_collide_z = (z0 + z1) / 2 + ri(-1, 0);      // rounds towards zero)
            CHECK(collide_object_node_rw, 0, x0, z0, x1, z1);
        }
        g_cov[C_COLLIDE_CALLS] += g_kinds[L_VOL_COLLIDE];
        break;
    }
    case 6: case 7:
        CHECK0(update_phobs_rw);
        g_cov[C_UPD_LIVE] += g_kinds[L_REC_UPDATE];
        g_cov[C_UPD_PLAY] += g_kinds[L_PLAY_UPDATE];
        break;
    case 8: case 9: case 10: {
        CHECK0(PhysTaskUpdate_rw);
        int t = g_kinds[L_REC_UPDATE] + g_kinds[L_PLAY_UPDATE];
        g_cov[C_TASK_TICKS] += t;
        g_cov[C_TASK_IDLE] += t == 0;
        break;
    }
    case 11: CHECK0(PhysTaskRestart_rw); break;
    case 12: CHECK0(PhysTaskPostRace_rw); break;
    case 13: CHECK(PhysTaskRegisterCar_rw, chance(80) ? (void*)ph : (void*)0, ri(0, 15)); break;
    case 14: CHECK(PhysTaskFindCar_rw, ri(0, 15)); if (chance(10)) { g_phobs = 0; CHECK(PhysTaskFindCar_rw, ri(0, 15)); g_phobs = (PhobRoot**)(g_arena + OFF_PHOBLIST); } break;
    case 15: CHECK(PhysTaskFindStaticObject_rw, ri(-2, 32)); g_cov[C_FIND_STATIC_HIT] += g_last_ret != 0; break;
    case 16: CHECK(PhysTaskGetMsgSize_rw, chance(20) ? rnd() : random_type()); g_cov[C_MSG_PANIC] += g_kinds[L_PANIC]; break;
    case 17: {
        uint8_t data[0x420];
        for (int k = 0; k < (int)sizeof data; k++) data[k] = (uint8_t)rnd();
        int n = chance(80) ? ri(0, 0x40) : ri(0, 0x1ff);
        if (chance(15) && g_events->len < 0x3fe) n = 0x3ff - (int)g_events->len - ri(0, 1);
        if (n > 0x1ff) { g_events->len = (uint32_t)(0x3ff - 0x1ff + ri(-1, 0)); n = 0x1ff; }
        memcpy(g_arena + OFF_DATA, data, 0x200);
        uint32_t before = g_events->len;
        CHECK(PhysTaskEvent_rw, (uint8_t)rnd(), (const void*)(g_arena + OFF_DATA), n);
        if (g_events->len != before) g_cov[C_EVENT_ADDED]++; else g_cov[C_EVENT_DROPPED]++;
        break;
    }
    case 18: CHECK0(PhysTaskGetTelemetry_rw); break;
    case 19: {
        CHECK(TimerConditioner_GetTicks_rw, g_timer, 0);
        int32_t r = (int32_t)g_last_ret;
        g_cov[r == 0 ? C_TICKS0 : r == 1 ? C_TICKS1 : r == 2 ? C_TICKS2 : r == 3 || r == 4 ? C_TICKS34 : C_TICKS_OTHER]++;
        break;
    }
    case 20: CHECK(TimerConditioner_PhysicsDeviation_rw, g_timer, 0); CHECK0(PhysicsGetDeviation_rw); MAIN(CHECK0(PhysicsGetDeviation_rw); CHECK(TimerConditioner_PhysicsDeviation_rw, g_timer, 0)); break;
    case 21: if (chance(10)) g_timer->time = special();
             CHECK(ConvertPTimeToPhysicsTime_rw, chance(50) ? (int)rnd() : g_timer->ptime + ri(-100000, 100000));
             CHECK(ConvertPhysicsTimeToPTime_rw, sp(g_timer->time + rf(-100, 100))); break;
    case 22: call_physics_thread(); break;
    case 23: case 43: {
        int32_t o = g_watchdog->outliers, m = g_watchdog->measured;
        CHECK(TimerWatchdog_tick_begin_rw, g_watchdog, 0);
        g_cov[C_WD_PANIC] += g_kinds[L_PANIC];
        if (g_watchdog->outliers != o) g_cov[C_WD_OUTLIER]++;
        else if (g_watchdog->measured != m && !g_kinds[L_PANIC]) g_cov[C_WD_HIST]++;
        CHECK(TimerWatchdog_tick_end_rw, g_watchdog, 0);
        break;
    }
    case 24: MAIN(CHECK(TimerWatchdog_init_rw, g_watchdog, 0)); break;
#ifndef VP_TEST_FIXES
    case 25: MAIN(CHECK(TimerWatchdog_dump_rw, g_watchdog, 0)); break;
#else
    case 25: break;                                    // (the fixed one's file name differs: fix_timer_log)
#endif
    // physics.obj, the main thread's
    case 26: MAIN(CHECK0(PhysicsGetTime_rw); CHECK0(PhysicsGetTemperature_rw); CHECK(PhysicsSetTemperature_rw, ubits(sp(rf(200, 350)))));
             break;
    case 27: MAIN(CHECK0(PhysicsPause_rw); CHECK0(PhysicsIsPaused_rw); CHECK0(PhysicsUnpause_rw)); break;
    case 28: MAIN(CHECK(PhysicsSetSpeed_rw, ri(0, 3)); CHECK0(PhysicsGetSpeed_rw); CHECK0(PhysicsGetRealism_rw); CHECK0(PhysicsIsDamageOn_rw));
             break;
    case 29: MAIN(CHECK0(PhysicsAbortRace_rw)); break;
    case 30: if (chance(25)) for (int k = 0; k < 16; k++) g_cars[k] = chance(90) ? obj_at(ri(0, g_nobjs - 1)) : g_cars[k];
             MAIN(CHECK(CameraSetType_rw, ri(0, 9)); CHECK(CameraSetFocusCar_rw, ri(-1, 16)); CHECK0(CameraGetMaxFocusCar_rw)); break;
    case 31: {
        float t = chance(10) ? special() : chance(20) ? rf(3500, 4000) : chance(10) ? rf(-100, 0) : rf(0, 400);
        MAIN(CHECK(PhysicsTimeString_rw, t, (uint8_t)(chance(45) ? 1 : chance(90) ? 0 : rnd())));
        g_cov[C_TIME_OVER] += t >= 3600.0f && t < 2.0e9f;
        break;
    }
    case 32: MAIN(CHECK0(PhysicsReadControls_rw)); break;
    case 33: {
        PhysicsEventStream* out = (PhysicsEventStream*)(g_arena + OFF_EVT);
        MAIN(CHECK(PhysicsGetStatePacket_rw, (void*)(g_arena + OFF_PKT_OUT), out); CHECK0(PhysicsReleaseStatePacket_rw));
        break;
    }
    case 34: {
        PhysicsEventStream* a = (PhysicsEventStream*)(g_arena + OFF_EVT);
        PhysicsEventStream* b = (PhysicsEventStream*)(g_arena + OFF_EVT2);
        fill_events(b, true);
        MAIN(CHECK(PhysicsEventStream_assign_rw, a, 0, (const PhysicsEventStream*)(chance(90) ? b : a)));
        break;
    }
    case 35: {
        PhobData* d = (PhobData*)(g_arena + OFF_DATA);
        d->type = random_type();
        d->size = ri(8, 0x100);
        int* msg = (int*)(g_arena + OFF_EVT);
        MAIN(CHECK(PhysicsCreate_rw, (const PhobData*)d, msg));
        break;
    }
    case 36: {
        Ctrl* c = ctrl();
        c->sleeps = 0;
        c->sleep_on = 1;
        c->sleep_after = chance(10) ? ri(5995, 6010) : ri(0, 6);
        c->sleep_target = chance(90) ? 2 : ri(0, 9);
        if (c->sleep_target == 1) c->sleep_target = 3;
        MAIN(CHECK0(PhysicsStart_rw));
        g_cov[C_START_PANIC] += g_kinds[L_PANIC] > 0;
        break;
    }
    case 37: {
        Ctrl* c = ctrl();
        c->sleeps = 0; c->sleep_on = 1; c->sleep_after = ri(0, 6); c->sleep_target = 8;
        MAIN(CHECK0(PhysicsStop_rw));
        break;
    }
    case 38: {
        Ctrl* c = ctrl();
        c->sleeps = 0; c->sleep_on = 1; c->sleep_after = ri(0, 6); c->sleep_target = 3;
        MAIN(CHECK0(PhysicsRestart_rw));
        break;
    }
    case 39: {
        Ctrl* c = ctrl();
        c->sleeps = 0; c->sleep_on = 1; c->sleep_after = ri(0, 6); c->sleep_target = 6;
        MAIN(CHECK0(PhysicsPostRace_rw));
        break;
    }
    // driver.obj
    case 40: case 41: {
        if (chance(30)) for (int k = 0; k < 0x100; k += 4) *(uint32_t*)(0x005221c8 + k) = chance(10) ? rnd() : ubits(sp(rf(-1.5f, 1.5f)));
        *(uint8_t*)0x005222a4 = (uint8_t)(chance(50) ? 0 : rnd());
        float v = chance(20) ? special() : rf(-5, 80);
        CHECK(DriverGetSteering_rw, v);
        g_cov[C_STEER_SCALED] += (int32_t)ubits(v) > 0x4178e38e;
        CHECK0(DriverGetThrottle_rw); CHECK0(DriverGetBraking_rw); CHECK0(DriverGetClutch_rw); CHECK0(DriverGetEBrake_rw);
        CHECK0(DriverGetGear_rw); CHECK0(DriverGetHorn_rw); CHECK0(DriverGetAirlift_rw); CHECK0(DriverGetReverse_rw);
        CHECK0(DriverGetHelp_rw); CHECK0(DriverGetLookSide_rw); CHECK0(DriverGetPitch_rw); CHECK0(DriverGetLookBack_rw);
        break;
    }
    case 42: {
        *(uint32_t*)0x00522270 = chance(20) ? ubits(special()) : ubits(rf(-0.5f, 1.5f));
        CHECK0(DriverGetClutch_rw);
        break;
    }
    default: break;
    }
}

static void run_task_world() {
    setup_world();
    int steps = ri(20, 80);
    g_phase = "task";
    for (int s = 0; s < steps; s++) {
        perturb();
        random_call();
    }
    // coverage, from the world as it ended up (a rough picture)
    g_phase = "end";
    CHECK0(PhysTaskEnd_rw);
}

static void run_begin_world() {
    setup_world();
    g_phase = "begin";
    // the parts on their own
    for (int k = 0; k < 6; k++) {
        build_stream(ri(0, 3));
        FakeSPtr* p = (FakeSPtr*)(g_arena + OFF_SPTR);
        p->s = stream();
        p->pos = 0;
        PhobRoot** out = (PhobRoot**)(g_arena + OFF_EVT);
        CHECK(create_phob_rw, (void*)p, out);
        g_cov[C_PHOB_PANIC] += g_kinds[L_PANIC];
        PhobData* d = (PhobData*)(g_arena + OFF_DATA);
        d->type = random_type();
        d->size = 8;
        CHECK(make_phob_rw, d, ri(0, 0x1000));
        g_cov[C_PHOB_PANIC] += g_kinds[L_PANIC];
    }
    // the task's start from a stream of records
    build_stream(chance(10) ? 0 : ri(1, 16));
    CHECK(PhysTaskBegin_rw, (void*)stream());
    g_cov[C_BEGIN_PHOBS] += g_kinds[L_CTOR];
    for (int k = 0; k < 8; k++) { perturb(); random_call(); }
    CHECK0(PhysTaskEnd_rw);
    // physics.obj's begin and end
    uint8_t* opts = g_arena + OFF_OPTS;
    for (int k = 0; k < 40; k += 4) *(uint32_t*)(opts + k) = rnd();
    MAIN(CHECK(PhysicsBegin_rw, (const void*)opts, (int)rnd()));
    MAIN(CHECK0(PhysicsEnd_rw));
}

#ifdef VP_TEST_FIXES
// create_phob's FIX: a stream of [a record of 0x1e5..0x1000 bytes, or of a negative size (its 8-byte header alone)],
// [an ordinary record], -1. The first must be read as its first 0x1e4 bytes (or its header) and the stream moved to
// the second, which must come out whole; then the end. Checked on the rewrite's calls: the reads' sizes, the stream
// positions, the constructors' FourCCs.
static int fix_create_phob() {
    int bad = 0;
    const int runs = 3000;
    g_logf_quiet = 1;
    g_logf_count = 0;
    for (int it = 0; it < runs; it++) {
        setup_world();
        FakeStream* s = stream();
        s->len = 0;
        s->cap = STREAM_CAP;
        const int32_t big = chance(25) ? -ri(1, 0x7fffffff) : ri(0x1e5, 0x1000);
        const uint32_t t1 = k_types[rnd() % 9], t2 = k_types[rnd() % 9];
        const int32_t size2 = ri(8, 0x60);
        auto put = [&](const void* p, int n) { memcpy(s->data + s->len, p, (size_t)n); s->len += n; };
        int32_t msg = ri(0, 0x1000);
        put(&msg, 4);
        PhobData d1 = {t1, big};
        put(&d1, 8);
        for (int k = 8; k < big; k++) s->data[s->len++] = (uint8_t)rnd();
        const int32_t second = s->len;
        msg = ri(0, 0x1000);
        put(&msg, 4);
        PhobData d2 = {t2, size2};
        put(&d2, 8);
        for (int k = 8; k < size2; k++) s->data[s->len++] = (uint8_t)rnd();
        const int32_t end = s->len;
        msg = -1;
        put(&msg, 4);
        FakeSPtr* p = (FakeSPtr*)(g_arena + OFF_SPTR);
        p->s = s;
        p->pos = 0;
        PhobRoot** out = (PhobRoot**)(g_arena + OFF_EVT);
        for (int i = 0; i < 64; i++) g_script[i] = rnd() | 1;        // (odd: no MemAlloc fails)
        g_pass = 1; g_nlog[1] = 0; g_si = 0;
        uint8_t r[3] = {9, 9, 9};
        int32_t pos[3] = {0, 0, 0};
        bool ran = true;
#if defined(__GNUC__) && !defined(__clang__)
        if (vp_try([&] {
                volatile uint8_t* vr = r;                // the calls' results so far stay in memory at a fault
                volatile int32_t* vpos = pos;
                for (int k = 0; k < 3; k++) { vr[k] = create_phob_rw((void*)p, out); vpos[k] = p->pos; }
            })) { ran = false; }
#else
        __try {
            for (int k = 0; k < 3; k++) { r[k] = create_phob_rw((void*)p, out); pos[k] = p->pos; }
        } __except (EXCEPTION_EXECUTE_HANDLER) { ran = false; }
#endif
        // the reads and the constructions, in order
        uint32_t reads[8], types[4];
        int nr = 0, nt = 0;
        for (int i = 0; i < g_nlog[1] && i < LOGN; i++) {
            const LogEntry& e = g_log[1][i];
            if (e.kind == L_MS_GET_DATA && nr < 8) reads[nr++] = e.a[1];
            if (e.kind == L_CTOR && nt < 4) types[nt++] = e.a[2];
        }
        const uint32_t first = big > 0 ? 0x1e4u : 8u;
        const bool ok = ran && r[0] == 1 && r[1] == 1 && r[2] == 0 && pos[0] == second && pos[1] == end &&
                        nr == 4 && reads[0] == 8 && reads[1] == first && reads[2] == 8 && reads[3] == (uint32_t)size2 &&
                        nt == 2 && types[0] == t1 && types[1] == t2;
        if (!ok && bad++ < 4)
            printf("  FIX create_phob (run %d, a record of %d bytes): returns %d %d %d, positions %d %d (want %d %d), %d reads, %d phobs\n",
                   it, big, r[0], r[1], r[2], pos[0], pos[1], second, end, nr, nt);
    }
    g_logf_quiet = 0;
    printf("fix build: create_phob on %d records past its buffer (or of a negative size): %d wrong, %d log lines\n", runs, bad,
           g_logf_count);
    return bad || g_logf_count != runs;
}
#endif

#ifdef VP_TEST_FIXES
// TimerWatchdog::dump's FIX: timer.log in <race.exe's folder>\log\, the folder made; with no exe path, or one too long
// for the file table's 0x100-byte names, c:\timer.log as before. Otherwise the same calls as the original's, with the
// same arguments (FileCreate's name apart), on random watchdogs.
static char g_long_exe[300];
static int fix_timer_log() {
    struct Case { const char* exe; const char* dir; const char* file; };
    memcpy(g_long_exe, "C:\\", 3);                     // "C:\ddd...\race.exe": a 248-character folder
    memset(g_long_exe + 3, 'd', 244);
    strcpy(g_long_exe + 247, "\\race.exe");
    const Case cases[] = {
        {"C:\\Games\\Viper Racing\\race.exe", "C:\\Games\\Viper Racing\\log", "C:\\Games\\Viper Racing\\log\\timer.log"},
        {"E:\\vr\\race.exe", "E:\\vr\\log", "E:\\vr\\log\\timer.log"},
        {"", "", "c:\\timer.log"},                                             // GetModuleFileNameA fails
        {g_long_exe, "", "c:\\timer.log"},                                     // too long for log\timer.log in 0x100
    };
    int bad = 0, runs = 0;
    g_logf_quiet = 1;
    for (int it = 0; it < 400; it++, runs++) {
        const Case& k = cases[it % 4];
        setup_world();
        for (int i = 0; i < 64; i++) g_script[i] = rnd();
        g_fake_exe = k.exe;
        g_pass = 0; g_nlog[0] = 0; g_si = 0;
        TimerWatchdog_dump_o(g_watchdog, 0);
        g_cd_name[0] = g_fc_name[0] = 0;
        g_pass = 1; g_nlog[1] = 0; g_si = 0;
        TimerWatchdog_dump_rw(g_watchdog, 0);
        bool same = g_nlog[0] == g_nlog[1];
        for (int i = 0; same && i < g_nlog[0] && i < LOGN; i++) {
            LogEntry a = g_log[0][i], b = g_log[1][i];
            if (a.kind == L_FILE_CREATE && b.kind == L_FILE_CREATE) a.a[0] = b.a[0] = 0;
            same = !memcmp(&a, &b, sizeof a);
        }
        const bool ok = same && !strcmp(g_fc_name, k.file) && !strcmp(g_cd_name, k.dir);
        if (!ok && bad++ < 4)
            printf("  FIX TimerWatchdog::dump (exe \"%.40s\"): calls %s, FileCreate(\"%s\"), CreateDirectory(\"%s\")\n", k.exe,
                   same ? "same" : "DIFFER", g_fc_name, g_cd_name);
    }
    g_logf_quiet = 0;
    g_fake_exe = "C:\\Games\\Viper Racing\\race.exe";
    printf("fix build: TimerWatchdog::dump's timer.log in <race.exe's folder>\\log\\: %d runs, %d wrong\n", runs, bad);
    return bad;
}
#endif

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 300;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_task.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    install_stubs();
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!g_arena) return 2;
    uint32_t gb = 0;
    for (const GRange& g : g_globals) gb += g.n;
    if (gb != GLOBALS_BYTES) { printf("GLOBALS_BYTES is %u, should be %u\n", (unsigned)GLOBALS_BYTES, gb); return 2; }
    for (g_world = 0; g_world < worlds; g_world++) {
        if (g_world % 5 == 4) run_begin_world();
        else run_task_world();
    }
    int failed = 0;
    long calls = 0;
    printf("%-40s %8s %8s %8s %8s %8s %8s\n", "function", "calls", "changed", "replay", "faults", "differ", "fp-miss");
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("%-40s %8ld %8ld %8ld %8ld %8ld %8ld\n", st.name, st.calls, st.changed, st.replay_only, st.faults, st.fails, st.fp_fails);
        if (st.fails || st.fp_fails) failed++;
        calls += st.calls;
    }
    printf("%d worlds, %ld calls; %d of %d functions differ or escape their footprint; %d unknown footprint classes\n",
           worlds, calls, failed, g_nstats, g_fp_unknown);
    printf("covered:");
    for (int i = 0; i < NCOV; i++) printf("%s %s %d", i ? "," : "", k_cov_names[i], g_cov[i]);
    printf("\n");
#ifdef VP_TEST_FIXES
    failed += fix_create_phob();
    failed += fix_timer_log() != 0;
#endif
    return failed ? 1 : 0;
}
