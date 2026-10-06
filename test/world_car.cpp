// world_car.cpp -- the world harness for the Car's per-tick update (hook/phys_car_update.cpp): the originals
// and the rewrites, run on the same random cars, compared byte for byte.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC test\world_car.cpp
//        /Fo<dir>\ /Fe<dir>\world_car.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_car.exe [iterations] [seed]
//
// out\race_v10.exe is loaded at 0x400000 the way test/fuzz.cpp does it. Two real Cars (Car's vtable, in
// AICar-sized buffers) are built in this process's memory: four Wheels and a Tire filled as Wheel::Setup
// leaves them (test/world_wheel.cpp's way), the drivetrain built with the ORIGINAL constructors and
// Setup/Connect calls as Car::Setup does (test/world_drivetrain.cpp's way), and the Car's own fields --
// realism, yaw control, race state, controls, aero, axles, drafting, sounds, damage, lowering, a plane's
// wings -- from random state; then a random dynamic state. Everything the functions call runs as the original
// code in the image -- the wheels, the drivetrain, PhobDyno's helpers and Update, the matrix functions,
// Transmission::SetGear(Auto) -- except what reaches outside the car, patched with a jump to a logging stub:
//   TerrainGetHeight (0x465c40)           the track under the wheels: a scripted hit
//   TerrainGetHeight(x,z) (0x465ae0)      SuperHelpOut's ground height: scripted
//   Random (0x41b6e0)                     scripted 0/1
//   PhysicsGetTime (0x42bc80)             scripted
//   WorldGetFocusCar (0x4626d0)           scripted
//   EngineSound::Update (0x472c60)        logged (the engine's sound samples aren't built)
//   Car::post_teleport_fadein (0x439aa0)  logged (another group's; reads the proxer table)
//   Car::reset_damage (0x439e50)          logged (rebuilds models)
//   GetBall (0x4410e0) / Ball::Throw (0x4412a0)   scripted ball, logged throw
//   the deity (0x5218ac): a RaceDeity-shaped object; RaceDeity::UpdateCar (0x443640) and TeleportToLine
//                                         (0x443a00) are patched to log their calls
// Each world picks a function; the original runs, the state and the logs are kept, the starting state is
// restored, the rewrite runs, and the cars, the wings, the sounds, the ball, the status table, the tire and
// hubs, the return value and the logs are compared. The bytes the original changed must also lie inside the
// rewrite's footprint. A share of the worlds ("wild") has floats replaced by extreme values.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GNUC__) && !defined(__clang__)
#include "vp_seh.h"                           // vp_try: the GCC stand-in for __try / __except
#endif

#include "../hook/port.h"
#undef PORT_FN_BUILDS
#define PORT_FN_BUILDS(V10, NAME, NEW, FP, PRO, PROLEN)                                                    \
    static const uint32_t VP_CAT(addr_, NEW) = V10;                                                      \
    static constexpr auto VP_CAT(fpof_, NEW) = &FP;

void Footprint::add(void* p, uint32_t bytes, const char* what) { if (n < MAX) r[n++] = {p, bytes, what}; }
void Footprint::object(void* obj, const char* what) {
    uint32_t vt = *(uint32_t*)obj;
    if (vt == 0x004dbfd0) add(obj, 3768, what);
    else if (vt == 0x004dc3b0) add(obj, 1152, what);
    else printf("footprint: %s at %p has an unknown class (vtable %08x)\n", what, obj, vt);
}

#include "../hook/phys_car_update.cpp"

// ---- random numbers --------------------------------------------------------------------------------------
static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float uni() { return (float)(rnd() & 0xffffff) / 16777216.0f; }
static float range(float a, float b) { return a + (b - a) * uni(); }
static bool chance(int pct) { return (int)(rnd() % 100) < pct; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float wildf() {
    uint32_t r = rnd(), k = r % 12;
    float s = (r & 0x100) ? -1.0f : 1.0f;
    switch (k) {
    case 0: return bitsf(0x7fc00000u | (rnd() & 0x3fffff));
    case 1: return bitsf(0xffc00000u);
    case 2: return s * INFINITY;
    case 3: return s * range(1e30f, 3e38f);
    case 4: return s * range(1e-30f, 1e-38f);
    case 5: return bitsf(rnd() & 0x807fffff);
    case 6: return (r & 0x200) ? 0.0f : -0.0f;
    case 7: return s * range(1e6f, 1e9f);
    case 8: return s;
    default: return bitsf(rnd());
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
    uint8_t* base = (uint8_t*)VirtualAlloc((void*)0x400000, nt->OptionalHeader.SizeOfImage, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
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
    if (!CreateProcessA(0, GetCommandLineA(), 0, 0, FALSE, CREATE_SUSPENDED, 0, 0, &si, &pi)) { printf("can't start the test process\n"); return 2; }
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

// ---- the full Wheel and Tire layouts (phys_wheel_update.cpp), for filling them --------------------------------
struct Damper { float bump_slope, bump_base, rebound_slope, rebound_base; };
struct Tire {
    float cornering_quad, cornering_lin, friction_slope, friction_base, camber_stiffness_factor, long_stiffness_factor,
        long_friction_factor, pacejka_b, pacejka_c, pacejka_d, pacejka_e, load_factor, mass, inertia, camber_peak, camber_peak_gain;
};
struct FullWheel {
    void** vtable; P3 droop_offset; P3* hub_vertex; P3 hub_vertex_rest; uint8_t broken, _021[3]; Damper damper;
    float inertia, inv_inertia, spring_rate, spring_preload, travel, static_deflection, bump_stop_rate, brake_torque_max,
        roll_resist_speed, roll_resist_static, radius, static_camber, static_toe, steer_quadratic, bump_steer, bump_camber,
        toe_compliance, camber_compliance, caster;
    P3 susp_axis; Tire* tire; float tire_load_factor, grip_scale, stiffness_scale, road_noise_quad, road_noise_lin,
        aligning_torque, surface_speed, spin_angle;
    P3 heading, replay_velocity, droop_point_world;
    float steer_angle, camber, steer_input, brake_input, drive_torque, compression, lat_force_ratio, long_force_ratio;
    P3 tire_force, chassis_tire_force; float lat_force_filtered; P3 susp_axis_world; float slide;
    uint8_t fx_flags, surface_class, _122[2]; float omega, rpm, brake_power; P3 contact_point; float prev_omega; P3 prev_axle;
    uint32_t _14c[3]; float anti_roll_force; int32_t traction_control; uint8_t digital_throttle, _161[3]; int32_t abs_mode;
    uint8_t on_ground, _169[3]; uint32_t _16c; float lat_slip, long_slip, susp_force, drag_torque, long_slip_raw;
    void* tire_sound; float brake_cooling, brake_heat_capacity, inv_brake_heat_capacity, brake_temp, heat_accum;
    int32_t surface; uint8_t is_remote, _1a1[3];
};
static_assert(sizeof(FullWheel) == 0x1a4, "FullWheel");

// ---- the world ----------------------------------------------------------------------------------------------
enum { CAR_BUF = 4224, NSOUND = 7, NSTATUS = 16 * 368 };
static uint8_t* g_carbuf[2];
static Car* car(int i) { return (Car*)g_carbuf[i]; }
static Tire g_tire;
static P3 g_hub[2][4];
static uint8_t g_wings[2][4][60];                // each car its own
static uint8_t g_sounds[NSOUND][64];             // start, shift, road, road2, scrape, horn, engine-sound idle
static uint8_t g_engine_sound[248];
static uint8_t g_ball[1152];
static uint8_t g_deity_obj[2012];
static uint8_t* const g_status = (uint8_t*)0x005540c2;
static uint8_t* const g_damage_on = (uint8_t*)0x521634;
static uint8_t* const g_pave_a = (uint8_t*)0x4e52e4;
static uint8_t* const g_pave_b = (uint8_t*)0x505ae8;
static uint8_t* const g_hornball = (uint8_t*)0x505b40;

// ---- stubs: scripts and the log ------------------------------------------------------------------------------
struct TerrainScript { uint8_t hit; float h, dx, dz; P3 n; int32_t surface; };
struct Script {
    TerrainScript terrain[8]; int random[8]; float time; int focus_car; uint8_t teleport_ok; uint8_t have_ball;
    float ground_y; float super_h;
};
struct Log { int n; uint32_t e[128]; };
static Script g_script;
static int g_terrain_i, g_random_i;
static Log g_log;
static void log_put(uint32_t v) { if (g_log.n < 128) g_log.e[g_log.n] = v; g_log.n++; }

static uint8_t __cdecl stub_terrain(uint32_t x, uint32_t z, P3* hit, P3* n, int32_t* surface) {
    log_put(0x7e440000u); log_put(x); log_put(z);
    const TerrainScript& t = g_script.terrain[g_terrain_i++ & 7];
    if (!t.hit) return 0;
    hit->x = bitsf(x) + t.dx;
    hit->z = bitsf(z) + t.dz;
    hit->y = g_script.ground_y + t.h;
    *n = t.n;
    *surface = t.surface;
    return 1;
}
static int __cdecl stub_random(int r) { log_put(0x4a4d0000u); log_put((uint32_t)r); return g_script.random[g_random_i++ & 7]; }
static float __cdecl stub_time() { log_put(0x71000000u); return g_script.time; }
static int __cdecl stub_focus() { log_put(0x70c00000u); return g_script.focus_car; }
static void __fastcall stub_engine_sound(void* self, int, uint8_t stalled) { log_put(0x35000000u); log_put((uint32_t)self); log_put(stalled); }
static void __fastcall stub_fadein(void* self, int) { log_put(0xfade0000u); log_put((uint32_t)self); }
static void __fastcall stub_reset_damage(void* self, int) { log_put(0xda3a0000u); log_put((uint32_t)self); }
static void* __cdecl stub_getball(int i) { log_put(0xba110000u); log_put((uint32_t)i); return g_script.have_ball ? g_ball : 0; }
static void __fastcall stub_throw(void* self, int, const Frame* fr, const P3* v, float t) {
#if defined(__GNUC__) && !defined(__clang__)
    // (in the blimp's view SetHorn throws with its own local zero vector: that address is its frame's layout, which
    // only the MSVC build shares with the original -- a stack address is logged as one marker; the contents below)
    const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
    const bool on_stack = (const void*)v >= tib->StackLimit && (const void*)v < tib->StackBase;
    log_put(0x7c000000u); log_put((uint32_t)self); log_put((uint32_t)fr); log_put(on_stack ? 0x57ac0000u : (uint32_t)v); log_put(fbits(t));
#else
    log_put(0x7c000000u); log_put((uint32_t)self); log_put((uint32_t)fr); log_put((uint32_t)v); log_put(fbits(t));
#endif
    if (v) { log_put(fbits(v->x)); log_put(fbits(v->y)); log_put(fbits(v->z)); }
}
static float __cdecl stub_terrain_xz(uint32_t x, uint32_t z) { log_put(0x7e450000u); log_put(x); log_put(z); return g_script.super_h; }
static void __fastcall stub_deity_update(void* self, int, int idx, void* c) { log_put(0xde170000u); log_put((uint32_t)self); log_put(idx); log_put((uint32_t)c); }
static uint8_t __fastcall stub_deity_teleport(void* self, int, int idx, void* c) {
    log_put(0xde170001u); log_put((uint32_t)self); log_put(idx); log_put((uint32_t)c);
    return g_script.teleport_ok;
}

// ---- building a car ----------------------------------------------------------------------------------------
static void rotation(M3* m, bool upright) {
    double yaw = range(-3.1416f, 3.1416f);
    double lim = upright ? 0.35 : 3.1416;
    double pitch = range((float)-lim, (float)lim), roll = range((float)-lim, (float)lim);
    double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch), cr = cos(roll), sr = sin(roll);
    double R[9] = {cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr,
                   -cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr,
                   sy * cp, -sp, cy * cp};
    for (int i = 0; i < 9; i++) m->m[i] = (float)R[i];
}
static void setup_tire(Tire* t) {
    t->cornering_quad = range(-0.0012f, -0.0002f); t->cornering_lin = range(12.0f, 30.0f);
    t->friction_slope = range(-4e-5f, 0.0f); t->friction_base = range(0.9f, 1.3f);
    t->camber_stiffness_factor = range(0.02f, 0.2f); t->long_stiffness_factor = range(0.8f, 2.0f);
    t->long_friction_factor = range(0.9f, 1.2f); t->pacejka_b = range(0.6f, 2.0f); t->pacejka_c = range(1.2f, 1.9f);
    t->pacejka_d = range(0.9f, 1.1f); t->pacejka_e = range(-1.5f, 0.6f); t->load_factor = range(0.8f, 1.3f);
    t->mass = range(8.0f, 15.0f); t->inertia = range(0.6f, 1.6f); t->camber_peak = range(0.05f, 0.15f);
    t->camber_peak_gain = range(0.0f, 0.2f);
}
static void setup_wheel(FullWheel* w, int i, P3* hv) {
    const float side = (i & 1) ? 1.0f : -1.0f;
    const bool front = i < 2;
    memset(w, 0, sizeof *w);
    w->vtable = (void**)0x004dca78;
    hv->x = side * range(0.7f, 0.85f); hv->y = range(-0.3f, -0.1f); hv->z = (front ? 1.0f : -1.0f) * range(1.2f, 1.5f);
    w->hub_vertex = hv;
    w->hub_vertex_rest = *hv;
    if (chance(15)) { float d = chance(50) ? 0.04f : 0.08f; hv->x += range(-d, d); hv->y += range(-d, d); hv->z += range(-d, d); }
    w->droop_offset.x = side * range(0.7f, 0.8f) - w->hub_vertex_rest.x;
    w->droop_offset.y = -range(0.45f, 0.7f) - w->hub_vertex_rest.y;
    w->droop_offset.z = (front ? 1.0f : -1.0f) * range(1.2f, 1.5f) - w->hub_vertex_rest.z;
    w->broken = chance(10);
    w->damper.bump_slope = range(-3000.0f, 0.0f); w->damper.bump_base = range(1000.0f, 6000.0f);
    w->damper.rebound_slope = chance(80) ? 0.0f : range(-500.0f, 500.0f); w->damper.rebound_base = range(1000.0f, 8000.0f);
    w->inertia = range(0.8f, 12.0f); w->inv_inertia = 1.0f / w->inertia;
    w->spring_rate = range(20000.0f, 120000.0f); w->spring_preload = chance(80) ? 0.0f : range(-500.0f, 500.0f);
    w->static_deflection = range(0.03f, 0.1f); w->travel = range(0.08f, 0.25f) + w->static_deflection;
    w->bump_stop_rate = w->spring_rate; w->brake_torque_max = range(500.0f, 5000.0f);
    w->roll_resist_speed = range(0.0f, 0.004f); w->roll_resist_static = range(0.0f, 0.004f);
    w->radius = range(0.28f, 0.4f); w->static_camber = -side * range(0.0f, 0.04f); w->static_toe = -side * range(-0.01f, 0.01f);
    w->bump_steer = side * range(-0.2f, 0.2f); w->bump_camber = side * range(-0.5f, 0.5f);
    w->toe_compliance = side * range(-2e-6f, 2e-6f); w->camber_compliance = side * range(-2e-6f, 2e-6f);
    w->caster = front ? range(0.0f, 0.15f) : 0.0f;
    w->susp_axis.x = 0.0f; w->susp_axis.y = range(0.95f, 1.0f); w->susp_axis.z = range(-0.2f, 0.2f);
    w->tire = &g_tire; w->tire_load_factor = g_tire.load_factor;
    w->grip_scale = range(0.8f, 1.2f); w->stiffness_scale = range(0.8f, 1.2f);
    w->road_noise_quad = chance(30) ? 0.0f : range(0.0f, 2e-5f); w->road_noise_lin = chance(30) ? 0.0f : range(0.0f, 1e-3f);
    w->brake_cooling = 134.0f; w->brake_heat_capacity = 1800.0f; w->inv_brake_heat_capacity = 1.0f / 1800.0f;
    w->brake_temp = range(280.0f, 900.0f);
}
static void randomize_wheel(FullWheel* w, const M3* rot, bool front) {
    w->aligning_torque = range(-50.0f, 50.0f);
    w->spin_angle = range(-6.4f, 6.4f);
    w->steer_input = front ? range(-0.5f, 0.5f) : 0.0f;
    float st = w->steer_input + w->static_toe;
    w->steer_angle = st;
    w->heading.x = sinf(st); w->heading.y = 0.0f; w->heading.z = cosf(st);
    w->camber = range(-0.08f, 0.08f);
    w->brake_input = chance(50) ? 0.0f : range(-1.0f, 1.0f);
    w->drive_torque = chance(40) ? 0.0f : range(-1500.0f, 6000.0f);
    w->compression = range(-0.1f, w->travel + 0.3f);
    w->lat_force_ratio = range(-1.5f, 1.5f); w->long_force_ratio = range(-1.5f, 1.5f);
    w->tire_force.x = range(-9000.0f, 9000.0f); w->tire_force.y = range(-9000.0f, 9000.0f); w->tire_force.z = range(-9000.0f, 9000.0f);
    w->chassis_tire_force.x = range(-6000.0f, 6000.0f); w->chassis_tire_force.y = range(-6000.0f, 6000.0f); w->chassis_tire_force.z = range(-6000.0f, 6000.0f);
    w->lat_force_filtered = range(-8000.0f, 8000.0f);
    const float* m = rot->m;
    w->susp_axis_world.x = m[3] * w->susp_axis.y + m[6] * w->susp_axis.z + m[0] * w->susp_axis.x;
    w->susp_axis_world.y = m[7] * w->susp_axis.z + m[1] * w->susp_axis.x + m[4] * w->susp_axis.y;
    w->susp_axis_world.z = m[2] * w->susp_axis.x + m[8] * w->susp_axis.z + m[5] * w->susp_axis.y;
    w->slide = chance(30) ? 0.0f : range(0.0f, 3.0f);
    w->fx_flags = (uint8_t)rnd();
    w->surface_class = (uint8_t)(rnd() % 6);
    int k = rnd() % 10;
    w->omega = k < 2 ? range(-9.0f, 9.0f) : k < 3 ? 0.0f : range(-220.0f, 220.0f);
    w->rpm = w->omega * 9.549296f;
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
    w->heat_accum = range(0.0f, 5e4f);
}

// the drivetrain, built as Car::Setup does (test/world_drivetrain.cpp)
typedef void*(__fastcall* Ctor_t)(void*, int);
typedef void(__fastcall* PowerCurveSetup_t)(void*, int, float, float, float, float, float);
typedef void(__fastcall* EngineSetup_t)(void*, int, PhobDyno*, float, float, const void*, float, float);
typedef void(__fastcall* DiffSetup_t)(void*, int, PhobDyno*, float, float);
typedef void(__fastcall* Connect2_t)(void*, int, void*, void*);
typedef void(__fastcall* ClutchSetup_t)(void*, int, PhobDyno*, float);
typedef void(__fastcall* TransSetup_t)(void*, int, PhobDyno*, float, float, int, const float*, void*, void*);
typedef void(__fastcall* Connect1_t)(void*, int, void*);
static void build_drivetrain(Car* c) {
    uint8_t* b = (uint8_t*)c;
    void* engine = b + 0xbfc; void* clutch = b + 0xc7c; void* trans = b + 0xc98;
    void* diff[3] = {b + 0xd04, b + 0xd24, b + 0xd44};
    ((Ctor_t)0x00446630)(engine, 0);
    uint8_t curve[24];
    ((PowerCurveSetup_t)0x00446590)(curve, 0, range(150, 600), range(4000, 7000), range(150, 600), range(2000, 4500), range(0, 60) * 1.3515152f);
    ((EngineSetup_t)0x00446700)(engine, 0, c, range(0.05f, 0.6f), range(0.001f, 0.03f), curve, range(5000, 8500), range(500, 1200));
    ((Ctor_t)0x00445d00)(clutch, 0);
    ((Ctor_t)0x00445f60)(trans, 0);
    for (int i = 0; i < 3; i++) ((Ctor_t)0x00445c00)(diff[i], 0);
    float ratios[8];
    int n = 1 + rnd() % 6;
    float r = range(2.5f, 4.0f);
    for (int i = 0; i < 8; i++) { ratios[i] = r; r *= range(0.6f, 0.85f); }
    ((TransSetup_t)0x00445fb0)(trans, 0, c, 0x1.b0f22p-3f, range(0.0f, 0.02f), n, ratios, engine, clutch);
    ((ClutchSetup_t)0x00445d20)(clutch, 0, c, range(150, 600) * 4.0f);
    float fd = range(2.5f, 4.5f);
    ((DiffSetup_t)0x00445c20)(diff[1], 0, c, fd, range(0, 60) * 1.35725f);
    ((DiffSetup_t)0x00445c20)(diff[2], 0, c, fd, range(0, 60) * 1.35725f);
    ((DiffSetup_t)0x00445c20)(diff[0], 0, c, 1.0f, range(0, 60) * 1.35725f);
    ((Connect2_t)0x00445c40)(diff[1], 0, &c->wheels[0], &c->wheels[1]);
    ((Connect2_t)0x00445c40)(diff[2], 0, &c->wheels[2], &c->wheels[3]);
    ((Connect2_t)0x00445c40)(diff[0], 0, diff[1], diff[2]);
    int out = rnd() % 3;
    ((Connect1_t)0x00445fa0)(trans, 0, out == 0 ? diff[1] : out == 1 ? diff[2] : diff[0]);
    ((Connect2_t)0x00445d40)(clutch, 0, engine, trans);
    // state
    *(float*)(b + 0xbfc + 0x10) = range(-500, 9000);                      // engine rpm
    *(float*)(b + 0xbfc + 0x2c) = uni();                                  // throttle
    *(float*)(b + 0xbfc + 0x38) = range(0, 1.2f);                         // smoothed throttle
    c->engine.stalled = chance(20);
    c->engine.cranking = chance(30);
    *(float*)(b + 0xbfc + 0x24) = range(280, 400);                        // block temperature
    *(float*)(b + 0xbfc + 0x68) = range(280, 400);                        // coolant temperature
    c->clutch_unit.engagement = chance(30) ? 0.0f : uni();
    *(float*)(b + 0xc7c + 0x18) = range(-2000, 2000);                     // clutch torque
    *(float*)(b + 0xc98 + 0x10) = range(-1000, 9000);                     // transmission rpm
    c->transmission.gear = -1 + rnd() % (n + 2);
    c->transmission.engaged_gear = -1 + rnd() % (n + 1);
    *(float*)(b + 0xc98 + 0x18) = rnd() % 4 ? ratios[rnd() % n] : range(-4, 4);
    *(float*)(b + 0xc98 + 0x28) = range(-1000, 9000);
    *(uint8_t*)(b + 0xc98 + 0x58) = c->transmission.engaged_gear != 0 ? 1 : (uint8_t)(rnd() % 4 == 0);
    *(float*)(b + 0xc98 + 0x5c) = range(-0.2f, 1.2f);
    *(float*)(b + 0xc98 + 0x60) = range(-0.3f, 1.3f);
    *(float*)(b + 0xc98 + 0x64) = uni();
    *(int32_t*)(b + 0xc98 + 0x68) = rnd() % 16 == 0 ? -1 + rnd() % 9 : rnd() % 6;
    for (int i = 0; i < 3; i++) { *(float*)((uint8_t*)diff[i] + 0xc) = range(-3000, 3000); *(float*)((uint8_t*)diff[i] + 0x10) = range(-3000, 3000); }
}

static void setup_sound(uint8_t* s) {
    for (int i = 0; i < 64; i++) s[i] = (uint8_t)rnd();
    *(uint32_t*)s = 0x004dd6d8;
    *(float*)(s + 4) = chance(30) ? 1.0f : range(0.2f, 2.0f);
    *(float*)(s + 8) = chance(30) ? bitsf(chance(50) ? 0x3f333333 : 0x3f800000) : range(0.0f, 1.0f);
}

static void randomize_car(int ci) {
    Car* c = car(ci);
    uint8_t* b = (uint8_t*)c;
    memset(b, 0, CAR_BUF);
    c->vtable = (void**)0x004dbfd0;
    rotation(&c->frame.rot, !chance(25));
    c->frame.pos.x = range(-2000.0f, 2000.0f); c->frame.pos.y = range(-50.0f, 300.0f); c->frame.pos.z = range(-2000.0f, 2000.0f);
    if (chance(3)) {                                                          // out of bounds
        float* p = &c->frame.pos.x + rnd() % 3;
        *p = (chance(50) ? 1.0f : -1.0f) * range(5000.0f, 9000.0f);
    }
    int k = rnd() % 10;
    float vs = k < 2 ? 0.2f : k < 4 ? 8.0f : 80.0f;
    c->velocity.x = range(-vs, vs); c->velocity.y = range(-vs * 0.2f, vs * 0.2f); c->velocity.z = range(-vs, vs);
    if (chance(30)) {                                                         // mostly forwards
        const float* m = c->frame.rot.m;
        float s = range(-10.0f, 60.0f);
        c->velocity.x = m[6] * s + range(-1.0f, 1.0f); c->velocity.y = m[7] * s; c->velocity.z = m[8] * s + range(-1.0f, 1.0f);
    }
    c->angular_velocity.x = range(-2.0f, 2.0f); c->angular_velocity.y = chance(20) ? 0.0f : range(-2.0f, 2.0f); c->angular_velocity.z = range(-2.0f, 2.0f);
    c->mass = range(1100.0f, 1700.0f);
    c->inv_mass = 1.0f / c->mass;
    c->inertia.x = range(1000.0f, 3000.0f); c->inertia.y = range(1000.0f, 3000.0f); c->inertia.z = range(300.0f, 1000.0f);
    c->inv_inertia.x = 1.0f / c->inertia.x; c->inv_inertia.y = 1.0f / c->inertia.y; c->inv_inertia.z = 1.0f / c->inertia.z;
    for (int i = 0; i < 9; i++) {
        c->inv_inertia_body.m[i] = (i % 4 == 0) ? (&c->inv_inertia.x)[i / 4] : 0.0f;
        c->inv_inertia_world.m[i] = (i % 4 == 0) ? range(2e-4f, 1e-3f) : range(-5e-5f, 5e-5f);
    }
    c->force.x = range(-5e4f, 5e4f); c->force.y = range(-5e4f, 5e4f); c->force.z = range(-5e4f, 5e4f);
    c->torque.x = range(-2e4f, 2e4f); c->torque.y = range(-2e4f, 2e4f); c->torque.z = range(-2e4f, 2e4f);
    c->impulse.x = range(-100.0f, 100.0f); c->impulse.y = range(-100.0f, 100.0f); c->impulse.z = range(-100.0f, 100.0f);
    c->impulse_weight = range(0.0f, 10.0f);
    P3* acc = (P3*)(b + 0x288);
    acc->x = range(-30.0f, 30.0f); acc->y = range(-30.0f, 30.0f); acc->z = range(-30.0f, 30.0f);
    c->tick_count = (uint16_t)rnd();
    c->collide_ground = chance(50);
    // the car's own fields
    c->realism = rnd() % 3;
    c->yaw_control = rnd() % 3;
    static const int states[] = {0, 2, 2, 2, 2, 3};
    c->race_state = states[rnd() % 6];
    c->steering = chance(20) ? 0.0f : range(-1.0f, 1.0f);
    c->pitch = range(-1.0f, 1.0f);
    c->opacity = chance(80) ? 1.0f : uni();
    c->car_index = chance(5) ? -1 : (int)(rnd() % 16);
    c->auto_clutch = chance(60);
    c->drag_long = range(0.2f, 0.8f); c->drag_lat = range(0.5f, 2.0f); c->drag_vert = range(0.5f, 3.0f);
    c->front_lift = chance(20) ? 0.0f : range(-2.0f, 0.5f); c->rear_lift = chance(20) ? 0.0f : range(-2.0f, 0.5f);
    c->front_spoiler_drag = range(0.0f, 0.3f); c->rear_spoiler_drag = range(0.0f, 0.3f);
    for (int i = 0; i < 4; i++) {
        FullWheel* w = (FullWheel*)&c->wheels[i];
        setup_wheel(w, i, &g_hub[ci][i]);
        w->droop_point_world.x = c->frame.pos.x + range(-1.5f, 1.5f);
        w->droop_point_world.y = c->frame.pos.y + range(-0.7f, -0.2f);
        w->droop_point_world.z = c->frame.pos.z + range(-1.5f, 1.5f);
        randomize_wheel(w, &c->frame.rot, i < 2);
    }
    c->is_plane = chance(10);
    if (c->is_plane) {
        for (int i = 0; i < 4; i++) {
            uint8_t* w = g_wings[ci][i];
            for (int j = 0; j < 60; j += 4) { float f = range(-1.0f, 1.0f); memcpy(w + j, &f, 4); }
            *(void**)w = c;                                                   // owner
            *(float*)(w + 4) = range(4.0f, 6.0f); *(float*)(w + 8) = range(0.8f, 2.5f); *(float*)(w + 0xc) = 0.09f;
            *(float*)(w + 0x10) = range(-3.0f, 3.0f); *(float*)(w + 0x14) = range(-0.5f, 0.5f); *(float*)(w + 0x18) = range(-3.0f, 3.0f);
            *(int32_t*)(w + 0x34) = i == 3 ? 1 : 0;
        }
        c->wing_left = (Wing*)g_wings[ci][0]; c->wing_right = (Wing*)g_wings[ci][1]; c->elevator = (Wing*)g_wings[ci][2]; c->rudder = (Wing*)g_wings[ci][3];
    }
    build_drivetrain(c);
    c->gear = c->transmission.gear;
    c->engaged_gear = chance(50) ? c->transmission.engaged_gear : -1 + rnd() % 7;
    c->shift_sound_gear = chance(50) ? c->engaged_gear : -1 + rnd() % 7;
    c->perceived_rpm = range(-500.0f, 9000.0f);
    c->heat_step = rnd() % 8;
    c->front_axle.x = 0.0f; c->front_axle.y = -range(0.2f, 0.5f); c->front_axle.z = range(1.0f, 1.6f);
    c->rear_axle.x = 0.0f; c->rear_axle.y = c->front_axle.y; c->rear_axle.z = -range(1.0f, 1.6f);
    c->center_of_pressure.x = range(-0.1f, 0.1f); c->center_of_pressure.y = range(-0.3f, 0.6f); c->center_of_pressure.z = range(-1.0f, 1.5f);
    c->max_steer_angle = range(0.3f, 0.7f);
    c->rpm_to_speed = range(0.005f, 0.02f);
    c->front_anti_roll = chance(20) ? 0.0f : range(0.0f, 60000.0f); c->rear_anti_roll = chance(20) ? 0.0f : range(0.0f, 60000.0f);
    c->speed = range(-10.0f, 90.0f);
    c->throttle = chance(30) ? 0.0f : uni();
    c->braking = chance(50) ? 0.0f : uni();
    c->ebrake = chance(60) ? 0.0f : uni();
    c->clutch = chance(50) ? 1.0f : uni();
    c->air_velocity.x = range(-20.0f, 20.0f); c->air_velocity.y = range(-10.0f, 10.0f); c->air_velocity.z = range(-90.0f, 90.0f);
    if (chance(50)) { c->draft_velocity.x = range(-30.0f, 30.0f); c->draft_velocity.y = range(-3.0f, 3.0f); c->draft_velocity.z = range(-30.0f, 30.0f); }
    c->draft_point.x = c->frame.pos.x + range(-8.0f, 8.0f); c->draft_point.y = c->frame.pos.y + range(-1.0f, 1.0f); c->draft_point.z = c->frame.pos.z + range(-8.0f, 8.0f);
    c->draft_radius = chance(70) ? 5.0f : range(0.0f, 12.0f);
    c->horn = chance(30);
    c->lap_top_speed = chance(50) ? 0.0f : range(0.0f, 80.0f);
    c->lowering = chance(12);
    c->scrape_energy = chance(50) ? 0.0f : range(0.0f, 1e8f);
    c->teleport_time = -100.0f;
    c->starter_on = chance(30);
    c->horn_volume = uni();
    c->lat_g = range(-2.0f, 2.0f); c->long_g = range(-2.0f, 2.0f); c->vert_g = range(-2.0f, 2.0f);
    c->steer_feedback = range(-100.0f, 100.0f);
    c->aero_g = range(-1.0f, 1.0f);
    c->damaged = chance(30);
    c->last_damage_time = range(0.0f, 100.0f);
    // sounds: some missing
    for (int i = 0; i < NSOUND; i++) setup_sound(g_sounds[i]);
    c->start_sound = chance(85) ? (SoundFx*)g_sounds[0] : 0;
    c->shift_sound = chance(85) ? (SoundFx*)g_sounds[1] : 0;
    c->road_sound = chance(85) ? (SoundFx*)g_sounds[2] : 0;
    c->road_sound_2 = chance(85) ? (SoundFx*)g_sounds[3] : 0;
    c->scrape_sound = chance(85) ? (SoundFx*)g_sounds[4] : 0;
    c->horn_sound = chance(85) ? (SoundFx*)g_sounds[5] : 0;
    for (int i = 0; i < 248; i++) g_engine_sound[i] = (uint8_t)rnd();
    *(int32_t*)(g_engine_sound + 0xf4) = rnd() % 8;
    for (int i = 0; i < 7; i++) *(void**)(g_engine_sound + 0xd4 + 4 * i) = chance(70) ? g_sounds[6] : 0;
    *(void**)(g_engine_sound + 0xf0) = chance(70) ? g_sounds[6] : 0;
    c->engine_sound = chance(85) ? (EngineSound*)g_engine_sound : 0;
}

static void randomize_world() {
    setup_tire(&g_tire);
    randomize_car(0);
    randomize_car(1);
    Car* c = car(0);
    // the second car near the first (for the draft), sometimes right on its draft point
    Car* o = car(1);
    if (chance(60)) {
        o->frame.pos.x = c->frame.pos.x + range(-8.0f, 8.0f); o->frame.pos.y = c->frame.pos.y + range(-1.0f, 1.0f);
        o->frame.pos.z = c->frame.pos.z + range(-8.0f, 8.0f);
        o->draft_point.x = c->frame.pos.x + range(-6.0f, 6.0f); o->draft_point.y = c->frame.pos.y + range(-0.5f, 0.5f);
        o->draft_point.z = c->frame.pos.z + range(-6.0f, 6.0f);
    }
    *g_damage_on = chance(70);
    *g_pave_a = chance(30);
    *g_pave_b = chance(50);
    *g_hornball = chance(50);
    *(int32_t*)0x00521600 = chance(30) ? 0xb : (int)(rnd() % 12);
    for (int i = 0; i < 1152; i += 4) { float f = range(-100.0f, 100.0f); memcpy(g_ball + i, &f, 4); }
    *(uint32_t*)g_ball = 0x004dc3b0;
    for (int i = 0; i < NSTATUS; i++) g_status[i] = (uint8_t)rnd();
    // the script
    g_script.ground_y = c->frame.pos.y - range(0.2f, 1.2f);
    const bool calm = chance(25);                        // all four wheels down and gripping (the lap top speed)
    if (calm) {
        g_script.ground_y = c->frame.pos.y;
        for (int i = 0; i < 4; i++) {
            FullWheel* w = (FullWheel*)&c->wheels[i];
            w->droop_point_world.y = c->frame.pos.y - range(0.6f, 0.7f);
            w->lat_slip = range(-0.1f, 0.1f); w->long_slip = range(-0.1f, 0.1f); w->slide = 0.0f;
            w->brake_input = 0.0f; w->drive_torque = 0.0f; w->on_ground = 1; w->broken = 0;
            w->omega = c->velocity.z / w->radius; w->prev_omega = w->omega; w->rpm = w->omega * 9.549296f;
        }
        c->lap_top_speed = 0.0f;
        c->angular_velocity.x = c->angular_velocity.y = c->angular_velocity.z = 0.0f;
        const float* m = c->frame.rot.m;
        float sp = range(5.0f, 60.0f);
        c->velocity.x = m[6] * sp; c->velocity.y = m[7] * sp; c->velocity.z = m[8] * sp;
        for (int i = 0; i < 4; i++) { FullWheel* w = (FullWheel*)&c->wheels[i]; w->omega = sp / w->radius; w->prev_omega = w->omega; w->rpm = w->omega * 9.549296f; }
    }
    for (int i = 0; i < 8; i++) {
        TerrainScript& t = g_script.terrain[i];
        t.hit = calm || chance(92);
        t.h = calm ? range(-0.62f, -0.56f) : range(-0.3f, 0.3f);
        t.dx = range(-0.02f, 0.02f); t.dz = range(-0.02f, 0.02f);
        int nk = calm ? 9 : rnd() % 10;
        double nx, ny, nz;
        if (nk < 6) { nx = range(-0.15f, 0.15f); nz = range(-0.15f, 0.15f); ny = 1.0; }
        else if (nk < 8) { nx = range(-1.0f, 1.0f); nz = range(-1.0f, 1.0f); ny = range(0.0f, 1.0f); }
        else if (nk < 9) { nx = range(-1.0f, 1.0f); nz = range(-1.0f, 1.0f); ny = range(-1.0f, 0.0f); }
        else { nx = 0.0; nz = 0.0; ny = 1.0; }
        double l = sqrt(nx * nx + ny * ny + nz * nz);
        if (calm || chance(90)) { nx /= l; ny /= l; nz /= l; }
        t.n.x = (float)nx; t.n.y = (float)ny; t.n.z = (float)nz;
        static const int surfaces[] = {0, 0, 0, 0, 0, 10, 10, 11, 12, 13, 14, 15, 20, 21, 23, 1, 2, 5, 9, 16, 22, 24, 100, -3};
        t.surface = calm ? 0 : surfaces[rnd() % (sizeof surfaces / sizeof *surfaces)];
        g_script.random[i] = rnd() & 1;
    }
    g_script.time = chance(30) ? c->last_damage_time + range(1.5f, 2.5f) : range(0.0f, 200.0f);
    g_script.focus_car = chance(50) ? c->car_index : (int)(rnd() % 16);
    g_script.teleport_ok = chance(50);
    g_script.have_ball = chance(70);
    g_script.super_h = range(-20.0f, 200.0f);
}

enum Kind : int;
// the arguments of the world's call
struct Args { float brakes[4]; float f; uint32_t u; int i; P3 force, point; uint8_t b; };
static Args g_args;

// some floats replaced with extreme values
static void wildify(Kind kind) {
    Car* c = car(0);
    if (kind == 2 && chance(70)) {                     // K_YAW:
        if (chance(50)) {                               // everything it reads, as the fuzzer would have it
            static const uint32_t all[] = {0x234, 0x238, 0x23c, 0x244, 0x500, 0xdcc, 0xd84, 0xd90};
            for (uint32_t off : all) if (chance(60)) { float f = wildf(); memcpy((uint8_t*)c + off, &f, 4); }
            if (chance(50)) c->yaw_control = (int)rnd();
            for (int i = 0; i < 4; i++) if (chance(50)) g_args.brakes[i] = wildf();
            // not the fptan range case (|steer| >= 2^63): there the original underflows its x87 stack
            if (fabs((double)c->max_steer_angle * c->steering) >= 9223372036854775808.0) c->steering = range(-1.0f, 1.0f);
            return;
        }                  // the yaw control's own inputs, extreme
        static const uint32_t yaw_floats[] = {0x234, 0x238, 0x23c, 0x244, 0x500, 0xdcc, 0xd84, 0xd90};
        int n = 1 + rnd() % 3;
        for (int i = 0; i < n; i++) { float f = wildf(); memcpy((uint8_t*)c + yaw_floats[rnd() % 8], &f, 4); }
        if (chance(50)) for (int i = 0; i < 4; i++) g_args.brakes[i] = wildf();
        if (fabs((double)c->max_steer_angle * c->steering) >= 9223372036854775808.0) c->steering = range(-1.0f, 1.0f);
        return;
    }
    static const uint32_t car_floats[] = {0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x64, 0x1f0, 0x200,
        0x234, 0x238, 0x23c, 0x240, 0x244, 0x248, 0x288, 0x28c, 0x290, 0x488, 0x500, 0x508, 0x538, 0x53c, 0x540, 0x544, 0x548,
        0x54c, 0x550, 0xd70, 0xd7c, 0xd80, 0xd84, 0xd88, 0xd8c, 0xd90, 0xd94, 0xd98, 0xd9c, 0xdcc, 0xdd0, 0xdd4, 0xdd8,
        0xde0, 0xde4, 0xde8, 0xdec, 0xdf0, 0xdf4, 0xdf8, 0xdfc, 0xe00, 0xe04, 0xe08, 0xe0c, 0xe10, 0xe14, 0xe30, 0xe40,
        0xe68, 0xea4, 0xea8, 0xeac, 0xeb0, 0xeb4};
    int n = 1 + rnd() % 4;
    for (int i = 0; i < n; i++) {
        uint32_t off = car_floats[rnd() % (sizeof car_floats / sizeof *car_floats)];
        float f = wildf();
        memcpy((uint8_t*)c + off, &f, 4);
    }
    if (chance(40)) {                                    // a wheel's float
        FullWheel* w = (FullWheel*)&c->wheels[rnd() % 4];
        uint32_t off;
        do off = 4 + 4 * (rnd() % ((sizeof(FullWheel) - 4) / 4));
        while (off == 0x10 || off == 0x20 || off == 0x8c || off == 0x120 || off == 0x15c || off == 0x160 ||
               off == 0x164 || off == 0x168 || off == 0x184 || off == 0x19c || off == 0x1a0);
        float f = wildf();
        memcpy((uint8_t*)w + off, &f, 4);
    }
    if (chance(20)) { float f = wildf(); memcpy(g_sounds[rnd() % NSOUND] + 4 + 4 * (rnd() % 2), &f, 4); }
    if (chance(20)) { Car* o = car(1); float* p = &o->draft_point.x + rnd() % 3; *p = wildf(); }
    if (chance(10)) g_script.time = wildf();
}

// ---- running both --------------------------------------------------------------------------------------------
struct State {
    uint8_t car[2][CAR_BUF];
    Tire tire;
    P3 hub[2][4];
    uint8_t wings[2][4][60];
    uint8_t sounds[NSOUND][64];
    uint8_t engine_sound[248];
    uint8_t ball[1152];
    uint8_t status[NSTATUS];
    uint8_t globals[8];
    float brakes[4];
    uint64_t ret;
    Log log;
};
static void save(State& s) {
    memcpy(s.car[0], g_carbuf[0], CAR_BUF); memcpy(s.car[1], g_carbuf[1], CAR_BUF);
    s.tire = g_tire;
    memcpy(s.hub, g_hub, sizeof g_hub);
    memcpy(s.wings, g_wings, sizeof g_wings);
    memcpy(s.sounds, g_sounds, sizeof g_sounds);
    memcpy(s.engine_sound, g_engine_sound, sizeof g_engine_sound);
    memcpy(s.ball, g_ball, sizeof g_ball);
    memcpy(s.status, g_status, NSTATUS);
    s.globals[0] = *g_damage_on; s.globals[1] = *g_pave_a; s.globals[2] = *g_pave_b; s.globals[3] = *g_hornball;
    memcpy(s.globals + 4, (void*)0x00521600, 4);
    memcpy(s.brakes, g_args.brakes, 16);
}
static void load(const State& s) {
    memcpy(g_carbuf[0], s.car[0], CAR_BUF); memcpy(g_carbuf[1], s.car[1], CAR_BUF);
    g_tire = s.tire;
    memcpy(g_hub, s.hub, sizeof g_hub);
    memcpy(g_wings, s.wings, sizeof g_wings);
    memcpy(g_sounds, s.sounds, sizeof g_sounds);
    memcpy(g_engine_sound, s.engine_sound, sizeof g_engine_sound);
    memcpy(g_ball, s.ball, sizeof g_ball);
    memcpy(g_status, s.status, NSTATUS);
    *g_damage_on = s.globals[0]; *g_pave_a = s.globals[1]; *g_pave_b = s.globals[2]; *g_hornball = s.globals[3];
    memcpy((void*)0x00521600, s.globals + 4, 4);
    memcpy(g_args.brakes, s.brakes, 16);
}

// the functions under test
enum Kind : int { K_UPDATE, K_COMMON, K_YAW, K_DRAFT, K_HEAT, K_HELP, K_SUPER, K_STEER, K_HORN, K_GEAR, K_GEARAUTO, K_GEARAI,
            K_SUSP, K_TC, K_ABS, K_THROTTLE, K_BRAKING, K_CLUTCH, K_EBRAKE, K_PITCH, K_GEARS, K_SKID, K_DAMAGED, K_OOB, N_KINDS };
static const char* const kind_names[N_KINDS] = {"Car::Update", "Car::UpdateCommon", "Car::apply_yaw_control", "Car::UpdateDraft",
    "Car::UpdateHeat", "Car::HelpOut", "Car::SuperHelpOut", "Car::SetSteering", "Car::SetHorn", "Car::SetGear", "Car::SetGearAuto",
    "Car::SetGearAutoAI", "Car::ApplySuspensionForce", "Car::SetTractionControl", "Car::SetABSBraking", "Car::SetThrottle",
    "Car::SetBraking", "Car::SetClutch", "Car::SetEBrake", "Car::SetPitch", "Car::SetGear(bad)", "Car::WheelsSkidding",
    "Car::AnyWheelsAreDamaged", "loc_is_out_of_bounds"};
static const int kind_weight[N_KINDS] = {40, 14, 6, 6, 4, 4, 3, 4, 4, 2, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static void randomize_args() {
    for (int i = 0; i < 4; i++) g_args.brakes[i] = chance(30) ? 0.0f : range(-1.5f, 1.5f);
    g_args.f = chance(20) ? wildf() : range(-1.5f, 1.5f);
    g_args.u = fbits(chance(10) ? wildf() : range(-0.5f, 1.5f));
    g_args.i = -2 + rnd() % 10;
    g_args.force.x = range(-2e4f, 2e4f); g_args.force.y = range(-2e4f, 2e4f); g_args.force.z = range(-2e4f, 2e4f);
    g_args.point = car(0)->frame.pos;
    g_args.point.x += range(-2.0f, 2.0f); g_args.point.y += range(-1.0f, 1.0f); g_args.point.z += range(-2.0f, 2.0f);
    g_args.b = chance(50);
}

static int g_fault_kind;
static uint64_t call_kind(Kind k, bool rewrite) {
    Car* c = car(0);
    Car* o = car(1);
    uint64_t r = 0;
#define ORIG(T, A) ((T)(A))
    switch (k) {
    case K_UPDATE: rewrite ? Car_Update(c, 0) : ORIG(void(__fastcall*)(Car*, int), 0x00437680)(c, 0); break;
    case K_COMMON: rewrite ? Car_UpdateCommon(c, 0) : ORIG(void(__fastcall*)(Car*, int), 0x004391b0)(c, 0); break;
    case K_YAW: rewrite ? Car_apply_yaw_control(c, 0, g_args.brakes) : ORIG(void(__fastcall*)(Car*, int, float*), 0x004386a0)(c, 0, g_args.brakes); break;
    case K_DRAFT: rewrite ? Car_UpdateDraft(c, 0, o) : ORIG(void(__fastcall*)(Car*, int, Car*), 0x00438830)(c, 0, o); break;
    case K_HEAT: rewrite ? Car_UpdateHeat(c, 0, g_args.u) : ORIG(void(__fastcall*)(Car*, int, uint32_t), 0x00438910)(c, 0, g_args.u); break;
    case K_HELP: rewrite ? Car_HelpOut(c, 0) : ORIG(void(__fastcall*)(Car*, int), 0x00439780)(c, 0); break;
    case K_SUPER: rewrite ? Car_SuperHelpOut(c, 0) : ORIG(void(__fastcall*)(Car*, int), 0x00439a00)(c, 0); break;
    case K_STEER: rewrite ? Car_SetSteering(c, 0, g_args.f) : ORIG(void(__fastcall*)(Car*, int, float), 0x00439620)(c, 0, g_args.f); break;
    case K_HORN: rewrite ? Car_SetHorn(c, 0, g_args.b) : ORIG(void(__fastcall*)(Car*, int, uint8_t), 0x0043afb0)(c, 0, g_args.b); break;
    case K_GEAR: case K_GEARS: rewrite ? Car_SetGear(c, 0, g_args.i) : ORIG(void(__fastcall*)(Car*, int, int), 0x00439710)(c, 0, g_args.i); break;
    case K_GEARAUTO: rewrite ? Car_SetGearAuto(c, 0) : ORIG(void(__fastcall*)(Car*, int), 0x00439740)(c, 0); break;
    case K_GEARAI: rewrite ? Car_SetGearAutoAI(c, 0) : ORIG(void(__fastcall*)(Car*, int), 0x00439760)(c, 0); break;
    case K_SUSP: rewrite ? Car_ApplySuspensionForce(c, 0, &g_args.force, &g_args.point)
                         : ORIG(void(__fastcall*)(Car*, int, const P3*, const P3*), 0x00437660)(c, 0, &g_args.force, &g_args.point); break;
    case K_TC: rewrite ? Car_SetTractionControl(c, 0, g_args.i) : ORIG(void(__fastcall*)(Car*, int, int), 0x004395e0)(c, 0, g_args.i); break;
    case K_ABS: rewrite ? Car_SetABSBraking(c, 0, g_args.i) : ORIG(void(__fastcall*)(Car*, int, int), 0x00439600)(c, 0, g_args.i); break;
    case K_THROTTLE: rewrite ? Car_SetThrottle(c, 0, g_args.u) : ORIG(void(__fastcall*)(Car*, int, uint32_t), 0x004396d0)(c, 0, g_args.u); break;
    case K_BRAKING: rewrite ? Car_SetBraking(c, 0, g_args.u) : ORIG(void(__fastcall*)(Car*, int, uint32_t), 0x004396e0)(c, 0, g_args.u); break;
    case K_CLUTCH: rewrite ? Car_SetClutch(c, 0, g_args.u) : ORIG(void(__fastcall*)(Car*, int, uint32_t), 0x004396f0)(c, 0, g_args.u); break;
    case K_EBRAKE: rewrite ? Car_SetEBrake(c, 0, g_args.u) : ORIG(void(__fastcall*)(Car*, int, uint32_t), 0x00439700)(c, 0, g_args.u); break;
    case K_PITCH: rewrite ? Car_SetPitch(c, 0, g_args.u) : ORIG(void(__fastcall*)(Car*, int, uint32_t), 0x0043b050)(c, 0, g_args.u); break;
    case K_SKID: r = rewrite ? (uint32_t)Car_WheelsSkidding(c, 0) : (uint32_t)ORIG(int(__fastcall*)(Car*, int), 0x00439f30)(c, 0); break;
    case K_DAMAGED: r = rewrite ? Car_AnyWheelsAreDamaged(c, 0) : ORIG(uint8_t(__fastcall*)(Car*, int), 0x0043b060)(c, 0); break;
    case K_OOB: r = rewrite ? loc_is_out_of_bounds(&c->frame.pos) : ORIG(uint8_t(__cdecl*)(const P3*), 0x00438660)(&c->frame.pos); break;
    default: break;
    }
#undef ORIG
    return r;
}
static uint32_t g_fault_eip, g_fault_code, g_fault_addr;
static int fault_filter(EXCEPTION_POINTERS* e) {
    g_fault_eip = (uint32_t)e->ContextRecord->Eip;
    g_fault_code = e->ExceptionRecord->ExceptionCode;
    g_fault_addr = e->ExceptionRecord->NumberParameters > 1 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static int run_guarded(Kind k, bool rewrite, uint64_t* ret) {
    g_log.n = 0;
    g_terrain_i = g_random_i = 0;
    unsigned cw;
    _clearfp();
    _controlfp_s(&cw, _PC_24, _MCW_PC);
#if defined(__GNUC__) && !defined(__clang__)
    if (vp_try([&] { *ret = call_kind(k, rewrite); }, fault_filter)) { return 1; }
    return 0;
#else
    __try { *ret = call_kind(k, rewrite); } __except (fault_filter(GetExceptionInformation())) { return 1; }
    return 0;
#endif
}

// the rewrite's footprint for the world's call
static void footprint_of(Kind k, Footprint& fp) {
    Car* c = car(0);
    fp.n = 0; fp.pure = false; fp.replay_only = 0;
    switch (k) {
    case K_UPDATE: fpof_Car_Update(fp, c, 0); break;
    case K_COMMON: fpof_Car_UpdateCommon(fp, c, 0); break;
    case K_YAW: fpof_Car_apply_yaw_control(fp, c, 0, g_args.brakes); break;
    case K_DRAFT: fpof_Car_UpdateDraft(fp, c, 0, car(1)); break;
    case K_HEAT: fpof_Car_UpdateHeat(fp, c, 0, g_args.u); break;
    case K_HELP: fpof_Car_HelpOut(fp, c, 0); break;
    case K_SUPER: fpof_Car_SuperHelpOut(fp, c, 0); break;
    case K_STEER: fpof_Car_SetSteering(fp, c, 0, g_args.f); break;
    case K_HORN: fpof_Car_SetHorn(fp, c, 0, g_args.b); break;
    case K_GEAR: case K_GEARS: fpof_Car_SetGear(fp, c, 0, g_args.i); break;
    case K_GEARAUTO: fpof_Car_SetGearAuto(fp, c, 0); break;
    case K_GEARAI: fpof_Car_SetGearAutoAI(fp, c, 0); break;
    case K_SUSP: fpof_Car_ApplySuspensionForce(fp, c, 0, &g_args.force, &g_args.point); break;
    default: fp.pure = true; break;                     // the pure ones: the car (their this)
    }
    if (fp.pure && fp.n == 0) fp.add(c, 3768, "car (pure)");
}
static bool in_footprint(const Footprint& fp, const uint8_t* p) {
    for (int i = 0; i < fp.n; i++)
        if (p >= (const uint8_t*)fp.r[i].p && p < (const uint8_t*)fp.r[i].p + fp.r[i].n) return true;
    return false;
}

// a name for a byte of the car
static const char* name_of(uint32_t off, char* buf) {
    struct F { uint32_t off, size; const char* name; };
    static const F car_fields[] = {
        {0x38, 36, "frame.rot"}, {0x5c, 12, "frame.pos"}, {0x234, 12, "velocity"}, {0x240, 12, "angular_velocity"},
        {0x24c, 12, "force"}, {0x258, 12, "torque"}, {0x264, 36, "inv_inertia_world"}, {0x288, 12, "acceleration"},
        {0x294, 12, "impulse"}, {0x2a0, 12, "angular_impulse"}, {0x2ac, 4, "impulse_weight"}, {0x500, 4, "steering"},
        {0x554, 4 * 0x1a4, "wheels"}, {0xbfc, 128, "engine"}, {0xc7c, 28, "clutch_unit"}, {0xc98, 108, "transmission"},
        {0xd04, 96, "diffs"}, {0xd64, 4, "gear"}, {0xd68, 4, "engaged_gear"}, {0xd6c, 4, "shift_sound_gear"},
        {0xd70, 4, "perceived_rpm"}, {0xd78, 4, "heat_step"}, {0xddc, 4, "speed"}, {0xde0, 4, "throttle"}, {0xde4, 4, "braking"},
        {0xdec, 4, "clutch"}, {0xdf0, 12, "air_velocity"}, {0xdfc, 12, "draft_velocity"}, {0xe08, 12, "draft_point"},
        {0xe28, 1, "horn"}, {0xe30, 4, "lap_top_speed"}, {0xe3d, 1, "lowering"}, {0xe40, 4, "scrape_energy"},
        {0xe49, 1, "starter_on"}, {0xe68, 4, "horn_volume"}, {0xea4, 4, "lat_g"}, {0xea8, 4, "long_g"}, {0xeac, 4, "vert_g"},
        {0xeb0, 4, "steer_feedback"}, {0xeb4, 4, "aero_g"}};
    for (const F& f : car_fields)
        if (off >= f.off && off < f.off + f.size) {
            if (f.off == 0x554) sprintf(buf, "wheels[%u]+0x%x", (off - 0x554) / 0x1a4, (off - 0x554) % 0x1a4);
            else sprintf(buf, "car.%s+%u", f.name, off - f.off);
            return buf;
        }
    sprintf(buf, "car+0x%x", off);
    return buf;
}

static bool compare_block(const uint8_t* o, const uint8_t* n, uint32_t size, const char* what, bool& same, int it, Kind k, bool car0) {
    char buf[96];
    for (uint32_t i = 0; i < size; i += 4) {
        uint32_t a, b;
        memcpy(&a, o + i, 4); memcpy(&b, n + i, 4);
        if (a == b) continue;
        if (same) printf("  MISMATCH, world %d (%s)\n", it, kind_names[k]);
        same = false;
        if (car0) printf("    %-34s original %08x (%.9g)  rewrite %08x (%.9g)\n", name_of(i, buf), a, bitsf(a), b, bitsf(b));
        else printf("    %s+0x%x original %08x (%.9g)  rewrite %08x (%.9g)\n", what, i, a, bitsf(a), b, bitsf(b));
    }
    return same;
}
static bool compare(const State& o, const State& n, int it, Kind k) {
    bool same = true;
    compare_block(o.car[0], n.car[0], CAR_BUF, "car", same, it, k, true);
    compare_block(o.car[1], n.car[1], CAR_BUF, "other car", same, it, k, false);
    compare_block((const uint8_t*)o.wings, (const uint8_t*)n.wings, sizeof o.wings, "wings", same, it, k, false);
    compare_block((const uint8_t*)o.sounds, (const uint8_t*)n.sounds, sizeof o.sounds, "sounds", same, it, k, false);
    compare_block(o.engine_sound, n.engine_sound, sizeof o.engine_sound, "engine sound", same, it, k, false);
    compare_block(o.ball, n.ball, sizeof o.ball, "ball", same, it, k, false);
    compare_block(o.status, n.status, NSTATUS, "status", same, it, k, false);
    compare_block((const uint8_t*)o.brakes, (const uint8_t*)n.brakes, 16, "brakes", same, it, k, false);
    if (memcmp(&o.tire, &n.tire, sizeof o.tire) || memcmp(o.hub, n.hub, sizeof o.hub) || memcmp(o.globals, n.globals, 8)) {
        if (same) printf("  MISMATCH, world %d (%s)\n", it, kind_names[k]);
        printf("    the tire, a hub vertex or a global differs\n");
        same = false;
    }
    if (o.ret != n.ret) {
        if (same) printf("  MISMATCH, world %d (%s)\n", it, kind_names[k]);
        printf("    return: original %llx rewrite %llx\n", o.ret, n.ret);
        same = false;
    }
    if (o.log.n != n.log.n || memcmp(o.log.e, n.log.e, 4 * (o.log.n < 128 ? o.log.n : 128))) {
        if (same) printf("  MISMATCH, world %d (%s)\n", it, kind_names[k]);
        printf("    the stub logs differ: %d / %d entries\n", o.log.n, n.log.n);
        int m = o.log.n < n.log.n ? o.log.n : n.log.n;
        if (m > 128) m = 128;
        for (int i = 0; i < m; i++) if (o.log.e[i] != n.log.e[i]) { printf("    first at %d: %08x / %08x\n", i, o.log.e[i], n.log.e[i]); break; }
        same = false;
    }
    return same;
}

// the bytes the original changed, against the rewrite's footprint (a change outside it is reported once a world)
static int check_footprint(const State& start, const State& orig, const Footprint& fp, int it, Kind k) {
    if (fp.replay_only) return 0;
    struct R { const uint8_t *a, *b, *live; uint32_t n; const char* what; } regions[] = {
        {start.car[0], orig.car[0], g_carbuf[0], CAR_BUF, "car"}, {start.car[1], orig.car[1], g_carbuf[1], CAR_BUF, "other car"},
        {(const uint8_t*)start.wings, (const uint8_t*)orig.wings, (const uint8_t*)g_wings, sizeof g_wings, "wings"},
        {(const uint8_t*)start.sounds, (const uint8_t*)orig.sounds, (const uint8_t*)g_sounds, sizeof g_sounds, "sounds"},
        {start.engine_sound, orig.engine_sound, g_engine_sound, 248, "engine sound"},
        {start.ball, orig.ball, g_ball, 1152, "ball"}, {start.status, orig.status, g_status, NSTATUS, "status"}};
    for (const R& r : regions)
        for (uint32_t i = 0; i < r.n; i++)
            if (r.a[i] != r.b[i] && !in_footprint(fp, r.live + i)) {
                char buf[96];
                printf("  FOOTPRINT, world %d (%s): %s+0x%x changed outside it%s%s\n", it, kind_names[k], r.what, i,
                       r.live == g_carbuf[0] ? " = " : "", r.live == g_carbuf[0] ? name_of(i, buf) : "");
                return 1;
            }
    return 0;
}

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 200000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    strcpy(exe, __FILE__);
    char* s = strstr(exe, "\\test\\world_car.cpp");
    if (s) strcpy(s, "\\out\\race_v10.exe");
    if (!load_race_exe(exe)) return 2;
    patch_jmp(0x00465c40, (void*)&stub_terrain);
    patch_jmp(0x0041b6e0, (void*)&stub_random);
    patch_jmp(0x0042bc80, (void*)&stub_time);
    patch_jmp(0x004626d0, (void*)&stub_focus);
    patch_jmp(0x00472c60, (void*)&stub_engine_sound);
    patch_jmp(0x00439aa0, (void*)&stub_fadein);
    patch_jmp(0x00439e50, (void*)&stub_reset_damage);
    patch_jmp(0x004410e0, (void*)&stub_getball);
    patch_jmp(0x004412a0, (void*)&stub_throw);
    patch_jmp(0x00465ae0, (void*)&stub_terrain_xz);
    // the deity: a RaceDeity-shaped object (its real vtable, so the footprint accepts it) whose UpdateCar and
    // TeleportToLine are patched to the logging stubs
    patch_jmp(0x00443640, (void*)&stub_deity_update);
    patch_jmp(0x00443a00, (void*)&stub_deity_teleport);
    *(uint32_t*)g_deity_obj = 0x004dc588;
    *(void**)0x005218ac = g_deity_obj;
    *(void**)0x00521f7c = 0;                            // no splash sound
    for (int i = 0; i < 2; i++) g_carbuf[i] = (uint8_t*)VirtualAlloc(0, CAR_BUF, MEM_COMMIT, PAGE_READWRITE);

    static State start, orig, rew;
    static Footprint fp;
    int differ = 0, faults = 0, wild_runs = 0, wild_differ = 0, fp_bad = 0, replay_only = 0;
    int per_kind[N_KINDS] = {0}, per_kind_bad[N_KINDS] = {0};
    int c_lower = 0, c_done_lower = 0, c_oob = 0, c_crank = 0, c_heat = 0, c_r0 = 0, c_r1 = 0, c_yawf = 0, c_air = 0, c_plane = 0,
        c_topspeed = 0, c_reset = 0, c_yaw_brk = 0, c_draft = 0, c_road2 = 0, c_help[3] = {0}, c_throw = 0, c_ebrake = 0;
    int total_w = 0;
    for (int i = 0; i < N_KINDS; i++) total_w += kind_weight[i];
    for (int it = 0; it < iterations; it++) {
        int pick = (int)(rnd() % total_w), k = 0;
        while (pick >= kind_weight[k]) pick -= kind_weight[k++];
        Kind kind = (Kind)k;
        randomize_world();
        randomize_args();
        bool wild = chance(kind == K_YAW ? 40 : 10);
        if (wild) { wildify(kind); wild_runs++; }
        // never the fptan range case (|steer| >= 2^63): there the original's yaw control underflows its x87 stack
        if (kind == K_YAW && !(fabs((double)car(0)->max_steer_angle * car(0)->steering) < 9223372036854775808.0) &&
            car(0)->max_steer_angle == car(0)->max_steer_angle && car(0)->steering == car(0)->steering)
            car(0)->steering = range(-1.0f, 1.0f), car(0)->max_steer_angle = range(0.3f, 0.7f);
        footprint_of(kind, fp);
        if (fp.replay_only) replay_only++;
        save(start);
        int fo = run_guarded(kind, false, &orig.ret);
        save(orig);
        orig.log = g_log;
        load(start);
        int fn = run_guarded(kind, true, &rew.ret);
        save(rew);
        rew.log = g_log;
        per_kind[kind]++;
        if (fo || fn) {
            if (fo != fn) { printf("  world %d (%s): original %s, rewrite %s\n", it, kind_names[kind], fo ? "faulted" : "ran", fn ? "faulted" : "ran"); differ++; per_kind_bad[kind]++; }
            faults++;
            if (faults <= 5) printf("  world %d (%s): fault %08x at eip %08x, address %08x (%s)\n", it, kind_names[kind], g_fault_code, g_fault_eip, g_fault_addr, fn ? "rewrite" : "original");
            continue;
        }
        if (!compare(orig, rew, it, kind)) {
            if (kind == K_YAW) {
                const Car* sc = (const Car*)start.car[0];
                printf("    inputs: v (%08x %08x %08x) wy %08x steering %08x max_steer %08x fa.z %08x ra.z %08x yaw_control %d brakes (%08x %08x %08x %08x)\n",
                       fbits(sc->velocity.x), fbits(sc->velocity.y), fbits(sc->velocity.z), fbits(sc->angular_velocity.y), fbits(sc->steering),
                       fbits(sc->max_steer_angle), fbits(sc->front_axle.z), fbits(sc->rear_axle.z), sc->yaw_control,
                       fbits(g_args.brakes[0]), fbits(g_args.brakes[1]), fbits(g_args.brakes[2]), fbits(g_args.brakes[3]));
            }
            differ++;
            per_kind_bad[kind]++;
            if (wild) wild_differ++;
            if (differ >= 20) { printf("stopping after 20 differing worlds\n"); break; }
        }
        fp_bad += check_footprint(start, orig, fp, it, kind);
        // coverage, from the starting state and the original's result
        const Car* sc = (const Car*)start.car[0];
        const Car* oc = (const Car*)orig.car[0];
        if (kind == K_UPDATE) {
            if (sc->lowering) c_lower++;
            if (sc->lowering && !oc->lowering) c_done_lower++;
            for (int i = 0; i + 3 < orig.log.n && i < 128; i++) {
                if (orig.log.e[i] == 0xde170001u) c_oob++;
                if (orig.log.e[i] == 0xda3a0000u) c_reset++;
            }
            if (sc->engine.stalled && fbits(sc->clutch_unit.engagement) < 0x3c23d70a) c_crank++;
            if (sc->heat_step == 7) c_heat++;
            if (sc->realism == 0) c_r0++;
            if (sc->realism == 1) c_r1++;
            int n = sc->wheels[0].on_ground + sc->wheels[1].on_ground + sc->wheels[2].on_ground + sc->wheels[3].on_ground;
            if (sc->realism < 2 && n > 2 && !sc->wheels[0].broken && !sc->wheels[1].broken) c_yawf++;
            if (n < 2) c_air++;
            if (sc->is_plane) c_plane++;
            if (memcmp(&sc->lap_top_speed, &oc->lap_top_speed, 4)) c_topspeed++;
            if (fbits(sc->ebrake) > 0x3dcccccd) c_ebrake++;
            if (sc->yaw_control && memcmp(&sc->wheels[0].brake_input, &oc->wheels[0].brake_input, 4) + memcmp(&sc->wheels[2].brake_input, &oc->wheels[2].brake_input, 4)) c_yaw_brk++;
        }
        if (kind == K_DRAFT && memcmp(&sc->draft_velocity, &oc->draft_velocity, 12)) c_draft++;
        if (kind == K_COMMON && sc->road_sound_2 && orig.log.n) c_road2++;
        if (kind == K_HELP && !sc->lowering && sc->race_state != 0 && sc->race_state != 3)
            c_help[fbits(sc->frame.rot.m[4]) > 0xbf4ccccdu ? 0 : fbits(sc->frame.rot.m[4]) < 0x3e4ccccd ? 1 : 2]++;
        if (kind == K_HORN) for (int i = 0; i < orig.log.n && i < 128; i++) if (orig.log.e[i] == 0x7c000000u) c_throw++;
    }
    printf("%d worlds (%d wild): %d differ (%d of them wild), %d faulted in both, %d changed bytes outside the footprint, %d replay-only\n",
           iterations, wild_runs, differ, wild_differ, faults, fp_bad, replay_only);
    printf("per function (worlds / differing):");
    for (int i = 0; i < N_KINDS; i++) printf("%s %s %d/%d", i ? "," : "", kind_names[i], per_kind[i], per_kind_bad[i]);
    printf("\n");
    printf("Car::Update covered: lowering %d (done %d), out of bounds %d, cranking %d, heat step %d, realism 0 %d / 1 %d, yaw force applied %d,\n"
           "  airborne drag %d, plane %d, lap top speed changed %d, damage reset %d, yaw-control brakes %d, e-brake locked %d\n",
           c_lower, c_done_lower, c_oob, c_crank, c_heat, c_r0, c_r1, c_yawf, c_air, c_plane, c_topspeed, c_reset, c_yaw_brk, c_ebrake);
    printf("others: draft applied %d, road2 worlds %d, HelpOut flip %d / side %d / up %d, ball thrown %d\n", c_draft, c_road2,
           c_help[0], c_help[1], c_help[2], c_throw);
    return differ || fp_bad ? 1 : 0;
}
