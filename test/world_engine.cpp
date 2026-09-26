// world_engine.cpp -- the engine group's rewrites (hook/phys_engine.cpp) against the originals, on random
// worlds, outside the game (docs/PORTING.md, checking step 3).
//
//   build (from the repo root, in an x86 VS prompt; linked at 0x10000000 so 0x400000 is free):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_engine.cpp
//        /Fe:world_engine.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:    world_engine.exe [iterations]
//
// Like test/fuzz.cpp it loads out\race_v10.exe's sections at 0x400000 in a child process that reserved the
// range before its heap existed. It builds a small world in memory -- a car (PhobDyno, Car-sized) with its
// Engine embedded at +3068, a plane's Propeller and Wings, a quarter car with its Damper, a road table -- with
// real vtables (the v1.0 addresses), randomises the fields each function reads (sane ranges, with some NaNs,
// infinities, zeros and denormals mixed in), and runs the original and the rewrite from the same starting
// state: the whole world and the return value must come out bit for bit the same. The originals' callees
// (Shaft::ApplyTorque, PhobDyno::ApplyForce, GetArmPosition, the virtual GetRPM and HeatTube::Update, ...)
// run for real, from the image, in both passes, so a callee given different arguments shows up as a
// difference in the world it writes. Each function is run with the FPU at 24-bit precision (the physics
// thread) and at 53 (the main thread's default, where the constructors and Setup may run).
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VP_FUZZ                                     // PORT_FN registers with fuzz.h, not the DLL's port table
#include "../hook/phys_engine.cpp"                  // the rewrites themselves (file-local functions)

// ---- what the rewrites and fuzz.h link against -------------------------------------------------------------
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
void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { add(obj, 4, what); }
bool g_fuzz_specials = true;
static uint32_t g_state = 0x2545f491u;
uint32_t fuzz_rand() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}
float fuzz_float() { return 0.0f; }
void fuzz_fill(Arena&) {}
int fuzz_report(int it, const Arena&, const void*, const void*, size_t) { return it + 1; }
int fuzz_guarded(void (*fn)(void*), void* arg) {
    __try { fn(arg); } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    return 0;
}

// ---- random values ------------------------------------------------------------------------------------------
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static float unit() { return (float)(fuzz_rand() & 0xffffff) / 16777216.0f; }          // [0, 1)
static float rf(float lo, float hi) { return lo + (hi - lo) * unit(); }
static int g_special_pct = 3;                       // per value: how often a special replaces it
static float special() {
    uint32_t r = fuzz_rand() % 9;
    switch (r) {
    case 0: return bitsf(0x7fc00000u | (fuzz_rand() & 0x3fffff));        // quiet NaN
    case 1: return bitsf(0xffc00000u | (fuzz_rand() & 0x3fffff));        // negative NaN
    case 2: return bitsf(0x7f800000u);
    case 3: return bitsf(0xff800000u);
    case 4: return 0.0f;
    case 5: return -0.0f;
    case 6: return bitsf(fuzz_rand() & 0x807fffff);                      // denormal
    case 7: return bitsf(0x7f000000u | (fuzz_rand() & 0x807fffff));      // huge
    default: return bitsf(fuzz_rand());                                  // any pattern
    }
}
static float v(float lo, float hi) { return (int)(fuzz_rand() % 100) < g_special_pct ? special() : rf(lo, hi); }
static float vs(float mag) { return v(-mag, mag); }

// ---- the world ------------------------------------------------------------------------------------------------
enum { ENGINE_OFF = 3068, CAR_BYTES = 4224 };
struct World {
    alignas(16) uint8_t car[CAR_BYTES];             // a PhobDyno, Car-sized; the Engine at +3068
    Propeller prop;
    Wing wing;
    Damper damper;
    QuarterCar qc;
    PowerCurve curve;
    ThermalMass masses[2];
    float spare[8];
};
static World g_w, g_before, g_after;
static TableTerrain g_t, g_t_before, g_t_after;
static PhobDyno* car() { return (PhobDyno*)g_w.car; }
static Engine* eng() { return (Engine*)(g_w.car + ENGINE_OFF); }

static void rand_bytes(void* p, size_t n) {
    uint8_t* b = (uint8_t*)p;
    for (size_t i = 0; i + 4 <= n; i += 4) { float f = vs(1000.0f); memcpy(b + i, &f, 4); }
}

// a car in motion: an orthonormal-ish frame, velocities, the force and torque accumulators
static void rand_car() {
    PhobDyno* c = car();
    c->vtable = (void**)0x004dbfd0;                 // Car
    float a = rf(-3.2f, 3.2f), b = rf(-1.6f, 1.6f), g = rf(-3.2f, 3.2f);
    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b), cg = cosf(g), sg = sinf(g);
    float m[9] = {ca * cg, sg, -sa * cg, -ca * sg * cb + sa * sb, cg * cb, sa * sg * cb + ca * sb,
                  ca * sg * sb + sa * cb, -cg * sb, -sa * sg * sb + ca * cb};
    for (int i = 0; i < 9; i++) c->frame.rot.m[i] = (fuzz_rand() % 50) ? m[i] : v(-1, 1);
    c->frame.pos = {vs(2000), vs(100), vs(2000)};
    c->velocity = {vs(80), vs(20), vs(80)};
    c->angular_velocity = {vs(5), vs(5), vs(5)};
    c->force = {vs(1e4f), vs(1e4f), vs(1e4f)};
    c->torque = {vs(1e4f), vs(1e4f), vs(1e4f)};
}

static void rand_engine() {
    Engine* e = eng();
    e->vtable = VT_Engine;
    e->owner = car();
    e->inertia = v(0.05f, 1.0f);
    e->inv_inertia = 1.0f / e->inertia;
    e->rpm = v(-500, 9000);
    e->drag = v(0, 0.02f);
    ThermalMass* t[2] = {&e->block, &e->coolant};
    for (ThermalMass* m : t) {
        m->conductance = v(0, 2000);
        m->heat_capacity = v(1e4f, 1e5f);
        m->inv_heat_capacity = 1.0f / m->heat_capacity;
        m->temperature = v(250, 420);
        m->heat = vs(5000);
    }
    e->throttle = v(0, 1);
    e->idle_throttle = v(0, 0.3f);
    e->idle_rpm = v(500, 1200);
    e->smoothed_throttle = v(0, 1);
    e->redline = v(4000, 9000);
    e->cranking = fuzz_rand() & 1;
    e->stalled = fuzz_rand() & 1;
    e->curve = {v(-1e-5f, 0), v(-0.05f, 0.2f), v(0, 500), v(0, 0.02f), v(3000, 8000), v(0, 0.1f)};
    e->thermostat.vtable = VT_HeatTube;
    e->thermostat.conductance = v(0, 10000);
    e->thermostat.a = &e->block;
    e->thermostat.b = &e->coolant;
}

// ---- the driver ---------------------------------------------------------------------------------------------
template <typename R> static uint64_t ret_bits(R r) { uint64_t u = 0; memcpy(&u, &r, sizeof r); return u; }
#define CALL(ORIG, FN, ...) (orig ? ((decltype(&FN))(ORIG))(__VA_ARGS__) : FN(__VA_ARGS__))

typedef void (*SetupFn)();
typedef uint64_t (*CallFn)(bool orig);
static float g_f[6];                                // this iteration's float arguments
static int32_t g_i;                                 // and integer one

static int report(const char* name, int it, const uint8_t* want, const uint8_t* got, size_t n, uint64_t ro,
                  uint64_t rn) {
    printf("  %-26s MISMATCH on iteration %d\n", name, it);
    printf("    args %08x %08x %08x %08x %08x %08x\n", (uint32_t)ret_bits(g_f[0]), (uint32_t)ret_bits(g_f[1]),
           (uint32_t)ret_bits(g_f[2]), (uint32_t)ret_bits(g_f[3]), (uint32_t)ret_bits(g_f[4]),
           (uint32_t)ret_bits(g_f[5]));
    if (ro != rn) printf("    return: original %016llx, rewrite %016llx\n", ro, rn);
    int shown = 0;
    for (size_t i = 0; i + 4 <= n && shown < 12; i += 4)
        if (memcmp(want + i, got + i, 4)) {
            uint32_t a, b;
            memcpy(&a, want + i, 4);
            memcpy(&b, got + i, 4);
            printf("    +%-6u original %08x (%.9g), rewrite %08x (%.9g)\n", (unsigned)i, a, bitsf(a), b, bitsf(b));
            shown++;
        }
    return 1;
}

static int run(const char* name, int iters, SetupFn setup, CallFn call, bool terrain = false) {
    uint8_t* region = terrain ? (uint8_t*)&g_t : (uint8_t*)&g_w;
    uint8_t* before = terrain ? (uint8_t*)&g_t_before : (uint8_t*)&g_before;
    uint8_t* after = terrain ? (uint8_t*)&g_t_after : (uint8_t*)&g_after;
    size_t n = terrain ? sizeof g_t : sizeof g_w;
    static const unsigned modes[2] = {_PC_24, _PC_53};
    for (int mode = 0; mode < 2; mode++) {
        unsigned cw;
        for (int it = 0; it < iters; it++) {
            setup();
            memcpy(before, region, n);
            _controlfp_s(&cw, modes[mode], _MCW_PC);
            uint64_t ro = call(true);
            memcpy(after, region, n);
            memcpy(region, before, n);
            uint64_t rn = call(false);
            _controlfp_s(&cw, _PC_53, _MCW_PC);
            if (ro != rn || memcmp(after, region, n)) {
                printf("  (precision %s)\n", mode ? "53" : "24");
                return report(name, it, after, region, n, ro, rn);
            }
        }
    }
    printf("  %-26s %d random worlds x 2 precisions: identical to the original\n", name, iters);
    return 0;
}

// ---- the functions --------------------------------------------------------------------------------------------
static void s_engine() { rand_car(); rand_engine(); for (float& f : g_f) f = vs(1000); }

static void s_ctor() { rand_bytes(eng(), sizeof(Engine)); }
static uint64_t c_ctor(bool orig) { return ret_bits(CALL(0x00446630, Engine_Ctor, eng(), 0)); }

static void s_setup() {
    rand_car();
    rand_bytes(eng(), sizeof(Engine));
    g_w.curve = {v(-1e-5f, 0), v(-0.05f, 0.2f), v(0, 500), v(0, 0.02f), v(3000, 8000), v(0, 0.1f)};
    g_f[0] = v(0.05f, 1); g_f[1] = v(0, 0.02f); g_f[2] = v(4000, 9000); g_f[3] = v(0, 1500);
}
static uint64_t c_setup(bool orig) {
    CALL(0x00446700, Engine_Setup, eng(), 0, car(), g_f[0], g_f[1], &g_w.curve, g_f[2], g_f[3]);
    return 0;
}

static void s_apply() { s_engine(); g_f[0] = vs(800); }
static uint64_t c_apply(bool orig) { CALL(0x00446810, Engine_ApplyTorque, eng(), 0, g_f[0]); return 0; }

static void s_heat() {
    s_engine();
    g_f[0] = v(0, 0.1f);
    *(float*)0x004ecf84 = v(250, 320);              // the ambient temperature PhysicsGetTemperature returns
}
static uint64_t c_heat(bool orig) { CALL(0x00446970, Engine_UpdateHeat, eng(), 0, g_f[0]); return 0; }

static void s_tube() {
    s_engine();
    HeatTube* t = &eng()->thermostat;
    ThermalMass* m[4] = {&eng()->block, &eng()->coolant, &g_w.masses[0], &g_w.masses[1]};
    for (ThermalMass& x : g_w.masses) x = {v(0, 2000), v(1e4f, 1e5f), v(0, 1e-4f), v(250, 420), vs(5000)};
    t->a = m[fuzz_rand() % 4];
    t->b = m[fuzz_rand() % 4];                      // sometimes the same mass
    g_f[0] = v(0, 0.1f);
}
static uint64_t c_tube(bool orig) {
    CALL(0x004472f0, HeatTube_Update, &eng()->thermostat, 0, g_f[0]);
    return 0;
}

static void s_prop_ctor() { rand_bytes(&g_w.prop, sizeof(Propeller)); for (float& f : g_f) f = v(-2, 2); }
static uint64_t c_prop_ctor(bool orig) {
    return ret_bits(CALL(0x00446e60, Propeller_Ctor, &g_w.prop, 0, (uint32_t)ret_bits(g_f[0]),
                         (uint32_t)ret_bits(g_f[1]), (uint32_t)ret_bits(g_f[2])));
}

static void s_prop() {
    rand_car();
    Propeller* p = &g_w.prop;
    p->vtable = VT_Propeller;
    p->owner = car();
    p->inertia = v(0.2f, 2);
    p->inv_inertia = 1.0f / p->inertia;
    p->rpm = (fuzz_rand() % 4) ? v(-500, 3000) : vs(20);
    p->drag = v(0, 0.01f);
    p->pos = {vs(1), vs(1), v(-1, 2)};
    g_f[0] = vs(1500);
}
static uint64_t c_prop(bool orig) { CALL(0x00446e90, Propeller_ApplyTorque, &g_w.prop, 0, g_f[0]); return 0; }

static void s_wing_ctor() {
    rand_bytes(&g_w.wing, sizeof(Wing));
    for (float& f : g_f) f = v(-8, 8);
    g_w.spare[0] = g_f[3]; g_w.spare[1] = g_f[4]; g_w.spare[2] = g_f[5];
    g_i = (int32_t)(fuzz_rand() % 3);
}
static uint64_t c_wing_ctor(bool orig) {
    return ret_bits(CALL(0x00447010, Wing_Ctor, &g_w.wing, 0, (const P3*)&g_w.spare[0], g_f[0], g_f[1], g_f[2],
                         g_i, car()));
}

static void s_wing() {
    rand_car();
    Wing* w = &g_w.wing;
    w->owner = car();
    w->span = v(0.5f, 8);
    w->chord = v(0.3f, 3);
    w->pos = {vs(4), vs(2), vs(4)};
    w->axis = (fuzz_rand() % 3 == 0) ? 1 : 0;
    w->angle = (fuzz_rand() % 4) ? vs(0.3f) : vs(3);
    if (fuzz_rand() % 8 == 0) car()->velocity = {vs(0.5f), vs(0.5f), vs(0.5f)};   // below 1 m/s
}
static uint64_t c_wing(bool orig) { CALL(0x00447070, Wing_Update, &g_w.wing, 0); return 0; }

static void s_qc() {
    QuarterCar* q = &g_w.qc;
    q->body_mass = v(100, 600);
    q->wheel_mass = v(10, 60);
    q->spring_rate = v(1e4f, 1e5f);
    q->tyre_rate = v(1e5f, 5e5f);
    q->damper = &g_w.damper;
    g_w.damper = {vs(3000), v(0, 5000), vs(3000), v(0, 5000)};
    q->body_pos = vs(0.5f);
    q->wheel_pos = vs(0.2f);
    q->body_vel = vs(3);
    q->wheel_vel = vs(5);
    q->load_error = v(0, 100);
    q->grip = v(0, 1000);
    q->contact_steps = fuzz_rand() % 10000;
    q->steps = fuzz_rand() % 10000;
    g_f[0] = v(0.0005f, 0.02f);
    g_f[1] = vs(0.2f);
}
static uint64_t c_qc(bool orig) { CALL(0x00447aa0, QuarterCar_Step, &g_w.qc, 0, g_f[0], g_f[1]); return 0; }

// the road table: pos kept where the original's unwrapped first index stays inside the object (or NaN, which
// __ftol turns into index 0), speed x dt small enough that the wrap loop ends
static void s_terrain() {
    TableTerrain* t = &g_t;
    t->vtable = (void**)0x004dc850;
    t->spacing = (fuzz_rand() % 4) ? 0.25f : rf(0.05f, 2.0f);
    t->length = (fuzz_rand() % 4) ? t->spacing * 4096.0f : rf(1.0f, t->spacing * 4096.0f);
    uint32_t k = fuzz_rand() % 20;
    t->pos = k == 0 ? bitsf(0x7fc00000u | (fuzz_rand() & 0x3fffff)) : k == 1 ? -rf(0, t->spacing * 3)
                                                                          : rf(0, t->length * 0.9999f);
    t->height = vs(1);
    for (int i = 0; i < 4096; i += 1 + (int)(fuzz_rand() % 64)) t->table[i] = (fuzz_rand() % 200) ? vs(0.3f) : special();
    g_f[0] = rf(0, 0.05f);
    g_f[1] = rf(-10, 200);
    if (!(t->pos == t->pos)) g_f[1] = 0.0f;         // NaN pos: NaN >= length is false, no wrap either way
}
static uint64_t c_reset(bool orig) { CALL(0x00447490, TableTerrain_Reset, &g_t, 0); return 0; }
static uint64_t c_height(bool orig) { return ret_bits(CALL(0x00447540, TableTerrain_GetHeight, &g_t, 0)); }
static uint64_t c_tstep(bool orig) { CALL(0x004474a0, TableTerrain_Step, &g_t, 0, g_f[0], g_f[1]); return 0; }

// ---- the original, loaded at 0x400000 (as test/fuzz.cpp) ----------------------------------------------------
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
    VirtualAllocEx(pi.hProcess, (void*)0x400000, 0x300000, MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2;
    GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iters = argc > 1 ? atoi(argv[1]) : 200000;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);                          // ...\test\world_engine.cpp (built /FC)
    char* s = strstr(exe, "\\test\\world_engine.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    rand_bytes(&g_w, sizeof g_w);
    rand_bytes(&g_t, sizeof g_t);
    int failed = 0;
    failed += run("Engine::Engine", iters, s_ctor, c_ctor);
    failed += run("Engine::Setup", iters, s_setup, c_setup);
    failed += run("Engine::ApplyTorque", iters, s_apply, c_apply);
    failed += run("Engine::UpdateHeat", iters, s_heat, c_heat);
    failed += run("HeatTube::Update", iters, s_tube, c_tube);
    failed += run("Propeller::Propeller", iters, s_prop_ctor, c_prop_ctor);
    failed += run("Propeller::ApplyTorque", iters, s_prop, c_prop);
    failed += run("Wing::Wing", iters, s_wing_ctor, c_wing_ctor);
    failed += run("Wing::Update", iters, s_wing, c_wing);
    failed += run("QuarterCar::Step", iters, s_qc, c_qc);
    failed += run("TableTerrain::Reset", iters / 10, s_terrain, c_reset, true);
    failed += run("TableTerrain::GetHeight", iters / 10, s_terrain, c_height, true);
    failed += run("TableTerrain::Step", iters / 10, s_terrain, c_tstep, true);
    printf("%d differ\n", failed);
    return failed ? 1 : 0;
}
