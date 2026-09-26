// world_car2.cpp -- the car.obj rewrites of group C2 (hook/phys_car.cpp: construction, damage, teleport, reset,
// replay) against the originals, on random worlds, outside the game (docs/PORTING.md, checking step 3).
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC /DVP_FUZZ test\world_car2.cpp
//        /Fo<outdir>\ /Fe<outdir>\world_car2.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_car2.exe [iterations] [name-filter]
//
// Loads out\race_v10.exe at 0x400000 the way test/fuzz.cpp does and includes the rewrite file itself. Every
// iteration builds a whole world in a bump arena: a CarData (PhobData) with random but plausible numbers, five
// LODs of a random body model (pristine and live copies, with a surface's vertex range and UVs), and a real Car
// made by the ORIGINAL constructor (so Setup, reset, the Wheels, the drivetrain and the SphereGroupVolume are the
// game's own); then random dynamic state (frame, velocities, controls, dents, damage grid, flags, wild floats).
// Each check snapshots the arena and the image's whole .data/.bss, runs the original, keeps the result,
// restores, runs the rewrite and compares the arena (the car, its models' vertices, the body volume and spheres,
// everything allocated during the call), all of .data/.bss, the return value and the stubs' call logs.
//
// Stubbed (a jump to a logger): MemAlloc (the bump arena, so both passes allocate the same addresses), operator
// delete, TireCreate, TireSound::Create, ResourceExists, StringTableGet/GetEntry/Forget, atof, atexit,
// PhysTaskRegisterCar, EngineSound::Create / ~EngineSound, Sound3D::Create, SoundDash::Create, PhysReplayAddEvent,
// the two ASSERT_MSGs, LogReport, LogPanic. Everything else runs as the game's code: PhobDyno and PhobRoot,
// Wheel, Engine/Clutch/Transmission/Differential, PowerCurve, Propeller/Wing, SphereGroupVolume and its spheres,
// the mr model functions, the matrix helpers, sprintf, PhysTaskFindCar, the replay handler table.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hook/phys_car.cpp"

// ---- what the rewrites link against, standing in for the DLL and test/fuzz.cpp -------------------------
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

static uint32_t g_state = 0x2545f491u;
bool g_fuzz_specials = true;
uint32_t fuzz_rand() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}
float fuzz_float() {
    uint32_t r = fuzz_rand(), k = r % 100;
    float u = (float)(fuzz_rand() & 0xffffff) / 16777216.0f;
    float s = (r & 0x100) ? -1.0f : 1.0f;
    if (k < 40) return s * u;
    if (k < 70) return s * u * 200.0f;
    if (k < 78) return s * u * 1e6f;
    if (k < 84) return s * u * 1e-6f;
    if (k < 88) return (r & 0x200) ? 0.0f : -0.0f;
    if (k < 90) return s;
    if (!g_fuzz_specials) return s * u;
    if (k < 93) return fbits(fuzz_rand() & 0x807fffff);
    if (k < 95) return fbits((r & 0x100 ? 0x80000000u : 0) | 0x7f800000u);
    if (k < 97) return fbits(0x7fc00000u | (fuzz_rand() & 0x3fffff));
    return fbits(fuzz_rand());
}
void fuzz_fill(Arena&) {}
int fuzz_report(int it, const Arena&, const void*, const void*, size_t) { return it + 1; }
int fuzz_guarded(void (*fn)(void*), void* arg) {
    __try { fn(arg); } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    return 0;
}

static float uni() { return (float)(fuzz_rand() & 0xffffff) / 16777216.0f; }
static float range(float a, float b) { return a + (b - a) * uni(); }
static bool chance(int pct) { return (int)(fuzz_rand() % 100) < pct; }
// a value in a plausible range, now and then anything at all
static float plausible(float lo, float hi, int wild_pct = 8) { return chance(wild_pct) ? fuzz_float() : range(lo, hi); }

// ---- the arena: everything the world allocates, and everything a call allocates ------------------------------
enum { ARENA_BYTES = 2 << 20 };
static uint8_t* g_arena;            // VirtualAlloc'd, zeroed
static uint32_t g_bump;             // next free byte
static uint8_t* g_arena_snap;
static uint8_t* g_arena_after;
static uint32_t g_bump_snap, g_bump_after;
static void* arena_alloc(uint32_t n) {
    uint32_t at = (g_bump + 15) & ~15u;
    if (at + n > ARENA_BYTES) { printf("arena full\n"); exit(3); }
    g_bump = at + n;
    return g_arena + at;
}

// the image's .data/.bss (every global the game has)
static uint8_t* const DATA = (uint8_t*)0x004e1000;
enum { DATA_BYTES = 0xf5f2c };
static uint8_t* g_data_snap;
static uint8_t* g_data_after;

// ---- the callees' log ---------------------------------------------------------------------------------------
enum { LOG_MAX = 4096 };
struct CallLog { uint32_t n; uint32_t w[LOG_MAX]; };
static CallLog g_log, g_log_orig;
static void log_word(uint32_t w) { if (g_log.n < LOG_MAX) g_log.w[g_log.n] = w; g_log.n++; }
static void log_str(const char* s) {
    size_t len = strlen(s);
    log_word((uint32_t)len);
    for (size_t i = 0; i < len; i += 4) { uint32_t w = 0; memcpy(&w, s + i, len - i < 4 ? len - i : 4); log_word(w); }
}
static void log_bytes(const void* p, int n) {
    log_word((uint32_t)n);
    for (int i = 0; i < n; i += 4) { uint32_t w = 0; memcpy(&w, (const uint8_t*)p + i, n - i < 4 ? n - i : 4); log_word(w); }
}

// the script the stubs follow (per world; identical for both passes)
struct Script { uint32_t resource_seed, sound_null_mask, dash_preset; uint32_t sound_calls; };
static Script g_script;

// ---- stubs ------------------------------------------------------------------------------------------------------
static void* __cdecl stub_MemAlloc(int n) { log_word('MALC'); log_word((uint32_t)n); return arena_alloc((uint32_t)n); }
static void __cdecl stub_delete(void* p) { log_word('DELE'); log_word((uint32_t)p); }
static void* __fastcall stub_sound_dtor(void* self, int, unsigned flags) {
    log_word('SDDT'); log_word((uint32_t)self); log_word(flags);
    return self;
}
static void __fastcall stub_sound_update(void* self, int) { log_word('SDUP'); log_word((uint32_t)self); }
static void* g_sound_vtable[8] = {(void*)&stub_sound_dtor, (void*)&stub_sound_update, 0, 0, 0, 0, 0, 0};
static void* fake_object(uint32_t n) {
    void** o = (void**)arena_alloc(n);
    o[0] = g_sound_vtable;
    return o;
}
static void* __cdecl stub_TireCreate(uint32_t w, uint32_t a, uint32_t d, const char* file) {
    log_word('TIRE'); log_word(w); log_word(a); log_word(d); log_str(file);
    float* t = (float*)arena_alloc(0x40);
    for (int i = 0; i < 16; i++) t[i] = 0.5f + i * 0.01f;
    t[0x2c / 4] = 1.05f;                       // load factor
    t[0x30 / 4] = 11.0f;                       // mass
    t[0x34 / 4] = 1.2f;                        // inertia
    return t;
}
static void* __cdecl stub_TireSoundCreate(int car_index, int wheel, void* w, void* car) {
    log_word('TSND'); log_word(car_index); log_word(wheel); log_word((uint32_t)w); log_word((uint32_t)car);
    return fake_object(0x10);
}
static uint8_t __cdecl stub_ResourceExists(const char* file) {
    log_word('RSRC'); log_str(file);
    uint32_t h = g_script.resource_seed;
    for (const char* p = file; *p; p++) h = h * 31 + (uint8_t)*p;
    return (uint8_t)((h >> 7) & 1);
}
static void* __cdecl stub_StringTableGet(const char* file) { log_word('STGT'); log_str(file); return (void*)0x5ab1e000; }
static const char* __cdecl stub_StringTableGetEntry(void* st, int row, int col) {
    log_word('STGE'); log_word((uint32_t)st); log_word(row); log_word(col);
    static const char* const v[] = {"-0.31", "0.77", "0.0521", "1e-3", "NaN?"};
    return v[(col + g_script.resource_seed) % 5];
}
static void __cdecl stub_StringTableForget(void* st) { log_word('STFG'); log_word((uint32_t)st); }
static double __cdecl stub_atof(const char* s) { log_word('ATOF'); log_str(s); return strtod(s, 0); }
static int __cdecl stub_atexit(void (*fn)()) { log_word('ATEX'); log_word((uint32_t)fn); return 0; }
static void __cdecl stub_RegisterCar(void* car, int index) { log_word('REGC'); log_word((uint32_t)car); log_word(index); }
static void* __cdecl stub_EngineSoundCreate(void* car) { log_word('ESCR'); log_word((uint32_t)car); return fake_object(0x40); }
static void __fastcall stub_EngineSoundDtor(void* self, int) { log_word('ESDT'); log_word((uint32_t)self); }
static void* __cdecl stub_Sound3DCreate(const char* name, int kind, const P3* v, const P3* p) {
    log_word('S3DC'); log_str(name); log_word(kind); log_word((uint32_t)v); log_word((uint32_t)p);
    if (g_script.sound_null_mask >> (g_script.sound_calls++ & 31) & 1) return 0;
    return fake_object(0x40);
}
static void* __cdecl stub_SoundDashCreate(const char* name, int kind, int index) {
    log_word('SDSH'); log_str(name); log_word(kind); log_word(index);
    if (g_script.sound_null_mask >> (g_script.sound_calls++ & 31) & 1) return 0;
    uint8_t* o = (uint8_t*)fake_object(0x40);
    if (g_script.dash_preset & 1) *(uint32_t*)(o + 0x30) = 0x3e4ccccd;
    if (g_script.dash_preset & 2) *(uint32_t*)(o + 8) = 0x3f19999a;
    return o;
}
static void __cdecl stub_ReplayAddEvent(int type, const void* data, int size) {
    log_word('REVT'); log_word(type); log_bytes(data, size);
}
static void __cdecl stub_assert(int ok, const char* msg, ...) { log_word('ASRT'); log_word(ok); (void)msg; }
static void __cdecl stub_logreport(const char* msg, ...) { log_word('LOGR'); log_str(msg); }
static void __cdecl stub_logpanic(const char* msg, ...) { log_word('PANC'); log_str(msg); }

static void patch_jump(uint32_t at, void* to) {
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xe9;
    int32_t rel = (int32_t)((uint32_t)to - (at + 5));
    memcpy(p + 1, &rel, 4);
}

// ---- the original, loaded at 0x400000 (as test/fuzz.cpp) ------------------------------------------------------
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

// ---- the world ------------------------------------------------------------------------------------------------
static uint32_t* const g_physics_tick = (uint32_t*)0x0052161c;
static uint8_t* const g_damage_on = (uint8_t*)0x00521634;
static int32_t* const g_car_count = (int32_t*)0x00520bb4;
static Car** const g_car_table = (Car**)0x00520c18;
static void** const g_handlers = (void**)0x00521930;
static void** const g_proxer_ptr = (void**)0x004ec480;

struct World {
    CarData* cd;
    Car* car;
    uint8_t* msg;                      // the message buffer (the car's slot at +0x100)
    uint8_t* p0; uint8_t* p1; uint8_t* out;   // replay packets
    uint8_t* ev;                       // a replay event
    P3* force; P3* point; float* dir;  // arguments
    uint32_t* proxer;                  // the fake Proxer and its row table
};
static World W;

static void random_rotation(M3* m, bool upright) {
    double yaw = range(-3.1416f, 3.1416f);
    double lim = upright ? 0.3 : 3.1416;
    double pitch = range((float)-lim, (float)lim), roll = range((float)-lim, (float)lim);
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch), cr = cos(roll), sr = sin(roll);
    double R[9] = {cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr,
                   -cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr,
                   sy * cp, -sp, cy * cp};
    for (int i = 0; i < 9; i++) m->m[i] = (float)R[i];
}

// a model: its model_info (handle; +0x10 -> mrModelInfo), the info, one surface, the vertices
static int32_t make_model(const mrVertex* src, int n, int first, int end) {
    uint8_t* mi = (uint8_t*)arena_alloc(0x20);
    mrModelInfo* info = (mrModelInfo*)arena_alloc(sizeof(mrModelInfo));
    mrSurface* s = (mrSurface*)arena_alloc(0x20);
    mrVertex* v = (mrVertex*)arena_alloc(n * 32 + 32);
    memcpy(v, src, n * 32);
    s->first_vertex = (int16_t)first;
    s->end_vertex = (int16_t)end;
    info->num_verts = n;
    info->verts = v;
    info->num_surfaces = 1;
    info->surfaces = s;
    *(mrModelInfo**)(mi + 0x10) = info;
    return (int32_t)(uintptr_t)mi;
}
static void random_vertex(mrVertex* v) {
    v->pos.x = range(-0.95f, 0.95f);
    v->pos.y = range(-0.2f, 1.3f);
    v->pos.z = range(-2.3f, 2.3f);
    for (int k = 0; k < 3; k++) v->_0c[k] = range(-1, 1);
    v->u = chance(95) ? uni() : range(-0.5f, 1.5f);
    v->v = chance(95) ? uni() : range(-0.5f, 1.5f);
    if (chance(2)) v->u = fuzz_float();
    if (chance(2)) v->v = fuzz_float();
}

static void random_cardata(CarData* cd) {
    uint8_t* raw = (uint8_t*)cd;
    for (int i = 0; i < (int)sizeof(CarData); i++) raw[i] = (uint8_t)fuzz_rand();
    *(uint32_t*)raw = fuzz_rand();                                // -> PhobRoot +0xc
    ((float*)raw)[2] = plausible(900.0f, 2000.0f);                // mass (lb -> kg in PhobDyno)
    for (int i = 3; i < 6; i++) ((float*)raw)[i] = plausible(10000.0f, 60000.0f);
    random_rotation(&cd->start_frame.rot, true);
    cd->start_frame.pos = {range(-500, 500), range(-5, 50), range(-500, 500)};
    cd->body_width = plausible(60.0f, 80.0f);
    for (int r = 0; r < 2; r++) {
        cd->track[r] = plausible(55.0f, 65.0f);
        cd->ride_height[r] = plausible(3.0f, 7.0f);
        cd->sway[r] = plausible(0.0f, 800.0f);
        cd->lift[r] = plausible(-0.3f, 0.3f);
        cd->spoiler_drag[r] = plausible(0.0f, 0.1f);
    }
    uint32_t k = fuzz_rand() % 8;
    if (k == 0) cd->track[1] = -cd->track[1];                     // a plane
    if (k == 1) cd->track[1] = -0.0f;
    cd->wheelbase = plausible(90.0f, 115.0f);
    cd->weight_distribution = plausible(40.0f, 60.0f);
    cd->torque_max = plausible(200.0f, 500.0f);
    cd->power = plausible(200.0f, 500.0f);
    cd->torque_rpm = plausible(3000.0f, 5000.0f);
    cd->power_rpm = plausible(5000.0f, 7000.0f);
    cd->idle_rpm = plausible(700.0f, 1000.0f);
    cd->redline = plausible(6000.0f, 8000.0f);
    cd->engine_inertia = plausible(1.0f, 5.0f);
    cd->engine_drag = plausible(0.0f, 0.01f);
    cd->fuel_consumption = plausible(5.0f, 20.0f);
    cd->fuel_capacity = plausible(10.0f, 25.0f);
    cd->final_drive = plausible(2.5f, 4.5f);
    int ng = (int)(fuzz_rand() % 8);
    for (int i = 0; i < 7; i++) cd->gear_ratios[i] = i < ng ? range(0.6f, 3.5f) : (chance(50) ? 0.0f : 0.005f);
    if (chance(5)) cd->gear_ratios[fuzz_rand() % 7] = fuzz_float();
    k = fuzz_rand() % 8;
    cd->torque_balance = k == 0 ? 0.0f : k == 1 ? 1.0f : k == 2 ? 0.01f : k == 3 ? 0.99f : k == 4 ? -0.5f : range(0, 1);
    for (int i = 0; i < 3; i++) cd->diff_stiffness[i] = plausible(0.0f, 50.0f);
    k = fuzz_rand() % 6;
    cd->gearbox_drag = k == 0 ? 0.0f : k == 1 ? -0.0f : k == 2 ? 1e-8f : k == 3 ? fuzz_float() : range(0.0f, 0.01f);
    cd->cm_height = plausible(15.0f, 22.0f);
    cd->wheel_lock = plausible(20.0f, 40.0f);
    cd->frontal_area = plausible(18.0f, 25.0f);
    cd->cp_height = plausible(0.0f, 20.0f);
    cd->cp_back = plausible(20.0f, 60.0f);
    for (int i = 0; i < 3; i++) cd->drag_coefficient[i] = plausible(0.3f, 1.2f);
    // the Wheel::Setup inputs (phys_wheel.cpp's CarData view)
    float* f = (float*)raw;
    for (int off = 0xcc; off < 0x16c; off += 4) if (off < 0xe4 || off >= 0xec) f[off / 4] = plausible(0.5f, 5.0f);
    f[0x130 / 4] = plausible(220, 320); f[0x134 / 4] = plausible(35, 55); f[0x138 / 4] = plausible(15, 18);
    f[0x13c / 4] = plausible(220, 320); f[0x140 / 4] = plausible(35, 55); f[0x144 / 4] = plausible(15, 18);
    cd->car_index = (int32_t)(fuzz_rand() % 8);
    int len = (int)(fuzz_rand() % 20);
    for (int i = 0; i < len; i++) cd->name[i] = "abcdefghijklmnopqrstuvwxyz"[fuzz_rand() % 26];
    cd->name[len] = 0;
}

// the world, up to (not including) the Car's construction
static void build_world_data() {
    memset(g_arena, 0, g_bump);
    g_bump = 0;
    W.cd = (CarData*)arena_alloc(0x200);
    random_cardata(W.cd);
    // the body model: LOD 0 and four smaller ones (some vertices shared exactly with LOD 0)
    int n0 = 20 + (int)(fuzz_rand() % 100);
    mrVertex* v0 = (mrVertex*)malloc(n0 * 32);
    for (int i = 0; i < n0; i++) random_vertex(&v0[i]);
    for (int lod = 0; lod < 5; lod++) {
        int n = lod == 0 ? n0 : 5 + (int)(fuzz_rand() % (n0 / 2 + 1));
        if (lod && chance(3)) n = 0;
        mrVertex* v = (mrVertex*)malloc((n + 1) * 32);
        for (int i = 0; i < n; i++) {
            if (lod == 0) v[i] = v0[i];
            else if (chance(40)) v[i] = v0[fuzz_rand() % n0];
            else random_vertex(&v[i]);
        }
        int first = n ? (int)(fuzz_rand() % (n / 2 + 1)) : 0, end = n ? first + (int)(fuzz_rand() % (n - first + 1)) : 0;
        if (chance(20)) { first = 0; end = n; }
        W.cd->lod_models[lod] = make_model(v, n, first, end);
        W.cd->live_models[lod] = make_model(v, n, first, end);
        free(v);
    }
    free(v0);
    W.msg = (uint8_t*)arena_alloc(0x400);
    W.p0 = (uint8_t*)arena_alloc(0x40);
    W.p1 = (uint8_t*)arena_alloc(0x40);
    W.out = (uint8_t*)arena_alloc(0x40);
    W.ev = (uint8_t*)arena_alloc(0x20);
    W.force = (P3*)arena_alloc(16);
    W.point = (P3*)arena_alloc(16);
    W.dir = (float*)arena_alloc(16);
    // the Proxer: row table at base + 0x10, one row of 32 bytes per car; entry 0 a pointer to (_, x, z)
    W.proxer = (uint32_t*)arena_alloc(0x200);
    W.proxer[0] = 0;                   // n
    W.proxer[3] = 32;                  // row stride
    W.proxer[4] = (uint32_t)(uintptr_t)(W.proxer + 16);   // base
    float* near_car = (float*)arena_alloc(16);
    near_car[1] = plausible(-8, 8);
    near_car[2] = plausible(-8, 8);
    for (int i = 0; i < 8; i++) (W.proxer + 16)[4 + i * 8] = chance(70) ? (uint32_t)(uintptr_t)near_car : 0;
    *g_proxer_ptr = W.proxer;
    // the script and the globals
    g_script.resource_seed = fuzz_rand();
    g_script.sound_null_mask = chance(20) ? fuzz_rand() : 0;
    g_script.dash_preset = fuzz_rand() & 3;
    g_script.sound_calls = 0;
    *g_physics_tick = fuzz_rand() % 200000;
    *g_damage_on = chance(80);
    *g_car_count = 8;
    memset(g_handlers, 0, 32);
    *(uint8_t*)0x00521d48 = (uint8_t)(chance(50) ? 3 : fuzz_rand() & 3);   // Setup's statics' guard
}

typedef Car*(__fastcall* CarCtor_t)(Car*, Edx, CarData*, void*);
static const CarCtor_t orig_ctor = (CarCtor_t)0x004364c0;

static void random_state() {
    Car* c = W.car;
    random_rotation(&c->frame.rot, chance(70));
    c->frame.pos = {range(-500, 500), range(-5, 50), range(-500, 500)};
    for (int i = 0; i < 3; i++) {
        (&c->velocity.x)[i] = chance(30) ? range(-3.5f, 3.5f) : plausible(-60, 60);
        (&c->angular_velocity.x)[i] = plausible(-5, 5);
    }
    c->steering = chance(10) ? fuzz_float() : range(-1.3f, 1.3f);
    c->engine.smoothed_throttle = chance(10) ? fuzz_float() : range(-0.1f, 1.1f);
    c->braking = chance(10) ? fuzz_float() : range(-0.1f, 1.1f);
    c->opacity = chance(30) ? 1.0f : chance(10) ? fuzz_float() : range(-0.1f, 1.1f);
    c->perceived_rpm = chance(10) ? fuzz_float() : range(-2500, 12500);
    c->engine.redline = plausible(5000, 9000);
    c->engaged_gear = (int32_t)(fuzz_rand() % 9) - 1;
    c->gear = (int32_t)(fuzz_rand() % 9) - 1;
    c->damaged = (uint8_t)(chance(50) ? 0 : chance(90) ? 1 : fuzz_rand());
    c->horn = (uint8_t)(chance(70) ? 0 : fuzz_rand());
    c->reset_event = (uint8_t)(chance(70) ? 0 : fuzz_rand());
    c->_004 = fuzz_rand();
    c->speed = plausible(0, 80);
    c->scrape_energy = plausible(0, 1e6f);
    c->teleport_time = (float)(*g_physics_tick * 0.016) - (chance(80) ? range(-1, 7) : fuzz_float());
    for (int i = 0; i < 16; i++) c->damage_grid[i] = (uint16_t)(chance(60) ? 0 : fuzz_rand());
    for (int i = 0; i < 4; i++) c->wheels[i].broken = (uint8_t)(chance(80) ? 0 : fuzz_rand());
    c->max_steer_angle = plausible(0.3f, 0.7f);
    float* misc[] = {&c->fuel, &c->lat_g, &c->long_g, &c->vert_g, &c->aero_g, &c->look_side, &c->clutch_engagement,
                     &c->engine.coolant_temperature, &c->engine.block_temperature, &c->steer_feedback};
    for (float* p : misc) *p = plausible(-10, 400);
    c->look_back = (uint8_t)(fuzz_rand() & 1);
    c->tick_count = (uint16_t)fuzz_rand();
    // dents: some live vertices moved
    if (chance(50))
        for (int lod = 0; lod < 5; lod++) {
            mrModelInfo* info = *(mrModelInfo**)((uint8_t*)(uintptr_t)c->live_models[lod] + 0x10);
            for (int j = 0; j < info->num_verts; j++)
                if (chance(20)) info->verts[j].pos.y += range(-0.05f, 0.05f);
        }
    g_car_table[c->car_index & 7] = c;
}

// one full world: data, the Car (original constructor), random state
static void build_world() {
    build_world_data();
    W.car = (Car*)arena_alloc(4224);
    for (int i = 0; i < 4224; i++) ((uint8_t*)W.car)[i] = (uint8_t)fuzz_rand();
    g_log.n = 0;
    orig_ctor(W.car, 0, W.cd, (void*)0x100);
    random_state();
}

// ---- one check: the original, then the rewrite, from the same world -------------------------------------------
static uint32_t g_ret_orig, g_ret_new;
static const char* g_name;
static int g_iter;

static void begin() {
    g_bump_snap = g_bump;
    memcpy(g_arena_snap, g_arena, ARENA_BYTES);
    memcpy(g_data_snap, DATA, DATA_BYTES);
    g_log.n = 0;
    g_script.sound_calls = 0;
}
static void mid() {
    g_bump_after = g_bump;
    memcpy(g_arena_after, g_arena, ARENA_BYTES);
    memcpy(g_data_after, DATA, DATA_BYTES);
    g_log_orig = g_log;
    memcpy(g_arena, g_arena_snap, ARENA_BYTES);
    memcpy(DATA, g_data_snap, DATA_BYTES);
    g_bump = g_bump_snap;
    g_log.n = 0;
    g_script.sound_calls = 0;
}
static const char* where(const uint8_t* p, char* buf) {
    const uint8_t* car = (const uint8_t*)W.car;
    if (p >= car && p < car + 4224) sprintf(buf, "car +0x%x", (unsigned)(p - car));
    else sprintf(buf, "arena +0x%x", (unsigned)(p - g_arena));
    return buf;
}
static bool finish() {
    bool ok = true;
    char buf[64];
    if (g_bump != g_bump_after) {
        printf("  %s iteration %d: allocated %u vs %u bytes\n", g_name, g_iter, g_bump_after, g_bump);
        ok = false;
    }
    for (uint32_t i = 0; i < ARENA_BYTES; i++)
        if (g_arena_after[i] != g_arena[i]) {
            uint32_t o = i & ~3u, x, y;
            memcpy(&x, g_arena_after + o, 4);
            memcpy(&y, g_arena + o, 4);
            // translate the arena address back to the object in the (restored) world
            printf("  %s iteration %d: %s original %08x (%.9g), rewrite %08x (%.9g)\n", g_name, g_iter,
                   where(g_arena + o, buf), x, fbits(x), y, fbits(y));
            ok = false;
            break;
        }
    for (uint32_t i = 0; i < DATA_BYTES; i++)
        if (g_data_after[i] != DATA[i]) {
            uint32_t o = i & ~3u, x, y;
            memcpy(&x, g_data_after + o, 4);
            memcpy(&y, DATA + o, 4);
            printf("  %s iteration %d: global 0x%08x original %08x, rewrite %08x\n", g_name, g_iter,
                   (unsigned)(uintptr_t)(DATA + o), x, y);
            ok = false;
            break;
        }
    if (g_log.n != g_log_orig.n || memcmp(g_log.w, g_log_orig.w, (g_log.n < LOG_MAX ? g_log.n : LOG_MAX) * 4)) {
        printf("  %s iteration %d: the calls differ (%u words vs %u)\n", g_name, g_iter, g_log_orig.n, g_log.n);
        int shown = 0;
        for (uint32_t i = 0; (i < g_log.n || i < g_log_orig.n) && i < LOG_MAX && shown < 40; i++) {
            uint32_t a = i < g_log_orig.n ? g_log_orig.w[i] : 0, b = i < g_log.n ? g_log.w[i] : 0;
            if (a != b || shown) { printf("    [%u] %08x %08x\n", i, a, b); shown++; }
        }
        ok = false;
    }
    if (g_ret_orig != g_ret_new) {
        printf("  %s iteration %d: returns %08x vs %08x\n", g_name, g_iter, g_ret_orig, g_ret_new);
        ok = false;
    }
    return ok;
}

// ---- apply_rect_damage: a pure function, fuzzed on bounded rectangles --------------------------------------------
// (rows are v x 15.99 truncated, written with no bounds check: the inputs keep them within a guarded buffer)
static float rect_coord() {
    uint32_t k = fuzz_rand() % 100;
    if (k < 70) return range(-0.2f, 1.2f);
    if (k < 80) return range(-4.0f, 8.0f);
    if (k < 84) return (float)(int)range(-2, 3) / 15.99f;        // on a cell edge
    if (k < 87) return fbits(0x7fc00000u | (fuzz_rand() & 0x3fffff));   // NaN: __ftol gives 0
    if (k < 90) return fbits((k & 1 ? 0x80000000u : 0) | 0x7f800000u);   // inf: 0 too
    if (k < 94) return (k & 1) ? 0.0f : -0.0f;
    if (k < 97) return fbits(fuzz_rand() & 0x807fffff);         // denormal
    return range(-1e-6f, 1e-6f);
}
static int fuzz_rect(int iterations) {
    static uint16_t a[512], b[512];
    for (int it = 0; it < iterations; it++) {
        for (int i = 0; i < 512; i++) a[i] = b[i] = (uint16_t)fuzz_rand();
        float u0 = rect_coord(), v0 = rect_coord(), u1 = rect_coord(), v1 = rect_coord();
        ((RectDamage_t)0x004370a0)(a + 128, bits(u0), bits(v0), bits(u1), bits(v1));
        apply_rect_damage(b + 128, u0, v0, u1, v1);
        if (memcmp(a, b, sizeof a)) {
            printf("  apply_rect_damage iteration %d: (%g, %g, %g, %g) differs\n", it, u0, v0, u1, v1);
            for (int i = 0; i < 512; i++)
                if (a[i] != b[i]) printf("    row %d: original %04x, rewrite %04x\n", i - 128, a[i], b[i]);
            return 1;
        }
    }
    return 0;
}

// the v1.0 originals, by address
typedef void(__fastcall* C0_t)(Car*, Edx);
typedef void(__fastcall* CAEF_t)(Car*, Edx, const P3*, const P3*, int32_t);
typedef void(__fastcall* CAAD_t)(Car*, Edx, const P3*, const P3*, uint8_t, uint8_t);
typedef void(__fastcall* CPkt_t)(Car*, Edx, uint8_t*);
typedef void(__fastcall* CUR_t)(Car*, Edx, const uint8_t*, const uint8_t*, uint32_t);
typedef void(__fastcall* CTel_t)(Car*, Edx, const P3*, const float*);
typedef void(__fastcall* CSetup_t)(Car*, Edx, const CarData*);
typedef int(__cdecl* Handler_t)(const uint8_t*, int32_t);
typedef void(__cdecl* V_t)();
typedef int32_t(__fastcall* CInt_t)(Car*, Edx);

static const char* const k_names[] = {
    "Car::Car", "Car::~Car", "Car::ApplyExternalForce", "Car::ActuallyApplyDamage", "Car::ResolveExternalImpulse",
    "Car::replay_damage_handler", "Car::replay_reset_handler", "Car::UpdateReplay", "Car::MakeReplayPacket",
    "Car::GetMessage", "Car::post_teleport_fadein", "Car::Teleport", "Car::reset_subordinates", "Car::reset_damage",
    "Car::Reset", "Car::reset(private)", "Car::Setup", "CarBegin", "CarEnd", "Car::GetReplayPacketSize"};
enum { NFN = sizeof k_names / sizeof k_names[0] };

static void random_vec(P3* p, float lo, float hi) { p->x = plausible(lo, hi); p->y = plausible(lo, hi); p->z = plausible(lo, hi); }

// ---- coverage: what the ORIGINAL did in each world (so a pass means something) ----------------------------------
struct Coverage { int car, models, other, globals, calls, branch[4]; };
static Coverage g_cov[32];
static uint8_t* after_of(const void* p) { return g_arena_after + ((const uint8_t*)p - g_arena); }
static bool changed(const void* p, uint32_t n) { return memcmp(after_of(p), g_arena_snap + ((const uint8_t*)p - g_arena), n) != 0; }
static bool log_has(uint32_t tag) { for (uint32_t i = 0; i < g_log_orig.n && i < LOG_MAX; i++) if (g_log_orig.w[i] == tag) return true; return false; }
static void cover(int which) {
    Coverage& c = g_cov[which];
    Car* car = W.car;
    bool mc = false;
    if (car && which != 0) {
        for (int lod = 0; lod < 5; lod++) {
            mrModelInfo* info = *(mrModelInfo**)((uint8_t*)(uintptr_t)car->live_models[lod] + 0x10);
            if (info->num_verts && changed(info->verts, info->num_verts * 32)) mc = true;
        }
    }
    c.car += car && changed(car, 3768);
    c.models += mc;
    uint32_t hw = g_bump_after > g_bump_snap ? g_bump_after : g_bump_snap;
    c.other += hw > g_bump_snap || (W.msg && changed(W.msg, 0x400)) || (W.out && changed(W.out, 0x40));
    c.globals += memcmp(g_data_after, g_data_snap, DATA_BYTES) != 0;
    c.calls += g_log_orig.n > 0;
    Car* a = car ? (Car*)after_of(car) : 0;                     // the original's result
    if (car) car = (Car*)(g_arena_snap + ((uint8_t*)car - g_arena));   // and the state it started from
    switch (which) {
    case 0: c.branch[0] += a->is_plane != 0; c.branch[1] += log_has('SDSH'); break;
    case 3: case 4: case 5:
        c.branch[0] += mc; c.branch[1] += memcmp(a->damage_grid, car->damage_grid, 32) != 0;
        c.branch[2] += log_has('REVT'); c.branch[3] += a->damaged != car->damaged; break;
    case 7: c.branch[0] += (W.p1[0x39] & 2) != 0; c.branch[1] += mc; c.branch[2] += a->is_plane != 0; break;
    case 9: c.branch[0] += car->is_plane != 0; c.branch[1] += (after_of(W.msg)[0x100 + 0x69] & 3) != 0;
        c.branch[2] += (after_of(W.msg)[0x100 + 0x69] & 0x40) != 0; break;
    case 10: c.branch[0] += a->opacity != car->opacity; c.branch[1] += a->teleport_time != car->teleport_time; break;
    case 16: c.branch[0] += a->is_plane != 0; c.branch[1] += log_has('ATEX'); c.branch[2] += log_has('ATOF'); break;
    case 2: c.branch[0] += a->scrape_energy != car->scrape_energy; break;
    }
}

static int run_one(int which, int it) {
    g_iter = it;
    g_name = k_names[which];
    g_ret_orig = g_ret_new = 0;
    Car* c;
    switch (which) {
    case 0: {                                                   // Car::Car on random bytes
        build_world_data();
        W.car = c = (Car*)arena_alloc(4224);
        for (int i = 0; i < 4224; i++) ((uint8_t*)c)[i] = (uint8_t)fuzz_rand();
        void* p2 = (void*)(uintptr_t)(fuzz_rand() % 0x400);
        begin(); g_ret_orig = (uint32_t)orig_ctor(c, 0, W.cd, p2);
        mid(); g_ret_new = (uint32_t)Car_ctor(c, 0, W.cd, p2);
        break;
    }
    case 1:
        build_world(); c = W.car;
        begin(); ((C0_t)0x00436920)(c, 0); mid(); Car_dtor(c, 0);
        break;
    case 2: {
        build_world(); c = W.car;
        random_vec(W.force, -20000, 20000);
        random_vec(W.point, -2, 2);
        int32_t s = (int32_t)(fuzz_rand() % 200);
        begin(); ((CAEF_t)0x00436a90)(c, 0, W.force, W.point, s); mid(); Car_ApplyExternalForce(c, 0, W.force, W.point, s);
        break;
    }
    case 3: {
        build_world(); c = W.car;
        random_vec(W.force, -20000, 20000);
        if (chance(30)) { W.force->x *= 0.03f; W.force->y *= 0.03f; W.force->z *= 0.03f; }
        W.point->x = plausible(-1.2f, 1.2f); W.point->y = plausible(-0.5f, 1.8f); W.point->z = plausible(-2.6f, 2.6f);
        uint8_t rev = (uint8_t)(chance(50) ? 0 : fuzz_rand()), nodent = (uint8_t)(chance(70) ? 0 : fuzz_rand());
        begin(); ((CAAD_t)0x00436b40)(c, 0, W.force, W.point, rev, nodent);
        mid(); Car_ActuallyApplyDamage(c, 0, W.force, W.point, rev, nodent);
        break;
    }
    case 4: {
        build_world(); c = W.car;
        if (chance(25)) c->vtable = (void**)0x004dc2e8;          // a NetCar: its ApplyDamage doesn't dent
        random_vec(W.force, -20000, 20000);
        if (chance(20)) { W.force->x *= 0.02f; W.force->y *= 0.02f; W.force->z *= 0.02f; }
        W.point->x = c->frame.pos.x + plausible(-3, 3);
        W.point->y = c->frame.pos.y + plausible(-3, 3);
        W.point->z = c->frame.pos.z + plausible(-3, 3);
        int32_t s = (int32_t)(fuzz_rand() % 200);
        begin(); ((CAEF_t)0x00437120)(c, 0, W.force, W.point, s); mid(); Car_ResolveExternalImpulse(c, 0, W.force, W.point, s);
        break;
    }
    case 5: {
        build_world(); c = W.car;
        if (chance(25)) c->vtable = (void**)0x004dc2e8;
        for (int i = 0; i < 16; i++) W.ev[i] = (uint8_t)fuzz_rand();
        W.ev[13] = (uint8_t)c->car_index;
        int32_t mode = (int32_t)(fuzz_rand() % 4);
        begin(); g_ret_orig = ((Handler_t)0x004374e0)(W.ev, mode); mid(); g_ret_new = Car_replay_damage_handler(W.ev, mode);
        break;
    }
    case 6: {
        build_world(); c = W.car;
        W.ev[0] = (uint8_t)c->car_index; W.ev[1] = 0;
        int32_t mode = (int32_t)(fuzz_rand() % 4);
        begin(); g_ret_orig = ((Handler_t)0x00437640)(W.ev, mode); mid(); g_ret_new = Car_replay_reset_handler(W.ev, mode);
        break;
    }
    case 7: {
        build_world(); c = W.car;
        for (int i = 0; i < 0x3c; i++) { W.p0[i] = (uint8_t)fuzz_rand(); W.p1[i] = (uint8_t)fuzz_rand(); }
        if (chance(50)) { W.p0[0x3a] = 0; }
        if (chance(30)) W.p1[0x39] &= ~2;
        // PhobDyno's part plausible (a position and rotation near each other) half the time: packets the game made
        if (chance(50)) {
            ((CPkt_t)0x00438cd0)(c, 0, W.p0);
            memcpy(W.p1, W.p0, 0x3c);
            for (int i = 0; i < 6; i++) W.p1[fuzz_rand() % 0x3c] = (uint8_t)fuzz_rand();
        }
        uint32_t k = fuzz_rand() % 4;
        float t = k == 0 ? 0.0f : k == 1 ? 1.0f : k == 2 ? uni() : fuzz_float();
        uint32_t tb = bits(t);
        begin(); ((CUR_t)0x00438950)(c, 0, W.p0, W.p1, tb); mid(); Car_UpdateReplay(c, 0, W.p0, W.p1, tb);
        break;
    }
    case 8:
        build_world(); c = W.car;
        for (int i = 0; i < 0x40; i++) W.out[i] = (uint8_t)fuzz_rand();
        begin(); ((CPkt_t)0x00438cd0)(c, 0, W.out); mid(); Car_MakeReplayPacket(c, 0, W.out);
        break;
    case 9:
        build_world(); c = W.car;
        if (chance(30)) c->is_plane = (uint8_t)(1 + fuzz_rand() % 255);
        for (int i = 0; i < 0x400; i++) W.msg[i] = (uint8_t)fuzz_rand();
        begin(); ((CPkt_t)0x00438ef0)(c, 0, W.msg); mid(); Car_GetMessage(c, 0, W.msg);
        break;
    case 10:
        build_world(); c = W.car;
        begin(); ((C0_t)0x00439aa0)(c, 0); mid(); Car_post_teleport_fadein(c, 0);
        break;
    case 11:
        build_world(); c = W.car;
        W.point->x = plausible(-500, 500); W.point->y = plausible(-5, 50); W.point->z = plausible(-500, 500);
        if (chance(70)) { float a = range(-3.2f, 3.2f); W.dir[0] = sinf(a); W.dir[1] = cosf(a); }
        else { W.dir[0] = fuzz_float(); W.dir[1] = fuzz_float(); }
        begin(); ((CTel_t)0x00439b50)(c, 0, W.point, W.dir); mid(); Car_Teleport(c, 0, W.point, W.dir);
        break;
    case 12:
        build_world(); c = W.car;
        begin(); ((C0_t)0x00439df0)(c, 0); mid(); Car_reset_subordinates(c, 0);
        break;
    case 13:
        build_world(); c = W.car;
        begin(); ((C0_t)0x00439e50)(c, 0); mid(); Car_reset_damage(c, 0);
        break;
    case 14:
        build_world(); c = W.car;
        begin(); ((C0_t)0x00439f90)(c, 0); mid(); Car_Reset(c, 0);
        break;
    case 15:
        build_world(); c = W.car;
        begin(); ((C0_t)0x00439fd0)(c, 0); mid(); Car_reset(c, 0);
        break;
    case 16: {
        build_world(); c = W.car;
        c->num_volumes = (int32_t)(fuzz_rand() % 4);             // Setup appends the body volume
        random_cardata(W.cd);                                    // (keeps the models: they're past +0x1b8)
        *(uint8_t*)0x00521d48 = (uint8_t)(fuzz_rand() & 3);
        *g_damage_on = chance(80);
        begin(); ((CSetup_t)0x0043a130)(c, 0, W.cd); mid(); Car_Setup(c, 0, W.cd);
        break;
    }
    case 17:
        build_world();
        if (chance(20)) g_handlers[5] = (void*)0x1234;
        begin(); ((V_t)0x0043b090)(); mid(); Car_Begin();
        break;
    case 18:
        build_world();
        if (chance(70)) g_handlers[fuzz_rand() % 8] = (void*)0x004374e0;
        begin(); ((V_t)0x0043b0b0)(); mid(); Car_End();
        break;
    case 19:
        build_world(); c = W.car;
        begin(); g_ret_orig = ((CInt_t)0x0043b0c0)(c, 0); mid(); g_ret_new = Car_GetReplayPacketSize(c, 0);
        break;
    }
    cover(which);
    return finish() ? 0 : 1;
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_FUZZ_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    const char* filter = argc > 2 ? argv[2] : 0;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);                        // ...\test\world_car2.cpp (built /FC) -> ...\out\race_v10.exe
    char* s = strstr(exe, "\\test\\world_car2.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    g_arena = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_arena_snap = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_arena_after = (uint8_t*)VirtualAlloc(0, ARENA_BYTES, MEM_COMMIT, PAGE_READWRITE);
    g_data_snap = (uint8_t*)malloc(DATA_BYTES);
    g_data_after = (uint8_t*)malloc(DATA_BYTES);
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
    patch_jump(0x0042d8b0, (void*)&stub_ReplayAddEvent);
    patch_jump(0x0042d0a0, (void*)&stub_assert);
    patch_jump(0x00426d70, (void*)&stub_assert);
    patch_jump(0x00411150, (void*)&stub_logreport);
    patch_jump(0x004112b0, (void*)&stub_logpanic);
    unsigned cw;
    _controlfp_s(&cw, _PC_24, _MCW_PC);           // the physics thread's mode (ExceptSinglePrecision)
    int failed = 0, ran = 0;
    if (!filter || strstr("apply_rect_damage", filter)) {
        int bad = fuzz_rect(iterations * 50);
        printf("  %-30s %s (%d bounded rectangles)\n", "apply_rect_damage", bad ? "DIFFERS (above)" : "identical to the original",
               iterations * 50);
        failed += bad; ran++;
    }
    for (int which = 0; which < NFN; which++) {
        if (filter && !strstr(k_names[which], filter)) continue;
        int bad = 0;
        for (int it = 0; it < iterations && bad < 3; it++) bad += run_one(which, it);
        const Coverage& cv = g_cov[which];
        printf("  %-30s %s  [car %d, models %d, other %d, globals %d, calls %d; branches %d %d %d %d]\n", k_names[which],
               bad ? "DIFFERS (above)" : "identical to the original", cv.car, cv.models, cv.other, cv.globals, cv.calls,
               cv.branch[0], cv.branch[1], cv.branch[2], cv.branch[3]);
        if (bad) failed++;
        ran++;
    }
    printf("%d of %d differ (%d random worlds each)\n", failed, ran, iterations);
    return failed ? 1 : 0;
}
