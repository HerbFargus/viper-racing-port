// world_race.cpp -- the race-logic rewrites (hook/phys_race.cpp: RaceDeity, DragDeity, SixtyToZeroDeity, the Deity
// base, CarDeityBegin/End) against the originals, on fake races outside the game (docs/PORTING.md, step 3).
//
//   build (x86 tools, e.g. after vcvarsall.bat x86), from the repository root:
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_race.cpp
//        /Fo%TEMP%\wr\ /Fe%TEMP%\wr\world_race.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_race.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does. A world is one race: a deity built by the
// ORIGINAL constructor from a GameOptions, up to 8 fake Cars (3768 bytes, a logging vtable whose Teleport slot
// moves the car) registered through RegisterCar, checkpoints, Reset, then a run of ticks -- physics_tick
// advanced, the cars moved, the game state sometimes forced -- of Update, UpdateCar for each car and, at random,
// every other entry point (TeleportToLine, GetMessage, NetPacket, StartRaceAt, the sorts, the getters...), then
// PostRace and the destructor. Drag and 60-0 races are run the same way, and CarDeityBegin/End for each race type.
// Everything the deity calls outside itself is a stub that logs its call and answers from a per-call script
// (the centre line's cursor and cookies, checkpoint crossings, record times, Random, the terrain height, the
// sounds and records it makes), or the original where it only reads a global the harness sets (PhysicsGetTime,
// GetGameState/SetGameState, MultiEnabled, CarMgrCount, WorldGetPlayerCar, PhysicsIsPaused...). qsort, __CIfmod,
// __ftol and HermiteEval run as the originals in the image. For each call the original runs, the world (a
// tracked arena of every object plus the globals) and the log are kept, the world restored, the rewrite runs,
// and the world, the return value, the log and any fault are compared; every byte the original changed must
// lie inside the rewrite's footprint (or a physics static the shadow check saves) unless it's replay_only.
// The world continues from the original's result.
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

// the rewrites, compiled in: each PORT_FN just names the original's address and the footprint function
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
static void** g_car_vtable;                                        // the fake cars' (below)
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) {
    uint32_t vt = *(uint32_t*)obj;
    if (vt == (uint32_t)(uintptr_t)g_car_vtable || vt == 0x004dbfd0) add(obj, 3768, what);
    else if (vt == 0x004dc588) add(obj, 0x7dc, what);
    else if (vt == 0x004dc460 || vt == 0x004db8c0 || vt == 0x004db930) add(obj, 0x68, what);
    else if (vt == 0x004dc4d0) add(obj, 0x48, what);
    else { printf("footprint: %s at %p has an unknown class (vtable %08x)\n", what, obj, vt); g_fp_unknown++; }
}

#include "../hook/phys_race.cpp"

// ---- random values ------------------------------------------------------------------------------------------
static uint32_t g_state = 0x2545f491u;
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
    case 0: return fbits(0x7fc00000u | (rnd() & 0x3fffff));                    // quiet NaN
    case 1: return fbits(0xffc00000u | (rnd() & 0x3fffff));                    // negative quiet NaN
    case 2: return fbits(0x7f800001u + (rnd() & 0x3ffffe));                    // signalling NaN
    case 3: return fbits(0x7f800000u | (rnd() & 0x80000000u));                 // +-inf
    case 4: return fbits(rnd() & 0x807fffffu);                                 // denormal
    case 5: return (rnd() & 1) ? 0.0f : -0.0f;
    case 6: return fbits(rnd() & 0x80000000u | 0x7f7fffffu);                    // +-FLT_MAX
    default: return fbits(rnd());                                               // any bits
    }
}
static int g_special_pct = 2;
// a special from a script word (the stubs must answer the same in both passes)
static float special_s(uint32_t s) {
    switch ((s >> 24) % 8) {
    case 0: return fbits(0x7fc00000u | (s & 0x3fffff));
    case 1: return fbits(0xffc00000u | (s & 0x3fffff));
    case 2: return fbits(0x7f800001u + (s & 0x3ffffe));
    case 3: return fbits(0x7f800000u | (s & 0x80000000u));
    case 4: return fbits(s & 0x807fffffu);
    case 5: return (s & 1) ? 0.0f : -0.0f;
    case 6: return fbits(s & 0x80000000u | 0x7f7fffffu);
    default: return fbits(s);
    }
}
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
    ARENA_SIZE = 0x38000,
    OFF_RACE = 0x0000, OFF_DRAG = 0x0800, OFF_SIXTY = 0x0900,
    OFF_CARS = 0x1000, CAR_STRIDE = 0x1000,                       // 16 cars
    OFF_HEAP = 0x11000, HEAP_SIZE = 0x20000,                       // MemAlloc's
    OFF_PACKET = 0x31000,
    OFF_CPS = 0x31400, CP_STRIDE = 0x90,                            // 4 checkpoints
    OFF_SEGS = 0x31800, SEG_STRIDE = 0x60, NSEGS = 6,               // a ring of ILSegs
    OFF_ILINE = 0x31c00,
    OFF_CARMGR = 0x32000,                                          // 16 x 0x168
    OFF_TRACKINFO = 0x33800, OFF_SKILL = 0x33900, OFF_OPTS = 0x33c00, OFF_WOPTS = 0x33c40,
    OFF_RECORDS = 0x34000, REC_CAP = 64,                            // 3 lists x 64 x 0x48
    NUM_CL_FIELDS = 4,
};
static uint8_t* g_arena;
static uint8_t* g_heap_top;
static uint32_t g_ncp = 1;                                          // checkpoints on the fake centre line

// the image globals the functions read or write, snapshotted with the arena
struct GRange { uint32_t at, n; const char* what; };
static const GRange g_globals[] = {
    {0x004e4e3c, 4, "game state"},   {0x00522368, 16, "lapped[]"},      {0x005222e0, 12, "drag start"},
    {0x00522318, 8, "drag heading"}, {0x004edd70, 8, "globa / sort fn"}, {0x005218ac, 4, "deity"},
    {0x0052161c, 4, "physics_tick"}, {0x00521540, 0x40, "record lists"}, {0x004fb454, 4, "multi"},
    {0x00521608, 1, "paused"},        {0x005218e8, 1, "replay mode"},    {0x004f48d0, 4, "CarMgrCount"},
    {0x004f4354, 4, "player car"},    {0x004e53f8, 1, "incognito"},
};
enum { NGLOBALS = sizeof g_globals / sizeof *g_globals, GLOBALS_BYTES = 0x40 + 16 + 12 + 8 + 8 + 4 + 4 + 4 + 4 + 1 + 1 + 4 + 4 + 1 };
// physics statics the shadow check saves automatically: a change there needn't be in the footprint
static const GRange g_auto_saved[] = {
    {0x00522338, 92, "racedty statics"}, {0x005222e0, 88, "dragdty statics"}, {0x004edd70, 256, "RaceDeity statics"},
    {0x005218ac, 36, "deity"}, {0x00521538, 160, "record statics"}, {0x0052161c, 16, "physics_tick"},
};

static int32_t* const g_tick = (int32_t*)0x0052161c;
static int32_t* const g_multi = (int32_t*)0x004fb454;
static uint8_t* const g_paused = (uint8_t*)0x00521608;
static uint8_t* const g_replay = (uint8_t*)0x005218e8;
static int32_t* const g_carmgr_count = (int32_t*)0x004f48d0;
static int32_t* const g_player_car = (int32_t*)0x004f4354;
static uint8_t* const g_incognito = (uint8_t*)0x004e53f8;

static RaceDeity* race() { return (RaceDeity*)(g_arena + OFF_RACE); }
static DragDeity* drag() { return (DragDeity*)(g_arena + OFF_DRAG); }
static SixtyToZeroDeity* sixty() { return (SixtyToZeroDeity*)(g_arena + OFF_SIXTY); }
static Car* car(int i) { return (Car*)(g_arena + OFF_CARS + i * CAR_STRIDE); }
static PhysicsPacket* packet() { return (PhysicsPacket*)(g_arena + OFF_PACKET); }
static CheckPoint* checkpoint(int i) { return (CheckPoint*)(g_arena + OFF_CPS + i * CP_STRIDE); }
static ILSeg* seg(int i) { return (ILSeg*)(g_arena + OFF_SEGS + i * SEG_STRIDE); }
static CarMgrInfo* carmgr(int i) { return (CarMgrInfo*)(g_arena + OFF_CARMGR + i * 0x168); }
static GameOptions* opts() { return (GameOptions*)(g_arena + OFF_OPTS); }
static GameOptions* wopts() { return (GameOptions*)(g_arena + OFF_WOPTS); }

// ---- the stubs' script and log -------------------------------------------------------------------------------
// Each stub answers from the script, a per-call list of random words consumed in order, so both passes see the
// same answers if they make the same calls; and logs its call. The fake CenterLine keeps a cursor in its tail
// (+0x60 next checkpoint, +0x64 cookie, +0x68 progress in metres) that update() advances.
struct LogEntry { uint32_t kind, a[5]; };
static LogEntry g_log[2][512];
static int g_nlog[2], g_pass;
static uint32_t g_script[64];
static int g_si;
static uint32_t script() { return g_script[g_si++ & 63]; }
static void logn(uint32_t kind, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0, uint32_t a4 = 0) {
    if (g_nlog[g_pass] < 512) g_log[g_pass][g_nlog[g_pass]] = {kind, {a0, a1, a2, a3, a4}};
    g_nlog[g_pass]++;
}
enum LogKind {
    L_MEMALLOC = 1, L_DELETE, L_CL_CTOR, L_CL_RESET, L_CL_NEXTCP, L_CL_UPDATE, L_CL_COOKIE, L_CL_C2M, L_CL_M2IL,
    L_CL_TIMEBETWEEN, L_CL_DTOR, L_QUICKTAN, L_HASCROSSED, L_REC_LAP, L_REC_ULAP, L_REC_STAGE, L_REC_USTAGE,
    L_REC_OVER, L_REC_UOVER, L_REC_RACETIME, L_TOSS, L_GHOST, L_CAST, L_CARMGR, L_WOPTS, L_TRACKNUM, L_TRACKINFO,
    L_SKILL, L_RANDOM, L_PANIC, L_REPORT, L_DAMAGED, L_TERRAIN, L_TELEPORT, L_BADVIRTUAL, L_RACEINFO_CTOR,
};
static const char* const g_log_names[] = {
    "?", "MemAlloc", "operator delete", "CenterLine::CenterLine", "CenterLine::reset", "CenterLine::get_next_checkpoint",
    "CenterLine::update", "CenterLine::get_car_dlong_cookie", "CenterLine::convert_cookie_to_meters",
    "CenterLine::convert_meters_to_ilpos", "CenterLine::time_between", "CenterLine::~CenterLine", "ILSeg::QuickTan",
    "CheckPoint::HasCrossed", "RecordNewLap", "RecordUnofficialNewLap", "RecordStage", "RecordUnofficialStage",
    "RecordRaceOver", "RecordUnofficialRaceOver", "RecordGetRaceTime", "Sound::Toss", "GhostNewBestLap",
    "MultiDeityCast", "CarMgrGetInfo", "WorldGameOptions", "AIGetTrackNumber", "AIGetTrackInfo", "AIGetSkill", "Random",
    "LogPanic", "LogReport", "Car::AnyWheelsAreDamaged", "TerrainGetHeight", "Car::Teleport", "(unexpected virtual)",
    "RaceInfo::RaceInfo",
};
static float g_racetime[16];
static int g_track;

// the fake CenterLine's cursor
struct FakeCL {
    uint8_t head[0x60];
    int32_t next_cp;                   // +0x60
    uint32_t cookie;                   // +0x64
    float meters;                      // +0x68
    uint32_t touched;                  // +0x6c
};
static_assert(sizeof(FakeCL) == 0x70, "FakeCL");
static void* g_cl_vtable[4];
static void* g_fake_car_vtable[32];

static void* __cdecl stub_MemAlloc(int n) {
    uint8_t* p = g_heap_top;
    g_heap_top += (n + 15) & ~15;
    if (g_heap_top > g_arena + OFF_HEAP + HEAP_SIZE) { printf("the fake heap is full\n"); exit(3); }
    logn(L_MEMALLOC, (uint32_t)n, (uint32_t)(uintptr_t)p);
    return p;
}
static void __cdecl stub_delete(void* p) { logn(L_DELETE, (uint32_t)(uintptr_t)p); }
static CenterLine* __fastcall stub_cl_ctor(CenterLine* cl, Edx, const uint8_t* arg) {
    logn(L_CL_CTOR, (uint32_t)(uintptr_t)cl, (uint32_t)(uintptr_t)arg);
    memset(cl, 0, sizeof *cl);
    cl->vtable = g_cl_vtable;
    cl->line = (script() & 7) ? (ILine*)(g_arena + OFF_ILINE) : 0;
    cl->on_track = (uint8_t)(script() & 1);
    cl->lateral = fbits(script());
    cl->max_dlong = (float)(script() % 5000) + 500.0f;
    FakeCL* f = (FakeCL*)cl;
    f->next_cp = 1;
    f->cookie = script() & 0xffff;
    f->meters = (float)f->cookie * 0.25f;
    return cl;
}
static void __fastcall stub_cl_reset(CenterLine* cl, Edx) {
    logn(L_CL_RESET, (uint32_t)(uintptr_t)cl);
    FakeCL* f = (FakeCL*)cl;
    f->next_cp = 1; f->cookie = 0; f->meters = 0.0f; f->touched = 0;
}
static int __fastcall stub_cl_next_cp(CenterLine* cl, Edx) {
    logn(L_CL_NEXTCP, (uint32_t)(uintptr_t)cl);
    return ((FakeCL*)cl)->next_cp;
}
static uint8_t __fastcall stub_cl_update(CenterLine* cl, Edx, const Point2D* pt, int idx) {
    logn(L_CL_UPDATE, (uint32_t)(uintptr_t)cl, ubits(pt->x), ubits(pt->z), (uint32_t)idx);
    FakeCL* f = (FakeCL*)cl;
    uint32_t s = script();
    f->touched = ubits(pt->x) ^ ubits(pt->z);
    f->cookie += (s >> 8) & 0x3ff;
    f->meters = (float)f->cookie * 0.25f;
    if ((s & 3) == 0) {
        f->next_cp = (f->next_cp % (int)g_ncp) + 1;
        return 1;
    }
    return 0;
}
static uint32_t __fastcall stub_cl_cookie(CenterLine* cl, Edx) {
    logn(L_CL_COOKIE, (uint32_t)(uintptr_t)cl);
    return ((FakeCL*)cl)->cookie;
}
static double __fastcall stub_cl_c2m(CenterLine* cl, Edx, uint32_t cookie) {
    logn(L_CL_C2M, (uint32_t)(uintptr_t)cl, cookie);
    float m = (float)(cookie & 0xfffff) * 0.25f;
    uint32_t s = script();
    if ((s & 15) == 0) m = special_s(s);
    return m;
}
static ILinePos* __fastcall stub_cl_m2il(CenterLine* cl, Edx, ILinePos* ret, uint32_t meters) {
    logn(L_CL_M2IL, (uint32_t)(uintptr_t)cl, meters);
    uint32_t s = script();
    ret->seg = seg((int)(s % NSEGS));
    ret->t = (float)((s >> 8) & 0xff) / 256.0f;
    return ret;
}
static double __fastcall stub_cl_time_between(CenterLine* cl, Edx, const CenterLine* other) {
    logn(L_CL_TIMEBETWEEN, (uint32_t)(uintptr_t)cl, (uint32_t)(uintptr_t)other);
    uint32_t s = script();
    float t = (float)(s & 0xfff) * 0.01f;
    if ((s & 0xf000) == 0) t = special_s(s);
    return t;
}
static void* __fastcall stub_cl_dtor(CenterLine* cl, Edx, int flag) {
    logn(L_CL_DTOR, (uint32_t)(uintptr_t)cl, (uint32_t)flag);
    return cl;
}
static void __fastcall stub_quicktan(const ILSeg* s, Edx, Point2D* out, uint32_t t) {
    logn(L_QUICKTAN, (uint32_t)(uintptr_t)s, t);
    uint32_t r = script();
    out->x = s->dir.x + (float)(r & 0xff) * 0.001f;
    out->z = s->dir.z - (float)((r >> 8) & 0xff) * 0.001f;
    if ((r & 0xff0000) == 0) out->x = special_s(r);
}
static uint8_t __fastcall stub_hascrossed(CheckPoint* cp, Edx, const P3* a, const P3* b) {
    logn(L_HASCROSSED, (uint32_t)(uintptr_t)cp, ubits(a->x), ubits(a->z), ubits(b->x), ubits(b->z));
    return (script() & 3) == 0;
}
static void __cdecl stub_rec_lap(int a, int b, uint32_t c, uint32_t d) { logn(L_REC_LAP, a, b, c, d); }
static void __cdecl stub_rec_ulap(int a, int b, uint32_t c, uint32_t d) { logn(L_REC_ULAP, a, b, c, d); }
static void __cdecl stub_rec_stage(int a, int b, int c, uint32_t d) { logn(L_REC_STAGE, a, b, c, d); }
static void __cdecl stub_rec_ustage(int a, int b, int c, uint32_t d) { logn(L_REC_USTAGE, a, b, c, d); }
static void __cdecl stub_rec_over(int a) { logn(L_REC_OVER, a); }
static void __cdecl stub_rec_uover(int a) { logn(L_REC_UOVER, a); }
static double __cdecl stub_rec_racetime(int i) { logn(L_REC_RACETIME, i); return g_racetime[i & 15]; }
static void __cdecl stub_toss(const char* name, int n) { logn(L_TOSS, (uint32_t)(uintptr_t)name, n); }
static void __cdecl stub_ghost(int a, int b) { logn(L_GHOST, a, b); }
static void __cdecl stub_cast(const DeityPacket* p, int n) {
    uint32_t w[3];
    memcpy(w, p, 12);
    logn(L_CAST, w[0], w[1], w[2] & 0xffffff, n);           // the pad byte isn't compared
}
static const CarMgrInfo* __cdecl stub_carmgr(int i) { logn(L_CARMGR, i); return carmgr(i & 15); }
static const GameOptions* __cdecl stub_wopts() { logn(L_WOPTS); return wopts(); }
static int __cdecl stub_tracknum() { logn(L_TRACKNUM); return g_track; }
static const AITrackInfo* __cdecl stub_trackinfo(int i) { logn(L_TRACKINFO, i); return (AITrackInfo*)(g_arena + OFF_TRACKINFO); }
static const uint8_t* __cdecl stub_skill(int i) { logn(L_SKILL, i); return g_arena + OFF_SKILL; }
static int __cdecl stub_random(int n) { uint32_t s = script(); logn(L_RANDOM, n, s); return (int)(s % (uint32_t)n); }
static void __cdecl stub_panic(const char* fmt, ...) { logn(L_PANIC, (uint32_t)(uintptr_t)fmt); }   // (a vararg may be absent)
static void __cdecl stub_report(const char* fmt, ...) { va_list ap; va_start(ap, fmt); uint32_t a = va_arg(ap, uint32_t); va_end(ap); logn(L_REPORT, (uint32_t)(uintptr_t)fmt, a); }
static uint8_t __fastcall stub_damaged(Car* c, Edx) { logn(L_DAMAGED, (uint32_t)(uintptr_t)c); return c->_e3e[0]; }
static double __cdecl stub_terrain(uint32_t x, uint32_t z) {
    logn(L_TERRAIN, x, z);
    float h = fbits(x) * 0.01f + fbits(z) * 0.02f;
    return h;
}
static void __fastcall stub_teleport(Car* c, Edx, const P3* pos, const Point2D* heading) {
    logn(L_TELEPORT, (uint32_t)(uintptr_t)c, ubits(pos->x), ubits(pos->y), ubits(pos->z), ubits(heading->x) ^ ubits(heading->z));
    memcpy(&c->frame.pos, pos, 12);
    memcpy(&c->frame.rot.m[6], &heading->x, 4);
    memcpy(&c->frame.rot.m[8], &heading->z, 4);
    c->lowering = 1;
}
static int __fastcall stub_bad_virtual(void* self, Edx) { logn(L_BADVIRTUAL, (uint32_t)(uintptr_t)self); return 0; }
static RaceInfo* __fastcall stub_raceinfo_ctor(RaceInfo* r, Edx) { logn(L_RACEINFO_CTOR, (uint32_t)(uintptr_t)r); return r; }

static void install_stubs() {
    patch_jmp(0x004140e0, (void*)&stub_MemAlloc);
    patch_jmp(0x00414390, (void*)&stub_delete);
    patch_jmp(0x00422400, (void*)&stub_cl_ctor);
    patch_jmp(0x004224c0, (void*)&stub_cl_reset);
    patch_jmp(0x00422950, (void*)&stub_cl_next_cp);
    patch_jmp(0x00422510, (void*)&stub_cl_update);
    patch_jmp(0x00422600, (void*)&stub_cl_cookie);
    patch_jmp(0x004227a0, (void*)&stub_cl_c2m);
    patch_jmp(0x00422710, (void*)&stub_cl_m2il);
    patch_jmp(0x00422840, (void*)&stub_cl_time_between);
    patch_jmp(0x00426150, (void*)&stub_quicktan);
    patch_jmp(0x00440db0, (void*)&stub_hascrossed);
    patch_jmp(0x0042a700, (void*)&stub_rec_lap);
    patch_jmp(0x0042a5d0, (void*)&stub_rec_ulap);
    patch_jmp(0x0042a7f0, (void*)&stub_rec_stage);
    patch_jmp(0x0042a840, (void*)&stub_rec_ustage);
    patch_jmp(0x0042a720, (void*)&stub_rec_over);
    patch_jmp(0x0042a7e0, (void*)&stub_rec_uover);
    patch_jmp(0x0042a860, (void*)&stub_rec_racetime);
    patch_jmp(0x004724c0, (void*)&stub_toss);
    patch_jmp(0x0040e950, (void*)&stub_ghost);
    patch_jmp(0x004a2b70, (void*)&stub_cast);
    patch_jmp(0x00464490, (void*)&stub_carmgr);
    patch_jmp(0x004627a0, (void*)&stub_wopts);
    patch_jmp(0x00420df0, (void*)&stub_tracknum);
    patch_jmp(0x00424e40, (void*)&stub_trackinfo);
    patch_jmp(0x00420d90, (void*)&stub_skill);
    patch_jmp(0x0041b6e0, (void*)&stub_random);
    patch_jmp(0x004112b0, (void*)&stub_panic);
    patch_jmp(0x00411150, (void*)&stub_report);
    patch_jmp(0x0043b060, (void*)&stub_damaged);
    patch_jmp(0x00465ae0, (void*)&stub_terrain);
    patch_jmp(0x00444290, (void*)&stub_raceinfo_ctor);
    g_cl_vtable[0] = (void*)&stub_cl_dtor;
    for (int i = 0; i < 32; i++) g_fake_car_vtable[i] = (void*)&stub_bad_virtual;
    g_fake_car_vtable[0x40 / 4] = (void*)&stub_teleport;
    g_car_vtable = g_fake_car_vtable;
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
static Stat g_stats[96];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    g_stats[g_nstats] = {name, 0, 0, 0, 0, 0, 0};
    return g_stats[g_nstats++];
}

struct Snapshot { uint8_t arena[ARENA_SIZE]; uint8_t globals[GLOBALS_BYTES]; uint8_t* heap_top; };
static Snapshot g_start, g_after;
static Footprint g_fp;
static int g_world;
static const char* g_phase = "";

static void save(Snapshot& s) {
    memcpy(s.arena, g_arena, ARENA_SIZE);
    uint8_t* p = s.globals;
    for (const GRange& g : g_globals) { memcpy(p, (void*)(uintptr_t)g.at, g.n); p += g.n; }
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
    uint32_t base = 0;
    const char* name;
    if (off < OFF_DRAG) { name = "RaceDeity"; base = OFF_RACE; }
    else if (off < OFF_SIXTY) { name = "DragDeity"; base = OFF_DRAG; }
    else if (off < OFF_CARS) { name = "SixtyToZeroDeity"; base = OFF_SIXTY; }
    else if (off < OFF_HEAP) { name = "car"; base = OFF_CARS + (off - OFF_CARS) / CAR_STRIDE * CAR_STRIDE; }
    else if (off < OFF_PACKET) { name = "heap (centre lines)"; base = OFF_HEAP; }
    else if (off < OFF_CPS) { name = "packet"; base = OFF_PACKET; }
    else if (off < OFF_RECORDS) { name = "fixtures"; base = OFF_CPS; }
    else { name = "records"; base = OFF_RECORDS; }
    *rel = off - base;
    return name;
}
static void snap_globals(uint8_t* p) {
    for (const GRange& g : g_globals) { memcpy(p, (void*)(uintptr_t)g.at, g.n); p += g.n; }
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
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    g_pass = 0; g_nlog[0] = 0; g_si = 0;
    uint64_t ro = 0, rn = 0;
    int fo = 0, fn = 0;
    __try { ro = run(true); } __except (EXCEPTION_EXECUTE_HANDLER) { fo = 1; }
    save(g_after);
    if (memcmp(g_after.arena, g_start.arena, ARENA_SIZE) || memcmp(g_after.globals, g_start.globals, GLOBALS_BYTES)) st.changed++;
    load(g_start);
    g_pass = 1; g_nlog[1] = 0; g_si = 0;
    _clearfp();
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    __try { rn = run(false); } __except (EXCEPTION_EXECUTE_HANDLER) { fn = 1; }
    if (fo || fn) {
        st.faults++;
        if (fo != fn && st.fails++ < 4) printf("MISMATCH %s (world %d, %s): original %s, rewrite %s\n", name, g_world, g_phase,
                                                fo ? "faulted" : "ran", fn ? "faulted" : "ran");
        load(g_after);
        return;
    }
    uint8_t now_globals[GLOBALS_BYTES];
    snap_globals(now_globals);
    int nlog0 = g_nlog[0] < 512 ? g_nlog[0] : 512;
    bool bad = ro != rn || memcmp(g_after.arena, g_arena, ARENA_SIZE) || memcmp(g_after.globals, now_globals, GLOBALS_BYTES) ||
               g_after.heap_top != g_heap_top || g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], sizeof(LogEntry) * nlog0);
    if (bad && st.fails++ < 4) {
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
        if (nl > 512) nl = 512;
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
#define CHECK0(FN)                                                                                      \
    check(VP_CAT(name_, FN), [&](bool orig) { return invoke(orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN); }, \
          [&](Footprint& fp) { VP_CAT(fpof_, FN)(fp); })

// ---- building a world ----------------------------------------------------------------------------------------
static void rotation(M3* m) {
    double yaw = rf(-3.1416f, 3.1416f), lim = chance(10) ? 3.1416 : 0.3;
    double pitch = rf((float)-lim, (float)lim), roll = rf((float)-lim, (float)lim);
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), spp = sin(pitch), cr = cos(roll), sr = sin(roll);
    double R[9] = {cy * cr + sy * spp * sr, cp * sr, -sy * cr + cy * spp * sr, -cy * sr + sy * spp * cr, cp * cr,
                   sy * sr + cy * spp * cr, sy * cp, -spp, cy * cp};
    for (int i = 0; i < 9; i++) m->m[i] = (float)R[i];
}
static void setup_car(int i) {
    Car* c = car(i);
    memset(c, 0, CAR_STRIDE);
    c->vtable = g_fake_car_vtable;
    rotation(&c->frame.rot);
    c->frame.pos.x = rf(-2000, 2000); c->frame.pos.y = rf(-50, 300); c->frame.pos.z = rf(-2000, 2000);
    c->velocity.x = rf(-60, 60); c->velocity.y = rf(-5, 5); c->velocity.z = rf(-60, 60);
    c->race_state = ri(0, 3);
    c->car_index = i;
    c->front_axle.x = sp(rf(-0.1f, 0.1f)); c->front_axle.y = sp(rf(-0.5f, 0.1f)); c->front_axle.z = sp(rf(1.0f, 1.6f));
    c->lap_top_speed = sp(rf(0, 90));
    c->lowering = chance(15);
    c->_e3e[0] = chance(20);
}
static void move_car(int i) {
    Car* c = car(i);
    c->frame.pos.x = sp(c->frame.pos.x + c->velocity.x * 0.016f);
    c->frame.pos.y = sp(c->frame.pos.y + c->velocity.y * 0.016f);
    c->frame.pos.z = sp(c->frame.pos.z + c->velocity.z * 0.016f);
    if (chance(10)) rotation(&c->frame.rot);
    if (chance(5)) c->frame.rot.m[4] = sp(rf(-1, 1));
    c->velocity.x = sp(c->velocity.x + rf(-2, 2)); c->velocity.z = sp(c->velocity.z + rf(-2, 2));
    if (chance(30)) c->lap_top_speed = sp(rf(0, 90));
    if (chance(3)) c->race_state = ri(0, 3);
    if (chance(5)) c->lowering = chance(50);
    if (chance(5)) c->_e3e[0] = chance(50);
}
static void setup_fixtures() {
    // checkpoints, the ideal-line ring, the line, the car manager, the AI tables, the record lists
    for (int i = 0; i < 4; i++) {
        CheckPoint* cp = checkpoint(i);
        memset(cp, 0, CP_STRIDE);
        cp->vtable = (void**)0x004dc350;
        cp->frame.pos.x = rf(-500, 500); cp->frame.pos.y = rf(-5, 5); cp->frame.pos.z = sp(rf(-500, 500));
    }
    for (int i = 0; i < NSEGS; i++) {
        ILSeg* s = seg(i);
        memset(s, 0, SEG_STRIDE);
        s->next = seg((i + 1) % NSEGS);
        s->p.x = rf(-1000, 1000); s->p.z = rf(-1000, 1000);
        s->dir.x = sp(rf(-30, 30)); s->dir.z = sp(rf(-30, 30));
        s->corridor_half_width = sp(rf(3, 12));
        s->field_2c = chance(50) ? 0 : rnd();
    }
    ILine* line = (ILine*)(g_arena + OFF_ILINE);
    memset(line, 0, 0x20);
    line->width = sp(rf(6, 30));
    for (int i = 0; i < 16; i++) {
        CarMgrInfo* ci = carmgr(i);
        memset(ci, 0, 0x168);
        ci->type = i == *g_player_car ? 1 : chance(12) ? 3 : chance(50) ? 0 : 2;
        ci->ai = ci->type == 1 ? (uint8_t)chance(10) : (uint8_t)(chance(85) ? 1 : 0);
        ci->rank = ri(0, 5);
    }
    AITrackInfo* ti = (AITrackInfo*)(g_arena + OFF_TRACKINFO);
    memset(ti, 0, 0x40);
    ti->first_lap_allowance = sp(rf(0, 10));
    float* skill = (float*)(g_arena + OFF_SKILL);
    for (int i = 0; i < 0x200 / 4; i++) skill[i] = sp(rf(30, 200));
    g_track = ri(0, 7);
    for (int i = 0; i < 16; i++) g_racetime[i] = chance(30) ? -1.0f : sp(rf(60, 600));
    RecordList* const lists[3] = {g_rec_race, g_rec_laps, g_rec_stages};
    for (int k = 0; k < 3; k++) {
        lists[k]->recs = g_arena + OFF_RECORDS + k * REC_CAP * 0x48;
        lists[k]->count = ri(0, 5);
        lists[k]->cap = REC_CAP;
    }
}
static void setup_globals(int ncars) {
    *g_carmgr_count = ncars;
    *g_player_car = chance(85) ? ri(0, ncars - 1) : -1;
    *g_multi = chance(10);
    *g_replay = chance(5);
    *g_paused = 0;
    *g_incognito = chance(30);
    *g_tick = ri(0, 3000);
    *g_game_state = ri(0, 3);
    *g_deity = 0;
    *g_globa = 0;
    *g_sort_fn = 0;
    for (int i = 0; i < 16; i++) g_lapped[i] = chance(50);
    GameOptions* o = opts();
    for (int i = 0; i < 10; i++) ((uint32_t*)o)[i] = rnd();
    o->num_laps = ri(1, 4);
    o->race_type = 0;
    memcpy(wopts(), o, 40);
    if (chance(30)) wopts()->num_laps = ri(1, 5);
    g_heap_top = g_arena + OFF_HEAP;
    memset(g_arena + OFF_HEAP, 0, HEAP_SIZE);
    memset(packet(), 0, 0x400);
    g_ncp = (uint32_t)ri(1, 4);
}
static void perturb_lines(RaceDeity* d, int ncars) {
    for (int i = 0; i < ncars; i++) {
        CenterLine* cl = d->info[i].centerline;
        if (!cl) continue;
        if (chance(10)) cl->on_track = (uint8_t)chance(50);
        if (chance(10)) cl->lateral = sp(rf(-20, 20));
        if (chance(5)) cl->line = chance(80) ? (ILine*)(g_arena + OFF_ILINE) : 0;
    }
}
static void wild_info(RaceDeity* d, int ncars) {
    RaceInfo& in = d->info[ri(0, ncars - 1)];
    float* f = &in.line_time;
    f[ri(0, 4)] = special();
    if (chance(30)) in.best_lap = special();
    if (chance(30)) in.best_speed = special();
}
static void fill_packet_tail() {
    PhysicsPacket* p = packet();
    p->drag.best_time = sp(chance(40) ? 0.0f : rf(0, 20));
    p->drag.best_speed = sp(chance(40) ? 0.0f : rf(0, 60));
}

// ---- the races ------------------------------------------------------------------------------------------------
static int g_cov[16];   // coverage: laps completed, ticks with a finished car, drag runs, drag running ticks, 60-0 stops, Begin/End

static void run_race_world() {
    int ncars = ri(1, 8);
    setup_globals(ncars);
    setup_fixtures();
    RaceDeity* d = race();
    memset(d, 0, 0x800);
    g_phase = "setup";
    CHECK(RaceDeity_ctor, d, 0, (const GameOptions*)opts());
    CHECK(RaceDeity_Init, d, 0);
    for (int i = 0; i < ncars; i++) {
        setup_car(i);
        if (chance(20)) CHECK(RaceDeity_RegisterCar_Net, d, 0, i, car(i));
        else CHECK(RaceDeity_RegisterCar_Local, d, 0, i, car(i));
    }
    for (uint32_t k = 0; k < g_ncp; k++) CHECK(RaceDeity_RegisterCheckPoint, d, 0, checkpoint(k & 3));
    if (chance(10)) CHECK(RaceDeity_GetNumCheckpoints, d, 0);
    *g_tick += ri(0, 5);
    CHECK(RaceDeity_Reset, d, 0);
    if (d->num_cars < 1) { CHECK(RaceDeity_dtor, d, 0); return; }     // an incognito ghost alone
    int n = d->num_cars;
    int steps = ri(20, 160);
    g_phase = "race";
    for (int s = 0; s < steps; s++) {
        *g_tick += ri(1, 3);
        *g_paused = chance(3);
        if (chance(4)) *g_game_state = ri(0, 3);
        if (chance(2)) *g_multi = chance(50);
        if (chance(1)) *g_replay = chance(50);
        for (int i = 0; i < n; i++) move_car(i);
        perturb_lines(d, n);
        if (chance(3)) wild_info(d, n);
        CHECK(RaceDeity_Update, d, 0);
        for (int i = 0; i < n; i++) CHECK(RaceDeity_UpdateCar, d, 0, i, car(i));
        int i = ri(0, n - 1);
        switch (rnd() % 24) {
        case 0: case 1: CHECK(RaceDeity_TeleportToLine, d, 0, i, car(i)); break;
        case 2: case 3: CHECK(RaceDeity_GetMessage, d, 0, packet()); break;
        case 4: CHECK(CanTeleport, car(i)); break;
        case 5: CHECK(RaceDeity_CarIsFinished, d, 0, i); CHECK(RaceDeity_GetCurrentLap, d, 0, i); break;
        case 6: CHECK(RaceDeity_GetCarByPlace, d, 0, ri(0, n - 1)); CHECK(RaceDeity_GetNumLaps, d, 0); break;
        case 7: CHECK(RaceDeity_GetCurrentLapStart, d, 0, i); CHECK(RaceDeity_GetDLong, d, 0, i); break;
        case 8: CHECK(RaceDeity_GetMaxDLong, d, 0); CHECK(RaceDeity_ConvertDLong, d, 0, rnd()); break;
        case 9: {
            DeityPacket pk;
            pk.a = sp(rf(0, 200)); pk.b = sp(rf(0, 90)); pk.car = (uint8_t)ri(0, n - 1); pk.lap = (uint8_t)ri(0, 5);
            pk.kind = chance(60) ? 0xfa : (uint8_t)ri(1, 4); pk._b = 0;
            CHECK(RaceDeity_NetPacket, d, 0, (const DeityPacket*)&pk);
            break;
        }
        case 10: CHECK(RaceDeity_StartRaceAt, d, 0, sp((float)(*g_tick * 0.016f + rf(-1, 10)))); break;
        case 11: CHECK(variance, ubits(sp(rf(30, 200))), chance(90) ? ri(1, 6) : ri(-6, -1)); break;
        case 12: CHECK(RaceDeity_place_cars, d, 0); break;
        case 13: CHECK(RaceDeity_sort_cars, d, 0, chance(50) ? 0x00443cc0u : 0x00443dd0u); break;
        case 14: CHECK(RaceDeity_set_bests, d, 0, &d->info[i]); break;
        case 15: CHECK(RaceDeity_record_newlap, d, 0, i, ri(0, 5), ubits(sp(rf(0, 200))), ubits(sp(rf(0, 90)))); break;
        case 16: CHECK(RaceDeity_record_newstage, d, 0, i, ri(0, 5), ri(0, 4), ubits(sp(rf(0, 200)))); break;
        case 17: CHECK(RaceDeity_reset_car, d, 0, i); break;
        case 18: CHECK(RaceDeity_final_sort, d, 0, &d->info[i], &d->info[ri(0, n - 1)]); break;
        case 19: CHECK(RaceDeity_race_sort, d, 0, &d->info[i], &d->info[ri(0, n - 1)]); break;
        case 20: {
            *g_sort_fn = (SortFn)(chance(50) ? 0x00443cc0u : 0x00443dd0u);
            int a = ri(0, n - 1), b = ri(0, n - 1);
            CHECK(RaceDeity_static_sort_f, (const void*)&a, (const void*)&b);
            *g_sort_fn = 0;
            break;
        }
        case 21: CHECK(Deity_StopRace, (Deity*)d, 0); break;
        case 22: CHECK(RaceDeity_RegisterCheckPoint, d, 0, checkpoint(0)); break;
        default: break;
        }
        for (int k = 0; k < n; k++) if (d->info[k].finished) { g_cov[1]++; break; }
    }
    g_phase = "post-race";
    for (int k = 0; k < n; k++) g_cov[0] += d->info[k].lap;
    CHECK(RaceDeity_PostRace, d, 0);
    if (chance(50)) CHECK(RaceDeity_GetMessage, d, 0, packet());
    CHECK(RaceDeity_dtor, d, 0);
}

static void run_drag_world() {
    setup_globals(1);
    setup_fixtures();
    DragDeity* d = drag();
    memset(d, 0, 0x100);
    opts()->race_type = chance(50) ? 1 : 3;
    g_phase = "drag setup";
    CHECK(DragDeity_ctor, d, 0, (const GameOptions*)opts());
    d->vtable = (void**)(opts()->race_type == 1 ? 0x004db8c0 : 0x004db930);
    setup_car(0);
    if (chance(5)) CHECK(DragDeity_RegisterCar_Net, d, 0, 0, car(0));
    CHECK(DragDeity_RegisterCar_Local, d, 0, 0, car(0));
    int ncp = chance(85) ? 3 : ri(0, 4);
    for (int k = 0; k < ncp; k++) CHECK(DragDeity_RegisterCheckPoint, d, 0, checkpoint(k & 3));
    CHECK(DragDeity_Init, d, 0);
    CHECK(DragDeity_Reset, d, 0);
    CHECK(DragDeity_GetMaxTime, d, 0);
    if (!(d->cp[0] && d->cp[1] && d->cp[2])) { CHECK(DragDeity_GetMessage, d, 0, packet()); return; }
    int steps = ri(20, 120);
    g_phase = "drag";
    for (int s = 0; s < steps; s++) {
        *g_tick += ri(1, 3);
        move_car(0);
        if (chance(30)) car(0)->frame.pos.z = sp(d->cp[1]->frame.pos.z + rf(-100, 1100));
        if (chance(3)) d->state = ri(0, 4);
        if (chance(20)) d->stage_time = sp((float)(*g_tick * 0.016f - rf(0, 4)));
        CHECK(DragDeity_UpdateCar, d, 0, 0, car(0));
        switch (rnd() % 10) {
        case 0: fill_packet_tail(); CHECK(DragDeity_GetMessage, d, 0, packet()); break;
        case 1: CHECK(DragDeity_TeleportToLine, d, 0, 0, car(0)); break;
        case 2: CHECK(ZeroToSixtyDeity_IsDone, d, 0, car(0)); break;
        case 3: CHECK(QuarterMileDeity_IsDone, d, 0, car(0)); break;
        case 4: CHECK(DragDeity_GetNumLaps, d, 0); CHECK(DragDeity_Init, d, 0); break;
        default: break;
        }
        if (d->state == 3) g_cov[3]++;
    }
    g_cov[2] += d->num_laps;
}

static void run_sixty_world() {
    setup_globals(1);
    setup_fixtures();
    SixtyToZeroDeity* d = sixty();
    memset(d, 0, 0x100);
    opts()->race_type = 5;
    g_phase = "60-0 setup";
    CHECK(SixtyToZeroDeity_ctor, d, 0, (const GameOptions*)opts());
    setup_car(0);
    CHECK(SixtyToZeroDeity_Init, d, 0);
    CHECK(SixtyToZeroDeity_Reset, d, 0);
    int steps = ri(30, 160);
    float target = rf(28, 45);
    g_phase = "60-0";
    for (int s = 0; s < steps; s++) {
        *g_tick += 1;
        Car* c = car(0);
        // a speed profile: up past 60 mph, then hard on the brakes to a stop, then again
        float speed = (float)sqrt((double)c->velocity.x * c->velocity.x + (double)c->velocity.y * c->velocity.y + (double)c->velocity.z * c->velocity.z);
        float want = d->state == 1 || (d->state == 2 && speed > 0.2f) ? speed - rf(0.5f, 3.0f) : speed + rf(0.5f, 3.0f);
        if (d->state == 0 && speed > target) want = speed - rf(0.5f, 3.0f);
        if (want < 0) want = 0;
        float k = speed > 1e-6f ? want / speed : 0.0f;
        c->velocity.x = sp(speed > 1e-6f ? c->velocity.x * k : want); c->velocity.y = sp(c->velocity.y * k); c->velocity.z = sp(c->velocity.z * k);
        c->frame.pos.x = sp(c->frame.pos.x + c->velocity.x * 0.016f); c->frame.pos.z = sp(c->frame.pos.z + c->velocity.z * 0.016f);
        if (chance(2)) d->state = ri(0, 3);
        CHECK(SixtyToZeroDeity_UpdateCar, d, 0, 0, c);
        switch (rnd() % 8) {
        case 0: fill_packet_tail(); CHECK(SixtyToZeroDeity_GetMessage, d, 0, packet()); break;
        case 1: CHECK(SixtyToZeroDeity_TeleportToLine, d, 0, 0, c); break;
        case 2: CHECK(SixtyToZeroDeity_GetNumLaps, d, 0); break;
        default: break;
        }
    }
    g_cov[4] += d->num_laps;
}

static void run_begin_end_world() {
    setup_globals(ri(1, 4));
    setup_fixtures();
    static const uint32_t types[] = {0, 1, 3, 5, 0, 1, 3, 5, 2, 4, 7};
    opts()->race_type = types[rnd() % (sizeof types / sizeof *types)];
    g_phase = "CarDeityBegin";
    CHECK(CarDeityBegin, (const GameOptions*)opts());
    Deity* d = *g_deity;
    if (d) {
        if (opts()->race_type == 0) {
            CHECK(RaceDeity_Init, (RaceDeity*)d, 0);
            CHECK(Deity_LocalCarsReady, d, 0);
        } else {
            CHECK(Deity_GetNumCheckpoints, d, 0);
            CHECK(Deity_CarIsFinished, d, 0, ri(0, 5));
        }
        CHECK0(CarDeityEnd);
        g_cov[5]++;
    }
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 300;
    if (argc > 2) g_state = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_race.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    install_stubs();
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (!g_arena) return 2;
    uint32_t gb = 0;
    for (const GRange& g : g_globals) gb += g.n;
    if (gb != GLOBALS_BYTES) { printf("GLOBALS_BYTES is %u, should be %u\n", (unsigned)GLOBALS_BYTES, gb); return 2; }
    for (g_world = 0; g_world < worlds; g_world++) {
        switch (g_world % 10) {
        case 7: run_drag_world(); break;
        case 8: run_sixty_world(); break;
        case 9: run_begin_end_world(); break;
        default: run_race_world(); break;
        }
    }
    int failed = 0;
    long calls = 0;
    printf("%-36s %8s %8s %8s %8s %8s %8s\n", "function", "calls", "changed", "replay", "faults", "differ", "fp-miss");
    for (int i = 0; i < g_nstats; i++) {
        Stat& st = g_stats[i];
        printf("%-36s %8ld %8ld %8ld %8ld %8ld %8ld\n", st.name, st.calls, st.changed, st.replay_only, st.faults, st.fails, st.fp_fails);
        if (st.fails || st.fp_fails) failed++;
        calls += st.calls;
    }
    printf("%d worlds, %ld calls; %d of %d functions differ or escape their footprint; %d unknown footprint classes\n",
           worlds, calls, failed, g_nstats, g_fp_unknown);
    printf("covered: laps completed %d, ticks with a finished car %d, drag runs %d, drag ticks running %d, 60-0 stops %d, Begin/End %d\n",
           g_cov[0], g_cov[1], g_cov[2], g_cov[3], g_cov[4], g_cov[5]);
    return failed ? 1 : 0;
}
