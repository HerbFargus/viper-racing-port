// world_wheel.cpp -- the world harness for Wheel::Update (hook/phys_wheel_update.cpp): the original and the
// rewrite, run on the same random cars, compared byte for byte.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_wheel.cpp
//        /Fo<dir>\ /Fe<dir>\world_wheel.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_wheel.exe [iterations] [seed]
//
// out\race_v10.exe is loaded at its own base, 0x400000, the way test/fuzz.cpp does it (a child process with
// the range reserved before its heap exists). A real Car (3768 bytes, Car's vtable) with its four Wheels and
// a Tire is built in this process's memory, filled with what Wheel::Setup / Car::Setup would leave there
// plus a random dynamic state: body frame, velocities, the force / impulse accumulators, the wheel's
// suspension, spin, slips, controls, ABS / TC / digital-throttle modes, broken, remote, surface; and the
// globals it reads (damage on, HackPaveTheWorld's two flags). Everything Wheel::Update calls runs as the
// original code in the image -- PhobDyno's force helpers, GetArmPosition, MatrixMulPoint,
// update_wheel_position, Damper::GetDampingRate, the Tire getters, get_impulse_magnitude -- except the two
// that reach outside the car, which are patched with a jump to a stub that logs its call:
//   TerrainGetHeight (0x465c40)  the track: returns a scripted hit (height relative to the droop point
//                                 it's asked about, normal, surface, or no hit)
//   Random (0x41b6e0)             the game's RNG: returns a scripted 0/1
// Each world is run through the original (0x448a60), the state and the stub logs kept; the starting state
// restored; then through the rewrite; and the car, tire, hub vertex and the logs compared.
// A share of the worlds ("wild") has some floats replaced by extreme values (NaN, inf, huge, tiny,
// denormal) to exercise the NaN-exact comparisons.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the rewrite, compiled into this program: PORT_FN only records its function
#include "../hook/port.h"
#undef PORT_FN
#define PORT_FN(V10, NAME, NEW, FP) \
    static void* const g_rewrite = (void*)&NEW; static void* const g_footprint = (void*)&FP;
#include "../hook/phys_wheel_update.cpp"

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) { add(obj, 3768, what); }

// ---- random numbers --------------------------------------------------------------------------------------
static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }             // [0, 1)
static float range(float a, float b) { return a + (b - a) * uni(); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float wildf() {
    uint32_t r = rnd(), k = r % 12;
    float s = (r & 0x100) ? -1.0f : 1.0f;
    switch (k) {
    case 0: return bitsf(0x7fc00000u | (rnd() & 0x3fffff));                 // quiet NaN
    case 1: return bitsf(0xffc00000u);                                       // the indefinite
    case 2: return s * INFINITY;
    case 3: return s * range(1e30f, 3e38f);
    case 4: return s * range(1e-30f, 1e-38f);
    case 5: return bitsf(rnd() & 0x807fffff);                                // denormal
    case 6: return (r & 0x200) ? 0.0f : -0.0f;
    case 7: return s * range(1e6f, 1e9f);
    case 8: return s;
    default: return bitsf(rnd());                                            // any bits at all
    }
}

// ---- the original, loaded at 0x400000 ---------------------------------------------------------------------
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
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) {
        printf("can't start the test process\n");
        return 2;
    }
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

// ---- the world ----------------------------------------------------------------------------------------------
static Car* g_car;
static Wheel* g_w;
static Tire g_tire;
static P3 g_hub[4];

// the globals Wheel::Update reads (through PhysicsIsDamageOn and HackPaveTheWorld)
static uint8_t* const g_damage_on = (uint8_t*)0x521634;
static uint8_t* const g_pave_a = (uint8_t*)0x4e52e4;
static uint8_t* const g_pave_b = (uint8_t*)0x505ae8;

// ---- the stubs' script and log ------------------------------------------------------------------------------
struct TerrainScript { uint8_t hit; float h, dx, dz; P3 n; int32_t surface; };
struct Script { TerrainScript terrain[4]; int random[4]; };
struct Log { int n; uint32_t e[32]; };
static Script g_script;
static int g_terrain_i, g_random_i;
static Log g_log;
static void log_put(uint32_t v) { if (g_log.n < 32) g_log.e[g_log.n] = v; g_log.n++; }

static uint8_t __cdecl stub_terrain(uint32_t x, uint32_t z, P3* hit, P3* n, int32_t* surface) {
    log_put(0x7e440000u);
    log_put(x);
    log_put(z);
    const TerrainScript& t = g_script.terrain[g_terrain_i++ & 3];
    if (!t.hit) return 0;
    // the hit: under (or above) the point asked about, h along world y from the droop point's height
    hit->x = bitsf(x) + t.dx;
    hit->z = bitsf(z) + t.dz;
    hit->y = g_w->droop_point_world.y + t.h;
    *n = t.n;
    *surface = t.surface;
    return 1;
}
static int __cdecl stub_random(int range) {
    log_put(0x4a4d0000u);
    log_put((uint32_t)range);
    return g_script.random[g_random_i++ & 3];
}

// ---- building a car ----------------------------------------------------------------------------------------
static void rotation(M3* m, bool upright) {
    // a random rotation: yaw, then some pitch / roll (large, even upside down, when not upright)
    double yaw = range(-3.1416f, 3.1416f);
    double lim = upright ? 0.35 : 3.1416;
    double pitch = range((float)-lim, (float)lim), roll = range((float)-lim, (float)lim);
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch), cr = cos(roll), sr = sin(roll);
    // rows: right, up, forward
    double R[9] = {cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr,
                   -cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr,
                   sy * cp, -sp, cy * cp};
    for (int i = 0; i < 9; i++) m->m[i] = (float)R[i];
}

static void setup_tire(Tire* t) {
    t->cornering_quad = range(-0.0012f, -0.0002f);
    t->cornering_lin = range(12.0f, 30.0f);
    t->friction_slope = range(-4e-5f, 0.0f);
    t->friction_base = range(0.9f, 1.3f);
    t->camber_stiffness_factor = range(0.02f, 0.2f);
    t->long_stiffness_factor = range(0.8f, 2.0f);
    t->long_friction_factor = range(0.9f, 1.2f);
    t->pacejka_b = range(0.6f, 2.0f);
    t->pacejka_c = range(1.2f, 1.9f);
    t->pacejka_d = range(0.9f, 1.1f);
    t->pacejka_e = range(-1.5f, 0.6f);
    t->load_factor = range(0.8f, 1.3f);
    t->mass = range(8.0f, 15.0f);
    t->inertia = range(0.6f, 1.6f);
    t->camber_peak = range(0.05f, 0.15f);
    t->camber_peak_gain = range(0.0f, 0.2f);
}

static void setup_wheel(Wheel* w, int i) {
    const float side = (i & 1) ? 1.0f : -1.0f;                  // 0 and 2 on the -x side
    const bool front = i < 2;
    memset(w, 0, sizeof *w);
    w->vtable = (void**)0x004dca78;
    P3* hv = &g_hub[i];
    hv->x = side * range(0.7f, 0.85f); hv->y = range(-0.3f, -0.1f); hv->z = (front ? 1.0f : -1.0f) * range(1.2f, 1.5f);
    w->hub_vertex = hv;
    w->hub_vertex_rest = *hv;
    if (chance(15)) {                                           // dented
        float d = chance(50) ? 0.04f : 0.08f;
        hv->x += range(-d, d); hv->y += range(-d, d); hv->z += range(-d, d);
    }
    w->droop_offset.x = side * range(0.7f, 0.8f) - w->hub_vertex_rest.x;
    w->droop_offset.y = -range(0.45f, 0.7f) - w->hub_vertex_rest.y;
    w->droop_offset.z = (front ? 1.0f : -1.0f) * range(1.2f, 1.5f) - w->hub_vertex_rest.z;
    w->broken = chance(10);
    w->damper.bump_slope = range(-3000.0f, 0.0f);
    w->damper.bump_base = range(1000.0f, 6000.0f);
    w->damper.rebound_slope = chance(80) ? 0.0f : range(-500.0f, 500.0f);
    w->damper.rebound_base = range(1000.0f, 8000.0f);
    w->inertia = range(0.8f, 12.0f);
    w->inv_inertia = 1.0f / w->inertia;
    w->spring_rate = range(20000.0f, 120000.0f);
    w->spring_preload = chance(80) ? 0.0f : range(-500.0f, 500.0f);
    w->static_deflection = range(0.03f, 0.1f);
    w->travel = range(0.08f, 0.25f) + w->static_deflection;
    w->bump_stop_rate = w->spring_rate;
    w->brake_torque_max = range(500.0f, 5000.0f);
    w->roll_resist_speed = range(0.0f, 0.004f);
    w->roll_resist_static = range(0.0f, 0.004f);
    w->radius = range(0.28f, 0.4f);
    w->static_camber = -side * range(0.0f, 0.04f);
    w->static_toe = -side * range(-0.01f, 0.01f);
    w->steer_quadratic = 0.0f;
    w->bump_steer = side * range(-0.2f, 0.2f);
    w->bump_camber = side * range(-0.5f, 0.5f);
    w->toe_compliance = side * range(-2e-6f, 2e-6f);
    w->camber_compliance = side * range(-2e-6f, 2e-6f);
    w->caster = front ? range(0.0f, 0.15f) : 0.0f;
    w->susp_axis.x = 0.0f; w->susp_axis.y = range(0.95f, 1.0f); w->susp_axis.z = range(-0.2f, 0.2f);
    w->tire = &g_tire;
    w->tire_load_factor = g_tire.load_factor;
    w->grip_scale = range(0.8f, 1.2f);
    w->stiffness_scale = range(0.8f, 1.2f);
    w->road_noise_quad = chance(30) ? 0.0f : range(0.0f, 2e-5f);
    w->road_noise_lin = chance(30) ? 0.0f : range(0.0f, 1e-3f);
    w->brake_cooling = 134.0f;
    w->brake_heat_capacity = 1800.0f;
    w->inv_brake_heat_capacity = 1.0f / 1800.0f;
    w->brake_temp = 290.0f;
}

// the dynamic state: what a tick in the middle of a race could find
static void randomize_wheel(Wheel* w, const M3* rot, bool front) {
    w->aligning_torque = range(-50.0f, 50.0f);
    w->spin_angle = range(-6.4f, 6.4f);
    if (chance(3)) w->spin_angle = bitsf(chance(50) ? 0x40c90fdc : 0xc0c90fdc);   // just past the wrap
    w->steer_input = front ? range(-0.5f, 0.5f) : 0.0f;
    float st = w->steer_input + w->static_toe;
    w->steer_angle = st;
    w->heading.x = sinf(st); w->heading.y = 0.0f; w->heading.z = cosf(st);
    w->camber = range(-0.08f, 0.08f);
    w->brake_input = chance(50) ? 0.0f : range(-1.0f, 1.0f);
    w->drive_torque = chance(40) ? 0.0f : range(-1500.0f, 6000.0f);
    w->compression = range(-0.1f, w->travel + 0.3f);
    w->lat_force_ratio = range(-1.5f, 1.5f);
    w->long_force_ratio = range(-1.5f, 1.5f);
    w->tire_force.x = range(-9000.0f, 9000.0f); w->tire_force.y = range(-9000.0f, 9000.0f); w->tire_force.z = range(-9000.0f, 9000.0f);
    w->chassis_tire_force.x = range(-6000.0f, 6000.0f); w->chassis_tire_force.y = range(-6000.0f, 6000.0f);
    w->chassis_tire_force.z = range(-6000.0f, 6000.0f);
    w->lat_force_filtered = range(-8000.0f, 8000.0f);
    // the suspension axis in the world, as update_wheel_position left it last tick
    const float* m = rot->m;
    w->susp_axis_world.x = m[3] * w->susp_axis.y + m[6] * w->susp_axis.z + m[0] * w->susp_axis.x;
    w->susp_axis_world.y = m[7] * w->susp_axis.z + m[1] * w->susp_axis.x + m[4] * w->susp_axis.y;
    w->susp_axis_world.z = m[2] * w->susp_axis.x + m[8] * w->susp_axis.z + m[5] * w->susp_axis.y;
    w->slide = chance(30) ? 0.0f : range(0.0f, 3.0f);
    w->fx_flags = (uint8_t)rnd();
    w->surface_class = (uint8_t)(rnd() % 6);
    int k = rnd() % 10;
    w->omega = k < 2 ? range(-9.0f, 9.0f) : k < 3 ? 0.0f : range(-220.0f, 220.0f);
    w->prev_omega = w->omega + range(-5.0f, 5.0f);
    w->prev_axle.x = m[0] + range(-0.05f, 0.05f); w->prev_axle.y = m[1] + range(-0.05f, 0.05f); w->prev_axle.z = m[2];
    w->contact_point = w->droop_point_world;
    w->contact_point.x += range(-0.5f, 0.5f); w->contact_point.y += range(-0.5f, 0.5f); w->contact_point.z += range(-0.5f, 0.5f);
    w->anti_roll_force = chance(40) ? 0.0f : range(-4000.0f, 4000.0f);
    w->traction_control = rnd() % 3;
    w->digital_throttle = chance(40);
    w->abs_mode = rnd() % 3;
    w->on_ground = chance(75);
    w->lat_slip = range(-3.0f, 3.0f);
    w->long_slip = chance(20) ? range(-0.2f, 0.2f) : range(-3.0f, 3.0f);
    w->susp_force = chance(20) ? range(0.0f, 400.0f) : range(0.0f, 18000.0f);
    w->drag_torque = range(0.0f, 600.0f);
    w->long_slip_raw = range(-3.0f, 3.0f);
    static const int surfaces[] = {0, 0, 0, 0, 10, 11, 12, 13, 14, 15, 20, 21, 23, 1, 5, 9, 16, 22, 24, 99, -1};
    w->surface = surfaces[rnd() % (sizeof surfaces / sizeof *surfaces)];
    w->is_remote = chance(10);
}

static void randomize_world(int wheel_index) {
    Car* car = g_car;
    memset(car, 0, sizeof *car);
    car->vtable = (void**)0x004dbfd0;
    rotation(&car->frame.rot, !chance(10));
    car->frame.pos.x = range(-2000.0f, 2000.0f); car->frame.pos.y = range(-50.0f, 300.0f); car->frame.pos.z = range(-2000.0f, 2000.0f);
    int k = rnd() % 10;
    float vs = k < 2 ? 0.2f : k < 4 ? 8.0f : 80.0f;             // near still / slow / racing
    car->velocity.x = range(-vs, vs); car->velocity.y = range(-vs * 0.2f, vs * 0.2f); car->velocity.z = range(-vs, vs);
    if (chance(15)) {                                           // mostly forwards
        const float* m = car->frame.rot.m;
        float s = range(-10.0f, 60.0f);
        car->velocity.x = m[6] * s; car->velocity.y = m[7] * s; car->velocity.z = m[8] * s;
    }
    car->angular_velocity.x = range(-2.0f, 2.0f); car->angular_velocity.y = range(-2.0f, 2.0f); car->angular_velocity.z = range(-2.0f, 2.0f);
    car->mass = range(1100.0f, 1700.0f);
    car->inv_mass = 1.0f / car->mass;
    for (int i = 0; i < 9; i++) car->inv_inertia_world.m[i] = (i % 4 == 0) ? range(2e-4f, 1e-3f) : range(-5e-5f, 5e-5f);
    car->force.x = range(-5e4f, 5e4f); car->force.y = range(-5e4f, 5e4f); car->force.z = range(-5e4f, 5e4f);
    car->torque.x = range(-2e4f, 2e4f); car->torque.y = range(-2e4f, 2e4f); car->torque.z = range(-2e4f, 2e4f);
    car->impulse.x = range(-100.0f, 100.0f); car->impulse.y = range(-100.0f, 100.0f); car->impulse.z = range(-100.0f, 100.0f);
    car->impulse_weight = range(0.0f, 10.0f);
    car->realism = rnd() % 3;
    setup_tire(&g_tire);
    for (int i = 0; i < 4; i++) {
        setup_wheel(&car->wheels[i], i);
        // last tick's droop point: near the body, below the hub
        Wheel* w = &car->wheels[i];
        w->droop_point_world.x = car->frame.pos.x + range(-1.5f, 1.5f);
        w->droop_point_world.y = car->frame.pos.y + range(-0.7f, -0.2f);
        w->droop_point_world.z = car->frame.pos.z + range(-1.5f, 1.5f);
        randomize_wheel(w, &car->frame.rot, i < 2);
    }
    g_w = &car->wheels[wheel_index];
    *g_damage_on = chance(70);
    *g_pave_a = chance(20);
    *g_pave_b = chance(50);

    // the scripted world outside the car
    for (int i = 0; i < 4; i++) {
        TerrainScript& t = g_script.terrain[i];
        t.hit = chance(92);
        // how far the ground is above the droop point: below it (off), in the travel, past the bump stop, beyond
        int c = rnd() % 10;
        Wheel* w = g_w;
        t.h = c < 2 ? range(-0.5f, 0.0f) : c < 7 ? range(0.0f, w->travel) : c < 9 ? range(w->travel - 0.03f, w->travel + 0.1f)
                                                                                : range(0.0f, 4.0f);
        t.dx = range(-0.02f, 0.02f);
        t.dz = range(-0.02f, 0.02f);
        int nk = rnd() % 10;
        double nx, ny, nz;
        if (nk < 6) { nx = range(-0.15f, 0.15f); nz = range(-0.15f, 0.15f); ny = 1.0; }         // nearly flat
        else if (nk < 8) { nx = range(-1.0f, 1.0f); nz = range(-1.0f, 1.0f); ny = range(0.0f, 1.0f); }   // steep
        else if (nk < 9) { nx = range(-1.0f, 1.0f); nz = range(-1.0f, 1.0f); ny = range(-1.0f, 0.0f); }  // overhang
        else { nx = 0.0; nz = 0.0; ny = 1.0; }
        double l = sqrt(nx * nx + ny * ny + nz * nz);
        if (chance(90)) { nx /= l; ny /= l; nz /= l; }                 // (else: a bad normal, for the assert)
        t.n.x = (float)nx; t.n.y = (float)ny; t.n.z = (float)nz;
        static const int surfaces[] = {0, 0, 0, 0, 0, 10, 10, 11, 12, 13, 14, 15, 20, 21, 23, 1, 2, 5, 9, 16, 22, 24, 100, -3};
        t.surface = surfaces[rnd() % (sizeof surfaces / sizeof *surfaces)];
    }
    for (int i = 0; i < 4; i++) g_script.random[i] = rnd() & 1;
}

// some floats of the car and the wheel under test replaced with extreme values
static void wildify() {
    uint8_t* w = (uint8_t*)g_w;
    int n = 1 + rnd() % 4;
    for (int i = 0; i < n; i++) {
        // any float field of the wheel except its pointers, vtable and the byte / int fields
        uint32_t off;
        do off = 4 + 4 * (rnd() % ((sizeof(Wheel) - 4) / 4));
        while (off == 0x10 || off == 0x20 || off == 0x8c || off == 0x120 || off == 0x15c || off == 0x160 ||
               off == 0x164 || off == 0x168 || off == 0x184 || off == 0x19c || off == 0x1a0);
        float f = wildf();
        memcpy(w + off, &f, 4);
    }
    if (chance(30)) {
        float* cf = (float*)((uint8_t*)g_car + 0x38);           // frame, then PhobDyno's floats
        static const int offs[] = {0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x64, 0x1f0,
                                   0x234, 0x238, 0x23c, 0x240, 0x244, 0x248, 0x264, 0x268, 0x284};
        (void)cf;
        int off = offs[rnd() % (sizeof offs / sizeof *offs)];
        float f = wildf();
        memcpy((uint8_t*)g_car + off, &f, 4);
    }
    if (chance(20)) {
        float* t = (float*)&g_tire;
        t[rnd() % 16] = wildf();
    }
    if (chance(20)) {
        TerrainScript& t = g_script.terrain[0];
        float* p = chance(50) ? &t.h : chance(50) ? &t.n.x : &t.n.y;
        *p = wildf();
    }
}

// ---- running both --------------------------------------------------------------------------------------------
struct State {
    uint8_t car[sizeof(Car)];
    Tire tire;
    P3 hub[4];
    uint8_t damage_on, pave_a, pave_b;
    Log log;
};
static void save(State& s) {
    memcpy(s.car, g_car, sizeof(Car));
    s.tire = g_tire;
    memcpy(s.hub, g_hub, sizeof g_hub);
    s.damage_on = *g_damage_on; s.pave_a = *g_pave_a; s.pave_b = *g_pave_b;
    s.log = g_log;
}
static void load(const State& s) {
    memcpy(g_car, s.car, sizeof(Car));
    g_tire = s.tire;
    memcpy(g_hub, s.hub, sizeof g_hub);
    *g_damage_on = s.damage_on; *g_pave_a = s.pave_a; *g_pave_b = s.pave_b;
}

typedef void(__fastcall* Update_t)(Wheel*, int, Car*);
static int run_guarded(Update_t fn) {
    g_log.n = 0;
    g_terrain_i = g_random_i = 0;
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, _PC_24, _MCW_PC);
    __try { fn(g_w, 0, g_car); } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    return 0;
}

// a name for a byte of the car
static const char* name_of(uint32_t off, char* buf) {
    struct F { uint32_t off, size; const char* name; };
    static const F wheel_fields[] = {
        {0x00, 4, "vtable"}, {0x04, 12, "droop_offset"}, {0x10, 4, "hub_vertex"}, {0x14, 12, "hub_vertex_rest"},
        {0x20, 4, "broken"}, {0x24, 16, "damper"}, {0x34, 4, "inertia"}, {0x38, 4, "inv_inertia"},
        {0x3c, 4, "spring_rate"}, {0x40, 4, "spring_preload"}, {0x44, 4, "travel"}, {0x48, 4, "static_deflection"},
        {0x4c, 4, "bump_stop_rate"}, {0x50, 4, "brake_torque_max"}, {0x54, 4, "roll_resist_speed"},
        {0x58, 4, "roll_resist_static"}, {0x5c, 4, "radius"}, {0x60, 32, "(static geometry)"}, {0x80, 12, "susp_axis"},
        {0x8c, 4, "tire"}, {0x90, 4, "tire_load_factor"}, {0x94, 4, "grip_scale"}, {0x98, 4, "stiffness_scale"},
        {0x9c, 4, "road_noise_quad"}, {0xa0, 4, "road_noise_lin"}, {0xa4, 4, "aligning_torque"},
        {0xa8, 4, "surface_speed"}, {0xac, 4, "spin_angle"}, {0xb0, 12, "heading"}, {0xbc, 12, "replay_velocity"},
        {0xc8, 12, "droop_point_world"}, {0xd4, 4, "steer_angle"}, {0xd8, 4, "camber"}, {0xdc, 4, "steer_input"},
        {0xe0, 4, "brake_input"}, {0xe4, 4, "drive_torque"}, {0xe8, 4, "compression"}, {0xec, 4, "lat_force_ratio"},
        {0xf0, 4, "long_force_ratio"}, {0xf4, 12, "tire_force"}, {0x100, 12, "chassis_tire_force"},
        {0x10c, 4, "lat_force_filtered"}, {0x110, 12, "susp_axis_world"}, {0x11c, 4, "slide"}, {0x120, 1, "fx_flags"},
        {0x121, 3, "surface_class"}, {0x124, 4, "omega"}, {0x128, 4, "rpm"}, {0x12c, 4, "brake_power"},
        {0x130, 12, "contact_point"}, {0x13c, 4, "prev_omega"}, {0x140, 12, "prev_axle"}, {0x14c, 12, "_14c"},
        {0x158, 4, "anti_roll_force"}, {0x15c, 4, "traction_control"}, {0x160, 4, "digital_throttle"},
        {0x164, 4, "abs_mode"}, {0x168, 4, "on_ground"}, {0x16c, 4, "_16c"}, {0x170, 4, "lat_slip"},
        {0x174, 4, "long_slip"}, {0x178, 4, "susp_force"}, {0x17c, 4, "drag_torque"}, {0x180, 4, "long_slip_raw"},
        {0x184, 32, "(brake heat)"}, {0x19c, 4, "surface"}, {0x1a0, 4, "is_remote"}};
    static const F car_fields[] = {
        {0x38, 36, "frame.rot"}, {0x5c, 12, "frame.pos"}, {0x234, 12, "velocity"}, {0x240, 12, "angular_velocity"},
        {0x24c, 12, "force"}, {0x258, 12, "torque"}, {0x294, 12, "impulse"}, {0x2a0, 12, "angular_impulse"},
        {0x2ac, 4, "impulse_weight"}};
    if (off >= 0x554 && off < 0x554 + 4 * sizeof(Wheel)) {
        uint32_t i = (off - 0x554) / sizeof(Wheel), o = (off - 0x554) % sizeof(Wheel);
        for (const F& f : wheel_fields)
            if (o >= f.off && o < f.off + f.size) { sprintf(buf, "wheels[%u].%s+%u", i, f.name, o - f.off); return buf; }
        sprintf(buf, "wheels[%u]+0x%x", i, o);
        return buf;
    }
    for (const F& f : car_fields)
        if (off >= f.off && off < f.off + f.size) { sprintf(buf, "car.%s+%u", f.name, off - f.off); return buf; }
    sprintf(buf, "car+0x%x", off);
    return buf;
}

static bool compare(const State& o, const State& n, int it) {
    bool same = true;
    char buf[96];
    for (uint32_t i = 0; i < sizeof(Car); i += 4)
        if (memcmp(o.car + i, n.car + i, 4)) {
            uint32_t a, b;
            memcpy(&a, o.car + i, 4);
            memcpy(&b, n.car + i, 4);
            if (same) printf("  MISMATCH, world %d (wheel %d)\n", it, (int)(g_w - g_car->wheels));
            printf("    %-34s original %08x (%.9g)  rewrite %08x (%.9g)\n", name_of(i, buf), a, bitsf(a), b, bitsf(b));
            same = false;
        }
    if (memcmp(&o.tire, &n.tire, sizeof o.tire) || memcmp(o.hub, n.hub, sizeof o.hub)) {
        if (same) printf("  MISMATCH, world %d\n", it);
        printf("    the tire or a hub vertex differs\n");
        same = false;
    }
    if (o.log.n != n.log.n || memcmp(o.log.e, n.log.e, 4 * (o.log.n < 32 ? o.log.n : 32))) {
        if (same) printf("  MISMATCH, world %d\n", it);
        printf("    the stub logs differ: %d / %d entries\n", o.log.n, n.log.n);
        same = false;
    }
    return same;
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 1000000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);                        // ...\test\world_wheel.cpp (built /FC) -> ...\out\race_v10.exe
    char* s = strstr(exe, "\\test\\world_wheel.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    patch_jmp(0x00465c40, (void*)&stub_terrain);
    patch_jmp(0x0041b6e0, (void*)&stub_random);
    g_car = (Car*)VirtualAlloc(0, sizeof(Car), MEM_COMMIT, PAGE_READWRITE);
    {   // the footprint names the car
        Footprint* fp = new Footprint;
        g_w = &g_car->wheels[0];
        ((void (*)(Footprint&, Wheel*, Edx, Car*))g_footprint)(*fp, g_w, 0, g_car);
        printf("footprint: %d region(s), first %p +%u \"%s\"\n", fp->n, fp->r[0].p, fp->r[0].n, fp->r[0].what);
        delete fp;
    }

    static State start, orig, rew;
    int differ = 0, faults = 0, wild_runs = 0, wild_differ = 0;
    // what the worlds covered (counted from the original's result)
    int c_ground = 0, c_air_pulled = 0, c_impulse = 0, c_broke = 0, c_capped = 0, c_realism = 0, c_abs = 0,
        c_tc = 0, c_lock = 0, c_slow = 0, c_surface[25] = {0}, c_nohit = 0, c_random = 0;
    for (int it = 0; it < iterations; it++) {
        int wi = rnd() % 4;
        randomize_world(wi);
        bool wild = chance(10);
        if (wild) { wildify(); wild_runs++; }
        Wheel w0 = *g_w;
        save(start);
        int fo = run_guarded((Update_t)0x00448a60);
        save(orig);
        orig.log = g_log;
        load(start);
        int fn = run_guarded((Update_t)g_rewrite);
        save(rew);
        rew.log = g_log;
        if (fo || fn) {
            if (fo != fn) { printf("  world %d: original %s, rewrite %s\n", it, fo ? "faulted" : "ran", fn ? "faulted" : "ran"); differ++; }
            faults++;
            continue;
        }
        if (!compare(orig, rew, it)) {
            differ++;
            if (wild) wild_differ++;
            if (differ >= 20) { printf("stopping after 20 differing worlds\n"); break; }
        }
        // coverage, from the original's pass
        const Car* oc = (const Car*)orig.car;
        const Wheel& ow = oc->wheels[wi];
        if (ow.on_ground) c_ground++;
        if (!ow.on_ground && orig.log.n >= 3 && g_script.terrain[0].hit) c_air_pulled++;
        if (memcmp(&oc->impulse, &((const Car*)start.car)->impulse, 12)) c_impulse++;
        if (ow.broken && !w0.broken) c_broke++;
        if (fabsf(ow.tire_force.x * ow.tire_force.x + ow.tire_force.y * ow.tire_force.y + ow.tire_force.z * ow.tire_force.z - 1e8f) < 1e5f) c_capped++;
        if (ow.on_ground && oc->realism == 0) c_realism++;
        if (w0.abs_mode && fbits(w0.brake_input) - 1u < 0x7f7fffffu && !w0.broken) c_abs++;
        if ((w0.traction_control || w0.digital_throttle) && fbits(w0.drive_torque) - 1u < 0x7f7fffffu) c_tc++;
        if (fabsf(w0.omega) < 9.0f) c_lock++;
        if (fabsf(ow.surface_speed) < 10.0f) c_slow++;
        if (ow.surface >= 0 && ow.surface < 25) c_surface[ow.surface]++;
        if (!g_script.terrain[0].hit) c_nohit++;
        for (int i = 0; i + 1 < orig.log.n && i + 1 < 32; i++) if (orig.log.e[i] == 0x4a4d0000u) c_random++;
    }
    printf("%d worlds (%d wild): %d differ (%d of them wild), %d faulted in both\n", iterations, wild_runs, differ,
           wild_differ, faults);
    printf("covered: on ground %d, hanging %d, bump-stop impulse %d, broken this tick %d, tyre force capped %d,\n"
           "  slide force (realism 0) %d, ABS armed %d, TC / digital armed %d, |omega| < 9 %d, slow wheel %d,\n"
           "  no terrain hit %d, Random calls %d\n", c_ground, c_air_pulled, c_impulse, c_broke, c_capped, c_realism,
           c_abs, c_tc, c_lock, c_slow, c_nohit, c_random);
    printf("  surfaces:");
    for (int i = 0; i < 25; i++) if (c_surface[i]) printf(" %d:%d", i, c_surface[i]);
    printf("\n");
    return differ ? 1 : 0;
}
