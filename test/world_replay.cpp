// world_replay.cpp -- the game's own replay and the ghost car (hook/phys_replay.cpp) against the originals, on
// random worlds, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_replay.cpp
//        /Fo<outdir>\ /Fe<outdir>\world_replay.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_replay.exe [worlds] [seed]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. Each world
// is a whole race's replay, made and played by calls that are each checked: a real Car and a real GhostCar (the
// ORIGINAL constructors, so Setup, the Wheels, the drivetrain and the body volume are the game's own), dynamic
// phobs (PhobDyno, Ball, Obstacle), a static CheckPoint and a test phob whose packet size (up to 64 KB) makes
// the 2 MB data ring wrap; the replay's statics set up by BGBegin / RegisterPhob / DoneCreating; the game's
// own event handlers installed (crash, splash, AI, Car damage and reset) plus an unknown one; a recording of
// random ticks (skips too) with random events (some past the 0x600-byte limit) and a moving world, over a frame
// ring sized to wrap; the ghost following the laps (feed_ghost_car, NewLap, Update, UpdateCommon, GetMessage);
// the seal; a playback of play / rewind / fast forward / shuttle / pause / telemetry; a save, a reload
// (sometimes of a damaged file) and another playback; and the rest of the ghost's and the Car stubs' calls.
//
// Every call runs twice from the same state -- the original, then the rewrite -- at the physics thread's 24-bit
// precision (the main thread's calls, 1 in 3, at 53): the whole arena, the image's .data/.bss, the return value,
// faults and the stubs' call logs must match bit for bit; every byte the original changed must lie in the
// rewrite's footprint (or, on the physics thread, in the physics statics, globals_phys.inc) unless it's
// replay_only. The world goes on from the original's result.
//
// Stubbed (a jump to a logger): MemAlloc (a bump arena), operator delete, the Car's construction callees
// (TireCreate, TireSound::Create, ResourceExists, StringTable*, atof, atexit, PhysTaskRegisterCar, EngineSound
// create / destroy, Sound3D::Create, SoundDash::Create), SingleBegin / _SingleEnter / _SingleLeave / _SingleEnd,
// FileReadExact / FileWrite (an in-memory file), PTimeNow (a script), LogReport, LogPanic, the ASSERT_MSGs,
// Multi(Un)RegisterLocalCar, RaceDeity's RegisterCar / UpdateCar / CarIsFinished / GetCurrentLap (scripted laps),
// Car::UpdateCommon, CollisionSound::Play; and PhysReplayAddEvent whenever it's called from inside another check
// (Car::Reset's event: an output, as the shadow framework captures it -- unpatched for its own checks). Everything
// else is the game's code. The replay's 2 MB is the same block each time (MemAlloc doesn't clear it), with heap
// before it: an unrecorded slot (-1) is read at data - 1, as in the game.
//
// Faults are compared too (both passes must fault alike): the only ones left are a missing IdealLine for the ghost's
// car (reset_to_head on NULL, 3% of worlds' line slots), as the original would.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
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

// ---- class sizes (state_layout.inc) and the physics statics (globals_phys.inc) -------------------------------------
struct ClassInfo { uint32_t vt, size; const char* name; uint32_t first, count; };
static const ClassInfo k_classes[] = {
#define VP_CLASSES
#include "../hook/state_layout.inc"
#undef VP_CLASSES
};
struct GlobalRange { uint32_t addr, bytes; const char* name; };
static const GlobalRange k_globals[] = {
#include "../hook/globals_phys.inc"
};
static void* g_pad_vt[16];                                 // the test phob's vtable
enum { PAD_SIZE = 0x80 };
static int g_fp_unknown;
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) {
    uint32_t vt = *(uint32_t*)obj;
    for (auto& c : k_classes)
        if (c.vt == vt) { add(obj, c.size, what); return; }
    if (vt == (uint32_t)(uintptr_t)g_pad_vt) { add(obj, PAD_SIZE, what); return; }
    printf("footprint: %s at %p has an unknown class (vtable %08x)\n", what, obj, vt);
    g_fp_unknown++;
}

#include "../hook/phys_replay.cpp"

// ---- random values ------------------------------------------------------------------------------------------
static uint32_t g_state = 0x2545f491u;
static uint32_t rnd() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float rf(float lo, float hi) { return lo + (hi - lo) * uni(); }
static int ri(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float special() {
    switch (rnd() % 8) {
    case 0: return Fbits(0x7fc00000u | (rnd() & 0x3fffff));
    case 1: return Fbits(0xffc00000u | (rnd() & 0x3fffff));
    case 2: return Fbits(0x7f800001u + (rnd() & 0x3ffffe));  // signalling
    case 3: return Fbits(0x7f800000u | (rnd() & 0x80000000u));
    case 4: return Fbits(rnd() & 0x807fffffu);
    case 5: return (rnd() & 1) ? 0.0f : -0.0f;
    case 6: return Fbits((rnd() & 0x80000000u) | 0x7f7fffffu);
    default: return Fbits(rnd());
    }
}
static float sp(float x, int pct = 3) { return (int)(rnd() % 100) < pct ? special() : x; }

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
    SetEnvironmentVariableA("VP_FUZZ_CHILD", "1");
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
static void patch_jump(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)(uintptr_t)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uint32_t)(uintptr_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// ---- the arena (everything the world and its calls allocate) and the image's .data/.bss -----------------------------
enum { ARENA_BYTES = 12 << 20 };
static uint8_t* g_arena;
static uint32_t g_bump, g_hw;                   // next free byte; the high-water mark (compared up to it)
static uint8_t* g_arena_start;
static uint8_t* g_arena_after;
static void* arena_alloc(uint32_t n) {
    uint32_t at = (g_bump + 15) & ~15u;
    if (at + n > ARENA_BYTES) { printf("arena full\n"); exit(3); }
    g_bump = at + n;
    if (g_bump > g_hw) g_hw = g_bump;
    return g_arena + at;
}
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES_IMG = 0xf5f2c };
static uint8_t* g_data_pristine;
static uint8_t* g_data_start;
static uint8_t* g_data_after;
static bool in_physics_globals(uint32_t a) {
    for (auto& g : k_globals)
        if (a >= g.addr && a < g.addr + g.bytes) return true;
    return false;
}

// ---- the stubs' log, one per pass ------------------------------------------------------------------------------------
enum { LOG_MAX = 8192 };
static uint32_t g_log[2][LOG_MAX];
static int g_nlog[2], g_pass;
static void log_put(uint32_t v) {
    if (g_nlog[g_pass] < LOG_MAX) g_log[g_pass][g_nlog[g_pass]] = v;
    g_nlog[g_pass]++;
}
static uint32_t off_of(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    if (b >= g_arena && b < g_arena + ARENA_BYTES) return (uint32_t)(b - g_arena) | 0x80000000u;
    if ((uintptr_t)p >= 0x400000 && (uintptr_t)p < 0x600000) return (uint32_t)(uintptr_t)p;   // the image
    return 0x57ac0000;                          // a stack address (a local's: differs between the passes)
}
static uint32_t hash_bytes(const void* p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}

// ---- the script the stubs follow (fixed within a check; cursors reset each pass) --------------------------------------
struct Script {
    int32_t times[8];                           // PTimeNow
    int32_t lap;                                // RaceDeity::GetCurrentLap
    uint8_t finished;                           // CarIsFinished
    uint32_t resource_seed, sound_null_mask;
};
static Script g_script;
static int g_time_i, g_sound_calls;
// the in-memory file (FileReadExact reads it; FileWrite appends to it)
enum { FILE_MAX = 3 << 20 };
struct FileState { uint32_t len, rpos; };
static uint8_t* g_file;
static FileState g_fs, g_fs_start, g_fs_after;
static uint8_t* g_file_after;                   // the original's written bytes

static uint8_t* g_databuf;                     // the replay's 2 MB, the same block each time (MemAlloc doesn't clear)
static void* __cdecl stub_MemAlloc(int n) {
    log_put('MALC'); log_put((uint32_t)n);
    if (n == 0x200000 && g_databuf) return g_databuf;
    return arena_alloc((uint32_t)n);
}
static void __cdecl stub_delete(void* p) { log_put('DELE'); log_put(off_of(p)); }
static int32_t __cdecl stub_SingleBegin(const char* s) { log_put('SBEG'); log_put(off_of(s)); return 5; }
static void __cdecl stub_SingleEnter(int32_t id, const char*, int32_t) { log_put('SENT'); log_put((uint32_t)id); }
static void __cdecl stub_SingleLeave(int32_t id, const char*, int32_t) { log_put('SLEV'); log_put((uint32_t)id); }
static void __cdecl stub_SingleEnd(int32_t id, const char*, int32_t) { log_put('SEND'); log_put((uint32_t)id); }
static uint8_t __cdecl stub_FileReadExact(int32_t fd, void* buf, int32_t n) {
    log_put('FRD '); log_put((uint32_t)fd); log_put(off_of(buf)); log_put((uint32_t)n);
    if (n < 0 || g_fs.rpos + (uint32_t)n > g_fs.len) { log_put(0); return 0; }
    memcpy(buf, g_file + g_fs.rpos, n);
    g_fs.rpos += n;
    log_put(1);
    return 1;
}
static uint8_t __cdecl stub_FileWrite(int32_t fd, const void* buf, int32_t n) {
    log_put('FWR '); log_put((uint32_t)fd); log_put((uint32_t)n);
    if (n < 0 || g_fs.len + (uint32_t)n > FILE_MAX) { log_put(0); return 0; }
    memcpy(g_file + g_fs.len, buf, n);
    g_fs.len += n;
    log_put(hash_bytes(buf, n));
    return 1;
}
static int32_t __cdecl stub_PTimeNow() { log_put('TIME'); return g_script.times[g_time_i++ & 7]; }
static void log_fmt(uint32_t tag, const char* fmt, va_list ap) {
    log_put(tag); log_put(off_of(fmt));
    int args = fmt == S_VERSION ? 2 : fmt == S_FRAMES || fmt == S_NO_HANDLER ? 1 : fmt == S_FRAME_IDX ? 2 : 0;
    for (int i = 0; i < args; i++) log_put(va_arg(ap, uint32_t));
}
static void __cdecl stub_LogReport(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_fmt('LOGR', fmt, ap); va_end(ap); }
static void __cdecl stub_LogPanic(const char* fmt, ...) { va_list ap; va_start(ap, fmt); log_fmt('PANC', fmt, ap); va_end(ap); }
static void __cdecl stub_assert(int32_t ok, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_put('ASRT'); log_put((uint32_t)ok);
    log_fmt('ASFM', fmt, ap);
    va_end(ap);
}
static void __cdecl stub_multi_register(void* car, int32_t i) { log_put('MREG'); log_put(off_of(car)); log_put((uint32_t)i); }
static void __cdecl stub_multi_unregister(void* car) { log_put('MUNR'); log_put(off_of(car)); }
static void __fastcall stub_deity_register(void* self, int, int32_t i, void* car) {
    log_put('DREG'); log_put(off_of(self)); log_put((uint32_t)i); log_put(off_of(car));
}
static void __fastcall stub_deity_update_car(void* self, int, int32_t i, void* car) {
    log_put('DUPC'); log_put((uint32_t)i); log_put(off_of(car));
    ((uint8_t*)self)[0x40 + (i & 15)]++;                   // something in the deity, as the real one writes
}
static uint8_t __fastcall stub_deity_finished(void*, int, int32_t i) { log_put('DFIN'); log_put((uint32_t)i); return g_script.finished; }
static int32_t __fastcall stub_deity_lap(void*, int, int32_t i) { log_put('DLAP'); log_put((uint32_t)i); return g_script.lap; }
static void __fastcall stub_car_update_common(void* self, int) { log_put('CUPC'); log_put(off_of(self)); }
static void __fastcall stub_collision_play(void* self, int, const P3* p, uint32_t loud) {
    log_put('CSPL'); log_put(off_of(self)); log_put(Ubits(p->x)); log_put(Ubits(p->y)); log_put(Ubits(p->z)); log_put(loud);
}
// the Car's construction (as test/world_car2.cpp)
static void* __fastcall stub_sound_dtor(void* self, int, unsigned flags) { log_put('SDDT'); log_put(off_of(self)); log_put(flags); return self; }
static void __fastcall stub_sound_update(void* self, int) { log_put('SDUP'); log_put(off_of(self)); }
static void* g_sound_vtable[8] = {(void*)&stub_sound_dtor, (void*)&stub_sound_update, 0, 0, 0, 0, 0, 0};
static void* fake_object(uint32_t n) {
    void** o = (void**)arena_alloc(n);
    memset(o, 0, n);
    o[0] = g_sound_vtable;
    return o;
}
static void* __cdecl stub_TireCreate(uint32_t w, uint32_t a, uint32_t d, const char*) {
    log_put('TIRE'); log_put(w); log_put(a); log_put(d);
    float* t = (float*)arena_alloc(0x40);
    for (int i = 0; i < 16; i++) t[i] = 0.5f + i * 0.01f;
    t[0x2c / 4] = 1.05f;
    t[0x30 / 4] = 11.0f;
    t[0x34 / 4] = 1.2f;
    return t;
}
static void* __cdecl stub_TireSoundCreate(int ci, int wh, void* w, void* car) {
    log_put('TSND'); log_put(ci); log_put(wh); log_put(off_of(w)); log_put(off_of(car));
    return fake_object(0x10);
}
static uint8_t __cdecl stub_ResourceExists(const char* file) {
    log_put('RSRC');
    uint32_t h = g_script.resource_seed;
    for (const char* p = file; *p; p++) h = h * 31 + (uint8_t)*p;
    return (uint8_t)((h >> 7) & 1);
}
static void* __cdecl stub_StringTableGet(const char*) { log_put('STGT'); return (void*)0x5ab1e000; }
static const char* __cdecl stub_StringTableGetEntry(void*, int row, int col) {
    log_put('STGE'); log_put(row); log_put(col);
    static const char* const v[] = {"-0.31", "0.77", "0.0521", "1e-3", "0.2"};
    return v[(col + g_script.resource_seed) % 5];
}
static void __cdecl stub_StringTableForget(void*) { log_put('STFG'); }
static double __cdecl stub_atof(const char* s) { log_put('ATOF'); return strtod(s, 0); }
static int __cdecl stub_atexit(void (*fn)()) { log_put('ATEX'); log_put((uint32_t)(uintptr_t)fn); return 0; }
static void __cdecl stub_RegisterCar(void* car, int index) { log_put('REGC'); log_put(off_of(car)); log_put(index); }
static void* __cdecl stub_EngineSoundCreate(void* car) { log_put('ESCR'); log_put(off_of(car)); return fake_object(0x100); }
static void __fastcall stub_EngineSoundDtor(void* self, int) { log_put('ESDT'); log_put(off_of(self)); }
static void* __cdecl stub_Sound3DCreate(const char*, int kind, const P3*, const P3*) {
    log_put('S3DC'); log_put(kind);
    if (g_script.sound_null_mask >> (g_sound_calls++ & 31) & 1) return 0;
    return fake_object(0x40);
}
static void* __cdecl stub_SoundDashCreate(const char*, int kind, int index) {
    log_put('SDSH'); log_put(kind); log_put(index);
    if (g_script.sound_null_mask >> (g_sound_calls++ & 31) & 1) return 0;
    return fake_object(0x40);
}
// PhysReplayAddEvent called from inside another check: an output, logged (as the shadow framework captures it)
static void __cdecl stub_add_event(int32_t type, const void* data, int32_t size) {
    log_put('REVT'); log_put((uint32_t)type); log_put((uint32_t)size);
    log_put(size > 0 && size < 0x1000 ? hash_bytes(data, size) : 0);
}
static uint8_t g_add_event_bytes[5];
static void add_event_real(bool real) {
    if (real) memcpy((void*)0x0042d8b0, g_add_event_bytes, 5);
    else patch_jump(0x0042d8b0, (void*)&stub_add_event);
}
// an event handler the footprint does not know (type 6): its payload starts with its own size (u16)
static int32_t __cdecl stub_handler6(const uint8_t* ev, int32_t mode) {
    uint16_t n;
    memcpy(&n, ev, 2);
    log_put('HND6'); log_put(off_of(ev)); log_put((uint32_t)mode); log_put(n);
    return n;                                   // the payload size AddEvent was given
}
// the test phob: packet size, dynamic flag, a packet counter and a checksum of what it replayed
struct Pad {
    void** vtable;
    uint32_t _004[0x60 / 4 - 1];
    int32_t size;                               // +0x60
    uint8_t dynamic, _65[3];                    // +0x64
    uint32_t made;                              // +0x68
    uint32_t sum;                               // +0x6c
    uint8_t _70[PAD_SIZE - 0x70];
};
static_assert(sizeof(Pad) == PAD_SIZE, "Pad");
static int32_t __fastcall pad_size(Pad* self, int) { log_put('PSIZ'); return self->size; }
static uint8_t __fastcall pad_dynamic(Pad* self, int) { log_put('PDYN'); return self->dynamic; }
static void __fastcall pad_make(Pad* self, int, uint8_t* p) {
    log_put('PMAK'); log_put(off_of(p));
    self->made++;
    memcpy(p, &self->made, self->size < 4 ? self->size : 4);   // within its packet
}
static void __fastcall pad_update(Pad* self, int, const uint8_t* p0, const uint8_t* p1, uint32_t t) {
    log_put('PUPD'); log_put(off_of(p0)); log_put(off_of(p1)); log_put(t);
    uint32_t a, b;
    memcpy(&a, p0, 4);
    memcpy(&b, p1, 4);
    self->sum = self->sum * 31 + a + b * 7;
}

// ---- the world -------------------------------------------------------------------------------------------------------
struct HVertex { P3 pos; float _0c[3]; float u, v; };
struct HSurface { uint8_t _00[0x18]; int16_t first_vertex, end_vertex, first_face, end_face; };
struct HModelInfo { int32_t num_verts; HVertex* verts; int32_t num_surfaces; HSurface* surfaces; };
struct HCarData {                               // phys_car.cpp's CarData, and the GhostCar's +0x1e0
    uint8_t _000[0x18];
    Frame start_frame;
    float body_width;
    uint32_t _04c;
    float track[2];
    float wheelbase, weight_distribution, ride_height[2], torque_max, power, torque_rpm, power_rpm, idle_rpm,
        redline, engine_inertia, engine_drag, fuel_consumption, fuel_capacity, final_drive, gear_ratios[7];
    uint32_t _0b0;
    float torque_balance, diff_stiffness[3];
    uint32_t _0c4;
    float gearbox_drag;
    uint8_t _0cc[0xe4 - 0xcc];
    float sway[2];
    uint8_t _0ec[0x128 - 0xec];
    float cm_height, wheel_lock;
    uint8_t _130[0x16c - 0x130];
    float frontal_area, cp_height, cp_back, drag_coefficient[3], lift[2], spoiler_drag[2];
    int32_t car_index;
    char name[32];
    int32_t lod_models[5], live_models[5];
    int32_t follow_car;                         // +0x1e0 (GhostCar)
};
static_assert(offsetof(HCarData, car_index) == 0x194 && offsetof(HCarData, follow_car) == 0x1e0, "HCarData");

static int32_t* const g_car_count = (int32_t*)0x00520bb4;
static void** const g_car_table = (void**)0x00520c18;
static PhysicsTelemetry* const g_tel = (PhysicsTelemetry*)0x00520c58;

struct World {
    CarL* car;                                  // car A (a Car)
    GhostCarL* ghost;
    HCarData* cd;                               // car A's data
    HCarData* gcd;                              // the ghost's
    PhobDyno* dyno[3];
    int ndyno;
    Pad* pad;
    uint8_t* checkpoint;
    PhobRoot** phobs;
    int nphobs;
    uint8_t* msg;                               // 0x800
    uint8_t* ev;                                // an event's payload, 0x800
    uint8_t* pk[3];                             // packets, 0x80 each
    PhysicsTelemetry* tel_out;
    uint8_t* deity;                             // a RaceDeity (vtable 0x4dc588, its methods stubbed)
    uint8_t* lines;                             // 8 IdealLines of 56 bytes
    uint8_t* record;                            // the ghost record (GhostGetData)
    uint8_t* sound_manager;
    GhostData* gd;                              // a GhostData for NewLap / init_from
    uint8_t* gpk;                               // packets for it (2400 x 60)
    int32_t* count;
    P3* force; P3* point;
    char* status;
};
static World W;

static void rand_rotation(M3* m, float lim) {
    double yaw = rf(-3.1416f, 3.1416f), pitch = rf(-lim, lim), roll = rf(-lim, lim);
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), spp = sin(pitch), cr = cos(roll), sr = sin(roll);
    double R[9] = {cy * cr + sy * spp * sr, cp * sr, -sy * cr + cy * spp * sr, -cy * sr + sy * spp * cr, cp * cr,
                   sy * sr + cy * spp * cr, sy * cp, -spp, cy * cp};
    for (int i = 0; i < 9; i++) m->m[i] = (float)R[i];
}
static void rand_p3(P3* p, float lo, float hi, int spct = 0) { p->x = sp(rf(lo, hi), spct); p->y = sp(rf(lo, hi), spct); p->z = sp(rf(lo, hi), spct); }

static int32_t make_model(const HVertex* src, int n, int first, int end) {
    uint8_t* mi = (uint8_t*)arena_alloc(0x20);
    memset(mi, 0, 0x20);
    HModelInfo* info = (HModelInfo*)arena_alloc(sizeof(HModelInfo));
    HSurface* s = (HSurface*)arena_alloc(0x20);
    memset(s, 0, 0x20);
    HVertex* v = (HVertex*)arena_alloc(n * 32 + 32);
    memcpy(v, src, n * 32);
    s->first_vertex = (int16_t)first;
    s->end_vertex = (int16_t)end;
    info->num_verts = n;
    info->verts = v;
    info->num_surfaces = 1;
    info->surfaces = s;
    *(HModelInfo**)(mi + 0x10) = info;
    return (int32_t)(uintptr_t)mi;
}
static void random_vertex(HVertex* v) {
    v->pos.x = rf(-0.95f, 0.95f);
    v->pos.y = rf(-0.2f, 1.3f);
    v->pos.z = rf(-2.3f, 2.3f);
    for (int k = 0; k < 3; k++) v->_0c[k] = rf(-1, 1);
    v->u = uni();
    v->v = uni();
}
static HCarData* make_cardata(int index) {
    HCarData* cd = (HCarData*)arena_alloc(0x200);
    uint8_t* raw = (uint8_t*)cd;
    for (int i = 0; i < 0x200; i++) raw[i] = (uint8_t)rnd();
    *(uint32_t*)raw = rnd();
    ((float*)raw)[2] = rf(900.0f, 2000.0f);
    for (int i = 3; i < 6; i++) ((float*)raw)[i] = rf(10000.0f, 60000.0f);
    rand_rotation(&cd->start_frame.rot, 0.3f);
    cd->start_frame.pos = {rf(-500, 500), rf(-5, 50), rf(-500, 500)};
    cd->body_width = rf(60, 80);
    for (int r = 0; r < 2; r++) {
        cd->track[r] = rf(55, 65);
        cd->ride_height[r] = rf(3, 7);
        cd->sway[r] = rf(0, 800);
        cd->lift[r] = rf(-0.3f, 0.3f);
        cd->spoiler_drag[r] = rf(0, 0.1f);
    }
    cd->wheelbase = rf(90, 115);
    cd->weight_distribution = rf(40, 60);
    cd->torque_max = rf(200, 500);
    cd->power = rf(200, 500);
    cd->torque_rpm = rf(3000, 5000);
    cd->power_rpm = rf(5000, 7000);
    cd->idle_rpm = rf(700, 1000);
    cd->redline = rf(6000, 8000);
    cd->engine_inertia = rf(1, 5);
    cd->engine_drag = rf(0, 0.01f);
    cd->fuel_consumption = rf(5, 20);
    cd->fuel_capacity = rf(10, 25);
    cd->final_drive = rf(2.5f, 4.5f);
    int ng = ri(1, 6);
    for (int i = 0; i < 7; i++) cd->gear_ratios[i] = i < ng ? rf(0.6f, 3.5f) : 0.0f;
    cd->torque_balance = rf(0, 1);
    for (int i = 0; i < 3; i++) cd->diff_stiffness[i] = rf(0, 50);
    cd->gearbox_drag = rf(0, 0.01f);
    cd->cm_height = rf(15, 22);
    cd->wheel_lock = rf(20, 40);
    cd->frontal_area = rf(18, 25);
    cd->cp_height = rf(0, 20);
    cd->cp_back = rf(20, 60);
    for (int i = 0; i < 3; i++) cd->drag_coefficient[i] = rf(0.3f, 1.2f);
    float* f = (float*)raw;
    for (int off = 0xcc; off < 0x16c; off += 4) if (off < 0xe4 || off >= 0xec) f[off / 4] = rf(0.5f, 5.0f);
    f[0x130 / 4] = rf(220, 320); f[0x134 / 4] = rf(35, 55); f[0x138 / 4] = rf(15, 18);
    f[0x13c / 4] = rf(220, 320); f[0x140 / 4] = rf(35, 55); f[0x144 / 4] = rf(15, 18);
    cd->car_index = index;
    strcpy(cd->name, index & 1 ? "viper" : "ghost");
    int n0 = ri(20, 80);
    HVertex* v0 = (HVertex*)malloc(n0 * 32);
    for (int i = 0; i < n0; i++) random_vertex(&v0[i]);
    for (int lod = 0; lod < 5; lod++) {
        int n = lod == 0 ? n0 : ri(5, n0 / 2 + 5);
        HVertex* v = (HVertex*)malloc((n + 1) * 32);
        for (int i = 0; i < n; i++) {
            if (lod == 0) v[i] = v0[i];
            else if (chance(40)) v[i] = v0[rnd() % n0];
            else random_vertex(&v[i]);
        }
        int first = ri(0, n / 2), end = first + ri(0, n - first);
        cd->lod_models[lod] = make_model(v, n, first, end);
        cd->live_models[lod] = make_model(v, n, first, end);
        free(v);
    }
    free(v0);
    return cd;
}

typedef void*(__fastcall* Ctor2_t)(void*, int, void*, void*);
typedef void(__fastcall* Obj_t0)(void*, int);
typedef void(__fastcall* Pkt_t0)(void*, int, uint8_t*);

static void random_car_state(CarL* c) {
    PhobDyno* d = (PhobDyno*)c;
    if (chance(30)) rand_rotation(&d->frame.rot, 0.3f);
    d->frame.pos.x += rf(-2, 2); d->frame.pos.y += rf(-0.1f, 0.1f); d->frame.pos.z += rf(-2, 2);
    rand_p3(&d->velocity, -60, 60, 2);
    rand_p3(&d->angular_velocity, -3, 3, 2);
    at<float>(c, 0xd70) = sp(rf(-2500, 12500));                // perceived rpm
    at<float>(c, 0x500) = sp(rf(-1.3f, 1.3f));                 // steering
    at<float>(c, 0xde0) = sp(rf(-0.1f, 1.1f));                 // throttle
    at<float>(c, 0xc34) = sp(rf(-0.1f, 1.1f));                 // smoothed throttle
    at<float>(c, 0xde4) = sp(rf(-0.1f, 1.1f));                 // braking
    at<int32_t>(c, 0xd68) = ri(-1, 7);                          // engaged gear
    at<uint8_t>(c, 0xe28) = (uint8_t)(chance(80) ? 0 : 1);     // horn
    at<uint8_t>(c, 0xe3c) = (uint8_t)(chance(90) ? 0 : 1);     // reset event
    at<float>(c, 0x50c) = chance(70) ? 1.0f : rf(0, 1);        // opacity
}
static void random_dyno(PhobDyno* d, uint32_t vt) {
    uint8_t* o = (uint8_t*)d;
    for (int i = 0; i < 0x4b0; i += 4) { float x = rf(-100, 100); memcpy(o + i, &x, 4); }
    d->vtable = (void**)(uintptr_t)vt;
    d->_004 = rnd() & 0x3ff;
    d->_008 = ri(0, 0x100);
    d->num_volumes = 0;
    rand_rotation(&d->frame.rot, 3.1416f);
    rand_p3(&d->frame.pos, -500, 500);
    rand_p3(&d->velocity, -40, 40, 2);
    rand_p3(&d->angular_velocity, -5, 5, 2);
}
static void move_world() {
    for (int i = 0; i < W.ndyno; i++) {
        PhobDyno* d = W.dyno[i];
        if (chance(20)) rand_rotation(&d->frame.rot, 3.1416f);
        d->frame.pos.x += rf(-1, 1); d->frame.pos.y += rf(-1, 1); d->frame.pos.z += rf(-1, 1);
        if (chance(5)) d->frame.pos.x = sp(d->frame.pos.x, 30);
        rand_p3(&d->velocity, -40, 40, 2);
    }
    if (chance(60)) random_car_state(W.car);
    // the telemetry the player's car would have left
    g_tel->dlong = rnd();
    g_tel->lap = ri(-1, 300);
    g_tel->speed = sp(chance(90) ? rf(-50, 280) : rf(-400, 400), 4);
    g_tel->lat_g = sp(chance(90) ? rf(-3, 3) : rf(-6, 6), 4);
    g_tel->long_g = sp(chance(90) ? rf(-3, 3) : rf(-6, 6), 4);
}

static void setup_statics() {
    memcpy(DATA, g_data_pristine, DATA_BYTES_IMG);
    *g_car_count = 8;
    *(uint8_t*)0x00521634 = (uint8_t)chance(80);               // damage on
    *(uint8_t*)0x00521d48 = 3;                                 // Setup's statics' guard
    g_physics_tick = ri(0, 5000);
    g_max_frames = 18750;                                      // $E59
}

static void build_world() {
    memset(g_arena, 0, ARENA_BYTES);            // (all of it: a wild write past the high-water mark mustn't linger)
    g_bump = g_hw = 0;
    setup_statics();
    arena_alloc(0x1000);                        // (a frame slot never recorded, -1, is read at data - 1: in the heap)
    g_databuf = (uint8_t*)arena_alloc(0x200000);
    for (int i = 0; i < 0x200000; i += 4) *(uint32_t*)(g_databuf + i) = rnd();
    g_script.resource_seed = rnd();
    g_script.sound_null_mask = chance(20) ? rnd() : 0;
    g_script.lap = 0;
    g_script.finished = 0;
    for (int i = 0; i < 8; i++) g_script.times[i] = chance(20) ? 0 : ri(0, 100000);
    // the Proxer the Car's Setup looks at (as world_car2)
    uint32_t* proxer = (uint32_t*)arena_alloc(0x200);
    memset(proxer, 0, 0x200);
    proxer[3] = 32;
    proxer[4] = (uint32_t)(uintptr_t)(proxer + 16);
    *(void**)0x004ec480 = proxer;
    // the deity (a RaceDeity whose methods are stubbed), its IdealLines, the sound manager, the ghost record
    W.deity = (uint8_t*)arena_alloc(2012);
    memset(W.deity, 0, 2012);
    *(uint32_t*)W.deity = VT_RACEDEITY;
    W.lines = (uint8_t*)arena_alloc(8 * 56);
    for (int i = 0; i < 8 * 56; i++) W.lines[i] = (uint8_t)rnd();
    for (int i = 0; i < 8; i++)
        if (chance(97)) *(uint8_t**)(W.deity + 0xa4 + 0x74 * i) = W.lines + 56 * i;   // (none: reset_to_head faults)
    g_deity = W.deity;
    g_race_deity = W.deity;
    W.sound_manager = (uint8_t*)arena_alloc(0x40);
    memset(W.sound_manager, 0, 0x40);
    {
        int n = ri(0, 6);
        void** list = (void**)arena_alloc(4 * 8);
        for (int i = 0; i < n; i++) {
            uint8_t* s = (uint8_t*)arena_alloc(0x40);
            memset(s, 0, 0x40);
            *(int32_t*)(s + 0x10) = ri(0, 7);
            list[i] = s;
        }
        *(void***)(W.sound_manager + 4) = list;
        *(int32_t*)(W.sound_manager + 8) = n;
    }
    g_sound_manager = W.sound_manager;
    W.msg = (uint8_t*)arena_alloc(0x800);
    W.ev = (uint8_t*)arena_alloc(0x800);
    for (int i = 0; i < 3; i++) W.pk[i] = (uint8_t*)arena_alloc(0x80);
    W.tel_out = (PhysicsTelemetry*)arena_alloc(32);
    W.gd = (GhostData*)arena_alloc(16);
    W.count = (int32_t*)arena_alloc(16);
    W.gpk = (uint8_t*)arena_alloc(2400 * 60 + 64);
    W.force = (P3*)arena_alloc(16);
    W.point = (P3*)arena_alloc(16);
    W.status = (char*)arena_alloc(64);
    // car A and the ghost, by the original constructors
    int ia = ri(0, 7), ig = (ia + ri(1, 7)) & 7;
    W.cd = make_cardata(ia);
    W.gcd = make_cardata(ig);
    W.gcd->follow_car = ia;
    W.car = (CarL*)arena_alloc(4224);
    W.ghost = (GhostCarL*)arena_alloc(4224);
    g_pass = 0;
    ((Ctor2_t)0x004364c0)(W.car, 0, W.cd, (void*)0x100);
    g_car_table[ia] = W.car;
    // the ghost record GhostGetData hands out: packets made by the original from car A's states
    W.record = (uint8_t*)arena_alloc(0x23458);
    memset(W.record, 0, 0x23458);
    g_ghost_record = chance(85) ? W.record : 0;
    W.record[1] = (uint8_t)chance(80);
    *(int32_t*)(W.record + 4) = ri(-3, 12);
    GhostData* rgd = (GhostData*)(W.record + 0x2344c);
    rgd->count = chance(10) ? 0 : ri(1, 300);
    rgd->lap_ticks = chance(10) ? ri(-10, 10) : ri(200, 6000);
    rgd->time_offset = chance(30) ? 0 : ri(-50, 400);
    for (int i = 0; i < rgd->count; i++) {
        random_car_state(W.car);
        ((Pkt_t0)0x00438cd0)(W.car, 0, W.record + 0xc + 60 * i);
    }
    ((Ctor2_t)0x0042dde0)(W.ghost, 0, W.gcd, (void*)0x300);
    g_car_table[ig] = W.ghost;
    random_car_state(W.car);
    // the other phobs
    static const uint32_t dyno_vt[3] = {0x004dc690, 0x004dc3b0, 0x004dc0c8};   // PhobDyno, Ball, Obstacle
    W.ndyno = ri(1, 3);
    for (int i = 0; i < W.ndyno; i++) {
        W.dyno[i] = (PhobDyno*)arena_alloc(0x4b0);
        random_dyno(W.dyno[i], dyno_vt[rnd() % 3]);
    }
    W.pad = (Pad*)arena_alloc(PAD_SIZE);
    memset(W.pad, 0, PAD_SIZE);
    W.pad->vtable = g_pad_vt;
    static const int32_t pad_sizes[] = {0, 4, 0x40, 0x3000, 0x8000, 0x8000, 0x10000, 0x10000};
    W.pad->size = pad_sizes[rnd() % 8];
    W.pad->dynamic = (uint8_t)chance(80);
    W.checkpoint = (uint8_t*)arena_alloc(132);
    memset(W.checkpoint, 0, 132);
    *(uint32_t*)W.checkpoint = 0x004dc350;
    // the phob list in a random order (the null entry: PlayUpdate skips it; RecordUpdate would crash on it)
    W.phobs = (PhobRoot**)arena_alloc(16 * 4);
    int n = 0;
    W.phobs[n++] = (PhobRoot*)W.car;
    W.phobs[n++] = (PhobRoot*)W.ghost;
    for (int i = 0; i < W.ndyno; i++) W.phobs[n++] = (PhobRoot*)W.dyno[i];
    W.phobs[n++] = (PhobRoot*)W.pad;
    W.phobs[n++] = (PhobRoot*)W.checkpoint;
    for (int i = n - 1; i > 0; i--) {
        int j = ri(0, i);
        PhobRoot* t = W.phobs[i]; W.phobs[i] = W.phobs[j]; W.phobs[j] = t;
    }
    W.nphobs = n;
}

// ---- running one call both ways ----------------------------------------------------------------------------------------
template <typename F, typename... Args> static uint64_t invoke(F f, Args... a) {
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
struct Stat { const char* name; long calls, changed, fails, fp_fails, faults, replay_only; bool main; };
static Stat g_stats[96];
static int g_nstats;
static Stat& stat(const char* name) {
    for (int i = 0; i < g_nstats; i++)
        if (!strcmp(g_stats[i].name, name)) return g_stats[i];
    g_stats[g_nstats] = {name, 0, 0, 0, 0, 0, 0, false};
    return g_stats[g_nstats++];
}
static Footprint g_fp;
static int g_world;
static unsigned g_pc;
enum Thread { PHYS, MAIN };

template <typename Run> struct Guard {
    Run* run; bool orig; uint64_t ret;
    static void call(void* c) { Guard* g = (Guard*)c; g->ret = (*g->run)(g->orig); }
};
static uint32_t g_fault_code, g_fault_addr, g_fault_data;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_addr = (uint32_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress;
    g_fault_data = e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static int guarded(void (*fn)(void*), void* ctx) {
    __try { fn(ctx); } __except (fault_filter(GetExceptionInformation())) { _fpreset(); return 1; }
    return 0;
}
static void reset_pass(int pass) {
    g_pass = pass;
    g_nlog[pass] = 0;
    g_time_i = 0;
    g_sound_calls = 0;
    unsigned cw;
    _controlfp_s(&cw, g_pc, _MCW_PC);
}
static const char* where(const uint8_t* p, char* buf) {
    struct { const void* p; uint32_t n; const char* name; } parts[] = {
        {W.car, 3768, "car"}, {W.ghost, 3840, "ghost"}, {W.pad, PAD_SIZE, "pad"}, {W.deity, 2012, "deity"},
        {W.msg, 0x800, "msg"}, {W.record, 0x23458, "record"}, {W.gpk, 2400 * 60, "gpk"}, {g_data, 0x200000, "replay data"},
        {g_frames, (uint32_t)(g_nframes > 0 ? g_nframes * 4 : 0), "frame table"}};
    for (auto& q : parts)
        if (q.p && p >= (const uint8_t*)q.p && p < (const uint8_t*)q.p + q.n) {
            sprintf(buf, "%s+0x%x", q.name, (unsigned)(p - (const uint8_t*)q.p));
            return buf;
        }
    for (int i = 0; i < W.ndyno; i++)
        if (p >= (uint8_t*)W.dyno[i] && p < (uint8_t*)W.dyno[i] + 0x4b0) {
            sprintf(buf, "dyno%d+0x%x", i, (unsigned)(p - (uint8_t*)W.dyno[i]));
            return buf;
        }
    if (p >= DATA && p < DATA + DATA_BYTES_IMG) sprintf(buf, "global 0x%08x", (unsigned)(uintptr_t)p);
    else sprintf(buf, "arena+0x%x", (unsigned)(p - g_arena));
    return buf;
}
static bool in_fp(const uint8_t* p) {
    for (int k = 0; k < g_fp.n; k++)
        if (p >= (uint8_t*)g_fp.r[k].p && p < (uint8_t*)g_fp.r[k].p + g_fp.r[k].n) return true;
    return false;
}
// the first changed byte (start -> after) in [0, n) outside the footprint; -1 if none
static long outside_fp(const uint8_t* start, const uint8_t* after, uint32_t n, uint8_t* live, bool globals, Thread th) {
    for (uint32_t i = 0; i < n; i += 64) {
        uint32_t m = n - i < 64 ? n - i : 64;
        if (!memcmp(start + i, after + i, m)) continue;
        for (uint32_t j = i; j < i + m; j++) {
            if (start[j] == after[j]) continue;
            if (in_fp(live + j)) continue;
            if (globals && th == PHYS && in_physics_globals((uint32_t)(uintptr_t)(live + j))) continue;
            return (long)j;
        }
    }
    return -1;
}
static int g_first_bad_world = -1;

template <typename Run, typename Fp> static uint64_t check(const char* name, Thread th, Run run, Fp footprint) {
    Stat& st = stat(name);
    st.calls++;
    st.main = th == MAIN;
    const uint32_t bump0 = g_bump, hw0 = g_hw;
    memcpy(g_arena_start, g_arena, hw0);
    memcpy(g_data_start, DATA, DATA_BYTES_IMG);
    g_fs_start = g_fs;
    g_fp.n = 0;
    g_fp.replay_only = 0;
    g_fp.pure = false;
    struct FpRun { Fp* fp; static void call(void* c) { (*((FpRun*)c)->fp)(g_fp); } } fr = {&footprint};
    if (guarded(&FpRun::call, &fr)) {
        printf("FOOTPRINT %s (world %d): the footprint function faulted\n", name, g_world);
        st.fp_fails++;
        g_fp.replay_only = "faulted";
    }
    g_pc = th == MAIN && chance(33) ? _PC_53 : _PC_24;
    // the original
    reset_pass(0);
    Guard<Run> go = {&run, true, 0};
    int fo = guarded(&Guard<Run>::call, &go);
    if (fo && st.faults < 2 && getenv("VP_FAULTS"))
        printf("FAULT %s (world %d): code %08x at %08x, address %08x; cur %d nframes %d\n", name, g_world, g_fault_code,
               g_fault_addr, g_fault_data, g_cur_frame, g_nframes);
    const uint32_t bump_o = g_bump, hw_o = g_hw;
    memcpy(g_arena_after, g_arena, hw_o);
    memcpy(g_data_after, DATA, DATA_BYTES_IMG);
    g_fs_after = g_fs;
    if (g_fs.len > g_fs_start.len) memcpy(g_file_after, g_file + g_fs_start.len, g_fs.len - g_fs_start.len);
    // back to the start; the rewrite
    memcpy(g_arena, g_arena_start, hw0);
    if (hw_o > hw0) memset(g_arena + hw0, 0, hw_o - hw0);
    memcpy(DATA, g_data_start, DATA_BYTES_IMG);
    g_bump = bump0;
    g_hw = hw0;
    g_fs = g_fs_start;
    reset_pass(1);
    Guard<Run> gn = {&run, false, 0};
    int fn = guarded(&Guard<Run>::call, &gn);
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    const uint32_t hw_n = g_hw > hw_o ? g_hw : hw_o;
    if (fo || fn) st.faults++;
    if (g_fp.replay_only) st.replay_only++;
    if (memcmp(g_arena_after, g_arena_start, hw0) || hw_o != hw0 || memcmp(g_data_after, g_data_start, DATA_BYTES_IMG))
        st.changed++;
    const int nl = g_nlog[0] < LOG_MAX ? g_nlog[0] : LOG_MAX;
    const bool file_bad = g_fs.len != g_fs_after.len || g_fs.rpos != g_fs_after.rpos ||
                          (g_fs.len > g_fs_start.len && memcmp(g_file_after, g_file + g_fs_start.len, g_fs.len - g_fs_start.len));
    if (hw_n > hw_o) memset(g_arena_after + hw_o, 0, hw_n - hw_o);
    const bool bad = fo != fn || (!fo && (go.ret != gn.ret || g_bump != bump_o || memcmp(g_arena_after, g_arena, hw_n) ||
                                          memcmp(g_data_after, DATA, DATA_BYTES_IMG) || g_nlog[0] != g_nlog[1] ||
                                          memcmp(g_log[0], g_log[1], 4 * nl) || file_bad));
    if (bad) {
        if (g_first_bad_world < 0) g_first_bad_world = g_world;
        if (st.fails++ < 4) {
            char buf[96];
            printf("MISMATCH %s (world %d, pc %s)\n", name, g_world, g_pc == _PC_24 ? "24" : "53");
            if (fo != fn) printf("  original %s, rewrite %s\n", fo ? "faulted" : "ran", fn ? "faulted" : "ran");
            if (go.ret != gn.ret) printf("  return: original %016llx, rewrite %016llx\n", go.ret, gn.ret);
            if (g_bump != bump_o) printf("  allocated: original to %x, rewrite to %x\n", bump_o, g_bump);
            if (file_bad) printf("  the file differs\n");
            int shown = 0;
            for (uint32_t i = 0; i < hw_n && shown < 16; i += 4)
                if (memcmp(g_arena_after + i, g_arena + i, 4)) {
                    uint32_t a, b;
                    memcpy(&a, g_arena_after + i, 4); memcpy(&b, g_arena + i, 4);
                    printf("  %s: original %08x (%.9g), rewrite %08x (%.9g)\n", where(g_arena + i, buf), a, Fbits(a), b, Fbits(b));
                    shown++;
                }
            for (uint32_t i = 0; i < DATA_BYTES_IMG && shown < 24; i += 4)
                if (memcmp(g_data_after + i, DATA + i, 4)) {
                    uint32_t a, b;
                    memcpy(&a, g_data_after + i, 4); memcpy(&b, DATA + i, 4);
                    printf("  global 0x%08x: original %08x, rewrite %08x\n", (unsigned)(uintptr_t)(DATA + i), a, b);
                    shown++;
                }
            if (g_nlog[0] != g_nlog[1] || memcmp(g_log[0], g_log[1], 4 * nl)) {
                printf("  stub logs: %d / %d entries\n", g_nlog[0], g_nlog[1]);
                int m = g_nlog[0] > g_nlog[1] ? g_nlog[0] : g_nlog[1];
                for (int i = 0, k = 0; i < m && i < LOG_MAX && k < 12; i++)
                    if (i >= g_nlog[0] || i >= g_nlog[1] || g_log[0][i] != g_log[1][i]) {
                        printf("    [%d] %08x / %08x\n", i, i < g_nlog[0] ? g_log[0][i] : 0, i < g_nlog[1] ? g_log[1][i] : 0);
                        k++;
                    }
            }
        }
    }
    // the footprint must cover every byte the original changed
    if (!g_fp.replay_only && !fo) {
        long j = outside_fp(g_arena_start, g_arena_after, hw0, g_arena, false, th);
        bool data = false;
        if (j < 0) { j = outside_fp(g_data_start, g_data_after, DATA_BYTES_IMG, DATA, true, th); data = true; }
        if (j >= 0 && st.fp_fails++ < 4) {
            char buf[96];
            printf("FOOTPRINT %s (world %d): %s changed outside it\n", name, g_world, where((data ? DATA : g_arena) + j, buf));
            if (getenv("VP_FP_DUMP"))
                for (int k = 0; k < g_fp.n; k++)
                    printf("    %s: %s, %u bytes\n", g_fp.r[k].what, where((const uint8_t*)g_fp.r[k].p, buf), g_fp.r[k].n);
        }
    }
    // go on from the original's result
    memcpy(g_arena, g_arena_after, hw_o);
    if (hw_n > hw_o) memset(g_arena + hw_o, 0, hw_n - hw_o);
    memcpy(DATA, g_data_after, DATA_BYTES_IMG);
    g_bump = bump_o;
    g_hw = hw_o;
    g_fs = g_fs_after;
    if (g_fs.len > g_fs_start.len) memcpy(g_file + g_fs_start.len, g_file_after, g_fs.len - g_fs_start.len);
    return go.ret;
}

#define CHECK(TH, FN, ...)                                                                                      [&]() {                                                                                                         auto args_ = std::make_tuple(__VA_ARGS__);                                                                  return check(VP_CAT(name_, FN), TH,                                                                                      [&](bool orig) {                                                                                                auto f = orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN;                                          return std::apply([&](auto... a) { return invoke(f, a...); }, args_);                                   },                                                                                                          [&](Footprint& fp) { std::apply([&](auto... a) { VP_CAT(fpof_, FN)(fp, a...); }, args_); });     }()
#define CHECK0(TH, FN)                                                                                          check(VP_CAT(name_, FN), TH,                                                                                      [&](bool orig) {                                                                                                auto f = orig ? (decltype(&FN))(uintptr_t)VP_CAT(addr_, FN) : &FN;                                          return invoke(f);                                                                                       },                                                                                                          [&](Footprint& fp) { VP_CAT(fpof_, FN)(fp); })

// ---- coverage -------------------------------------------------------------------------------------------------------------
static long c_frames, c_index_wrap, c_data_wrap, c_carry_on, c_drop, c_drop_log, c_events, c_dispatch[8], c_at_end,
    c_sealed_wrapped, c_sealed_plain, c_seal_skipped, c_load_ok, c_load_fail[8], c_ghost_init, c_ghost_visible,
    c_ghost_follow, c_ghost_neg, c_ghost_hidden, c_feed_cut, c_feed_plain, c_newlap_taken, c_sound_stop, c_modes[3],
    c_tel_ok, c_tel_out, c_worlds, c_path[4], c_ghost_early;

// ---- one world ----------------------------------------------------------------------------------------------------------
static bool seal_would_end() {
    if (!g_wrapped) return true;
    const int32_t cur = g_cur_frame;
    const int32_t end = g_frames[cur] + g_ev_cursor + 1;      // after the separator
    const int32_t n = g_nframes;
    int32_t k = (cur - 1 + n) % n;
    for (int it = 0; it < 4 * n + 8; it++) {
        const int32_t e = (k - 1 + n) % n;
        if (g_frames[e] < end && g_frames[k] >= end) return true;
        k = e;
    }
    return false;
}
static void random_event(int32_t* type, int32_t* size) {
    uint8_t* e = W.ev;
    for (int i = 0; i < 0x100; i++) e[i] = (uint8_t)rnd();
    const int32_t ia = at<int32_t>(W.car, C_CAR_INDEX), ig = at<int32_t>(W.ghost, C_CAR_INDEX);
    switch (rnd() % 10) {
    case 0: *type = 1; *size = 0x14; break;                                   // crash sound
    case 1: *type = 3; *size = 0x1c; break;                                   // splash
    case 2: *type = 4; *size = 0x2c; *(int32_t*)e = chance(97) ? ri(0, 1) : 2; break;   // AI (2: LogPanic)
    case 3: case 4:                                                          // damage: 3 x 8-bit point, 3 x 16-bit impulse
        *type = 5; *size = 14;
        e[13] = (uint8_t)(chance(80) ? ia : ig);
        break;
    case 5: *type = 7; *size = 2; e[0] = (uint8_t)(chance(80) ? ia : ig); e[1] = 0; break;
    case 6: *type = 6; *size = ri(2, 9); memcpy(e, size, 2); break;         // the unknown handler
    case 7: *type = ri(0, 2) == 0 ? 2 : 0; *size = ri(0, 8); break;          // no handler: ends the frame's dispatch
    default:                                                                 // big ones reach the 0x600 limit
        if (chance(85)) { *type = 1; *size = 0x14; }
        else { *type = 6; *size = ri(0x100, 0x700); memcpy(e, size, 2); }   // (a handler's size always matches)
        break;
    }
}

static int32_t g_rec_start;
static void record(int ticks) {
    g_rec_start = g_physics_tick;
    PhobRoot** phobs = W.phobs;
    const int n = W.nphobs;
    for (int t = 0; t < ticks; t++) {
        g_physics_tick = g_physics_tick + (chance(94) ? 1 : ri(2, 40));
        move_world();
        if (chance(3)) g_script.lap++;
        g_script.finished = (uint8_t)chance(2);
        // events, only once a frame exists (before that AddEvent reads frame_idx[-1])
        if (g_cur_frame != -1) {
            int ne = chance(50) ? 0 : ri(1, 4);
            for (int k = 0; k < ne; k++) {
                int32_t type, size;
                random_event(&type, &size);
                const int32_t before = g_ev_cursor;
                const int32_t logged0 = g_last_drop_log;
                add_event_real(true);
                CHECK(PHYS, PhysReplayAddEvent_rw, type, (const void*)W.ev, size);
                add_event_real(false);
                if (g_ev_cursor == before) { c_drop++; } else c_events++;
                if (g_last_drop_log != logged0) c_drop_log++;
            }
        }
        const int32_t cur0 = g_cur_frame, wraps0 = g_wraps, mark0 = g_wrap_mark;
        {                                                               // which way advance_replay_frame will go
            const int32_t f = (g_physics_tick / 6) % g_nframes;
            if (f != g_cur_frame && g_cur_frame != -1) {
                const int32_t next = g_frames[g_cur_frame] + g_ev_cursor + 1;
                const bool room = next + 0x600 < 0x200000, bl = f == 0;
                c_path[!bl && room ? 0 : !room ? 1 : g_wrap_mark == g_wraps ? 2 : 3]++;
            }
        }
        CHECK(PHYS, PhysReplayRecordUpdate_rw, phobs, n);
        if (g_cur_frame != cur0) {
            c_frames++;
            if (g_wraps != wraps0) c_index_wrap++;
            if (g_wrap_mark != mark0) c_data_wrap++;
            if (cur0 != -1 && g_cur_frame == 0 && g_frames[0] != 0) c_carry_on++;
        }
        // the ghost, as the physics task runs it
        if (chance(85)) {
            const uint32_t fl0 = flags(W.ghost);
            if ((fl0 & 1) && g_script.lap == W.ghost->last_lap &&
                g_physics_tick - W.ghost->lap_start - W.ghost->time_offset < 0) c_ghost_early++;
            const int32_t lap0 = W.ghost->last_lap;
            CHECK(PHYS, GhostCar_Update, W.ghost, 0);
            if (W.ghost->last_lap != lap0 && W.ghost->count) c_ghost_init++;
            if (flags(W.ghost) & 1) c_ghost_visible++;
            if ((fl0 & 1) && !(flags(W.ghost) & 1)) c_ghost_hidden++;
            if (!(flags(W.ghost) & 1)) c_ghost_follow++;
            const uint32_t pf = W.ghost->prev_flags;
            CHECK(PHYS, GhostCar_UpdateCommon, W.ghost, 0);
            if ((pf & 2) && !(flags(W.ghost) & 2)) c_sound_stop++;
            if (chance(30)) CHECK(PHYS, GhostCar_GetMessage, W.ghost, 0, W.msg);
        }
        // a best lap: its packets out of the ring into the record, offered to the ghost
        if (chance(4) && g_cur_frame != -1) {
            const int32_t ia = at<int32_t>(W.car, C_CAR_INDEX);
            GhostData* rgd = (GhostData*)(W.record + 0x2344c);
            rgd->count = chance(80) ? 2400 : ri(0, 50);
            // a lap no longer than the race so far (as GhostNewBestLap sees it)
            const int32_t so_far = g_physics_tick - g_rec_start;
            int32_t lap = chance(70) ? ri(6, 6 * g_nframes + 60) : ri(0, 12 * g_nframes + 100);
            if (lap > so_far) lap = so_far > 0 ? ri(0, so_far) : 0;
            rgd->lap_ticks = lap;
            rgd->time_offset = chance(50) || lap < 2 ? 0 : ri(0, lap / 2);   // (a negative duration: a negative count)
            const int32_t dur = lap - rgd->time_offset;
            const int32_t start = rgd->time_offset + (g_physics_tick - lap);
            const int32_t r = (int32_t)CHECK(PHYS, feed_ghost_car_rw, ia, start, dur, W.record + 0xc, &rgd->count);
            if (r) c_feed_cut++; else c_feed_plain++;
            rgd->time_offset += r;
            const int32_t best0 = W.ghost->best_ticks;
            CHECK(PHYS, GhostCar_NewLap, (const GhostData*)rgd, (const uint8_t*)(W.record + 0xc), ri(0, 9));
            if (W.ghost->best_ticks != best0) c_newlap_taken++;
        }
    }
}

static void play(int steps) {
    PhobRoot** phobs = W.phobs;
    const int n = W.nphobs;
    for (int s = 0; s < steps; s++) {
        const int k = (int)(rnd() % 100);
        if (k < 55) {
            // the physics clock runs on unless paused (the game never steps it back: the UI moves it)
            if (!g_physics_paused) g_physics_tick = g_physics_tick + (chance(95) ? 1 : ri(2, 12));
            c_modes[(uint32_t)g_event_mode < 3 ? g_event_mode : 0]++;
            const uint8_t paused0 = g_physics_paused;
            CHECK(PHYS, PhysReplayPlayUpdate_rw, phobs, chance(97) ? n : 0);
            if (!paused0 && g_physics_paused) c_at_end++;
            if (chance(30)) CHECK(PHYS, GhostCar_Update, W.ghost, 0);
        } else if (k < 60) CHECK0(MAIN, PhysReplayRewind_rw);
        else if (k < 63) CHECK0(MAIN, PhysReplayFastForward_rw);
        else if (k < 70) {
            float sh = chance(90) ? rf(-4, 4) : chance(50) ? rf(-400, 400) : special();
            CHECK(MAIN, PhysReplayShuttle_rw, Ubits(sh));
        } else if (k < 74) CHECK0(MAIN, PhysReplayPause_rw);
        else if (k < 78) CHECK0(MAIN, PhysReplayResume_rw);
        else if (k < 80) CHECK0(MAIN, PhysReplayIsAtEnd_rw);
        else if (k < 82) CHECK0(MAIN, PhysReplayIsPaused_rw);
        else if (k < 83) CHECK0(MAIN, PhysReplayPlayMode_rw);
        else if (k < 90) {
            const float lo = (float)g_start_time / 62.5f, hi = (float)(g_start_time + 6 * (g_num_frames - 2)) / 62.5f;
            float t = chance(85) ? rf(lo - 0.2f, hi + 0.2f) : chance(50) ? rf(-100, 1000) : special();
            const uint8_t ok = (uint8_t)CHECK(MAIN, PhysReplayGetTelemetry_rw, W.tel_out, Ubits(t));
            if (ok) c_tel_ok++; else c_tel_out++;
        } else if (k < 93) CHECK(PHYS, convert_phystime2frame_rw, (int32_t)(chance(80) ? g_physics_tick : ri(0, 0x7fffffff)));
        else if (k < 96) CHECK(PHYS, next_replay_frame_rw, ri(-5, g_nframes + 5), ri(-3, 3 * g_nframes));
        else if (k < 98) CHECK(PHYS, prev_replay_frame_rw, ri(-5, g_nframes + 5), ri(-3, 3 * g_nframes));
        else {
            CHECK(PHYS, convert_phystime2deltat_rw, (int32_t)rnd());
            CHECK(PHYS, convert_frame2phystime_rw, (int32_t)rnd());
            const int32_t fa = g_frames[ri(0, g_nframes - 1)], fb = g_frames[ri(0, g_nframes - 1)];
            if (fa >= 0 && fb >= 0)
                CHECK(PHYS, update_replay_rw, (const ReplayFrame*)(g_data + fa), (const ReplayFrame*)(g_data + fb), Ubits(rf(0, 1)));
        }
    }
}

static void setup_replay() {
    CHECK0(PHYS, PhysReplayBGBegin_rw);
    for (int i = 0; i < W.nphobs; i++) CHECK(PHYS, PhysReplayRegisterPhob_rw, W.phobs[i]);
    CHECK0(PHYS, PhysReplayDoneCreating_rw);
}
static void install_handlers() {
    static const struct { int32_t type; uint32_t fn; } h[] = {
        {1, H_CRASH}, {3, H_SPLASH}, {4, H_AI}, {5, H_DAMAGE}, {7, H_RESET}};
    for (auto& x : h)
        if (chance(90)) CHECK(PHYS, PhysReplayInstallEventHandler_rw, x.type, (void*)(uintptr_t)x.fn);
    if (chance(50)) CHECK(PHYS, PhysReplayInstallEventHandler_rw, 6, (void*)&stub_handler6);
    if (chance(10)) CHECK(PHYS, PhysReplayInstallEventHandler_rw, 1, (void*)(uintptr_t)H_SPLASH);   // taken: asserts
}

static void ghost_misc() {
    GhostCarL* g = W.ghost;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 0x80; j++) W.pk[i][j] = (uint8_t)rnd();
    if (chance(60)) {                                                       // packets the original makes
        random_car_state(W.car);
        ((Pkt_t0)0x00438cd0)(W.car, 0, W.pk[0]);
        memcpy(W.pk[1], W.pk[0], 0x3c);
        for (int i = 0; i < 6; i++) W.pk[1][rnd() % 0x3c] = (uint8_t)rnd();
        W.pk[0][0x3a] = chance(50) ? 0 : W.pk[0][0x3a];
    }
    const float t = chance(80) ? uni() : special();
    switch (rnd() % 12) {
    case 0: CHECK(PHYS, GhostCar_UpdateGhost, g, 0, (const uint8_t*)W.pk[0], (const uint8_t*)W.pk[1], Ubits(t)); break;
    case 1: CHECK(PHYS, GhostCar_UpdateReplay, g, 0, (const uint8_t*)W.pk[0], (const uint8_t*)W.pk[1], Ubits(t)); break;
    case 2: CHECK(PHYS, GhostCar_MakeReplayPacket, g, 0, W.pk[2]); break;
    case 3:                                                                 // Car::Reset adds an event: a sane frame
        if (g_frames && g_nframes > 0 && g_cur_frame >= 0 && g_cur_frame < g_nframes && g_frames[g_cur_frame] >= 0 &&
            g_frames[g_cur_frame] < 0x200000 - 0x700 && g_ev_cursor >= 0 && g_ev_cursor < 0x600)
            CHECK(PHYS, GhostCar_Reset, g, 0);
        break;
    case 4: CHECK(PHYS, GhostCar_reset, g, 0); break;
    case 5: {
        W.gd->lap_ticks = ri(-10, 8000); W.gd->time_offset = ri(-100, 500); W.gd->count = ri(0, 2400);
        for (int i = 0; i < W.gd->count * 60; i += 4) *(uint32_t*)(W.gpk + i) = rnd();
        CHECK(PHYS, GhostCar_init_from, g, 0, (const GhostData*)W.gd, (const uint8_t*)W.gpk, ri(-2, 10));
        break;
    }
    case 6:
        W.gd->lap_ticks = chance(50) ? g->best_ticks + ri(-3, 3) : ri(-10, 8000);
        CHECK(PHYS, GhostCar_new_lap, g, 0, (const GhostData*)W.gd, (const uint8_t*)W.gpk, ri(-2, 10));
        break;
    case 7: {
        const int32_t time = chance(80) ? ri(0, 6 * g->count + 12) : ri(-20, 0x7fffff);
        CHECK(PHYS, update_ghost_car_rw, g, g->packets, time, chance(80) ? g->count : ri(-1, 2400));
        if (time < 0) c_ghost_neg++;
        break;
    }
    case 8:
        flags(g) = rnd() & 0x3ff;
        g->prev_flags = rnd() & 0x3ff;
        CHECK(PHYS, GhostCar_UpdateCommon, g, 0);
        break;
    case 9: {
        CarL* c = chance(50) ? W.car : (CarL*)g;
        rand_p3(W.force, -20000, 20000);
        W.point->x = rf(-1.2f, 1.2f); W.point->y = rf(-0.5f, 1.8f); W.point->z = rf(-2.6f, 2.6f);
        CHECK(PHYS, Car_ApplyDamage, c, 0, (const P3*)W.force, (const P3*)W.point, rnd());
        break;
    }
    case 10: {
        CarL* c = chance(50) ? W.car : (CarL*)g;
        CHECK(PHYS, Car_GetPerceivedThrottle, c, 0);
        CHECK(PHYS, Car_GetPerceivedRPM, c, 0);
        CHECK(PHYS, Car_DoneLowering, c, 0);
        CHECK(PHYS, PhobDyno_GetDyno, (PhobRoot*)c, 0);
        CHECK(PHYS, PhobDyno_IsDynamic, (PhobRoot*)c, 0);
        CHECK(PHYS, PhobRoot_MakeStatusString, (PhobRoot*)c, 0, W.status);
        CHECK(PHYS, GhostCar_GetReplayPacketSize, (PhobRoot*)g, 0);
        CHECK(PHYS, GhostCar_GetMessageSize, (PhobRoot*)g, 0);
        CHECK(PHYS, GhostCar_IsSolid, (PhobRoot*)g, 0);
        break;
    }
    default: CHECK(PHYS, GhostCar_GetMessage, g, 0, W.msg); break;
    }
}

static void run_world() {
    c_worlds++;
    build_world();
    // the replay's lifetime: set up; the handlers
    setup_replay();
    const int32_t cap_choice = (int32_t)(rnd() % 4);
    install_handlers();
    // a smaller frame cap makes the ring wrap (it's read by DoneCreating: set it and set up again)
    if (cap_choice) {
        CHECK0(PHYS, PhysReplayBGEnd_rw);
        g_max_frames = cap_choice == 1 ? ri(2, 12) : ri(13, 90);
        setup_replay();
        install_handlers();
    }
    g_fs.len = g_fs.rpos = 0;
    // the race
    const int nf = g_nframes;
    int ticks = chance(70) ? ri(nf * 6 / 2, nf * 6 * 3 + 30) : ri(10, 300);
    if (ticks > 700) ticks = ri(200, 700);
    record(ticks);
    // a few ghost / stub calls while the buffer is live
    for (int i = 0; i < 12; i++) ghost_misc();
    // the seal
    if (!seal_would_end()) { c_seal_skipped++; return; }
    const bool wrapped = g_wrapped != 0;
    CHECK0(PHYS, PhysReplaySealRecording_rw);
    if (wrapped) c_sealed_wrapped++; else c_sealed_plain++;
    // the replay screen
    CHECK0(MAIN, PhysReplayPlayBegin_rw);
    play(ri(40, 250));
    for (int i = 0; i < 6; i++) ghost_misc();
    // save; end; a new buffer; load (maybe damaged); play again
    g_fs.len = g_fs.rpos = 0;
    CHECK(MAIN, PhysReplaySave_rw, 7);
    CHECK0(MAIN, PhysReplayPlayEnd_rw);
    CHECK0(PHYS, PhysReplayBGEnd_rw);
    if (chance(15)) g_max_frames = ri(2, 200);                              // a different buffer: nframes may differ
    setup_replay();
    install_handlers();
    int damage = chance(65) ? 0 : ri(1, 7);
    switch (damage) {
    case 1: *(uint32_t*)(g_file + 0) ^= 1u << ri(0, 31); break;             // cookie
    case 2: *(int32_t*)(g_file + 4) = ri(0, 9); break;                     // version
    case 3: *(int32_t*)(g_file + 20) += ri(-2, 2); break;                  // nframes
    case 4: *(int32_t*)(g_file + 24) = 0x200000 + ri(1, 100); break;        // data size
    case 5: *(int32_t*)(g_file + 12) = chance(50) ? ri(-5, -1) : g_nframes + ri(1, 5); break;   // frames
    case 6: *(int32_t*)(g_file + 16) += ri(-2, 2); break;                  // frame size
    case 7: g_fs.len = ri(0, (int)g_fs.len - 1 > 0 ? (int)g_fs.len - 1 : 0); break;   // cut short
    }
    g_fs.rpos = 0;
    const uint8_t ok = (uint8_t)CHECK(MAIN, PhysReplayLoad_rw, 7);
    if (ok) c_load_ok++; else c_load_fail[damage]++;
    if (ok) {
        CHECK0(MAIN, PhysReplayPlayBegin_rw);
        play(ri(20, 120));
    }
    // handlers out (one that isn't there: LogPanic)
    static const uint32_t hs[] = {H_CRASH, H_SPLASH, H_AI, H_DAMAGE, H_RESET, 0x12345678};
    for (uint32_t h : hs)
        if (chance(70)) CHECK(PHYS, PhysReplayUninstallEventHandler_rw, (void*)(uintptr_t)h);
    // the ghost's end and a new one
    if (chance(50)) {
        CHECK(PHYS, GhostCar_dtor, W.ghost, 0);
        GhostCarL* g2 = (GhostCarL*)arena_alloc(4224);
        for (int i = 0; i < 4224; i++) ((uint8_t*)g2)[i] = (uint8_t)rnd();
        CHECK(PHYS, GhostCar_ctor, g2, 0, (void*)W.gcd, (void*)0x300);
        W.ghost = g2;
        for (int i = 0; i < 4; i++) ghost_misc();
    }
    CHECK0(PHYS, PhysReplayBGEnd_rw);
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_FUZZ_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int worlds = argc > 1 ? atoi(argv[1]) : 40;
    if (argc > 2) g_state = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_replay.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_arena_start = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_arena_after = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_data_pristine = (uint8_t*)malloc(DATA_BYTES_IMG);
    g_data_start = (uint8_t*)malloc(DATA_BYTES_IMG);
    g_data_after = (uint8_t*)malloc(DATA_BYTES_IMG);
    g_file = (uint8_t*)malloc(FILE_MAX);
    g_file_after = (uint8_t*)malloc(FILE_MAX);
    memcpy(g_data_pristine, DATA, DATA_BYTES_IMG);
    g_pad_vt[6] = (void*)&pad_size;                            // +0x18 GetReplayPacketSize
    g_pad_vt[7] = (void*)&pad_update;                          // +0x1c UpdateReplay
    g_pad_vt[8] = (void*)&pad_make;                            // +0x20 MakeReplayPacket
    g_pad_vt[11] = (void*)&pad_dynamic;                        // +0x2c IsDynamic
    patch_jump(0x004140e0, (void*)&stub_MemAlloc);
    patch_jump(0x00414390, (void*)&stub_delete);
    patch_jump(0x0043c080, (void*)&stub_TireCreate);
    patch_jump(0x004731c0, (void*)&stub_TireSoundCreate);
    patch_jump(0x00419d10, (void*)&stub_ResourceExists);
    patch_jump(0x0041b210, (void*)&stub_StringTableGet);
    patch_jump(0x0041b2a0, (void*)&stub_StringTableGetEntry);
    patch_jump(0x0041b270, (void*)&stub_StringTableForget);
    patch_jump(0x004cfe40, (void*)&stub_atof);
    patch_jump(0x004ceff0, (void*)&stub_atexit);
    patch_jump(0x00426d00, (void*)&stub_RegisterCar);
    patch_jump(0x004725f0, (void*)&stub_EngineSoundCreate);
    patch_jump(0x00472c10, (void*)&stub_EngineSoundDtor);
    patch_jump(0x00472560, (void*)&stub_Sound3DCreate);
    patch_jump(0x00472510, (void*)&stub_SoundDashCreate);
    patch_jump(0x0042d0a0, (void*)&stub_assert);
    patch_jump(0x00426d70, (void*)&stub_assert);
    patch_jump(0x00411150, (void*)&stub_LogReport);
    patch_jump(0x004112b0, (void*)&stub_LogPanic);
    patch_jump(0x00414f30, (void*)&stub_SingleBegin);
    patch_jump(0x00415000, (void*)&stub_SingleEnter);
    patch_jump(0x00415070, (void*)&stub_SingleLeave);
    patch_jump(0x00415090, (void*)&stub_SingleEnd);
    patch_jump(0x004118b0, (void*)&stub_FileReadExact);
    patch_jump(0x00411a30, (void*)&stub_FileWrite);
    patch_jump(0x00413b40, (void*)&stub_PTimeNow);
    patch_jump(0x004a2760, (void*)&stub_multi_register);
    patch_jump(0x004a27e0, (void*)&stub_multi_unregister);
    patch_jump(0x00442fc0, (void*)&stub_deity_register);
    patch_jump(0x00443640, (void*)&stub_deity_update_car);
    patch_jump(0x00444190, (void*)&stub_deity_finished);
    patch_jump(0x004441c0, (void*)&stub_deity_lap);
    patch_jump(0x004391b0, (void*)&stub_car_update_common);
    patch_jump(0x0043c510, (void*)&stub_collision_play);
    memcpy(g_add_event_bytes, (void*)0x0042d8b0, 5);
    add_event_real(false);
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    for (g_world = 0; g_world < worlds; g_world++) run_world();

    printf("\n%-34s %7s %7s %6s %6s %6s %5s\n", "function", "calls", "changed", "differ", "fp", "faults", "replay");
    int bad = 0;
    for (int i = 0; i < g_nstats; i++) {
        const Stat& st = g_stats[i];
        printf("%-34s %7ld %7ld %6ld %6ld %6ld %5ld%s\n", st.name, st.calls, st.changed, st.fails, st.fp_fails, st.faults,
               st.replay_only, st.main ? "  (main)" : "");
        if (st.fails || st.fp_fails) bad++;
    }
    printf("\ncoverage: %ld worlds; frames %ld, index wraps %ld, data wraps %ld, carried on %ld; events %ld, dropped %ld "
           "(logged %ld); PlayUpdate modes %ld/%ld/%ld, reached the end %ld; seal plain %ld / wrapped %ld (skipped %ld); "
           "load ok %ld, failed %ld %ld %ld %ld %ld %ld %ld %ld; telemetry %ld in / %ld out\n",
           c_worlds, c_frames, c_index_wrap, c_data_wrap, c_carry_on, c_events, c_drop, c_drop_log, c_modes[0], c_modes[1],
           c_modes[2], c_at_end, c_sealed_plain, c_sealed_wrapped, c_seal_skipped, c_load_ok, c_load_fail[0], c_load_fail[1],
           c_load_fail[2], c_load_fail[3], c_load_fail[4], c_load_fail[5], c_load_fail[6], c_load_fail[7], c_tel_ok, c_tel_out);
    printf("advance_replay_frame: straight on %ld, data wrapped %ld, index wrap carried on %ld, index wrap to 0 %ld\n",
           c_path[0], c_path[1], c_path[2], c_path[3]);
    printf("ghost: before its lap's start %ld; laps taken %ld, visible ticks %ld, following %ld, hidden at the finish %ld, "
           "negative time %ld, sounds stopped %ld; feed_ghost_car cut %ld / whole %ld; NewLap taken %ld\n",
           c_ghost_early, c_ghost_init, c_ghost_visible, c_ghost_follow, c_ghost_hidden, c_ghost_neg, c_sound_stop, c_feed_cut, c_feed_plain,
           c_newlap_taken);
    if (g_fp_unknown) printf("%d footprint objects of an unknown class\n", g_fp_unknown);
    printf("%d of %d functions differ or miss their footprint%s\n", bad, g_nstats,
           g_first_bad_world >= 0 ? " (see above)" : "");
    return bad ? 1 : 0;
}
